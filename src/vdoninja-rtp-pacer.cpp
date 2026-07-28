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

constexpr size_t kDefaultMinimumQueueBytes = 4U * 1024U * 1024U;
constexpr size_t kMinimumRepairQueueBytes = 64U * 1024U;
constexpr size_t kMaxConsecutiveRepairPackets = 4;
constexpr auto kRepairBudgetWindow = std::chrono::milliseconds(100);
constexpr auto kRepairMaximumAge = std::chrono::milliseconds(500);
constexpr size_t kMinimumDuplicateQueueBytes = 64U * 1024U;
constexpr uint64_t kVideoPacerRateMultiplier = 2;
constexpr uint64_t kMinimumVideoPacerBitrate = 2000000;
constexpr uint64_t kMaximumVideoPacerBitrate = 100000000;

size_t calculateBurstBudget(uint64_t bitrateBitsPerSecond, std::chrono::milliseconds burstWindow)
{
	if (bitrateBitsPerSecond == 0 || burstWindow.count() <= 0) {
		throw std::invalid_argument("RTP pacer bitrate and burst window must be positive");
	}

	const long double bytes =
	    static_cast<long double>(bitrateBitsPerSecond) * static_cast<long double>(burstWindow.count()) / 8000.0L;
	if (bytes >= static_cast<long double>(std::numeric_limits<size_t>::max())) {
		return std::numeric_limits<size_t>::max();
	}
	return std::max<size_t>(1, static_cast<size_t>(std::ceil(bytes)));
}

size_t calculateDefaultQueueLimit(uint64_t bitrateBitsPerSecond)
{
	const uint64_t halfSecondBytes = bitrateBitsPerSecond / 16U;
	if (halfSecondBytes >= static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
		return std::numeric_limits<size_t>::max();
	}
	return std::max(kDefaultMinimumQueueBytes, static_cast<size_t>(halfSecondBytes));
}

size_t calculateTimedBudget(uint64_t bitrateBitsPerSecond, std::chrono::milliseconds window)
{
	if (bitrateBitsPerSecond == 0 || window.count() <= 0) {
		return 0;
	}
	const long double bytes =
	    static_cast<long double>(bitrateBitsPerSecond) * static_cast<long double>(window.count()) / 8000.0L;
	if (bytes >= static_cast<long double>(std::numeric_limits<size_t>::max())) {
		return std::numeric_limits<size_t>::max();
	}
	return std::max<size_t>(1, static_cast<size_t>(std::ceil(bytes)));
}

uint64_t calculateRepairBitrate(uint64_t bitrateBitsPerSecond)
{
	// The publisher's main pacer normally runs at twice the encoder rate.
	// Reserving one quarter of that rate caps NACK repair near half the encoded
	// media rate while leaving most scheduler slots available to live video.
	return std::max<uint64_t>(1, bitrateBitsPerSecond / 4U);
}

uint16_t rtpSequenceNumber(const RtpPacketPacer::Packet &packet)
{
	if (packet.size() < 4) {
		return 0;
	}
	return static_cast<uint16_t>((static_cast<uint16_t>(packet[2]) << 8) | static_cast<uint16_t>(packet[3]));
}

uint64_t elapsedMilliseconds(std::chrono::steady_clock::time_point end, std::chrono::steady_clock::time_point start)
{
	if (end <= start) {
		return 0;
	}
	return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count());
}

std::chrono::steady_clock::duration tokenWaitDuration(long double missingBytes, uint64_t bitrateBitsPerSecond)
{
	const long double nanoseconds =
	    missingBytes * 8.0L * 1000000000.0L / static_cast<long double>(bitrateBitsPerSecond);
	const auto roundedNanoseconds = static_cast<int64_t>(std::max<long double>(1.0L, std::ceil(nanoseconds)));
	return std::chrono::nanoseconds(roundedNanoseconds);
}

} // namespace

uint64_t videoPacerBitrateForEncoderRate(int encoderBitrate) noexcept
{
	const uint64_t positiveBitrate = static_cast<uint64_t>(std::max(encoderBitrate, 1));
	const uint64_t multiplied = positiveBitrate > kMaximumVideoPacerBitrate / kVideoPacerRateMultiplier
	                                ? kMaximumVideoPacerBitrate
	                                : positiveBitrate * kVideoPacerRateMultiplier;
	return std::clamp(multiplied, kMinimumVideoPacerBitrate, kMaximumVideoPacerBitrate);
}

uint64_t videoPacerBitrateForEncoderAndProtectionRate(int encoderBitrate, uint64_t protectionBitrate) noexcept
{
	const uint64_t positiveBitrate =
	    std::min(static_cast<uint64_t>(std::max(encoderBitrate, 1)), kMaximumVideoPacerBitrate);
	const uint64_t combined = protectionBitrate > kMaximumVideoPacerBitrate - positiveBitrate
	                              ? kMaximumVideoPacerBitrate
	                              : positiveBitrate + protectionBitrate;
	return std::max(videoPacerBitrateForEncoderRate(encoderBitrate), combined);
}

