/*
 * OBS/FFmpeg-linked validation gate for the production native receiver path.
 * This executable does not initialize or launch OBS.
 */

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <exception>
#include <functional>
#include <iostream>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "vdoninja-source.h"
#include "vdoninja-utils.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/opt.h>
}

extern "C" const char *obs_module_text(const char *lookupString)
{
	return lookupString;
}

namespace
{

using namespace vdoninja;
using namespace std::chrono_literals;

void require(bool condition, const std::string &message)
{
	if (!condition) {
		throw std::runtime_error(message);
	}
}

template <typename Predicate>
void requireEventually(Predicate &&predicate, const std::string &message, std::chrono::milliseconds timeout = 5s)
{
	const auto deadline = std::chrono::steady_clock::now() + timeout;
	while (!predicate()) {
		if (std::chrono::steady_clock::now() >= deadline) {
			throw std::runtime_error(message);
		}
		std::this_thread::sleep_for(5ms);
	}
}

template <typename Predicate> bool waitUntil(Predicate &&predicate, std::chrono::milliseconds timeout = 5s)
{
	const auto deadline = std::chrono::steady_clock::now() + timeout;
	while (!predicate()) {
		if (std::chrono::steady_clock::now() >= deadline) {
			return false;
		}
		std::this_thread::sleep_for(5ms);
	}
	return true;
}

std::string ffmpegError(int error)
{
	char buffer[AV_ERROR_MAX_STRING_SIZE] = {};
	av_strerror(error, buffer, sizeof(buffer));
	return buffer;
}

std::vector<uint8_t> encodeVp9Keyframe(uint8_t luma)
{
	const AVCodec *codec = avcodec_find_encoder(AV_CODEC_ID_VP9);
	require(codec != nullptr, "FFmpeg VP9 encoder is unavailable");

	AVCodecContext *context = avcodec_alloc_context3(codec);
	require(context != nullptr, "failed to allocate VP9 encoder context");
	context->width = 16;
	context->height = 16;
	context->pix_fmt = AV_PIX_FMT_YUV420P;
	context->time_base = AVRational{1, 90000};
	context->framerate = AVRational{30, 1};
	context->gop_size = 1;
	context->max_b_frames = 0;
	context->bit_rate = 100000;
	av_opt_set(context->priv_data, "deadline", "realtime", 0);
	av_opt_set_int(context->priv_data, "lag-in-frames", 0, 0);
	av_opt_set_int(context->priv_data, "cpu-used", 8, 0);

	const int openResult = avcodec_open2(context, codec, nullptr);
	if (openResult < 0) {
		const std::string message = "failed to open VP9 encoder: " + ffmpegError(openResult);
		avcodec_free_context(&context);
		throw std::runtime_error(message);
	}

	AVFrame *frame = av_frame_alloc();
	AVPacket *packet = av_packet_alloc();
	require(frame != nullptr && packet != nullptr, "failed to allocate VP9 encoder frame/packet");
	frame->format = context->pix_fmt;
	frame->width = context->width;
	frame->height = context->height;
	frame->pts = 0;
	const int bufferResult = av_frame_get_buffer(frame, 32);
	require(bufferResult >= 0, "failed to allocate VP9 encoder frame buffer");
	require(av_frame_make_writable(frame) >= 0, "failed to make VP9 encoder frame writable");
	for (int y = 0; y < frame->height; ++y) {
		std::memset(frame->data[0] + static_cast<ptrdiff_t>(y) * frame->linesize[0], luma,
		            static_cast<size_t>(frame->width));
	}
	for (int plane = 1; plane <= 2; ++plane) {
		for (int y = 0; y < frame->height / 2; ++y) {
			std::memset(frame->data[plane] + static_cast<ptrdiff_t>(y) * frame->linesize[plane], 128,
			            static_cast<size_t>(frame->width / 2));
		}
	}

	int result = avcodec_send_frame(context, frame);
	require(result >= 0, "failed to submit VP9 encoder frame: " + ffmpegError(result));
	result = avcodec_receive_packet(context, packet);
	if (result == AVERROR(EAGAIN)) {
		require(avcodec_send_frame(context, nullptr) >= 0, "failed to flush VP9 encoder");
		result = avcodec_receive_packet(context, packet);
	}
	require(result >= 0, "failed to receive VP9 packet: " + ffmpegError(result));
	require((packet->flags & AV_PKT_FLAG_KEY) != 0, "generated VP9 access unit was not a keyframe");
	std::vector<uint8_t> accessUnit(packet->data, packet->data + packet->size);

	av_packet_free(&packet);
	av_frame_free(&frame);
	avcodec_free_context(&context);
	return accessUnit;
}

std::vector<std::vector<uint8_t>> encodeVp9Gop(uint8_t firstLuma, size_t frameCount)
{
	const AVCodec *codec = avcodec_find_encoder(AV_CODEC_ID_VP9);
	require(codec != nullptr, "FFmpeg VP9 encoder is unavailable");
	AVCodecContext *context = avcodec_alloc_context3(codec);
	require(context != nullptr, "failed to allocate VP9 GOP encoder context");
	context->width = 16;
	context->height = 16;
	context->pix_fmt = AV_PIX_FMT_YUV420P;
	context->time_base = AVRational{1, 90000};
	context->framerate = AVRational{30, 1};
	context->gop_size = static_cast<int>(std::max<size_t>(frameCount, 2));
	context->max_b_frames = 0;
	context->bit_rate = 100000;
	av_opt_set(context->priv_data, "deadline", "realtime", 0);
	av_opt_set_int(context->priv_data, "lag-in-frames", 0, 0);
	av_opt_set_int(context->priv_data, "cpu-used", 8, 0);
	const int openResult = avcodec_open2(context, codec, nullptr);
	if (openResult < 0) {
		const std::string message = "failed to open VP9 GOP encoder: " + ffmpegError(openResult);
		avcodec_free_context(&context);
		throw std::runtime_error(message);
	}

	AVFrame *frame = av_frame_alloc();
	AVPacket *packet = av_packet_alloc();
	require(frame != nullptr && packet != nullptr, "failed to allocate VP9 GOP encoder frame/packet");
	frame->format = context->pix_fmt;
	frame->width = context->width;
	frame->height = context->height;
	require(av_frame_get_buffer(frame, 32) >= 0, "failed to allocate VP9 GOP encoder frame buffer");

	std::vector<std::vector<uint8_t>> accessUnits;
	const auto drainPackets = [&]() {
		while (true) {
			const int result = avcodec_receive_packet(context, packet);
			if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) {
				return;
			}
			require(result >= 0, "failed to receive VP9 GOP packet: " + ffmpegError(result));
			accessUnits.emplace_back(packet->data, packet->data + packet->size);
			av_packet_unref(packet);
		}
	};

	for (size_t index = 0; index < frameCount; ++index) {
		require(av_frame_make_writable(frame) >= 0, "failed to make VP9 GOP frame writable");
		frame->pts = static_cast<int64_t>(index);
		const uint8_t luma = static_cast<uint8_t>(firstLuma + (index % 31));
		for (int y = 0; y < frame->height; ++y) {
			std::memset(frame->data[0] + static_cast<ptrdiff_t>(y) * frame->linesize[0], luma,
			            static_cast<size_t>(frame->width));
		}
		for (int plane = 1; plane <= 2; ++plane) {
			for (int y = 0; y < frame->height / 2; ++y) {
				std::memset(frame->data[plane] + static_cast<ptrdiff_t>(y) * frame->linesize[plane], 128,
				            static_cast<size_t>(frame->width / 2));
			}
		}
		const int result = avcodec_send_frame(context, frame);
		require(result >= 0, "failed to submit VP9 GOP frame: " + ffmpegError(result));
		drainPackets();
	}
	require(avcodec_send_frame(context, nullptr) >= 0, "failed to flush VP9 GOP encoder");
	drainPackets();
	require(accessUnits.size() == frameCount, "VP9 GOP encoder did not produce one access unit per input frame");
	require(!accessUnits.empty(), "VP9 GOP encoder produced no access units");

	av_packet_free(&packet);
	av_frame_free(&frame);
	avcodec_free_context(&context);
	return accessUnits;
}

class OutputCollector
{
public:
	void add(NativeMediaTestOutput output)
	{
		std::lock_guard<std::mutex> lock(mutex_);
		outputs_.push_back(std::move(output));
	}

	std::vector<NativeMediaTestOutput> copy() const
	{
		std::lock_guard<std::mutex> lock(mutex_);
		return outputs_;
	}

	size_t size() const
	{
		std::lock_guard<std::mutex> lock(mutex_);
		return outputs_.size();
	}

private:
	mutable std::mutex mutex_;
	std::vector<NativeMediaTestOutput> outputs_;
};

class StageLatch
{
public:
	StageLatch(NativeMediaTestStage stage, bool alpha) : stage_(stage), alpha_(alpha) {}

	void hook(NativeMediaTestStage stage, bool alpha, uint32_t, uint64_t)
	{
		std::unique_lock<std::mutex> lock(mutex_);
		if (entered_ || stage != stage_ || alpha != alpha_) {
			return;
		}
		entered_ = true;
		enteredCondition_.notify_all();
		releaseCondition_.wait(lock, [this]() { return released_; });
	}

	void waitUntilEntered()
	{
		std::unique_lock<std::mutex> lock(mutex_);
		require(enteredCondition_.wait_for(lock, 10s, [this]() { return entered_; }),
		        "packet path did not reach requested stage latch");
	}

	void release()
	{
		std::lock_guard<std::mutex> lock(mutex_);
		released_ = true;
		releaseCondition_.notify_all();
	}

private:
	NativeMediaTestStage stage_;
	bool alpha_ = false;
	std::mutex mutex_;
	std::condition_variable enteredCondition_;
	std::condition_variable releaseCondition_;
	bool entered_ = false;
	bool released_ = false;
};

class TrackCommitLatch
{
public:
	explicit TrackCommitLatch(const rtc::Track *target) : target_(target) {}

	void hook(const std::shared_ptr<rtc::Track> &track)
	{
		std::unique_lock<std::mutex> lock(mutex_);
		if (track.get() != target_ || entered_) {
			return;
		}
		entered_ = true;
		enteredCondition_.notify_all();
		releaseCondition_.wait(lock, [this]() { return released_; });
	}

	void waitUntilEntered()
	{
		std::unique_lock<std::mutex> lock(mutex_);
		require(enteredCondition_.wait_for(lock, 10s, [this]() { return entered_; }),
		        "RTC manager path did not reach the post-slot-commit latch");
	}

	void release()
	{
		std::lock_guard<std::mutex> lock(mutex_);
		released_ = true;
		releaseCondition_.notify_all();
	}

private:
	const rtc::Track *target_ = nullptr;
	std::mutex mutex_;
	std::condition_variable enteredCondition_;
	std::condition_variable releaseCondition_;
	bool entered_ = false;
	bool released_ = false;
};

class DataChannelStageLatch
{
public:
	DataChannelStageLatch(NativeMediaTestDataChannelStage stage, const rtc::DataChannel *target = nullptr)
	    : stage_(stage), target_(target)
	{
	}

	void hook(NativeMediaTestDataChannelStage stage, const std::shared_ptr<rtc::DataChannel> &channel)
	{
		std::unique_lock<std::mutex> lock(mutex_);
		if (entered_ || stage != stage_ || (target_ && channel.get() != target_)) {
			return;
		}
		entered_ = true;
		enteredCondition_.notify_all();
		releaseCondition_.wait(lock, [this]() { return released_; });
	}

	void waitUntilEntered()
	{
		std::unique_lock<std::mutex> lock(mutex_);
		require(enteredCondition_.wait_for(lock, 10s, [this]() { return entered_; }),
		        "DataChannel path did not reach the requested lifecycle stage");
	}

	void release()
	{
		std::lock_guard<std::mutex> lock(mutex_);
		released_ = true;
		releaseCondition_.notify_all();
	}

private:
	NativeMediaTestDataChannelStage stage_;
	const rtc::DataChannel *target_ = nullptr;
	std::mutex mutex_;
	std::condition_variable enteredCondition_;
	std::condition_variable releaseCondition_;
	bool entered_ = false;
	bool released_ = false;
};

class PeerDispatchLatch
{
public:
	explicit PeerDispatchLatch(uint64_t generation) : generation_(generation) {}

	void hook(const std::shared_ptr<PeerInfo> &peer)
	{
		std::unique_lock<std::mutex> lock(mutex_);
		if (!peer || peer->generation != generation_ || entered_) {
			return;
		}
		entered_ = true;
		enteredCondition_.notify_all();
		releaseCondition_.wait(lock, [this]() { return released_; });
	}

	void waitUntilEntered()
	{
		std::unique_lock<std::mutex> lock(mutex_);
		require(enteredCondition_.wait_for(lock, 10s, [this]() { return entered_; }),
		        "RTC lifecycle path did not reach the post-current-peer validation latch");
	}

	void release()
	{
		std::lock_guard<std::mutex> lock(mutex_);
		released_ = true;
		releaseCondition_.notify_all();
	}

private:
	uint64_t generation_ = 0;
	std::mutex mutex_;
	std::condition_variable enteredCondition_;
	std::condition_variable releaseCondition_;
	bool entered_ = false;
	bool released_ = false;
};

class SignalingLifecycleAdmissionLatch
{
public:
	SignalingLifecycleAdmissionLatch(uint64_t socketEpoch, uint64_t generation)
	    : socketEpoch_(socketEpoch), generation_(generation)
	{
	}

	void hook(const SignalingLifecycleEvent &event, const std::optional<PeerEventIdentity> &identity)
	{
		std::unique_lock<std::mutex> lock(mutex_);
		if (entered_ || event.socketEpoch != socketEpoch_ || !identity || identity->generation != generation_) {
			return;
		}
		entered_ = true;
		enteredCondition_.notify_all();
		releaseCondition_.wait(lock, [this]() { return released_; });
	}

	void waitUntilEntered()
	{
		std::unique_lock<std::mutex> lock(mutex_);
		require(enteredCondition_.wait_for(lock, 10s, [this]() { return entered_; }),
		        "signaling lifecycle path did not reach the post-admission latch");
	}

	void release()
	{
		std::lock_guard<std::mutex> lock(mutex_);
		released_ = true;
		releaseCondition_.notify_all();
	}

private:
	uint64_t socketEpoch_ = 0;
	uint64_t generation_ = 0;
	std::mutex mutex_;
	std::condition_variable enteredCondition_;
	std::condition_variable releaseCondition_;
	bool entered_ = false;
	bool released_ = false;
};

class PeerDispatchBarrier
{
public:
	PeerDispatchBarrier(uint64_t generation, size_t expected) : generation_(generation), expected_(expected) {}

	void hook(const std::shared_ptr<PeerInfo> &peer)
	{
		std::unique_lock<std::mutex> lock(mutex_);
		if (!peer || peer->generation != generation_ || released_) {
			return;
		}
		++entered_;
		enteredCondition_.notify_all();
		releaseCondition_.wait(lock, [this]() { return released_; });
	}

	void waitUntilEntered()
	{
		std::unique_lock<std::mutex> lock(mutex_);
		require(enteredCondition_.wait_for(lock, 10s, [this]() { return entered_ >= expected_; }),
		        "RTC lifecycle paths did not all reach the post-current-peer validation barrier");
	}

	void release()
	{
		std::lock_guard<std::mutex> lock(mutex_);
		released_ = true;
		releaseCondition_.notify_all();
	}

private:
	uint64_t generation_ = 0;
	size_t expected_ = 0;
	std::mutex mutex_;
	std::condition_variable enteredCondition_;
	std::condition_variable releaseCondition_;
	size_t entered_ = 0;
	bool released_ = false;
};

class TrackDispatchLatch
{
public:
	TrackDispatchLatch(const rtc::Track *retiredTrack, TrackType type) : retiredTrack_(retiredTrack), type_(type) {}

	void hook(const TrackSlotEvent &event, bool hadSubscriber)
	{
		std::unique_lock<std::mutex> lock(mutex_);
		if (entered_ || hadSubscriber || event.track || event.retiredTrack.get() != retiredTrack_ ||
		    event.type != type_) {
			return;
		}
		entered_ = true;
		enteredCondition_.notify_all();
		releaseCondition_.wait(lock, [this]() { return released_; });
	}

	void waitUntilEntered()
	{
		std::unique_lock<std::mutex> lock(mutex_);
		require(enteredCondition_.wait_for(lock, 10s, [this]() { return entered_; }),
		        "track retirement did not reach the post-null-subscriber snapshot latch");
	}

	void release()
	{
		std::lock_guard<std::mutex> lock(mutex_);
		released_ = true;
		releaseCondition_.notify_all();
	}

private:
	const rtc::Track *retiredTrack_ = nullptr;
	TrackType type_ = TrackType::Video;
	std::mutex mutex_;
	std::condition_variable enteredCondition_;
	std::condition_variable releaseCondition_;
	bool entered_ = false;
	bool released_ = false;
};

class OwnerSessionProbe
{
public:
	void block(NativeMediaTestOwnerSessionStage stage, PeerManagerCompletionKind kind, const void *handle = nullptr)
	{
		std::lock_guard<std::mutex> lock(mutex_);
		blockStage_ = stage;
		blockKind_ = kind;
		blockHandle_ = handle;
		blockEnabled_ = true;
	}

	void hook(NativeMediaTestOwnerSessionStage stage, PeerManagerCompletionKind kind, const void *handle)
	{
		std::unique_lock<std::mutex> lock(mutex_);
		events_.push_back({stage, kind, handle});
		eventCondition_.notify_all();
		if (!blockEnabled_ || blockEntered_ || stage != blockStage_ || kind != blockKind_ ||
		    (blockHandle_ && blockHandle_ != handle)) {
			return;
		}
		blockEntered_ = true;
		blockEnteredCondition_.notify_all();
		blockReleaseCondition_.wait(lock, [this]() { return blockReleased_; });
	}

	void waitUntilBlocked()
	{
		std::unique_lock<std::mutex> lock(mutex_);
		require(blockEnteredCondition_.wait_for(lock, 10s, [this]() { return blockEntered_; }),
		        "owner-session completion did not reach its deterministic block stage");
	}

	void waitUntilSeen(NativeMediaTestOwnerSessionStage stage, PeerManagerCompletionKind kind, size_t expected = 1,
	                   const void *handle = nullptr)
	{
		std::unique_lock<std::mutex> lock(mutex_);
		require(eventCondition_.wait_for(
		            lock, 10s,
		            [this, stage, kind, expected, handle]() { return countLocked(stage, kind, handle) >= expected; }),
		        "owner-session completion did not reach the requested deterministic event count");
	}

	size_t count(NativeMediaTestOwnerSessionStage stage, PeerManagerCompletionKind kind,
	             const void *handle = nullptr) const
	{
		std::lock_guard<std::mutex> lock(mutex_);
		return countLocked(stage, kind, handle);
	}

	bool firstPrecedes(NativeMediaTestOwnerSessionStage firstStage, PeerManagerCompletionKind firstKind,
	                   NativeMediaTestOwnerSessionStage secondStage, PeerManagerCompletionKind secondKind,
	                   const void *firstHandle = nullptr, const void *secondHandle = nullptr) const
	{
		std::lock_guard<std::mutex> lock(mutex_);
		const auto first =
		    std::find_if(events_.begin(), events_.end(), [firstStage, firstKind, firstHandle](const Event &event) {
			    return event.stage == firstStage && event.kind == firstKind &&
			           (!firstHandle || event.handle == firstHandle);
		    });
		const auto second =
		    std::find_if(events_.begin(), events_.end(), [secondStage, secondKind, secondHandle](const Event &event) {
			    return event.stage == secondStage && event.kind == secondKind &&
			           (!secondHandle || event.handle == secondHandle);
		    });
		return first != events_.end() && second != events_.end() && first < second;
	}

	void releaseBlock()
	{
		std::lock_guard<std::mutex> lock(mutex_);
		blockReleased_ = true;
		blockReleaseCondition_.notify_all();
	}

private:
	struct Event {
		NativeMediaTestOwnerSessionStage stage;
		PeerManagerCompletionKind kind;
		const void *handle = nullptr;
	};

	size_t countLocked(NativeMediaTestOwnerSessionStage stage, PeerManagerCompletionKind kind, const void *handle) const
	{
		return static_cast<size_t>(
		    std::count_if(events_.begin(), events_.end(), [stage, kind, handle](const Event &event) {
			    return event.stage == stage && event.kind == kind && (!handle || event.handle == handle);
		    }));
	}

	mutable std::mutex mutex_;
	std::condition_variable eventCondition_;
	std::condition_variable blockEnteredCondition_;
	std::condition_variable blockReleaseCondition_;
	std::vector<Event> events_;
	NativeMediaTestOwnerSessionStage blockStage_ = NativeMediaTestOwnerSessionStage::BeforePermit;
	PeerManagerCompletionKind blockKind_ = PeerManagerCompletionKind::TrackClosed;
	const void *blockHandle_ = nullptr;
	bool blockEnabled_ = false;
	bool blockEntered_ = false;
	bool blockReleased_ = false;
};

class OwnerSessionHookGuard
{
public:
	OwnerSessionHookGuard(std::unique_ptr<VDONinjaPeerManager> &manager, OwnerSessionProbe &probe)
	    : manager_(manager), probe_(probe)
	{
	}

	OwnerSessionHookGuard(const OwnerSessionHookGuard &) = delete;
	OwnerSessionHookGuard &operator=(const OwnerSessionHookGuard &) = delete;

	~OwnerSessionHookGuard() { cleanup(); }

	void disarm() noexcept { armed_ = false; }

private:
	void cleanup() noexcept
	{
		if (!armed_) {
			return;
		}
		probe_.releaseBlock();
		if (manager_) {
			try {
				manager_->setNativeMediaTestOwnerSessionHook(nullptr);
			} catch (...) {
			}
			manager_.reset();
		}
	}

	std::unique_ptr<VDONinjaPeerManager> &manager_;
	OwnerSessionProbe &probe_;
	bool armed_ = true;
};

class OwnerSessionThreadGuard
{
public:
	OwnerSessionThreadGuard(OwnerSessionProbe &probe, std::function<void()> run)
	    : probe_(probe), thread_(std::move(run))
	{
	}

	OwnerSessionThreadGuard(const OwnerSessionThreadGuard &) = delete;
	OwnerSessionThreadGuard &operator=(const OwnerSessionThreadGuard &) = delete;

	~OwnerSessionThreadGuard()
	{
		probe_.releaseBlock();
		join();
	}

	void join()
	{
		if (thread_.joinable()) {
			thread_.join();
		}
	}

private:
	OwnerSessionProbe &probe_;
	std::thread thread_;
};

class StageSignal
{
public:
	explicit StageSignal(NativeMediaTestStage stage) : stage_(stage) {}

	void hook(NativeMediaTestStage stage)
	{
		if (stage != stage_) {
			return;
		}
		std::lock_guard<std::mutex> lock(mutex_);
		reached_ = true;
		condition_.notify_all();
	}

	void waitUntilReached()
	{
		std::unique_lock<std::mutex> lock(mutex_);
		require(condition_.wait_for(lock, 10s, [this]() { return reached_; }),
		        "native media path did not reach the requested exact signal stage");
	}

private:
	NativeMediaTestStage stage_;
	std::mutex mutex_;
	std::condition_variable condition_;
	bool reached_ = false;
};

class StageCounter
{
public:
	explicit StageCounter(NativeMediaTestStage stage) : stage_(stage) {}

	void hook(NativeMediaTestStage stage)
	{
		if (stage != stage_) {
			return;
		}
		std::lock_guard<std::mutex> lock(mutex_);
		++count_;
		condition_.notify_all();
	}

	void waitUntilCount(size_t expected)
	{
		std::unique_lock<std::mutex> lock(mutex_);
		require(condition_.wait_for(lock, 10s, [this, expected]() { return count_ >= expected; }),
		        "native media path did not reach the requested deterministic stage count");
	}

private:
	NativeMediaTestStage stage_;
	std::mutex mutex_;
	std::condition_variable condition_;
	size_t count_ = 0;
};

class EventRecorder
{
public:
	void add(std::string event)
	{
		std::lock_guard<std::mutex> lock(mutex_);
		events_.push_back(std::move(event));
	}

	std::vector<std::string> copy() const
	{
		std::lock_guard<std::mutex> lock(mutex_);
		return events_;
	}

private:
	mutable std::mutex mutex_;
	std::vector<std::string> events_;
};

std::shared_ptr<rtc::Track> addVp9ReceiveTrack(const std::shared_ptr<PeerInfo> &peer, const std::string &mid)
{
	require(peer && peer->pc, "linked RTC gate did not create a real PeerConnection");
	rtc::Description::Video description(mid, rtc::Description::Direction::RecvOnly);
	description.addVP9Codec(98);
	auto track = peer->pc->addTrack(description);
	require(track != nullptr, "linked RTC gate did not create a real libdatachannel Track");
	return track;
}

std::shared_ptr<rtc::Track> addOpusReceiveTrack(const std::shared_ptr<PeerInfo> &peer, const std::string &mid)
{
	require(peer && peer->pc, "linked RTC gate did not create a real PeerConnection");
	rtc::Description::Audio description(mid, rtc::Description::Direction::RecvOnly);
	description.addOpusCodec(111);
	auto track = peer->pc->addTrack(description);
	require(track != nullptr, "linked RTC gate did not create a real libdatachannel audio Track");
	return track;
}

struct OwnedRtcTrack {
	std::shared_ptr<rtc::PeerConnection> peerConnection;
	std::shared_ptr<rtc::Track> track;
};

struct OwnedRtcDataChannel {
	std::shared_ptr<rtc::PeerConnection> peerConnection;
	std::shared_ptr<rtc::DataChannel> channel;
};

OwnedRtcDataChannel makeUnconnectedDataChannel(const std::string &label)
{
	OwnedRtcDataChannel owned;
	owned.peerConnection = std::make_shared<rtc::PeerConnection>(rtc::Configuration{});
	owned.channel = owned.peerConnection->createDataChannel(label);
	require(owned.channel != nullptr, "linked RTC gate did not create an independent real DataChannel");
	return owned;
}

OwnedRtcTrack makeVp9ReceiveTrack(const std::string &mid)
{
	OwnedRtcTrack owned;
	owned.peerConnection = std::make_shared<rtc::PeerConnection>(rtc::Configuration{});
	rtc::Description::Video description(mid, rtc::Description::Direction::RecvOnly);
	description.addVP9Codec(98);
	owned.track = owned.peerConnection->addTrack(description);
	require(owned.track != nullptr, "linked RTC gate did not create an independent real Track");
	return owned;
}

class LocalRtcSignalingRelay : public std::enable_shared_from_this<LocalRtcSignalingRelay>
{
public:
	using ErrorCallback = std::function<void(const std::string &error)>;

	static std::shared_ptr<LocalRtcSignalingRelay> create(const std::shared_ptr<rtc::PeerConnection> &first,
	                                                      const std::shared_ptr<rtc::PeerConnection> &second,
	                                                      ErrorCallback onError,
	                                                      bool forceFirstCandidateBeforeDescription = false)
	{
		auto relay = std::shared_ptr<LocalRtcSignalingRelay>(
		    new LocalRtcSignalingRelay(first, second, std::move(onError), forceFirstCandidateBeforeDescription));
		relay->install(first, second);
		return relay;
	}

	bool firstCandidateWasQueuedBeforeDescription() const
	{
		std::lock_guard<std::mutex> lock(mutex_);
		return firstToSecond_.candidatesQueuedBeforeDescription != 0;
	}

	bool waitForFirstCandidateBeforeDescription()
	{
		std::unique_lock<std::mutex> lock(mutex_);
		return candidateQueued_.wait_for(lock, 10s,
		                                 [this]() { return firstToSecond_.candidatesQueuedBeforeDescription != 0; });
	}

	void releaseFirstDescription()
	{
		{
			std::lock_guard<std::mutex> lock(mutex_);
			firstToSecond_.requireCandidateBeforeDescription = false;
		}
		applyDescriptionAndQueuedCandidates(Direction::FirstToSecond);
	}

private:
	enum class Direction { FirstToSecond, SecondToFirst };

	struct PendingRemoteSignals {
		std::weak_ptr<rtc::PeerConnection> target;
		std::optional<rtc::Description> description;
		std::vector<rtc::Candidate> candidates;
		bool applyingDescription = false;
		bool descriptionApplied = false;
		bool requireCandidateBeforeDescription = false;
		size_t candidatesQueuedBeforeDescription = 0;
	};

	LocalRtcSignalingRelay(const std::shared_ptr<rtc::PeerConnection> &first,
	                       const std::shared_ptr<rtc::PeerConnection> &second, ErrorCallback onError,
	                       bool forceFirstCandidateBeforeDescription)
	    : onError_(std::move(onError))
	{
		firstToSecond_.target = second;
		firstToSecond_.requireCandidateBeforeDescription = forceFirstCandidateBeforeDescription;
		secondToFirst_.target = first;
	}

	void install(const std::shared_ptr<rtc::PeerConnection> &first, const std::shared_ptr<rtc::PeerConnection> &second)
	{
		const std::weak_ptr<LocalRtcSignalingRelay> weakRelay = shared_from_this();
		first->onLocalDescription([weakRelay](rtc::Description description) {
			if (const auto relay = weakRelay.lock()) {
				relay->queueDescription(Direction::FirstToSecond, std::move(description));
			}
		});
		second->onLocalDescription([weakRelay](rtc::Description description) {
			if (const auto relay = weakRelay.lock()) {
				relay->queueDescription(Direction::SecondToFirst, std::move(description));
			}
		});
		first->onLocalCandidate([weakRelay](rtc::Candidate candidate) {
			if (const auto relay = weakRelay.lock()) {
				relay->queueCandidate(Direction::FirstToSecond, std::move(candidate));
			}
		});
		second->onLocalCandidate([weakRelay](rtc::Candidate candidate) {
			if (const auto relay = weakRelay.lock()) {
				relay->queueCandidate(Direction::SecondToFirst, std::move(candidate));
			}
		});
	}

	PendingRemoteSignals &pending(Direction direction)
	{
		return direction == Direction::FirstToSecond ? firstToSecond_ : secondToFirst_;
	}

	void queueDescription(Direction direction, rtc::Description description)
	{
		{
			std::lock_guard<std::mutex> lock(mutex_);
			pending(direction).description = std::move(description);
		}
		applyDescriptionAndQueuedCandidates(direction);
	}

	void queueCandidate(Direction direction, rtc::Candidate candidate)
	{
		std::shared_ptr<rtc::PeerConnection> target;
		bool descriptionApplied = false;
		{
			std::lock_guard<std::mutex> lock(mutex_);
			auto &signals = pending(direction);
			if (signals.descriptionApplied) {
				descriptionApplied = true;
				target = signals.target.lock();
			} else {
				++signals.candidatesQueuedBeforeDescription;
				signals.candidates.push_back(std::move(candidate));
				candidateQueued_.notify_all();
			}
		}
		if (!descriptionApplied) {
			applyDescriptionAndQueuedCandidates(direction);
			return;
		}
		if (!target) {
			reportError("local RTC signaling target expired before remote candidate");
			return;
		}
		addCandidate(target, candidate);
	}

	void applyDescriptionAndQueuedCandidates(Direction direction)
	{
		std::shared_ptr<rtc::PeerConnection> target;
		std::optional<rtc::Description> description;
		{
			std::lock_guard<std::mutex> lock(mutex_);
			auto &signals = pending(direction);
			if (signals.descriptionApplied || signals.applyingDescription || !signals.description ||
			    signals.requireCandidateBeforeDescription) {
				return;
			}
			target = signals.target.lock();
			description = signals.description;
			signals.applyingDescription = true;
		}
		if (!target) {
			reportError("local RTC signaling target expired before remote description");
			return;
		}
		try {
			target->setRemoteDescription(*description);
		} catch (const std::exception &error) {
			{
				std::lock_guard<std::mutex> lock(mutex_);
				pending(direction).applyingDescription = false;
			}
			reportError(error.what());
			return;
		}

		std::vector<rtc::Candidate> candidates;
		{
			std::lock_guard<std::mutex> lock(mutex_);
			auto &signals = pending(direction);
			signals.descriptionApplied = true;
			signals.applyingDescription = false;
			signals.description.reset();
			candidates.swap(signals.candidates);
		}
		for (const auto &candidate : candidates) {
			addCandidate(target, candidate);
		}
	}

	void addCandidate(const std::shared_ptr<rtc::PeerConnection> &target, const rtc::Candidate &candidate)
	{
		try {
			target->addRemoteCandidate(candidate);
		} catch (const std::exception &error) {
			reportError(error.what());
		}
	}

	void reportError(const std::string &error)
	{
		if (onError_) {
			onError_(error);
		}
	}

	mutable std::mutex mutex_;
	std::condition_variable candidateQueued_;
	PendingRemoteSignals firstToSecond_;
	PendingRemoteSignals secondToFirst_;
	ErrorCallback onError_;
};

struct ConnectedRtcTrackPair {
	struct State {
		std::mutex mutex;
		std::condition_variable condition;
		std::shared_ptr<rtc::Track> receiver;
		bool senderConnected = false;
		bool senderTrackOpen = false;
		std::string error;
	};

	std::shared_ptr<State> state;
	std::shared_ptr<rtc::PeerConnection> senderPeer;
	std::shared_ptr<rtc::PeerConnection> receiverPeer;
	std::shared_ptr<rtc::Track> sender;
	std::shared_ptr<rtc::Track> receiver;
	std::shared_ptr<LocalRtcSignalingRelay> signalingRelay;
};

struct ConnectedRtcDataChannelPair {
	struct State {
		std::mutex mutex;
		std::condition_variable condition;
		size_t openCount = 0;
		std::vector<std::pair<std::string, std::string>> messages;
		std::string error;
	};

	std::shared_ptr<State> state;
	std::shared_ptr<PeerInfo> receiver;
	std::shared_ptr<rtc::PeerConnection> senderPeer;
	std::vector<std::shared_ptr<rtc::DataChannel>> senderChannels;
	std::shared_ptr<LocalRtcSignalingRelay> signalingRelay;
};

struct ConnectedUnmanagedDataChannelPair {
	struct State {
		std::mutex mutex;
		std::condition_variable condition;
		std::shared_ptr<rtc::DataChannel> receiver;
		bool senderOpen = false;
		std::string error;
	};

	std::shared_ptr<State> state;
	std::shared_ptr<rtc::PeerConnection> senderPeer;
	std::shared_ptr<rtc::PeerConnection> receiverPeer;
	std::shared_ptr<rtc::DataChannel> sender;
	std::shared_ptr<rtc::DataChannel> receiver;
	std::shared_ptr<LocalRtcSignalingRelay> signalingRelay;
	size_t connectionAttempts = 0;
};

enum class UnmanagedRtcAttemptDisposition { Connected, RetryableTimeout, Fatal };

