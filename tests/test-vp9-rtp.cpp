/*
 * Unit tests for VP9 RTP payload descriptor parsing (RFC 9628)
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#include <iomanip>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "vdoninja-rtp-utils.h"

using namespace vdoninja;

TEST(RtcpSenderReportTest, BecomesDueAfterOneClockSecond)
{
	EXPECT_FALSE(isRtcpSenderReportDue(89999, 0, 90000));
	EXPECT_TRUE(isRtcpSenderReportDue(90000, 0, 90000));
}

TEST(RtcpSenderReportTest, HandlesRtpTimestampWrapAround)
{
	constexpr uint32_t last = 0xFFFF0000u;
	EXPECT_TRUE(isRtcpSenderReportDue(last + 90000u, last, 90000));
}

TEST(RtcpSenderReportTest, RejectsInvalidClockRate)
{
	EXPECT_FALSE(isRtcpSenderReportDue(100, 0, 0));
}

// A cached keyframe replayed to a viewer carries the RTP timestamp of the GOP it
// was captured in, which can be seconds behind the live stream. Modular
// subtraction turns that regression into a delta just under 2^32, which used to
// read as a long-overdue report and emitted a sender report advertising a
// rewound RTP timestamp — corrupting the viewer's RTP/NTP mapping and stalling
// both audio and video while it resynchronized.
TEST(RtcpSenderReportTest, RejectsLargeTimestampRegression)
{
	constexpr uint32_t last = 720000u; // 8 seconds of 90kHz media
	EXPECT_FALSE(isRtcpSenderReportDue(0, last, 90000));
}

TEST(RtcpSenderReportTest, RejectsSmallTimestampRegression)
{
	EXPECT_FALSE(isRtcpSenderReportDue(1000, 2000, 90000));
}

TEST(RtcpSenderReportTest, TreatsHalfRangeAsTheForwardBoundary)
{
	EXPECT_TRUE(isRtcpSenderReportDue(0x7FFFFFFFu, 0, 90000));
	EXPECT_FALSE(isRtcpSenderReportDue(0x80000000u, 0, 90000));
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Build the mandatory descriptor byte from individual flag bits.
static uint8_t descByte(bool I, bool P, bool L, bool F, bool B, bool E, bool V, bool Z)
{
	return static_cast<uint8_t>((I ? 0x80 : 0) | (P ? 0x40 : 0) | (L ? 0x20 : 0) | (F ? 0x10 : 0) | (B ? 0x08 : 0) |
	                            (E ? 0x04 : 0) | (V ? 0x02 : 0) | (Z ? 0x01 : 0));
}

// ---------------------------------------------------------------------------
// Minimal descriptor — no optional fields
// ---------------------------------------------------------------------------

TEST(Vp9DescriptorTest, NullPayloadIsInvalid)
{
	const auto r = parseVP9PayloadDescriptor(nullptr, 10);
	EXPECT_FALSE(r.valid);
}

TEST(Vp9DescriptorTest, ZeroSizeIsInvalid)
{
	const uint8_t buf[] = {0x00};
	const auto r = parseVP9PayloadDescriptor(buf, 0);
	EXPECT_FALSE(r.valid);
}

TEST(Vp9DescriptorTest, SinglePacketFrame_BothBitsSet)
{
	// I=0,P=0,L=0,F=0, B=1,E=1, V=0,Z=0 => 0x0C
	// Followed by 3 payload bytes
	const uint8_t buf[] = {descByte(0, 0, 0, 0, 1, 1, 0, 0), 0xAB, 0xCD, 0xEF};
	const auto r = parseVP9PayloadDescriptor(buf, sizeof(buf));
	EXPECT_TRUE(r.valid);
	EXPECT_TRUE(r.startOfFrame);
	EXPECT_TRUE(r.endOfFrame);
	EXPECT_EQ(r.payloadOffset, 1u);
}

TEST(Vp9DescriptorTest, FirstFragment_BStartOnly)
{
	// B=1, E=0
	const uint8_t buf[] = {descByte(0, 0, 0, 0, 1, 0, 0, 0), 0x01, 0x02};
	const auto r = parseVP9PayloadDescriptor(buf, sizeof(buf));
	EXPECT_TRUE(r.valid);
	EXPECT_TRUE(r.startOfFrame);
	EXPECT_FALSE(r.endOfFrame);
	EXPECT_EQ(r.payloadOffset, 1u);
}

TEST(Vp9DescriptorTest, LastFragment_EEndOnly)
{
	// B=0, E=1
	const uint8_t buf[] = {descByte(0, 0, 0, 0, 0, 1, 0, 0), 0x03, 0x04};
	const auto r = parseVP9PayloadDescriptor(buf, sizeof(buf));
	EXPECT_TRUE(r.valid);
	EXPECT_FALSE(r.startOfFrame);
	EXPECT_TRUE(r.endOfFrame);
	EXPECT_EQ(r.payloadOffset, 1u);
}

TEST(Vp9DescriptorTest, MiddleFragment_NeitherBNorE)
{
	// B=0, E=0
	const uint8_t buf[] = {descByte(0, 0, 0, 0, 0, 0, 0, 0), 0x05};
	const auto r = parseVP9PayloadDescriptor(buf, sizeof(buf));
	EXPECT_TRUE(r.valid);
	EXPECT_FALSE(r.startOfFrame);
	EXPECT_FALSE(r.endOfFrame);
	EXPECT_EQ(r.payloadOffset, 1u);
}

// ---------------------------------------------------------------------------
// PictureID — I bit
// ---------------------------------------------------------------------------

TEST(Vp9DescriptorTest, SevenBitPictureID_M0)
{
	// I=1, M=0 in first PID byte => 1 byte PID total
	// desc: I=1, B=1, E=1
	const uint8_t buf[] = {
	    descByte(1, 0, 0, 0, 1, 1, 0, 0),
	    0x42, // M=0, PID=0x42
	    0xAA, // payload
	};
	const auto r = parseVP9PayloadDescriptor(buf, sizeof(buf));
	EXPECT_TRUE(r.valid);
	EXPECT_TRUE(r.startOfFrame);
	EXPECT_TRUE(r.endOfFrame);
	EXPECT_EQ(r.payloadOffset, 2u); // desc(1) + PID(1)
}

TEST(Vp9DescriptorTest, FifteenBitPictureID_M1)
{
	// I=1, M=1 in first PID byte => 2 byte PID total
	const uint8_t buf[] = {
	    descByte(1, 0, 0, 0, 1, 1, 0, 0),
	    0x81, // M=1, PID high bits
	    0x23, // PID low byte
	    0xBB, // payload
	};
	const auto r = parseVP9PayloadDescriptor(buf, sizeof(buf));
	EXPECT_TRUE(r.valid);
	EXPECT_EQ(r.payloadOffset, 3u); // desc(1) + PID(2)
}

TEST(Vp9DescriptorTest, PictureID_TruncatedAtFirstByte_Invalid)
{
	// I=1 but only descriptor byte present — no PID byte
	const uint8_t buf[] = {descByte(1, 0, 0, 0, 1, 1, 0, 0)};
	const auto r = parseVP9PayloadDescriptor(buf, sizeof(buf));
	EXPECT_FALSE(r.valid);
}

TEST(Vp9DescriptorTest, PictureID_TruncatedAtSecondByte_Invalid)
{
	// I=1, M=1 but only one PID byte present
	const uint8_t buf[] = {descByte(1, 0, 0, 0, 1, 1, 0, 0), 0x80}; // M=1, no second byte
	const auto r = parseVP9PayloadDescriptor(buf, sizeof(buf));
	EXPECT_FALSE(r.valid);
}

// ---------------------------------------------------------------------------
// Layer indices — L bit
// ---------------------------------------------------------------------------

TEST(Vp9DescriptorTest, LayerIndicesNonFlexible_TwoExtraBytes)
{
	// L=1, F=0 => 1 byte TID/U/SID/D + 1 byte TL0PICIDX
	const uint8_t buf[] = {
	    descByte(0, 0, 1, 0, 1, 1, 0, 0),
	    0x01, // TID/U/SID/D
	    0x05, // TL0PICIDX
	    0xCC, // payload
	};
	const auto r = parseVP9PayloadDescriptor(buf, sizeof(buf));
	EXPECT_TRUE(r.valid);
	EXPECT_EQ(r.payloadOffset, 3u); // desc(1) + layer(1) + TL0(1)
}

TEST(Vp9DescriptorTest, LayerIndicesFlexible_OneExtraByte)
{
	// L=1, F=1, P=0 => 1 byte TID/U/SID/D only (no TL0PICIDX in flexible mode)
	const uint8_t buf[] = {
	    descByte(0, 0, 1, 1, 1, 1, 0, 0),
	    0x01, // TID/U/SID/D
	    0xDD, // payload
	};
	const auto r = parseVP9PayloadDescriptor(buf, sizeof(buf));
	EXPECT_TRUE(r.valid);
	EXPECT_EQ(r.payloadOffset, 2u); // desc(1) + layer(1)
}

TEST(Vp9DescriptorTest, LayerIndices_TruncatedLayerByte_Invalid)
{
	// L=1 but only descriptor byte present
	const uint8_t buf[] = {descByte(0, 0, 1, 0, 1, 1, 0, 0)};
	const auto r = parseVP9PayloadDescriptor(buf, sizeof(buf));
	EXPECT_FALSE(r.valid);
}

// ---------------------------------------------------------------------------
// P_DIFFs — F=1, P=1 (flexible mode)
// ---------------------------------------------------------------------------

TEST(Vp9DescriptorTest, FlexibleMode_OnePDiff_N0)
{
	// F=1, P=1, L=0 => one P_DIFF byte with N=0 (no more follow)
	const uint8_t buf[] = {
	    descByte(0, 1, 0, 1, 1, 1, 0, 0),
	    0x04, // P_DIFF=2, N=0
	    0xEE, // payload
	};
	const auto r = parseVP9PayloadDescriptor(buf, sizeof(buf));
	EXPECT_TRUE(r.valid);
	EXPECT_EQ(r.payloadOffset, 2u); // desc(1) + P_DIFF(1)
}

TEST(Vp9DescriptorTest, FlexibleMode_TwoPDiffs_SecondN0)
{
	// F=1, P=1, L=0 => two P_DIFF bytes; first has N=1 (another follows), second has N=0
	const uint8_t buf[] = {
	    descByte(0, 1, 0, 1, 1, 1, 0, 0),
	    0x05, // P_DIFF, N=1
	    0x06, // P_DIFF, N=0
	    0xFF, // payload
	};
	const auto r = parseVP9PayloadDescriptor(buf, sizeof(buf));
	EXPECT_TRUE(r.valid);
	EXPECT_EQ(r.payloadOffset, 3u); // desc(1) + P_DIFF(2)
}

TEST(Vp9DescriptorTest, FlexibleMode_ThreePDiffs_AllConsumed)
{
	// F=1, P=1 with three P_DIFFs (max per RFC 9628)
	const uint8_t buf[] = {
	    descByte(0, 1, 0, 1, 1, 1, 0, 0),
	    0x03, // P_DIFF, N=1
	    0x03, // P_DIFF, N=1
	    0x02, // P_DIFF, N=0
	    0x11, // payload
	};
	const auto r = parseVP9PayloadDescriptor(buf, sizeof(buf));
	EXPECT_TRUE(r.valid);
	EXPECT_EQ(r.payloadOffset, 4u);
}

TEST(Vp9DescriptorTest, FlexibleMode_PDiff_Truncated_Invalid)
{
	// F=1, P=1 but no P_DIFF byte present
	const uint8_t buf[] = {descByte(0, 1, 0, 1, 1, 1, 0, 0)};
	const auto r = parseVP9PayloadDescriptor(buf, sizeof(buf));
	EXPECT_FALSE(r.valid);
}

TEST(Vp9DescriptorTest, FlexibleModeRejectsContinuationAfterThirdReference)
{
	for (int scalability = 0; scalability <= 1; ++scalability) {
		const uint8_t buf[] = {descByte(1, 1, 0, 1, 1, 1, scalability, 0), 10, 0x03, 0x05, 0x07, 0x02, 0xEE};
		EXPECT_FALSE(parseVP9PayloadDescriptor(buf, sizeof(buf)).valid);
		EXPECT_FALSE(parseVP9PayloadDescriptor(buf, 5).valid);
	}
}

TEST(Vp9DescriptorTest, FlexibleModeRejectsZeroReferenceDistance)
{
	for (size_t reference = 0; reference < 3; ++reference) {
		uint8_t buf[] = {descByte(1, 1, 0, 1, 1, 1, 0, 0), 10, 0x03, 0x05, 0x06, 0xEE};
		buf[2 + reference] &= 0x01; // Preserve the continuation bit, but clear P_DIFF.
		EXPECT_FALSE(parseVP9PayloadDescriptor(buf, sizeof(buf)).valid);
	}
}

// ---------------------------------------------------------------------------
// Scalability structure — V bit
// ---------------------------------------------------------------------------

TEST(Vp9DescriptorTest, ScalabilityStructure_NoYNoG)
{
	// V=1, Y=0, G=0 => just the SS header byte
	const uint8_t buf[] = {
	    descByte(0, 0, 0, 0, 1, 1, 1, 0),
	    0x00, // N_S=0, Y=0, G=0
	    0x22, // payload
	};
	const auto r = parseVP9PayloadDescriptor(buf, sizeof(buf));
	EXPECT_TRUE(r.valid);
	EXPECT_EQ(r.payloadOffset, 2u); // desc(1) + SS_header(1)
}

TEST(Vp9DescriptorTest, ScalabilityStructure_WithResolution_TwoLayers)
{
	// V=1, N_S=1 (2 layers), Y=1, G=0 => SS header(1) + 2*4 resolution bytes
	const uint8_t buf[] = {
	    descByte(0, 0, 0, 0, 1, 1, 1, 0),
	    // SS header: N_S=1 (bits 7-5 = 001), Y=1 (bit 4), G=0 (bit 3) => 0x30
	    0x30, 0x05, 0x00, // width layer 0
	    0x03, 0x00,       // height layer 0
	    0x0A, 0x00,       // width layer 1
	    0x06, 0x00,       // height layer 1
	    0x77,             // payload
	};
	const auto r = parseVP9PayloadDescriptor(buf, sizeof(buf));
	EXPECT_TRUE(r.valid);
	EXPECT_EQ(r.payloadOffset, 10u); // desc(1) + SS_hdr(1) + 2*4(8)
}

TEST(Vp9DescriptorTest, ScalabilityStructure_WithGroupDesc)
{
	// V=1, N_S=0, Y=0, G=1, N_G=1 group entry with R=1 reference
	// SS header byte: N_S=000, Y=0, G=1 => 0x08
	// N_G = 1
	// Group entry: T=0, U=0, R=01 => 0x04, then 1 P_DIFF byte
	const uint8_t buf[] = {
	    descByte(0, 0, 0, 0, 1, 1, 1, 0),
	    0x08, // SS header: N_S=0, Y=0, G=1
	    0x01, // N_G=1
	    0x04, // T=0,U=0,R=1,RES=0 -> R=1
	    0x01, // P_DIFF for the 1 reference
	    0x88, // payload
	};
	const auto r = parseVP9PayloadDescriptor(buf, sizeof(buf));
	EXPECT_TRUE(r.valid);
	EXPECT_EQ(r.payloadOffset, 5u);
}

TEST(Vp9DescriptorTest, ScalabilityStructure_Truncated_Invalid)
{
	// V=1, Y=1, N_S=0 (1 layer) but resolution bytes missing
	const uint8_t buf[] = {
	    descByte(0, 0, 0, 0, 1, 1, 1, 0),
	    0x10, // N_S=0, Y=1, G=0 => needs 4 more bytes but none follow
	};
	const auto r = parseVP9PayloadDescriptor(buf, sizeof(buf));
	EXPECT_FALSE(r.valid);
}

// ---------------------------------------------------------------------------
// Combined fields
// ---------------------------------------------------------------------------

TEST(Vp9DescriptorTest, Combined_PID_Layers_Flexible_Payload)
{
	// I=1 (15-bit PID), L=1, F=1, P=1, B=1, E=1
	// PID: M=1, high=0x01, low=0x23 => 2 bytes
	// Layer: 1 byte (F=1 => no TL0PICIDX)
	// P_DIFF: N=0 => 1 byte
	// payload: 1 byte
	const uint8_t buf[] = {
	    descByte(1, 1, 1, 1, 1, 1, 0, 0),
	    0x81,
	    0x23, // 15-bit PID
	    0x20, // TID/U/SID/D (flexible, no TL0)
	    0x06, // P_DIFF, N=0
	    0x99, // payload byte
	};
	const auto r = parseVP9PayloadDescriptor(buf, sizeof(buf));
	EXPECT_TRUE(r.valid);
	EXPECT_TRUE(r.startOfFrame);
	EXPECT_TRUE(r.endOfFrame);
	EXPECT_EQ(r.payloadOffset, 5u); // desc(1)+PID(2)+layer(1)+P_DIFF(1)
}

TEST(Vp9DescriptorTest, ZFlagDoesNotAffectParsing)
{
	// Z=1 should be ignored for reassembly
	const uint8_t buf[] = {descByte(0, 0, 0, 0, 1, 1, 0, 1), 0xAA};
	const auto r = parseVP9PayloadDescriptor(buf, sizeof(buf));
	EXPECT_TRUE(r.valid);
	EXPECT_TRUE(r.startOfFrame);
	EXPECT_TRUE(r.endOfFrame);
	EXPECT_EQ(r.payloadOffset, 1u);
}

TEST(Vp9DescriptorTest, PayloadOffsetPointsPastAllDescriptorBytes)
{
	// Verify that payload bytes after the descriptor are not consumed
	// I=1 (7-bit PID), L=1, F=0, B=1, E=1
	// desc(1) + PID(1) + layer(1) + TL0(1) = 4 header bytes, then payload
	const uint8_t buf[] = {
	    descByte(1, 0, 1, 0, 1, 1, 0, 0),
	    0x10, // M=0, PID=16
	    0x00, // TID/U/SID/D
	    0x07, // TL0PICIDX
	    0xDE,
	    0xAD,
	    0xBE,
	    0xEF, // actual VP9 bitstream bytes
	};
	const auto r = parseVP9PayloadDescriptor(buf, sizeof(buf));
	EXPECT_TRUE(r.valid);
	EXPECT_EQ(r.payloadOffset, 4u);
	// Verify the bytes at payloadOffset are the expected bitstream bytes
	EXPECT_EQ(buf[r.payloadOffset], 0xDE);
	EXPECT_EQ(buf[r.payloadOffset + 1], 0xAD);
}

// ---------------------------------------------------------------------------
// Alpha-channel dual-track scenarios
// The alpha VP9 stream is a normal VP9 stream whose Y plane carries the alpha
// values. The descriptor format is identical; these tests verify that the
// parser handles typical alpha-stream packet patterns correctly.
// ---------------------------------------------------------------------------

// A single-packet alpha frame (B=1, E=1) with no optional fields.
TEST(Vp9DescriptorTest, AlphaStream_SinglePacketFrame)
{
	const uint8_t buf[] = {descByte(0, 0, 0, 0, 1, 1, 0, 0), 0x42, 0x43};
	const auto r = parseVP9PayloadDescriptor(buf, sizeof(buf));
	EXPECT_TRUE(r.valid);
	EXPECT_TRUE(r.startOfFrame);
	EXPECT_TRUE(r.endOfFrame);
	EXPECT_EQ(r.payloadOffset, 1u);
	EXPECT_EQ(buf[r.payloadOffset], 0x42);
}

// First fragment of a multi-packet alpha frame (B=1, E=0).
TEST(Vp9DescriptorTest, AlphaStream_FirstFragment)
{
	const uint8_t buf[] = {descByte(0, 0, 0, 0, 1, 0, 0, 0), 0x01, 0x02, 0x03};
	const auto r = parseVP9PayloadDescriptor(buf, sizeof(buf));
	EXPECT_TRUE(r.valid);
	EXPECT_TRUE(r.startOfFrame);
	EXPECT_FALSE(r.endOfFrame);
	EXPECT_EQ(r.payloadOffset, 1u);
}

// Last fragment of a multi-packet alpha frame (B=0, E=1).
TEST(Vp9DescriptorTest, AlphaStream_LastFragment)
{
	const uint8_t buf[] = {descByte(0, 0, 0, 0, 0, 1, 0, 0), 0xAA, 0xBB};
	const auto r = parseVP9PayloadDescriptor(buf, sizeof(buf));
	EXPECT_TRUE(r.valid);
	EXPECT_FALSE(r.startOfFrame);
	EXPECT_TRUE(r.endOfFrame);
	EXPECT_EQ(r.payloadOffset, 1u);
}

// Alpha stream packet with 15-bit PictureID (I=1, M=1) and B=1/E=1.
TEST(Vp9DescriptorTest, AlphaStream_With15BitPictureID)
{
	// desc(1) + PID_hi(1, M=1) + PID_lo(1) = 3 header bytes
	const uint8_t buf[] = {
	    descByte(1, 0, 0, 0, 1, 1, 0, 0),
	    0x80 | 0x01, // M=1, PID high bits
	    0x23,        // PID low byte
	    0xFF,        // alpha bitstream
	};
	const auto r = parseVP9PayloadDescriptor(buf, sizeof(buf));
	EXPECT_TRUE(r.valid);
	EXPECT_TRUE(r.startOfFrame);
	EXPECT_TRUE(r.endOfFrame);
	EXPECT_EQ(r.payloadOffset, 3u);
	EXPECT_EQ(buf[r.payloadOffset], 0xFF);
}

// Alpha stream with Z=1 (not a reference for upper spatial layers) — should
// be ignored by the parser and not affect validity or offsets.
TEST(Vp9DescriptorTest, AlphaStream_ZFlagIgnored)
{
	// Z=1 set, B=1, E=1
	const uint8_t buf[] = {descByte(0, 0, 0, 0, 1, 1, 0, 1), 0x55};
	const auto r = parseVP9PayloadDescriptor(buf, sizeof(buf));
	EXPECT_TRUE(r.valid);
	EXPECT_TRUE(r.startOfFrame);
	EXPECT_TRUE(r.endOfFrame);
	EXPECT_EQ(r.payloadOffset, 1u);
}

// Typical alpha keyframe descriptor: B=1, E=1, V=1 (scalability structure
// with 1 spatial layer, no resolution info, no group description).
TEST(Vp9DescriptorTest, AlphaStream_KeyframeWithScalabilityStructure)
{
	// V=1 SS byte: N_S=0 (1 layer), Y=0, G=0 => SS=0x00
	const uint8_t buf[] = {
	    descByte(0, 0, 0, 0, 1, 1, 1, 0),
	    0x00,       // SS: N_S=0, Y=0, G=0
	    0xDE, 0xAD, // alpha bitstream
	};
	const auto r = parseVP9PayloadDescriptor(buf, sizeof(buf));
	EXPECT_TRUE(r.valid);
	EXPECT_TRUE(r.startOfFrame);
	EXPECT_TRUE(r.endOfFrame);
	EXPECT_EQ(r.payloadOffset, 2u);
	EXPECT_EQ(buf[r.payloadOffset], 0xDE);
}

// ---------------------------------------------------------------------------
// Fuzz / robustness coverage
//
// parseVP9PayloadDescriptor consumes raw bytes straight off the network from an
// untrusted peer, so malformed input must never read past the buffer and must
// never report a payloadOffset beyond it. Both callers in vdoninja-source.cpp
// compute `payloadSize - desc.payloadOffset`; if payloadOffset ever exceeded
// payloadSize that subtraction would wrap into a huge size_t and the assembly
// buffers would copy far out of bounds.
//
// Buffers here are sized exactly to the input so that an over-read lands
// outside a heap allocation, where a sanitizer build can see it.
// ---------------------------------------------------------------------------

namespace
{

struct FuzzCheck {
	bool ok = true;
	std::string detail;
};

// Assert the invariants every caller depends on, whatever the input was.
FuzzCheck checkDescriptorInvariants(const std::vector<uint8_t> &input)
{
	// Exact-size copy: reading input[size] is then a heap overflow, not a read of
	// some adjacent stack byte that happens to be harmless.
	std::vector<uint8_t> exact(input.begin(), input.end());
	const uint8_t *ptr = exact.empty() ? nullptr : exact.data();
	const Vp9DescriptorResult r = parseVP9PayloadDescriptor(ptr, exact.size());

	FuzzCheck check;
	auto fail = [&](const std::string &why) {
		std::ostringstream os;
		os << why << " (size=" << exact.size() << ", bytes=";
		for (size_t i = 0; i < exact.size() && i < 24; ++i) {
			os << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(exact[i]) << " ";
		}
		os << ")";
		check.ok = false;
		check.detail = os.str();
	};

	if (r.valid) {
		if (r.payloadOffset > exact.size()) {
			fail("payloadOffset past end of payload; callers would underflow size - payloadOffset");
		} else if (r.payloadOffset < 1) {
			fail("valid descriptor must consume at least the mandatory byte");
		}
	} else if (r.payloadOffset != 0) {
		fail("invalid descriptor must not report a payload offset");
	}

	// Parsing is pure: the same bytes must always produce the same answer.
	const Vp9DescriptorResult again = parseVP9PayloadDescriptor(ptr, exact.size());
	if (check.ok && (again.valid != r.valid || again.payloadOffset != r.payloadOffset ||
	                 again.startOfFrame != r.startOfFrame || again.endOfFrame != r.endOfFrame)) {
		fail("parser is not deterministic for identical input");
	}

	return check;
}

} // namespace

// Every possible mandatory descriptor byte, truncated at every length, against
// several tail fillers. Truncation is where descriptor parsers overrun, because
// each optional field has to re-check the remaining length.
TEST(Vp9DescriptorFuzzTest, ExhaustiveTruncationNeverOverruns)
{
	// 0x00 and 0xFF drive the length-bearing fields (N_S, N_G, R, M, N) to their
	// extremes; 0x5A/0xA5 mix the flag bits within each optional field.
	const uint8_t fillers[] = {0x00, 0xFF, 0x5A, 0xA5};
	constexpr size_t kMaxLen = 48;

	for (int first = 0; first <= 0xFF; ++first) {
		for (uint8_t filler : fillers) {
			for (size_t len = 1; len <= kMaxLen; ++len) {
				std::vector<uint8_t> buf(len, filler);
				buf[0] = static_cast<uint8_t>(first);
				const FuzzCheck check = checkDescriptorInvariants(buf);
				ASSERT_TRUE(check.ok) << check.detail;
			}
		}
	}
}

// Fully random payloads, including empty and single-byte ones.
TEST(Vp9DescriptorFuzzTest, RandomPayloadsNeverOverrun)
{
	// Fixed seed: fuzz failures here must be reproducible from the test name alone.
	std::mt19937 rng(0x9E3779B9u);
	std::uniform_int_distribution<int> sizeDist(0, 64);
	std::uniform_int_distribution<int> byteDist(0, 255);

	for (int iteration = 0; iteration < 20000; ++iteration) {
		std::vector<uint8_t> buf(static_cast<size_t>(sizeDist(rng)));
		for (uint8_t &b : buf) {
			b = static_cast<uint8_t>(byteDist(rng));
		}
		const FuzzCheck check = checkDescriptorInvariants(buf);
		ASSERT_TRUE(check.ok) << check.detail << " (iteration=" << iteration << ")";
	}
}

// A scalability structure declares its own lengths (N_S resolutions, N_G picture
// group entries, R references each), so it is the easiest place to walk off the
// end. Drive those counts to their maximums against a too-short buffer.
TEST(Vp9DescriptorFuzzTest, ScalabilityStructureLengthsAreBounded)
{
	for (int nS = 0; nS <= 7; ++nS) {
		for (int y = 0; y <= 1; ++y) {
			for (int g = 0; g <= 1; ++g) {
				const uint8_t ss = static_cast<uint8_t>((nS << 5) | (y ? 0x10 : 0) | (g ? 0x08 : 0));
				for (int nG = 0; nG <= 255; nG += 17) {
					for (size_t len = 1; len <= 40; ++len) {
						std::vector<uint8_t> buf(len, 0xFF);
						buf[0] = descByte(0, 0, 0, 0, 1, 1, 1, 0); // V=1 only
						if (len > 1) {
							buf[1] = ss;
						}
						if (len > 2) {
							buf[2] = static_cast<uint8_t>(nG);
						}
						const FuzzCheck check = checkDescriptorInvariants(buf);
						ASSERT_TRUE(check.ok)
						    << check.detail << " (N_S=" << nS << " Y=" << y << " G=" << g << " N_G=" << nG << ")";
					}
				}
			}
		}
	}
}