uint16_t rewindRtpSequenceNumber(uint16_t nextSequenceNumber, size_t unsentPackets) noexcept
{
	return static_cast<uint16_t>(nextSequenceNumber - static_cast<uint16_t>(unsentPackets));
}

RtpSharedPacerBudget::RtpSharedPacerBudget(size_t burstBudgetBytes)
    : burstBudgetBytes_(burstBudgetBytes), availableTokens_(static_cast<long double>(burstBudgetBytes)),
      lastTokenUpdate_(std::chrono::steady_clock::now())
{
	if (burstBudgetBytes_ == 0) {
		throw std::invalid_argument("Shared RTP pacer burst budget must be positive");
	}
}

uint64_t RtpSharedPacerBudget::addParticipant(uint64_t bitrateBitsPerSecond)
{
	if (bitrateBitsPerSecond == 0) {
		throw std::invalid_argument("Shared RTP pacer participant bitrate must be positive");
	}

	std::lock_guard<std::mutex> lock(mutex_);
	updateTokensLocked(std::chrono::steady_clock::now());
	const uint64_t participantId = nextParticipantId_++;
	participantRates_.emplace(participantId, bitrateBitsPerSecond);
	if (bitrateBitsPerSecond > std::numeric_limits<uint64_t>::max() - aggregateBitrateBitsPerSecond_) {
		aggregateBitrateBitsPerSecond_ = std::numeric_limits<uint64_t>::max();
	} else {
		aggregateBitrateBitsPerSecond_ += bitrateBitsPerSecond;
	}
	cv_.notify_all();
	return participantId;
}

void RtpSharedPacerBudget::updateParticipant(uint64_t participantId, uint64_t bitrateBitsPerSecond)
{
	if (participantId == 0 || bitrateBitsPerSecond == 0) {
		throw std::invalid_argument("Shared RTP pacer participant update must be positive");
	}

	std::lock_guard<std::mutex> lock(mutex_);
	const auto found = participantRates_.find(participantId);
	if (found == participantRates_.end()) {
		return;
	}
	updateTokensLocked(std::chrono::steady_clock::now());
	aggregateBitrateBitsPerSecond_ -= found->second;
	found->second = bitrateBitsPerSecond;
	if (bitrateBitsPerSecond > std::numeric_limits<uint64_t>::max() - aggregateBitrateBitsPerSecond_) {
		aggregateBitrateBitsPerSecond_ = std::numeric_limits<uint64_t>::max();
	} else {
		aggregateBitrateBitsPerSecond_ += bitrateBitsPerSecond;
	}
	cv_.notify_all();
}

void RtpSharedPacerBudget::removeParticipant(uint64_t participantId)
{
	if (participantId == 0) {
		return;
	}

	std::lock_guard<std::mutex> lock(mutex_);
	updateTokensLocked(std::chrono::steady_clock::now());
	const auto found = participantRates_.find(participantId);
	if (found == participantRates_.end()) {
		return;
	}
	aggregateBitrateBitsPerSecond_ -= found->second;
	participantRates_.erase(found);
	if (participantRates_.empty()) {
		availableTokens_ = static_cast<long double>(burstBudgetBytes_);
	}
	cv_.notify_all();
}

bool RtpSharedPacerBudget::acquire(size_t packetBytes, const std::function<bool()> &cancelled, bool *waited)
{
	if (packetBytes == 0) {
		if (waited) {
			*waited = false;
		}
		return true;
	}

	std::unique_lock<std::mutex> lock(mutex_);
	const uint64_t waiterId = nextWaiterId_++;
	waiters_.push_back(waiterId);
	bool didWait = false;
	auto removeWaiter = [&]() {
		const auto found = std::find(waiters_.begin(), waiters_.end(), waiterId);
		if (found != waiters_.end()) {
			waiters_.erase(found);
		}
		cv_.notify_all();
	};

	while (true) {
		if (cancelled && cancelled()) {
			removeWaiter();
			if (waited) {
				*waited = didWait;
			}
			return false;
		}

		const auto now = std::chrono::steady_clock::now();
		updateTokensLocked(now);
		if (aggregateBitrateBitsPerSecond_ == 0) {
			removeWaiter();
			if (waited) {
				*waited = didWait;
			}
			return false;
		}

		const bool atFront = !waiters_.empty() && waiters_.front() == waiterId;
		const long double requiredTokens = static_cast<long double>(std::min(packetBytes, burstBudgetBytes_));
		if (atFront && availableTokens_ >= requiredTokens) {
			// Subtract the full packet size, even when one packet is larger than
			// the bucket. The negative balance repays that unavoidable packet
			// burst before another packet is admitted.
			availableTokens_ -= static_cast<long double>(packetBytes);
			waiters_.pop_front();
			cv_.notify_all();
			if (waited) {
				*waited = didWait;
			}
			return true;
		}

		if (!atFront) {
			didWait = true;
			cv_.wait_for(lock, std::chrono::milliseconds(5));
			continue;
		}

		const auto waitDuration = tokenWaitDuration(requiredTokens - availableTokens_, aggregateBitrateBitsPerSecond_);
		const auto cancellationPoll =
		    std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::milliseconds(5));
		didWait = true;
		cv_.wait_for(lock, std::min(waitDuration, cancellationPoll));
	}
}