struct UnmanagedRtcConnectionAttempt {
	std::optional<ConnectedUnmanagedDataChannelPair> pair;
	UnmanagedRtcAttemptDisposition disposition = UnmanagedRtcAttemptDisposition::Fatal;
	std::string failure;
};

template <typename Action> void runRtcCleanupNoexcept(Action &&action) noexcept
{
	try {
		action();
	} catch (...) {
	}
}

void discardUnmanagedDataChannelPair(ConnectedUnmanagedDataChannelPair &pair) noexcept
{
	std::shared_ptr<rtc::DataChannel> receiver = pair.receiver;
	if (pair.state) {
		std::lock_guard<std::mutex> lock(pair.state->mutex);
		if (pair.state->receiver) {
			receiver = std::move(pair.state->receiver);
		}
	}
	if (pair.sender) {
		runRtcCleanupNoexcept([&]() { pair.sender->onOpen(nullptr); });
	}
	if (receiver) {
		runRtcCleanupNoexcept([&]() { receiver->onOpen(nullptr); });
	}
	if (pair.senderPeer) {
		runRtcCleanupNoexcept([&]() { pair.senderPeer->onStateChange(nullptr); });
		runRtcCleanupNoexcept([&]() { pair.senderPeer->onLocalDescription(nullptr); });
		runRtcCleanupNoexcept([&]() { pair.senderPeer->onLocalCandidate(nullptr); });
		runRtcCleanupNoexcept([&]() { pair.senderPeer->close(); });
	}
	if (pair.receiverPeer) {
		runRtcCleanupNoexcept([&]() { pair.receiverPeer->onStateChange(nullptr); });
		runRtcCleanupNoexcept([&]() { pair.receiverPeer->onLocalDescription(nullptr); });
		runRtcCleanupNoexcept([&]() { pair.receiverPeer->onLocalCandidate(nullptr); });
		runRtcCleanupNoexcept([&]() { pair.receiverPeer->onDataChannel(nullptr); });
		runRtcCleanupNoexcept([&]() { pair.receiverPeer->close(); });
	}
	pair.signalingRelay.reset();
	pair.sender.reset();
	pair.receiver.reset();
	pair.senderPeer.reset();
	pair.receiverPeer.reset();
	pair.state.reset();
}

UnmanagedRtcConnectionAttempt tryMakeConnectedUnmanagedDataChannelPair(const std::string &label,
                                                                       bool forceSenderCandidateBeforeDescription)
{
	UnmanagedRtcConnectionAttempt result;
	ConnectedUnmanagedDataChannelPair pair;
	pair.state = std::make_shared<ConnectedUnmanagedDataChannelPair::State>();
	pair.senderPeer = std::make_shared<rtc::PeerConnection>(rtc::Configuration{});
	pair.receiverPeer = std::make_shared<rtc::PeerConnection>(rtc::Configuration{});
	const auto state = pair.state;
	const auto recordError = [state](const std::string &error) {
		std::lock_guard<std::mutex> lock(state->mutex);
		state->error = error;
		state->condition.notify_all();
	};

	pair.receiverPeer->onDataChannel([state](std::shared_ptr<rtc::DataChannel> channel) {
		std::lock_guard<std::mutex> lock(state->mutex);
		state->receiver = std::move(channel);
		state->condition.notify_all();
	});
	const std::weak_ptr<ConnectedUnmanagedDataChannelPair::State> weakState = state;
	const auto observePeerState = [weakState](rtc::PeerConnection::State peerState) {
		if (const auto state = weakState.lock()) {
			std::lock_guard<std::mutex> lock(state->mutex);
			if (peerState == rtc::PeerConnection::State::Failed) {
				state->error = "unmanaged RTC PeerConnection failed before DataChannel adoption";
			}
			state->condition.notify_all();
		}
	};
	pair.senderPeer->onStateChange(observePeerState);
	pair.receiverPeer->onStateChange(observePeerState);
	pair.signalingRelay = LocalRtcSignalingRelay::create(pair.senderPeer, pair.receiverPeer, recordError,
	                                                     forceSenderCandidateBeforeDescription);

	pair.sender = pair.senderPeer->createDataChannel(label);
	if (!pair.sender) {
		result.failure = "linked RTC gate did not create unmanaged sender DataChannel";
		discardUnmanagedDataChannelPair(pair);
		return result;
	}
	pair.sender->onOpen([state]() {
		std::lock_guard<std::mutex> lock(state->mutex);
		state->senderOpen = true;
		state->condition.notify_all();
	});
	try {
		pair.senderPeer->setLocalDescription(rtc::Description::Type::Offer);
	} catch (const std::exception &error) {
		result.failure = error.what();
		discardUnmanagedDataChannelPair(pair);
		return result;
	}
	if (forceSenderCandidateBeforeDescription) {
		if (!pair.signalingRelay->waitForFirstCandidateBeforeDescription()) {
			result.failure = "forced candidate-before-description gate did not observe a local candidate";
			discardUnmanagedDataChannelPair(pair);
			return result;
		}
		pair.signalingRelay->releaseFirstDescription();
	}
	{
		std::unique_lock<std::mutex> lock(state->mutex);
		const bool completed = state->condition.wait_for(lock, 10s, [&]() {
			return !state->error.empty() || (state->senderOpen && state->receiver && state->receiver->isOpen());
		});
		if (!completed || !state->error.empty()) {
			result.failure = state->error.empty() ? "unmanaged RTC DataChannel pair did not connect" : state->error;
			result.disposition = !completed && state->error.empty() ? UnmanagedRtcAttemptDisposition::RetryableTimeout
			                                                        : UnmanagedRtcAttemptDisposition::Fatal;
			lock.unlock();
			discardUnmanagedDataChannelPair(pair);
			return result;
		}
		pair.receiver = state->receiver;
		state->receiver.reset();
	}
	result.pair = std::move(pair);
	result.disposition = UnmanagedRtcAttemptDisposition::Connected;
	return result;
}

ConnectedUnmanagedDataChannelPair
makeConnectedUnmanagedDataChannelPair(const std::string &label, bool forceSenderCandidateBeforeDescription = false)
{
	std::string failures;
	size_t attemptsMade = 0;
	for (size_t attempt = 1; attempt <= 3; ++attempt) {
		attemptsMade = attempt;
		auto result = tryMakeConnectedUnmanagedDataChannelPair(label, forceSenderCandidateBeforeDescription);
		if (result.disposition == UnmanagedRtcAttemptDisposition::Connected && result.pair) {
			result.pair->connectionAttempts = attempt;
			std::cerr << "[RTC SETUP] connected on attempt " << attempt << "/3\n";
			return std::move(*result.pair);
		}
		failures +=
		    (failures.empty() ? "" : "; ") + std::string("attempt ") + std::to_string(attempt) + ": " + result.failure;
		if (result.disposition != UnmanagedRtcAttemptDisposition::RetryableTimeout || attempt == 3) {
			break;
		}
		std::cerr << "[RTC SETUP RETRY] attempt " << attempt << "/3: readiness timeout before adoption\n";
	}
	require(false, "unmanaged RTC DataChannel pair failed after " + std::to_string(attemptsMade) +
	                   " fresh attempt(s): " + failures);
	return {};
}

ConnectedRtcDataChannelPair makeConnectedManagerDataChannelPair(VDONinjaPeerManager &manager, const std::string &uuid,
                                                                const std::vector<std::string> &labels,
                                                                std::function<void()> senderOpenReadyProbe = {})
{
	require(!labels.empty(), "local RTC DataChannel pair requires at least one label");
	require(std::any_of(labels.begin(), labels.end(),
	                    [](const std::string &label) { return label.empty() || label == "sendChannel"; }),
	        "local RTC DataChannel pair requires an accepted control-channel label");
	ConnectedRtcDataChannelPair pair;
	pair.state = std::make_shared<ConnectedRtcDataChannelPair::State>();
	pair.receiver = manager.createNativeMediaTestViewerPeer(uuid);
	require(pair.receiver && pair.receiver->pc, "linked RTC gate did not create the manager receiver PeerConnection");
	pair.senderPeer = std::make_shared<rtc::PeerConnection>(rtc::Configuration{});

	const auto state = pair.state;
	const auto recordError = [state](const std::string &error) {
		std::lock_guard<std::mutex> lock(state->mutex);
		state->error = error;
		state->condition.notify_all();
	};
	pair.signalingRelay = LocalRtcSignalingRelay::create(pair.senderPeer, pair.receiver->pc, std::move(recordError));

	for (const auto &label : labels) {
		auto channel = pair.senderPeer->createDataChannel(label);
		require(channel != nullptr, "linked RTC gate did not create sender DataChannel " + label);
		channel->onOpen([state]() {
			std::lock_guard<std::mutex> lock(state->mutex);
			++state->openCount;
			state->condition.notify_all();
		});
		channel->onMessage([state, label](auto data) {
			if (!std::holds_alternative<std::string>(data)) {
				return;
			}
			std::lock_guard<std::mutex> lock(state->mutex);
			state->messages.emplace_back(label, std::get<std::string>(data));
			state->condition.notify_all();
		});
		pair.senderChannels.push_back(std::move(channel));
	}

	pair.senderPeer->setLocalDescription(rtc::Description::Type::Offer);
	{
		std::unique_lock<std::mutex> lock(state->mutex);
		require(
		    state->condition.wait_for(
		        lock, 30s, [&]() { return !state->error.empty() || state->openCount == pair.senderChannels.size(); }),
		    "local RTC DataChannel pair did not open every sender channel");
		require(state->error.empty(), state->error);
	}
	if (senderOpenReadyProbe) {
		senderOpenReadyProbe();
	}
	std::string receiverCommitError;
	const bool receiverCommitted = waitUntil(
	    [&]() {
		    {
			    std::lock_guard<std::mutex> lock(state->mutex);
			    if (!state->error.empty()) {
				    receiverCommitError = state->error;
				    return true;
			    }
		    }
		    std::shared_ptr<rtc::DataChannel> currentDataChannel;
		    {
			    std::lock_guard<std::mutex> mediaLock(pair.receiver->mediaMutex);
			    currentDataChannel = pair.receiver->dataChannel;
		    }
		    if (!currentDataChannel || !currentDataChannel->isOpen()) {
			    return false;
		    }
		    std::lock_guard<std::mutex> mediaLock(pair.receiver->mediaMutex);
		    return pair.receiver->dataChannel == currentDataChannel && pair.receiver->hasDataChannel &&
		           pair.receiver->dataChannelOpenDispatched;
	    },
	    10s);
	require(receiverCommitError.empty(), receiverCommitError);
	require(receiverCommitted, "local RTC DataChannel receiver did not commit its open control channel");
	return pair;
}

void testManagerDataChannelPairHelperWaitsForReceiverOpenCommit()
{
	VDONinjaPeerManager manager;
	DataChannelStageLatch lifecycleLatch(NativeMediaTestDataChannelStage::BeforeCallbacksInstalled);
	manager.setNativeMediaTestDataChannelLifecycleHook(
	    [&lifecycleLatch](NativeMediaTestDataChannelStage stage, const std::shared_ptr<PeerInfo> &,
	                      const std::shared_ptr<rtc::DataChannel> &channel,
	                      uint64_t) { lifecycleLatch.hook(stage, channel); });

	std::mutex progressMutex;
	std::condition_variable progressCondition;
	bool senderOpenWaitCompleted = false;
	bool helperReturned = false;
	std::optional<ConnectedRtcDataChannelPair> pair;
	std::exception_ptr helperError;
	std::thread helper([&]() {
		try {
			pair.emplace(
			    makeConnectedManagerDataChannelPair(manager, "dc-helper-receiver-commit", {"sendChannel"}, [&]() {
				    std::lock_guard<std::mutex> lock(progressMutex);
				    senderOpenWaitCompleted = true;
				    progressCondition.notify_all();
			    }));
		} catch (...) {
			helperError = std::current_exception();
		}
		{
			std::lock_guard<std::mutex> lock(progressMutex);
			helperReturned = true;
			progressCondition.notify_all();
		}
	});

	bool senderReadyObserved = false;
	bool returnedWhileLifecycleBlocked = false;
	try {
		lifecycleLatch.waitUntilEntered();
		std::unique_lock<std::mutex> lock(progressMutex);
		senderReadyObserved = progressCondition.wait_for(lock, 10s, [&]() { return senderOpenWaitCompleted; });
		if (senderReadyObserved) {
			returnedWhileLifecycleBlocked = progressCondition.wait_for(lock, 100ms, [&]() { return helperReturned; });
		}
	} catch (...) {
		lifecycleLatch.release();
		helper.join();
		manager.setNativeMediaTestDataChannelLifecycleHook(nullptr);
		throw;
	}
	lifecycleLatch.release();
	helper.join();
	manager.setNativeMediaTestDataChannelLifecycleHook(nullptr);

	if (helperError) {
		std::rethrow_exception(helperError);
	}
	require(senderReadyObserved, "manager DataChannel helper did not complete its sender-open wait");
	require(!returnedWhileLifecycleBlocked,
	        "manager DataChannel helper returned before the receiver open lifecycle committed");
	require(pair.has_value() && pair->receiver,
	        "manager DataChannel helper did not return its connected receiver peer");
	std::shared_ptr<rtc::DataChannel> currentDataChannel;
	{
		std::lock_guard<std::mutex> mediaLock(pair->receiver->mediaMutex);
		currentDataChannel = pair->receiver->dataChannel;
	}
	const bool receiverOpen = currentDataChannel && currentDataChannel->isOpen();
	bool exactReceiverCommit = false;
	{
		std::lock_guard<std::mutex> mediaLock(pair->receiver->mediaMutex);
		exactReceiverCommit = pair->receiver->dataChannel == currentDataChannel && pair->receiver->hasDataChannel &&
		                      pair->receiver->dataChannelOpenDispatched;
	}
	require(receiverOpen && exactReceiverCommit,
	        "manager DataChannel helper returned without the exact current receiver open commit");
}

ConnectedRtcTrackPair makeConnectedVp9TrackPair(const std::string &mid)
{
	ConnectedRtcTrackPair pair;
	pair.state = std::make_shared<ConnectedRtcTrackPair::State>();
	pair.senderPeer = std::make_shared<rtc::PeerConnection>(rtc::Configuration{});
	pair.receiverPeer = std::make_shared<rtc::PeerConnection>(rtc::Configuration{});
	const auto state = pair.state;
	const auto recordError = [state](const std::string &error) {
		std::lock_guard<std::mutex> lock(state->mutex);
		state->error = error;
		state->condition.notify_all();
	};
	pair.signalingRelay = LocalRtcSignalingRelay::create(pair.senderPeer, pair.receiverPeer, recordError);
	pair.receiverPeer->onTrack([state](std::shared_ptr<rtc::Track> track) {
		std::lock_guard<std::mutex> lock(state->mutex);
		state->receiver = std::move(track);
		state->condition.notify_all();
	});
	pair.senderPeer->onStateChange([state](rtc::PeerConnection::State connectionState) {
		std::lock_guard<std::mutex> lock(state->mutex);
		if (connectionState == rtc::PeerConnection::State::Connected) {
			state->senderConnected = true;
		} else if (connectionState == rtc::PeerConnection::State::Failed) {
			state->error = "local RTC sender connection failed";
		}
		state->condition.notify_all();
	});

	rtc::Description::Video description(mid, rtc::Description::Direction::SendOnly);
	description.addVP9Codec(98);
	pair.sender = pair.senderPeer->addTrack(description);
	require(pair.sender != nullptr, "linked RTC gate did not create the local VP9 sender Track");
	pair.sender->onOpen([state]() {
		std::lock_guard<std::mutex> lock(state->mutex);
		state->senderTrackOpen = true;
		state->condition.notify_all();
	});
	pair.senderPeer->setLocalDescription(rtc::Description::Type::Offer);
	{
		std::unique_lock<std::mutex> lock(state->mutex);
		require(state->condition.wait_for(lock, 30s,
		                                  [&]() {
			                                  return !state->error.empty() ||
			                                         (state->receiver && state->senderConnected &&
			                                          state->senderTrackOpen);
		                                  }),
		        "local RTC pair did not connect and expose its receiver Track");
		require(state->error.empty(), state->error);
		pair.receiver = state->receiver;
	}
	require(pair.receiver != nullptr && pair.sender->isOpen(), "local RTC pair Track was not open after connection");
	return pair;
}

rtc::binary makeVp9RtpProbe(uint32_t timestamp)
{
	rtc::binary packet(14, std::byte{0});
	packet[0] = std::byte{0x80};
	packet[1] = std::byte{0xE2}; // marker + VP9 payload type 98
	packet[2] = std::byte{0x00};
	packet[3] = std::byte{0x01};
	packet[4] = static_cast<std::byte>(timestamp >> 24);
	packet[5] = static_cast<std::byte>(timestamp >> 16);
	packet[6] = static_cast<std::byte>(timestamp >> 8);
	packet[7] = static_cast<std::byte>(timestamp);
	packet[8] = std::byte{0x01};
	packet[9] = std::byte{0x02};
	packet[10] = std::byte{0x03};
	packet[11] = std::byte{0x04};
	packet[12] = std::byte{0x0C}; // VP9 B=1, E=1
	packet[13] = std::byte{0x01};
	return packet;
}

uint32_t feedNextGopFrame(VDONinjaSource &source, bool alpha, const std::vector<std::vector<uint8_t>> &gop,
                          size_t &cursor, uint32_t baseTimestamp)
{
	require(cursor < gop.size(), "linked gate exhausted its VP9 GOP while pumping frame-threaded decode");
	const uint32_t timestamp = baseTimestamp + static_cast<uint32_t>(cursor) * 3000u;
	source.feedNativeMediaTestVp9AccessUnit(alpha, gop[cursor], timestamp);
	++cursor;
	return timestamp;
}

void feedUntilOutputCount(VDONinjaSource &source, OutputCollector &output, const std::vector<std::vector<uint8_t>> &gop,
                          size_t &cursor, uint32_t baseTimestamp, size_t targetCount)
{
	while (output.size() < targetCount && cursor < gop.size()) {
		feedNextGopFrame(source, false, gop, cursor, baseTimestamp);
	}
	require(output.size() >= targetCount, "frame-threaded decoder did not reach the requested output count");
}

void requirePipelineEmpty(VDONinjaSource &source, const std::string &context)
{
	const NativeMediaTestSnapshot snapshot = source.nativeMediaTestSnapshot();
	require(!snapshot.primaryAssemblyActive && !snapshot.alphaAssemblyActive, context + ": assembly stayed active");
	require(snapshot.primaryAssemblyBytes == 0 && snapshot.alphaAssemblyBytes == 0,
	        context + ": assembly bytes survived transition");
	require(!snapshot.primaryDecoderAllocated && !snapshot.alphaDecoderAllocated,
	        context + ": decoder survived transition");
	require(snapshot.pendingPrimaryFrames == 0 && snapshot.pendingAlphaFrames == 0,
	        context + ": exact-pair queue survived transition");
	require(snapshot.retainedVideoFrames == 0, context + ": cloned AVFrame outlived stale callback");
}

void testRtcManagerSourceRejectsLateAlphaAfterInactiveRemoval()
{
	VDONinjaSource source(NativeMediaTestTag{});
	VDONinjaPeerManager manager;
	source.bindNativeMediaTestPeerManager(manager);
	auto peer = manager.createNativeMediaTestViewerPeer("rtc-peer", "session-a");
	auto primary = addVp9ReceiveTrack(peer, "video");
	auto alpha = addVp9ReceiveTrack(peer, "video-alpha");
	manager.receiveNativeMediaTestTrack(peer, primary);
	require(source.nativeMediaTestTrackSnapshot().video == primary.get(),
	        "production manager/source callback did not attach the real primary RTC track");

	TrackCommitLatch latch(alpha.get());
	manager.setNativeMediaTestTrackCommitHook([&latch](const std::string &, TrackType,
	                                                   const std::shared_ptr<rtc::Track> &track,
	                                                   uint64_t) { latch.hook(track); });
	std::thread lateAdd([&]() { manager.receiveNativeMediaTestTrack(peer, alpha); });
	latch.waitUntilEntered();
	manager.prepareNativeMediaTestViewerTracks(peer, "v=0\r\n");
	{
		std::lock_guard<std::mutex> mediaLock(peer->mediaMutex);
		require(!peer->alphaVideoTrack, "actual inactive-alpha preparation did not clear the manager slot");
	}
	latch.release();
	lateAdd.join();
	manager.setNativeMediaTestTrackCommitHook(nullptr);
	require(source.nativeMediaTestTrackSnapshot().alpha == nullptr,
	        "late manager add resurrected alpha after the newer inactive-alpha removal reached the source");
	require(source.nativeMediaTestRejectedTrackEventCount() == 0,
	        "manager dispatched an add after its exact alpha slot/revision lease had already retired");
}

void testRtcManagerSourceReplacementOrdering()
{
	for (const bool alphaSlot : {false, true}) {
		VDONinjaSource source(NativeMediaTestTag{});
		VDONinjaPeerManager manager;
		source.bindNativeMediaTestPeerManager(manager);
		auto peer = manager.createNativeMediaTestViewerPeer(alphaSlot ? "replace-alpha" : "replace-video");
		if (alphaSlot) {
			auto primary = addVp9ReceiveTrack(peer, "video");
			manager.receiveNativeMediaTestTrack(peer, primary);
		}
		auto older = makeVp9ReceiveTrack(alphaSlot ? "video-alpha" : "video");
		auto newer = makeVp9ReceiveTrack(alphaSlot ? "video-alpha" : "video");
		TrackCommitLatch latch(older.track.get());
		manager.setNativeMediaTestTrackCommitHook([&latch](const std::string &, TrackType,
		                                                   const std::shared_ptr<rtc::Track> &track,
		                                                   uint64_t) { latch.hook(track); });
		std::thread lateOlder([&]() { manager.receiveNativeMediaTestTrack(peer, older.track); });
		latch.waitUntilEntered();
		manager.receiveNativeMediaTestTrack(peer, newer.track);
		latch.release();
		lateOlder.join();
		manager.setNativeMediaTestTrackCommitHook(nullptr);

		const auto sourceTracks = source.nativeMediaTestTrackSnapshot();
		std::lock_guard<std::mutex> mediaLock(peer->mediaMutex);
		if (alphaSlot) {
			require(peer->alphaVideoTrack == newer.track && sourceTracks.alpha == newer.track.get(),
			        "late alpha replacement did not converge manager/source on the newest slot identity");
		} else {
			require(peer->videoTrack == newer.track && sourceTracks.video == newer.track.get(),
			        "late primary replacement did not converge manager/source on the newest slot identity");
		}
		require(source.nativeMediaTestRejectedTrackEventCount() == 0,
		        "manager dispatched a replaced RTC add after its exact slot/revision lease was stale");
	}
}

void testRtcStaleAddCannotClearReaddedIdenticalTrackCallbacks()
{
	for (const bool alphaSlot : {false, true}) {
		VDONinjaSource source(NativeMediaTestTag{});
		VDONinjaPeerManager manager;
		source.bindNativeMediaTestPeerManager(manager);
		auto peer = manager.createNativeMediaTestViewerPeer(alphaSlot ? "same-alpha" : "same-video");
		if (alphaSlot) {
			auto primary = addVp9ReceiveTrack(peer, "video");
			manager.receiveNativeMediaTestTrack(peer, primary);
		}
		auto reused = makeConnectedVp9TrackPair(alphaSlot ? "video-alpha" : "video");
		auto replacement = makeVp9ReceiveTrack(alphaSlot ? "video-alpha" : "video");
		TrackCommitLatch latch(reused.receiver.get());
		manager.setNativeMediaTestTrackCommitHook([&latch](const std::string &, TrackType,
		                                                   const std::shared_ptr<rtc::Track> &track,
		                                                   uint64_t) { latch.hook(track); });
		std::thread staleAdd([&]() { manager.receiveNativeMediaTestTrack(peer, reused.receiver); });
		latch.waitUntilEntered();
		manager.receiveNativeMediaTestTrack(peer, replacement.track);
		manager.receiveNativeMediaTestTrack(peer, reused.receiver);
		latch.release();
		staleAdd.join();
		manager.setNativeMediaTestTrackCommitHook(nullptr);

		StageSignal messageDelivered(NativeMediaTestStage::PreAssembly);
		source.setNativeMediaTestStageHook(
		    [&messageDelivered, alphaSlot](NativeMediaTestStage stage, bool alpha, uint32_t, uint64_t) {
			    if (alpha == alphaSlot) {
				    messageDelivered.hook(stage);
			    }
		    });
		require(reused.sender->send(makeVp9RtpProbe(alphaSlot ? 91000u : 90000u)),
		        "current re-added RTC sender rejected the RTP probe");
		messageDelivered.waitUntilReached();
		source.setNativeMediaTestStageHook(nullptr);

		reused.receiver->close();
		const auto sourceTracks = source.nativeMediaTestTrackSnapshot();
		require(alphaSlot ? sourceTracks.alpha == nullptr : sourceTracks.video == nullptr,
		        "late stale add cleared the current identical RTC track onClosed callback");
	}
}

void testRtcManagerSourceTerminalRetirementOrdering()
{
	for (const bool alphaSlot : {false, true}) {
		VDONinjaSource source(NativeMediaTestTag{});
		VDONinjaPeerManager manager;
		source.bindNativeMediaTestPeerManager(manager);
		auto peer = manager.createNativeMediaTestViewerPeer(alphaSlot ? "terminal-alpha" : "terminal-video");
		if (alphaSlot) {
			auto primary = addVp9ReceiveTrack(peer, "video");
			manager.receiveNativeMediaTestTrack(peer, primary);
		}
		auto retired = makeVp9ReceiveTrack(alphaSlot ? "video-alpha" : "video");
		TrackCommitLatch latch(retired.track.get());
		manager.setNativeMediaTestTrackCommitHook([&latch](const std::string &, TrackType,
		                                                   const std::shared_ptr<rtc::Track> &track,
		                                                   uint64_t) { latch.hook(track); });
		std::thread lateAdd([&]() { manager.receiveNativeMediaTestTrack(peer, retired.track); });
		latch.waitUntilEntered();
		manager.retireNativeMediaTestPeer(peer);
		latch.release();
		lateAdd.join();
		manager.setNativeMediaTestTrackCommitHook(nullptr);

		const auto sourceTracks = source.nativeMediaTestTrackSnapshot();
		std::lock_guard<std::mutex> mediaLock(peer->mediaMutex);
		require(!peer->videoTrack && !peer->alphaVideoTrack, "terminal retirement left a manager video slot populated");
		require(sourceTracks.video == nullptr && sourceTracks.alpha == nullptr,
		        "terminal retirement allowed a late RTC track to reattach at the source");
		require(source.nativeMediaTestRejectedTrackEventCount() == 0,
		        "manager dispatched a terminal stale RTC add after its peer/slot lease was stale");
	}
}

void testRtcManagerSourceSameUuidSessionRotationOrdering()
{
	for (const bool alphaSlot : {false, true}) {
		VDONinjaSource source(NativeMediaTestTag{});
		VDONinjaPeerManager manager;
		source.bindNativeMediaTestPeerManager(manager);
		const std::string uuid = alphaSlot ? "rotate-alpha" : "rotate-video";
		auto oldPeer = manager.createNativeMediaTestViewerPeer(uuid, "session-a");
		if (alphaSlot) {
			auto oldPrimary = addVp9ReceiveTrack(oldPeer, "video");
			manager.receiveNativeMediaTestTrack(oldPeer, oldPrimary);
		}
		auto stale = makeVp9ReceiveTrack(alphaSlot ? "video-alpha" : "video");
		TrackCommitLatch latch(stale.track.get());
		manager.setNativeMediaTestTrackCommitHook([&latch](const std::string &, TrackType,
		                                                   const std::shared_ptr<rtc::Track> &track,
		                                                   uint64_t) { latch.hook(track); });
		std::thread oldSessionAdd([&]() { manager.receiveNativeMediaTestTrack(oldPeer, stale.track); });
		latch.waitUntilEntered();
		manager.retireNativeMediaTestPeer(oldPeer);
		auto newPeer = manager.createNativeMediaTestViewerPeer(uuid, "session-b");
		auto newPrimary = addVp9ReceiveTrack(newPeer, "video");
		manager.receiveNativeMediaTestTrack(newPeer, newPrimary);
		std::shared_ptr<rtc::Track> newest = newPrimary;
		if (alphaSlot) {
			newest = addVp9ReceiveTrack(newPeer, "video-alpha");
			manager.receiveNativeMediaTestTrack(newPeer, newest);
		}
		latch.release();
		oldSessionAdd.join();
		manager.setNativeMediaTestTrackCommitHook(nullptr);

		const auto sourceTracks = source.nativeMediaTestTrackSnapshot();
		std::lock_guard<std::mutex> mediaLock(newPeer->mediaMutex);
		require(newPeer->videoTrack == newPrimary && sourceTracks.video == newPrimary.get(),
		        "same-UUID session rotation did not preserve the new primary identity");
		if (alphaSlot) {
			require(newPeer->alphaVideoTrack == newest && sourceTracks.alpha == newest.get(),
			        "same-UUID session rotation did not preserve the new alpha identity");
		} else {
			require(sourceTracks.alpha == nullptr,
			        "same-UUID primary-only session rotation unexpectedly retained alpha");
		}
		require(source.nativeMediaTestRejectedTrackEventCount() == 0,
		        "manager dispatched an old same-UUID session track after its peer lease was stale");
	}
}

void testManagerOwnedTrackClosePreservesWholePeerOwnership()
{
	for (const bool withAlpha : {false, true}) {
		VDONinjaSource source(NativeMediaTestTag{});
		VDONinjaPeerManager manager;
		source.bindNativeMediaTestPeerManager(manager);
		const std::string suffix = withAlpha ? "alpha" : "primary";
		auto ownerA = manager.createNativeMediaTestViewerPeer("track-close-a-" + suffix, "session-a");
		auto ownerAVideo = addVp9ReceiveTrack(ownerA, "video");
		auto ownerAAudio = addOpusReceiveTrack(ownerA, "audio");
		std::shared_ptr<rtc::Track> ownerAAlpha;
		manager.receiveNativeMediaTestTrack(ownerA, ownerAVideo);
		if (withAlpha) {
			ownerAAlpha = addVp9ReceiveTrack(ownerA, "video-alpha");
			manager.receiveNativeMediaTestTrack(ownerA, ownerAAlpha);
		}
		manager.receiveNativeMediaTestTrack(ownerA, ownerAAudio);

		auto ownerB = manager.createNativeMediaTestViewerPeer("track-close-b-" + suffix, "session-b");
		auto ownerBVideo = addVp9ReceiveTrack(ownerB, "video");
		auto ownerBAudio = addOpusReceiveTrack(ownerB, "audio");
		std::shared_ptr<rtc::Track> ownerBAlpha;
		manager.receiveNativeMediaTestTrack(ownerB, ownerBVideo);
		if (withAlpha) {
			ownerBAlpha = addVp9ReceiveTrack(ownerB, "video-alpha");
			manager.receiveNativeMediaTestTrack(ownerB, ownerBAlpha);
		}
		manager.receiveNativeMediaTestTrack(ownerB, ownerBAudio);
		if (withAlpha) {
			manager.dispatchNativeMediaTestDataChannelMessage(
			    ownerB,
			    R"({"audioMuted":true,"videoMuted":true,"virtualHangup":true,"info":{"directorVideoMuted":true}})");
		}

		if (ownerAAlpha) {
			ownerAAlpha->close();
			requireEventually(
			    [&]() {
				    bool managerAlphaRetired = false;
				    {
					    std::lock_guard<std::mutex> lock(ownerA->mediaMutex);
					    managerAlphaRetired = !ownerA->alphaVideoTrack;
				    }
				    return managerAlphaRetired && source.nativeMediaTestTrackSnapshot().alpha == nullptr;
			    },
			    "real alpha Track::close did not converge the manager/source alpha slot");
			const auto tracks = source.nativeMediaTestTrackSnapshot();
			require(tracks.video == ownerAVideo.get() && tracks.audio == ownerAAudio.get(),
			        "alpha-only close disturbed owner A primary/audio ownership");
		}

		ownerAVideo->close();
		requireEventually(
		    [&]() {
			    bool managerVideoRetired = false;
			    {
				    std::lock_guard<std::mutex> lock(ownerA->mediaMutex);
				    managerVideoRetired = !ownerA->videoTrack;
			    }
			    const auto tracks = source.nativeMediaTestTrackSnapshot();
			    return managerVideoRetired && tracks.video == nullptr && tracks.audio == ownerAAudio.get();
		    },
		    "video-only Track::close did not retire exactly at the manager or mixed deferred B video with owner A "
		    "audio");
		auto snapshot = source.nativeMediaTestSnapshot();
		require(!snapshot.videoSuppressed && snapshot.sourceAudioActive,
		        "stored owner B control state leaked while owner A audio remained authoritative");

		ownerAAudio->close();
		requireEventually(
		    [&]() {
			    bool managerAudioRetired = false;
			    {
				    std::lock_guard<std::mutex> lock(ownerA->mediaMutex);
				    managerAudioRetired = !ownerA->audioTrack;
			    }
			    const auto tracks = source.nativeMediaTestTrackSnapshot();
			    return managerAudioRetired && tracks.video == ownerBVideo.get() && tracks.audio == ownerBAudio.get() &&
			           tracks.alpha == ownerBAlpha.get();
		    },
		    "final owner A audio Track::close did not atomically adopt owner B primary/alpha/audio slots");

		const auto tracks = source.nativeMediaTestTrackSnapshot();
		snapshot = source.nativeMediaTestSnapshot();
		require(tracks.video == ownerBVideo.get() && tracks.alpha == ownerBAlpha.get() &&
		            tracks.audio == ownerBAudio.get(),
		        "manager/source slots diverged after whole-peer Track::close adoption");
		require(withAlpha ? (snapshot.audioMuted && !snapshot.sourceAudioActive && snapshot.mediaVideoMuted &&
		                     snapshot.directorVideoMuted && snapshot.virtualHangup && snapshot.videoSuppressed)
		                  : (!snapshot.audioMuted && snapshot.sourceAudioActive && !snapshot.mediaVideoMuted &&
		                     !snapshot.directorVideoMuted && !snapshot.virtualHangup && !snapshot.videoSuppressed),
		        "Track::close adoption did not publish owner B stored/default control state");
	}
}

