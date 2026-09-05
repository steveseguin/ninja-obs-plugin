/*
 * OBS VDO.Ninja Plugin
 * Bounded RTP packet pacer
 */

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include "vdoninja-loss-protection.h"

namespace vdoninja
{

// Reclaims a contiguous tail of assigned-but-unsent RTP sequence numbers.
// uint16_t conversion intentionally preserves RTP wrap-around semantics.
uint16_t rewindRtpSequenceNumber(uint16_t nextSequenceNumber, size_t unsentPackets) noexcept;

// The pacer is a burst smoother, not an encoder bitrate limiter. Some
// VideoToolbox modes substantially overshoot very low configured bitrates, so
// retain enough egress headroom to prevent that overshoot becoming latency.
uint64_t videoPacerBitrateForEncoderRate(int encoderBitrate) noexcept;
uint64_t videoPacerBitrateForEncoderAndProtectionRate(int encoderBitrate, uint64_t protectionBitrate) noexcept;

// A small aggregate token bucket shared by every viewer pacer. Per-peer pacers
// still preserve frame ordering and fairness, while this budget prevents all
// viewers from releasing their short burst allowance at the same instant.
class RtpSharedPacerBudget
{
public:
	explicit RtpSharedPacerBudget(size_t burstBudgetBytes);

	uint64_t addParticipant(uint64_t bitrateBitsPerSecond);
	void updateParticipant(uint64_t participantId, uint64_t bitrateBitsPerSecond);
	void removeParticipant(uint64_t participantId);
	bool acquire(size_t packetBytes, const std::function<bool()> &cancelled, bool *waited = nullptr);
	void wake();

	uint64_t bitrateBitsPerSecond() const;
	size_t burstBudgetBytes() const noexcept { return burstBudgetBytes_; }
	size_t participantCount() const;

private:
	void recalculateBitrateLocked();
	void updateTokensLocked(std::chrono::steady_clock::time_point now);

	const size_t burstBudgetBytes_;
	mutable std::mutex mutex_;
	std::condition_variable cv_;
	std::unordered_map<uint64_t, uint64_t> participantRates_;
	std::deque<uint64_t> waiters_;
	uint64_t aggregateBitrateBitsPerSecond_ = 0;
	uint64_t nextParticipantId_ = 1;
	uint64_t nextWaiterId_ = 1;
	long double availableTokens_ = 0.0L;
	std::chrono::steady_clock::time_point lastTokenUpdate_;
};

struct RtpPacerStats {
	size_t queuedBytes = 0;
	size_t maxQueuedBytes = 0;
	size_t queuedFrames = 0;
	size_t maxQueuedFrames = 0;
	// Kept as "batch" for log and script compatibility. This is now the
	// largest token-bucket burst sent without an intervening pacing wait.
	size_t maxBatchBytes = 0;
	uint64_t maxPacketDelayMs = 0;
	uint64_t maxFrameSendDurationMs = 0;
	uint64_t maxKeyframeSendDurationMs = 0;
	uint64_t sentPackets = 0;
	uint64_t sentFrames = 0;
	uint64_t sentKeyframes = 0;
	uint64_t droppedFrames = 0;
	uint64_t failedFrames = 0;
	uint64_t sendFailures = 0;
	uint64_t queuedRepairs = 0;
	uint64_t sentRepairs = 0;
	uint64_t droppedRepairs = 0;
	uint64_t expiredRepairs = 0;
	uint64_t failedRepairs = 0;
	uint64_t queuedDuplicates = 0;
	uint64_t sentDuplicates = 0;
	uint64_t droppedDuplicates = 0;
	uint64_t expiredDuplicates = 0;
	uint64_t failedDuplicates = 0;
	uint64_t sentDuplicateBytes = 0;
};

struct RtpPacerFrameInfo {
	bool keyframe = false;
	uint32_t timestamp = 0;
};

struct RtpPacerFrameResult {
	RtpPacerFrameInfo info;
	bool success = false;
	uint64_t sentPackets = 0;
	uint64_t sendFailures = 0;
	uint64_t queueDelayMs = 0;
	uint64_t sendDurationMs = 0;
};

enum class RtpPacerRepairOutcome {
	Sent,
	Expired,
	SendFailed,
};

struct RtpPacketDuplicationConfig {
	VideoProtectionMode mode = VideoProtectionMode::Off;
	uint64_t averageBitrateBitsPerSecond = 0;
	std::chrono::milliseconds delay{15};
	std::chrono::milliseconds maxAge{250};
	std::chrono::milliseconds budgetWindow{2000};
};

// Smooths RTP egress with a small token bucket. Frames are admitted atomically,
// retain their boundaries, and complete in FIFO order. The bucket permits a
// short ordinary-frame burst while large keyframes are spread over time.
class RtpPacketPacer
{
public:
	using Packet = std::vector<std::byte>;
	// Returning false reports a transport-level rejection that did not throw.
	// libdatachannel uses this path when its ICE/UDP send cannot accept a packet.
	using SendCallback = std::function<bool(Packet &&)>;
	using FrameCompletionCallback = std::function<void(const RtpPacerFrameResult &)>;
	using RepairCompletionCallback = std::function<void(RtpPacerRepairOutcome)>;

