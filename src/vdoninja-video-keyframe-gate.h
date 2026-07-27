/*
 * OBS VDO.Ninja Plugin
 * Per-peer video synchronization state
 */

#pragma once

#include <cstddef>
#include <cstdint>

namespace vdoninja
{

// Tracks whether a peer can consume live delta frames and whether replaying the
// cached keyframe is still safe. A pending live keyframe permits dependent
// frames to queue behind it, but recovery is not declared complete until every
// packet of that live keyframe has been accepted by the transport. A cached
// startup keyframe can provide an immediate still image, but it cannot establish
// the current prediction chain after intervening delta frames.
class VideoKeyframeGate
{
public:
	using KeyframeTicket = uint64_t;
	enum class DecoderRequestDisposition {
		CachedPrimeAllowed,
		PendingLiveKeyframe,
		RequireNewLiveKeyframe,
		ContinueLiveStream,
	};

	bool canQueueFrame(bool keyframe, bool cachedReplay) const noexcept
	{
		if (cachedReplay) {
			return keyframe && awaitingKeyframe_ && cachedPrimeAllowed_ && pendingKeyframes_ == 0;
		}
		return keyframe || !awaitingKeyframe_ || pendingLiveKeyframes_ > 0;
	}

	KeyframeTicket onKeyframeQueued(bool cachedReplay = false) noexcept
	{
		cachedPrimeAllowed_ = false;
		if (pendingGeneration_ != generation_) {
			pendingGeneration_ = generation_;
			pendingKeyframes_ = 0;
			pendingLiveKeyframes_ = 0;
		}
		++pendingKeyframes_;
		if (!cachedReplay) {
			++pendingLiveKeyframes_;
		}
		return generation_;
	}

	// Returns true when this completion moved a waiting decoder back into the
	// synchronized state. A stale completion from an older recovery generation
	// cannot reopen the gate.
	bool onKeyframeSendCompleted(KeyframeTicket ticket, bool success, bool cachedReplay = false) noexcept
	{
		if (ticket == pendingGeneration_ && pendingKeyframes_ > 0) {
			--pendingKeyframes_;
			if (!cachedReplay && pendingLiveKeyframes_ > 0) {
				--pendingLiveKeyframes_;
			}
		}
		if (ticket != generation_) {
			return false;
		}

		if (!success) {
			awaitingKeyframe_ = true;
			cachedPrimeAllowed_ = false;
			return false;
		}

		if (cachedReplay) {
			// The cached IDR predates the current live frame. It is useful for
			// initial paint, but deltas must remain blocked until a live IDR
			// establishes the current prediction chain.
			awaitingKeyframe_ = true;
			cachedPrimeAllowed_ = false;
			return false;
		}

		const bool recovered = awaitingKeyframe_;
		awaitingKeyframe_ = false;
		cachedPrimeAllowed_ = false;
		return recovered;
	}

	// A first request can be part of initial decoder startup, where the cached
	// keyframe is exactly what the peer needs. A synchronized viewer's PLI is a
	// request for a future IDR, not proof that the sender lost a frame locally.
	// Keep live deltas flowing while the fixed <=2-second GOP reaches its next
	// natural IDR; otherwise a harmless decoder refresh becomes a guaranteed
	// sender-side freeze because libobs cannot force an IDR on demand. Known
	// local loss enters requireLiveKeyframe() separately and remains gated.
	DecoderRequestDisposition onDecoderKeyframeRequest() noexcept
	{
		if (awaitingKeyframe_ && cachedPrimeAllowed_ && pendingKeyframes_ == 0) {
			return DecoderRequestDisposition::CachedPrimeAllowed;
		}
		if (awaitingKeyframe_) {
			cachedPrimeAllowed_ = false;
			return pendingLiveKeyframes_ == 0 ? DecoderRequestDisposition::RequireNewLiveKeyframe
			                                  : DecoderRequestDisposition::PendingLiveKeyframe;
		}
		cachedPrimeAllowed_ = false;
		return pendingLiveKeyframes_ == 0 ? DecoderRequestDisposition::ContinueLiveStream
		                                  : DecoderRequestDisposition::PendingLiveKeyframe;
	}

	void resetForCachedPrime() noexcept
	{
		advanceGeneration();
		awaitingKeyframe_ = true;
		cachedPrimeAllowed_ = true;
	}

	// allowPendingKeyframe is appropriate after a transport failure when a newer
	// keyframe is already queued behind the failed frame. Upstream frame loss
	// must use the default and invalidate older queued keyframes.
	void requireLiveKeyframe(bool allowPendingKeyframe = false) noexcept
	{
		if (!allowPendingKeyframe || pendingLiveKeyframes_ == 0) {
			advanceGeneration();
		}
		awaitingKeyframe_ = true;
		cachedPrimeAllowed_ = false;
	}

	bool isAwaitingKeyframe() const noexcept { return awaitingKeyframe_; }
	bool isCachedPrimeAllowed() const noexcept { return cachedPrimeAllowed_; }
	bool hasPendingKeyframe() const noexcept { return pendingKeyframes_ > 0; }
	bool hasPendingLiveKeyframe() const noexcept { return pendingLiveKeyframes_ > 0; }

private:
	void advanceGeneration() noexcept
	{
		++generation_;
		if (generation_ == 0) {
			++generation_;
		}
		pendingGeneration_ = generation_;
		pendingKeyframes_ = 0;
		pendingLiveKeyframes_ = 0;
	}

	bool awaitingKeyframe_ = true;
	bool cachedPrimeAllowed_ = true;
	KeyframeTicket generation_ = 1;
	KeyframeTicket pendingGeneration_ = 1;
	size_t pendingKeyframes_ = 0;
	size_t pendingLiveKeyframes_ = 0;
};

} // namespace vdoninja
