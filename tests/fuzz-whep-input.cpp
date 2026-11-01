/* SPDX-License-Identifier: AGPL-3.0-only */
// Exercise the production JSON and SDP parsers used by WHEP, without a network.
#include <cstdlib>

#include "vdoninja-utils.h"

extern "C" void blog(int, const char *, ...) {}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *bytes, size_t size)
{
	using namespace vdoninja;
	if (size > 2 * 1024 * 1024)
		return 0;
	const std::string input(reinterpret_cast<const char *>(bytes), size);
	const JsonParser request(input);
	for (const char *key : {"type", "streamID", "get", "sdp"})
		request.getString(key);
	const JsonParser candidate(request.getObject("candidate"));
	for (const char *key : {"usernameFragment", "candidate", "sdpMid"})
		candidate.getString(key);
	for (const auto &sdp : {input, request.getString("sdp")}) {
		const auto media = parseOfferedMediaSections(sdp);
		for (const auto &section : media)
			offeredMediaSectionCanSend(section);
		offerHasActiveVp9AlphaSection(media);
	}
	// JSON escaping must preserve every byte, including Unicode and controls.
	const auto encoded = JsonBuilder().add("sdp", input).build();
	if (JsonParser(encoded).getString("sdp") != input)
		std::abort();
	return 0;
}