void testRenegotiationRecreatesClosedManagerOwnedTracks()
{
	VDONinjaSource source(NativeMediaTestTag{});
	VDONinjaPeerManager manager;
	source.bindNativeMediaTestPeerManager(manager);
	auto peer = manager.createNativeMediaTestViewerPeer("renegotiated-track-close", "session-a");
	const std::string offer = "v=0\r\n"
	                          "m=video 9 UDP/TLS/RTP/SAVPF 98\r\n"
	                          "a=mid:video\r\n"
	                          "a=sendonly\r\n"
	                          "a=rtpmap:98 VP9/90000\r\n"
	                          "m=audio 9 UDP/TLS/RTP/SAVPF 111\r\n"
	                          "a=mid:audio\r\n"
	                          "a=sendonly\r\n"
	                          "a=rtpmap:111 opus/48000/2\r\n";
	manager.prepareNativeMediaTestViewerTracks(peer, offer);
	std::shared_ptr<rtc::Track> firstVideo;
	std::shared_ptr<rtc::Track> firstAudio;
	{
		std::lock_guard<std::mutex> lock(peer->mediaMutex);
		firstVideo = peer->videoTrack;
		firstAudio = peer->audioTrack;
	}
	require(firstVideo && firstAudio, "initial renegotiation did not prepare both receiver tracks");
	firstVideo->close();
	firstAudio->close();
	requireEventually(
	    [&]() {
		    std::lock_guard<std::mutex> lock(peer->mediaMutex);
		    return !peer->videoTrack && !peer->audioTrack;
	    },
	    "closed receiver tracks remained installed in manager slots before renegotiation");

	manager.prepareNativeMediaTestViewerTracks(peer, offer);
	std::shared_ptr<rtc::Track> replacementVideo;
	std::shared_ptr<rtc::Track> replacementAudio;
	{
		std::lock_guard<std::mutex> lock(peer->mediaMutex);
		replacementVideo = peer->videoTrack;
		replacementAudio = peer->audioTrack;
	}
	const auto tracks = source.nativeMediaTestTrackSnapshot();
	require(replacementVideo && replacementAudio && replacementVideo != firstVideo && replacementAudio != firstAudio &&
	            !replacementVideo->isClosed() && !replacementAudio->isClosed(),
	        "renegotiation reused a closed manager track instead of creating a replacement");
	require(tracks.video == replacementVideo.get() && tracks.audio == replacementAudio.get(),
	        "source did not converge on renegotiated replacement tracks");
}

void testTrackErrorAndCloseRetireExactlyOnceAndRenegotiate()
{
	VDONinjaSource source(NativeMediaTestTag{});
	VDONinjaPeerManager manager;
	source.bindNativeMediaTestPeerManager(manager);
	auto peer = manager.createNativeMediaTestViewerPeer("track-error-exact-once", "session-a");
	const std::string offer = "v=0\r\n"
	                          "m=video 9 UDP/TLS/RTP/SAVPF 98\r\n"
	                          "a=mid:video\r\n"
	                          "a=sendonly\r\n"
	                          "a=rtpmap:98 VP9/90000\r\n";
	manager.prepareNativeMediaTestViewerTracks(peer, offer);
	std::shared_ptr<rtc::Track> failedTrack;
	{
		std::lock_guard<std::mutex> lock(peer->mediaMutex);
		failedTrack = peer->videoTrack;
	}
	require(failedTrack && source.nativeMediaTestTrackSnapshot().video == failedTrack.get(),
	        "error lifecycle gate did not install the initial manager/source track");

	std::atomic<int> retirementCount{0};
	manager.setNativeMediaTestTrackDispatchHook([&](const TrackSlotEvent &event, bool) {
		if (!event.track && event.retiredTrack == failedTrack) {
			retirementCount.fetch_add(1, std::memory_order_acq_rel);
		}
	});
	manager.dispatchNativeMediaTestTrackError(peer, TrackType::Video);
	requireEventually(
	    [&]() {
		    bool managerRetired = false;
		    {
			    std::lock_guard<std::mutex> lock(peer->mediaMutex);
			    managerRetired = !peer->videoTrack;
		    }
		    return managerRetired && source.nativeMediaTestTrackSnapshot().video == nullptr;
	    },
	    "Track::onError retirement did not converge manager/source slots");
	require(failedTrack->isClosed(), "terminal Track::onError left a reusable non-closed libdatachannel Track");
	failedTrack->close();
	require(retirementCount.load(std::memory_order_acquire) == 1,
	        "Track error followed by close emitted more than one exact retirement");

	manager.prepareNativeMediaTestViewerTracks(peer, offer);
	std::shared_ptr<rtc::Track> replacement;
	{
		std::lock_guard<std::mutex> lock(peer->mediaMutex);
		replacement = peer->videoTrack;
	}
	require(replacement && replacement != failedTrack && !replacement->isClosed() &&
	            source.nativeMediaTestTrackSnapshot().video == replacement.get(),
	        "renegotiation reused the errored Track instead of creating an open replacement");
	manager.setNativeMediaTestTrackDispatchHook(nullptr);
}

void testStaleTrackTerminalLeaseCannotClearReaddedHandle()
{
	VDONinjaSource source(NativeMediaTestTag{});
	VDONinjaPeerManager manager;
	source.bindNativeMediaTestPeerManager(manager);
	auto peer = manager.createNativeMediaTestViewerPeer("stale-track-terminal-lease", "session-a");
	auto sameHandle = addVp9ReceiveTrack(peer, "video");
	auto replacement = makeVp9ReceiveTrack("video");
	manager.receiveNativeMediaTestTrack(peer, sameHandle);

	TrackCommitLatch terminalLatch(sameHandle.get());
	manager.setNativeMediaTestTrackLifecycleHook([&terminalLatch](const std::shared_ptr<PeerInfo> &, TrackType,
	                                                              const std::shared_ptr<rtc::Track> &track,
	                                                              uint64_t) { terminalLatch.hook(track); });
	std::thread delayedError([&]() { manager.dispatchNativeMediaTestTrackError(peer, TrackType::Video); });
	terminalLatch.waitUntilEntered();
	manager.receiveNativeMediaTestTrack(peer, replacement.track);
	manager.receiveNativeMediaTestTrack(peer, sameHandle);
	terminalLatch.release();
	delayedError.join();
	manager.setNativeMediaTestTrackLifecycleHook(nullptr);

	{
		std::lock_guard<std::mutex> lock(peer->mediaMutex);
		require(peer->videoTrack == sameHandle, "stale terminal callback cleared a re-added identical manager handle");
	}
	require(!sameHandle->isClosed() && source.nativeMediaTestTrackSnapshot().video == sameHandle.get(),
	        "stale terminal callback closed or detached a re-added identical source handle");

	sameHandle->close();
	requireEventually(
	    [&]() {
		    bool managerRetired = false;
		    {
			    std::lock_guard<std::mutex> lock(peer->mediaMutex);
			    managerRetired = !peer->videoTrack;
		    }
		    return managerRetired && source.nativeMediaTestTrackSnapshot().video == nullptr;
	    },
	    "re-added identical Track lost its fresh manager-owned close callback");
}

void testTrackCloseDuringLifecycleInstallSuppressesAdd()
{
	VDONinjaSource source(NativeMediaTestTag{});
	VDONinjaPeerManager manager;
	source.bindNativeMediaTestPeerManager(manager);
	auto peer = manager.createNativeMediaTestViewerPeer("close-during-track-install", "session-a");
	auto track = addVp9ReceiveTrack(peer, "video");
	TrackCommitLatch installLatch(track.get());
	manager.setNativeMediaTestTrackCommitHook([&installLatch](const std::string &, TrackType,
	                                                          const std::shared_ptr<rtc::Track> &committed,
	                                                          uint64_t) { installLatch.hook(committed); });
	std::thread install([&]() { manager.receiveNativeMediaTestTrack(peer, track); });
	installLatch.waitUntilEntered();
	track->close();
	installLatch.release();
	install.join();
	manager.setNativeMediaTestTrackCommitHook(nullptr);

	bool managerRetired = false;
	{
		std::lock_guard<std::mutex> lock(peer->mediaMutex);
		managerRetired = !peer->videoTrack;
	}
	require(managerRetired && source.nativeMediaTestTrackSnapshot().video == nullptr,
	        "Track closed during lifecycle install leaked a manager/source add");
}

void testStalePreInstallTrackCallbacksAreCleaned()
{
	VDONinjaSource source(NativeMediaTestTag{});
	VDONinjaPeerManager manager;
	source.bindNativeMediaTestPeerManager(manager);
	auto peer = manager.createNativeMediaTestViewerPeer("stale-pre-install-track", "session-a");
	auto stale = addVp9ReceiveTrack(peer, "video");
	auto current = makeVp9ReceiveTrack("video");
	TrackCommitLatch installLatch(stale.get());
	manager.setNativeMediaTestTrackBeforeInstallHook(
	    [&installLatch](const std::shared_ptr<rtc::Track> &track) { installLatch.hook(track); });
	std::thread staleInstall([&]() { manager.receiveNativeMediaTestTrack(peer, stale); });
	installLatch.waitUntilEntered();
	manager.receiveNativeMediaTestTrack(peer, current.track);
	installLatch.release();
	staleInstall.join();
	manager.setNativeMediaTestTrackBeforeInstallHook(nullptr);

	std::atomic<int> staleRetirements{0};
	std::atomic<int> currentRetirements{0};
	std::atomic<int> staleCallbackEntries{0};
	manager.setNativeMediaTestTrackDispatchHook([&](const TrackSlotEvent &event, bool) {
		if (!event.track && event.retiredTrack == stale) {
			staleRetirements.fetch_add(1, std::memory_order_acq_rel);
		}
		if (!event.track && event.retiredTrack == current.track) {
			currentRetirements.fetch_add(1, std::memory_order_acq_rel);
		}
	});
	manager.setNativeMediaTestTrackLifecycleHook(
	    [&](const std::shared_ptr<PeerInfo> &, TrackType, const std::shared_ptr<rtc::Track> &track, uint64_t) {
		    if (track == stale) {
			    staleCallbackEntries.fetch_add(1, std::memory_order_acq_rel);
		    }
	    });
	stale->close();
	manager.setNativeMediaTestTrackLifecycleHook(nullptr);
	{
		std::lock_guard<std::mutex> lock(peer->mediaMutex);
		require(peer->videoTrack == current.track,
		        "closing a stale pre-install handle cleared the current manager slot");
	}
	require(source.nativeMediaTestTrackSnapshot().video == current.track.get() &&
	            staleCallbackEntries.load(std::memory_order_acquire) == 0 &&
	            staleRetirements.load(std::memory_order_acquire) == 0,
	        "stale pre-install callbacks survived manager suppression and acted on the source");

	current.track->close();
	requireEventually(
	    [&]() {
		    std::lock_guard<std::mutex> lock(peer->mediaMutex);
		    return !peer->videoTrack;
	    },
	    "current replacement lost its manager-owned lifecycle callback after stale cleanup");
	require(currentRetirements.load(std::memory_order_acquire) == 1 &&
	            source.nativeMediaTestTrackSnapshot().video == nullptr,
	        "current replacement did not retire exactly once after stale pre-install cleanup");
	manager.setNativeMediaTestTrackDispatchHook(nullptr);
}

void testTrackCallbackCleanupNeverHoldsManagerLocks()
{
	VDONinjaSource source(NativeMediaTestTag{});
	VDONinjaPeerManager manager;
	source.bindNativeMediaTestPeerManager(manager);
	auto peer = manager.createNativeMediaTestViewerPeer("track-cleanup-lock-contract", "session-a");
	auto retiring = addVp9ReceiveTrack(peer, "video");
	auto current = makeVp9ReceiveTrack("video");
	manager.receiveNativeMediaTestTrack(peer, retiring);

	TrackCommitLatch terminalLatch(retiring.get());
	TrackCommitLatch cleanupLatch(retiring.get());
	std::atomic<bool> registryLockAvailable{false};
	std::atomic<bool> mediaLockAvailable{false};
	manager.setNativeMediaTestTrackLifecycleHook([&terminalLatch](const std::shared_ptr<PeerInfo> &, TrackType,
	                                                              const std::shared_ptr<rtc::Track> &track,
	                                                              uint64_t) { terminalLatch.hook(track); });
	manager.setNativeMediaTestTrackCleanupHook([&](const std::shared_ptr<rtc::Track> &track) {
		if (track != retiring) {
			return;
		}
		registryLockAvailable.store(manager.nativeMediaTestPeerRegistryLockAvailable(), std::memory_order_release);
		const bool mediaAvailable = peer->mediaMutex.try_lock();
		if (mediaAvailable) {
			peer->mediaMutex.unlock();
		}
		mediaLockAvailable.store(mediaAvailable, std::memory_order_release);
		cleanupLatch.hook(track);
	});

	std::thread terminal([&]() { retiring->close(); });
	terminalLatch.waitUntilEntered();
	std::thread replacement([&]() { manager.receiveNativeMediaTestTrack(peer, current.track); });
	cleanupLatch.waitUntilEntered();
	require(registryLockAvailable.load(std::memory_order_acquire) && mediaLockAvailable.load(std::memory_order_acquire),
	        "Track callback cleanup attempted a synchronized RTC setter while holding manager locks");
	terminalLatch.release();
	terminal.join();
	cleanupLatch.release();
	replacement.join();
	manager.setNativeMediaTestTrackLifecycleHook(nullptr);
	manager.setNativeMediaTestTrackCleanupHook(nullptr);

	{
		std::lock_guard<std::mutex> lock(peer->mediaMutex);
		require(peer->videoTrack == current.track, "terminal/cleanup race cleared the replacement manager Track");
	}
	require(source.nativeMediaTestTrackSnapshot().video == current.track.get(),
	        "terminal/cleanup race detached the replacement source Track");
	current.track->close();
	requireEventually(
	    [&]() {
		    std::lock_guard<std::mutex> lock(peer->mediaMutex);
		    return !peer->videoTrack;
	    },
	    "replacement Track lost its lifecycle callback after cleanup lock race");
}

void testTrackInFlightCallbackIsSafeDuringManagerDestruction()
{
	auto manager = std::make_unique<VDONinjaPeerManager>();
	auto peer = manager->createNativeMediaTestViewerPeer("track-destruction", "session-a");
	auto track = addVp9ReceiveTrack(peer, "video");
	manager->receiveNativeMediaTestTrack(peer, track);

	OwnerSessionProbe probe;
	probe.block(NativeMediaTestOwnerSessionStage::PermitAcquired, PeerManagerCompletionKind::TrackClosed, track.get());
	OwnerSessionHookGuard ownerSessionHookGuard(manager, probe);
	manager->setNativeMediaTestOwnerSessionHook([&probe](NativeMediaTestOwnerSessionStage stage,
	                                                     PeerManagerCompletionKind kind,
	                                                     const void *handle) { probe.hook(stage, kind, handle); });
	OwnerSessionThreadGuard closing(probe, [&]() { track->close(); });
	const bool callbackHeld = waitUntil(
	    [&]() {
		    return probe.count(NativeMediaTestOwnerSessionStage::PermitAcquired, PeerManagerCompletionKind::TrackClosed,
		                       track.get()) == 1;
	    },
	    10s);
	if (!callbackHeld) {
		probe.releaseBlock();
		closing.join();
		manager->setNativeMediaTestOwnerSessionHook(nullptr);
		manager.reset();
		ownerSessionHookGuard.disarm();
		require(false, "Track callback did not acquire its owner-session permit before destruction");
	}

	std::atomic<bool> destructionFinished{false};
	OwnerSessionThreadGuard destroying(probe, [&]() {
		manager.reset();
		destructionFinished.store(true, std::memory_order_release);
	});
	const bool destructorReachedObservableState = waitUntil(
	    [&]() {
		    return probe.count(NativeMediaTestOwnerSessionStage::WaitingForPermits,
		                       PeerManagerCompletionKind::OwnerSession) >= 1 ||
		           destructionFinished.load(std::memory_order_acquire);
	    },
	    10s);
	const bool destructionFinishedWhileHeld = destructionFinished.load(std::memory_order_acquire);
	const size_t waitingWhileHeld =
	    probe.count(NativeMediaTestOwnerSessionStage::WaitingForPermits, PeerManagerCompletionKind::OwnerSession);
	const size_t drainedWhileHeld =
	    probe.count(NativeMediaTestOwnerSessionStage::WorkDrained, PeerManagerCompletionKind::OwnerSession);
	const size_t trackDetachedWhileHeld =
	    probe.count(NativeMediaTestOwnerSessionStage::AfterDetach, PeerManagerCompletionKind::TrackClosed, track.get());
	probe.releaseBlock();
	closing.join();
	destroying.join();
	ownerSessionHookGuard.disarm();

	require(!destructionFinishedWhileHeld,
	        "manager destruction completed while an admitted Track callback still held its owner-session permit");
	require(waitingWhileHeld == 1,
	        "manager destruction did not enter exactly one Track owner-session permit wait while work was held");
	require(drainedWhileHeld == 0,
	        "manager destruction reported Track owner-session work drained before the held callback completed");
	require(trackDetachedWhileHeld == 0,
	        "manager destruction detached the exact Track callback before its admitted work drained");
	require(destructorReachedObservableState,
	        "manager destruction exposed neither its permit wait nor completion while the Track callback was held");
	require(!manager && destructionFinished.load(std::memory_order_acquire),
	        "manager destruction did not complete after the in-flight Track callback drained");
	require(probe.count(NativeMediaTestOwnerSessionStage::WorkDrained, PeerManagerCompletionKind::OwnerSession) == 1,
	        "manager destruction did not report exactly one completed Track owner-session drain");
	require(probe.count(NativeMediaTestOwnerSessionStage::AfterDetach, PeerManagerCompletionKind::TrackClosed,
	                    track.get()) == 1,
	        "manager destruction did not detach the exact Track callback once after its admitted work drained");
	require(
	    probe.firstPrecedes(NativeMediaTestOwnerSessionStage::PermitAcquired, PeerManagerCompletionKind::TrackClosed,
	                        NativeMediaTestOwnerSessionStage::WaitingForPermits,
	                        PeerManagerCompletionKind::OwnerSession, track.get()) &&
	        probe.firstPrecedes(NativeMediaTestOwnerSessionStage::WaitingForPermits,
	                            PeerManagerCompletionKind::OwnerSession, NativeMediaTestOwnerSessionStage::WorkDrained,
	                            PeerManagerCompletionKind::OwnerSession) &&
	        probe.firstPrecedes(NativeMediaTestOwnerSessionStage::WorkDrained, PeerManagerCompletionKind::OwnerSession,
	                            NativeMediaTestOwnerSessionStage::AfterDetach, PeerManagerCompletionKind::TrackClosed,
	                            nullptr, track.get()),
	    "Track owner-session events did not order permit, wait, drain, then exact callback detachment");
}

void testCompletionDelayedUntilAfterOwnerShutdownIsRejected()
{
	auto manager = std::make_unique<VDONinjaPeerManager>();
	auto peer = manager->createNativeMediaTestPublisherPeer("completion-delayed-shutdown", "session-a");
	std::shared_ptr<rtc::Track> videoTrack;
	{
		std::lock_guard<std::mutex> mediaLock(peer->mediaMutex);
		videoTrack = peer->videoTrack;
	}
	auto feedbackCompletion = manager->nativeMediaTestVideoFeedbackCompletion(peer);
	require(videoTrack != nullptr && static_cast<bool>(feedbackCompletion),
	        "delayed completion gate did not install its real linked feedback function");
	OwnerSessionProbe probe;
	probe.block(NativeMediaTestOwnerSessionStage::BeforePermit, PeerManagerCompletionKind::VideoFeedback,
	            videoTrack.get());
	OwnerSessionHookGuard ownerSessionHookGuard(manager, probe);
	manager->setNativeMediaTestOwnerSessionHook([&probe](NativeMediaTestOwnerSessionStage stage,
	                                                     PeerManagerCompletionKind kind,
	                                                     const void *handle) { probe.hook(stage, kind, handle); });

	OwnerSessionThreadGuard completion(probe, [&]() { feedbackCompletion(); });
	probe.waitUntilBlocked();
	manager.reset();
	probe.waitUntilSeen(NativeMediaTestOwnerSessionStage::WorkDrained, PeerManagerCompletionKind::OwnerSession);
	require(probe.count(NativeMediaTestOwnerSessionStage::WaitingForPermits, PeerManagerCompletionKind::OwnerSession) ==
	            0,
	        "pre-permit completion was incorrectly admitted into the shutdown drain");
	probe.releaseBlock();
	completion.join();
	ownerSessionHookGuard.disarm();
	probe.waitUntilSeen(NativeMediaTestOwnerSessionStage::PermitRejected, PeerManagerCompletionKind::VideoFeedback, 1,
	                    videoTrack.get());
}

void testInstalledFunctionRegistrationFailureDetachesSetter()
{
	auto manager = std::make_unique<VDONinjaPeerManager>();
	OwnerSessionProbe probe;
	OwnerSessionHookGuard ownerSessionHookGuard(manager, probe);
	manager->setNativeMediaTestOwnerSessionHook([&probe](NativeMediaTestOwnerSessionStage stage,
	                                                     PeerManagerCompletionKind kind,
	                                                     const void *handle) { probe.hook(stage, kind, handle); });
	auto peer = manager->createNativeMediaTestViewerPeer("registration-failure");
	auto mediaTrack = makeVp9ReceiveTrack("video");
	const void *trackHandle = mediaTrack.track.get();
	manager->failNextNativeMediaTestOwnerSessionFunctionRegistration(PeerManagerCompletionKind::TrackClosed);
	bool registrationThrew = false;
	try {
		manager->receiveNativeMediaTestTrack(peer, mediaTrack.track);
	} catch (const std::bad_alloc &) {
		registrationThrew = true;
	}
	require(registrationThrew, "owner-session gate did not inject the requested function-registration failure");
	require(probe.count(NativeMediaTestOwnerSessionStage::AfterDetach, PeerManagerCompletionKind::TrackClosed,
	                    trackHandle) == 1,
	        "function-registration failure left its already-installed Track setter attached");
	manager.reset();
	ownerSessionHookGuard.disarm();
	require(probe.count(NativeMediaTestOwnerSessionStage::AfterDetach, PeerManagerCompletionKind::TrackClosed,
	                    trackHandle) == 1,
	        "failed Track function registration was retained for a second shutdown detach");
}

void testVideoAlphaAudioTrackCompletionsShareOwnerSession()
{
	auto manager = std::make_unique<VDONinjaPeerManager>();
	OwnerSessionProbe probe;
	OwnerSessionHookGuard ownerSessionHookGuard(manager, probe);
	manager->setNativeMediaTestOwnerSessionHook([&probe](NativeMediaTestOwnerSessionStage stage,
	                                                     PeerManagerCompletionKind kind,
	                                                     const void *handle) { probe.hook(stage, kind, handle); });
	auto peer = manager->createNativeMediaTestViewerPeer("track-kind-session", "session-a");
	auto video = addVp9ReceiveTrack(peer, "video");
	auto alpha = addVp9ReceiveTrack(peer, "video-alpha");
	auto audio = addOpusReceiveTrack(peer, "audio");
	manager->receiveNativeMediaTestTrack(peer, video);
	manager->receiveNativeMediaTestTrack(peer, alpha);
	manager->receiveNativeMediaTestTrack(peer, audio);

	video->close();
	alpha->close();
	audio->close();
	for (const auto &track : {video, alpha, audio}) {
		probe.waitUntilSeen(NativeMediaTestOwnerSessionStage::PermitAcquired, PeerManagerCompletionKind::TrackClosed, 1,
		                    track.get());
	}
	manager.reset();
	ownerSessionHookGuard.disarm();
	for (const auto &track : {video, alpha, audio}) {
		require(probe.count(NativeMediaTestOwnerSessionStage::AfterDetach, PeerManagerCompletionKind::TrackClosed,
		                    track.get()) == 1,
		        "shutdown did not detach one exact Track::onClosed function");
		require(probe.count(NativeMediaTestOwnerSessionStage::AfterDetach, PeerManagerCompletionKind::TrackError,
		                    track.get()) == 1,
		        "shutdown did not detach one exact Track::onError function");
	}
}

void testSourceResetPreservesManagerTrackLifecycleOwnership()
{
	VDONinjaSource source(NativeMediaTestTag{});
	VDONinjaPeerManager manager;
	source.bindNativeMediaTestPeerManager(manager);
	auto peer = manager.createNativeMediaTestViewerPeer("source-reset-track-lifecycle", "session-a");
	auto track = addVp9ReceiveTrack(peer, "video");
	manager.receiveNativeMediaTestTrack(peer, track);
	std::atomic<int> retirementCount{0};
	manager.setNativeMediaTestTrackDispatchHook([&](const TrackSlotEvent &event, bool) {
		if (!event.track && event.retiredTrack == track) {
			retirementCount.fetch_add(1, std::memory_order_acq_rel);
		}
	});

	source.resetNativeMediaTestState();
	{
		std::lock_guard<std::mutex> lock(peer->mediaMutex);
		require(peer->videoTrack == track, "source reset unexpectedly mutated the manager Track slot");
	}
	require(source.nativeMediaTestTrackSnapshot().video == nullptr,
	        "source reset retained its source-owned Track callback state");
	track->close();
	requireEventually(
	    [&]() {
		    std::lock_guard<std::mutex> lock(peer->mediaMutex);
		    return !peer->videoTrack;
	    },
	    "source reset cleared the manager-owned Track close callback");
	require(retirementCount.load(std::memory_order_acquire) == 1,
	        "source reset did not preserve exact-once manager Track lifecycle ownership");
	manager.setNativeMediaTestTrackDispatchHook(nullptr);
}

void testInterleavedPendingPeerBundlesNeverMix()
{
	VDONinjaSource source(NativeMediaTestTag{});
	VDONinjaPeerManager manager;
	source.bindNativeMediaTestPeerManager(manager);
	auto ownerA = manager.createNativeMediaTestViewerPeer("bundle-owner-a", "session-a");
	auto ownerAVideo = addVp9ReceiveTrack(ownerA, "video");
	auto ownerAAudio = addOpusReceiveTrack(ownerA, "audio");
	manager.receiveNativeMediaTestTrack(ownerA, ownerAVideo);
	manager.receiveNativeMediaTestTrack(ownerA, ownerAAudio);

	auto ownerB = manager.createNativeMediaTestViewerPeer("bundle-owner-b", "session-b");
	auto ownerBVideo = addVp9ReceiveTrack(ownerB, "video");
	manager.receiveNativeMediaTestTrack(ownerB, ownerBVideo);
	auto ownerC = manager.createNativeMediaTestViewerPeer("bundle-owner-c", "session-c");
	auto ownerCAudio = addOpusReceiveTrack(ownerC, "audio");
	manager.receiveNativeMediaTestTrack(ownerC, ownerCAudio);

	ownerAVideo->close();
	requireEventually(
	    [&]() {
		    const auto tracks = source.nativeMediaTestTrackSnapshot();
		    return tracks.video == nullptr && tracks.audio == ownerAAudio.get();
	    },
	    "partial owner A retirement adopted a pending B/C slot");
	ownerAAudio->close();
	requireEventually(
	    [&]() {
		    const auto tracks = source.nativeMediaTestTrackSnapshot();
		    return tracks.video == ownerBVideo.get() && tracks.audio == nullptr;
	    },
	    "oldest pending B bundle was mixed with C audio during adoption");

	auto ownerBAudio = addOpusReceiveTrack(ownerB, "audio");
	manager.receiveNativeMediaTestTrack(ownerB, ownerBAudio);
	const auto tracks = source.nativeMediaTestTrackSnapshot();
	require(tracks.video == ownerBVideo.get() && tracks.audio == ownerBAudio.get(),
	        "owner B audio could not complete its already-adopted peer bundle");
}

void testPendingPeerBundleBlocksPacketsUntilAtomicPublish()
{
	VDONinjaSource source(NativeMediaTestTag{});
	VDONinjaPeerManager manager;
	source.bindNativeMediaTestPeerManager(manager);
	auto ownerA = manager.createNativeMediaTestViewerPeer("atomic-bundle-a", "session-a");
	auto ownerAVideo = addVp9ReceiveTrack(ownerA, "video");
	auto ownerAAudio = addOpusReceiveTrack(ownerA, "audio");
	manager.receiveNativeMediaTestTrack(ownerA, ownerAVideo);
	manager.receiveNativeMediaTestTrack(ownerA, ownerAAudio);

	auto ownerB = manager.createNativeMediaTestViewerPeer("atomic-bundle-b", "session-b");
	auto ownerBVideo = makeConnectedVp9TrackPair("video");
	auto ownerBAlpha = addVp9ReceiveTrack(ownerB, "video-alpha");
	auto ownerBAudio = addOpusReceiveTrack(ownerB, "audio");
	manager.receiveNativeMediaTestTrack(ownerB, ownerBVideo.receiver);
	manager.receiveNativeMediaTestTrack(ownerB, ownerBAlpha);
	manager.receiveNativeMediaTestTrack(ownerB, ownerBAudio);
	manager.dispatchNativeMediaTestDataChannelMessage(
	    ownerB, R"({"audioMuted":true,"videoMuted":true,"virtualHangup":true,"info":{"directorVideoMuted":true}})");

	ownerAVideo->close();
	requireEventually(
	    [&]() {
		    const auto tracks = source.nativeMediaTestTrackSnapshot();
		    return tracks.video == nullptr && tracks.audio == ownerAAudio.get();
	    },
	    "atomic bundle setup did not retain owner A audio after primary retirement");

	StageLatch primaryAttached(NativeMediaTestStage::PendingBundlePrimaryAttached, false);
	StageSignal packetRejected(NativeMediaTestStage::PendingBundlePacketRejected);
	std::atomic<int> preAssemblyCount{0};
	source.setNativeMediaTestStageHook([&](NativeMediaTestStage stage, bool alpha, uint32_t timestamp, uint64_t epoch) {
		if (stage == NativeMediaTestStage::PreAssembly && !alpha) {
			preAssemblyCount.fetch_add(1, std::memory_order_acq_rel);
		}
		packetRejected.hook(stage);
		primaryAttached.hook(stage, alpha, timestamp, epoch);
	});
	std::thread finalRetirement([&]() { ownerAAudio->close(); });
	primaryAttached.waitUntilEntered();
	require(ownerBVideo.sender->send(makeVp9RtpProbe(281000u)),
	        "pending owner B sender rejected the atomic-adoption RTP probe");
	packetRejected.waitUntilReached();
	require(preAssemblyCount.load(std::memory_order_acquire) == 0,
	        "pending owner B packet entered assembly before alpha/audio/control state committed");
	const auto duringAdoption = source.nativeMediaTestSnapshot();
	require(!duringAdoption.audioMuted && !duringAdoption.videoSuppressed,
	        "pending owner B control state published before its whole media bundle committed");

	primaryAttached.release();
	finalRetirement.join();
	const auto adoptedTracks = source.nativeMediaTestTrackSnapshot();
	const auto adoptedState = source.nativeMediaTestSnapshot();
	require(adoptedTracks.video == ownerBVideo.receiver.get() && adoptedTracks.alpha == ownerBAlpha.get() &&
	            adoptedTracks.audio == ownerBAudio.get(),
	        "atomic owner B adoption did not publish all source slots together");
	require(adoptedState.audioMuted && !adoptedState.sourceAudioActive && adoptedState.videoSuppressed,
	        "atomic owner B adoption did not publish its stored control state before unblocking packets");

	StageSignal packetAccepted(NativeMediaTestStage::PreAssembly);
	source.setNativeMediaTestStageHook([&packetAccepted](NativeMediaTestStage stage, bool alpha, uint32_t, uint64_t) {
		if (!alpha) {
			packetAccepted.hook(stage);
		}
	});
	auto postCommitProbe = makeVp9RtpProbe(284000u);
	postCommitProbe[3] = std::byte{0x02};
	require(ownerBVideo.sender->send(postCommitProbe), "adopted owner B sender rejected the post-commit RTP probe");
	packetAccepted.waitUntilReached();
	source.setNativeMediaTestStageHook(nullptr);
}

void testOldGenerationDisconnectCannotClearNewGenerationMedia()
{
	VDONinjaSource source(NativeMediaTestTag{});
	VDONinjaPeerManager manager;
	source.bindNativeMediaTestPeerManager(manager);
	const std::string uuid = "lifecycle-rotate";
	auto oldPeer = manager.createNativeMediaTestViewerPeer(uuid, "session-a");
	auto oldPrimary = addVp9ReceiveTrack(oldPeer, "video");
	manager.receiveNativeMediaTestTrack(oldPeer, oldPrimary);

	PeerDispatchLatch latch(oldPeer->generation);
	manager.setNativeMediaTestPeerDisconnectDispatchHook(
	    [&latch](const std::shared_ptr<PeerInfo> &peer) { latch.hook(peer); });
	std::thread lateDisconnect([&]() { manager.dispatchNativeMediaTestPeerDisconnected(oldPeer); });
	latch.waitUntilEntered();
	manager.retireNativeMediaTestPeer(oldPeer);

	auto newPeer = manager.createNativeMediaTestViewerPeer(uuid, "session-b");
	auto newPrimary = addVp9ReceiveTrack(newPeer, "video");
	auto newAlpha = addVp9ReceiveTrack(newPeer, "video-alpha");
	auto newAudio = addOpusReceiveTrack(newPeer, "audio");
	manager.receiveNativeMediaTestTrack(newPeer, newPrimary);
	manager.receiveNativeMediaTestTrack(newPeer, newAlpha);
	manager.receiveNativeMediaTestTrack(newPeer, newAudio);
	source.applyNativeMediaTestPeerCleanup({uuid, oldPeer->session, oldPeer->generation});
	require(!manager.disconnectPeer({uuid, oldPeer->session, oldPeer->generation}),
	        "stale signaling cleanup identity disconnected the current manager generation");
	{
		const auto preReleaseTracks = source.nativeMediaTestTrackSnapshot();
		require(preReleaseTracks.video == newPrimary.get() && preReleaseTracks.alpha == newAlpha.get() &&
		            preReleaseTracks.audio == newAudio.get(),
		        "stale signaling cleanup identity cleared current same-UUID media");
	}
	latch.release();
	lateDisconnect.join();
	manager.setNativeMediaTestPeerDisconnectDispatchHook(nullptr);

	const auto sourceTracks = source.nativeMediaTestTrackSnapshot();
	require(sourceTracks.video == newPrimary.get() && sourceTracks.alpha == newAlpha.get() &&
	            sourceTracks.audio == newAudio.get(),
	        "delayed old-generation disconnect cleared current same-UUID primary/alpha/audio media");
}

