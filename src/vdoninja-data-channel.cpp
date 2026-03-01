/*
 * OBS VDO.Ninja Plugin
 * Data channel implementation
 */

#include "vdoninja-data-channel.h"

#include <algorithm>
#include <cctype>
#include <initializer_list>

namespace vdoninja
{

namespace
{

std::string firstNonEmptyValue(const JsonParser &json, const std::initializer_list<const char *> &keys)
{
	for (const char *key : keys) {
		if (!json.hasKey(key)) {
			continue;
		}
		const std::string value = trim(json.getString(key));
		if (!value.empty()) {
			return value;
		}
	}
	return "";
}

bool looksLikeWhepUrl(const std::string &candidate)
{
	return candidate.rfind("https://", 0) == 0 || candidate.rfind("http://", 0) == 0 ||
	       candidate.rfind("whep:", 0) == 0;
}

std::string extractWhepUrlRecursive(const JsonParser &json, int depth)
{
	if (depth > 3) {
		return "";
	}

	std::string direct =
	    firstNonEmptyValue(json, {"whepUrl", "whep", "whepplay", "whepPlay", "whepshare", "whepShare"});
	if (looksLikeWhepUrl(direct)) {
		return direct;
	}

	std::string urlValue = firstNonEmptyValue(json, {"url", "URL"});
	if (looksLikeWhepUrl(urlValue)) {
		return urlValue;
	}

	for (const char *nestedKey : {"whepSettings", "whepScreenSettings", "info", "data"}) {
		if (!json.hasKey(nestedKey)) {
			continue;
		}

		const std::string nestedObject = json.getObject(nestedKey);
		if (nestedObject.empty() || nestedObject[0] != '{') {
			continue;
		}

		JsonParser nestedJson(nestedObject);
		const std::string nestedUrl = extractWhepUrlRecursive(nestedJson, depth + 1);
		if (!nestedUrl.empty()) {
			return nestedUrl;
		}
	}

	return "";
}

std::string asciiLower(std::string value)
{
	std::transform(value.begin(), value.end(), value.begin(),
	               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return value;
}

bool isLegacyRemoteActionValue(const std::string &value)
{
	const std::string lowered = asciiLower(value);
	return lowered == "nextscene" || lowered == "prevscene" || lowered == "setscene" ||
	       lowered == "setcurrentscene" || lowered == "startstreaming" || lowered == "stopstreaming" ||
	       lowered == "startrecording" || lowered == "stoprecording" || lowered == "startvirtualcam" ||
	       lowered == "stopvirtualcam" || lowered == "mute" || lowered == "unmute";
}

std::string normalizeRemoteAction(std::string action)
{
	if (action == "setCurrentScene") {
		return "setScene";
	}
	return action;
}

} // namespace

VDONinjaDataChannel::VDONinjaDataChannel()
{
	logDebug("Data channel handler created");
}

VDONinjaDataChannel::~VDONinjaDataChannel() {}

DataMessage VDONinjaDataChannel::parseMessage(const std::string &rawMessage)
{
	DataMessage msg;
	msg.timestamp = currentTimeMs();

	try {
		JsonParser json(rawMessage);

		// Determine message type
		if (json.hasKey("chat") || json.hasKey("chatMessage")) {
			msg.type = DataMessageType::Chat;
			msg.data = json.getString("chat", json.getString("chatMessage"));
		} else if (json.hasKey("tally") || json.hasKey("tallyOn") || json.hasKey("tallyOff") ||
		           json.hasKey("tallyPreview")) {
			msg.type = DataMessageType::Tally;
			msg.data = rawMessage;
		} else if (json.hasKey("requestKeyframe") || json.hasKey("keyframe")) {
			msg.type = DataMessageType::RequestKeyframe;
		} else if (json.hasKey("muted") || json.hasKey("audioMuted") || json.hasKey("videoMuted")) {
			msg.type = DataMessageType::Mute;
			msg.data = rawMessage;
		} else if (json.hasKey("stats")) {
			msg.type = DataMessageType::Stats;
			msg.data = json.getString("stats");
		} else if (json.hasKey("obsCommand") || json.hasKey("action") ||
		           (json.hasKey("remote") && (json.hasKey("scene") || json.hasKey("value")))) {
			msg.type = DataMessageType::RemoteControl;
			msg.data = rawMessage;
		} else if (json.hasKey("custom") || json.hasKey("type")) {
			msg.type = DataMessageType::Custom;
			msg.data = rawMessage;
		}
	} catch (const std::exception &e) {
		logError("Failed to parse data message: %s", e.what());
	}

	return msg;
}

std::string VDONinjaDataChannel::createChatMessage(const std::string &message)
{
	JsonBuilder builder;
	builder.add("chat", message);
	builder.add("timestamp", currentTimeMs());
	return builder.build();
}

std::string VDONinjaDataChannel::createTallyMessage(const TallyState &state)
{
	JsonBuilder builder;

	if (state.program) {
		builder.add("tallyOn", true);
	} else if (state.preview) {
		builder.add("tallyPreview", true);
	} else {
		builder.add("tallyOff", true);
	}

	return builder.build();
}

std::string VDONinjaDataChannel::createMuteMessage(bool audioMuted, bool videoMuted)
{
	JsonBuilder builder;
	builder.add("audioMuted", audioMuted);
	builder.add("videoMuted", videoMuted);
	return builder.build();
}

std::string VDONinjaDataChannel::createKeyframeRequest()
{
	JsonBuilder builder;
	builder.add("requestKeyframe", true);
	return builder.build();
}

std::string VDONinjaDataChannel::createCustomMessage(const std::string &type, const std::string &data)
{
	JsonBuilder builder;
	builder.add("type", type);
	builder.add("data", data);
	builder.add("timestamp", currentTimeMs());
	return builder.build();
}

void VDONinjaDataChannel::handleMessage(const std::string &senderId, const std::string &rawMessage)
{
	DataMessage msg = parseMessage(rawMessage);
	msg.senderId = senderId;

	try {
		JsonParser json(rawMessage);

		switch (msg.type) {
		case DataMessageType::Chat:
			parseChatMessage(senderId, json);
			break;
		case DataMessageType::Tally:
			parseTallyMessage(senderId, json);
			break;
		case DataMessageType::Mute:
			parseMuteMessage(senderId, json);
			break;
		case DataMessageType::RequestKeyframe:
			if (onKeyframeRequest_) {
				onKeyframeRequest_(senderId);
			}
			break;
		case DataMessageType::RemoteControl:
			parseRemoteControlMessage(senderId, json);
			break;
		case DataMessageType::Custom:
			parseCustomMessage(senderId, json);
			break;
		default:
			logDebug("Unknown data message type from %s", senderId.c_str());
			break;
		}
	} catch (const std::exception &e) {
		logError("Error handling data message: %s", e.what());
	}
}

std::string VDONinjaDataChannel::extractWhepPlaybackUrl(const std::string &rawMessage) const
{
	if (rawMessage.empty()) {
		return "";
	}

	try {
		JsonParser json(rawMessage);
		return extractWhepUrlRecursive(json, 0);
	} catch (const std::exception &) {
		return "";
	}
}

void VDONinjaDataChannel::parseChatMessage(const std::string &senderId, const JsonParser &json)
{
	std::string message = json.getString("chat", json.getString("chatMessage"));

	logDebug("Chat from %s: %s", senderId.c_str(), message.c_str());

	if (onChatMessage_) {
		onChatMessage_(senderId, message);
	}
}

void VDONinjaDataChannel::parseTallyMessage(const std::string &senderId, const JsonParser &json)
{
	TallyState state;

	if (json.hasKey("tallyOn")) {
		state.program = json.getBool("tallyOn");
	}
	if (json.hasKey("tallyPreview")) {
		state.preview = json.getBool("tallyPreview");
	}
	if (json.hasKey("tallyOff") && json.getBool("tallyOff")) {
		state.program = false;
		state.preview = false;
	}

	// Store peer tally state
	{
		std::lock_guard<std::mutex> lock(mutex_);
		peerTallies_[senderId] = state;
	}

	logDebug("Tally from %s: program=%d, preview=%d", senderId.c_str(), state.program, state.preview);

	if (onTallyChange_) {
		onTallyChange_(senderId, state);
	}
}

void VDONinjaDataChannel::parseMuteMessage(const std::string &senderId, const JsonParser &json)
{
	bool audioMuted = json.getBool("audioMuted", json.getBool("muted"));
	bool videoMuted = json.getBool("videoMuted");

	logDebug("Mute from %s: audio=%d, video=%d", senderId.c_str(), audioMuted, videoMuted);

	if (onMuteChange_) {
		onMuteChange_(senderId, audioMuted, videoMuted);
	}
}

void VDONinjaDataChannel::parseCustomMessage(const std::string &senderId, const JsonParser &json)
{
	std::string data = json.getString("data");

	if (onCustomData_) {
		onCustomData_(senderId, data);
	}
}

void VDONinjaDataChannel::setOnChatMessage(OnChatMessageCallback callback)
{
	onChatMessage_ = callback;
}
void VDONinjaDataChannel::setOnTallyChange(OnTallyChangeCallback callback)
{
	onTallyChange_ = callback;
}
void VDONinjaDataChannel::setOnMuteChange(OnMuteChangeCallback callback)
{
	onMuteChange_ = callback;
}
void VDONinjaDataChannel::setOnCustomData(OnCustomDataCallback callback)
{
	onCustomData_ = callback;
}
void VDONinjaDataChannel::setOnKeyframeRequest(OnKeyframeRequestCallback callback)
{
	onKeyframeRequest_ = callback;
}
void VDONinjaDataChannel::setOnRemoteControl(OnRemoteControlCallback callback)
{
	onRemoteControl_ = callback;
}

void VDONinjaDataChannel::setLocalTally(const TallyState &state)
{
	std::lock_guard<std::mutex> lock(mutex_);
	localTally_ = state;
}

TallyState VDONinjaDataChannel::getLocalTally() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return localTally_;
}

TallyState VDONinjaDataChannel::getPeerTally(const std::string &peerId) const
{
	std::lock_guard<std::mutex> lock(mutex_);
	auto it = peerTallies_.find(peerId);
	if (it != peerTallies_.end()) {
		return it->second;
	}
	return TallyState{};
}

std::map<std::string, TallyState> VDONinjaDataChannel::getAllPeerTallies() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return peerTallies_;
}

