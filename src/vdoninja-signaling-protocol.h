/*
 * OBS VDO.Ninja Plugin
 * Signaling protocol normalization helpers
 */

#pragma once

#include <string>
#include <vector>

namespace vdoninja
{

enum class ParsedSignalKind {
	Unknown,
	Listing,
	Offer,
	Answer,
	Candidate,
	CandidatesBundle,
	Request,
	Alert,
	VideoAddedToRoom,
	VideoRemovedFromRoom
};

struct ParsedCandidate {
	std::string candidate;
	std::string mid;
};

struct ParsedSignalMessage {
	ParsedSignalKind kind = ParsedSignalKind::Unknown;
	std::string uuid;
	std::string session;
	std::string sdp;
	std::string type;
	std::string candidate;
	std::string mid;
	std::vector<ParsedCandidate> candidates;
	std::string request;
	std::string alert;
	std::string streamId;
	std::vector<std::string> listingMembers;
};

bool parseSignalingMessage(const std::string &message, ParsedSignalMessage &parsed, std::string *error = nullptr);

} // namespace vdoninja