void testFirstNewGenerationEventAtomicallyRetiresOldMedia()
{
	VDONinjaSource source(NativeMediaTestTag{});
	VDONinjaPeerManager manager;
	source.bindNativeMediaTestPeerManager(manager);
	const std::string uuid = "partial-generation";
	auto oldPeer = manager.createNativeMediaTestViewerPeer(uuid, "session-a");
	auto oldPrimary = addVp9ReceiveTrack(oldPeer, "video");
	auto oldAlpha = addVp9ReceiveTrack(oldPeer, "video-alpha");
	auto oldAudio = addOpusReceiveTrack(oldPeer, "audio");
	manager.receiveNativeMediaTestTrack(oldPeer, oldPrimary);
	manager.receiveNativeMediaTestTrack(oldPeer, oldAlpha);
	manager.receiveNativeMediaTestTrack(oldPeer, oldAudio);

	require(source.advanceNativeMediaTestPeerIdentity({uuid, "session-b", oldPeer->generation + 1}),
	        "first replacement-generation event was not accepted");
	const auto tracks = source.nativeMediaTestTrackSnapshot();
	require(tracks.video == nullptr && tracks.alpha == nullptr && tracks.audio == nullptr,
	        "first replacement-generation event left mixed old primary/alpha/audio ownership");

	auto staleReplacement = makeVp9ReceiveTrack("video");
	manager.receiveNativeMediaTestTrack(oldPeer, staleReplacement.track);
	const auto afterStale = source.nativeMediaTestTrackSnapshot();
	require(afterStale.video == nullptr && afterStale.alpha == nullptr && afterStale.audio == nullptr,
	        "old-generation track event re-entered after replacement ownership was established");
}

void testFirstNewGenerationEventResetsPriorMuteState(const std::vector<std::vector<uint8_t>> &primaryGop)
{
	VDONinjaSource source(NativeMediaTestTag{});
	VDONinjaPeerManager manager;
	OutputCollector videoOutput;
	std::atomic<int> audioOutputs{0};
	source.setNativeMediaTestOutputHook(
	    [&videoOutput](NativeMediaTestOutput output) { videoOutput.add(std::move(output)); });
	source.setNativeMediaTestAudioOutputHook(
	    [&audioOutputs](uint64_t) { audioOutputs.fetch_add(1, std::memory_order_relaxed); });
	source.bindNativeMediaTestPeerManager(manager);
	const std::string uuid = "generation-mute-reset";
	auto oldPeer = manager.createNativeMediaTestViewerPeer(uuid, "session-a");
	auto oldPrimary = addVp9ReceiveTrack(oldPeer, "video");
	auto oldAlpha = addVp9ReceiveTrack(oldPeer, "video-alpha");
	auto oldAudio = addOpusReceiveTrack(oldPeer, "audio");
	manager.receiveNativeMediaTestTrack(oldPeer, oldPrimary);
	manager.receiveNativeMediaTestTrack(oldPeer, oldAlpha);
	manager.receiveNativeMediaTestTrack(oldPeer, oldAudio);
	manager.dispatchNativeMediaTestDataChannelMessage(
	    oldPeer, R"({"audioMuted":true,"videoMuted":true,"virtualHangup":true,"info":{"directorVideoMuted":true}})");

	auto snapshot = source.nativeMediaTestSnapshot();
	require(snapshot.audioMuted && snapshot.mediaVideoMuted && snapshot.directorVideoMuted && snapshot.virtualHangup &&
	            snapshot.videoSuppressed,
	        "gen1 did not establish all peer-scoped audio/video suppression components");

	manager.retireNativeMediaTestPeer(oldPeer);
	auto newPeer = manager.createNativeMediaTestViewerPeer(uuid, "session-b");
	auto newPrimary = addVp9ReceiveTrack(newPeer, "video");
	manager.receiveNativeMediaTestTrack(newPeer, newPrimary);

	snapshot = source.nativeMediaTestSnapshot();
	require(!snapshot.audioMuted && !snapshot.sourceAudioActive && !snapshot.mediaVideoMuted &&
	            !snapshot.directorVideoMuted && !snapshot.virtualHangup && !snapshot.videoSuppressed,
	        "first gen2 track left gen1 audio/video suppression authoritative");

	size_t cursor = 0;
	feedUntilOutputCount(source, videoOutput, primaryGop, cursor, 1800000u, 1);
	source.emitNativeMediaTestAudioFrame(123456789u);
	require(videoOutput.size() == 1, "gen2 video output remained blocked by gen1 suppression");
	require(audioOutputs.load(std::memory_order_relaxed) == 1, "gen2 audio output remained blocked by gen1 audio mute");
}

void testConcurrentGenerationStartAndCurrentStateLinearize()
{
	for (const bool stateReachesSourceFirst : {false, true}) {
		VDONinjaSource source(NativeMediaTestTag{});
		VDONinjaPeerManager manager;
		source.bindNativeMediaTestPeerManager(manager);
		const std::string uuid = stateReachesSourceFirst ? "generation-state-first" : "generation-track-first";
		auto oldPeer = manager.createNativeMediaTestViewerPeer(uuid, "session-a");
		auto oldPrimary = addVp9ReceiveTrack(oldPeer, "video");
		manager.receiveNativeMediaTestTrack(oldPeer, oldPrimary);
		manager.dispatchNativeMediaTestDataChannelMessage(
		    oldPeer,
		    R"({"audioMuted":true,"videoMuted":true,"virtualHangup":true,"info":{"directorVideoMuted":true}})");
		manager.retireNativeMediaTestPeer(oldPeer);

		auto newPeer = manager.createNativeMediaTestViewerPeer(uuid, "session-b");
		auto newPrimary = addVp9ReceiveTrack(newPeer, "video");
		if (stateReachesSourceFirst) {
			TrackCommitLatch trackLatch(newPrimary.get());
			manager.setNativeMediaTestTrackCommitHook([&trackLatch](const std::string &, TrackType,
			                                                        const std::shared_ptr<rtc::Track> &track,
			                                                        uint64_t) { trackLatch.hook(track); });
			std::thread delayedTrack([&]() { manager.receiveNativeMediaTestTrack(newPeer, newPrimary); });
			trackLatch.waitUntilEntered();
			manager.dispatchNativeMediaTestDataChannelMessage(newPeer, R"({"videoMuted":true})");
			const auto beforeTrack = source.nativeMediaTestSnapshot();
			trackLatch.release();
			delayedTrack.join();
			manager.setNativeMediaTestTrackCommitHook(nullptr);
			require(beforeTrack.mediaVideoMuted && beforeTrack.videoSuppressed && !beforeTrack.audioMuted &&
			            !beforeTrack.directorVideoMuted && !beforeTrack.virtualHangup,
			        "gen2 state-first event did not reset gen1 components before applying current mute");
		} else {
			PeerDispatchLatch stateLatch(newPeer->generation);
			manager.setNativeMediaTestPeerDataDispatchHook(
			    [&stateLatch](const std::shared_ptr<PeerInfo> &peer) { stateLatch.hook(peer); });
			std::thread delayedState(
			    [&]() { manager.dispatchNativeMediaTestDataChannelMessage(newPeer, R"({"videoMuted":true})"); });
			stateLatch.waitUntilEntered();
			manager.receiveNativeMediaTestTrack(newPeer, newPrimary);
			const auto beforeState = source.nativeMediaTestSnapshot();
			stateLatch.release();
			delayedState.join();
			manager.setNativeMediaTestPeerDataDispatchHook(nullptr);
			require(!beforeState.audioMuted && !beforeState.mediaVideoMuted && !beforeState.directorVideoMuted &&
			            !beforeState.virtualHangup && !beforeState.videoSuppressed,
			        "gen2 track-first event did not atomically clear all gen1 suppression state");
		}

		const auto finalSnapshot = source.nativeMediaTestSnapshot();
		require(source.nativeMediaTestTrackSnapshot().video == newPrimary.get() && finalSnapshot.mediaVideoMuted &&
		            finalSnapshot.videoSuppressed && !finalSnapshot.audioMuted && !finalSnapshot.directorVideoMuted &&
		            !finalSnapshot.virtualHangup && !finalSnapshot.sourceAudioActive &&
		            source.nativeMediaTestCanAcquireVideoCommitState(),
		        "concurrent gen2 generation-start/state ordering lost current state, media, audio ownership, or commit "
		        "lock");
	}
}

void testOldGenerationDataMessagesCannotMutateNewGeneration()
{
	{
		VDONinjaSource source(NativeMediaTestTag{});
		VDONinjaPeerManager manager;
		source.bindNativeMediaTestPeerManager(manager);
		const std::string uuid = "data-bye-rotate";
		auto oldPeer = manager.createNativeMediaTestViewerPeer(uuid, "session-a");
		auto oldPrimary = addVp9ReceiveTrack(oldPeer, "video");
		manager.receiveNativeMediaTestTrack(oldPeer, oldPrimary);

		PeerDispatchLatch latch(oldPeer->generation);
		manager.setNativeMediaTestPeerDataDispatchHook(
		    [&latch](const std::shared_ptr<PeerInfo> &peer) { latch.hook(peer); });
		std::thread lateBye([&]() { manager.dispatchNativeMediaTestDataChannelMessage(oldPeer, R"({"bye":true})"); });
		latch.waitUntilEntered();
		manager.retireNativeMediaTestPeer(oldPeer);
		auto newPeer = manager.createNativeMediaTestViewerPeer(uuid, "session-b");
		auto newPrimary = addVp9ReceiveTrack(newPeer, "video");
		auto newAlpha = addVp9ReceiveTrack(newPeer, "video-alpha");
		auto newAudio = addOpusReceiveTrack(newPeer, "audio");
		manager.receiveNativeMediaTestTrack(newPeer, newPrimary);
		manager.receiveNativeMediaTestTrack(newPeer, newAlpha);
		manager.receiveNativeMediaTestTrack(newPeer, newAudio);
		latch.release();
		lateBye.join();
		manager.setNativeMediaTestPeerDataDispatchHook(nullptr);

		const auto tracks = source.nativeMediaTestTrackSnapshot();
		require(tracks.video == newPrimary.get() && tracks.alpha == newAlpha.get() && tracks.audio == newAudio.get(),
		        "delayed old-generation data-channel bye cleared current same-UUID media");
	}

	{
		VDONinjaSource source(NativeMediaTestTag{});
		VDONinjaPeerManager manager;
		source.bindNativeMediaTestPeerManager(manager);
		const std::string uuid = "data-mute-rotate";
		auto oldPeer = manager.createNativeMediaTestViewerPeer(uuid, "session-a");
		auto oldPrimary = addVp9ReceiveTrack(oldPeer, "video");
		manager.receiveNativeMediaTestTrack(oldPeer, oldPrimary);

		PeerDispatchLatch latch(oldPeer->generation);
		manager.setNativeMediaTestPeerDataDispatchHook(
		    [&latch](const std::shared_ptr<PeerInfo> &peer) { latch.hook(peer); });
		std::thread lateMute(
		    [&]() { manager.dispatchNativeMediaTestDataChannelMessage(oldPeer, R"({"videoMuted":true})"); });
		latch.waitUntilEntered();
		manager.retireNativeMediaTestPeer(oldPeer);
		auto newPeer = manager.createNativeMediaTestViewerPeer(uuid, "session-b");
		auto newPrimary = addVp9ReceiveTrack(newPeer, "video");
		manager.receiveNativeMediaTestTrack(newPeer, newPrimary);
		latch.release();
		lateMute.join();
		manager.setNativeMediaTestPeerDataDispatchHook(nullptr);

		const auto snapshot = source.nativeMediaTestSnapshot();
		require(!snapshot.mediaVideoMuted && !snapshot.videoSuppressed,
		        "delayed old-generation mute overwrote current same-UUID suppression state");
	}
}

void testTerminalGenerationRejectsParkedEqualGenerationEvents()
{
	VDONinjaSource source(NativeMediaTestTag{});
	VDONinjaPeerManager manager;
	source.bindNativeMediaTestPeerManager(manager);
	const std::string uuid = "terminal-data-events";
	auto peer = manager.createNativeMediaTestViewerPeer(uuid, "session-a");
	auto primary = addVp9ReceiveTrack(peer, "video");
	manager.receiveNativeMediaTestTrack(peer, primary);

	PeerDispatchBarrier barrier(peer->generation, 2);
	manager.setNativeMediaTestPeerDataDispatchHook(
	    [&barrier](const std::shared_ptr<PeerInfo> &dispatchedPeer) { barrier.hook(dispatchedPeer); });
	std::thread lateMute([&]() { manager.dispatchNativeMediaTestDataChannelMessage(peer, R"({"videoMuted":true})"); });
	std::thread lateBye([&]() { manager.dispatchNativeMediaTestDataChannelMessage(peer, R"({"bye":true})"); });
	barrier.waitUntilEntered();

	const auto cleanupIdentity = manager.claimPeerEventIdentity(uuid, peer->session);
	require(cleanupIdentity.has_value(), "could not claim an ordered current-generation cleanup identity");
	source.applyNativeMediaTestPeerCleanup(*cleanupIdentity);
	auto snapshot = source.nativeMediaTestSnapshot();
	require(snapshot.acceptedPeerCleanups == 1 && snapshot.peerRetirements == 1 && snapshot.peerRetrySchedules == 1,
	        "initial terminal cleanup did not retire media and schedule recovery exactly once");
	barrier.release();
	lateMute.join();
	lateBye.join();
	manager.setNativeMediaTestPeerDataDispatchHook(nullptr);

	snapshot = source.nativeMediaTestSnapshot();
	require(!snapshot.mediaVideoMuted && !snapshot.videoSuppressed,
	        "parked equal-generation mute mutated state after terminal cleanup");
	require(snapshot.acceptedPeerCleanups == 1 && snapshot.peerRetirements == 1 && snapshot.peerRetrySchedules == 1,
	        "parked equal-generation bye repeated cleanup, media retirement, or retry scheduling");
	require(source.nativeMediaTestTrackSnapshot().video == nullptr,
	        "parked equal-generation event reinstalled terminal media");
	const auto postTerminalIdentity = manager.claimPeerEventIdentity(uuid, peer->session);
	require(postTerminalIdentity.has_value() && !source.advanceNativeMediaTestPeerIdentity(*postTerminalIdentity),
	        "same-generation connected/active event reopened terminal reducer state");
	source.applyNativeMediaTestPeerCleanup(*postTerminalIdentity);
	snapshot = source.nativeMediaTestSnapshot();
	require(snapshot.acceptedPeerCleanups == 1 && snapshot.peerRetirements == 1 && snapshot.peerRetrySchedules == 1,
	        "same-generation repeated cleanup changed terminal exact-once effects");

	manager.retireNativeMediaTestPeer(peer);
	auto replacement = manager.createNativeMediaTestViewerPeer(uuid, "session-b");
	auto replacementPrimary = addVp9ReceiveTrack(replacement, "video");
	manager.receiveNativeMediaTestTrack(replacement, replacementPrimary);
	manager.dispatchNativeMediaTestDataChannelMessage(replacement, R"({"videoMuted":true})");
	snapshot = source.nativeMediaTestSnapshot();
	require(source.nativeMediaTestTrackSnapshot().video == replacementPrimary.get() && snapshot.mediaVideoMuted &&
	            snapshot.videoSuppressed,
	        "replacement generation did not reopen after terminal generation state");
}

void testTerminalAdoptionPublishesNewOwnerDefaultState(const std::vector<std::vector<uint8_t>> &primaryGop)
{
	for (const bool rtcTerminal : {false, true}) {
		VDONinjaSource source(NativeMediaTestTag{});
		VDONinjaPeerManager manager;
		OutputCollector videoOutput;
		std::atomic<int> audioOutputs{0};
		source.setNativeMediaTestOutputHook(
		    [&videoOutput](NativeMediaTestOutput output) { videoOutput.add(std::move(output)); });
		source.setNativeMediaTestAudioOutputHook(
		    [&audioOutputs](uint64_t) { audioOutputs.fetch_add(1, std::memory_order_relaxed); });
		source.bindNativeMediaTestPeerManager(manager);

		auto ownerA = manager.createNativeMediaTestViewerPeer(
		    rtcTerminal ? "terminal-default-a-rtc" : "terminal-default-a-signal", "session-a");
		auto ownerAPrimary = addVp9ReceiveTrack(ownerA, "video");
		auto ownerAAudio = addOpusReceiveTrack(ownerA, "audio");
		manager.receiveNativeMediaTestTrack(ownerA, ownerAPrimary);
		manager.receiveNativeMediaTestTrack(ownerA, ownerAAudio);
		manager.dispatchNativeMediaTestDataChannelMessage(
		    ownerA, R"({"audioMuted":true,"videoMuted":true,"virtualHangup":true,"info":{"directorVideoMuted":true}})");

		auto ownerB = manager.createNativeMediaTestViewerPeer(
		    rtcTerminal ? "terminal-default-b-rtc" : "terminal-default-b-signal", "session-b");
		auto ownerBPrimary = addVp9ReceiveTrack(ownerB, "video");
		auto ownerBAudio = addOpusReceiveTrack(ownerB, "audio");
		manager.receiveNativeMediaTestTrack(ownerB, ownerBPrimary);
		manager.receiveNativeMediaTestTrack(ownerB, ownerBAudio);
		auto tracks = source.nativeMediaTestTrackSnapshot();
		require(tracks.video == ownerAPrimary.get() && tracks.audio == ownerAAudio.get(),
		        "owner B tracks were not deferred while owner A remained active");

		PeerDispatchLatch delayedOwnerAState(ownerA->generation);
		manager.setNativeMediaTestPeerDataDispatchHook(
		    [&delayedOwnerAState](const std::shared_ptr<PeerInfo> &peer) { delayedOwnerAState.hook(peer); });
		std::thread delayedA([&]() {
			manager.dispatchNativeMediaTestDataChannelMessage(
			    ownerA,
			    R"({"audioMuted":false,"videoMuted":false,"virtualHangup":false,"info":{"directorVideoMuted":false}})");
		});
		delayedOwnerAState.waitUntilEntered();
		bool terminalIdentityClaimed = true;
		if (rtcTerminal) {
			manager.dispatchNativeMediaTestPeerDisconnected(ownerA);
		} else {
			const auto terminalIdentity = manager.claimPeerEventIdentity(ownerA->uuid, ownerA->session);
			terminalIdentityClaimed = terminalIdentity.has_value();
			if (terminalIdentity) {
				source.applyNativeMediaTestPeerCleanup(*terminalIdentity);
			}
		}
		delayedOwnerAState.release();
		delayedA.join();
		manager.setNativeMediaTestPeerDataDispatchHook(nullptr);
		require(terminalIdentityClaimed, "could not claim owner A signaling terminal identity");

		tracks = source.nativeMediaTestTrackSnapshot();
		const auto snapshot = source.nativeMediaTestSnapshot();
		require(tracks.video == ownerBPrimary.get() && tracks.audio == ownerBAudio.get(),
		        "terminal owner A did not adopt deferred owner B video/audio");
		require(!snapshot.audioMuted && snapshot.sourceAudioActive && !snapshot.mediaVideoMuted &&
		            !snapshot.directorVideoMuted && !snapshot.virtualHangup && !snapshot.videoSuppressed,
		        "terminal owner A suppression remained authoritative after default-state owner B adoption");
		require(snapshot.peerRetirements == 1 && snapshot.peerRetrySchedules == 1 &&
		            snapshot.acceptedPeerCleanups == (rtcTerminal ? 0 : 1),
		        "terminal adoption effects were not exact once for the selected terminal path");

		size_t cursor = 0;
		feedUntilOutputCount(source, videoOutput, primaryGop, cursor, 2100000u, 1);
		source.emitNativeMediaTestAudioFrame(22334455u);
		require(videoOutput.size() == 1 && audioOutputs.load(std::memory_order_relaxed) == 1,
		        "default-state owner B real video/audio output remained blocked by terminal owner A");
		require(source.nativeMediaTestCanAcquireVideoCommitState(),
		        "terminal default-state adoption left the suppression commit locked");
	}
}

void testTerminalAdoptionPreservesStoredNewOwnerState()
{
	for (const bool rtcTerminal : {false, true}) {
		for (const bool ownerBStateBeforeTerminal : {false, true}) {
			VDONinjaSource source(NativeMediaTestTag{});
			VDONinjaPeerManager manager;
			source.bindNativeMediaTestPeerManager(manager);
			const std::string suffix =
			    std::string(rtcTerminal ? "rtc-" : "signal-") + (ownerBStateBeforeTerminal ? "before" : "after");
			auto ownerA = manager.createNativeMediaTestViewerPeer("terminal-stored-a-" + suffix, "session-a");
			auto ownerAPrimary = addVp9ReceiveTrack(ownerA, "video");
			auto ownerAAudio = addOpusReceiveTrack(ownerA, "audio");
			manager.receiveNativeMediaTestTrack(ownerA, ownerAPrimary);
			manager.receiveNativeMediaTestTrack(ownerA, ownerAAudio);
			manager.dispatchNativeMediaTestDataChannelMessage(
			    ownerA,
			    R"({"audioMuted":true,"videoMuted":true,"virtualHangup":true,"info":{"directorVideoMuted":true}})");

			auto ownerB = manager.createNativeMediaTestViewerPeer("terminal-stored-b-" + suffix, "session-b");
			auto ownerBPrimary = addVp9ReceiveTrack(ownerB, "video");
			auto ownerBAudio = addOpusReceiveTrack(ownerB, "audio");
			manager.receiveNativeMediaTestTrack(ownerB, ownerBPrimary);
			manager.receiveNativeMediaTestTrack(ownerB, ownerBAudio);

			PeerDispatchLatch delayedOwnerAState(ownerA->generation);
			PeerDispatchLatch delayedOwnerBState(ownerB->generation);
			std::thread delayedB;
			if (ownerBStateBeforeTerminal) {
				manager.dispatchNativeMediaTestDataChannelMessage(
				    ownerA,
				    R"({"audioMuted":false,"videoMuted":false,"virtualHangup":false,"info":{"directorVideoMuted":false}})");
				manager.dispatchNativeMediaTestDataChannelMessage(
				    ownerB,
				    R"({"audioMuted":true,"videoMuted":true,"virtualHangup":true,"info":{"directorVideoMuted":true}})");
				const auto inactiveUpdate = source.nativeMediaTestSnapshot();
				require(!inactiveUpdate.audioMuted && !inactiveUpdate.videoSuppressed,
				        "deferred owner B state leaked into active owner A before terminal adoption");
				manager.dispatchNativeMediaTestDataChannelMessage(
				    ownerA,
				    R"({"audioMuted":true,"videoMuted":true,"virtualHangup":true,"info":{"directorVideoMuted":true}})");
				manager.setNativeMediaTestPeerDataDispatchHook(
				    [&delayedOwnerAState](const std::shared_ptr<PeerInfo> &peer) { delayedOwnerAState.hook(peer); });
			} else {
				manager.setNativeMediaTestPeerDataDispatchHook(
				    [&delayedOwnerAState, &delayedOwnerBState](const std::shared_ptr<PeerInfo> &peer) {
					    delayedOwnerAState.hook(peer);
					    delayedOwnerBState.hook(peer);
				    });
				delayedB = std::thread([&]() {
					manager.dispatchNativeMediaTestDataChannelMessage(
					    ownerB,
					    R"({"audioMuted":true,"videoMuted":true,"virtualHangup":true,"info":{"directorVideoMuted":true}})");
				});
				delayedOwnerBState.waitUntilEntered();
			}

			std::thread delayedA([&]() {
				manager.dispatchNativeMediaTestDataChannelMessage(
				    ownerA,
				    R"({"audioMuted":false,"videoMuted":false,"virtualHangup":false,"info":{"directorVideoMuted":false}})");
			});
			delayedOwnerAState.waitUntilEntered();
			bool terminalIdentityClaimed = true;
			if (rtcTerminal) {
				manager.dispatchNativeMediaTestPeerDisconnected(ownerA);
			} else {
				const auto terminalIdentity = manager.claimPeerEventIdentity(ownerA->uuid, ownerA->session);
				terminalIdentityClaimed = terminalIdentity.has_value();
				if (terminalIdentity) {
					source.applyNativeMediaTestPeerCleanup(*terminalIdentity);
				}
			}
			NativeMediaTestSnapshot beforeBResume;
			if (!ownerBStateBeforeTerminal) {
				beforeBResume = source.nativeMediaTestSnapshot();
				delayedOwnerBState.release();
				delayedB.join();
			}
			delayedOwnerAState.release();
			delayedA.join();
			manager.setNativeMediaTestPeerDataDispatchHook(nullptr);
			require(terminalIdentityClaimed, "could not claim stored-state owner A terminal identity");
			if (!ownerBStateBeforeTerminal) {
				require(!beforeBResume.audioMuted && !beforeBResume.videoSuppressed,
				        "owner A state remained published after terminal adoption before owner B state resumed");
			}

			const auto tracks = source.nativeMediaTestTrackSnapshot();
			const auto snapshot = source.nativeMediaTestSnapshot();
			require(tracks.video == ownerBPrimary.get() && tracks.audio == ownerBAudio.get(),
			        "stored-state terminal adoption did not retain owner B video/audio");
			require(snapshot.audioMuted && !snapshot.sourceAudioActive && snapshot.mediaVideoMuted &&
			            snapshot.directorVideoMuted && snapshot.virtualHangup && snapshot.videoSuppressed,
			        "owner B stored suppression did not remain authoritative after either terminal ordering");
			require(snapshot.peerRetirements == 1 && snapshot.acceptedPeerCleanups == (rtcTerminal ? 0 : 1) &&
			            source.nativeMediaTestCanAcquireVideoCommitState(),
			        "stored-state terminal adoption repeated effects or retained the commit lock");
		}
	}
}

void testNoSubscriberRetirementCannotClearReaddedTrackCallbacks()
{
	for (const bool alphaSlot : {false, true}) {
		VDONinjaSource source(NativeMediaTestTag{});
		VDONinjaPeerManager manager;
		source.bindNativeMediaTestPeerManager(manager);
		auto peer = manager.createNativeMediaTestViewerPeer(alphaSlot ? "no-subscriber-alpha" : "no-subscriber-video");
		if (alphaSlot) {
			auto primary = addVp9ReceiveTrack(peer, "video");
			manager.receiveNativeMediaTestTrack(peer, primary);
		}
		auto reused = makeConnectedVp9TrackPair(alphaSlot ? "video-alpha" : "video");
		manager.receiveNativeMediaTestTrack(peer, reused.receiver);
		manager.setOnTrack(nullptr);

		const TrackType type = alphaSlot ? TrackType::AlphaVideo : TrackType::Video;
		TrackDispatchLatch latch(reused.receiver.get(), type);
		manager.setNativeMediaTestTrackDispatchHook(
		    [&latch](const TrackSlotEvent &event, bool hadSubscriber) { latch.hook(event, hadSubscriber); });
		std::thread delayedRetirement([&]() { manager.retireNativeMediaTestTrackSlot(peer, type); });
		latch.waitUntilEntered();
		source.bindNativeMediaTestPeerManager(manager);
		manager.receiveNativeMediaTestTrack(peer, reused.receiver);
		latch.release();
		delayedRetirement.join();
		manager.setNativeMediaTestTrackDispatchHook(nullptr);

		StageSignal messageDelivered(NativeMediaTestStage::PreAssembly);
		source.setNativeMediaTestStageHook(
		    [&messageDelivered, alphaSlot](NativeMediaTestStage stage, bool alpha, uint32_t, uint64_t) {
			    if (alpha == alphaSlot) {
				    messageDelivered.hook(stage);
			    }
		    });
		require(reused.sender->send(makeVp9RtpProbe(alphaSlot ? 191000u : 190000u)),
		        "re-added RTC sender rejected the no-subscriber RTP probe");
		messageDelivered.waitUntilReached();
		source.setNativeMediaTestStageHook(nullptr);

		reused.receiver->close();
		const auto tracks = source.nativeMediaTestTrackSnapshot();
		require(alphaSlot ? tracks.alpha == nullptr : tracks.video == nullptr,
		        "delayed no-subscriber cleanup cleared the re-added Track onClosed callback");
	}
}

void testSessionlessSignalingCleanupDoesNotRebindAfterGenerationReuse()
{
	{
		VDONinjaSource source(NativeMediaTestTag{});
		VDONinjaPeerManager manager;
		source.bindNativeMediaTestPeerManager(manager);
		const std::string uuid = "ambiguous-ws-cleanup";
		auto oldPeer = manager.createNativeMediaTestViewerPeer(uuid, "session-a");
		auto oldPrimary = addVp9ReceiveTrack(oldPeer, "video");
		manager.receiveNativeMediaTestTrack(oldPeer, oldPrimary);
		manager.retireNativeMediaTestPeer(oldPeer);
		auto currentPeer = manager.createNativeMediaTestViewerPeer(uuid, "session-b");
		auto currentPrimary = addVp9ReceiveTrack(currentPeer, "video");
		manager.receiveNativeMediaTestTrack(currentPeer, currentPrimary);

		source.applyNativeMediaTestSignalingCleanup(manager, uuid, "");
		require(source.nativeMediaTestTrackSnapshot().video == currentPrimary.get(),
		        "sessionless delayed websocket cleanup rebound onto the replacement generation");
		require(source.nativeMediaTestSnapshot().ambiguousSessionlessCleanups == 1,
		        "ambiguous sessionless websocket cleanup was not recorded exactly once");
	}

	{
		VDONinjaSource source(NativeMediaTestTag{});
		VDONinjaPeerManager manager;
		source.bindNativeMediaTestPeerManager(manager);
		const std::string uuid = "single-generation-sessionless-cleanup";
		auto peer = manager.createNativeMediaTestViewerPeer(uuid, "session-only");
		auto primary = addVp9ReceiveTrack(peer, "video");
		manager.receiveNativeMediaTestTrack(peer, primary);
		source.applyNativeMediaTestSignalingCleanup(manager, uuid, "");
		require(source.nativeMediaTestTrackSnapshot().video == nullptr &&
		            source.nativeMediaTestSnapshot().acceptedPeerCleanups == 1,
		        "unambiguous first-generation sessionless cleanup was not accepted exactly once");
	}

	{
		VDONinjaSource source(NativeMediaTestTag{});
		VDONinjaPeerManager manager;
		source.bindNativeMediaTestPeerManager(manager);
		const std::string uuid = "matched-ws-cleanup";
		auto peer = manager.createNativeMediaTestViewerPeer(uuid, "session-current");
		auto primary = addVp9ReceiveTrack(peer, "video");
		manager.receiveNativeMediaTestTrack(peer, primary);
		source.applyNativeMediaTestSignalingCleanup(manager, uuid, peer->session);
		source.applyNativeMediaTestSignalingCleanup(manager, uuid, peer->session);
		const auto snapshot = source.nativeMediaTestSnapshot();
		require(source.nativeMediaTestTrackSnapshot().video == nullptr && snapshot.acceptedPeerCleanups == 1,
		        "session-matched websocket cleanup did not retire the current generation exactly once");
	}

	{
		VDONinjaSource source(NativeMediaTestTag{});
		VDONinjaPeerManager manager;
		source.bindNativeMediaTestPeerManager(manager);
		auto peer = manager.createNativeMediaTestViewerPeer("channel-bound-bye", "session-current");
		auto primary = addVp9ReceiveTrack(peer, "video");
		manager.receiveNativeMediaTestTrack(peer, primary);
		manager.dispatchNativeMediaTestDataChannelMessage(peer, R"({"bye":true})");
		require(source.nativeMediaTestTrackSnapshot().video == nullptr &&
		            source.nativeMediaTestSnapshot().acceptedPeerCleanups == 1,
		        "identity-bearing DataChannel bye did not retire the current generation exactly once");
	}
}

void testManagerProvenanceRejectsSessionlessCleanupAfterUnobservedUuidReuse()
{
	for (const bool firstGenerationTwoSourceEvent : {false, true}) {
		VDONinjaSource source(NativeMediaTestTag{});
		VDONinjaPeerManager manager;
		const std::string uuid = firstGenerationTwoSourceEvent ? "unobserved-reuse-with-event" : "unobserved-reuse";
		auto generationOne = manager.createNativeMediaTestViewerPeer(uuid, "session-a");
		manager.retireNativeMediaTestPeer(generationOne);
		auto generationTwo = manager.createNativeMediaTestViewerPeer(uuid, "session-b");
		source.bindNativeMediaTestPeerManager(manager);
		std::shared_ptr<rtc::Track> generationTwoPrimary;
		if (firstGenerationTwoSourceEvent) {
			generationTwoPrimary = addVp9ReceiveTrack(generationTwo, "video");
			manager.receiveNativeMediaTestTrack(generationTwo, generationTwoPrimary);
		}

		source.applyNativeMediaTestSignalingCleanup(manager, uuid, "");
		const auto currentIdentity = manager.getPeerIdentity(uuid);
		const auto snapshot = source.nativeMediaTestSnapshot();
		require(currentIdentity && currentIdentity->generation == generationTwo->generation &&
		            snapshot.acceptedPeerCleanups == 0 && snapshot.ambiguousSessionlessCleanups == 1,
		        "manager provenance did not reject sessionless cleanup after unobserved UUID reuse");
		if (firstGenerationTwoSourceEvent) {
			require(source.nativeMediaTestTrackSnapshot().video == generationTwoPrimary.get(),
			        "ambiguous cleanup cleared the first observed generation-two media event");
		}
	}

	{
		VDONinjaSource source(NativeMediaTestTag{});
		VDONinjaPeerManager manager;
		source.bindNativeMediaTestPeerManager(manager);
		const std::string uuid = "reuse-survives-source-reset";
		auto generationOne = manager.createNativeMediaTestViewerPeer(uuid, "session-a");
		auto generationOnePrimary = addVp9ReceiveTrack(generationOne, "video");
		manager.receiveNativeMediaTestTrack(generationOne, generationOnePrimary);
		manager.retireNativeMediaTestPeer(generationOne);
		auto generationTwo = manager.createNativeMediaTestViewerPeer(uuid, "session-b");
		auto generationTwoPrimary = addVp9ReceiveTrack(generationTwo, "video");
		manager.receiveNativeMediaTestTrack(generationTwo, generationTwoPrimary);
		source.resetNativeMediaTestState();
		source.applyNativeMediaTestSignalingCleanup(manager, uuid, "");
		const auto currentIdentity = manager.getPeerIdentity(uuid);
		const auto snapshot = source.nativeMediaTestSnapshot();
		require(currentIdentity && currentIdentity->generation == generationTwo->generation &&
		            snapshot.acceptedPeerCleanups == 0 && snapshot.ambiguousSessionlessCleanups == 1,
		        "source reducer reset erased manager-owned UUID reuse provenance");
	}

	{
		VDONinjaSource source(NativeMediaTestTag{});
		VDONinjaPeerManager manager;
		source.bindNativeMediaTestPeerManager(manager);
		const std::string uuid = "reused-exact-cleanup";
		auto generationOne = manager.createNativeMediaTestViewerPeer(uuid, "session-a");
		manager.retireNativeMediaTestPeer(generationOne);
		auto generationTwo = manager.createNativeMediaTestViewerPeer(uuid, "session-b");
		auto generationTwoPrimary = addVp9ReceiveTrack(generationTwo, "video");
		manager.receiveNativeMediaTestTrack(generationTwo, generationTwoPrimary);
		source.applyNativeMediaTestSignalingCleanup(manager, uuid, "wrong-session");
		require(source.nativeMediaTestTrackSnapshot().video == generationTwoPrimary.get() &&
		            source.nativeMediaTestSnapshot().acceptedPeerCleanups == 0,
		        "session-mismatched cleanup retired reused UUID generation");
		source.applyNativeMediaTestSignalingCleanup(manager, uuid, generationTwo->session);
		source.applyNativeMediaTestSignalingCleanup(manager, uuid, generationTwo->session);
		require(source.nativeMediaTestTrackSnapshot().video == nullptr &&
		            source.nativeMediaTestSnapshot().acceptedPeerCleanups == 1,
		        "session-matched generation-two cleanup did not retire exactly once");
	}
}

