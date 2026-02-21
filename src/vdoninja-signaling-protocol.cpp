/*
 * OBS VDO.Ninja Plugin
 * Signaling protocol normalization helpers
 */

#include "vdoninja-signaling-protocol.h"

#include <initializer_list>

#include "vdoninja-utils.h"

namespace vdoninja
{

namespace
{

std::string getAnyString(const JsonParser &json, const std::initializer_list<const char *> &keys)
{
	for (const char *key : keys) {
		if (json.hasKey(key)) {
			return json.getString(key);
		}
	}
	return "";
}

void parseCandidateBundle(const JsonParser &json, ParsedSignalMessage &parsed)
{
	auto rawCandidates = json.getArray("candidates");
	for (const auto &rawEntry : rawCandidates) {
		if (rawEntry.empty()) {
			continue;
		}

		if (rawEntry[0] == '{') {
			JsonParser candidateJson(rawEntry);
			ParsedCandidate candidate;
			candidate.candidate = getAnyString(candidateJson, {"candidate"});
			candidate.mid = getAnyString(candidateJson, {"mid", "sdpMid", "smid", "rmid"});
			if (!candidate.candidate.empty()) {
				parsed.candidates.push_back(candidate);
			}
		} else {
			ParsedCandidate candidate;
			candidate.candidate = rawEntry;
			candidate.mid = getAnyString(json, {"mid", "sdpMid", "smid", "rmid"});
			if (!candidate.candidate.empty()) {
				parsed.candidates.push_back(candidate);
			}
		}
	}
}

} // namespace

bool parseSignalingMessage(const std::string &message, ParsedSignalMessage &parsed, std::string *error)
{
	try {
		JsonParser json(message);

		parsed.uuid = getAnyString(json, {"UUID", "uuid"});
		parsed.session = getAnyString(json, {"session"});
		parsed.request = getAnyString(json, {"request"});

		if (parsed.request == "listing" || json.hasKey("listing") || json.hasKey("list")) {
			parsed.kind = ParsedSignalKind::Listing;
			auto listing = json.hasKey("list") ? json.getArray("list") : json.getArray("listing");
			for (const auto &member : listing) {
				if (member.empty()) {
					continue;
				}
				if (member[0] == '{') {
					JsonParser memberJson(member);
					std::string streamId =
					    getAnyString(memberJson, {"streamID", "streamId", "whep", "whepUrl", "url", "URL"});
					if (!streamId.empty()) {
						parsed.listingMembers.push_back(streamId);
					}
				} else {
					parsed.listingMembers.push_back(member);
				}
			}
			return true;
		}

		if (json.hasKey("description")) {
			JsonParser desc(json.getObject("description"));
			parsed.type = getAnyString(desc, {"type"});
			parsed.sdp = getAnyString(desc, {"sdp"});
		} else if (json.hasKey("sdp")) {
			parsed.type = getAnyString(json, {"type"});
			parsed.sdp = getAnyString(json, {"sdp"});
		}

		if (!parsed.sdp.empty()) {
			if (parsed.type == "offer") {
				parsed.kind = ParsedSignalKind::Offer;
				return true;
			}
			if (parsed.type == "answer") {
				parsed.kind = ParsedSignalKind::Answer;
				return true;
			}
		}

		if (json.hasKey("candidate")) {
			parsed.kind = ParsedSignalKind::Candidate;
			const std::string candidateRaw = json.getRaw("candidate");
			if (!candidateRaw.empty() && candidateRaw[0] == '{') {
				JsonParser candidateJson(candidateRaw);
				parsed.candidate = getAnyString(candidateJson, {"candidate"});
				parsed.mid = getAnyString(candidateJson, {"mid", "sdpMid", "smid", "rmid"});
			} else {
				parsed.candidate = getAnyString(json, {"candidate"});
				parsed.mid = getAnyString(json, {"mid", "sdpMid", "smid", "rmid"});
			}
			return true;
		}

		if (json.hasKey("candidates")) {
			parsed.kind = ParsedSignalKind::CandidatesBundle;
			parseCandidateBundle(json, parsed);
			return true;
		}

		if (parsed.request == "alert" || parsed.request == "error") {
			parsed.kind = ParsedSignalKind::Alert;
			parsed.alert = getAnyString(json, {"message", "alert", "error"});
			return true;
		}

		if (parsed.request == "videoaddedtoroom") {
			parsed.kind = ParsedSignalKind::VideoAddedToRoom;
			parsed.streamId = getAnyString(json, {"streamID", "streamId", "whep", "whepUrl", "url", "URL"});
			return true;
		}

		if (parsed.request == "videoremovedfromroom") {
			parsed.kind = ParsedSignalKind::VideoRemovedFromRoom;
			parsed.streamId = getAnyString(json, {"streamID", "streamId", "whep", "whepUrl", "url", "URL"});
			return true;
		}

		if (!parsed.request.empty()) {
			parsed.kind = ParsedSignalKind::Request;
			return true;
		}

		if (json.hasKey("alert")) {
			parsed.kind = ParsedSignalKind::Alert;
			parsed.alert = getAnyString(json, {"alert", "message"});
			return true;
		}

		if (json.hasKey("videoAddedToRoom")) {
			parsed.kind = ParsedSignalKind::VideoAddedToRoom;
			parsed.streamId = getAnyString(json, {"streamID", "streamId", "whep", "whepUrl", "url", "URL"});
			return true;
		}

		if (json.hasKey("videoRemovedFromRoom")) {
			parsed.kind = ParsedSignalKind::VideoRemovedFromRoom;
			parsed.streamId = getAnyString(json, {"streamID", "streamId", "whep", "whepUrl", "url", "URL"});
			return true;
		}

		parsed.kind = ParsedSignalKind::Unknown;
		return true;
	} catch (const std::exception &ex) {
		if (error) {
			*error = ex.what();
		}
		return false;
	}
}

} // namespace vdoninja
