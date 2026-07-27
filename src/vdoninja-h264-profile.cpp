/*
 * OBS VDO.Ninja Plugin
 * H.264 profile-level-id extraction and safe fallback selection
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#include "vdoninja-h264-profile.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <limits>

namespace vdoninja
{

namespace
{

std::optional<std::string> profileLevelIdFromSps(const uint8_t *nal, size_t size)
{
	if (!nal || size < 4 || (nal[0] & 0x1FU) != 7 || nal[1] == 0 || nal[3] == 0) {
		return std::nullopt;
	}
	char value[7] = {};
	std::snprintf(value, sizeof(value), "%02x%02x%02x", nal[1], nal[2], nal[3]);
	return std::string(value);
}

uint32_t readU32(const uint8_t *data)
{
	return (static_cast<uint32_t>(data[0]) << 24U) | (static_cast<uint32_t>(data[1]) << 16U) |
	       (static_cast<uint32_t>(data[2]) << 8U) | static_cast<uint32_t>(data[3]);
}

bool startCodeAt(const uint8_t *data, size_t size, size_t offset, size_t &length)
{
	length = 0;
	if (offset + 3 <= size && data[offset] == 0 && data[offset + 1] == 0 && data[offset + 2] == 1) {
		length = 3;
		return true;
	}
	if (offset + 4 <= size && data[offset] == 0 && data[offset + 1] == 0 && data[offset + 2] == 0 &&
	    data[offset + 3] == 1) {
		length = 4;
		return true;
	}
	return false;
}

std::optional<std::string> fromAnnexB(const uint8_t *data, size_t size)
{
	for (size_t offset = 0; offset < size;) {
		size_t startCodeLength = 0;
		if (!startCodeAt(data, size, offset, startCodeLength)) {
			++offset;
			continue;
		}
		const size_t nalStart = offset + startCodeLength;
		if (nalStart >= size) {
			return std::nullopt;
		}
		size_t nalEnd = nalStart;
		while (nalEnd < size) {
			size_t ignoredLength = 0;
			if (startCodeAt(data, size, nalEnd, ignoredLength)) {
				break;
			}
			++nalEnd;
		}
		if (const auto value = profileLevelIdFromSps(data + nalStart, nalEnd - nalStart)) {
			return value;
		}
		offset = nalEnd;
	}
	return std::nullopt;
}

std::optional<std::string> fromLengthPrefixedAvcc(const uint8_t *data, size_t size)
{
	size_t offset = 0;
	while (offset + 4 <= size) {
		const uint32_t nalBytes = readU32(data + offset);
		offset += 4;
		if (nalBytes == 0 || nalBytes > size - offset) {
			return std::nullopt;
		}
		if (const auto value = profileLevelIdFromSps(data + offset, nalBytes)) {
			return value;
		}
		offset += nalBytes;
	}
	return std::nullopt;
}

} // namespace

std::optional<std::string> deriveH264ProfileLevelId(const uint8_t *data, size_t size)
{
	if (!data || size < 4) {
		return std::nullopt;
	}

	// ISO/IEC 14496-15 AVCDecoderConfigurationRecord.
	if (size >= 7 && data[0] == 1 && (data[4] & 0xFCU) == 0xFCU && (data[5] & 0xE0U) == 0xE0U && data[1] != 0 &&
	    data[3] != 0) {
		char value[7] = {};
		std::snprintf(value, sizeof(value), "%02x%02x%02x", data[1], data[2], data[3]);
		return std::string(value);
	}

	if (const auto value = fromAnnexB(data, size)) {
		return value;
	}
	if (const auto value = fromLengthPrefixedAvcc(data, size)) {
		return value;
	}
	return profileLevelIdFromSps(data, size);
}

std::string fallbackH264ProfileLevelId(uint32_t width, uint32_t height, uint32_t fpsNumerator, uint32_t fpsDenominator)
{
	const uint64_t safeWidth = std::max<uint32_t>(1, width);
	const uint64_t safeHeight = std::max<uint32_t>(1, height);
	const uint64_t macroblocksPerFrame = ((safeWidth + 15U) / 16U) * ((safeHeight + 15U) / 16U);
	const uint64_t denominator = std::max<uint32_t>(1, fpsDenominator);
	const long double macroblocksPerSecond = static_cast<long double>(macroblocksPerFrame) *
	                                         static_cast<long double>(std::max<uint32_t>(1, fpsNumerator)) /
	                                         static_cast<long double>(denominator);

	struct LevelLimit {
		uint64_t maxMacroblocksPerFrame;
		uint64_t maxMacroblocksPerSecond;
		const char *levelId;
	};
	static constexpr std::array<LevelLimit, 7> levels = {
	    LevelLimit{1620, 40500, "1e"},    LevelLimit{3600, 108000, "1f"}, LevelLimit{5120, 216000, "20"},
	    LevelLimit{8192, 245760, "28"},   LevelLimit{8704, 522240, "2a"}, LevelLimit{22080, 589824, "32"},
	    LevelLimit{36864, 2073600, "34"},
	};
	for (const auto &level : levels) {
		if (macroblocksPerFrame <= level.maxMacroblocksPerFrame &&
		    macroblocksPerSecond <= static_cast<long double>(level.maxMacroblocksPerSecond)) {
			return std::string("42e0") + level.levelId;
		}
	}
	return "42e034";
}

bool isValidH264ProfileLevelId(const std::string &profileLevelId)
{
	if (profileLevelId.size() != 6) {
		return false;
	}
	return std::all_of(profileLevelId.begin(), profileLevelId.end(),
	                   [](unsigned char value) { return std::isxdigit(value) != 0; });
}

std::string h264FmtpForProfileLevelId(const std::string &profileLevelId)
{
	const std::string safeProfile = isValidH264ProfileLevelId(profileLevelId) ? profileLevelId : "42e01f";
	return "profile-level-id=" + safeProfile + ";packetization-mode=1;level-asymmetry-allowed=1";
}

} // namespace vdoninja