void testParkedOldStreamRemovedLifecycleCannotRetireReplacementGeneration()
{
	VDONinjaSource source(NativeMediaTestTag{});
	VDONinjaPeerManager manager;
	source.bindNativeMediaTestPeerManager(manager);
	const std::string uuid = "stream-lifecycle-rotate";
	const std::string streamId = "camera-stream";
	source.setNativeMediaTestStreamId(streamId);

	auto oldPeer = manager.createNativeMediaTestViewerPeer(uuid, "session-a");
	oldPeer->streamId = streamId;
	auto oldPrimary = addVp9ReceiveTrack(oldPeer, "video");
	manager.receiveNativeMediaTestTrack(oldPeer, oldPrimary);

	SignalingLifecycleEvent staleRemoval;
	staleRemoval.kind = SignalingLifecycleEventKind::StreamRemoved;
	staleRemoval.socketEpoch = 41;
	staleRemoval.wsSequence = 7;
	staleRemoval.uuid = uuid;
	staleRemoval.session = oldPeer->session;
	staleRemoval.streamId = streamId;
	SignalingLifecycleAdmissionLatch latch(staleRemoval.socketEpoch, oldPeer->generation);
	source.setNativeMediaTestSignalingLifecycleHook(
	    [&latch](const SignalingLifecycleEvent &event, const std::optional<PeerEventIdentity> &identity) {
		    latch.hook(event, identity);
	    });
	std::thread delayedRemoval([&]() { source.applyNativeMediaTestSignalingLifecycleEvent(manager, staleRemoval); });
	latch.waitUntilEntered();

	manager.retireNativeMediaTestPeer(oldPeer);
	auto currentPeer = manager.createNativeMediaTestViewerPeer(uuid, "session-b");
	currentPeer->streamId = streamId;
	auto currentPrimary = addVp9ReceiveTrack(currentPeer, "video");
	auto currentAlpha = addVp9ReceiveTrack(currentPeer, "video-alpha");
	auto currentAudio = addOpusReceiveTrack(currentPeer, "audio");
	manager.receiveNativeMediaTestTrack(currentPeer, currentPrimary);
	manager.receiveNativeMediaTestTrack(currentPeer, currentAlpha);
	manager.receiveNativeMediaTestTrack(currentPeer, currentAudio);
	auto currentDataChannel = currentPeer->pc->createDataChannel("sendChannel");
	manager.receiveNativeMediaTestDataChannel(currentPeer, currentDataChannel);

	latch.release();
	delayedRemoval.join();
	source.setNativeMediaTestSignalingLifecycleHook(nullptr);

	const auto sourceTracks = source.nativeMediaTestTrackSnapshot();
	const auto currentIdentity = manager.getPeerIdentity(uuid);
	bool managerLeaseSurvived = false;
	{
		std::lock_guard<std::mutex> mediaLock(currentPeer->mediaMutex);
		managerLeaseSurvived =
		    currentPeer->videoTrack == currentPrimary && currentPeer->alphaVideoTrack == currentAlpha &&
		    currentPeer->audioTrack == currentAudio && currentPeer->dataChannel == currentDataChannel;
	}
	const auto sourceState = source.nativeMediaTestSnapshot();
	require(currentIdentity && currentIdentity->generation == currentPeer->generation && managerLeaseSurvived &&
	            sourceTracks.video == currentPrimary.get() && sourceTracks.alpha == currentAlpha.get() &&
	            sourceTracks.audio == currentAudio.get() && sourceState.acceptedPeerCleanups == 0 &&
	            sourceState.peerRetrySchedules == 0 && sourceState.legacyStreamRemovalActions == 0,
	        "parked old-session streamRemoved erased replacement primary/alpha/audio/DataChannel state");
}

void testCurrentLifecycleShapesRetireExactlyOnceWithoutLegacyCompetition()
{
	for (const bool streamRemoved : {false, true}) {
		VDONinjaSource source(NativeMediaTestTag{});
		VDONinjaPeerManager manager;
		VDONinjaSignaling signaling;
		source.bindNativeMediaTestPeerManager(manager);
		source.bindNativeMediaTestSignaling(signaling, manager);
		const std::string suffix = streamRemoved ? "stream" : "cleanup";
		const std::string uuid = "lifecycle-authoritative-" + suffix;
		const std::string session = "session-current";
		const std::string streamId = "deployed-camera-" + suffix;
		source.setNativeMediaTestStreamId(streamId);
		auto peer = manager.createNativeMediaTestViewerPeer(uuid, session);
		peer->streamId = streamId;
		auto primary = addVp9ReceiveTrack(peer, "video");
		manager.receiveNativeMediaTestTrack(peer, primary);

		JsonBuilder lifecycleMessage;
		if (streamRemoved) {
			lifecycleMessage.add("videoRemovedFromRoom", true);
			lifecycleMessage.add("streamID", streamId);
		} else {
			lifecycleMessage.add("request", "cleanup");
		}
		lifecycleMessage.add("UUID", uuid);
		lifecycleMessage.add("session", session);
		const std::string encoded = lifecycleMessage.build();
		signaling.processIncomingMessage(encoded);
		signaling.processIncomingMessage(encoded);

		const auto snapshot = source.nativeMediaTestSnapshot();
		require(!manager.getPeerIdentity(uuid) && source.nativeMediaTestTrackSnapshot().video == nullptr &&
		            snapshot.acceptedPeerCleanups == 1 && snapshot.peerRetirements == 1 &&
		            snapshot.peerRetrySchedules == 1 && snapshot.legacyStreamRemovalActions == 0,
		        "authoritative deployed " + suffix +
		            " lifecycle shape did not retire exactly once through one identity-safe callback path");
	}
}

void testCurrentSocketLifecycleEventLinearizesOutsideManagerLocks()
{
	VDONinjaPeerManager manager;
	const std::string uuid = "lifecycle-current-socket";
	auto peer = manager.createNativeMediaTestViewerPeer(uuid, "session-current");
	std::atomic<int> callbacks{0};
	std::atomic<bool> registryLockAvailable{false};
	std::atomic<bool> exactIdentityVisible{false};
	manager.setOnAcceptedSignalingLifecycleEvent([&](const AcceptedSignalingLifecycleEvent &accepted) {
		callbacks.fetch_add(1, std::memory_order_acq_rel);
		const bool available = manager.nativeMediaTestPeerRegistryLockAvailable();
		registryLockAvailable.store(available, std::memory_order_release);
		if (available) {
			const auto current = manager.getPeerIdentity(accepted.identity.uuid);
			exactIdentityVisible.store(current && current->generation == accepted.identity.generation,
			                           std::memory_order_release);
		}
	});

	SignalingLifecycleEvent cleanup;
	cleanup.kind = SignalingLifecycleEventKind::PeerCleanup;
	cleanup.socketEpoch = 47;
	cleanup.wsSequence = 12;
	cleanup.uuid = uuid;
	cleanup.session = peer->session;
	const auto firstDisposition = manager.processSignalingLifecycleEvent(cleanup);
	const auto duplicateDisposition = manager.processSignalingLifecycleEvent(cleanup);
	require(firstDisposition == SignalingLifecycleDisposition::Accepted &&
	            duplicateDisposition == SignalingLifecycleDisposition::Stale &&
	            callbacks.load(std::memory_order_acquire) == 1 &&
	            registryLockAvailable.load(std::memory_order_acquire) &&
	            exactIdentityVisible.load(std::memory_order_acquire) && !manager.getPeerIdentity(uuid),
	        "current socket lifecycle event did not linearize exactly once outside manager locks");
}

void testUniqueSessionlessStreamRemovedLifecycleIsAccepted()
{
	{
		VDONinjaSource source(NativeMediaTestTag{});
		VDONinjaPeerManager manager;
		source.bindNativeMediaTestPeerManager(manager);
		const std::string uuid = "stream-sessionless-unique";
		const std::string streamId = "stream-sessionless-unique-id";
		source.setNativeMediaTestStreamId(streamId);
		auto peer = manager.createNativeMediaTestViewerPeer(uuid, "session-only");
		peer->streamId = streamId;
		auto primary = addVp9ReceiveTrack(peer, "video");
		manager.receiveNativeMediaTestTrack(peer, primary);
		SignalingLifecycleEvent removal;
		removal.kind = SignalingLifecycleEventKind::StreamRemoved;
		removal.socketEpoch = 51;
		removal.wsSequence = 1;
		removal.uuid = uuid;
		removal.streamId = streamId;
		source.applyNativeMediaTestSignalingLifecycleEvent(manager, removal);
		source.applyNativeMediaTestSignalingLifecycleEvent(manager, removal);
		const auto snapshot = source.nativeMediaTestSnapshot();
		require(!manager.getPeerIdentity(uuid) && source.nativeMediaTestTrackSnapshot().video == nullptr &&
		            snapshot.acceptedPeerCleanups == 1 && snapshot.peerRetrySchedules == 1 &&
		            snapshot.ambiguousSessionlessCleanups == 0 && snapshot.legacyStreamRemovalActions == 0,
		        "unique first-generation sessionless streamRemoved was not accepted exactly once");
	}
}

void testAmbiguousSessionlessStreamRemovedCannotRebindAfterReuse()
{
	{
		VDONinjaSource source(NativeMediaTestTag{});
		VDONinjaPeerManager manager;
		source.bindNativeMediaTestPeerManager(manager);
		const std::string uuid = "stream-sessionless-reuse";
		const std::string streamId = "stream-sessionless-reuse-id";
		source.setNativeMediaTestStreamId(streamId);
		auto oldPeer = manager.createNativeMediaTestViewerPeer(uuid, "session-a");
		oldPeer->streamId = streamId;
		manager.retireNativeMediaTestPeer(oldPeer);
		auto currentPeer = manager.createNativeMediaTestViewerPeer(uuid, "session-b");
		currentPeer->streamId = streamId;
		auto currentPrimary = addVp9ReceiveTrack(currentPeer, "video");
		manager.receiveNativeMediaTestTrack(currentPeer, currentPrimary);
		auto currentDataChannel = currentPeer->pc->createDataChannel("sendChannel");
		manager.receiveNativeMediaTestDataChannel(currentPeer, currentDataChannel);
		source.resetNativeMediaTestState();
		auto postResetPrimary = makeVp9ReceiveTrack("video");
		manager.receiveNativeMediaTestTrack(currentPeer, postResetPrimary.track);
		SignalingLifecycleEvent ambiguousRemoval;
		ambiguousRemoval.kind = SignalingLifecycleEventKind::StreamRemoved;
		ambiguousRemoval.socketEpoch = 52;
		ambiguousRemoval.wsSequence = 1;
		ambiguousRemoval.uuid = uuid;
		ambiguousRemoval.streamId = streamId;
		source.applyNativeMediaTestSignalingLifecycleEvent(manager, ambiguousRemoval);
		bool managerMediaSurvived = false;
		{
			std::lock_guard<std::mutex> mediaLock(currentPeer->mediaMutex);
			managerMediaSurvived =
			    currentPeer->videoTrack == postResetPrimary.track && currentPeer->dataChannel == currentDataChannel;
		}
		const auto snapshot = source.nativeMediaTestSnapshot();
		require(manager.getPeerIdentity(uuid) && managerMediaSurvived &&
		            source.nativeMediaTestTrackSnapshot().video == postResetPrimary.track.get() &&
		            snapshot.acceptedPeerCleanups == 0 && snapshot.peerRetrySchedules == 0 &&
		            snapshot.ambiguousSessionlessCleanups == 1 && snapshot.legacyStreamRemovalActions == 0,
		        "ambiguous sessionless streamRemoved rebound onto a reused UUID generation");
	}
}

void testUuidlessStreamRemovedCannotRebindCurrentOwner()
{
	{
		VDONinjaSource source(NativeMediaTestTag{});
		VDONinjaPeerManager manager;
		source.bindNativeMediaTestPeerManager(manager);
		const std::string streamId = "stream-only-removal";
		source.setNativeMediaTestStreamId(streamId);
		auto oldOwner = manager.createNativeMediaTestViewerPeer("stream-only-old-owner", "session-old");
		oldOwner->streamId = streamId;
		manager.retireNativeMediaTestPeer(oldOwner);
		auto currentPeer = manager.createNativeMediaTestViewerPeer("stream-only-current", "session-current");
		currentPeer->streamId = streamId;
		auto currentPrimary = addVp9ReceiveTrack(currentPeer, "video");
		manager.receiveNativeMediaTestTrack(currentPeer, currentPrimary);
		SignalingLifecycleEvent streamOnlyRemoval;
		streamOnlyRemoval.kind = SignalingLifecycleEventKind::StreamRemoved;
		streamOnlyRemoval.socketEpoch = 53;
		streamOnlyRemoval.wsSequence = 1;
		streamOnlyRemoval.streamId = streamId;
		source.applyNativeMediaTestSignalingLifecycleEvent(manager, streamOnlyRemoval);
		const auto snapshot = source.nativeMediaTestSnapshot();
		require(manager.getPeerIdentity(currentPeer->uuid) &&
		            source.nativeMediaTestTrackSnapshot().video == currentPrimary.get() &&
		            snapshot.acceptedPeerCleanups == 0 && snapshot.peerRetrySchedules == 0 &&
		            snapshot.legacyStreamRemovalActions == 0,
		        "UUID-less stream-only removal rebound to and erased the current stream owner");
	}
}

void testStaleSocketEpochStreamRemovedRejectedAfterSourceReset()
{
	VDONinjaSource source(NativeMediaTestTag{});
	VDONinjaPeerManager manager;
	source.bindNativeMediaTestPeerManager(manager);
	const std::string uuid = "stream-socket-epoch";
	const std::string streamId = "stream-socket-epoch-id";
	source.setNativeMediaTestStreamId(streamId);
	auto peer = manager.createNativeMediaTestViewerPeer(uuid, "session-current");
	peer->streamId = streamId;
	auto originalPrimary = addVp9ReceiveTrack(peer, "video");
	manager.receiveNativeMediaTestTrack(peer, originalPrimary);

	SignalingLifecycleEvent newerSocketHint;
	newerSocketHint.kind = SignalingLifecycleEventKind::StreamRemoved;
	newerSocketHint.socketEpoch = 62;
	newerSocketHint.wsSequence = 10;
	newerSocketHint.uuid = "unrelated-peer";
	newerSocketHint.streamId = "unrelated-stream";
	source.applyNativeMediaTestSignalingLifecycleEvent(manager, newerSocketHint);

	source.resetNativeMediaTestState();
	auto currentPrimary = makeVp9ReceiveTrack("video");
	manager.receiveNativeMediaTestTrack(peer, currentPrimary.track);
	require(source.nativeMediaTestTrackSnapshot().video == currentPrimary.track.get(),
	        "source reset fixture did not republish current manager media");

	SignalingLifecycleEvent staleRemoval;
	staleRemoval.kind = SignalingLifecycleEventKind::StreamRemoved;
	staleRemoval.socketEpoch = 62;
	staleRemoval.wsSequence = 9;
	staleRemoval.uuid = uuid;
	staleRemoval.session = peer->session;
	staleRemoval.streamId = streamId;
	source.applyNativeMediaTestSignalingLifecycleEvent(manager, staleRemoval);
	staleRemoval.socketEpoch = 61;
	staleRemoval.wsSequence = 99;
	source.applyNativeMediaTestSignalingLifecycleEvent(manager, staleRemoval);
	const auto identity = manager.getPeerIdentity(uuid);
	const auto snapshot = source.nativeMediaTestSnapshot();
	require(identity && identity->generation == peer->generation &&
	            source.nativeMediaTestTrackSnapshot().video == currentPrimary.track.get() &&
	            snapshot.acceptedPeerCleanups == 0 && snapshot.peerRetrySchedules == 0 &&
	            snapshot.legacyStreamRemovalActions == 0,
	        "stale socket-epoch streamRemoved acted after source reset erased local reducer history");
}

void testDelayedDataChannelOpenCannotActOnReplacementGeneration()
{
	VDONinjaSource source(NativeMediaTestTag{});
	VDONinjaPeerManager manager;
	source.bindNativeMediaTestPeerManager(manager);
	const std::string uuid = "data-open-rotate";
	auto oldPeer = manager.createNativeMediaTestViewerPeer(uuid, "session-a");
	auto oldPrimary = addVp9ReceiveTrack(oldPeer, "video");
	manager.receiveNativeMediaTestTrack(oldPeer, oldPrimary);
	auto oldDataChannel = oldPeer->pc->createDataChannel("sendChannel");
	manager.receiveNativeMediaTestDataChannel(oldPeer, oldDataChannel);

	PeerDispatchLatch latch(oldPeer->generation);
	manager.setNativeMediaTestPeerDataOpenDispatchHook(
	    [&latch](const std::shared_ptr<PeerInfo> &peer) { latch.hook(peer); });
	std::thread delayedOpen([&]() { manager.dispatchNativeMediaTestDataChannelOpen(oldPeer, oldDataChannel); });
	latch.waitUntilEntered();
	manager.retireNativeMediaTestPeer(oldPeer);
	auto newPeer = manager.createNativeMediaTestViewerPeer(uuid, "session-b");
	auto newPrimary = addVp9ReceiveTrack(newPeer, "video");
	manager.receiveNativeMediaTestTrack(newPeer, newPrimary);
	latch.release();
	delayedOpen.join();
	manager.setNativeMediaTestPeerDataOpenDispatchHook(nullptr);
	require(source.nativeMediaTestSnapshot().dataChannelOpenActions == 0,
	        "delayed old-generation DataChannel open acted on the replacement UUID");

	auto newDataChannel = newPeer->pc->createDataChannel("sendChannel");
	manager.receiveNativeMediaTestDataChannel(newPeer, newDataChannel);
	manager.dispatchNativeMediaTestDataChannelOpen(newPeer, newDataChannel);
	require(source.nativeMediaTestSnapshot().dataChannelOpenActions == 1,
	        "current-generation DataChannel open did not perform its preference action exactly once");
}

void testInboundDataChannelDispatchWaitsForRealOpenAndDeliversAlphaPreference()
{
	VDONinjaSource source(NativeMediaTestTag{});
	VDONinjaPeerManager manager;
	source.bindNativeMediaTestPeerManager(manager);

	std::atomic<int> dispatchAttempts{0};
	std::atomic<bool> incomingHandlerReturning{false};
	std::atomic<bool> openCommittedBeforeIncomingReturn{false};
	std::atomic<bool> everyDispatchFollowedIncomingReturn{true};
	manager.setNativeMediaTestDataChannelLifecycleHook([&](NativeMediaTestDataChannelStage stage,
	                                                       const std::shared_ptr<PeerInfo> &,
	                                                       const std::shared_ptr<rtc::DataChannel> &, uint64_t) {
		if (stage != NativeMediaTestDataChannelStage::IncomingReturning) {
			return;
		}
		if (source.nativeMediaTestSnapshot().dataChannelOpenActions != 0) {
			openCommittedBeforeIncomingReturn.store(true, std::memory_order_release);
		}
		incomingHandlerReturning.store(true, std::memory_order_release);
	});
	manager.setNativeMediaTestPeerDataOpenDispatchHook(
	    [&dispatchAttempts, &incomingHandlerReturning,
	     &everyDispatchFollowedIncomingReturn](const std::shared_ptr<PeerInfo> &) {
		    dispatchAttempts.fetch_add(1, std::memory_order_acq_rel);
		    if (!incomingHandlerReturning.load(std::memory_order_acquire)) {
			    everyDispatchFollowedIncomingReturn.store(false, std::memory_order_release);
		    }
	    });

	auto pair = makeConnectedManagerDataChannelPair(manager, "dc-real-open", {"sendChannel"});
	std::string sourcePreferences;
	requireEventually(
	    [&]() {
		    std::lock_guard<std::mutex> lock(pair.state->mutex);
		    const auto found = std::find_if(pair.state->messages.begin(), pair.state->messages.end(),
		                                    [](const auto &message) { return message.first == "sendChannel"; });
		    if (found == pair.state->messages.end()) {
			    return false;
		    }
		    sourcePreferences = found->second;
		    return true;
	    },
	    "source-produced viewer preferences did not traverse the real paired DataChannel", 2s);
	manager.setNativeMediaTestPeerDataOpenDispatchHook(nullptr);
	manager.setNativeMediaTestDataChannelLifecycleHook(nullptr);
	const JsonParser request(sourcePreferences);
	const JsonParser info(request.getObject("info"));
	require(info.getString("alpha_receive") == "vp9-dualtrack-v1",
	        "source-produced viewer preferences lost the Game Capture dual-track alpha capability");
	bool managerObservedExactOpen = false;
	{
		std::lock_guard<std::mutex> mediaLock(pair.receiver->mediaMutex);
		managerObservedExactOpen = pair.receiver->dataChannelOpenDispatched;
	}
	require(incomingHandlerReturning.load(std::memory_order_acquire) &&
	            !openCommittedBeforeIncomingReturn.load(std::memory_order_acquire) &&
	            dispatchAttempts.load(std::memory_order_acquire) == 1 &&
	            everyDispatchFollowedIncomingReturn.load(std::memory_order_acquire) &&
	            source.nativeMediaTestSnapshot().dataChannelOpenActions == 1 && managerObservedExactOpen,
	        "sendChannel dispatch was not committed by its exact manager-owned onOpen lifecycle callback");
}

void testInboundControlRoutingRejectsNonSendChannelLabels()
{
	VDONinjaSource source(NativeMediaTestTag{});
	VDONinjaPeerManager manager;
	source.bindNativeMediaTestPeerManager(manager);
	std::atomic<int> messageDispatches{0};
	manager.setNativeMediaTestPeerDataDispatchHook([&messageDispatches](const std::shared_ptr<PeerInfo> &) {
		messageDispatches.fetch_add(1, std::memory_order_acq_rel);
	});
	const std::vector<std::string> labels = {"chunked", "resources", "sendChannel", "x-meta", "file", "arbitrary"};
	auto pair = makeConnectedManagerDataChannelPair(manager, "dc-label-routing", labels);

	requireEventually([&]() { return source.nativeMediaTestSnapshot().dataChannelOpenActions >= 1; },
	                  "authoritative sendChannel did not dispatch an open event");
	std::this_thread::sleep_for(100ms);
	require(source.nativeMediaTestSnapshot().dataChannelOpenActions == 1,
	        "non-control DataChannel labels entered the VDO.Ninja control path");
	std::shared_ptr<rtc::DataChannel> installed;
	{
		std::lock_guard<std::mutex> mediaLock(pair.receiver->mediaMutex);
		installed = pair.receiver->dataChannel;
	}
	require(installed && installed->label() == "sendChannel",
	        "a non-control DataChannel replaced the authoritative sendChannel slot");
	for (size_t index = 0; index < pair.senderChannels.size(); ++index) {
		try {
			const bool sent = pair.senderChannels[index]->send("probe-" + labels[index]);
			if (labels[index] == "sendChannel") {
				require(sent, "failed to send authoritative sendChannel routing probe");
			}
		} catch (const std::exception &) {
			require(labels[index] != "sendChannel", "authoritative sendChannel closed while sending its routing probe");
		}
	}
	requireEventually([&]() { return messageDispatches.load(std::memory_order_acquire) >= 1; },
	                  "authoritative sendChannel message did not reach control routing");
	std::this_thread::sleep_for(100ms);
	manager.setNativeMediaTestPeerDataDispatchHook(nullptr);
	require(messageDispatches.load(std::memory_order_acquire) == 1,
	        "non-control DataChannel message entered the VDO.Ninja control parser");
}

void testEmptyLabelLegacyControlChannelRemainsCompatible()
{
	VDONinjaSource source(NativeMediaTestTag{});
	VDONinjaPeerManager manager;
	source.bindNativeMediaTestPeerManager(manager);
	auto pair = makeConnectedManagerDataChannelPair(manager, "dc-empty-legacy", {""});
	requireEventually(
	    [&]() {
		    std::lock_guard<std::mutex> lock(pair.state->mutex);
		    return !pair.state->messages.empty();
	    },
	    "empty-label legacy control channel did not receive source preferences");
	std::string preferences;
	{
		std::lock_guard<std::mutex> lock(pair.state->mutex);
		preferences = pair.state->messages.front().second;
	}
	const JsonParser request(preferences);
	const JsonParser info(request.getObject("info"));
	require(source.nativeMediaTestSnapshot().dataChannelOpenActions == 1 &&
	            info.getString("alpha_receive") == "vp9-dualtrack-v1",
	        "empty-label VDO.Ninja legacy control routing lost source alpha preferences");
}

void testDataChannelExactLeaseRejectsStaleMessagesAndClearsCurrentClose()
{
	VDONinjaPeerManager manager;
	std::mutex messageMutex;
	std::vector<std::string> messages;
	std::mutex incomingMutex;
	std::condition_variable incomingCondition;
	std::vector<std::shared_ptr<rtc::DataChannel>> incomingChannels;
	manager.setOnDataChannelMessage([&](const PeerEventIdentity &, const std::string &message) {
		std::lock_guard<std::mutex> lock(messageMutex);
		messages.push_back(message);
	});
	manager.setNativeMediaTestDataChannelLifecycleHook([&](NativeMediaTestDataChannelStage stage,
	                                                       const std::shared_ptr<PeerInfo> &,
	                                                       const std::shared_ptr<rtc::DataChannel> &channel, uint64_t) {
		if (stage == NativeMediaTestDataChannelStage::IncomingEntered) {
			std::lock_guard<std::mutex> lock(incomingMutex);
			incomingChannels.push_back(channel);
			incomingCondition.notify_all();
		}
	});
	auto pair = makeConnectedManagerDataChannelPair(manager, "dc-exact-lease", {"sendChannel", "sendChannel"});
	require(pair.senderChannels.size() == 2, "exact DataChannel lease gate did not create both channels");
	{
		std::unique_lock<std::mutex> lock(incomingMutex);
		require(incomingCondition.wait_for(lock, 10s, [&]() { return incomingChannels.size() == 2; }),
		        "manager did not expose both inbound DataChannel handles");
	}
	manager.setNativeMediaTestDataChannelLifecycleHook(nullptr);

	std::optional<uint16_t> currentId;
	{
		std::lock_guard<std::mutex> mediaLock(pair.receiver->mediaMutex);
		currentId = pair.receiver->dataChannel ? pair.receiver->dataChannel->id() : std::nullopt;
	}
	require(currentId.has_value(), "current inbound DataChannel had no negotiated stream id");
	size_t currentIndex = pair.senderChannels.size();
	for (size_t index = 0; index < pair.senderChannels.size(); ++index) {
		if (pair.senderChannels[index]->id() == currentId) {
			currentIndex = index;
			break;
		}
	}
	require(currentIndex < pair.senderChannels.size(), "could not map manager DataChannel lease to its sender handle");
	const size_t staleIndex = currentIndex == 0 ? 1 : 0;

	require(pair.senderChannels[staleIndex]->send(std::string("stale-dc")), "failed to send stale DataChannel probe");
	require(pair.senderChannels[currentIndex]->send(std::string("current-dc")),
	        "failed to send current DataChannel probe");
	requireEventually(
	    [&]() {
		    std::lock_guard<std::mutex> lock(messageMutex);
		    return std::find(messages.begin(), messages.end(), "current-dc") != messages.end();
	    },
	    "current DataChannel message did not reach the control callback");
	std::this_thread::sleep_for(100ms);
	{
		std::lock_guard<std::mutex> lock(messageMutex);
		require(std::find(messages.begin(), messages.end(), "stale-dc") == messages.end(),
		        "replaced DataChannel retained a live message callback");
	}

	auto mediaTrack = makeVp9ReceiveTrack("video");
	manager.receiveNativeMediaTestTrack(pair.receiver, mediaTrack.track);
	pair.senderChannels[staleIndex]->close();
	std::this_thread::sleep_for(100ms);
	{
		std::lock_guard<std::mutex> mediaLock(pair.receiver->mediaMutex);
		require(pair.receiver->hasDataChannel && pair.receiver->dataChannel &&
		            pair.receiver->videoTrack == mediaTrack.track,
		        "stale DataChannel close cleared the current channel or healthy media");
	}
	pair.senderChannels[currentIndex]->close();
	requireEventually(
	    [&]() {
		    std::lock_guard<std::mutex> mediaLock(pair.receiver->mediaMutex);
		    return !pair.receiver->hasDataChannel && !pair.receiver->dataChannel;
	    },
	    "current DataChannel close did not clear its exact control slot", 2s);
	{
		std::lock_guard<std::mutex> mediaLock(pair.receiver->mediaMutex);
		require(pair.receiver->videoTrack == mediaTrack.track,
		        "current DataChannel close incorrectly retired a healthy media Track");
	}
}

void testDataChannelCloseDuringCallbackInstallReplaysTerminalState()
{
	VDONinjaSource source(NativeMediaTestTag{});
	VDONinjaPeerManager manager;
	source.bindNativeMediaTestPeerManager(manager);
	auto peer = manager.createNativeMediaTestViewerPeer("dc-close-install");
	auto owned = makeUnconnectedDataChannel("sendChannel");
	DataChannelStageLatch latch(NativeMediaTestDataChannelStage::BeforeCallbacksInstalled, owned.channel.get());
	manager.setNativeMediaTestDataChannelLifecycleHook(
	    [&latch](NativeMediaTestDataChannelStage stage, const std::shared_ptr<PeerInfo> &,
	             const std::shared_ptr<rtc::DataChannel> &channel, uint64_t) { latch.hook(stage, channel); });
	std::thread installing([&]() { manager.receiveNativeMediaTestDataChannel(peer, owned.channel); });
	latch.waitUntilEntered();
	owned.channel->close();
	latch.release();
	installing.join();
	manager.setNativeMediaTestDataChannelLifecycleHook(nullptr);

	const bool retired = waitUntil(
	    [&]() {
		    std::lock_guard<std::mutex> mediaLock(peer->mediaMutex);
		    return !peer->hasDataChannel && !peer->dataChannel && !peer->dataChannelOpenDispatched;
	    },
	    2s);
	require(retired && source.nativeMediaTestSnapshot().dataChannelOpenActions == 0,
	        "DataChannel closed during callback installation was published as a live control channel");
}

void testDataChannelPreInstallReplacementCannotOverwriteNewerLease()
{
	VDONinjaSource source(NativeMediaTestTag{});
	VDONinjaPeerManager manager;
	source.bindNativeMediaTestPeerManager(manager);
	auto peer = manager.createNativeMediaTestViewerPeer("dc-preinstall-replace");
	auto stale = makeUnconnectedDataChannel("sendChannel");
	auto current = makeUnconnectedDataChannel("sendChannel");
	DataChannelStageLatch latch(NativeMediaTestDataChannelStage::BeforeCallbacksInstalled, stale.channel.get());
	manager.setNativeMediaTestDataChannelLifecycleHook(
	    [&latch](NativeMediaTestDataChannelStage stage, const std::shared_ptr<PeerInfo> &,
	             const std::shared_ptr<rtc::DataChannel> &channel, uint64_t) { latch.hook(stage, channel); });
	std::thread delayedStale([&]() { manager.receiveNativeMediaTestDataChannel(peer, stale.channel); });
	latch.waitUntilEntered();
	manager.receiveNativeMediaTestDataChannel(peer, current.channel);
	latch.release();
	delayedStale.join();
	manager.setNativeMediaTestDataChannelLifecycleHook(nullptr);

	std::lock_guard<std::mutex> mediaLock(peer->mediaMutex);
	require(peer->dataChannel == current.channel && peer->hasDataChannel && peer->dataChannelRevision >= 2 &&
	            !peer->dataChannelOpenDispatched && source.nativeMediaTestSnapshot().dataChannelOpenActions == 0,
	        "stale pre-install DataChannel overwrote the newer exact lease");
}

void testDuplicateDataChannelDeliveryDoesNotReinstallOrBumpLease()
{
	VDONinjaSource source(NativeMediaTestTag{});
	VDONinjaPeerManager manager;
	source.bindNativeMediaTestPeerManager(manager);
	auto peer = manager.createNativeMediaTestViewerPeer("dc-duplicate-handle");
	auto owned = makeUnconnectedDataChannel("sendChannel");
	manager.receiveNativeMediaTestDataChannel(peer, owned.channel);
	uint64_t firstRevision = 0;
	{
		std::lock_guard<std::mutex> mediaLock(peer->mediaMutex);
		firstRevision = peer->dataChannelRevision;
	}
	manager.receiveNativeMediaTestDataChannel(peer, owned.channel);
	std::lock_guard<std::mutex> mediaLock(peer->mediaMutex);
	require(firstRevision > 0 && peer->dataChannelRevision == firstRevision && peer->dataChannel == owned.channel &&
	            source.nativeMediaTestSnapshot().dataChannelOpenActions == 0,
	        "duplicate delivery of one DataChannel bumped/reinstalled its live exact lease");
}

void testPublisherDataChannelInstallsBeforePeerRegistration()
{
	VDONinjaPeerManager manager;
	std::atomic<int> errorCallbackInstalls{0};
	manager.setNativeMediaTestDataChannelLifecycleHook(
	    [&errorCallbackInstalls](NativeMediaTestDataChannelStage stage, const std::shared_ptr<PeerInfo> &,
	                             const std::shared_ptr<rtc::DataChannel> &, uint64_t) {
		    if (stage == NativeMediaTestDataChannelStage::AfterErrorCallbackInstalled) {
			    errorCallbackInstalls.fetch_add(1, std::memory_order_acq_rel);
		    }
	    });
	auto peer = manager.createNativeMediaTestPublisherPeer("dc-publisher-pre-register");
	manager.setNativeMediaTestDataChannelLifecycleHook(nullptr);
	std::shared_ptr<rtc::DataChannel> channel;
	uint64_t revision = 0;
	{
		std::lock_guard<std::mutex> mediaLock(peer->mediaMutex);
		channel = peer->dataChannel;
		revision = peer->dataChannelRevision;
	}
	require(channel && channel->label() == "sendChannel" && revision > 0 &&
	            errorCallbackInstalls.load(std::memory_order_acquire) == 1,
	        "publisher sendChannel lost callbacks while setup preceded peer registration");
	std::atomic<int> openCallbacks{0};
	manager.setOnDataChannel([&](const PeerEventIdentity &, std::shared_ptr<rtc::DataChannel>) {
		openCallbacks.fetch_add(1, std::memory_order_acq_rel);
	});
	manager.dispatchNativeMediaTestDataChannelOpen(peer, channel);
	require(openCallbacks.load(std::memory_order_acquire) == 1,
	        "registered publisher sendChannel did not retain its pre-registration exact lease");
}