	RtpPacketPacer(uint64_t bitrateBitsPerSecond, std::chrono::milliseconds burstWindow, SendCallback sendCallback,
	               size_t maxQueueBytes = 0, std::shared_ptr<RtpSharedPacerBudget> sharedBudget = {},
	               RtpPacketDuplicationConfig duplicationConfig = {});
	~RtpPacketPacer();

	RtpPacketPacer(const RtpPacketPacer &) = delete;
	RtpPacketPacer &operator=(const RtpPacketPacer &) = delete;

	bool enqueueFrame(std::vector<Packet> packets, RtpPacerFrameInfo info = {},
	                  FrameCompletionCallback completionCallback = {});
	bool enqueueRepair(Packet packet, SendCallback sendCallback, RepairCompletionCallback completionCallback = {});
	size_t discardQueuedDeltaFramesUntilKeyframe();
	// Drops every not-yet-started media frame. If the front frame is already
	// being transmitted, it is allowed to finish so a partial RTP frame is
	// never created deliberately. discardedPackets receives the number of
	// assigned-but-unsent RTP packets removed from the tail, allowing the
	// caller to reclaim their sequence numbers without creating NACK-visible
	// gaps.
	size_t discardQueuedMediaFramesAfterCurrent(size_t *discardedPackets = nullptr);
	void updateBitrate(uint64_t bitrateBitsPerSecond, uint64_t duplicateBitrateBitsPerSecond = 0);
	void stop();
	RtpPacerStats getStats(bool resetInterval = false);

	size_t batchBudgetBytes() const noexcept { return burstBudgetBytes_.load(std::memory_order_acquire); }
	size_t maxQueueBytes() const noexcept { return maxQueueBytes_; }
	uint64_t bitrateBitsPerSecond() const noexcept { return bitrateBitsPerSecond_.load(std::memory_order_acquire); }

private:
	struct QueuedFrame {
		uint64_t id = 0;
		std::vector<Packet> packets;
		size_t nextPacket = 0;
		size_t remainingBytes = 0;
		std::chrono::steady_clock::time_point queuedAt;
		std::chrono::steady_clock::time_point firstSendAt;
		RtpPacerFrameInfo info;
		FrameCompletionCallback completionCallback;
		uint64_t sentPackets = 0;
		uint64_t sendFailures = 0;
		bool started = false;
	};

	struct QueuedRepair {
		Packet packet;
		std::chrono::steady_clock::time_point queuedAt;
		std::chrono::steady_clock::time_point expiresAt;
		SendCallback sendCallback;
		RepairCompletionCallback completionCallback;
	};

	struct QueuedDuplicate {
		Packet packet;
		std::chrono::steady_clock::time_point queuedAt;
		std::chrono::steady_clock::time_point notBefore;
		std::chrono::steady_clock::time_point expiresAt;
	};

	bool shouldQueueDuplicate(const Packet &packet, const RtpPacerFrameInfo &info) const;
	void queueDuplicateLocked(Packet packet, std::chrono::steady_clock::time_point sentAt);
	void pruneExpiredDuplicatesLocked(std::chrono::steady_clock::time_point now);
	void run();

	std::atomic<uint64_t> bitrateBitsPerSecond_;
	const std::chrono::milliseconds burstWindow_;
	std::atomic<size_t> burstBudgetBytes_;
	std::atomic<uint64_t> repairBitrateBitsPerSecond_;
	std::atomic<size_t> repairBudgetBytes_;
	const size_t maxQueueBytes_;
	SendCallback sendCallback_;
	std::shared_ptr<RtpSharedPacerBudget> sharedBudget_;
	uint64_t sharedParticipantId_ = 0;
	const RtpPacketDuplicationConfig duplicationConfig_;
	std::atomic<uint64_t> duplicateBitrateBitsPerSecond_;
	std::atomic<size_t> duplicateBudgetBytes_;
	const size_t duplicateQueueLimitBytes_;

	std::mutex stopMutex_;
	std::mutex mutex_;
	std::condition_variable cv_;
	std::deque<QueuedFrame> queue_;
	std::deque<QueuedRepair> repairQueue_;
	std::deque<QueuedDuplicate> duplicateQueue_;
	size_t queuedBytes_ = 0;
	size_t queuedRepairBytes_ = 0;
	size_t queuedDuplicateBytes_ = 0;
	uint64_t queueMutationGeneration_ = 0;
	uint64_t nextFrameId_ = 1;
	std::atomic<bool> stopping_{false};
	RtpPacerStats stats_;
	std::thread worker_;
};

} // namespace vdoninja