void RtpSharedPacerBudget::wake()
{
	cv_.notify_all();
}

uint64_t RtpSharedPacerBudget::bitrateBitsPerSecond() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return aggregateBitrateBitsPerSecond_;
}

size_t RtpSharedPacerBudget::participantCount() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return participantRates_.size();
}

void RtpSharedPacerBudget::updateTokensLocked(std::chrono::steady_clock::time_point now)
{
	if (now <= lastTokenUpdate_) {
		return;
	}
	if (aggregateBitrateBitsPerSecond_ > 0) {
		const long double elapsedSeconds = std::chrono::duration<long double>(now - lastTokenUpdate_).count();
		const long double addedTokens =
		    elapsedSeconds * static_cast<long double>(aggregateBitrateBitsPerSecond_) / 8.0L;
		availableTokens_ = std::min(static_cast<long double>(burstBudgetBytes_), availableTokens_ + addedTokens);
	}
	lastTokenUpdate_ = now;
}

RtpPacketPacer::RtpPacketPacer(uint64_t bitrateBitsPerSecond, std::chrono::milliseconds burstWindow,
                               SendCallback sendCallback, size_t maxQueueBytes,
                               std::shared_ptr<RtpSharedPacerBudget> sharedBudget,
                               RtpPacketDuplicationConfig duplicationConfig)
    : bitrateBitsPerSecond_(bitrateBitsPerSecond), burstWindow_(burstWindow),
      burstBudgetBytes_(calculateBurstBudget(bitrateBitsPerSecond, burstWindow)),
      repairBitrateBitsPerSecond_(calculateRepairBitrate(bitrateBitsPerSecond)),
      repairBudgetBytes_(calculateTimedBudget(calculateRepairBitrate(bitrateBitsPerSecond), kRepairBudgetWindow)),
      maxQueueBytes_(maxQueueBytes ? maxQueueBytes : calculateDefaultQueueLimit(bitrateBitsPerSecond)),
      sendCallback_(std::move(sendCallback)), sharedBudget_(std::move(sharedBudget)),
      duplicationConfig_(std::move(duplicationConfig)),
      duplicateBitrateBitsPerSecond_(duplicationConfig_.averageBitrateBitsPerSecond),
      duplicateBudgetBytes_(
          calculateTimedBudget(duplicationConfig_.averageBitrateBitsPerSecond, duplicationConfig_.budgetWindow)),
      duplicateQueueLimitBytes_(
          duplicationConfig_.mode == VideoProtectionMode::Off ||
                  calculateTimedBudget(duplicationConfig_.averageBitrateBitsPerSecond,
                                       duplicationConfig_.budgetWindow) == 0
              ? 0
              : std::min(maxQueueBytes_, std::max(kMinimumDuplicateQueueBytes,
                                                  calculateTimedBudget(duplicationConfig_.averageBitrateBitsPerSecond,
                                                                       duplicationConfig_.budgetWindow))))
{
	if (!sendCallback_) {
		throw std::invalid_argument("RTP pacer send callback is required");
	}
	if (maxQueueBytes_ == 0) {
		throw std::invalid_argument("RTP pacer queue limit must be positive");
	}
	if (duplicationConfig_.mode != VideoProtectionMode::Off &&
	    (duplicationConfig_.averageBitrateBitsPerSecond == 0 || duplicationConfig_.delay.count() < 0 ||
	     duplicationConfig_.maxAge.count() <= 0 || duplicationConfig_.budgetWindow.count() <= 0 ||
	     duplicationConfig_.delay >= duplicationConfig_.maxAge)) {
		throw std::invalid_argument("RTP packet duplication settings are invalid");
	}
	if (sharedBudget_) {
		sharedParticipantId_ = sharedBudget_->addParticipant(bitrateBitsPerSecond_.load(std::memory_order_acquire));
	}
	try {
		worker_ = std::thread(&RtpPacketPacer::run, this);
	} catch (...) {
		if (sharedBudget_) {
			sharedBudget_->removeParticipant(sharedParticipantId_);
			sharedParticipantId_ = 0;
		}
		throw;
	}
}

RtpPacketPacer::~RtpPacketPacer()
{
	stop();
}

bool RtpPacketPacer::enqueueFrame(std::vector<Packet> packets, RtpPacerFrameInfo info,
                                  FrameCompletionCallback completionCallback)
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
			++stats_.droppedFrames;
			return false;
		}

		QueuedFrame frame;
		frame.id = nextFrameId_++;
		frame.packets = std::move(packets);
		frame.remainingBytes = frameBytes;
		frame.queuedAt = now;
		frame.info = info;
		frame.completionCallback = std::move(completionCallback);
		queue_.push_back(std::move(frame));
		queuedBytes_ += frameBytes;
		stats_.queuedBytes = queuedBytes_;
		stats_.queuedFrames = queue_.size();
		stats_.maxQueuedBytes = std::max(stats_.maxQueuedBytes, queuedBytes_);
		stats_.maxQueuedFrames = std::max(stats_.maxQueuedFrames, queue_.size());
	}
	cv_.notify_one();
	return true;
}