void testSynchronousOpenReplaySubscriberDoesNotHoldLifecycleLock()
{
	VDONinjaPeerManager manager;
	auto peer = manager.createNativeMediaTestViewerPeer("dc-sync-open-replay");
	auto pair = makeConnectedUnmanagedDataChannelPair("sendChannel");
	auto replacement = makeUnconnectedDataChannel("sendChannel");
	std::thread replacing;
	std::atomic<bool> subscriberRan{false};
	std::atomic<bool> replacementCommittedBeforeReturn{false};
	manager.setOnDataChannel([&](const PeerEventIdentity &, const std::shared_ptr<rtc::DataChannel> &channel) {
		if (channel != pair.receiver || subscriberRan.exchange(true, std::memory_order_acq_rel)) {
			return;
		}
		replacing = std::thread([&]() { manager.receiveNativeMediaTestDataChannel(peer, replacement.channel); });
		replacementCommittedBeforeReturn.store(waitUntil(
		                                           [&]() {
			                                           std::lock_guard<std::mutex> mediaLock(peer->mediaMutex);
			                                           return peer->dataChannel == replacement.channel;
		                                           },
		                                           500ms),
		                                       std::memory_order_release);
	});
	manager.receiveNativeMediaTestDataChannel(peer, pair.receiver);
	if (replacing.joinable()) {
		replacing.join();
	}
	require(subscriberRan.load(std::memory_order_acquire) &&
	            replacementCommittedBeforeReturn.load(std::memory_order_acquire),
	        "synchronously replayed onOpen subscriber ran under the DataChannel lifecycle lock");
}

void testSynchronousQueuedMessageReplaySubscriberDoesNotHoldLifecycleLock()
{
	VDONinjaPeerManager manager;
	auto peer = manager.createNativeMediaTestViewerPeer("dc-sync-message-replay");
	auto pair = makeConnectedUnmanagedDataChannelPair("sendChannel");
	manager.receiveNativeMediaTestDataChannel(peer, pair.receiver);
	auto parked = makeUnconnectedDataChannel("sendChannel");
	manager.receiveNativeMediaTestDataChannel(peer, parked.channel);
	require(pair.sender->send(std::string("queued-before-re-adopt")),
	        "failed to queue DataChannel message while manager callbacks were absent");
	auto replacement = makeUnconnectedDataChannel("sendChannel");
	std::thread replacing;
	std::atomic<bool> subscriberRan{false};
	std::atomic<bool> replacementCommittedBeforeReturn{false};
	manager.setOnDataChannelMessage([&](const PeerEventIdentity &, const std::string &message) {
		if (message != "queued-before-re-adopt" || subscriberRan.exchange(true, std::memory_order_acq_rel)) {
			return;
		}
		replacing = std::thread([&]() { manager.receiveNativeMediaTestDataChannel(peer, replacement.channel); });
		replacementCommittedBeforeReturn.store(waitUntil(
		                                           [&]() {
			                                           std::lock_guard<std::mutex> mediaLock(peer->mediaMutex);
			                                           return peer->dataChannel == replacement.channel;
		                                           },
		                                           500ms),
		                                       std::memory_order_release);
	});
	manager.receiveNativeMediaTestDataChannel(peer, pair.receiver);
	if (replacing.joinable()) {
		replacing.join();
	}
	require(subscriberRan.load(std::memory_order_acquire) &&
	            replacementCommittedBeforeReturn.load(std::memory_order_acquire),
	        "synchronously flushed onMessage subscriber ran under the DataChannel lifecycle lock");
}

void testDataChannelDeferredReplayDrainsInOpenThenMessageOrder()
{
	VDONinjaPeerManager manager;
	auto peer = manager.createNativeMediaTestViewerPeer("dc-replay-drain-order");
	auto pair = makeConnectedUnmanagedDataChannelPair("sendChannel", true);
	require(pair.signalingRelay && pair.signalingRelay->firstCandidateWasQueuedBeforeDescription(),
	        "deferred replay gate did not force candidate arrival before remote description");
	require(pair.connectionAttempts >= 1 && pair.connectionAttempts <= 3,
	        "deferred replay gate lost its bounded local RTC setup attempt count");

	std::mutex eventMutex;
	std::vector<std::string> events;
	manager.setOnDataChannel([&](const PeerEventIdentity &, const std::shared_ptr<rtc::DataChannel> &) {
		std::lock_guard<std::mutex> lock(eventMutex);
		events.push_back("open");
	});
	manager.setOnDataChannelMessage([&](const PeerEventIdentity &, const std::string &message) {
		std::lock_guard<std::mutex> lock(eventMutex);
		events.push_back(message);
	});

	std::mutex drainMutex;
	std::condition_variable drainCondition;
	bool callbackQueuedDuringDrain = false;
	bool injectedDuringDrain = false;
	manager.setNativeMediaTestDataChannelLifecycleHook([&](NativeMediaTestDataChannelStage stage,
	                                                       const std::shared_ptr<PeerInfo> &,
	                                                       const std::shared_ptr<rtc::DataChannel> &, uint64_t) {
		if (stage == NativeMediaTestDataChannelStage::DeferredCallbackQueuedDuringDrain) {
			std::lock_guard<std::mutex> lock(drainMutex);
			callbackQueuedDuringDrain = true;
			drainCondition.notify_all();
			return;
		}
		if (stage != NativeMediaTestDataChannelStage::BeforeDeferredCallbacksDrained || injectedDuringDrain) {
			return;
		}
		injectedDuringDrain = true;
		require(pair.sender->send(std::string("queued-during-drain")),
		        "failed to inject DataChannel callback during deferred drain transition");
		std::unique_lock<std::mutex> lock(drainMutex);
		require(drainCondition.wait_for(lock, 2s, [&]() { return callbackQueuedDuringDrain; }),
		        "post-install DataChannel callback did not join the active drain queue");
	});
	manager.receiveNativeMediaTestDataChannel(peer, pair.receiver);
	manager.setNativeMediaTestDataChannelLifecycleHook(nullptr);

	std::lock_guard<std::mutex> lock(eventMutex);
	std::string observedOrder;
	for (const auto &event : events) {
		observedOrder += (observedOrder.empty() ? "" : ",") + event;
	}
	require(injectedDuringDrain && callbackQueuedDuringDrain &&
	            events == std::vector<std::string>({"open", "queued-during-drain"}),
	        "DataChannel callback arriving during replay overtook open or an earlier queued message: " + observedOrder);
}

void testRejectedOpenDispatchDoesNotPoisonSameHandleReAdoption()
{
	VDONinjaPeerManager manager;
	auto peer = manager.createNativeMediaTestViewerPeer("dc-rejected-open-readd");
	auto first = makeConnectedUnmanagedDataChannelPair("sendChannel");
	auto replacement = makeUnconnectedDataChannel("sendChannel");
	std::atomic<int> openSubscribers{0};
	manager.setOnDataChannel([&](const PeerEventIdentity &, const std::shared_ptr<rtc::DataChannel> &) {
		openSubscribers.fetch_add(1, std::memory_order_acq_rel);
	});
	PeerDispatchLatch latch(peer->generation);
	manager.setNativeMediaTestPeerDataOpenDispatchHook(
	    [&latch](const std::shared_ptr<PeerInfo> &hookPeer) { latch.hook(hookPeer); });
	std::thread staleOpen([&]() { manager.receiveNativeMediaTestDataChannel(peer, first.receiver); });
	latch.waitUntilEntered();
	std::thread replacing([&]() { manager.receiveNativeMediaTestDataChannel(peer, replacement.channel); });
	const bool replacementCommitted = waitUntil(
	    [&]() {
		    std::lock_guard<std::mutex> mediaLock(peer->mediaMutex);
		    return peer->dataChannel == replacement.channel;
	    },
	    500ms);
	latch.release();
	staleOpen.join();
	replacing.join();
	manager.setNativeMediaTestPeerDataOpenDispatchHook(nullptr);
	require(replacementCommitted && openSubscribers.load(std::memory_order_acquire) == 0,
	        "replaced DataChannel reached the open subscriber before re-adoption");

	manager.receiveNativeMediaTestDataChannel(peer, first.receiver);
	requireEventually([&]() { return openSubscribers.load(std::memory_order_acquire) == 1; },
	                  "rejected real open dispatch poisoned automatic same-handle re-adoption");
	std::this_thread::sleep_for(25ms);
	{
		std::lock_guard<std::mutex> mediaLock(peer->mediaMutex);
		require(peer->dataChannel == first.receiver && peer->dataChannelOpenDispatched &&
		            openSubscribers.load(std::memory_order_acquire) == 1,
		        "real same-handle re-adoption did not commit exactly one current open subscriber");
	}
}

void testDataChannelStaleOpenAndErrorCannotMutateCurrentLease()
{
	VDONinjaPeerManager manager;
	auto peer = manager.createNativeMediaTestViewerPeer("dc-stale-terminal");
	auto stale = makeUnconnectedDataChannel("sendChannel");
	auto current = makeUnconnectedDataChannel("sendChannel");
	std::atomic<int> errorCallbackInstalls{0};
	manager.setNativeMediaTestDataChannelLifecycleHook(
	    [&errorCallbackInstalls](NativeMediaTestDataChannelStage stage, const std::shared_ptr<PeerInfo> &,
	                             const std::shared_ptr<rtc::DataChannel> &, uint64_t) {
		    if (stage == NativeMediaTestDataChannelStage::AfterErrorCallbackInstalled) {
			    errorCallbackInstalls.fetch_add(1, std::memory_order_acq_rel);
		    }
	    });
	manager.receiveNativeMediaTestDataChannel(peer, stale.channel);
	manager.receiveNativeMediaTestDataChannel(peer, current.channel);
	manager.setNativeMediaTestDataChannelLifecycleHook(nullptr);
	require(errorCallbackInstalls.load(std::memory_order_acquire) == 2,
	        "manager did not install an exact-leased onError callback on each DataChannel");
	auto mediaTrack = makeVp9ReceiveTrack("video");
	manager.receiveNativeMediaTestTrack(peer, mediaTrack.track);

	std::atomic<int> openCallbacks{0};
	manager.setOnDataChannel([&](const PeerEventIdentity &, std::shared_ptr<rtc::DataChannel>) {
		openCallbacks.fetch_add(1, std::memory_order_acq_rel);
	});
	manager.dispatchNativeMediaTestDataChannelOpen(peer, stale.channel);
	manager.dispatchNativeMediaTestDataChannelOpen(peer, current.channel);
	require(openCallbacks.load(std::memory_order_acquire) == 1,
	        "stale same-generation DataChannel open reached the application callback");

	manager.dispatchNativeMediaTestDataChannelError(peer, stale.channel, "stale-error");
	{
		std::lock_guard<std::mutex> mediaLock(peer->mediaMutex);
		require(peer->dataChannel == current.channel && peer->hasDataChannel && peer->videoTrack == mediaTrack.track,
		        "stale DataChannel error cleared the current channel or healthy media");
	}
	manager.dispatchNativeMediaTestDataChannelError(peer, current.channel, "current-error");
	requireEventually(
	    [&]() {
		    std::lock_guard<std::mutex> mediaLock(peer->mediaMutex);
		    return !peer->dataChannel && !peer->hasDataChannel;
	    },
	    "current DataChannel error did not clear its exact slot", 2s);
	uint64_t terminalRevision = 0;
	{
		std::lock_guard<std::mutex> mediaLock(peer->mediaMutex);
		terminalRevision = peer->dataChannelRevision;
		require(peer->videoTrack == mediaTrack.track && current.channel->isClosed(),
		        "current DataChannel error failed to close exact channel or retired healthy media");
	}
	current.channel->close();
	std::this_thread::sleep_for(100ms);
	{
		std::lock_guard<std::mutex> mediaLock(peer->mediaMutex);
		require(peer->dataChannelRevision == terminalRevision && peer->videoTrack == mediaTrack.track,
		        "DataChannel error followed by close retired the exact lease more than once");
	}
}

void testPausedDataChannelDispatchRevalidatesExactLeaseBeforeSubscriber()
{
	{
		VDONinjaPeerManager manager;
		auto peer = manager.createNativeMediaTestViewerPeer("dc-paused-open");
		auto stale = makeUnconnectedDataChannel("sendChannel");
		auto current = makeUnconnectedDataChannel("sendChannel");
		manager.receiveNativeMediaTestDataChannel(peer, stale.channel);
		std::atomic<int> openCallbacks{0};
		manager.setOnDataChannel([&](const PeerEventIdentity &, std::shared_ptr<rtc::DataChannel>) {
			openCallbacks.fetch_add(1, std::memory_order_acq_rel);
		});
		PeerDispatchLatch latch(peer->generation);
		manager.setNativeMediaTestPeerDataOpenDispatchHook(
		    [&latch](const std::shared_ptr<PeerInfo> &hookPeer) { latch.hook(hookPeer); });
		std::thread delayedOpen([&]() { manager.dispatchNativeMediaTestDataChannelOpen(peer, stale.channel); });
		latch.waitUntilEntered();
		manager.receiveNativeMediaTestDataChannel(peer, current.channel);
		latch.release();
		delayedOpen.join();
		manager.setNativeMediaTestPeerDataOpenDispatchHook(nullptr);
		require(openCallbacks.load(std::memory_order_acquire) == 0,
		        "DC1 open subscriber ran after DC2 replaced its exact lease");
		manager.dispatchNativeMediaTestDataChannelOpen(peer, current.channel);
		require(openCallbacks.load(std::memory_order_acquire) == 1,
		        "current DC2 open subscriber was lost after stale dispatch rejection");
	}

	{
		VDONinjaPeerManager manager;
		auto pair = makeConnectedManagerDataChannelPair(manager, "dc-paused-message", {"sendChannel"});
		std::atomic<int> messageCallbacks{0};
		std::atomic<bool> dispatchCompleted{false};
		manager.setOnDataChannelMessage([&](const PeerEventIdentity &, const std::string &) {
			messageCallbacks.fetch_add(1, std::memory_order_acq_rel);
		});
		PeerDispatchLatch latch(pair.receiver->generation);
		manager.setNativeMediaTestPeerDataDispatchHook(
		    [&latch](const std::shared_ptr<PeerInfo> &hookPeer) { latch.hook(hookPeer); });
		manager.setNativeMediaTestPeerDataDispatchCompleteHook([&dispatchCompleted](const std::shared_ptr<PeerInfo> &) {
			dispatchCompleted.store(true, std::memory_order_release);
		});
		require(pair.senderChannels.front()->send(std::string("late-dc1-message")),
		        "failed to send paused stale DataChannel message");
		latch.waitUntilEntered();
		auto current = makeUnconnectedDataChannel("sendChannel");
		std::thread replacing([&]() { manager.receiveNativeMediaTestDataChannel(pair.receiver, current.channel); });
		const bool replacementCommitted = waitUntil(
		    [&]() {
			    std::lock_guard<std::mutex> mediaLock(pair.receiver->mediaMutex);
			    return pair.receiver->dataChannel == current.channel;
		    },
		    2s);
		latch.release();
		replacing.join();
		requireEventually([&]() { return dispatchCompleted.load(std::memory_order_acquire); },
		                  "paused stale message dispatch did not complete");
		manager.setNativeMediaTestPeerDataDispatchHook(nullptr);
		manager.setNativeMediaTestPeerDataDispatchCompleteHook(nullptr);
		require(replacementCommitted && messageCallbacks.load(std::memory_order_acquire) == 0,
		        "DC1 message subscriber ran after DC2 replaced its exact lease");
	}
}

void testDataChannelCallbackCleanupRunsOutsideManagerLocks()
{
	VDONinjaPeerManager manager;
	auto peer = manager.createNativeMediaTestViewerPeer("dc-cleanup-locks");
	auto retired = makeUnconnectedDataChannel("sendChannel");
	auto current = makeUnconnectedDataChannel("sendChannel");
	manager.receiveNativeMediaTestDataChannel(peer, retired.channel);
	std::atomic<int> cleanupEntries{0};
	std::atomic<bool> locksAvailable{true};
	manager.setNativeMediaTestDataChannelLifecycleHook([&](NativeMediaTestDataChannelStage stage,
	                                                       const std::shared_ptr<PeerInfo> &hookPeer,
	                                                       const std::shared_ptr<rtc::DataChannel> &channel, uint64_t) {
		if (stage != NativeMediaTestDataChannelStage::BeforeCallbackCleanup || channel != retired.channel) {
			return;
		}
		cleanupEntries.fetch_add(1, std::memory_order_acq_rel);
		if (!manager.nativeMediaTestPeerRegistryLockAvailable() || !hookPeer->mediaMutex.try_lock()) {
			locksAvailable.store(false, std::memory_order_release);
			return;
		}
		hookPeer->mediaMutex.unlock();
	});
	manager.receiveNativeMediaTestDataChannel(peer, current.channel);
	manager.setNativeMediaTestDataChannelLifecycleHook(nullptr);
	std::lock_guard<std::mutex> mediaLock(peer->mediaMutex);
	require(cleanupEntries.load(std::memory_order_acquire) == 1 && locksAvailable.load(std::memory_order_acquire) &&
	            peer->dataChannel == current.channel,
	        "retired DataChannel callback cleanup ran under manager locks or did not run exactly once");
}

void testDataChannelAliasCommitRacesCannotResurrectClosedTransport()
{
	{
		VDONinjaPeerManager manager;
		auto transport = makeConnectedManagerDataChannelPair(manager, "dc-replace-transport", {"sendChannel"});
		auto target = manager.createNativeMediaTestViewerPeer("dc-replace-target", "target-session");
		manager.bindViewerSignalingDataChannel(transport.receiver->uuid, target->uuid, target->session);
		manager.bindViewerSignalingDataChannel(transport.receiver->uuid, "dc-replace-pending", "target-session");
		require(manager.nativeMediaTestPendingViewerSignalingDataChannelCount() == 1,
		        "replacement alias purge gate did not stage its pending route");
		auto replacement = makeUnconnectedDataChannel("sendChannel");
		manager.receiveNativeMediaTestDataChannel(transport.receiver, replacement.channel);
		bool existingAliasCleared = false;
		{
			std::lock_guard<std::mutex> mediaLock(target->mediaMutex);
			existingAliasCleared = !target->signalingDataChannel;
		}
		require(existingAliasCleared && manager.nativeMediaTestPendingViewerSignalingDataChannelCount() == 0,
		        "DC1-to-DC2 replacement retained existing or pending aliases to DC1");
	}

	{
		VDONinjaPeerManager manager;
		auto transport = makeConnectedManagerDataChannelPair(manager, "dc-alias-transport", {"sendChannel"});
		auto target = manager.createNativeMediaTestViewerPeer("dc-alias-target", "target-session");
		DataChannelStageLatch latch(NativeMediaTestDataChannelStage::BeforeAliasCommit);
		manager.setNativeMediaTestDataChannelLifecycleHook(
		    [&latch](NativeMediaTestDataChannelStage stage, const std::shared_ptr<PeerInfo> &,
		             const std::shared_ptr<rtc::DataChannel> &channel, uint64_t) { latch.hook(stage, channel); });
		std::thread binding(
		    [&]() { manager.bindViewerSignalingDataChannel(transport.receiver->uuid, target->uuid, target->session); });
		latch.waitUntilEntered();
		transport.senderChannels.front()->close();
		const bool transportCleared = waitUntil(
		    [&]() {
			    std::lock_guard<std::mutex> mediaLock(transport.receiver->mediaMutex);
			    return !transport.receiver->dataChannel;
		    },
		    2s);
		latch.release();
		binding.join();
		manager.setNativeMediaTestDataChannelLifecycleHook(nullptr);
		std::lock_guard<std::mutex> mediaLock(target->mediaMutex);
		require(transportCleared && !target->signalingDataChannel,
		        "existing-target alias commit resurrected a closed transport DataChannel");
	}

	{
		VDONinjaPeerManager manager;
		auto transport = makeConnectedManagerDataChannelPair(manager, "dc-pending-transport", {"sendChannel"});
		DataChannelStageLatch latch(NativeMediaTestDataChannelStage::BeforePendingAliasCommit);
		manager.setNativeMediaTestDataChannelLifecycleHook(
		    [&latch](NativeMediaTestDataChannelStage stage, const std::shared_ptr<PeerInfo> &,
		             const std::shared_ptr<rtc::DataChannel> &channel, uint64_t) { latch.hook(stage, channel); });
		std::thread binding([&]() {
			manager.bindViewerSignalingDataChannel(transport.receiver->uuid, "dc-pending-target", "target-session");
		});
		latch.waitUntilEntered();
		transport.senderChannels.front()->close();
		const bool transportCleared = waitUntil(
		    [&]() {
			    std::lock_guard<std::mutex> mediaLock(transport.receiver->mediaMutex);
			    return !transport.receiver->dataChannel;
		    },
		    2s);
		latch.release();
		binding.join();
		manager.setNativeMediaTestDataChannelLifecycleHook(nullptr);
		require(transportCleared && manager.nativeMediaTestPendingViewerSignalingDataChannelCount() == 0,
		        "pending alias commit retained a closed transport DataChannel");
	}

	{
		VDONinjaPeerManager manager;
		auto transport = makeConnectedManagerDataChannelPair(manager, "dc-consume-transport", {"sendChannel"});
		manager.bindViewerSignalingDataChannel(transport.receiver->uuid, "dc-consume-target", "target-session");
		require(manager.nativeMediaTestPendingViewerSignalingDataChannelCount() == 1,
		        "pending alias consume race did not stage its transport route");
		auto target = manager.createNativeMediaTestViewerPeer("dc-consume-target", "target-session");
		DataChannelStageLatch latch(NativeMediaTestDataChannelStage::BeforePendingAliasConsume);
		manager.setNativeMediaTestDataChannelLifecycleHook(
		    [&latch](NativeMediaTestDataChannelStage stage, const std::shared_ptr<PeerInfo> &,
		             const std::shared_ptr<rtc::DataChannel> &channel, uint64_t) { latch.hook(stage, channel); });
		std::thread consuming(
		    [&]() { manager.consumeNativeMediaTestPendingViewerSignalingDataChannel(target, target->session); });
		latch.waitUntilEntered();
		transport.senderChannels.front()->close();
		const bool transportCleared = waitUntil(
		    [&]() {
			    std::lock_guard<std::mutex> mediaLock(transport.receiver->mediaMutex);
			    return !transport.receiver->dataChannel;
		    },
		    2s);
		latch.release();
		consuming.join();
		manager.setNativeMediaTestDataChannelLifecycleHook(nullptr);
		std::lock_guard<std::mutex> mediaLock(target->mediaMutex);
		require(transportCleared && !target->signalingDataChannel,
		        "pending alias consume resurrected a transport closed after route removal");
	}
}

void testOpenDataChannelSameHandleReAdoptionSurvivesPausedCleanup()
{
	VDONinjaPeerManager manager;
	std::mutex incomingMutex;
	std::shared_ptr<rtc::DataChannel> firstReceiver;
	manager.setNativeMediaTestDataChannelLifecycleHook([&](NativeMediaTestDataChannelStage stage,
	                                                       const std::shared_ptr<PeerInfo> &,
	                                                       const std::shared_ptr<rtc::DataChannel> &channel, uint64_t) {
		if (stage == NativeMediaTestDataChannelStage::IncomingEntered) {
			std::lock_guard<std::mutex> lock(incomingMutex);
			if (!firstReceiver) {
				firstReceiver = channel;
			}
		}
	});
	auto pair = makeConnectedManagerDataChannelPair(manager, "dc-same-handle-readd", {"sendChannel"});
	manager.setNativeMediaTestDataChannelLifecycleHook(nullptr);
	{
		std::lock_guard<std::mutex> lock(incomingMutex);
		require(firstReceiver != nullptr && firstReceiver->isOpen(),
		        "same-handle cleanup race did not retain the paired open receiver handle");
	}
	auto replacement = makeUnconnectedDataChannel("sendChannel");
	DataChannelStageLatch cleanupLatch(NativeMediaTestDataChannelStage::BeforeCallbackCleanup, firstReceiver.get());
	manager.setNativeMediaTestDataChannelLifecycleHook(
	    [&cleanupLatch](NativeMediaTestDataChannelStage stage, const std::shared_ptr<PeerInfo> &,
	                    const std::shared_ptr<rtc::DataChannel> &channel,
	                    uint64_t) { cleanupLatch.hook(stage, channel); });
	std::thread replacing([&]() { manager.receiveNativeMediaTestDataChannel(pair.receiver, replacement.channel); });
	cleanupLatch.waitUntilEntered();
	manager.receiveNativeMediaTestDataChannel(pair.receiver, firstReceiver);
	cleanupLatch.release();
	replacing.join();
	manager.setNativeMediaTestDataChannelLifecycleHook(nullptr);

	std::mutex messageMutex;
	std::vector<std::string> messages;
	manager.setOnDataChannelMessage([&](const PeerEventIdentity &, const std::string &message) {
		std::lock_guard<std::mutex> lock(messageMutex);
		messages.push_back(message);
	});
	require(pair.senderChannels.front()->send(std::string("same-handle-current")),
	        "re-adopted open DataChannel rejected its current message probe");
	requireEventually(
	    [&]() {
		    std::lock_guard<std::mutex> lock(messageMutex);
		    return std::find(messages.begin(), messages.end(), "same-handle-current") != messages.end();
	    },
	    "paused stale cleanup erased callbacks from the re-adopted open DataChannel");

	// Retire the same open handle again and let callback cleanup finish before re-adopting
	// it. Cleanup must not reset libdatachannel's internal open-trigger state.
	auto postCleanupReplacement = makeUnconnectedDataChannel("sendChannel");
	manager.receiveNativeMediaTestDataChannel(pair.receiver, postCleanupReplacement.channel);
	{
		std::lock_guard<std::mutex> mediaLock(pair.receiver->mediaMutex);
		require(pair.receiver->dataChannel == postCleanupReplacement.channel,
		        "completed-cleanup setup did not retire the original open DataChannel");
	}
	manager.receiveNativeMediaTestDataChannel(pair.receiver, firstReceiver);
	require(pair.senderChannels.front()->send(std::string("same-handle-after-cleanup")),
	        "post-cleanup re-adopted DataChannel rejected its current message probe");
	requireEventually(
	    [&]() {
		    std::lock_guard<std::mutex> lock(messageMutex);
		    return std::find(messages.begin(), messages.end(), "same-handle-after-cleanup") != messages.end();
	    },
	    "completed callback cleanup reset the re-adopted open DataChannel state");
	pair.senderChannels.front()->close();
	requireEventually(
	    [&]() {
		    std::lock_guard<std::mutex> mediaLock(pair.receiver->mediaMutex);
		    return !pair.receiver->dataChannel && !pair.receiver->hasDataChannel;
	    },
	    "re-adopted open DataChannel did not retain its exact close callback");
}

void testDataChannelInCallbackRetirementDrainsCallbackCleanup()
{
	VDONinjaPeerManager manager;
	auto pair = makeConnectedManagerDataChannelPair(manager, "dc-self-cleanup", {"sendChannel"});
	auto mediaTrack = makeVp9ReceiveTrack("video");
	manager.receiveNativeMediaTestTrack(pair.receiver, mediaTrack.track);
	std::shared_ptr<rtc::DataChannel> retired;
	{
		std::lock_guard<std::mutex> mediaLock(pair.receiver->mediaMutex);
		retired = pair.receiver->dataChannel;
	}
	require(retired != nullptr, "self-cleanup gate did not capture the open receiver DataChannel");
	auto replacement = makeUnconnectedDataChannel("sendChannel");
	std::atomic<bool> replacedInsideSubscriber{false};
	std::mutex messageMutex;
	std::vector<std::string> messages;
	manager.setOnDataChannelMessage([&](const PeerEventIdentity &, const std::string &message) {
		if (message == "replace-from-subscriber") {
			manager.receiveNativeMediaTestDataChannel(pair.receiver, replacement.channel);
			replacedInsideSubscriber.store(true, std::memory_order_release);
			return;
		}
		std::lock_guard<std::mutex> lock(messageMutex);
		messages.push_back(message);
	});
	require(pair.senderChannels.front()->send(std::string("replace-from-subscriber")),
	        "failed to send in-callback DataChannel retirement probe");
	requireEventually(
	    [&]() {
		    bool replacementCurrent = false;
		    {
			    std::lock_guard<std::mutex> mediaLock(pair.receiver->mediaMutex);
			    replacementCurrent = pair.receiver->dataChannel == replacement.channel;
		    }
		    return replacedInsideSubscriber.load(std::memory_order_acquire) && replacementCurrent &&
		           manager.nativeMediaTestPendingDataChannelCallbackCleanupCount(pair.receiver) == 0;
	    },
	    "same-callback DataChannel retirement did not drain its deferred callback cleanup");

	require(pair.senderChannels.front()->send(std::string("queued-after-retirement")),
	        "retired open DataChannel rejected its callback-cleanup probe");
	std::this_thread::sleep_for(25ms);
	{
		std::lock_guard<std::mutex> lock(messageMutex);
		require(std::find(messages.begin(), messages.end(), "queued-after-retirement") == messages.end(),
		        "retired DataChannel retained a live manager message callback before re-adoption");
	}
	manager.receiveNativeMediaTestDataChannel(pair.receiver, retired);
	requireEventually(
	    [&]() {
		    std::lock_guard<std::mutex> lock(messageMutex);
		    return std::find(messages.begin(), messages.end(), "queued-after-retirement") != messages.end();
	    },
	    "retired DataChannel retained a live stale message callback instead of queueing for re-adoption");

	pair.senderChannels.front()->close();
	requireEventually(
	    [&]() {
		    bool channelRetiredAndMediaHealthy = false;
		    {
			    std::lock_guard<std::mutex> mediaLock(pair.receiver->mediaMutex);
			    channelRetiredAndMediaHealthy =
			        !pair.receiver->dataChannel && pair.receiver->videoTrack == mediaTrack.track;
		    }
		    return channelRetiredAndMediaHealthy &&
		           manager.nativeMediaTestPendingDataChannelCallbackCleanupCount(pair.receiver) == 0;
	    },
	    "terminal DataChannel callback cleanup leaked or retired healthy media");
}

void testIdentityBearingByePreservesTransportPeerAndCleanupState()
{
	VDONinjaSource source(NativeMediaTestTag{});
	VDONinjaPeerManager manager;
	source.bindNativeMediaTestPeerManager(manager);
	auto pair = makeConnectedManagerDataChannelPair(manager, "dc-targeted-bye", {"sendChannel"});
	auto mediaTrack = makeVp9ReceiveTrack("video");
	manager.receiveNativeMediaTestTrack(pair.receiver, mediaTrack.track);
	std::atomic<bool> dispatchCompleted{false};
	manager.setNativeMediaTestPeerDataDispatchCompleteHook(
	    [&](const std::shared_ptr<PeerInfo> &) { dispatchCompleted.store(true, std::memory_order_release); });
	JsonBuilder targetedBye;
	targetedBye.add("bye", true);
	targetedBye.add("UUID", pair.receiver->uuid);
	targetedBye.add("session", pair.receiver->session);
	require(pair.senderChannels.front()->send(targetedBye.build()),
	        "failed to send identity-bearing bye control probe");
	requireEventually([&]() { return dispatchCompleted.load(std::memory_order_acquire); },
	                  "identity-bearing bye did not complete its DataChannel dispatch");
	manager.setNativeMediaTestPeerDataDispatchCompleteHook(nullptr);
	bool transportAndMediaHealthy = false;
	{
		std::lock_guard<std::mutex> mediaLock(pair.receiver->mediaMutex);
		transportAndMediaHealthy = pair.receiver->dataChannel && pair.receiver->videoTrack == mediaTrack.track;
	}
	require(transportAndMediaHealthy && source.nativeMediaTestSnapshot().targetedPeerByes == 1 &&
	            manager.nativeMediaTestPendingDataChannelCallbackCleanupCount(pair.receiver) == 0,
	        "identity-bearing bye retired the healthy transport peer or leaked callback cleanup");
}

void testPeerConnectionDescriptionAndFeedbackFunctionsShareOwnerSession()
{
	VDONinjaSignaling signaling;
	auto manager = std::make_unique<VDONinjaPeerManager>();
	OwnerSessionProbe probe;
	OwnerSessionHookGuard ownerSessionHookGuard(manager, probe);
	manager->setNativeMediaTestOwnerSessionHook([&probe](NativeMediaTestOwnerSessionStage stage,
	                                                     PeerManagerCompletionKind kind,
	                                                     const void *handle) { probe.hook(stage, kind, handle); });
	manager->initialize(&signaling);
	const void *signalingHandle = &signaling;
	auto peer = manager->createNativeMediaTestPublisherPeer("pc-owner-session", "session-a");
	require(peer && peer->pc, "owner-session gate did not create its real manager PeerConnection");
	const void *pcHandle = peer->pc.get();
	std::shared_ptr<rtc::Track> videoTrack;
	{
		std::lock_guard<std::mutex> mediaLock(peer->mediaMutex);
		videoTrack = peer->videoTrack;
	}
	require(videoTrack != nullptr, "publisher did not retain its real video Track");
	auto feedbackCompletion = manager->nativeMediaTestVideoFeedbackCompletion(peer);
	require(static_cast<bool>(feedbackCompletion), "publisher Track did not retain its exact PLI feedback completion");
	feedbackCompletion();
	probe.waitUntilSeen(NativeMediaTestOwnerSessionStage::PermitAcquired, PeerManagerCompletionKind::VideoFeedback, 1,
	                    videoTrack.get());

	try {
		peer->pc->setLocalDescription(rtc::Description::Type::Offer);
	} catch (const std::exception &) {
		// Auto-negotiation may already have generated the same local offer.
	}
	probe.waitUntilSeen(NativeMediaTestOwnerSessionStage::PermitAcquired,
	                    PeerManagerCompletionKind::PeerConnectionLocalDescription, 1, pcHandle);
	peer->pc->close();
	probe.waitUntilSeen(NativeMediaTestOwnerSessionStage::PermitAcquired,
	                    PeerManagerCompletionKind::PeerConnectionState, 1, pcHandle);

	manager.reset();
	ownerSessionHookGuard.disarm();
	for (const auto kind :
	     {PeerManagerCompletionKind::PeerConnectionState, PeerManagerCompletionKind::PeerConnectionLocalDescription,
	      PeerManagerCompletionKind::PeerConnectionLocalCandidate,
	      PeerManagerCompletionKind::PeerConnectionGatheringState, PeerManagerCompletionKind::PeerConnectionTrack,
	      PeerManagerCompletionKind::PeerConnectionDataChannel}) {
		require(probe.count(NativeMediaTestOwnerSessionStage::AfterDetach, kind, pcHandle) == 1,
		        "shutdown did not detach exactly one manager PeerConnection function");
	}
	for (const auto kind :
	     {PeerManagerCompletionKind::SignalingOffer, PeerManagerCompletionKind::SignalingAnswer,
	      PeerManagerCompletionKind::SignalingOfferRequest, PeerManagerCompletionKind::SignalingIceRestartRequest,
	      PeerManagerCompletionKind::SignalingIceCandidate, PeerManagerCompletionKind::SignalingPeerCleanup}) {
		require(probe.count(NativeMediaTestOwnerSessionStage::AfterDetach, kind, signalingHandle) == 1,
		        "shutdown did not detach exactly one manager signaling function");
	}
	require(probe.count(NativeMediaTestOwnerSessionStage::AfterDetach, PeerManagerCompletionKind::VideoFeedback) == 1,
	        "shutdown did not detach the real video feedback media chain");
	require(probe.count(NativeMediaTestOwnerSessionStage::AfterDetach, PeerManagerCompletionKind::AudioFeedback) == 1,
	        "shutdown did not detach the real audio feedback media chain");
	require(probe.firstPrecedes(NativeMediaTestOwnerSessionStage::WorkDrained, PeerManagerCompletionKind::OwnerSession,
	                            NativeMediaTestOwnerSessionStage::AfterDetach,
	                            PeerManagerCompletionKind::PeerConnectionLocalDescription, nullptr, pcHandle),
	        "installed PeerConnection functions detached before owner-session work drained");
	require(probe.firstPrecedes(NativeMediaTestOwnerSessionStage::WorkDrained, PeerManagerCompletionKind::OwnerSession,
	                            NativeMediaTestOwnerSessionStage::AfterDetach,
	                            PeerManagerCompletionKind::SignalingPeerCleanup, nullptr, signalingHandle),
	        "installed signaling functions detached before owner-session work drained");
}

