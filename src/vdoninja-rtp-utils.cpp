/*
 * OBS VDO.Ninja Plugin
 * RTP utility functions — codec-specific payload descriptor parsing
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#include "vdoninja-rtp-utils.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <iomanip>
#include <map>
#include <sstream>

namespace vdoninja
{

bool addRtpColorSpaceExtension(std::vector<std::byte> &packet, uint8_t id, const std::array<uint8_t, 4> &color)
{
	if (id == 0 || id > 14 || packet.size() <= 12 || packet[0] != std::byte{0x80} ||
	    !(std::to_integer<uint8_t>(packet[1]) & 0x80))
		return false;
	const std::byte extension[] = {std::byte{0xbe},
	                               std::byte{0xde},
	                               std::byte{0},
	                               std::byte{2},
	                               std::byte{static_cast<uint8_t>((id << 4) | 3)},
	                               std::byte{color[0]},
	                               std::byte{color[1]},
	                               std::byte{color[2]},
	                               std::byte{color[3]},
	                               std::byte{0},
	                               std::byte{0},
	                               std::byte{0}};
	packet.insert(packet.begin() + 12, std::begin(extension), std::end(extension));
	packet[0] = std::byte{0x90};
	return true;
}

namespace
{
struct CodecUnit {
	const uint8_t *data;
	size_t size;
};

bool readLeb(const uint8_t *data, size_t size, size_t &offset, size_t &value)
{
	value = 0;
	for (unsigned i = 0; i < 8 && offset < size; ++i) {
		const uint8_t byte = data[offset++];
		// All supported units fit in the receive bound; reject oversized lengths
		// before shifting (also safe on 32-bit hosts).
		if (i >= 4 && (byte & 0x7f))
			return false;
		if (i < 4)
			value |= static_cast<size_t>(byte & 0x7f) << (7 * i);
		if (!(byte & 0x80))
			return value <= VideoRtpAssembler::maxBytes;
	}
	return false;
}

void writeLeb(std::vector<uint8_t> &out, size_t value)
{
	do {
		const auto byte = static_cast<uint8_t>(value & 0x7f);
		value >>= 7;
		out.push_back(byte | (value ? 0x80 : 0));
	} while (value);
}

bool hevcUnits(const uint8_t *data, size_t size, std::vector<CodecUnit> &units)
{
	if (!data || size < 2 || size > VideoRtpAssembler::maxBytes)
		return false;
	const auto startCode = [&](size_t offset) -> size_t {
		if (offset + 3 <= size && data[offset] == 0 && data[offset + 1] == 0) {
			if (data[offset + 2] == 1)
				return 3;
			if (offset + 4 <= size && data[offset + 2] == 0 && data[offset + 3] == 1)
				return 4;
		}
		return 0;
	};
	if (startCode(0)) {
		size_t offset = 0;
		while (offset < size) {
			const size_t prefix = startCode(offset);
			if (!prefix)
				return false;
			const size_t begin = offset + prefix;
			offset = begin;
			while (offset < size && !startCode(offset))
				++offset;
			size_t end = offset;
			while (end > begin && data[end - 1] == 0)
				--end;
			if (end - begin < 2)
				return false;
			units.push_back({data + begin, end - begin});
		}
	} else {
		// OBS emits Annex B; also accept four-byte length-prefixed access units.
		size_t offset = 0;
		while (offset + 4 <= size) {
			const size_t length = (size_t(data[offset]) << 24) | (size_t(data[offset + 1]) << 16) |
			                      (size_t(data[offset + 2]) << 8) | data[offset + 3];
			offset += 4;
			if (length < 2 || length > size - offset)
				return false;
			units.push_back({data + offset, length});
			offset += length;
		}
		if (offset != size)
			return false;
	}
	for (const auto &unit : units) {
		// Single-layer HEVC, nonzero temporal_id_plus1, no forbidden bit.
		if ((unit.data[0] & 0x81) || (unit.data[1] & 0xf8) || !(unit.data[1] & 7) || ((unit.data[0] >> 1) & 0x3f) >= 48)
			return false;
	}
	return !units.empty();
}

bool av1Units(const uint8_t *data, size_t size, VideoRtpPayloads &units)
{
	if (!data || !size || size > VideoRtpAssembler::maxBytes)
		return false;
	size_t offset = 0;
	while (offset < size) {
		const uint8_t header = data[offset++];
		if (header & 0x81)
			return false;
		std::vector<uint8_t> unit{static_cast<uint8_t>(header & ~2)};
		if (header & 4) {
			if (offset == size || (data[offset] & 7))
				return false;
			unit.push_back(data[offset++]);
		}
		size_t length = size - offset;
		if ((header & 2) && !readLeb(data, size, offset, length))
			return false;
		if (length > size - offset)
			return false;
		const unsigned type = (header >> 3) & 15;
		if (type != 2 && type != 8 && type != 15) {
			unit.insert(unit.end(), data + offset, data + offset + length);
			units.push_back(std::move(unit));
		}
		offset += length;
	}
	return !units.empty();
}
} // namespace

bool isHevcKeyframe(const uint8_t *data, size_t size)
{
	std::vector<CodecUnit> units;
	if (!hevcUnits(data, size, units))
		return false;
	return std::any_of(units.begin(), units.end(), [](const auto &unit) {
		const unsigned type = (unit.data[0] >> 1) & 63;
		return type >= 16 && type <= 21;
	});
}

bool av1HasSequenceHeader(const uint8_t *data, size_t size)
{
	VideoRtpPayloads units;
	if (!av1Units(data, size, units))
		return false;
	return std::any_of(units.begin(), units.end(), [](const auto &unit) { return ((unit[0] >> 3) & 15) == 1; });
}

std::optional<std::string> encodedVideoFormat(ExtendedVideoCodec codec, const uint8_t *data, size_t size)
{
	if (codec == ExtendedVideoCodec::HEVC) {
		std::vector<CodecUnit> units;
		if (!hevcUnits(data, size, units))
			return std::nullopt;
		for (const auto &unit : units) {
			if (((unit.data[0] >> 1) & 63) != 33)
				continue;
			std::vector<uint8_t> rbsp;
			unsigned zeros = 0;
			for (size_t i = 2; i < unit.size; ++i) {
				if (zeros >= 2 && unit.data[i] == 3) {
					zeros = 0;
					continue;
				}
				zeros = unit.data[i] == 0 ? zeros + 1 : 0;
				rbsp.push_back(unit.data[i]);
			}
			if (rbsp.size() < 13 || !(rbsp[1] & 31))
				return std::nullopt;
			std::ostringstream fmtp;
			fmtp << "profile-space=" << (rbsp[1] >> 6) << ";profile-id=" << (rbsp[1] & 31)
			     << ";tier-flag=" << ((rbsp[1] >> 5) & 1) << ";level-id=" << unsigned(rbsp[12])
			     << ";interop-constraints=";
			for (size_t i = 6; i < 12; ++i)
				fmtp << std::hex << std::setfill('0') << std::setw(2) << unsigned(rbsp[i]);
			return fmtp.str();
		}
		return std::nullopt;
	}
	VideoRtpPayloads units;
	if (!av1Units(data, size, units))
		return std::nullopt;
	for (const auto &unit : units) {
		if (((unit[0] >> 3) & 15) != 1)
			continue;
		size_t bit = (unit[0] & 4 ? 2 : 1) * 8;
		bool valid = true;
		const auto read = [&](unsigned count) -> uint32_t {
			if (count > 32 || bit + count > unit.size() * 8) {
				valid = false;
				return 0;
			}
			uint32_t value = 0;
			while (count--) {
				value = (value << 1) | ((unit[bit / 8] >> (7 - bit % 8)) & 1);
				++bit;
			}
			return value;
		};
		const unsigned profile = read(3);
		read(1); // still_picture
		const bool reduced = read(1) != 0;
		if (!reduced) {
			if (read(1)) { // timing_info_present_flag
				read(32);
				read(32);
				if (read(1)) { // equal_picture_interval: UVLC
					unsigned leading = 0;
					while (valid && !read(1) && leading < 32)
						++leading;
					if (leading < 32)
						read(leading);
				}
				if (read(1)) { // decoder_model_info_present_flag
					read(5);
					read(32);
					read(5);
					read(5);
				}
			}
			read(1);  // initial_display_delay_present_flag
			read(5);  // operating_points_cnt_minus_1
			read(12); // operating_point_idc[0]; OBS emits a single operating point.
		}
		const unsigned level = read(5);
		const unsigned tier = !reduced && level > 7 ? read(1) : 0;
		if (!valid || profile > 2 || level > 23)
			return std::nullopt;
		return "profile=" + std::to_string(profile) + ";level-idx=" + std::to_string(level) +
		       ";tier=" + std::to_string(tier);
	}
	return std::nullopt;
}

bool offeredVideoFormatSupports(ExtendedVideoCodec codec, const std::string &offer, const std::string &encoded)
{
	bool valid = true;
	const auto parameters = [&](const std::string &fmtp) {
		std::map<std::string, unsigned> result;
		std::istringstream stream(fmtp);
		std::string token;
		while (std::getline(stream, token, ';')) {
			const size_t equals = token.find('=');
			if (equals == std::string::npos)
				continue;
			std::string key = token.substr(0, equals);
			key.erase(0, key.find_first_not_of(" \t"));
			key.erase(key.find_last_not_of(" \t") + 1);
			if (key != "profile-space" && key != "profile-id" && key != "tier-flag" && key != "level-id" &&
			    key != "sprop-max-don-diff" && key != "profile" && key != "tier" && key != "level-idx")
				continue;
			unsigned value = 0;
			std::istringstream number(token.substr(equals + 1));
			if (number >> value) {
				number >> std::ws;
				if (!number.eof() || value > 65535 || !result.emplace(key, value).second)
					valid = false;
			} else
				valid = false;
		}
		return result;
	};
	const auto offered = parameters(offer), actual = parameters(encoded);
	if (!valid)
		return false;
	const auto get = [](const auto &values, const char *key, unsigned fallback) {
		const auto it = values.find(key);
		return it == values.end() ? fallback : it->second;
	};
	if (codec == ExtendedVideoCodec::HEVC) {
		// Range extensions share profile-id=4. Their constraint bits distinguish
		// 4:4:4, bit depths and other incompatible formats; require exact agreement.
		if (get(actual, "profile-id", 1) >= 4) {
			const auto constraints = [](const std::string &fmtp) {
				std::istringstream stream(fmtp);
				std::string token;
				while (std::getline(stream, token, ';')) {
					token.erase(
					    std::remove_if(token.begin(), token.end(), [](unsigned char c) { return std::isspace(c); }),
					    token.end());
					std::transform(token.begin(), token.end(), token.begin(),
					               [](unsigned char c) { return std::tolower(c); });
					if (token.rfind("interop-constraints=", 0) == 0)
						return token.substr(19);
				}
				return std::string("b00000000000");
			};
			if (constraints(offer) != constraints(encoded))
				return false;
		}
		return get(offered, "profile-space", 0) == get(actual, "profile-space", 0) &&
		       get(offered, "profile-id", 1) == get(actual, "profile-id", 1) &&
		       get(offered, "tier-flag", 0) >= get(actual, "tier-flag", 0) &&
		       get(offered, "level-id", 93) >= get(actual, "level-id", 93) &&
		       get(offered, "sprop-max-don-diff", 0) == 0;
	}
	return get(offered, "profile", 0) >= get(actual, "profile", 0) &&
	       get(offered, "tier", 0) >= get(actual, "tier", 0) &&
	       get(offered, "level-idx", 5) >= get(actual, "level-idx", 5);
}

std::optional<VideoRtpPayloads> packetizeHevc(const uint8_t *data, size_t size, size_t maxPayload)
{
	std::vector<CodecUnit> units;
	if (maxPayload < 4 || !hevcUnits(data, size, units))
		return std::nullopt;
	VideoRtpPayloads result;
	for (const auto &unit : units) {
		if (unit.size <= maxPayload) {
			result.emplace_back(unit.data, unit.data + unit.size);
			continue;
		}
		for (size_t offset = 2; offset < unit.size;) {
			const size_t chunk = std::min(maxPayload - 3, unit.size - offset);
			std::vector<uint8_t> payload{static_cast<uint8_t>((unit.data[0] & 0x81) | (49 << 1)), unit.data[1],
			                             static_cast<uint8_t>(((unit.data[0] >> 1) & 63) | (offset == 2 ? 0x80 : 0) |
			                                                  (offset + chunk == unit.size ? 0x40 : 0))};
			payload.insert(payload.end(), unit.data + offset, unit.data + offset + chunk);
			result.push_back(std::move(payload));
			offset += chunk;
		}
	}
	return result;
}

std::optional<VideoRtpPayloads> packetizeAv1(const uint8_t *data, size_t size, size_t maxPayload)
{
	VideoRtpPayloads units, result;
	if (maxPayload < 4 || !av1Units(data, size, units))
		return std::nullopt;
	for (const auto &unit : units) {
		for (size_t offset = 0; offset < unit.size();) {
			const size_t chunk = std::min(maxPayload - 1, unit.size() - offset);
			// W=1: a single OBU element, with its length inferred from the packet.
			const bool sequenceStart = result.empty() && ((unit[0] >> 3) & 15) == 1;
			std::vector<uint8_t> payload{static_cast<uint8_t>(
			    0x10 | (offset ? 0x80 : 0) | (offset + chunk < unit.size() ? 0x40 : 0) | (sequenceStart ? 8 : 0))};
			payload.insert(payload.end(), unit.begin() + offset, unit.begin() + offset + chunk);
			result.push_back(std::move(payload));
			offset += chunk;
		}
	}
	return result;
}

void VideoRtpAssembler::reset()
{
	active_ = false;
	rejectedTimestamp_.reset();
	frame_.clear();
	fragment_.clear();
}

std::optional<std::vector<uint8_t>> VideoRtpAssembler::push(ExtendedVideoCodec codec, const uint8_t *payload,
                                                            size_t size, uint32_t timestamp, bool marker)
{
	if (rejectedTimestamp_ == timestamp)
		return std::nullopt;
	if (!active_ || timestamp != timestamp_) {
		reset();
		active_ = true;
		timestamp_ = timestamp;
	}
	if (!payload || !size || size > maxBytes / 4 || frame_.size() + fragment_.size() + size * 4 > maxBytes ||
	    !(codec == ExtendedVideoCodec::HEVC ? appendHevc(payload, size) : appendAv1(payload, size))) {
		reset();
		rejectedTimestamp_ = timestamp;
		return std::nullopt;
	}
	if (!marker)
		return std::nullopt;
	if (!fragment_.empty() || frame_.empty()) {
		reset();
		rejectedTimestamp_ = timestamp;
		return std::nullopt;
	}
	auto result = std::move(frame_);
	reset();
	return result;
}

bool VideoRtpAssembler::appendHevc(const uint8_t *payload, size_t size)
{
	if (size < 2 || (payload[0] & 0x81) || (payload[1] & 0xf8) || !(payload[1] & 7))
		return false;
	const unsigned type = (payload[0] >> 1) & 63;
	const auto appendNal = [&](const uint8_t *data, size_t length) {
		frame_.insert(frame_.end(), {0, 0, 0, 1});
		frame_.insert(frame_.end(), data, data + length);
	};
	if (type < 48) {
		if (!fragment_.empty())
			return false;
		appendNal(payload, size);
		return true;
	}
	if (type == 48) {
		if (!fragment_.empty())
			return false;
		size_t offset = 2;
		unsigned count = 0;
		while (offset + 2 <= size) {
			const size_t length = (size_t(payload[offset]) << 8) | payload[offset + 1];
			offset += 2;
			if (length < 2 || length > size - offset || ((payload[offset] >> 1) & 63) >= 48 ||
			    (payload[offset] & 0x81) || (payload[offset + 1] & 0xf8) || !(payload[offset + 1] & 7))
				return false;
			appendNal(payload + offset, length);
			offset += length;
			++count;
		}
		return offset == size && count > 0;
	}
	if (type != 49 || size < 4)
		return false; // No PACI or DONL in the negotiated single-layer mode.
	const bool start = (payload[2] & 0x80) != 0, end = (payload[2] & 0x40) != 0;
	const unsigned nalType = payload[2] & 63;
	if (nalType >= 48 || (start && end))
		return false;
	const uint8_t header = static_cast<uint8_t>((payload[0] & 0x81) | (nalType << 1));
	if (start) {
		if (!fragment_.empty())
			return false;
		fragment_ = {header, payload[1]};
	} else if (fragment_.size() < 2 || fragment_[0] != header || fragment_[1] != payload[1])
		return false;
	fragment_.insert(fragment_.end(), payload + 3, payload + size);
	if (end) {
		appendNal(fragment_.data(), fragment_.size());
		fragment_.clear();
	}
	return true;
}

bool VideoRtpAssembler::appendObu(const uint8_t *data, size_t size)
{
	if (!size || (data[0] & 0x81))
		return false;
	const size_t headerSize = (data[0] & 4) ? 2 : 1;
	if (size < headerSize || (headerSize == 2 && (data[1] & 7)))
		return false;
	size_t offset = headerSize, length = size - offset;
	if (data[0] & 2) {
		if (!readLeb(data, size, offset, length) || length != size - offset)
			return false;
	}
	const unsigned type = (data[0] >> 3) & 15;
	if (type == 2 || type == 8 || type == 15)
		return true;
	frame_.push_back(data[0] | 2); // FFmpeg's low-overhead OBU format needs the size field.
	if (headerSize == 2)
		frame_.push_back(data[1]);
	writeLeb(frame_, length);
	frame_.insert(frame_.end(), data + offset, data + size);
	return true;
}

bool VideoRtpAssembler::appendAv1(const uint8_t *payload, size_t size)
{
	if (size < 2 || (payload[0] & 7))
		return false;
	if ((payload[0] & 8) && !frame_.empty())
		return false;
	const bool continuation = (payload[0] & 0x80) != 0, continues = (payload[0] & 0x40) != 0;
	if ((continuation && (payload[0] & 8)) || continuation != !fragment_.empty())
		return false;
	const unsigned count = (payload[0] >> 4) & 3;
	size_t offset = 1;
	unsigned index = 0;
	while (offset < size) {
		++index;
		size_t length = size - offset;
		if ((!count || index < count) && !readLeb(payload, size, offset, length))
			return false;
		if (!length || length > size - offset || (count && index > count))
			return false;
		const bool last = offset + length == size;
		if ((index == 1 && continuation) || (last && continues)) {
			fragment_.insert(fragment_.end(), payload + offset, payload + offset + length);
			if (!(last && continues)) {
				if (!appendObu(fragment_.data(), fragment_.size()))
					return false;
				fragment_.clear();
			}
		} else if (!appendObu(payload + offset, length))
			return false;
		offset += length;
	}
	return index != 0 && (!count || index == count);
}

bool isVp9Keyframe(const uint8_t *data, size_t size)
{
	if (!data || size < 4)
		return false;
	size_t bit = 0;
	const auto read = [&](unsigned count) {
		unsigned value = 0;
		while (count--) {
			value = (value << 1) | ((data[bit / 8] >> (7 - bit % 8)) & 1);
			++bit;
		}
		return value;
	};
	if (read(2) != 2)
		return false;
	const unsigned low = read(1);
	const unsigned profile = low | (read(1) << 1);
	if (profile == 3 && (size < 5 || read(1) != 0))
		return false;
	if (read(1) != 0 || read(1) != 0)
		return false; // show-existing / inter frame
	read(2);          // show-frame / error-resilient
	return read(24) == 0x498342;
}

bool isH264Keyframe(const uint8_t *data, size_t size)
{
	if (!data)
		return false;
	for (size_t i = 0; i + 3 < size; ++i) {
		if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1 && (data[i + 3] & 0x1f) == 5)
			return true;
	}
	return false;
}

std::optional<RtpPayloadView> parseRtpPayloadView(const uint8_t *packetData, size_t packetSize)
{
	if (!packetData || packetSize < 12) {
		return std::nullopt;
	}

	const uint8_t version = static_cast<uint8_t>((packetData[0] >> 6) & 0x03);
	if (version != 2) {
		return std::nullopt;
	}

	// With RTP/RTCP mux enabled, track callbacks can surface RTCP control packets
	// alongside media packets. RTCP packet types occupy the raw second-octet range
	// 192-223, while our RTP payload types are dynamic values outside that band.
	const uint8_t rawPacketType = packetData[1];
	if (rawPacketType >= 192 && rawPacketType <= 223) {
		return std::nullopt;
	}

	const bool hasPadding = (packetData[0] & 0x20) != 0;
	const bool hasExtension = (packetData[0] & 0x10) != 0;
	const size_t csrcCount = static_cast<size_t>(packetData[0] & 0x0F);
	const uint8_t payloadType = static_cast<uint8_t>(packetData[1] & 0x7F);

	size_t headerSize = 12 + (csrcCount * 4);
	if (headerSize > packetSize) {
		return std::nullopt;
	}

	if (hasExtension) {
		if (headerSize + 4 > packetSize) {
			return std::nullopt;
		}
		const size_t extensionWords = static_cast<size_t>((static_cast<uint16_t>(packetData[headerSize + 2]) << 8) |
		                                                  static_cast<uint16_t>(packetData[headerSize + 3]));
		headerSize += 4 + (extensionWords * 4);
		if (headerSize > packetSize) {
			return std::nullopt;
		}
	}

	size_t payloadSize = packetSize - headerSize;
	if (hasPadding) {
		const uint8_t paddingSize = packetData[packetSize - 1];
		if (paddingSize == 0 || paddingSize > payloadSize) {
			return std::nullopt;
		}
		payloadSize -= paddingSize;
	}

	if (payloadSize == 0) {
		return std::nullopt;
	}

	return RtpPayloadView{headerSize, payloadSize, payloadType};
}

std::optional<std::vector<uint8_t>> extractRedPrimaryPayload(const uint8_t *payload, size_t payloadSize)
{
	if (!payload || payloadSize < 2) {
		return std::nullopt;
	}
	size_t index = 0;
	size_t redundantBytes = 0;
	while (index < payloadSize) {
		if ((payload[index] & 0x80) == 0) {
			++index;
			if (redundantBytes >= payloadSize - index) {
				return std::nullopt;
			}
			index += redundantBytes;
			return std::vector<uint8_t>(payload + index, payload + payloadSize);
		}
		if (payloadSize - index < 4) {
			return std::nullopt;
		}
		const size_t blockLength = (static_cast<size_t>(payload[index + 2] & 0x03) << 8) | payload[index + 3];
		if (blockLength > payloadSize - redundantBytes) {
			return std::nullopt;
		}
		redundantBytes += blockLength;
		index += 4;
	}
	return std::nullopt;
}

std::optional<size_t> normalizeRtxPacket(uint8_t *packetData, size_t packetSize, uint8_t originalPayloadType)
{
	const auto payload = parseRtpPayloadView(packetData, packetSize);
	if (!payload || payload->size <= 2 || originalPayloadType > 127) {
		return std::nullopt;
	}
	// The first two payload bytes are the original sequence number. Preserve
	// CSRCs, header extensions, marker, SSRC, and trailing RTP padding.
	packetData[2] = packetData[payload->offset];
	packetData[3] = packetData[payload->offset + 1];
	packetData[1] = (packetData[1] & 0x80) | originalPayloadType;
	std::memmove(packetData + payload->offset, packetData + payload->offset + 2, packetSize - payload->offset - 2);
	return packetSize - 2;
}

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
			if ((payload[offset] >> 1) == 0 || (i == 2 && N)) {
				// RFC 9628 forbids zero P_DIFF and more than three references.
				return result;
			}
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

bool isRtcpSenderReportDue(uint32_t currentTimestamp, uint32_t lastReportedTimestamp, uint32_t clockRate)
{
	if (clockRate == 0) {
		return false;
	}

	// Modular subtraction keeps forward progress correct across the 32-bit
	// wrap, but it also turns a backwards timestamp into a value just under
	// 2^32, which would read as a hugely overdue report. Anything landing in
	// the upper half of the range is a regression, not elapsed time.
	constexpr uint32_t kBackwardsThreshold = 0x80000000u;
	const uint32_t elapsed = currentTimestamp - lastReportedTimestamp;
	if (elapsed >= kBackwardsThreshold) {
		return false;
	}

	return elapsed >= clockRate;
}

uint32_t sanitizeMonotonicTimestamp(uint32_t candidate, bool &hasLast, uint32_t &last, uint32_t fallbackStep)
{
	if (!hasLast) {
		hasLast = true;
		last = candidate;
		return candidate;
	}

	// A forward timestamp can be numerically smaller after wrapping. Match the
	// modular ordering used by the sender-report and continuity helpers.
	const uint32_t elapsed = candidate - last;
	if (elapsed == 0 || elapsed >= 0x80000000u) {
		candidate = last + fallbackStep;
	}

	last = candidate;
	return candidate;
}

std::vector<uint8_t> buildOpusRtpPacket(const uint8_t *payload, size_t payloadSize, uint8_t payloadType,
                                        uint16_t sequenceNumber, uint32_t timestamp, uint32_t ssrc)
{
	std::vector<uint8_t> packet;
	if ((!payload && payloadSize != 0) || payloadSize > packet.max_size() - 12) {
		return {};
	}

	packet.resize(12 + payloadSize);
	packet[0] = 0x80;               // V=2, P=0, X=0, CC=0
	packet[1] = payloadType & 0x7F; // M=0
	packet[2] = sequenceNumber >> 8;
	packet[3] = sequenceNumber & 0xFF;
	packet[4] = timestamp >> 24;
	packet[5] = (timestamp >> 16) & 0xFF;
	packet[6] = (timestamp >> 8) & 0xFF;
	packet[7] = timestamp & 0xFF;
	packet[8] = ssrc >> 24;
	packet[9] = (ssrc >> 16) & 0xFF;
	packet[10] = (ssrc >> 8) & 0xFF;
	packet[11] = ssrc & 0xFF;
	if (payloadSize != 0) {
		std::memcpy(packet.data() + 12, payload, payloadSize);
	}
	return packet;
}

RtpTimestampStepTracker::RtpTimestampStepTracker(uint32_t expectedStep) : expectedStep_(expectedStep) {}

void RtpTimestampStepTracker::observe(uint32_t timestamp)
{
	interval_.packets++;
	if (!hasLastTimestamp_) {
		hasLastTimestamp_ = true;
		lastTimestamp_ = timestamp;
		return;
	}

	const uint32_t step = timestamp - lastTimestamp_;
	lastTimestamp_ = timestamp;

	constexpr uint32_t kBackwardsThreshold = 0x80000000u;
	if (step == 0 || step >= kBackwardsThreshold) {
		interval_.nonForwardSteps++;
		return;
	}

	interval_.maxForwardStep = std::max(interval_.maxForwardStep, step);
	if (expectedStep_ != 0 && step > expectedStep_) {
		interval_.largeSteps++;
	}
}

RtpTimestampStepStats RtpTimestampStepTracker::takeInterval()
{
	const RtpTimestampStepStats snapshot = interval_;
	interval_ = {};
	return snapshot;
}

void RtpTimestampStepTracker::reset()
{
	hasLastTimestamp_ = false;
	lastTimestamp_ = 0;
	interval_ = {};
}

} // namespace vdoninja