bool RtpPacketPacer::enqueueRepair(Packet packet, SendCallback sendCallback,
                                   RepairCompletionCallback completionCallback)
{
	if (packet.empty() || !sendCallback) {
		return false;
	}

	const size_t packetBytes = packet.size();
	const size_t repairQueueLimit = std::max(kMinimumRepairQueueBytes, maxQueueBytes_ / 4U);
	const auto now = std::chrono::steady_clock::now();
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (stopping_ || packetBytes > repairQueueLimit || packetBytes > repairQueueLimit - queuedRepairBytes_ ||
		    packetBytes > maxQueueBytes_ - queuedBytes_) {
			++stats_.droppedRepairs;
			return false;
		}

		QueuedRepair repair;
		repair.packet = std::move(packet);
		repair.queuedAt = now;
		repair.expiresAt = now + kRepairMaximumAge;
		repair.sendCallback = std::move(sendCallback);
		repair.completionCallback = std::move(completionCallback);
		repairQueue_.push_back(std::move(repair));
		queuedBytes_ += packetBytes;
		queuedRepairBytes_ += packetBytes;
		++stats_.queuedRepairs;
		stats_.queuedBytes = queuedBytes_;
		stats_.maxQueuedBytes = std::max(stats_.maxQueuedBytes, queuedBytes_);
	}
	cv_.notify_one();
	return true;
}

bool RtpPacketPacer::shouldQueueDuplicate(const Packet &packet, const RtpPacerFrameInfo &info) const
{
	return duplicationConfig_.mode != VideoProtectionMode::Off &&
	       duplicateBudgetBytes_.load(std::memory_order_acquire) > 0 && packet.size() >= 4 &&
	       shouldDuplicateVideoPacket(duplicationConfig_.mode, info.keyframe, rtpSequenceNumber(packet));
}

void RtpPacketPacer::queueDuplicateLocked(Packet packet, std::chrono::steady_clock::time_point sentAt)
{
	if (packet.empty()) {
		return;
	}

	const size_t packetBytes = packet.size();
	if (duplicateQueueLimitBytes_ == 0 || packetBytes > duplicateQueueLimitBytes_ ||
	    packetBytes > duplicateQueueLimitBytes_ - queuedDuplicateBytes_ ||
	    packetBytes > maxQueueBytes_ - queuedBytes_) {
		++stats_.droppedDuplicates;
		return;
	}

	QueuedDuplicate duplicate;
	duplicate.packet = std::move(packet);
	duplicate.queuedAt = sentAt;
	duplicate.notBefore = sentAt + duplicationConfig_.delay;
	duplicate.expiresAt = sentAt + duplicationConfig_.maxAge;
	duplicateQueue_.push_back(std::move(duplicate));
	queuedBytes_ += packetBytes;
	queuedDuplicateBytes_ += packetBytes;
	++stats_.queuedDuplicates;
	stats_.queuedBytes = queuedBytes_;
	stats_.maxQueuedBytes = std::max(stats_.maxQueuedBytes, queuedBytes_);
	cv_.notify_one();
}

void RtpPacketPacer::pruneExpiredDuplicatesLocked(std::chrono::steady_clock::time_point now)
{
	while (!duplicateQueue_.empty() && now >= duplicateQueue_.front().expiresAt) {
		const size_t packetBytes = duplicateQueue_.front().packet.size();
		queuedBytes_ -= packetBytes;
		queuedDuplicateBytes_ -= packetBytes;
		duplicateQueue_.pop_front();
		++stats_.droppedDuplicates;
		++stats_.expiredDuplicates;
	}
	stats_.queuedBytes = queuedBytes_;
}

size_t RtpPacketPacer::discardQueuedDeltaFramesUntilKeyframe()
{
	std::lock_guard<std::mutex> lock(mutex_);
	size_t discardedFrames = 0;
	auto frame = queue_.begin();
	if (frame != queue_.end() && frame->started) {
		++frame;
	}
	while (frame != queue_.end() && !frame->info.keyframe) {
		queuedBytes_ -= frame->remainingBytes;
		frame = queue_.erase(frame);
		++discardedFrames;
	}
	if (discardedFrames != 0) {
		++queueMutationGeneration_;
	}
	stats_.queuedBytes = queuedBytes_;
	stats_.queuedFrames = queue_.size();
	stats_.droppedFrames += discardedFrames;
	cv_.notify_all();
	return discardedFrames;
}

