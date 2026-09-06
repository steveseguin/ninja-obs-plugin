#include <filesystem>
#include <iterator>

#include <gtest/gtest.h>

#include "rtp-timing-trace.h"

namespace
{
class PassThrough final : public rtc::MediaHandler
{
public:
	size_t outgoingCalls = 0, incomingCalls = 0;
	rtc::message_ptr response;
	void outgoing(rtc::message_vector &, const rtc::message_callback &send) override
	{
		++outgoingCalls;
		if (response)
			send(response);
	}
	void incoming(rtc::message_vector &, const rtc::message_callback &) override { ++incomingCalls; }
	bool requestKeyframe(const rtc::message_callback &) override { return true; }
	bool requestBitrate(unsigned int bitrate, const rtc::message_callback &) override { return bitrate == 3000000; }
};
rtc::message_ptr message(std::initializer_list<uint8_t> bytes, rtc::Message::Type type = rtc::Message::Binary)
{
	auto result = rtc::make_message(bytes.size(), type);
	size_t i = 0;
	for (auto value : bytes)
		(*result)[i++] = std::byte(value);
	return result;
}
class TimingTraceTest : public ::testing::Test
{
protected:
	std::filesystem::path directory;
	void SetUp() override
	{
		directory =
		    std::filesystem::temp_directory_path() /
		    ("vdoninja-timing-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
		std::filesystem::create_directory(directory);
	}
	void TearDown() override { std::filesystem::remove_all(directory); }
	std::string trace()
	{
		auto it = std::filesystem::directory_iterator(directory);
		if (it == std::filesystem::directory_iterator{})
			return {};
		std::ifstream file(it->path());
		return std::string(std::istreambuf_iterator<char>(file), {});
	}
};
TEST_F(TimingTraceTest, PreservesMediaAndFeedbackWhileRecordingSenderReportMapping)
{
	auto inner = std::make_shared<PassThrough>();
	inner->response = message({0x80, 200, 0, 6, 0, 0, 0, 1, 0, 0, 0, 2, 0, 0, 0, 3, 0, 0, 0, 4, 0, 0, 0, 0, 0, 0, 0, 0},
	                          rtc::Message::Control);
	auto rtp = message({0x80, 0xe0, 0xff, 0xff, 0xff, 0xff, 0xff, 0, 0, 0, 0, 1, 0x65});
	rtc::message_vector packets{rtp};
	rtc::message_ptr sent;
	{
		vdoninja::RtpTimingTrace tracer(inner, directory.string(), 1);
		tracer.outgoing(packets, [&](rtc::message_ptr value) { sent = value; });
		EXPECT_EQ(packets.front(), rtp);
		EXPECT_EQ(sent, inner->response);
		EXPECT_TRUE(std::filesystem::is_empty(directory));
		EXPECT_TRUE(tracer.requestKeyframe({}));
		EXPECT_TRUE(tracer.requestBitrate(3000000, {}));
	}
	const auto text = trace();
	EXPECT_NE(text.find(",96,1,65535,4294967040,0,0,13"), std::string::npos);
	EXPECT_NE(text.find(",200,1,0,4,2,3,28"), std::string::npos);
	EXPECT_NE(text.find("# overflow=0"), std::string::npos);
}
TEST_F(TimingTraceTest, CapturesRequestedSequencesAndStillForwardsIncomingFeedback)
{
	auto inner = std::make_shared<PassThrough>();
	auto nack = message({0x81, 205, 0, 3, 0, 0, 0, 2, 0, 0, 0, 1, 0xff, 0xff, 0, 1}, rtc::Message::Control);
	rtc::message_vector packets{nack};
	{
		vdoninja::RtpTimingTrace tracer(inner, directory.string(), 1);
		tracer.incoming(packets, {});
	}
	EXPECT_EQ(inner->incomingCalls, 1u);
	EXPECT_EQ(packets.front(), nack);
	EXPECT_NE(trace().find(",205,1,65535,0,0,0,0"), std::string::npos);
	EXPECT_NE(trace().find(",205,1,0,0,0,0,0"), std::string::npos);
}
TEST_F(TimingTraceTest, TraceCapacityDoesNotDropActualMedia)
{
	auto inner = std::make_shared<PassThrough>();
	rtc::message_vector packets{message({0x80, 96, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1})};
	{
		vdoninja::RtpTimingTrace tracer(inner, directory.string(), 1);
		for (size_t i = 0; i < 131074; ++i)
			tracer.outgoing(packets, {});
	}
	EXPECT_EQ(inner->outgoingCalls, 131074u);
	EXPECT_NE(trace().find("# overflow=2"), std::string::npos);
}
} // namespace
