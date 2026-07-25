/*
 * OBS VDO.Ninja Plugin
 * Bounded RTP packet pacer implementation
 */

#include "vdoninja-rtp-pacer.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

namespace vdoninja
{

namespace
{

constexpr size_t kDefaultMinimumQueueBytes = 1024 * 1024;

size_t calculateBatchBudget(uint64_t bitrateBitsPerSecond, std::chrono::milliseconds interval)
{
	if (bitrateBitsPerSecond == 0 || interval.count() <= 0) {
		throw std::invalid_argument("RTP pacer bitrate and interval must be positive");
	}

	const long double bytes =
	    static_cast<long double>(bitrateBitsPerSecond) * static_cast<long double>(interval.count()) / 8000.0L;
	if (bytes >= static_cast<long double>(std::numeric_limits<size_t>::max())) {
		return std::numeric_limits<size_t>::max();
	}
	return std::max<size_t>(1, static_cast<size_t>(std::ceil(bytes)));
}

size_t calculateDefaultQueueLimit(uint64_t bitrateBitsPerSecond)
{
	const uint64_t halfSecondBytes = bitrateBitsPerSecond / 16;
	if (halfSecondBytes >= static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
		return std::numeric_limits<size_t>::max();
	}
	return std::max(kDefaultMinimumQueueBytes, static_cast<size_t>(halfSecondBytes));
}

} // namespace

RtpPacketPacer::RtpPacketPacer(uint64_t bitrateBitsPerSecond, std::chrono::milliseconds interval,
                               SendCallback sendCallback, size_t maxQueueBytes)
    : interval_(interval), batchBudgetBytes_(calculateBatchBudget(bitrateBitsPerSecond, interval)),
      maxQueueBytes_(maxQueueBytes ? maxQueueBytes : calculateDefaultQueueLimit(bitrateBitsPerSecond)),
      sendCallback_(std::move(sendCallback))
{
	if (!sendCallback_) {
		throw std::invalid_argument("RTP pacer send callback is required");
	}
	if (maxQueueBytes_ == 0) {
		throw std::invalid_argument("RTP pacer queue limit must be positive");
	}
	worker_ = std::thread(&RtpPacketPacer::run, this);
}

RtpPacketPacer::~RtpPacketPacer()
{
	stop();
}

bool RtpPacketPacer::enqueueFrame(std::vector<Packet> packets)
{
	if (packets.empty()) {
		return false;
	}

	size_t frameBytes = 0;
	for (const auto &packet : packets) {
		if (packet.size() > std::numeric_limits<size_t>::max() - frameBytes) {
			frameBytes = std::numeric_limits<size_t>::max();
			break;
		}
		frameBytes += packet.size();
	}

	const auto now = std::chrono::steady_clock::now();
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (stopping_ || frameBytes > maxQueueBytes_ || frameBytes > maxQueueBytes_ - queuedBytes_) {
			stats_.droppedFrames++;
			return false;
		}

		for (auto &packet : packets) {
			queue_.push_back({std::move(packet), now});
		}
		queuedBytes_ += frameBytes;
		stats_.queuedBytes = queuedBytes_;
		stats_.maxQueuedBytes = std::max(stats_.maxQueuedBytes, queuedBytes_);
	}
	cv_.notify_one();
	return true;
}

void RtpPacketPacer::stop()
{
	std::lock_guard<std::mutex> stopLock(stopMutex_);
	{
		std::lock_guard<std::mutex> lock(mutex_);
		stopping_ = true;
	}
	cv_.notify_all();

	if (worker_.joinable()) {
		worker_.join();
	}
}

RtpPacerStats RtpPacketPacer::getStats(bool resetInterval)
{
	std::lock_guard<std::mutex> lock(mutex_);
	stats_.queuedBytes = queuedBytes_;
	const RtpPacerStats snapshot = stats_;
	if (resetInterval) {
		stats_.maxQueuedBytes = queuedBytes_;
		stats_.maxBatchBytes = 0;
		stats_.maxPacketDelayMs = 0;
		stats_.sentPackets = 0;
		stats_.droppedFrames = 0;
		stats_.sendFailures = 0;
	}
	return snapshot;
}

void RtpPacketPacer::run()
{
	std::unique_lock<std::mutex> lock(mutex_);
	auto nextBatchAt = std::chrono::steady_clock::time_point::min();

	while (!stopping_) {
		cv_.wait(lock, [this]() { return stopping_ || !queue_.empty(); });
		if (stopping_) {
			break;
		}

		auto now = std::chrono::steady_clock::now();
		if (now < nextBatchAt) {
			cv_.wait_until(lock, nextBatchAt, [this]() { return stopping_; });
			continue;
		}

		std::vector<QueuedPacket> batch;
		size_t batchBytes = 0;
		while (!queue_.empty()) {
			const size_t packetBytes = queue_.front().payload.size();
			if (!batch.empty() && packetBytes > batchBudgetBytes_ - batchBytes) {
				break;
			}

			batchBytes += packetBytes;
			queuedBytes_ -= packetBytes;
			batch.push_back(std::move(queue_.front()));
			queue_.pop_front();

			// An individual packet larger than the budget must still make
			// progress, but it remains the only packet in this batch.
			if (batchBytes >= batchBudgetBytes_) {
				break;
			}
		}

		stats_.queuedBytes = queuedBytes_;
		stats_.maxBatchBytes = std::max(stats_.maxBatchBytes, batchBytes);
		for (const auto &packet : batch) {
			const auto delay = std::chrono::duration_cast<std::chrono::milliseconds>(now - packet.queuedAt);
			stats_.maxPacketDelayMs =
			    std::max(stats_.maxPacketDelayMs, static_cast<uint64_t>(std::max<int64_t>(0, delay.count())));
		}

		lock.unlock();
		uint64_t sentPackets = 0;
		uint64_t sendFailures = 0;
		for (auto &packet : batch) {
			try {
				sendCallback_(std::move(packet.payload));
				sentPackets++;
			} catch (...) {
				sendFailures++;
			}
		}
		lock.lock();

		stats_.sentPackets += sentPackets;
		stats_.sendFailures += sendFailures;
		nextBatchAt = std::chrono::steady_clock::now() + interval_;
	}

	queue_.clear();
	queuedBytes_ = 0;
	stats_.queuedBytes = 0;
}

} // namespace vdoninja
