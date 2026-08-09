/*
 * Compile and behavior contract for the unit-test libdatachannel stub.
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#include <rtc/rtc.hpp>

#include <cstddef>
#include <string>
#include <type_traits>
#include <variant>

#include <gtest/gtest.h>

static_assert(std::variant_size_v<rtc::message_variant> == 2);
static_assert(std::is_same_v<std::variant_alternative_t<0, rtc::message_variant>, rtc::binary>);
static_assert(std::is_same_v<std::variant_alternative_t<1, rtc::message_variant>, std::string>);
static_assert(std::is_same_v<rtc::WebSocket::Message, rtc::message_variant>);

TEST(RtcStubCompatibilityTest, MessageVariantMatchesLibdatachannelBinaryAndStringAlternatives)
{
	rtc::message_variant binaryMessage = rtc::binary{std::byte{0x2a}};
	ASSERT_TRUE(std::holds_alternative<rtc::binary>(binaryMessage));
	EXPECT_EQ(std::get<rtc::binary>(binaryMessage).front(), std::byte{0x2a});

	rtc::message_variant stringMessage = std::string("signal");
	ASSERT_TRUE(std::holds_alternative<std::string>(stringMessage));
	EXPECT_EQ(std::get<std::string>(stringMessage), "signal");
}