size_t RtpPacketPacer::discardQueuedMediaFramesAfterCurrent(size_t *discardedPackets)
{
	std::lock_guard<std::mutex> lock(mutex_);
	auto firstDiscarded = queue_.begin();
	if (firstDiscarded != queue_.end() && firstDiscarded->started) {
		++firstDiscarded;
	}

	size_t discardedFrames = 0;
	size_t removedPackets = 0;
	for (auto frame = firstDiscarded; frame != queue_.end(); ++frame) {
		queuedBytes_ -= frame->remainingBytes;
		removedPackets += frame->packets.size() - frame->nextPacket;
		++discardedFrames;
	}
	queue_.erase(firstDiscarded, queue_.end());

	const size_t discardedDuplicates = duplicateQueue_.size();
	for (const auto &duplicate : duplicateQueue_) {
		queuedBytes_ -= duplicate.packet.size();
	}
	queuedDuplicateBytes_ = 0;
	duplicateQueue_.clear();
	if (discardedFrames != 0 || discardedDuplicates != 0) {
		++queueMutationGeneration_;
	}
	stats_.queuedBytes = queuedBytes_;
	stats_.queuedFrames = queue_.size();
	stats_.droppedFrames += discardedFrames;
	stats_.droppedDuplicates += discardedDuplicates;
	if (discardedPackets) {
		*discardedPackets = removedPackets;
	}
	cv_.notify_all();
	return discardedFrames;
}

void RtpPacketPacer::updateBitrate(uint64_t bitrateBitsPerSecond, uint64_t duplicateBitrateBitsPerSecond)
{
	if (bitrateBitsPerSecond == 0) {
		throw std::invalid_argument("RTP pacer updated bitrate must be positive");
	}
	if (duplicationConfig_.mode != VideoProtectionMode::Off && duplicateBitrateBitsPerSecond == 0) {
		throw std::invalid_argument("Enabled RTP duplication requires a positive updated repair bitrate");
	}

	bitrateBitsPerSecond_.store(bitrateBitsPerSecond, std::memory_order_release);
	burstBudgetBytes_.store(calculateBurstBudget(bitrateBitsPerSecond, burstWindow_), std::memory_order_release);
	const uint64_t repairBitrate = calculateRepairBitrate(bitrateBitsPerSecond);
	repairBitrateBitsPerSecond_.store(repairBitrate, std::memory_order_release);
	repairBudgetBytes_.store(calculateTimedBudget(repairBitrate, kRepairBudgetWindow), std::memory_order_release);
	duplicateBitrateBitsPerSecond_.store(duplicateBitrateBitsPerSecond, std::memory_order_release);
	duplicateBudgetBytes_.store(calculateTimedBudget(duplicateBitrateBitsPerSecond, duplicationConfig_.budgetWindow),
	                            std::memory_order_release);
	if (sharedBudget_ && sharedParticipantId_ != 0) {
		sharedBudget_->updateParticipant(sharedParticipantId_, bitrateBitsPerSecond);
	}
	cv_.notify_all();
}

void RtpPacketPacer::stop()
{
	std::lock_guard<std::mutex> stopLock(stopMutex_);
	{
		std::lock_guard<std::mutex> lock(mutex_);
		stopping_.store(true, std::memory_order_release);
	}
	cv_.notify_all();
	if (sharedBudget_) {
		sharedBudget_->wake();
	}

	if (worker_.joinable()) {
		worker_.join();
	}
	if (sharedBudget_ && sharedParticipantId_ != 0) {
		sharedBudget_->removeParticipant(sharedParticipantId_);
		sharedParticipantId_ = 0;
	}
}

RtpPacerStats RtpPacketPacer::getStats(bool resetInterval)
{
	std::lock_guard<std::mutex> lock(mutex_);
	stats_.queuedBytes = queuedBytes_;
	stats_.queuedFrames = queue_.size();
	const RtpPacerStats snapshot = stats_;
	if (resetInterval) {
		stats_.maxQueuedBytes = queuedBytes_;
		stats_.maxQueuedFrames = queue_.size();
		stats_.maxBatchBytes = 0;
		stats_.maxPacketDelayMs = 0;
		stats_.maxFrameSendDurationMs = 0;
		stats_.maxKeyframeSendDurationMs = 0;
		stats_.sentPackets = 0;
		stats_.sentFrames = 0;
		stats_.sentKeyframes = 0;
		stats_.droppedFrames = 0;
		stats_.failedFrames = 0;
		stats_.sendFailures = 0;
		stats_.queuedRepairs = 0;
		stats_.sentRepairs = 0;
		stats_.droppedRepairs = 0;
		stats_.expiredRepairs = 0;
		stats_.failedRepairs = 0;
		stats_.queuedDuplicates = 0;
		stats_.sentDuplicates = 0;
		stats_.droppedDuplicates = 0;
		stats_.expiredDuplicates = 0;
		stats_.failedDuplicates = 0;
		stats_.sentDuplicateBytes = 0;
	}
	return snapshot;
}