void testAdmittedFeedbackCompletionDrainsBeforeOwnerState()
{
	auto manager = std::make_unique<VDONinjaPeerManager>();
	auto peer = manager->createNativeMediaTestPublisherPeer("feedback-owner-drain", "session-a");
	std::shared_ptr<rtc::Track> videoTrack;
	{
		std::lock_guard<std::mutex> mediaLock(peer->mediaMutex);
		videoTrack = peer->videoTrack;
	}
	require(videoTrack != nullptr, "feedback drain gate did not create its real publisher Track");
	auto feedbackCompletion = manager->nativeMediaTestVideoFeedbackCompletion(peer);
	require(static_cast<bool>(feedbackCompletion), "feedback drain gate did not capture its exact PLI completion");
	OwnerSessionProbe probe;
	probe.block(NativeMediaTestOwnerSessionStage::PermitAcquired, PeerManagerCompletionKind::VideoFeedback,
	            videoTrack.get());
	OwnerSessionHookGuard ownerSessionHookGuard(manager, probe);
	manager->setNativeMediaTestOwnerSessionHook([&probe](NativeMediaTestOwnerSessionStage stage,
	                                                     PeerManagerCompletionKind kind,
	                                                     const void *handle) { probe.hook(stage, kind, handle); });
	OwnerSessionThreadGuard feedback(probe, [&]() { feedbackCompletion(); });
	probe.waitUntilBlocked();
	OwnerSessionThreadGuard destroying(probe, [&]() { manager.reset(); });
	probe.waitUntilSeen(NativeMediaTestOwnerSessionStage::WaitingForPermits, PeerManagerCompletionKind::OwnerSession);
	probe.releaseBlock();
	feedback.join();
	destroying.join();
	ownerSessionHookGuard.disarm();
	require(!manager, "owner state was not released after its admitted feedback completion drained");
}

void testSameHandleDataChannelReplacementDrainsBeforeDetachingEveryInstalledFunction()
{
	auto manager = std::make_unique<VDONinjaPeerManager>();
	OwnerSessionProbe probe;
	OwnerSessionHookGuard ownerSessionHookGuard(manager, probe);
	manager->setNativeMediaTestOwnerSessionHook([&probe](NativeMediaTestOwnerSessionStage stage,
	                                                     PeerManagerCompletionKind kind,
	                                                     const void *handle) { probe.hook(stage, kind, handle); });
	auto pair = makeConnectedManagerDataChannelPair(*manager, "dc-owner-session-readd", {"sendChannel"});
	std::shared_ptr<rtc::DataChannel> original;
	{
		std::lock_guard<std::mutex> mediaLock(pair.receiver->mediaMutex);
		original = pair.receiver->dataChannel;
	}
	require(original != nullptr && original->isOpen(),
	        "same-handle shutdown gate did not retain its real open receiver DataChannel");
	auto replacement = makeUnconnectedDataChannel("sendChannel");
	manager->receiveNativeMediaTestDataChannel(pair.receiver, replacement.channel);
	manager->receiveNativeMediaTestDataChannel(pair.receiver, original);
	manager.reset();
	ownerSessionHookGuard.disarm();

	for (const auto kind :
	     {PeerManagerCompletionKind::DataChannelClosed, PeerManagerCompletionKind::DataChannelError,
	      PeerManagerCompletionKind::DataChannelOpen, PeerManagerCompletionKind::DataChannelMessage}) {
		require(probe.count(NativeMediaTestOwnerSessionStage::AfterDetach, kind, original.get()) == 2,
		        "same-handle replacement did not retain both exact shutdown detachers");
	}
	require(probe.firstPrecedes(NativeMediaTestOwnerSessionStage::WorkDrained, PeerManagerCompletionKind::OwnerSession,
	                            NativeMediaTestOwnerSessionStage::AfterDetach,
	                            PeerManagerCompletionKind::DataChannelMessage, nullptr, original.get()),
	        "same-handle DataChannel functions detached before owner-session work drained");
	pair.senderChannels.front()->close();
}

void testPeerManagerOwnerSessionLifecycleCycles()
{
	constexpr size_t cycleCount = 128;
	for (size_t cycle = 0; cycle < cycleCount; ++cycle) {
		auto manager = std::make_unique<VDONinjaPeerManager>();
		auto peer = manager->createNativeMediaTestPublisherPeer("owner-cycle-" + std::to_string(cycle),
		                                                        "session-" + std::to_string(cycle));
		require(peer && peer->pc && peer->videoTrack && peer->audioTrack,
		        "owner-session lifecycle cycle did not create the real linked RTC object chain");
		auto feedbackCompletion = manager->nativeMediaTestVideoFeedbackCompletion(peer);
		require(static_cast<bool>(feedbackCompletion),
		        "owner-session lifecycle cycle did not install the real feedback completion");
		manager.reset();
		feedbackCompletion();
	}
}

void testDataChannelInFlightCallbackIsSafeDuringManagerDestruction()
{
	auto manager = std::make_unique<VDONinjaPeerManager>();
	auto pair = makeConnectedManagerDataChannelPair(*manager, "dc-destruction", {"sendChannel"});
	std::shared_ptr<rtc::DataChannel> receiverChannel;
	{
		std::lock_guard<std::mutex> mediaLock(pair.receiver->mediaMutex);
		receiverChannel = pair.receiver->dataChannel;
	}
	require(receiverChannel != nullptr, "manager destruction gate did not retain its real receiver DataChannel");
	OwnerSessionProbe probe;
	probe.block(NativeMediaTestOwnerSessionStage::PermitAcquired, PeerManagerCompletionKind::DataChannelMessage,
	            receiverChannel.get());
	OwnerSessionHookGuard ownerSessionHookGuard(manager, probe);
	manager->setNativeMediaTestOwnerSessionHook([&probe](NativeMediaTestOwnerSessionStage stage,
	                                                     PeerManagerCompletionKind kind,
	                                                     const void *handle) { probe.hook(stage, kind, handle); });
	const bool messageSent = pair.senderChannels.front()->send(std::string("destruction-probe"));
	if (!messageSent) {
		probe.releaseBlock();
		manager->setNativeMediaTestOwnerSessionHook(nullptr);
		manager.reset();
		ownerSessionHookGuard.disarm();
		pair.senderChannels.front()->close();
		require(false, "failed to send in-flight destruction DataChannel probe");
	}
	const bool callbackHeld = waitUntil(
	    [&]() {
		    return probe.count(NativeMediaTestOwnerSessionStage::PermitAcquired,
		                       PeerManagerCompletionKind::DataChannelMessage, receiverChannel.get()) == 1;
	    },
	    10s);
	if (!callbackHeld) {
		probe.releaseBlock();
		manager->setNativeMediaTestOwnerSessionHook(nullptr);
		manager.reset();
		ownerSessionHookGuard.disarm();
		pair.senderChannels.front()->close();
		require(false, "DataChannel callback did not acquire its owner-session permit before destruction");
	}
	std::atomic<bool> destructionFinished{false};
	OwnerSessionThreadGuard destroying(probe, [&]() {
		manager.reset();
		destructionFinished.store(true, std::memory_order_release);
	});
	const bool destructorReachedObservableState = waitUntil(
	    [&]() {
		    return probe.count(NativeMediaTestOwnerSessionStage::WaitingForPermits,
		                       PeerManagerCompletionKind::OwnerSession) >= 1 ||
		           destructionFinished.load(std::memory_order_acquire);
	    },
	    10s);
	const bool destructionFinishedWhileHeld = destructionFinished.load(std::memory_order_acquire);
	const size_t waitingWhileHeld =
	    probe.count(NativeMediaTestOwnerSessionStage::WaitingForPermits, PeerManagerCompletionKind::OwnerSession);
	const size_t drainedWhileHeld =
	    probe.count(NativeMediaTestOwnerSessionStage::WorkDrained, PeerManagerCompletionKind::OwnerSession);
	const size_t dataChannelDetachedWhileHeld =
	    probe.count(NativeMediaTestOwnerSessionStage::AfterDetach, PeerManagerCompletionKind::DataChannelMessage,
	                receiverChannel.get());
	probe.releaseBlock();
	destroying.join();
	ownerSessionHookGuard.disarm();
	pair.senderChannels.front()->close();

	require(!destructionFinishedWhileHeld,
	        "manager destruction completed while an admitted DataChannel callback still held its owner-session permit");
	require(waitingWhileHeld == 1,
	        "manager destruction did not enter exactly one DataChannel owner-session permit wait while work was held");
	require(drainedWhileHeld == 0,
	        "manager destruction reported DataChannel owner-session work drained before the held callback completed");
	require(dataChannelDetachedWhileHeld == 0,
	        "manager destruction detached the exact DataChannel callback before its admitted work drained");
	require(
	    destructorReachedObservableState,
	    "manager destruction exposed neither its permit wait nor completion while the DataChannel callback was held");
	require(!manager && destructionFinished.load(std::memory_order_acquire),
	        "manager destruction did not complete after the in-flight DataChannel callback drained");
	require(probe.count(NativeMediaTestOwnerSessionStage::WorkDrained, PeerManagerCompletionKind::OwnerSession) == 1,
	        "manager destruction did not report exactly one completed DataChannel owner-session drain");
	require(probe.count(NativeMediaTestOwnerSessionStage::AfterDetach, PeerManagerCompletionKind::DataChannelMessage,
	                    receiverChannel.get()) == 1,
	        "manager destruction did not detach the exact DataChannel callback once after its admitted work drained");
	require(
	    probe.firstPrecedes(NativeMediaTestOwnerSessionStage::PermitAcquired,
	                        PeerManagerCompletionKind::DataChannelMessage,
	                        NativeMediaTestOwnerSessionStage::WaitingForPermits,
	                        PeerManagerCompletionKind::OwnerSession, receiverChannel.get()) &&
	        probe.firstPrecedes(NativeMediaTestOwnerSessionStage::WaitingForPermits,
	                            PeerManagerCompletionKind::OwnerSession, NativeMediaTestOwnerSessionStage::WorkDrained,
	                            PeerManagerCompletionKind::OwnerSession) &&
	        probe.firstPrecedes(NativeMediaTestOwnerSessionStage::WorkDrained, PeerManagerCompletionKind::OwnerSession,
	                            NativeMediaTestOwnerSessionStage::AfterDetach,
	                            PeerManagerCompletionKind::DataChannelMessage, nullptr, receiverChannel.get()),
	    "DataChannel owner-session events did not order permit, wait, drain, then exact callback detachment");
}

void testDataChannelSnapshotRejectsCallbackAfterSourceDestruction()
{
	VDONinjaPeerManager manager;
	auto source = std::make_unique<VDONinjaSource>(NativeMediaTestTag{});
	source->bindNativeMediaTestPeerManager(manager);
	auto pair = makeConnectedManagerDataChannelPair(manager, "dc-source-destruction", {"sendChannel"});
	PeerDispatchLatch latch(pair.receiver->generation);
	std::atomic<bool> dispatchCompleted{false};
	manager.setNativeMediaTestPeerDataDispatchHook(
	    [&latch](const std::shared_ptr<PeerInfo> &peer) { latch.hook(peer); });
	manager.setNativeMediaTestPeerDataDispatchCompleteHook([&dispatchCompleted](const std::shared_ptr<PeerInfo> &) {
		dispatchCompleted.store(true, std::memory_order_release);
	});
	require(pair.senderChannels.front()->send(std::string(R"({"bye":true})")),
	        "failed to send source-destruction DataChannel probe");
	latch.waitUntilEntered();
	source.reset();
	latch.release();
	requireEventually([&]() { return dispatchCompleted.load(std::memory_order_acquire); },
	                  "snapshotted source callback did not reach post-subscriber completion");
	manager.setNativeMediaTestPeerDataDispatchHook(nullptr);
	manager.setNativeMediaTestPeerDataDispatchCompleteHook(nullptr);
	require(manager.getPeerIdentity(pair.receiver->uuid).has_value(),
	        "snapshotted source callback acted after source destruction");
}

void testRtcTrackOrderingHeavyRepeat()
{
	for (int iteration = 0; iteration < 24; ++iteration) {
		VDONinjaSource source(NativeMediaTestTag{});
		VDONinjaPeerManager manager;
		source.bindNativeMediaTestPeerManager(manager);
		auto peer = manager.createNativeMediaTestViewerPeer("repeat-" + std::to_string(iteration));
		auto older = makeVp9ReceiveTrack("video");
		auto newer = makeVp9ReceiveTrack("video");
		TrackCommitLatch latch(older.track.get());
		manager.setNativeMediaTestTrackCommitHook([&latch](const std::string &, TrackType,
		                                                   const std::shared_ptr<rtc::Track> &track,
		                                                   uint64_t) { latch.hook(track); });
		std::thread lateOlder([&]() { manager.receiveNativeMediaTestTrack(peer, older.track); });
		latch.waitUntilEntered();
		manager.receiveNativeMediaTestTrack(peer, newer.track);
		latch.release();
		lateOlder.join();
		manager.setNativeMediaTestTrackCommitHook(nullptr);
		const auto sourceTracks = source.nativeMediaTestTrackSnapshot();
		std::lock_guard<std::mutex> mediaLock(peer->mediaMutex);
		require(peer->videoTrack == newer.track && sourceTracks.video == newer.track.get(),
		        "repeated RTC ordering gate did not converge on newest primary identity");
		require(source.nativeMediaTestRejectedTrackEventCount() == 0,
		        "repeated RTC ordering gate dispatched a stale add past the manager lease check");
	}
}

void testRealDecodeAndAlphaComposition(const std::vector<std::vector<uint8_t>> &primaryGop,
                                       const std::vector<std::vector<uint8_t>> &alphaGop)
{
	VDONinjaSource source(NativeMediaTestTag{});
	OutputCollector output;
	source.setNativeMediaTestOutputHook([&output](NativeMediaTestOutput frame) { output.add(std::move(frame)); });

	source.transitionNativeMediaTestPipeline(false);
	size_t primaryCursor = 0;
	feedUntilOutputCount(source, output, primaryGop, primaryCursor, 1000, 1);
	auto frames = output.copy();
	require(frames.size() == 1, "real VP9 primary decode did not produce exactly one output frame");
	require(frames[0].rtpTimestamp == 1000 && !frames[0].hasAlpha,
	        "opaque primary output lost its decoder-preserved RTP timestamp");
	require(std::all_of(frames[0].bgra.begin() + 3, frames[0].bgra.end(),
	                    [index = size_t{3}](uint8_t value) mutable {
		                    const bool alphaByte = index % 4 == 3;
		                    ++index;
		                    return !alphaByte || value == 255;
	                    }),
	        "opaque primary output did not force a fully opaque BGRA alpha channel");

	source.transitionNativeMediaTestPipeline(true);
	primaryCursor = 0;
	size_t alphaCursor = 0;
	while (output.size() < 2 && primaryCursor < primaryGop.size() && alphaCursor < alphaGop.size()) {
		const uint32_t timestamp = 200000u + static_cast<uint32_t>(primaryCursor) * 3000u;
		source.feedNativeMediaTestVp9AccessUnit(true, alphaGop[alphaCursor++], timestamp);
		source.feedNativeMediaTestVp9AccessUnit(false, primaryGop[primaryCursor++], timestamp);
	}
	frames = output.copy();
	require(frames.size() == 2, "exact primary/alpha RTP pair did not emit");
	require(frames.back().rtpTimestamp == 200000u && frames.back().hasAlpha,
	        "paired output lost RTP identity or transparency state");
	bool foundNonOpaqueAlpha = false;
	for (size_t index = 3; index < frames.back().bgra.size(); index += 4) {
		foundNonOpaqueAlpha = foundNonOpaqueAlpha || frames.back().bgra[index] < 250;
	}
	require(foundNonOpaqueAlpha, "decoded VP9 alpha Y plane was not applied to BGRA output");
}

void testRtpReorderDropWrapAndRecovery(const std::vector<std::vector<uint8_t>> &primaryGop,
                                       const std::vector<std::vector<uint8_t>> &alphaGop)
{
	VDONinjaSource source(NativeMediaTestTag{});
	OutputCollector output;
	source.setNativeMediaTestOutputHook([&output](NativeMediaTestOutput frame) { output.add(std::move(frame)); });
	source.transitionNativeMediaTestPipeline(true);

	constexpr uint32_t baseTimestamp = 0xFFFF0000u;
	constexpr size_t missingMateIndex = 10;
	const auto timestampFor = [](size_t index) {
		return static_cast<uint32_t>(baseTimestamp + static_cast<uint32_t>(index) * 3000u);
	};

	// Give primary a real decoder-output lead. This is not just packet order:
	// frame-threaded primary outputs are already queued before alpha produces.
	size_t primaryCursor = 0;
	size_t alphaCursor = 0;
	for (; primaryCursor < 18; ++primaryCursor) {
		source.feedNativeMediaTestVp9AccessUnit(false, primaryGop[primaryCursor], timestampFor(primaryCursor));
	}
	const auto latencySnapshot = source.nativeMediaTestSnapshot();
	require(latencySnapshot.pendingPrimaryFrames > 0 && latencySnapshot.pendingAlphaFrames == 0,
	        "gate did not establish differing primary/alpha decoder output latency");
	for (; alphaCursor < 16; ++alphaCursor) {
		const uint32_t timestamp = timestampFor(alphaCursor) + (alphaCursor == missingMateIndex ? 1u : 0u);
		source.feedNativeMediaTestVp9AccessUnit(true, alphaGop[alphaCursor], timestamp);
	}

	bool usedAlphaFirst = false;
	bool usedPrimaryFirst = false;
	while (primaryCursor < primaryGop.size() || alphaCursor < alphaGop.size()) {
		const bool alphaFirst = ((primaryCursor + alphaCursor) % 2) == 0;
		if (alphaFirst) {
			usedAlphaFirst = true;
			if (alphaCursor < alphaGop.size()) {
				const uint32_t timestamp = timestampFor(alphaCursor) + (alphaCursor == missingMateIndex ? 1u : 0u);
				source.feedNativeMediaTestVp9AccessUnit(true, alphaGop[alphaCursor], timestamp);
				++alphaCursor;
			}
			if (primaryCursor < primaryGop.size()) {
				source.feedNativeMediaTestVp9AccessUnit(false, primaryGop[primaryCursor], timestampFor(primaryCursor));
				++primaryCursor;
			}
		} else {
			usedPrimaryFirst = true;
			if (primaryCursor < primaryGop.size()) {
				source.feedNativeMediaTestVp9AccessUnit(false, primaryGop[primaryCursor], timestampFor(primaryCursor));
				++primaryCursor;
			}
			if (alphaCursor < alphaGop.size()) {
				const uint32_t timestamp = timestampFor(alphaCursor) + (alphaCursor == missingMateIndex ? 1u : 0u);
				source.feedNativeMediaTestVp9AccessUnit(true, alphaGop[alphaCursor], timestamp);
				++alphaCursor;
			}
		}
	}
	require(usedAlphaFirst && usedPrimaryFirst, "gate did not exercise both cross-track submission orders");

	const auto frames = output.copy();
	require(frames.size() >= 12, "delayed alpha pairing produced too few exact pairs");
	bool sawPreWrap = false;
	bool sawPostWrap = false;
	bool sawRecoveryAfterMissingMate = false;
	for (size_t index = 0; index < frames.size(); ++index) {
		const auto match = std::find_if(primaryGop.begin(), primaryGop.end(), [&](const auto &au) {
			const size_t candidate = static_cast<size_t>(&au - primaryGop.data());
			return candidate != missingMateIndex && timestampFor(candidate) == frames[index].rtpTimestamp;
		});
		require(match != primaryGop.end() && frames[index].hasAlpha,
		        "alpha output was not an exact primary/alpha RTP mate");
		require(frames[index].rtpTimestamp != timestampFor(missingMateIndex) &&
		            frames[index].rtpTimestamp != timestampFor(missingMateIndex) + 1u,
		        "missing primary/alpha mate was incorrectly composed");
		sawPreWrap = sawPreWrap || frames[index].rtpTimestamp >= baseTimestamp;
		sawPostWrap = sawPostWrap || frames[index].rtpTimestamp < baseTimestamp;
		sawRecoveryAfterMissingMate = sawRecoveryAfterMissingMate ||
		                              isRtpTimestampBefore(timestampFor(missingMateIndex), frames[index].rtpTimestamp);
		if (index > 0) {
			require(isRtpTimestampBefore(frames[index - 1].rtpTimestamp, frames[index].rtpTimestamp),
			        "drop/reorder/wrap sequence regressed RTP ordering");
			require(frames[index].outputTimestampNs > frames[index - 1].outputTimestampNs,
			        "serialized OBS timestamp did not remain monotonic across RTP wrap");
		}
	}
	require(sawPreWrap && sawPostWrap && sawRecoveryAfterMissingMate,
	        "exact alpha pairing did not demonstrate missing-mate recovery across RTP wrap");
}

void testTransitionFlushesBothPipelines(const std::vector<std::vector<uint8_t>> &primaryGop,
                                        const std::vector<std::vector<uint8_t>> &alphaGop)
{
	VDONinjaSource source(NativeMediaTestTag{});
	OutputCollector output;
	source.setNativeMediaTestOutputHook([&output](NativeMediaTestOutput frame) { output.add(std::move(frame)); });
	source.transitionNativeMediaTestPipeline(true);

	source.feedNativeMediaTestVp9Packet(false, {0x01, 0x02}, 3000, true, false);
	source.feedNativeMediaTestVp9Packet(true, {0x03, 0x04}, 3001, true, false);
	auto snapshot = source.nativeMediaTestSnapshot();
	require(snapshot.primaryAssemblyActive && snapshot.alphaAssemblyActive && snapshot.primaryAssemblyBytes == 2 &&
	            snapshot.alphaAssemblyBytes == 2,
	        "partial primary/alpha assemblies were not established");
	source.transitionNativeMediaTestPipeline(true); // Primary replacement uses the canonical two-track reset.
	requirePipelineEmpty(source, "primary replacement");

	for (size_t index = 0; index < 24; ++index) {
		const uint32_t timestamp = 40000u + static_cast<uint32_t>(index) * 3000u;
		source.feedNativeMediaTestVp9AccessUnit(false, primaryGop[index], timestamp);
		source.feedNativeMediaTestVp9AccessUnit(true, alphaGop[index], timestamp + 1u);
	}
	snapshot = source.nativeMediaTestSnapshot();
	require(snapshot.primaryDecoderAllocated && snapshot.alphaDecoderAllocated && snapshot.pendingPrimaryFrames > 0 &&
	            snapshot.pendingAlphaFrames > 0,
	        "independent decoded primary/alpha state was not established");
	source.transitionNativeMediaTestPipeline(false); // Alpha close/removal flushes both decoders and pair queues.
	requirePipelineEmpty(source, "alpha close");
	size_t primaryCursor = 0;
	feedUntilOutputCount(source, output, primaryGop, primaryCursor, 420000, 1);
	require(output.size() == 1, "opaque primary did not recover after alpha close reset");
}

void testEveryEpochAdmissionStageDropsStale(const std::vector<std::vector<uint8_t>> &primaryGop,
                                            const std::vector<std::vector<uint8_t>> &alphaGop)
{
	const std::vector<NativeMediaTestStage> stages = {NativeMediaTestStage::Identity, NativeMediaTestStage::PreAssembly,
	                                                  NativeMediaTestStage::PreDecode, NativeMediaTestStage::PrePair,
	                                                  NativeMediaTestStage::PreOutput};

	for (const NativeMediaTestStage stage : stages) {
		for (const bool alpha : {false, true}) {
			VDONinjaSource source(NativeMediaTestTag{});
			OutputCollector output;
			source.setNativeMediaTestOutputHook(
			    [&output](NativeMediaTestOutput frame) { output.add(std::move(frame)); });
			source.transitionNativeMediaTestPipeline(true);
			const uint32_t baseTimestamp = 500000u + static_cast<uint32_t>(stage) * 200000u + (alpha ? 100000u : 0u);
			size_t primaryCursor = 0;
			size_t alphaCursor = 0;

			if (stage == NativeMediaTestStage::PrePair) {
				for (size_t index = 0; index < 20; ++index) {
					if (alpha) {
						feedNextGopFrame(source, true, alphaGop, alphaCursor, baseTimestamp);
					} else {
						feedNextGopFrame(source, false, primaryGop, primaryCursor, baseTimestamp);
					}
				}
			} else if (stage == NativeMediaTestStage::PreOutput) {
				for (size_t index = 0; index < 20; ++index) {
					const uint32_t timestamp = baseTimestamp + static_cast<uint32_t>(index) * 3000u;
					source.feedNativeMediaTestVp9AccessUnit(false, primaryGop[primaryCursor++], timestamp);
					source.feedNativeMediaTestVp9AccessUnit(true, alphaGop[alphaCursor++], timestamp);
				}
				if (alpha) {
					feedNextGopFrame(source, false, primaryGop, primaryCursor, baseTimestamp);
				} else {
					feedNextGopFrame(source, true, alphaGop, alphaCursor, baseTimestamp);
				}
				const auto mateSnapshot = source.nativeMediaTestSnapshot();
				require(alpha ? mateSnapshot.pendingPrimaryFrames > 0 : mateSnapshot.pendingAlphaFrames > 0,
				        "pre-output stale gate did not queue the exact counterpart first");
			}
			const size_t baselineOutputs = output.size();

			StageLatch latch(stage, alpha);
			source.setNativeMediaTestStageHook(
			    [&latch](NativeMediaTestStage currentStage, bool currentAlpha, uint32_t currentTimestamp,
			             uint64_t epoch) { latch.hook(currentStage, currentAlpha, currentTimestamp, epoch); });

			const size_t inputIndex = alpha ? alphaCursor++ : primaryCursor++;
			const uint32_t inputTimestamp = baseTimestamp + static_cast<uint32_t>(inputIndex) * 3000u;
			std::thread worker([&]() {
				source.feedNativeMediaTestVp9AccessUnit(alpha, alpha ? alphaGop[inputIndex] : primaryGop[inputIndex],
				                                        inputTimestamp);
			});
			latch.waitUntilEntered();
			const uint64_t staleEpoch = source.nativeMediaTestEpoch();
			source.transitionNativeMediaTestPipeline(true);
			require(source.nativeMediaTestEpoch() != staleEpoch, "media transition did not advance callback epoch");
			latch.release();
			worker.join();
			source.setNativeMediaTestStageHook(nullptr);

			require(output.size() == baselineOutputs, "stale callback emitted after a canonical media transition");
			requirePipelineEmpty(source, "stale stage callback");
		}
	}
}

void testSendPacketEagainDrainAndRetry(const std::vector<std::vector<uint8_t>> &primaryGop)
{
	VDONinjaSource source(NativeMediaTestTag{});
	OutputCollector output;
	source.setNativeMediaTestOutputHook([&output](NativeMediaTestOutput frame) { output.add(std::move(frame)); });
	source.transitionNativeMediaTestPipeline(false);

	std::atomic<int> sendCalls{0};
	std::atomic<int> receiveCalls{0};
	const AVPacket *firstPacket = nullptr;
	const uint8_t *firstData = nullptr;
	int firstSize = 0;
	int64_t firstPts = AV_NOPTS_VALUE;
	int64_t firstDts = AV_NOPTS_VALUE;
	std::vector<uint8_t> firstPayload;
	bool retriedSamePacket = false;
	std::vector<std::string> events;
	source.setNativeMediaTestVideoDecoderHooks(
	    [&](AVCodecContext *decoder, const AVPacket *packet) {
		    const int call = ++sendCalls;
		    events.push_back(call == 1 ? "send-eagain" : "send-retry");
		    if (call == 1) {
			    firstPacket = packet;
			    firstData = packet->data;
			    firstSize = packet->size;
			    firstPts = packet->pts;
			    firstDts = packet->dts;
			    firstPayload.assign(packet->data, packet->data + packet->size);
			    return AVERROR(EAGAIN);
		    }
		    retriedSamePacket = packet == firstPacket && packet->data == firstData && packet->size == firstSize &&
		                        packet->pts == firstPts && packet->dts == firstDts &&
		                        firstPayload.size() == static_cast<size_t>(packet->size) &&
		                        std::memcmp(firstPayload.data(), packet->data, firstPayload.size()) == 0;
		    return avcodec_send_packet(decoder, packet);
	    },
	    [&](AVCodecContext *decoder, AVFrame *frame) {
		    ++receiveCalls;
		    const int result = avcodec_receive_frame(decoder, frame);
		    events.push_back(result == AVERROR(EAGAIN) ? "receive-eagain" : "receive-other");
		    return result;
	    });

	size_t cursor = 0;
	const uint32_t firstTimestamp = feedNextGopFrame(source, false, primaryGop, cursor, 6000);
	require(sendCalls.load() == 2, "EAGAIN path did not retry exactly once");
	require(receiveCalls.load() == 2, "EAGAIN path did not drain exactly to EAGAIN before and after retry");
	const std::vector<std::string> expectedEvents = {"send-eagain", "receive-eagain", "send-retry", "receive-eagain"};
	require(events == expectedEvents, "EAGAIN path did not execute send, drain-to-EAGAIN, exact retry, drain");
	require(retriedSamePacket, "EAGAIN path changed packet pointer/data/size/PTS/DTS/payload before its single retry");
	source.setNativeMediaTestVideoDecoderHooks(nullptr, nullptr);
	feedUntilOutputCount(source, output, primaryGop, cursor, 6000, 1);
	require(output.copy().front().rtpTimestamp == firstTimestamp,
	        "EAGAIN drain/retry path lost the accepted packet's originating RTP PTS");
	require(source.nativeMediaTestSnapshot().retainedVideoFrames == 0,
	        "retained decoded AVFrame leaked after EAGAIN recovery");
}

void testAlphaSendPacketEagainDrainAndRetry(const std::vector<std::vector<uint8_t>> &primaryGop,
                                            const std::vector<std::vector<uint8_t>> &alphaGop)
{
	VDONinjaSource source(NativeMediaTestTag{});
	OutputCollector output;
	source.setNativeMediaTestOutputHook([&output](NativeMediaTestOutput frame) { output.add(std::move(frame)); });
	source.transitionNativeMediaTestPipeline(true);

	std::atomic<int> sendCalls{0};
	std::atomic<int> receiveCalls{0};
	const AVPacket *firstPacket = nullptr;
	const uint8_t *firstData = nullptr;
	int firstSize = 0;
	int64_t firstPts = AV_NOPTS_VALUE;
	int64_t firstDts = AV_NOPTS_VALUE;
	std::vector<uint8_t> firstPayload;
	bool retriedSamePacket = false;
	std::vector<std::string> firstEvents;
	source.setNativeMediaTestAlphaDecoderHooks(
	    [&](AVCodecContext *decoder, const AVPacket *packet) {
		    const int call = ++sendCalls;
		    if (firstEvents.size() < 4) {
			    firstEvents.push_back(call == 1 ? "send-eagain" : "send-retry");
		    }
		    if (call == 1) {
			    firstPacket = packet;
			    firstData = packet->data;
			    firstSize = packet->size;
			    firstPts = packet->pts;
			    firstDts = packet->dts;
			    firstPayload.assign(packet->data, packet->data + packet->size);
			    return AVERROR(EAGAIN);
		    }
		    if (call == 2) {
			    retriedSamePacket = packet == firstPacket && packet->data == firstData && packet->size == firstSize &&
			                        packet->pts == firstPts && packet->dts == firstDts &&
			                        firstPayload.size() == static_cast<size_t>(packet->size) &&
			                        std::memcmp(firstPayload.data(), packet->data, firstPayload.size()) == 0;
		    }
		    return avcodec_send_packet(decoder, packet);
	    },
	    [&](AVCodecContext *decoder, AVFrame *frame) {
		    ++receiveCalls;
		    const int result = avcodec_receive_frame(decoder, frame);
		    if (firstEvents.size() < 4) {
			    firstEvents.push_back(result == AVERROR(EAGAIN) ? "receive-eagain" : "receive-other");
		    }
		    return result;
	    });

	constexpr uint32_t baseTimestamp = 26000;
	source.feedNativeMediaTestVp9AccessUnit(true, alphaGop[0], baseTimestamp);
	require(sendCalls.load() == 2, "alpha EAGAIN path did not retry exactly once");
	require(receiveCalls.load() == 2, "alpha EAGAIN path did not drain exactly to EAGAIN before and after retry");
	const std::vector<std::string> expectedEvents = {"send-eagain", "receive-eagain", "send-retry", "receive-eagain"};
	require(firstEvents == expectedEvents,
	        "alpha EAGAIN path did not execute send, drain-to-EAGAIN, exact retry, drain");
	require(retriedSamePacket, "alpha EAGAIN path changed packet identity before its single retry");

	source.feedNativeMediaTestVp9AccessUnit(false, primaryGop[0], baseTimestamp);
	for (size_t index = 1; output.size() == 0 && index < std::min(primaryGop.size(), alphaGop.size()); ++index) {
		const uint32_t timestamp = baseTimestamp + static_cast<uint32_t>(index) * 3000u;
		source.feedNativeMediaTestVp9AccessUnit(true, alphaGop[index], timestamp);
		source.feedNativeMediaTestVp9AccessUnit(false, primaryGop[index], timestamp);
	}
	require(output.size() > 0, "alpha EAGAIN recovery never reached an exact primary/alpha mate");
	const auto frames = output.copy();
	require(frames.front().hasAlpha && frames.front().rtpTimestamp == baseTimestamp,
	        "alpha EAGAIN recovery did not preserve the accepted packet RTP PTS through exact mate pairing");
	source.setNativeMediaTestAlphaDecoderHooks(nullptr, nullptr);
}

