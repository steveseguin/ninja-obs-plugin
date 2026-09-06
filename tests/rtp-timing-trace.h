// Opt-in diagnostic wrapper. Built only with BUILD_RTP_TIMING_TRACE=ON.
// Records headers in bounded memory; writes only when the handler is destroyed.
#pragma once
#include <rtc/rtc.hpp>

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

#include "../src/vdoninja-rtp-repair.h"

namespace vdoninja
{
class RtpTimingTrace final : public rtc::MediaHandler
{
public:
	RtpTimingTrace(std::shared_ptr<rtc::MediaHandler> inner, std::string directory, uint32_t ssrc)
	    : inner_(std::move(inner)), ssrc_(ssrc)
	{
		const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
		path_ = directory + "/rtp-" + std::to_string(ssrc) + "-" + std::to_string(stamp) + ".csv";
		records_.reserve(kLimit);
	}
	~RtpTimingTrace() override
	{
		std::ofstream file(path_);
		file << "# version=2 nack_capture=1\n";
		file << "steady_ns,unix_ns,kind,ssrc,sequence,rtp_timestamp,ntp_seconds,ntp_fraction,bytes\n";
		for (const auto &r : records_)
			file << r.steady << ',' << r.unixTime << ',' << r.kind << ',' << r.ssrc << ',' << r.sequence << ','
			     << r.timestamp << ',' << r.ntpSeconds << ',' << r.ntpFraction << ',' << r.bytes << '\n';
		file << "# overflow=" << overflow_ << '\n';
	}
	void media(const rtc::Description::Media &description) override { inner_->mediaChain(description); }
	void incoming(rtc::message_vector &messages, const rtc::message_callback &send) override
	{
		for (const auto &message : messages) {
			if (!message || message->type != rtc::Message::Control)
				continue;
			const auto requested =
			    parseRtcpNackRequests(reinterpret_cast<const uint8_t *>(message->data()), message->size(), ssrc_);
			for (uint16_t sequence : requested) {
				Record r{};
				r.kind = 205;
				r.ssrc = ssrc_;
				r.sequence = sequence;
				r.steady = std::chrono::duration_cast<std::chrono::nanoseconds>(
				               std::chrono::steady_clock::now().time_since_epoch())
				               .count();
				r.unixTime = std::chrono::duration_cast<std::chrono::nanoseconds>(
				                 std::chrono::system_clock::now().time_since_epoch())
				                 .count();
				std::lock_guard<std::mutex> lock(mutex_);
				if (records_.size() < kLimit)
					records_.push_back(r);
				else
					++overflow_;
			}
		}
		inner_->incomingChain(messages, send);
	}
	void outgoing(rtc::message_vector &messages, const rtc::message_callback &send) override
	{
		for (const auto &message : messages)
			observe(message);
		inner_->outgoingChain(messages, [&](rtc::message_ptr message) {
			observe(message);
			send(std::move(message));
		});
	}
	bool requestKeyframe(const rtc::message_callback &send) override { return inner_->requestKeyframe(send); }
	bool requestBitrate(unsigned int bitrate, const rtc::message_callback &send) override
	{
		return inner_->requestBitrate(bitrate, send);
	}

private:
	struct Record {
		int64_t steady, unixTime;
		unsigned kind;
		uint32_t ssrc, sequence, timestamp, ntpSeconds, ntpFraction;
		size_t bytes;
	};
	static constexpr size_t kLimit = 131072;
	static uint32_t word(const uint8_t *p)
	{
		return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
	}
	void observe(const rtc::message_ptr &message)
	{
		if (!message || message->size() < 12)
			return;
		const auto *p = reinterpret_cast<const uint8_t *>(message->data());
		if ((p[0] >> 6) != 2)
			return;
		Record r{};
		r.steady =
		    std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
		        .count();
		r.unixTime =
		    std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch())
		        .count();
		r.bytes = message->size();
		if (message->type == rtc::Message::Control) {
			if (p[1] != 200 || message->size() < 28)
				return;
			r.kind = 200;
			r.ssrc = word(p + 4);
			r.ntpSeconds = word(p + 8);
			r.ntpFraction = word(p + 12);
			r.timestamp = word(p + 16);
		} else {
			r.kind = p[1] & 127;
			r.sequence = (uint32_t(p[2]) << 8) | p[3];
			r.timestamp = word(p + 4);
			r.ssrc = word(p + 8);
		}
		std::lock_guard<std::mutex> lock(mutex_);
		if (records_.size() < kLimit)
			records_.push_back(r);
		else
			++overflow_;
	}
	std::shared_ptr<rtc::MediaHandler> inner_;
	uint32_t ssrc_;
	std::string path_;
	std::vector<Record> records_;
	std::mutex mutex_;
	size_t overflow_ = 0;
};
inline std::shared_ptr<rtc::MediaHandler> wrapRtpTimingTrace(std::shared_ptr<rtc::MediaHandler> inner, uint32_t ssrc)
{
	const char *directory = std::getenv("VDONINJA_RTP_TRACE_DIR");
	if (!directory || !*directory)
		return inner;
	return std::make_shared<RtpTimingTrace>(std::move(inner), directory, ssrc);
}
} // namespace vdoninja