void RtpPacketPacer::run()
{
	std::unique_lock<std::mutex> lock(mutex_);
	long double availableTokens = static_cast<long double>(burstBudgetBytes_.load(std::memory_order_acquire));
	auto lastTokenUpdate = std::chrono::steady_clock::now();
	long double repairTokens = static_cast<long double>(repairBudgetBytes_.load(std::memory_order_acquire));
	auto lastRepairTokenUpdate = lastTokenUpdate;
	long double duplicateTokens = static_cast<long double>(duplicateBudgetBytes_.load(std::memory_order_acquire));
	auto lastDuplicateTokenUpdate = lastTokenUpdate;
	auto lastSendAt = std::chrono::steady_clock::time_point::min();
	size_t currentBurstBytes = 0;
	size_t consecutiveRepairPackets = 0;

	while (!stopping_.load(std::memory_order_acquire)) {
		auto now = std::chrono::steady_clock::now();
		pruneExpiredDuplicatesLocked(now);
		if (!repairQueue_.empty() && now >= repairQueue_.front().expiresAt) {
			QueuedRepair expired = std::move(repairQueue_.front());
			const size_t packetBytes = expired.packet.size();
			repairQueue_.pop_front();
			queuedBytes_ -= packetBytes;
			queuedRepairBytes_ -= packetBytes;
			stats_.queuedBytes = queuedBytes_;
			++stats_.droppedRepairs;
			++stats_.expiredRepairs;

			if (expired.completionCallback) {
				lock.unlock();
				try {
					expired.completionCallback(RtpPacerRepairOutcome::Expired);
				} catch (...) {
					// Repair diagnostics must never terminate the pacer.
				}
				lock.lock();
			}
			continue;
		}

		if (queue_.empty() && repairQueue_.empty() && duplicateQueue_.empty()) {
			currentBurstBytes = 0;
			consecutiveRepairPackets = 0;
			cv_.wait(lock, [this]() {
				return stopping_.load(std::memory_order_acquire) || !queue_.empty() || !repairQueue_.empty() ||
				       !duplicateQueue_.empty();
			});
			continue;
		}

		const uint64_t currentBitrate = bitrateBitsPerSecond_.load(std::memory_order_acquire);
		const size_t currentBurstBudget = burstBudgetBytes_.load(std::memory_order_acquire);
		availableTokens = std::min(availableTokens, static_cast<long double>(currentBurstBudget));
		if (now > lastTokenUpdate) {
			const long double elapsedSeconds = std::chrono::duration<long double>(now - lastTokenUpdate).count();
			const long double addedTokens = elapsedSeconds * static_cast<long double>(currentBitrate) / 8.0L;
			availableTokens = std::min(static_cast<long double>(currentBurstBudget), availableTokens + addedTokens);
			lastTokenUpdate = now;
		}
		const uint64_t currentRepairBitrate = repairBitrateBitsPerSecond_.load(std::memory_order_acquire);
		const size_t currentRepairBudget = repairBudgetBytes_.load(std::memory_order_acquire);
		repairTokens = std::min(repairTokens, static_cast<long double>(currentRepairBudget));
		if (now > lastRepairTokenUpdate) {
			const long double elapsedSeconds = std::chrono::duration<long double>(now - lastRepairTokenUpdate).count();
			const long double addedTokens = elapsedSeconds * static_cast<long double>(currentRepairBitrate) / 8.0L;
			repairTokens = std::min(static_cast<long double>(currentRepairBudget), repairTokens + addedTokens);
			lastRepairTokenUpdate = now;
		}
		const uint64_t currentDuplicateBitrate = duplicateBitrateBitsPerSecond_.load(std::memory_order_acquire);
		const size_t currentDuplicateBudget = duplicateBudgetBytes_.load(std::memory_order_acquire);
		duplicateTokens = std::min(duplicateTokens, static_cast<long double>(currentDuplicateBudget));
		if (currentDuplicateBudget > 0 && now > lastDuplicateTokenUpdate) {
			const long double elapsedSeconds =
			    std::chrono::duration<long double>(now - lastDuplicateTokenUpdate).count();
			const long double addedTokens = elapsedSeconds * static_cast<long double>(currentDuplicateBitrate) / 8.0L;
			duplicateTokens = std::min(static_cast<long double>(currentDuplicateBudget), duplicateTokens + addedTokens);
			lastDuplicateTokenUpdate = now;
		}

		if (!queue_.empty() && queue_.front().nextPacket >= queue_.front().packets.size()) {
			queue_.pop_front();
			stats_.queuedFrames = queue_.size();
			continue;
		}

		bool repairReady = false;
		if (!repairQueue_.empty()) {
			const size_t repairBytes = repairQueue_.front().packet.size();
			const long double requiredRepairTokens =
			    static_cast<long double>(std::min(repairBytes, currentRepairBudget));
			repairReady = repairTokens >= requiredRepairTokens;
		}
		const bool sendRepair =
		    repairReady && (queue_.empty() || consecutiveRepairPackets < kMaxConsecutiveRepairPackets);
		bool duplicateReady = false;
		if (!duplicateQueue_.empty() && now >= duplicateQueue_.front().notBefore) {
			const size_t duplicateBytes = duplicateQueue_.front().packet.size();
			const long double requiredDuplicateTokens =
			    static_cast<long double>(std::min(duplicateBytes, currentDuplicateBudget));
			duplicateReady = duplicateTokens >= requiredDuplicateTokens;
		}
		// Duplication is optional protection traffic. Never spend a live-media
		// pacing slot on it, even when the queued frame is still young: one
		// duplicate before every primary packet can otherwise add a full frame
		// of latency repeatedly during a large keyframe. Copies use only true
		// idle capacity and expire when the stream leaves none available.
		const bool liveMediaBacklogged = !queue_.empty();
		const bool sendDuplicate = !sendRepair && !liveMediaBacklogged && duplicateReady;

		if (!sendRepair && !sendDuplicate && queue_.empty()) {
			auto wakeAt = std::chrono::steady_clock::time_point::max();
			if (!repairQueue_.empty()) {
				const size_t repairBytes = repairQueue_.front().packet.size();
				const long double requiredRepairTokens =
				    static_cast<long double>(std::min(repairBytes, currentRepairBudget));
				const auto repairReadyAt =
				    now + tokenWaitDuration(requiredRepairTokens - repairTokens, currentRepairBitrate);
				wakeAt = std::min(repairReadyAt, repairQueue_.front().expiresAt);
			}
			if (!duplicateQueue_.empty()) {
				auto duplicateWakeAt = duplicateQueue_.front().notBefore;
				if (now >= duplicateWakeAt) {
					const size_t duplicateBytes = duplicateQueue_.front().packet.size();
					const long double requiredDuplicateTokens =
					    static_cast<long double>(std::min(duplicateBytes, currentDuplicateBudget));
					duplicateWakeAt =
					    now + tokenWaitDuration(requiredDuplicateTokens - duplicateTokens, currentDuplicateBitrate);
				}
				duplicateWakeAt = std::min(duplicateWakeAt, duplicateQueue_.front().expiresAt);
				wakeAt = std::min(wakeAt, duplicateWakeAt);
			}
			if (wakeAt == std::chrono::steady_clock::time_point::max()) {
				continue;
			}
			currentBurstBytes = 0;
			cv_.wait_until(lock, wakeAt);
			continue;
		}

		const size_t packetBytes = sendRepair      ? repairQueue_.front().packet.size()
		                           : sendDuplicate ? duplicateQueue_.front().packet.size()
		                                           : queue_.front().packets[queue_.front().nextPacket].size();
		const uint64_t selectedQueueMutationGeneration = queueMutationGeneration_;
		const long double requiredTokens = static_cast<long double>(std::min(packetBytes, currentBurstBudget));
		if (availableTokens < requiredTokens) {
			const auto wakeAt = now + tokenWaitDuration(requiredTokens - availableTokens, currentBitrate);
			currentBurstBytes = 0;
			cv_.wait_until(lock, wakeAt);
			continue;
		}

		if (sharedBudget_) {
			lock.unlock();
			bool sharedPacingWaited = false;
			const bool acquired = sharedBudget_->acquire(
			    packetBytes, [this]() { return stopping_.load(std::memory_order_acquire); }, &sharedPacingWaited);
			lock.lock();
			if (!acquired || stopping_.load(std::memory_order_acquire)) {
				continue;
			}
			if (selectedQueueMutationGeneration != queueMutationGeneration_) {
				// A recovery path discarded the selected frame or duplicate
				// while this worker waited on the aggregate viewer budget.
				// Re-select under the pacer lock before touching a queue front.
				currentBurstBytes = 0;
				continue;
			}
			if (sharedPacingWaited) {
				currentBurstBytes = 0;
			}
			now = std::chrono::steady_clock::now();
		}

		if (sendDuplicate && now >= duplicateQueue_.front().expiresAt) {
			pruneExpiredDuplicatesLocked(now);
			continue;
		}

		if (lastSendAt != std::chrono::steady_clock::time_point::min() && now - lastSendAt >= burstWindow_) {
			currentBurstBytes = 0;
		}
		availableTokens -= static_cast<long double>(packetBytes);
		currentBurstBytes += packetBytes;
		stats_.maxBatchBytes = std::max(stats_.maxBatchBytes, currentBurstBytes);
		lastSendAt = now;

		if (sendRepair) {
			QueuedRepair repair = std::move(repairQueue_.front());
			repairQueue_.pop_front();
			queuedBytes_ -= packetBytes;
			queuedRepairBytes_ -= packetBytes;
			stats_.queuedBytes = queuedBytes_;
			stats_.maxPacketDelayMs = std::max(stats_.maxPacketDelayMs, elapsedMilliseconds(now, repair.queuedAt));
			repairTokens -= static_cast<long double>(packetBytes);
			++consecutiveRepairPackets;

			lock.unlock();
			bool sent = false;
			try {
				sent = repair.sendCallback(std::move(repair.packet));
			} catch (...) {
				sent = false;
			}
			lock.lock();

			if (sent) {
				++stats_.sentPackets;
				++stats_.sentRepairs;
			} else {
				++stats_.sendFailures;
				++stats_.failedRepairs;
			}

			if (repair.completionCallback) {
				lock.unlock();
				try {
					repair.completionCallback(sent ? RtpPacerRepairOutcome::Sent : RtpPacerRepairOutcome::SendFailed);
				} catch (...) {
					// Repair diagnostics must never terminate the pacer.
				}
				lock.lock();
			}
			continue;
		}

		if (sendDuplicate) {
			QueuedDuplicate duplicate = std::move(duplicateQueue_.front());
			duplicateQueue_.pop_front();
			queuedBytes_ -= packetBytes;
			queuedDuplicateBytes_ -= packetBytes;
			stats_.queuedBytes = queuedBytes_;
			stats_.maxPacketDelayMs = std::max(stats_.maxPacketDelayMs, elapsedMilliseconds(now, duplicate.queuedAt));
			duplicateTokens -= static_cast<long double>(packetBytes);
			consecutiveRepairPackets = 0;

			lock.unlock();
			bool sent = false;
			try {
				sent = sendCallback_(std::move(duplicate.packet));
			} catch (...) {
				sent = false;
			}
			lock.lock();

			if (sent) {
				++stats_.sentPackets;
				++stats_.sentDuplicates;
				stats_.sentDuplicateBytes += packetBytes;
			} else {
				++stats_.sendFailures;
				++stats_.failedDuplicates;
			}
			continue;
		}

		consecutiveRepairPackets = 0;
		QueuedFrame &frame = queue_.front();
		if (!frame.started) {
			frame.started = true;
			frame.firstSendAt = now;
		}

		const uint64_t frameId = frame.id;
		Packet packet = std::move(frame.packets[frame.nextPacket]);
		Packet duplicatePacket;
		if (shouldQueueDuplicate(packet, frame.info)) {
			duplicatePacket = packet;
		}
		++frame.nextPacket;
		frame.remainingBytes -= packetBytes;
		queuedBytes_ -= packetBytes;
		stats_.queuedBytes = queuedBytes_;
		stats_.maxPacketDelayMs = std::max(stats_.maxPacketDelayMs, elapsedMilliseconds(now, frame.queuedAt));

		lock.unlock();
		bool sent = false;
		try {
			sent = sendCallback_(std::move(packet));
		} catch (...) {
			sent = false;
		}
		const auto completedAt = std::chrono::steady_clock::now();
		lock.lock();

		if (queue_.empty() || queue_.front().id != frameId) {
			continue;
		}

		QueuedFrame &updatedFrame = queue_.front();
		if (sent) {
			++updatedFrame.sentPackets;
			++stats_.sentPackets;
			if (!duplicatePacket.empty()) {
				queueDuplicateLocked(std::move(duplicatePacket), completedAt);
			}
		} else {
			++updatedFrame.sendFailures;
			++stats_.sendFailures;
		}

		const bool frameComplete = sent && updatedFrame.nextPacket >= updatedFrame.packets.size();
		const bool frameFailed = !sent;
		if (!frameComplete && !frameFailed) {
			continue;
		}

		if (frameFailed) {
			queuedBytes_ -= updatedFrame.remainingBytes;
			++stats_.failedFrames;
		} else {
			++stats_.sentFrames;
			if (updatedFrame.info.keyframe) {
				++stats_.sentKeyframes;
			}
		}
		stats_.queuedBytes = queuedBytes_;

		RtpPacerFrameResult result;
		result.info = updatedFrame.info;
		result.success = frameComplete;
		result.sentPackets = updatedFrame.sentPackets;
		result.sendFailures = updatedFrame.sendFailures;
		result.queueDelayMs = elapsedMilliseconds(updatedFrame.firstSendAt, updatedFrame.queuedAt);
		result.sendDurationMs = elapsedMilliseconds(completedAt, updatedFrame.firstSendAt);
		stats_.maxFrameSendDurationMs = std::max(stats_.maxFrameSendDurationMs, result.sendDurationMs);
		if (updatedFrame.info.keyframe) {
			stats_.maxKeyframeSendDurationMs = std::max(stats_.maxKeyframeSendDurationMs, result.sendDurationMs);
		}
		FrameCompletionCallback completionCallback = std::move(updatedFrame.completionCallback);
		queue_.pop_front();
		stats_.queuedFrames = queue_.size();

		if (completionCallback) {
			lock.unlock();
			try {
				completionCallback(result);
			} catch (...) {
				// Completion diagnostics must never terminate the pacer.
			}
			lock.lock();
		}
	}

	queue_.clear();
	repairQueue_.clear();
	duplicateQueue_.clear();
	queuedBytes_ = 0;
	queuedRepairBytes_ = 0;
	queuedDuplicateBytes_ = 0;
	stats_.queuedBytes = 0;
	stats_.queuedFrames = 0;
}

} // namespace vdoninja
