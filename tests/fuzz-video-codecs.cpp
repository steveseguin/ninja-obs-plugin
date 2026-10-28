/* SPDX-License-Identifier: AGPL-3.0-only */
// Standalone libFuzzer target; link vdoninja-rtp-utils.cpp with ASan/UBSan.
#include <algorithm>
#include <cstdlib>
#include <string>

#include "vdoninja-rtcp-feedback.h"
#include "vdoninja-rtp-repair.h"
#include "vdoninja-rtp-utils.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	using namespace vdoninja;
	if (size > 65536)
		return 0;
	parseRtpPayloadView(data, size);
	parseVP9PayloadDescriptor(data, size);
	isVp9Keyframe(data, size);
	isH264Keyframe(data, size);
	isHevcKeyframe(data, size);
	av1HasSequenceHeader(data, size);
	parseRtcpNackRequests(data, size, 0x11223344);
	RtcpFeedbackTracker feedback(0x11223344);
	feedback.observe(data, size);
	feedback.snapshot();
	extractRedPrimaryPayload(data, size);
	std::vector<std::byte> rtp(size);
	std::transform(data, data + size, rtp.begin(), [](uint8_t b) { return std::byte{b}; });
	addRtpColorSpaceExtension(rtp, size ? data[0] : 0, {1, 1, 1, 0x10});
	// Give arbitrary FCI bytes a valid NACK envelope/SSRC too, so fuzzing reaches
	// sequence expansion and feedback accounting instead of only header rejection.
	const size_t fciSize = std::min(size, size_t(256)) & ~size_t(3);
	std::vector<uint8_t> nack(12 + fciSize, 0);
	nack[0] = 0x81;
	nack[1] = 205;
	nack[3] = static_cast<uint8_t>(nack.size() / 4 - 1);
	nack[8] = 0x11;
	nack[9] = 0x22;
	nack[10] = 0x33;
	nack[11] = 0x44;
	std::copy(data, data + fciSize, nack.begin() + 12);
	parseRtcpNackRequests(nack.data(), nack.size(), 0x11223344);
	feedback.observe(nack.data(), nack.size());
	for (auto codec : {ExtendedVideoCodec::HEVC, ExtendedVideoCodec::AV1}) {
		encodedVideoFormat(codec, data, size);
		const std::string text(reinterpret_cast<const char *>(data), size);
		offeredVideoFormatSupports(codec, text, text);
		auto packets =
		    codec == ExtendedVideoCodec::HEVC ? packetizeHevc(data, size, 123) : packetizeAv1(data, size, 123);
		VideoRtpAssembler assembler;
		if (packets) {
			for (size_t i = 0; i < packets->size(); ++i) {
				const auto &packet = (*packets)[i];
				if (packet.size() > 123)
					std::abort();
				auto result = assembler.push(codec, packet.data(), packet.size(), 1234, i + 1 == packets->size());
				if (i + 1 == packets->size() && !result)
					std::abort();
			}
		}
		assembler.reset();
		// Stateful malformed/truncated fragments, marker changes, loss resets,
		// and timestamp changes. Exercise buffers across calls, not just parsers.
		uint32_t timestamp = 0xfffffff0;
		for (size_t offset = 0; offset < size;) {
			const uint8_t control = data[offset++];
			const size_t length = std::min(size - offset, size_t(control & 63));
			if (control & 64)
				timestamp += 3000;
			if (control == 0)
				assembler.reset();
			auto result = assembler.push(codec, data + offset, length, timestamp, control & 128);
			if (result && result->size() > VideoRtpAssembler::maxBytes)
				std::abort();
			offset += length;
		}
	}
	return 0;
}
