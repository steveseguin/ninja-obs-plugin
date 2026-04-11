/*
 * Unit tests for VP9 RTP payload descriptor parsing (RFC 9628)
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#include <gtest/gtest.h>

#include "vdoninja-rtp-utils.h"

using namespace vdoninja;

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
	    // SS header: N_S=1 (bits 7-4 = 0001), Y=1 (bit 3), G=0 (bit 2) => 0x18
	    0x18, 0x05, 0x00, // width layer 0
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
	// SS header byte: N_S=0000, Y=0, G=1 => 0x04
	// N_G = 1
	// Group entry: T=0, U=0, R=01 => 0x04, then 1 P_DIFF byte
	const uint8_t buf[] = {
	    descByte(0, 0, 0, 0, 1, 1, 1, 0),
	    0x04, // SS header: N_S=0, Y=0, G=1
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
	    0x08, // N_S=0, Y=1, G=0 => needs 4 more bytes but none follow
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
