/*
 * OBS VDO.Ninja Plugin
 * RTP utility functions — codec-specific payload descriptor parsing
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#include "vdoninja-rtp-utils.h"

namespace vdoninja
{

// ---------------------------------------------------------------------------
// VP9 RTP payload descriptor parser — RFC 9628 section 4.2
//
// Mandatory first byte layout:
//   bit 7: I  — PictureID present
//   bit 6: P  — inter-picture predicted layer frame
//   bit 5: L  — layer indices present
//   bit 4: F  — flexible mode (P_DIFFs present when P=1)
//   bit 3: B  — start of VP9 frame (first packet)
//   bit 2: E  — end of VP9 frame (last packet)
//   bit 1: V  — scalability structure (SS) present
//   bit 0: Z  — not a reference for upper spatial layers
//
// Optional fields follow in this order:
//   PictureID     (if I=1): 1 byte (M=0, 7-bit PID) or 2 bytes (M=1, 15-bit PID)
//   Layer indices (if L=1): 1 byte + 1 byte TL0PICIDX (non-flexible, F=0 only)
//   P_DIFFs       (if F=1 and P=1): variable-length, each byte is P_DIFF(7)+N; stop when N=0
//   Scalability structure (if V=1): variable-length
// ---------------------------------------------------------------------------

Vp9DescriptorResult parseVP9PayloadDescriptor(const uint8_t *payload, size_t size)
{
	Vp9DescriptorResult result;

	if (!payload || size == 0) {
		return result;
	}

	size_t offset = 0;

	// --- Mandatory descriptor byte ---
	const uint8_t desc = payload[offset++];
	const bool I = (desc & 0x80) != 0;
	const bool P = (desc & 0x40) != 0;
	const bool L = (desc & 0x20) != 0;
	const bool F = (desc & 0x10) != 0;
	const bool B = (desc & 0x08) != 0;
	const bool E = (desc & 0x04) != 0;
	const bool V = (desc & 0x02) != 0;
	// Z bit (0x01) is not needed for reassembly

	// --- PictureID (optional) ---
	if (I) {
		if (offset >= size) {
			return result;
		}
		const bool M = (payload[offset] & 0x80) != 0;
		offset++; // consume first PID byte
		if (M) {
			if (offset >= size) {
				return result;
			}
			offset++; // consume second byte of 15-bit PID
		}
	}

	// --- Layer indices (optional) ---
	if (L) {
		if (offset >= size) {
			return result;
		}
		offset++; // TID/U/SID/D byte
		if (!F) {
			// Non-flexible mode: TL0PICIDX follows
			if (offset >= size) {
				return result;
			}
			offset++; // TL0PICIDX
		}
	}

	// --- P_DIFFs (flexible mode, present when F=1 and P=1) ---
	if (F && P) {
		// Each P_DIFF is one byte: bits[7:1] = P_DIFF value, bit[0] = N (another follows if 1)
		// RFC 9628 allows at most 3 P_DIFFs.
		for (int i = 0; i < 3; ++i) {
			if (offset >= size) {
				return result;
			}
			const bool N = (payload[offset] & 0x01) != 0;
			offset++;
			if (!N) {
				break;
			}
		}
	}

	// --- Scalability structure (optional) ---
	if (V) {
		if (offset >= size) {
			return result;
		}
		const uint8_t ss = payload[offset++];
		const uint8_t N_S = static_cast<uint8_t>((ss >> 5) & 0x07);
		const bool Y = (ss & 0x10) != 0;
		const bool G = (ss & 0x08) != 0;

		// Per-layer resolution: (N_S + 1) entries of WIDTH(2) + HEIGHT(2) = 4 bytes each
		if (Y) {
			const size_t resolutionBytes = static_cast<size_t>(N_S + 1) * 4;
			if (offset + resolutionBytes > size) {
				return result;
			}
			offset += resolutionBytes;
		}

		// Picture group description
		if (G) {
			if (offset >= size) {
				return result;
			}
			const uint8_t N_G = payload[offset++];
			for (uint8_t g = 0; g < N_G; ++g) {
				// |T(3)|U|R(2)|RES(2)|
				if (offset >= size) {
					return result;
				}
				const uint8_t R = (payload[offset] >> 2) & 0x03;
				offset++;
				// R reference P_DIFF bytes (8-bit each in SS context)
				if (offset + R > size) {
					return result;
				}
				offset += R;
			}
		}
	}

	result.valid = true;
	result.startOfFrame = B;
	result.endOfFrame = E;
	result.payloadOffset = offset;
	return result;
}

} // namespace vdoninja