void testOverlappingDecodedCallbacksSerializeFinalOutput(const std::vector<std::vector<uint8_t>> &primaryGop)
{
	VDONinjaSource source(NativeMediaTestTag{});
	OutputCollector output;
	source.setNativeMediaTestOutputHook([&output](NativeMediaTestOutput frame) { output.add(std::move(frame)); });
	source.transitionNativeMediaTestPipeline(false);
	size_t cursor = 0;
	constexpr uint32_t baseTimestamp = 7000;
	feedUntilOutputCount(source, output, primaryGop, cursor, baseTimestamp, 1);

	StageLatch olderFrameLatch(NativeMediaTestStage::PrePair, false);
	source.setNativeMediaTestStageHook(
	    [&olderFrameLatch](NativeMediaTestStage stage, bool alpha, uint32_t timestamp, uint64_t epoch) {
		    olderFrameLatch.hook(stage, alpha, timestamp, epoch);
	    });
	const size_t olderInputIndex = cursor++;
	std::thread olderCallback([&]() {
		source.feedNativeMediaTestVp9AccessUnit(false, primaryGop[olderInputIndex],
		                                        baseTimestamp + static_cast<uint32_t>(olderInputIndex) * 3000u);
	});
	olderFrameLatch.waitUntilEntered();

	// The newer callback decodes and outputs while the older decoded AVFrame is
	// retained outside videoDecodeMutex_. Final output serialization must accept
	// the newer timestamp and reject the late older callback.
	feedNextGopFrame(source, false, primaryGop, cursor, baseTimestamp);
	olderFrameLatch.release();
	olderCallback.join();
	source.setNativeMediaTestStageHook(nullptr);

	const auto frames = output.copy();
	require(frames.size() == 2 && frames.back().rtpTimestamp == baseTimestamp + 6000u,
	        "overlapping decoded callbacks bypassed serialized RTP output ordering");
	require(source.nativeMediaTestSnapshot().retainedVideoFrames == 0,
	        "overlapping decoded callback retained an AVFrame after final output");
}

void testLinkedGateUsesShippedDecoderThreading(const std::vector<uint8_t> &primaryAu,
                                               const std::vector<uint8_t> &alphaAu)
{
	VDONinjaSource source(NativeMediaTestTag{});
	OutputCollector output;
	source.setNativeMediaTestOutputHook([&output](NativeMediaTestOutput frame) { output.add(std::move(frame)); });
	source.transitionNativeMediaTestPipeline(true);
	source.feedNativeMediaTestVp9AccessUnit(false, primaryAu, 9000);
	source.feedNativeMediaTestVp9AccessUnit(true, alphaAu, 9000);

	const auto snapshot = source.nativeMediaTestSnapshot();
	require(snapshot.primaryRequestedThreadCount == 0, "linked gate changed the shipped primary decoder thread count");
	require((snapshot.primaryRequestedThreadType & FF_THREAD_FRAME) != 0,
	        "linked gate disabled shipped primary frame threading");
	require(snapshot.alphaRequestedThreadCount == 0, "linked gate changed the shipped alpha decoder thread count");
	require((snapshot.alphaRequestedThreadType & FF_THREAD_FRAME) != 0,
	        "linked gate disabled shipped alpha frame threading");
	require((snapshot.primaryActiveThreadType & FF_THREAD_FRAME) != 0,
	        "primary decoder did not activate shipped frame threading");
	require((snapshot.alphaActiveThreadType & FF_THREAD_FRAME) != 0,
	        "alpha decoder did not activate shipped frame threading");
}

void testFrameThreadedDecodePreservesRtpPts(const std::vector<std::vector<uint8_t>> &primaryGop)
{
	VDONinjaSource source(NativeMediaTestTag{});
	OutputCollector output;
	source.setNativeMediaTestOutputHook([&output](NativeMediaTestOutput frame) { output.add(std::move(frame)); });
	source.transitionNativeMediaTestPipeline(false);
	std::vector<uint32_t> submitted;
	for (size_t index = 0; index < primaryGop.size(); ++index) {
		const uint32_t timestamp = 100000u + static_cast<uint32_t>(index) * 3000u;
		submitted.push_back(timestamp);
		source.feedNativeMediaTestVp9AccessUnit(false, primaryGop[index], timestamp);
	}

	const auto frames = output.copy();
	require(frames.size() >= 8, "shipped frame-threaded decoder did not produce enough delayed VP9 output");
	require(frames.size() < submitted.size(), "frame-threaded gate did not exercise decoder-retained delayed output");
	for (size_t index = 0; index < frames.size(); ++index) {
		require(frames[index].rtpTimestamp == submitted[index],
		        "delayed frame-threaded decode lost its originating RTP PTS");
		if (index > 0) {
			require(frames[index].outputTimestampNs > frames[index - 1].outputTimestampNs,
			        "delayed frame-threaded output timestamp was not monotonic");
		}
	}
}

void testSuppressionSerializesWithFinalCommit(const std::vector<std::vector<uint8_t>> &primaryGop)
{
	VDONinjaSource source(NativeMediaTestTag{});
	OutputCollector output;
	std::atomic<int> clears{0};
	source.setNativeMediaTestOutputHook([&output](NativeMediaTestOutput frame) { output.add(std::move(frame)); });
	source.transitionNativeMediaTestPipeline(false);
	size_t cursor = 0;
	feedUntilOutputCount(source, output, primaryGop, cursor, 10000, 1);
	source.setNativeMediaTestClearOutputHook(
	    [&clears](bool hadVideo, std::string) { clears.fetch_add(hadVideo ? 1 : 0, std::memory_order_relaxed); });

	StageLatch commitLatch(NativeMediaTestStage::PreCommit, false);
	StageSignal mutePublished(NativeMediaTestStage::MutePublished);
	source.setNativeMediaTestStageHook(
	    [&commitLatch, &mutePublished](NativeMediaTestStage stage, bool alpha, uint32_t timestamp, uint64_t epoch) {
		    commitLatch.hook(stage, alpha, timestamp, epoch);
		    mutePublished.hook(stage);
	    });
	std::thread decodedFrame([&]() { feedNextGopFrame(source, false, primaryGop, cursor, 10000); });
	commitLatch.waitUntilEntered();
	std::thread suppression([&]() { source.applyNativeMediaTestVideoSuppression(true); });
	mutePublished.waitUntilReached();
	commitLatch.release();
	decodedFrame.join();
	suppression.join();
	source.setNativeMediaTestStageHook(nullptr);

	require(output.size() == 1, "a parked decoded frame emitted after remote video suppression");
	require(clears.load(std::memory_order_relaxed) == 1, "suppression did not clear active output exactly once");
	feedNextGopFrame(source, false, primaryGop, cursor, 10000);
	require(output.size() == 1, "muted video emitted after suppression completed");
	source.applyNativeMediaTestVideoSuppression(false);
	feedUntilOutputCount(source, output, primaryGop, cursor, 10000, 2);
	require(output.size() == 2, "video output did not recover after suppression was removed");
}

void testSuppressionAggregateCannotPublishStaleComponentSnapshot()
{
	{
		VDONinjaSource source(NativeMediaTestTag{});
		StageLatch firstCommit(NativeMediaTestStage::SuppressionAttempt, false);
		StageCounter requests(NativeMediaTestStage::SuppressionRequest);
		source.setNativeMediaTestStageHook(
		    [&firstCommit, &requests](NativeMediaTestStage stage, bool alpha, uint32_t timestamp, uint64_t epoch) {
			    requests.hook(stage);
			    firstCommit.hook(stage, alpha, timestamp, epoch);
		    });

		std::thread firstTrue([&]() { source.applyNativeMediaTestVideoSuppression(true); });
		firstCommit.waitUntilEntered();
		std::thread latestFalse([&]() { source.applyNativeMediaTestVideoSuppression(false); });
		requests.waitUntilCount(2);
		firstCommit.release();
		firstTrue.join();
		latestFalse.join();
		source.setNativeMediaTestStageHook(nullptr);

		const auto snapshot = source.nativeMediaTestSnapshot();
		require(!snapshot.mediaVideoMuted && !snapshot.videoSuppressed,
		        "serialized true-then-false suppression did not preserve the latest false state");
	}

	{
		VDONinjaSource source(NativeMediaTestTag{});
		source.applyNativeMediaTestVideoSuppression(true);
		StageLatch firstCommit(NativeMediaTestStage::SuppressionAttempt, false);
		StageCounter requests(NativeMediaTestStage::SuppressionRequest);
		source.setNativeMediaTestStageHook(
		    [&firstCommit, &requests](NativeMediaTestStage stage, bool alpha, uint32_t timestamp, uint64_t epoch) {
			    requests.hook(stage);
			    firstCommit.hook(stage, alpha, timestamp, epoch);
		    });
		std::thread firstFalse([&]() { source.applyNativeMediaTestVideoSuppression(false); });
		firstCommit.waitUntilEntered();
		std::thread latestTrue([&]() { source.applyNativeMediaTestVideoSuppression(true); });
		requests.waitUntilCount(2);
		firstCommit.release();
		firstFalse.join();
		latestTrue.join();
		source.setNativeMediaTestStageHook(nullptr);

		const auto snapshot = source.nativeMediaTestSnapshot();
		require(snapshot.mediaVideoMuted && snapshot.videoSuppressed,
		        "serialized false-then-true suppression did not preserve the latest true state");
	}
}

void testSuppressionComponentsAndResetRemainCoherent()
{
	VDONinjaSource source(NativeMediaTestTag{});
	ReceiverVideoSuppressionUpdate media;
	media.hasMediaVideoMuted = true;
	media.mediaVideoMuted = true;
	ReceiverVideoSuppressionUpdate director;
	director.hasDirectorVideoMuted = true;
	director.directorVideoMuted = true;
	ReceiverVideoSuppressionUpdate virtualHangup;
	virtualHangup.hasVirtualHangup = true;
	virtualHangup.virtualHangup = true;

	source.applyNativeMediaTestVideoSuppressionUpdate(media);
	source.applyNativeMediaTestVideoSuppressionUpdate(director);
	source.applyNativeMediaTestVideoSuppressionUpdate(virtualHangup);
	auto snapshot = source.nativeMediaTestSnapshot();
	require(snapshot.mediaVideoMuted && snapshot.directorVideoMuted && snapshot.virtualHangup &&
	            snapshot.videoSuppressed,
	        "media/director/virtual suppression components did not publish one coherent aggregate");

	media.mediaVideoMuted = false;
	source.applyNativeMediaTestVideoSuppressionUpdate(media);
	director.directorVideoMuted = false;
	source.applyNativeMediaTestVideoSuppressionUpdate(director);
	snapshot = source.nativeMediaTestSnapshot();
	require(!snapshot.mediaVideoMuted && !snapshot.directorVideoMuted && snapshot.virtualHangup &&
	            snapshot.videoSuppressed,
	        "clearing overlapping media/director state incorrectly cleared virtual-hangup suppression");
	virtualHangup.virtualHangup = false;
	source.applyNativeMediaTestVideoSuppressionUpdate(virtualHangup);
	snapshot = source.nativeMediaTestSnapshot();
	require(!snapshot.mediaVideoMuted && !snapshot.directorVideoMuted && !snapshot.virtualHangup &&
	            !snapshot.videoSuppressed,
	        "final overlapping suppression component did not coherently restore output");

	StageLatch suppressionCommit(NativeMediaTestStage::SuppressionAttempt, false);
	StageSignal resetRequest(NativeMediaTestStage::SuppressionResetRequest);
	source.setNativeMediaTestStageHook([&suppressionCommit, &resetRequest](NativeMediaTestStage stage, bool alpha,
	                                                                       uint32_t timestamp, uint64_t epoch) {
		resetRequest.hook(stage);
		suppressionCommit.hook(stage, alpha, timestamp, epoch);
	});
	std::thread suppression([&]() { source.applyNativeMediaTestVideoSuppression(true); });
	suppressionCommit.waitUntilEntered();
	std::thread reset([&]() { source.resetNativeMediaTestState(); });
	resetRequest.waitUntilReached();
	suppressionCommit.release();
	suppression.join();
	reset.join();
	source.setNativeMediaTestStageHook(nullptr);

	snapshot = source.nativeMediaTestSnapshot();
	require(!snapshot.mediaVideoMuted && !snapshot.directorVideoMuted && !snapshot.virtualHangup &&
	            !snapshot.videoSuppressed,
	        "serialized reset left suppression components and authoritative aggregate inconsistent");
}

void testFinalOutputHoldsMuteCommitState(const std::vector<std::vector<uint8_t>> &primaryGop)
{
	VDONinjaSource source(NativeMediaTestTag{});
	OutputCollector output;
	source.setNativeMediaTestOutputHook([&output](NativeMediaTestOutput frame) { output.add(std::move(frame)); });
	source.transitionNativeMediaTestPipeline(false);
	size_t cursor = 0;
	feedUntilOutputCount(source, output, primaryGop, cursor, 22000, 1);

	StageLatch commitLatch(NativeMediaTestStage::CommitAuthorized, false);
	source.setNativeMediaTestStageHook(
	    [&commitLatch](NativeMediaTestStage stage, bool alpha, uint32_t timestamp, uint64_t epoch) {
		    commitLatch.hook(stage, alpha, timestamp, epoch);
	    });
	std::thread decodedFrame([&]() { feedNextGopFrame(source, false, primaryGop, cursor, 22000); });
	commitLatch.waitUntilEntered();
	const bool commitStateWasAvailable = source.nativeMediaTestCanAcquireVideoCommitState();
	commitLatch.release();
	decodedFrame.join();
	source.setNativeMediaTestStageHook(nullptr);
	require(!commitStateWasAvailable,
	        "final output authorization did not hold the mute commit state through the actual output hook");
}

void testOutputLinearizesBeforeMuteAndClear(const std::vector<std::vector<uint8_t>> &primaryGop)
{
	for (int iteration = 0; iteration < 12; ++iteration) {
		VDONinjaSource source(NativeMediaTestTag{});
		OutputCollector output;
		std::atomic<int> clears{0};
		source.setNativeMediaTestOutputHook([&output](NativeMediaTestOutput frame) { output.add(std::move(frame)); });
		source.transitionNativeMediaTestPipeline(false);
		size_t cursor = 0;
		const uint32_t baseTimestamp = 30000u + static_cast<uint32_t>(iteration) * 100000u;
		feedUntilOutputCount(source, output, primaryGop, cursor, baseTimestamp, 1);

		EventRecorder order;
		StageLatch commitLatch(NativeMediaTestStage::CommitAuthorized, false);
		StageSignal suppressionRequest(NativeMediaTestStage::SuppressionRequest);
		source.setNativeMediaTestOutputHook([&output, &order](NativeMediaTestOutput frame) {
			order.add("output");
			output.add(std::move(frame));
		});
		source.setNativeMediaTestClearOutputHook([&clears, &order](bool hadVideo, std::string) {
			if (hadVideo) {
				clears.fetch_add(1, std::memory_order_relaxed);
				order.add("clear");
			}
		});
		source.setNativeMediaTestStageHook([&commitLatch, &suppressionRequest, &order](NativeMediaTestStage stage,
		                                                                               bool alpha, uint32_t timestamp,
		                                                                               uint64_t epoch) {
			commitLatch.hook(stage, alpha, timestamp, epoch);
			if (stage == NativeMediaTestStage::SuppressionAttempt) {
				order.add("mute-attempt");
			} else if (stage == NativeMediaTestStage::MutePublished) {
				order.add("mute-published");
			}
			suppressionRequest.hook(stage);
		});

		std::thread decodedFrame([&]() { feedNextGopFrame(source, false, primaryGop, cursor, baseTimestamp); });
		commitLatch.waitUntilEntered();
		std::thread suppression([&]() { source.applyNativeMediaTestVideoSuppression(true); });
		suppressionRequest.waitUntilReached();
		commitLatch.release();
		decodedFrame.join();
		suppression.join();
		source.setNativeMediaTestStageHook(nullptr);

		const auto events = order.copy();
		const auto outputIt = std::find(events.begin(), events.end(), "output");
		const auto muteIt = std::find(events.begin(), events.end(), "mute-published");
		const auto clearIt = std::find(events.begin(), events.end(), "clear");
		require(outputIt != events.end() && muteIt != events.end() && clearIt != events.end() && outputIt < muteIt &&
		            muteIt < clearIt,
		        "inverse mute ordering was not output commit, mute publication, then exactly one clear");
		require(output.size() == 2 && clears.load(std::memory_order_relaxed) == 1,
		        "inverse mute ordering emitted/cleared an unexpected number of frames");
		feedNextGopFrame(source, false, primaryGop, cursor, baseTimestamp);
		require(output.size() == 2, "frame emitted after inverse-order suppression completed");
	}
}

void testStallClearRechecksFreshCommit(const std::vector<std::vector<uint8_t>> &primaryGop)
{
	VDONinjaSource source(NativeMediaTestTag{});
	OutputCollector output;
	std::atomic<int> clears{0};
	source.setNativeMediaTestOutputHook([&output](NativeMediaTestOutput frame) { output.add(std::move(frame)); });
	source.transitionNativeMediaTestPipeline(false);
	size_t cursor = 0;
	feedUntilOutputCount(source, output, primaryGop, cursor, 14000, 1);
	source.setNativeMediaTestClearOutputHook(
	    [&clears](bool hadVideo, std::string) { clears.fetch_add(hadVideo ? 1 : 0, std::memory_order_relaxed); });
	source.ageNativeMediaTestVideoOutput(5000);

	StageLatch staleDecisionLatch(NativeMediaTestStage::PreStallClear, false);
	source.setNativeMediaTestStageHook(
	    [&staleDecisionLatch](NativeMediaTestStage stage, bool alpha, uint32_t timestamp, uint64_t epoch) {
		    staleDecisionLatch.hook(stage, alpha, timestamp, epoch);
	    });
	std::thread ticker([&]() { source.videoTick(0.0f); });
	staleDecisionLatch.waitUntilEntered();
	feedNextGopFrame(source, false, primaryGop, cursor, 14000);
	staleDecisionLatch.release();
	ticker.join();
	source.setNativeMediaTestStageHook(nullptr);

	const auto freshSnapshot = source.nativeMediaTestSnapshot();
	require(clears.load(std::memory_order_relaxed) == 0 && freshSnapshot.videoOutputActive,
	        "stale tick decision cleared a freshly committed video frame");
	source.ageNativeMediaTestVideoOutput(5000);
	source.videoTick(0.0f);
	source.videoTick(0.0f);
	const auto expiredSnapshot = source.nativeMediaTestSnapshot();
	require(clears.load(std::memory_order_relaxed) == 1 && !expiredSnapshot.videoOutputActive,
	        "truly expired output was not cleared exactly once");
}

void testDimensionPublicationIsCoherent(const std::vector<std::vector<uint8_t>> &primaryGop)
{
	VDONinjaSource source(NativeMediaTestTag{});
	OutputCollector output;
	source.setNativeMediaTestOutputHook([&output](NativeMediaTestOutput frame) { output.add(std::move(frame)); });
	source.transitionNativeMediaTestPipeline(false);
	size_t cursor = 0;
	feedUntilOutputCount(source, output, primaryGop, cursor, 16000, 1);
	require(output.size() == 1 && output.copy().front().width == 16 && output.copy().front().height == 16,
	        "dimension gate did not establish the old coherent output size");

	StageLatch dimensionLatch(NativeMediaTestStage::DimensionUpdateMidpoint, false);
	source.setNativeMediaTestStageHook(
	    [&dimensionLatch](NativeMediaTestStage stage, bool alpha, uint32_t timestamp, uint64_t epoch) {
		    dimensionLatch.hook(stage, alpha, timestamp, epoch);
	    });
	std::thread updater([&]() { source.updateNativeMediaTestDimensions(32, 24); });
	dimensionLatch.waitUntilEntered();
	feedNextGopFrame(source, false, primaryGop, cursor, 16000);
	dimensionLatch.release();
	updater.join();
	source.setNativeMediaTestStageHook(nullptr);
	feedNextGopFrame(source, false, primaryGop, cursor, 16000);

	const auto frames = output.copy();
	require(frames.size() == 3, "dimension publication gate emitted an unexpected frame count");
	require(frames[1].width == 16 && frames[1].height == 16,
	        "concurrent dimension update exposed a torn old/new output size");
	require(frames[2].width == 32 && frames[2].height == 24,
	        "coherent new dimensions were not published after the update");
}

void testSymmetricOwnershipAndDeferredAdoption()
{
	std::string primaryPeer;
	std::string alphaPeer;
	require(mediaTrackPeerCanOwn("peer-a", primaryPeer, alphaPeer), "alpha-first peer could not claim empty media");
	alphaPeer = "peer-a";
	require(!mediaTrackPeerCanOwn("peer-b", primaryPeer, alphaPeer),
	        "video from peer B was allowed to mix with alpha from peer A");
	const std::string deferredPrimaryPeer = "peer-b";
	alphaPeer.clear();
	require(mediaTrackPeerCanOwn(deferredPrimaryPeer, primaryPeer, alphaPeer),
	        "deferred video peer B could not be adopted after peer A retired");
	primaryPeer = deferredPrimaryPeer;
	require(mediaTrackPeerCanOwn("peer-b", primaryPeer, alphaPeer),
	        "alpha from adopted peer B could not join its primary track");
	require(!mediaTrackPeerCanOwn("peer-a", primaryPeer, alphaPeer),
	        "retired peer A could re-enter adopted peer B media ownership");
}

} // namespace

int main(int argc, char **argv)
{
	struct GateCase {
		const char *name;
		std::function<void()> run;
	};
	struct RtcCleanupGuard {
		~RtcCleanupGuard() { rtc::Cleanup().wait(); }
	} rtcCleanup;

	try {
		const std::vector<uint8_t> primaryAu = encodeVp9Keyframe(96);
		const std::vector<uint8_t> alphaAu = encodeVp9Keyframe(72);
		const auto primaryGop = encodeVp9Gop(80, 48);
		const auto alphaGop = encodeVp9Gop(48, 48);
		const std::vector<GateCase> cases = {
		    {"RTC manager-to-source stale alpha add rejection",
		     testRtcManagerSourceRejectsLateAlphaAfterInactiveRemoval},
		    {"RTC manager-to-source primary/alpha replacement ordering", testRtcManagerSourceReplacementOrdering},
		    {"RTC stale add preserves re-added identical track callbacks",
		     testRtcStaleAddCannotClearReaddedIdenticalTrackCallbacks},
		    {"RTC manager-to-source primary/alpha terminal retirement ordering",
		     testRtcManagerSourceTerminalRetirementOrdering},
		    {"RTC manager-to-source same-UUID session rotation ordering",
		     testRtcManagerSourceSameUuidSessionRotationOrdering},
		    {"manager-owned Track close preserves whole-peer ownership",
		     testManagerOwnedTrackClosePreservesWholePeerOwnership},
		    {"renegotiation recreates closed manager-owned tracks", testRenegotiationRecreatesClosedManagerOwnedTracks},
		    {"Track error and close retire exactly once then renegotiate",
		     testTrackErrorAndCloseRetireExactlyOnceAndRenegotiate},
		    {"stale Track terminal lease cannot clear re-added handle",
		     testStaleTrackTerminalLeaseCannotClearReaddedHandle},
		    {"Track close during lifecycle install suppresses add", testTrackCloseDuringLifecycleInstallSuppressesAdd},
		    {"stale pre-install Track callbacks are cleaned", testStalePreInstallTrackCallbacksAreCleaned},
		    {"Track callback cleanup never holds manager locks", testTrackCallbackCleanupNeverHoldsManagerLocks},
		    {"Track in-flight callback is safe during manager destruction",
		     testTrackInFlightCallbackIsSafeDuringManagerDestruction},
		    {"completion delayed until after owner shutdown is rejected",
		     testCompletionDelayedUntilAfterOwnerShutdownIsRejected},
		    {"installed function registration failure detaches setter",
		     testInstalledFunctionRegistrationFailureDetachesSetter},
		    {"video alpha audio Track completions share one owner session",
		     testVideoAlphaAudioTrackCompletionsShareOwnerSession},
		    {"source reset preserves manager Track lifecycle ownership",
		     testSourceResetPreservesManagerTrackLifecycleOwnership},
		    {"interleaved pending peer bundles never mix", testInterleavedPendingPeerBundlesNeverMix},
		    {"pending peer bundle blocks packets until atomic publish",
		     testPendingPeerBundleBlocksPacketsUntilAtomicPublish},
		    {"old generation disconnect preserves current same-UUID media",
		     testOldGenerationDisconnectCannotClearNewGenerationMedia},
		    {"first new-generation event atomically retires old media",
		     testFirstNewGenerationEventAtomicallyRetiresOldMedia},
		    {"first new-generation event resets prior mute state",
		     [&]() { testFirstNewGenerationEventResetsPriorMuteState(primaryGop); }},
		    {"concurrent generation-start and current state linearize",
		     testConcurrentGenerationStartAndCurrentStateLinearize},
		    {"old generation data bye/mute preserve current same-UUID state",
		     testOldGenerationDataMessagesCannotMutateNewGeneration},
		    {"terminal generation rejects parked equal-generation events",
		     testTerminalGenerationRejectsParkedEqualGenerationEvents},
		    {"terminal adoption publishes new owner default state",
		     [&]() { testTerminalAdoptionPublishesNewOwnerDefaultState(primaryGop); }},
		    {"terminal adoption preserves stored new owner state", testTerminalAdoptionPreservesStoredNewOwnerState},
		    {"no-subscriber retirement preserves re-added Track callbacks",
		     testNoSubscriberRetirementCannotClearReaddedTrackCallbacks},
		    {"sessionless signaling cleanup cannot rebind after generation reuse",
		     testSessionlessSignalingCleanupDoesNotRebindAfterGenerationReuse},
		    {"manager provenance rejects sessionless cleanup after unobserved UUID reuse",
		     testManagerProvenanceRejectsSessionlessCleanupAfterUnobservedUuidReuse},
		    {"parked old streamRemoved lifecycle preserves replacement generation",
		     testParkedOldStreamRemovedLifecycleCannotRetireReplacementGeneration},
		    {"current lifecycle shapes retire exactly once without legacy competition",
		     testCurrentLifecycleShapesRetireExactlyOnceWithoutLegacyCompetition},
		    {"current socket lifecycle event linearizes outside manager locks",
		     testCurrentSocketLifecycleEventLinearizesOutsideManagerLocks},
		    {"unique sessionless streamRemoved lifecycle is accepted",
		     testUniqueSessionlessStreamRemovedLifecycleIsAccepted},
		    {"ambiguous sessionless streamRemoved cannot rebind after UUID reuse",
		     testAmbiguousSessionlessStreamRemovedCannotRebindAfterReuse},
		    {"UUID-less streamRemoved cannot rebind the current stream owner",
		     testUuidlessStreamRemovedCannotRebindCurrentOwner},
		    {"stale socket epoch streamRemoved is rejected after source reset",
		     testStaleSocketEpochStreamRemovedRejectedAfterSourceReset},
		    {"delayed DataChannel open preserves replacement generation",
		     testDelayedDataChannelOpenCannotActOnReplacementGeneration},
		    {"manager DataChannel pair helper waits for receiver open commit",
		     testManagerDataChannelPairHelperWaitsForReceiverOpenCommit},
		    {"inbound DataChannel dispatch waits for real open and delivers alpha preference",
		     testInboundDataChannelDispatchWaitsForRealOpenAndDeliversAlphaPreference},
		    {"inbound control routing rejects non-sendChannel labels",
		     testInboundControlRoutingRejectsNonSendChannelLabels},
		    {"empty-label legacy control channel remains compatible",
		     testEmptyLabelLegacyControlChannelRemainsCompatible},
		    {"DataChannel exact lease rejects stale messages and clears current close",
		     testDataChannelExactLeaseRejectsStaleMessagesAndClearsCurrentClose},
		    {"DataChannel close during callback install replays terminal state",
		     testDataChannelCloseDuringCallbackInstallReplaysTerminalState},
		    {"DataChannel pre-install replacement cannot overwrite newer lease",
		     testDataChannelPreInstallReplacementCannotOverwriteNewerLease},
		    {"duplicate DataChannel delivery does not reinstall or bump lease",
		     testDuplicateDataChannelDeliveryDoesNotReinstallOrBumpLease},
		    {"publisher DataChannel installs before peer registration",
		     testPublisherDataChannelInstallsBeforePeerRegistration},
		    {"synchronous DataChannel open replay subscriber does not hold lifecycle lock",
		     testSynchronousOpenReplaySubscriberDoesNotHoldLifecycleLock},
		    {"synchronous queued DataChannel message replay subscriber does not hold lifecycle lock",
		     testSynchronousQueuedMessageReplaySubscriberDoesNotHoldLifecycleLock},
		    {"DataChannel deferred replay drains in open-then-message order",
		     testDataChannelDeferredReplayDrainsInOpenThenMessageOrder},
		    {"rejected DataChannel open dispatch does not poison same-handle re-adoption",
		     testRejectedOpenDispatchDoesNotPoisonSameHandleReAdoption},
		    {"DataChannel stale open and error cannot mutate current lease",
		     testDataChannelStaleOpenAndErrorCannotMutateCurrentLease},
		    {"paused DataChannel dispatch revalidates exact lease before subscriber",
		     testPausedDataChannelDispatchRevalidatesExactLeaseBeforeSubscriber},
		    {"DataChannel callback cleanup runs outside manager locks",
		     testDataChannelCallbackCleanupRunsOutsideManagerLocks},
		    {"DataChannel alias commit races cannot resurrect closed transport",
		     testDataChannelAliasCommitRacesCannotResurrectClosedTransport},
		    {"open DataChannel same-handle re-adoption survives paused cleanup",
		     testOpenDataChannelSameHandleReAdoptionSurvivesPausedCleanup},
		    {"DataChannel in-callback retirement drains callback cleanup",
		     testDataChannelInCallbackRetirementDrainsCallbackCleanup},
		    {"identity-bearing DataChannel bye preserves transport peer and cleanup state",
		     testIdentityBearingByePreservesTransportPeerAndCleanupState},
		    {"PeerConnection description and feedback functions share one owner session",
		     testPeerConnectionDescriptionAndFeedbackFunctionsShareOwnerSession},
		    {"admitted feedback completion drains before owner state",
		     testAdmittedFeedbackCompletionDrainsBeforeOwnerState},
		    {"same-handle DataChannel replacement drains before detaching functions",
		     testSameHandleDataChannelReplacementDrainsBeforeDetachingEveryInstalledFunction},
		    {"peer-manager owner-session 128 lifecycle cycles", testPeerManagerOwnerSessionLifecycleCycles},
		    {"DataChannel in-flight callback is safe during manager destruction",
		     testDataChannelInFlightCallbackIsSafeDuringManagerDestruction},
		    {"DataChannel snapshot rejects callback after source destruction",
		     testDataChannelSnapshotRejectsCallbackAfterSourceDestruction},
		    {"RTC manager-to-source ordering heavy repeat", testRtcTrackOrderingHeavyRepeat},
		    {"shipped decoder threading configuration",
		     [&]() { testLinkedGateUsesShippedDecoderThreading(primaryAu, alphaAu); }},
		    {"frame-thread delayed decode preserves originating RTP PTS",
		     [&]() { testFrameThreadedDecodePreservesRtpPts(primaryGop); }},
		    {"real VP9 decode and alpha composition",
		     [&]() { testRealDecodeAndAlphaComposition(primaryGop, alphaGop); }},
		    {"RTP age eviction/reorder/drop/wrap/recovery",
		     [&]() { testRtpReorderDropWrapAndRecovery(primaryGop, alphaGop); }},
		    {"canonical two-pipeline transition flush",
		     [&]() { testTransitionFlushesBothPipelines(primaryGop, alphaGop); }},
		    {"stale epoch rejection at 5 stages x 2 tracks (10 latches)",
		     [&]() { testEveryEpochAdmissionStageDropsStale(primaryGop, alphaGop); }},
		    {"FFmpeg send EAGAIN drain and retry", [&]() { testSendPacketEagainDrainAndRetry(primaryGop); }},
		    {"alpha FFmpeg send EAGAIN drain, exact retry, and mate pairing",
		     [&]() { testAlphaSendPacketEagainDrainAndRetry(primaryGop, alphaGop); }},
		    {"overlapping decoded callback output serialization",
		     [&]() { testOverlappingDecodedCallbacksSerializeFinalOutput(primaryGop); }},
		    {"suppression serialized with final output commit",
		     [&]() { testSuppressionSerializesWithFinalCommit(primaryGop); }},
		    {"suppression aggregate rejects stale component snapshot",
		     testSuppressionAggregateCannotPublishStaleComponentSnapshot},
		    {"suppression components and reset remain coherent", testSuppressionComponentsAndResetRemainCoherent},
		    {"final output holds mute commit state", [&]() { testFinalOutputHoldsMuteCommitState(primaryGop); }},
		    {"output-before-mute linearization and deadlock repeat",
		     [&]() { testOutputLinearizesBeforeMuteAndClear(primaryGop); }},
		    {"stall clear rechecks fresh commit", [&]() { testStallClearRechecksFreshCommit(primaryGop); }},
		    {"coherent output dimension publication", [&]() { testDimensionPublicationIsCoherent(primaryGop); }},
		    {"symmetric peer ownership and deferred adoption", testSymmetricOwnershipAndDeferredAdoption},
		};

		const std::string filter = argc > 1 ? argv[1] : "";
		size_t executed = 0;
		for (const auto &gateCase : cases) {
			if (!filter.empty() && std::string(gateCase.name).find(filter) == std::string::npos) {
				continue;
			}
			gateCase.run();
			++executed;
			std::cout << "[PASS] " << gateCase.name << '\n';
		}
		require(executed > 0, "no linked native media case matched the requested filter");
		std::cout << "[PASS] all " << executed << " selected OBS/FFmpeg-linked native media cases\n";
		return 0;
	} catch (const std::exception &error) {
		std::cerr << "[FAIL] " << error.what() << '\n';
		return 1;
	}
}
