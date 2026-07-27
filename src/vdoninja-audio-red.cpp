/*
 * OBS VDO.Ninja Plugin
 * RFC 2198 audio RED helpers
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#include "vdoninja-audio-red.h"

#include <algorithm>
#include <cctype>
#include <sstream>

#include "vdoninja-utils.h"

namespace vdoninja
{

namespace
{

constexpr uint32_t kMaximumRedTimestampOffset = (1U << 14U) - 1U;
constexpr size_t kMaximumRedBlockLength = (1U << 10U) - 1U;

const SdpOfferedCodec *findCodec(const SdpOfferedMediaSection &section, int payloadType, const char *name)
{
	for (const auto &codec : section.codecs) {
		if (codec.payloadType != payloadType) {
			continue;
		}
		std::string codecName = codec.codec;
		std::transform(codecName.begin(), codecName.end(), codecName.begin(),
		               [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
		if (codecName == name) {
			return &codec;
		}
	}
	return nullptr;
}

bool validRedFormatParameters(const std::string &formatParameters, uint8_t opusPayloadType)
{
	if (formatParameters.empty()) {
		return false;
	}

	std::stringstream values(formatParameters);
	std::string value;
	size_t count = 0;
	while (std::getline(values, value, '/')) {
		if (value.empty()) {
			return false;
		}
		try {
			size_t consumed = 0;
			const int payloadType = std::stoi(value, &consumed);
			if (consumed != value.size() || payloadType != opusPayloadType) {
				return false;
			}
		} catch (...) {
			return false;
		}
		++count;
	}
	return count >= 2;
}

} // namespace

AudioRedPayload buildAudioRedPayload(const uint8_t *currentPayload, size_t currentPayloadSize,
                                     uint32_t currentTimestamp, const uint8_t *previousPayload,
                                     size_t previousPayloadSize, uint32_t previousTimestamp, uint8_t opusPayloadType,
                                     size_t maximumPayloadSize)
{
	AudioRedPayload result;
	if ((!currentPayload && currentPayloadSize != 0) || opusPayloadType > 127 || maximumPayloadSize == 0 ||
	    currentPayloadSize >= result.bytes.max_size()) {
		return result;
	}

	const uint32_t timestampOffset = currentTimestamp - previousTimestamp;
	bool validRedundantBlock = previousPayload && previousPayloadSize > 0 &&
	                           previousPayloadSize <= kMaximumRedBlockLength && timestampOffset > 0 &&
	                           timestampOffset <= kMaximumRedTimestampOffset && maximumPayloadSize >= 5 &&
	                           currentPayloadSize <= maximumPayloadSize - 5 &&
	                           previousPayloadSize <= maximumPayloadSize - 5 - currentPayloadSize;
	if (validRedundantBlock &&
	    previousPayloadSize > result.bytes.max_size() - currentPayloadSize - static_cast<size_t>(5)) {
		validRedundantBlock = false;
	}

	result.bytes.reserve(currentPayloadSize + (validRedundantBlock ? previousPayloadSize + 5 : 1));
	if (validRedundantBlock) {
		result.bytes.push_back(static_cast<uint8_t>(0x80U | opusPayloadType));
		result.bytes.push_back(static_cast<uint8_t>((timestampOffset >> 6U) & 0xFFU));
		result.bytes.push_back(
		    static_cast<uint8_t>(((timestampOffset & 0x3FU) << 2U) | ((previousPayloadSize >> 8U) & 0x03U)));
		result.bytes.push_back(static_cast<uint8_t>(previousPayloadSize & 0xFFU));
	}
	result.bytes.push_back(opusPayloadType);
	if (validRedundantBlock) {
		result.bytes.insert(result.bytes.end(), previousPayload, previousPayload + previousPayloadSize);
		result.includesRedundantBlock = true;
		result.redundantBytes = previousPayloadSize;
	}
	if (currentPayloadSize > 0) {
		result.bytes.insert(result.bytes.end(), currentPayload, currentPayload + currentPayloadSize);
	}
	return result;
}

bool answerSelectsAudioRed(const std::string &sdp, uint8_t redPayloadType, uint8_t opusPayloadType)
{
	for (const auto &section : parseOfferedMediaSections(sdp)) {
		if (section.type != "audio") {
			continue;
		}

		const SdpOfferedCodec *red = findCodec(section, redPayloadType, "red");
		const SdpOfferedCodec *opus = findCodec(section, opusPayloadType, "opus");
		if (!red || !opus || red->clockRate != 48000 || (red->channels != 0 && red->channels != 2) ||
		    opus->clockRate != 48000 || !validRedFormatParameters(red->formatParameters, opusPayloadType)) {
			return false;
		}

		for (const int payloadType : section.payloadTypes) {
			if (payloadType == redPayloadType) {
				return true;
			}
			if (payloadType == opusPayloadType) {
				return false;
			}
		}
		return false;
	}
	return false;
}

} // namespace vdoninja
