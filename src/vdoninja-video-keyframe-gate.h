/*
 * OBS VDO.Ninja Plugin
 * Per-peer video synchronization state
 */

#pragma once

namespace vdoninja
{

// Tracks whether a peer can consume live delta frames and whether replaying the
// cached keyframe is still safe. A cached keyframe is useful for a new peer, but
// it must not be replayed after an already-synchronized decoder reports loss:
// the live encoder has moved on to a different prediction chain by then.
class VideoKeyframeGate
{
public:
	bool canQueueFrame(bool keyframe, bool cachedReplay) const noexcept
	{
		if (cachedReplay) {
			return keyframe && awaitingKeyframe_ && cachedPrimeAllowed_;
		}
		return keyframe || !awaitingKeyframe_;
	}

	void onKeyframeQueued() noexcept
	{
		awaitingKeyframe_ = false;
		cachedPrimeAllowed_ = false;
	}

	// A first request can be part of initial decoder startup, where the cached
	// keyframe is exactly what the peer needs. Once synchronized (or already
	// waiting for live recovery), a request must wait for the encoder's next live
	// keyframe instead of rewinding to the cached GOP.
	void onDecoderKeyframeRequest() noexcept
	{
		if (!awaitingKeyframe_ || !cachedPrimeAllowed_) {
			awaitingKeyframe_ = true;
			cachedPrimeAllowed_ = false;
		}
	}

	void resetForCachedPrime() noexcept
	{
		awaitingKeyframe_ = true;
		cachedPrimeAllowed_ = true;
	}

	void requireLiveKeyframe() noexcept
	{
		awaitingKeyframe_ = true;
		cachedPrimeAllowed_ = false;
	}

	bool isAwaitingKeyframe() const noexcept { return awaitingKeyframe_; }
	bool isCachedPrimeAllowed() const noexcept { return cachedPrimeAllowed_; }

private:
	bool awaitingKeyframe_ = true;
	bool cachedPrimeAllowed_ = true;
};

} // namespace vdoninja
