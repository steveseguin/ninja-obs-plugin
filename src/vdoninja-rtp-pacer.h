/*
 * OBS VDO.Ninja Plugin
 * Bounded RTP packet pacer
 */

#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace vdoninja
{

struct RtpPacerStats {
	size_t queuedBytes = 0;
	size_t maxQueuedBytes = 0;
	size_t maxBatchBytes = 0;
	uint64_t maxPacketDelayMs = 0;
	uint64_t sentPackets = 0;
	uint64_t droppedFrames = 0;
	uint64_t sendFailures = 0;
};

// Sends at most one bitrate-sized batch per interval. Frames are admitted
// atomically so a queue limit can never leave half an H.264 frame in flight.
class RtpPacketPacer
{
public:
	using Packet = std::vector<std::byte>;
	using SendCallback = std::function<void(Packet &&)>;

	RtpPacketPacer(uint64_t bitrateBitsPerSecond, std::chrono::milliseconds interval, SendCallback sendCallback,
	               size_t maxQueueBytes = 0);
	~RtpPacketPacer();

	RtpPacketPacer(const RtpPacketPacer &) = delete;
	RtpPacketPacer &operator=(const RtpPacketPacer &) = delete;

	bool enqueueFrame(std::vector<Packet> packets);
	void stop();
	RtpPacerStats getStats(bool resetInterval = false);

	size_t batchBudgetBytes() const noexcept { return batchBudgetBytes_; }
	size_t maxQueueBytes() const noexcept { return maxQueueBytes_; }

private:
	struct QueuedPacket {
		Packet payload;
		std::chrono::steady_clock::time_point queuedAt;
	};

	void run();

	const std::chrono::milliseconds interval_;
	const size_t batchBudgetBytes_;
	const size_t maxQueueBytes_;
	SendCallback sendCallback_;

	std::mutex stopMutex_;
	std::mutex mutex_;
	std::condition_variable cv_;
	std::deque<QueuedPacket> queue_;
	size_t queuedBytes_ = 0;
	bool stopping_ = false;
	RtpPacerStats stats_;
	std::thread worker_;
};

} // namespace vdoninja