void VDONinjaDataChannel::parseRemoteControlMessage(const std::string &senderId, const JsonParser &json)
{
	std::string action;
	std::string value;

	if (json.hasKey("obsCommand")) {
		const std::string commandObject = json.getObject("obsCommand");
		if (!commandObject.empty()) {
			JsonParser commandJson(commandObject);
			action = trim(commandJson.getString("action"));
			value = trim(commandJson.getString("value"));
		}
	}

	if (action.empty() && json.hasKey("action")) {
		action = trim(json.getString("action"));
	}

	if (value.empty()) {
		if (json.hasKey("value")) {
			value = trim(json.getString("value"));
		} else if (json.hasKey("scene")) {
			value = trim(json.getString("scene"));
		}
	}

	// Backward compatibility: older payloads used "remote" as the action key.
	if (action.empty() && json.hasKey("remote")) {
		const std::string remoteValue = trim(json.getString("remote"));
		if (isLegacyRemoteActionValue(remoteValue)) {
			action = remoteValue;
		}
	}

	action = normalizeRemoteAction(action);
	if (action.empty()) {
		return;
	}

	logInfo("Remote control from %s: action=%s value=%s", senderId.c_str(), action.c_str(), value.c_str());

	if (onRemoteControl_) {
		onRemoteControl_(action, value);
	}
}

} // namespace vdoninja
