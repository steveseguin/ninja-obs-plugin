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

bool looksLikePlaybackHint(const std::string &candidate)
{
	return candidate.rfind("https://", 0) == 0 || candidate.rfind("http://", 0) == 0 ||
	       candidate.rfind("whep:", 0) == 0;
}

std::string extractPlaybackHintRecursive(const JsonParser &json, int depth)
{
	if (depth > 3) {
		return "";
	}

	std::string direct =
	    firstNonEmptyValue(json, {"whepUrl", "whep", "whepplay", "whepPlay", "whepshare", "whepShare"});
	if (looksLikePlaybackHint(direct)) {
		return direct;
	}

	std::string urlValue = firstNonEmptyValue(json, {"url", "URL"});
	if (looksLikePlaybackHint(urlValue)) {
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
		const std::string nestedUrl = extractPlaybackHintRecursive(nestedJson, depth + 1);
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
	return lowered == "nextscene" || lowered == "prevscene" || lowered == "setscene" || lowered == "setcurrentscene" ||
	       lowered == "startstreaming" || lowered == "stopstreaming" || lowered == "startrecording" ||
	       lowered == "stoprecording" || lowered == "startvirtualcam" || lowered == "stopvirtualcam" ||
	       lowered == "mute" || lowered == "unmute";
}

bool isPeerCleanupRequest(const JsonParser &json)
{
	// Browser VDO.Ninja data-channel cleanup checks for top-level bye presence.
	if (json.hasKey("bye")) {
		return true;
	}
	if (!json.hasKey("request")) {
		return false;
	}
	const std::string request = asciiLower(trim(json.getString("request")));
	return request == "cleanup";
}

std::string normalizeRemoteAction(std::string action)
{
	if (action == "setCurrentScene") {
		return "setScene";
	}
	return action;
}

void skipWhitespace(const std::string &json, size_t &pos)
{
	while (pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos]))) {
		pos++;
	}
}

void skipJsonString(const std::string &json, size_t &pos)
{
	if (pos >= json.size() || json[pos] != '"') {
		return;
	}
	pos++;
	while (pos < json.size()) {
		if (json[pos] == '\\' && pos + 1 < json.size()) {
			pos += 2;
			continue;
		}
		if (json[pos] == '"') {
			pos++;
			return;
		}
		pos++;
	}
}

void skipJsonValue(const std::string &json, size_t &pos)
{
	if (pos >= json.size()) {
		return;
	}

	if (json[pos] == '"') {
		skipJsonString(json, pos);
		return;
	}

	if (json[pos] == '{' || json[pos] == '[') {
		const char open = json[pos];
		const char close = open == '{' ? '}' : ']';
		int depth = 1;
		pos++;
		while (pos < json.size() && depth > 0) {
			if (json[pos] == '"') {
				skipJsonString(json, pos);
				continue;
			}
			if (json[pos] == open) {
				depth++;
			} else if (json[pos] == close) {
				depth--;
			}
			pos++;
		}
		return;
	}

	while (pos < json.size() && json[pos] != ',' && json[pos] != '}' &&
	       !std::isspace(static_cast<unsigned char>(json[pos]))) {
		pos++;
	}
}

std::string extractTopLevelRawJsonValue(const std::string &json, const std::string &targetKey)
{
	size_t pos = 0;
	skipWhitespace(json, pos);
	if (pos >= json.size() || json[pos] != '{') {
		return "";
	}
	pos++;

	while (pos < json.size()) {
		skipWhitespace(json, pos);
		if (pos >= json.size() || json[pos] == '}') {
			break;
		}
		if (json[pos] == ',') {
			pos++;
			continue;
		}
		if (json[pos] != '"') {
			break;
		}

		pos++;
		std::string key;
		while (pos < json.size()) {
			if (json[pos] == '\\' && pos + 1 < json.size()) {
				pos++;
				key += json[pos++];
				continue;
			}
			if (json[pos] == '"') {
				pos++;
				break;
			}
			key += json[pos++];
		}

		skipWhitespace(json, pos);
		if (pos >= json.size() || json[pos] != ':') {
			break;
		}
		pos++;
		skipWhitespace(json, pos);

		const size_t valueStart = pos;
		skipJsonValue(json, pos);
		if (key == targetKey) {
			return trim(json.substr(valueStart, pos - valueStart));
		}
	}

	return "";
}

std::string appendTopLevelStringField(const std::string &rawMessage, const std::string &key, const std::string &value)
{
	const std::string message = trim(rawMessage);
	if (message.size() < 2 || message.front() != '{' || message.back() != '}') {
		return rawMessage;
	}

	JsonBuilder field;
	field.add(key, value);
	const std::string fieldJson = field.build();
	const std::string fieldEntry = fieldJson.substr(1, fieldJson.size() - 2);
	const std::string prefix = message.substr(0, message.size() - 1);
	const bool hasExistingFields = trim(prefix.substr(1)).size() > 0;

	return prefix + (hasExistingFields ? "," : "") + fieldEntry + "}";
}

bool hasOfficialInitialMuteState(const JsonParser &json)
{
	if (!json.hasKey("info")) {
		return false;
	}

	try {
		JsonParser info(json.getObject("info"));
		return info.hasKey("muted") || info.hasKey("video_muted_init");
	} catch (const std::exception &) {
		return false;
	}
}

bool hasOfficialInitialScreenShareState(const JsonParser &json)
{
	if (!json.hasKey("info")) {
		return false;
	}

	try {
		JsonParser info(json.getObject("info"));
		return info.hasKey("screenShareState");
	} catch (const std::exception &) {
		return false;
	}
}

bool hasOfficialInitialDirectorVideoState(const JsonParser &json)
{
	if (!json.hasKey("info")) {
		return false;
	}

	try {
		JsonParser info(json.getObject("info"));
		return info.hasKey("directorVideoMuted");
	} catch (const std::exception &) {
		return false;
	}
}

bool hasOfficialInitialDirectorAudioState(const JsonParser &json)
{
	if (!json.hasKey("info")) {
		return false;
	}

	try {
		JsonParser info(json.getObject("info"));
		return info.hasKey("directorSpeakerMuted") || info.hasKey("directorDisplayMuted");
	} catch (const std::exception &) {
		return false;
	}
}

bool hasOfficialInitialDirectorTransformState(const JsonParser &json)
{
	if (!json.hasKey("info")) {
		return false;
	}

	try {
		JsonParser info(json.getObject("info"));
		return info.hasKey("directorMirror") || info.hasKey("directorFlip") || info.hasKey("rotate_video");
	} catch (const std::exception &) {
		return false;
	}
}

void parseOfficialRotateCommand(const JsonParser &json, DirectorTransformStateUpdate &update)
{
	if (!json.hasKey("rotate")) {
		return;
	}

	update.hasRotateCommand = true;
	const std::string rotate = asciiLower(trim(json.getString("rotate")));
	if (rotate == "true" || rotate == "toggle") {
		update.rotateToggle = true;
		return;
	}
	if (rotate == "false" || rotate == "off" || rotate == "reset") {
		update.rotateReset = true;
		return;
	}

	try {
		update.hasRotateCommandDegrees = true;
		update.rotateCommandDegrees = std::stoi(rotate);
	} catch (const std::exception &) {
		update.hasRotateCommandDegrees = false;
		update.rotateCommandDegrees = 0;
	}
}

bool hasOfficialMediaControl(const JsonParser &json)
{
	return json.hasKey("bitrate") || json.hasKey("audioBitrate") || json.hasKey("targetBitrate") ||
	       json.hasKey("targetAudioBitrate") || json.hasKey("optimizedBitrate") || json.hasKey("requestResolution");
}

bool hasOfficialRecoveryControl(const JsonParser &json)
{
	return json.hasKey("refreshVideo") || json.hasKey("refreshMicrophone") || json.hasKey("refreshConnection") ||
	       json.hasKey("refreshAll") || json.hasKey("restartWhip");
}

bool hasOfficialMeshControl(const JsonParser &json)
{
	return json.hasKey("connectionMap");
}

bool hasOfficialStatsRequest(const JsonParser &json)
{
	return json.hasKey("requestStats") || json.hasKey("requestStatsContinuous");
}

bool hasOfficialStatsResponse(const JsonParser &json)
{
	return json.hasKey("remoteStats") || json.hasKey("stats");
}

bool hasOfficialKeyframeRequest(const JsonParser &json)
{
	return json.hasKey("requestKeyframe") || json.hasKey("keyframe");
}

bool allowsIndependentKeyframeFanout(DataMessageType type)
{
	switch (type) {
	case DataMessageType::Signaling:
	case DataMessageType::Ping:
	case DataMessageType::Pong:
	case DataMessageType::IceRestartRequest:
	case DataMessageType::Hangup:
	case DataMessageType::PeerBye:
		return false;
	default:
		return true;
	}
}

bool hasAcceptedRemoteControlShape(const JsonParser &json)
{
	return ((json.hasKey("obsCommand") || json.hasKey("action")) && json.hasKey("remote")) ||
	       (json.hasKey("remote") && (json.hasKey("scene") || json.hasKey("value")) &&
	        isLegacyRemoteActionValue(trim(json.getString("remote"))));
}

std::string officialUnsupportedControlName(const JsonParser &json)
{
	if ((json.hasKey("obsCommand") || json.hasKey("action")) && !json.hasKey("remote")) {
		return "obsCommand";
	}
	if (json.hasKey("rotate")) {
		return "rotate";
	}
	if (json.hasKey("mirrorGuestState") && json.hasKey("mirrorGuestTarget")) {
		return "mirrorGuestState";
	}

	for (const char *key : {"getAudioSettings",
	                        "getVideoSettings",
	                        "requestVideoHack",
	                        "changeCamera",
	                        "changeMicrophone",
	                        "changeSpeaker",
	                        "requestAudioHack",
	                        "requestChangeEQ",
	                        "requestChangeGating",
	                        "requestChangeCompressor",
	                        "requestChangeMicDelay",
	                        "requestChangeSubGain",
	                        "requestChangeLowcut",
	                        "requestChangeMicPanning",
	                        "requestVideoRecord",
	                        "changeOrder",
	                        "changeURL",
	                        "changeLabel",
	                        "remoteVideoMuted",
	                        "lowerhand",
	                        "displayMute",
	                        "speakerMute",
	                        "volume",
	                        "micIsolated",
	                        "micIsolate",
	                        "lowerVolume",
	                        "requestUpload",
	                        "stopClock",
	                        "resumeClock",
	                        "setClock",
	                        "hideClock",
	                        "showClock",
	                        "startClock",
	                        "pauseClock",
	                        "showTime",
	                        "group",
	                        "reload",
	                        "scale",
	                        "pan",
	                        "tilt",
	                        "zoom",
	                        "focus",
	                        "autofocus",
	                        "exposure",
	                        "keyframeRate",
	                        "reconnectPeer",
	                        "getConnectionMap"}) {
		if (json.hasKey(key)) {
			return key;
		}
	}
	return "";
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
		if (json.hasKey("description") || json.hasKey("candidate") || json.hasKey("candidates")) {
			msg.type = DataMessageType::Signaling;
			msg.data = rawMessage;
		} else if (json.hasKey("ping")) {
			msg.type = DataMessageType::Ping;
			msg.data = extractTopLevelRawJsonValue(rawMessage, "ping");
		} else if (json.hasKey("pong")) {
			msg.type = DataMessageType::Pong;
			msg.data = extractTopLevelRawJsonValue(rawMessage, "pong");
		} else if (isPeerCleanupRequest(json)) {
			msg.type = DataMessageType::PeerBye;
			msg.data = rawMessage;
		} else if (json.hasKey("iceRestartRequest")) {
			msg.type = DataMessageType::IceRestartRequest;
			msg.data = rawMessage;
		} else if (json.hasKey("hangup")) {
			msg.type = DataMessageType::Hangup;
			msg.data = rawMessage;
		} else if (hasOfficialStatsRequest(json)) {
			msg.type = DataMessageType::StatsRequest;
			msg.data = rawMessage;
			if (json.hasKey("requestStatsContinuous")) {
				msg.statsRequestMode = json.getBool("requestStatsContinuous") ? StatsRequestMode::ContinuousStart
				                                                              : StatsRequestMode::ContinuousStop;
			} else {
				msg.statsRequestMode = StatsRequestMode::Immediate;
			}
		} else if (hasAcceptedRemoteControlShape(json)) {
			msg.type = DataMessageType::RemoteControl;
			msg.data = rawMessage;
		} else if (json.hasKey("chat") || json.hasKey("chatMessage")) {
			msg.type = DataMessageType::Chat;
			msg.data = json.getString("chat", json.getString("chatMessage"));
		} else if (json.hasKey("tally") || json.hasKey("tallyOn") || json.hasKey("tallyOff") ||
		           json.hasKey("tallyPreview")) {
			msg.type = DataMessageType::Tally;
			msg.data = rawMessage;
		} else if (hasOfficialKeyframeRequest(json)) {
			msg.type = DataMessageType::RequestKeyframe;
		} else if (hasOfficialRecoveryControl(json)) {
			msg.type = DataMessageType::RecoveryControl;
			msg.data = rawMessage;
		} else if (const std::string unsupportedControl = officialUnsupportedControlName(json);
		           !unsupportedControl.empty()) {
			msg.type = DataMessageType::UnsupportedControl;
			msg.data = unsupportedControl;
		} else if (hasOfficialStatsResponse(json)) {
			msg.type = DataMessageType::Stats;
			msg.data = json.hasKey("remoteStats") ? json.getRaw("remoteStats") : json.getRaw("stats");
		} else if (json.hasKey("muted") || json.hasKey("muteState") || json.hasKey("audioMuted") ||
		           json.hasKey("videoMuted") || hasOfficialInitialMuteState(json)) {
			msg.type = DataMessageType::Mute;
			msg.data = rawMessage;
		} else if (json.hasKey("obsState") || json.hasKey("sceneDisplay") || json.hasKey("sceneMute")) {
			msg.type = DataMessageType::ObsState;
			msg.data = rawMessage;
		} else if (hasOfficialMediaControl(json)) {
			msg.type = DataMessageType::MediaControl;
			msg.data = rawMessage;
		} else if (json.hasKey("screenShareState") || json.hasKey("screenStopped") ||
		           hasOfficialInitialScreenShareState(json)) {
			msg.type = DataMessageType::ScreenShareState;
			msg.data = rawMessage;
		} else if (json.hasKey("directVideoMuted") || json.hasKey("virtualHangup") ||
		           hasOfficialInitialDirectorVideoState(json)) {
			msg.type = DataMessageType::DirectorVideoState;
			msg.data = rawMessage;
		} else if (hasOfficialInitialDirectorAudioState(json)) {
			msg.type = DataMessageType::DirectorAudioState;
			msg.data = rawMessage;
		} else if (json.hasKey("rotate_video") || json.hasKey("rotate") ||
		           (json.hasKey("mirrorGuestState") && json.hasKey("mirrorGuestTarget")) ||
		           hasOfficialInitialDirectorTransformState(json)) {
			msg.type = DataMessageType::DirectorTransformState;
			msg.data = rawMessage;
		} else if (hasOfficialMeshControl(json)) {
			msg.type = DataMessageType::MeshControl;
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

bool VDONinjaDataChannel::hasKeyframeRequest(const std::string &rawMessage) const
{
	try {
		JsonParser json(rawMessage);
		if (!hasOfficialKeyframeRequest(json)) {
			return false;
		}
		if (json.hasKey("description") || json.hasKey("candidate") || json.hasKey("candidates") ||
		    json.hasKey("ping") || json.hasKey("pong") || isPeerCleanupRequest(json) ||
		    json.hasKey("iceRestartRequest") || json.hasKey("hangup")) {
			return false;
		}
		return true;
	} catch (const std::exception &) {
		return false;
	}
}

std::string VDONinjaDataChannel::recoveryControlRejectionName(const std::string &rawMessage) const
{
	const RecoveryControlUpdate recovery = parseRecoveryControl(rawMessage);
	if (recovery.hasRefreshMicrophone && recovery.refreshMicrophone) {
		return "refreshMicrophone";
	}
	if (recovery.hasRefreshVideo && recovery.refreshVideo) {
		return "refreshVideo";
	}
	if (recovery.hasRefreshConnection && recovery.refreshConnection) {
		return "refreshConnection";
	}
	if (recovery.hasRefreshAll && recovery.refreshAll) {
		return "refreshAll";
	}
	if (recovery.hasRestartWhip && recovery.restartWhip) {
		return "restartWhip";
	}
	return "";
}

std::string VDONinjaDataChannel::unsupportedControlName(const std::string &rawMessage) const
{
	try {
		JsonParser json(rawMessage);
		return officialUnsupportedControlName(json);
	} catch (const std::exception &) {
		return "";
	}
}

MuteStateUpdate VDONinjaDataChannel::parseMuteState(const std::string &rawMessage) const
{
	MuteStateUpdate update;

	try {
		JsonParser json(rawMessage);
		update.hasAudioMuted = json.hasKey("audioMuted") || json.hasKey("muteState") || json.hasKey("muted");
		update.audioMuted = json.getBool("audioMuted", json.getBool("muteState", json.getBool("muted")));
		update.hasVideoMuted = json.hasKey("videoMuted");
		update.videoMuted = json.getBool("videoMuted");
		if (json.hasKey("info")) {
			JsonParser info(json.getObject("info"));
			if (!update.hasAudioMuted && info.hasKey("muted")) {
				update.hasAudioMuted = true;
				update.audioMuted = info.getBool("muted");
			}
			if (!update.hasVideoMuted && info.hasKey("video_muted_init")) {
				update.hasVideoMuted = true;
				update.videoMuted = info.getBool("video_muted_init");
			}
		}
	} catch (const std::exception &e) {
		logError("Failed to parse mute state: %s", e.what());
	}

	return update;
}

MediaControlUpdate VDONinjaDataChannel::parseMediaControl(const std::string &rawMessage) const
{
	MediaControlUpdate update;

	try {
		JsonParser json(rawMessage);
		if (json.hasKey("bitrate")) {
			update.hasVideoBitrate = true;
			update.videoBitrateKbps = json.getInt("bitrate");
		}
		if (json.hasKey("audioBitrate")) {
			update.hasAudioBitrate = true;
			update.audioBitrateKbps = json.getInt("audioBitrate");
		}
		if (json.hasKey("targetBitrate")) {
			update.hasTargetVideoBitrate = true;
			update.targetVideoBitrateKbps = json.getInt("targetBitrate");
		}
		if (json.hasKey("targetAudioBitrate")) {
			update.hasTargetAudioBitrate = true;
			update.targetAudioBitrateKbps = json.getInt("targetAudioBitrate");
		}
		if (json.hasKey("optimizedBitrate")) {
			update.hasOptimizedBitrate = true;
			update.optimizedBitrateKbps = json.getInt("optimizedBitrate");
		}
		if (json.hasKey("requestResolution")) {
			update.hasRequestResolution = true;
			JsonParser resolution(json.getObject("requestResolution"));
			if (resolution.hasKey("w")) {
				update.hasRequestWidth = true;
				update.requestWidth = resolution.getInt("w");
			}
			if (resolution.hasKey("h")) {
				update.hasRequestHeight = true;
				update.requestHeight = resolution.getInt("h");
			}
			if (resolution.hasKey("s")) {
				update.hasRequestScale = true;
				update.requestScale = resolution.getInt("s");
			}
			if (resolution.hasKey("c")) {
				update.hasRequestCover = true;
				update.requestCover = resolution.getBool("c");
			}
		}
	} catch (const std::exception &e) {
		logError("Failed to parse media-control state: %s", e.what());
	}

	return update;
}

ScreenShareStateUpdate VDONinjaDataChannel::parseScreenShareState(const std::string &rawMessage) const
{
	ScreenShareStateUpdate update;

	try {
		JsonParser json(rawMessage);
		if (json.hasKey("screenShareState")) {
			update.hasScreenShareState = true;
			update.screenShareState = json.getBool("screenShareState");
		}
		if (json.hasKey("screenStopped")) {
			update.hasScreenStopped = true;
			update.screenStopped = json.getBool("screenStopped");
		}
		if (json.hasKey("info")) {
			JsonParser info(json.getObject("info"));
			if (!update.hasScreenShareState && info.hasKey("screenShareState")) {
				update.hasScreenShareState = true;
				update.screenShareState = info.getBool("screenShareState");
			}
		}
	} catch (const std::exception &e) {
		logError("Failed to parse screen-share state: %s", e.what());
	}

	return update;
}

DirectorVideoStateUpdate VDONinjaDataChannel::parseDirectorVideoState(const std::string &rawMessage) const
{
	DirectorVideoStateUpdate update;

	try {
		JsonParser json(rawMessage);
		if (json.hasKey("directVideoMuted")) {
			update.hasDirectVideoMuted = true;
			update.directVideoMuted = json.getBool("directVideoMuted");
		}
		if (json.hasKey("virtualHangup")) {
			update.hasVirtualHangup = true;
			update.virtualHangup = json.getBool("virtualHangup");
		}
		if (json.hasKey("remoteVideoMuted")) {
			update.hasRemoteVideoMuted = true;
			update.remoteVideoMuted = json.getBool("remoteVideoMuted");
		}
		if (json.hasKey("target")) {
			update.hasTarget = true;
			const std::string rawTarget = extractTopLevelRawJsonValue(rawMessage, "target");
			if (rawTarget == "true") {
				update.targetSelf = true;
			} else if (rawTarget != "false" && rawTarget != "null") {
				update.target = json.getString("target");
			}
		}
		if (json.hasKey("info")) {
			JsonParser info(json.getObject("info"));
			if (!update.hasDirectVideoMuted && info.hasKey("directorVideoMuted")) {
				update.hasDirectVideoMuted = true;
				update.directVideoMuted = info.getBool("directorVideoMuted");
			}
		}
	} catch (const std::exception &e) {
		logError("Failed to parse director video state: %s", e.what());
	}

	return update;
}

ReceiverVideoSuppressionUpdate VDONinjaDataChannel::parseReceiverVideoSuppression(const std::string &rawMessage) const
{
	ReceiverVideoSuppressionUpdate update;

	const MuteStateUpdate mute = parseMuteState(rawMessage);
	if (mute.hasVideoMuted) {
		update.hasMediaVideoMuted = true;
		update.mediaVideoMuted = mute.videoMuted;
	}

	const DirectorVideoStateUpdate directorVideo = parseDirectorVideoState(rawMessage);
	if (directorVideo.hasDirectVideoMuted) {
		update.hasDirectorVideoMuted = true;
		update.directorVideoMuted = directorVideo.directVideoMuted;
		if (directorVideo.hasTarget) {
			update.hasDirectorVideoTarget = true;
			update.directorVideoTargetSelf = directorVideo.targetSelf;
			update.directorVideoTarget = directorVideo.target;
		}
	}
	if (directorVideo.hasVirtualHangup) {
		update.hasVirtualHangup = true;
		update.virtualHangup = directorVideo.virtualHangup;
	}

	return update;
}

bool VDONinjaDataChannel::receiverDirectorVideoAppliesToPeer(const ReceiverVideoSuppressionUpdate &update,
                                                             const std::string &peerUuid) const
{
	if (!update.hasDirectorVideoMuted) {
		return false;
	}
	if (!update.hasDirectorVideoTarget) {
		return true;
	}
	if (update.directorVideoTargetSelf) {
		return true;
	}
	return !update.directorVideoTarget.empty() && update.directorVideoTarget == peerUuid;
}

DirectorAudioStateUpdate VDONinjaDataChannel::parseDirectorAudioState(const std::string &rawMessage) const
{
	DirectorAudioStateUpdate update;

	try {
		JsonParser json(rawMessage);
		if (json.hasKey("speakerMute")) {
			update.hasSpeakerMuted = true;
			update.speakerMuted = json.getBool("speakerMute");
		}
		if (json.hasKey("displayMute")) {
			update.hasDisplayMuted = true;
			update.displayMuted = json.getBool("displayMute");
		}
		if (json.hasKey("info")) {
			JsonParser info(json.getObject("info"));
			if (!update.hasSpeakerMuted && info.hasKey("directorSpeakerMuted")) {
				update.hasSpeakerMuted = true;
				update.speakerMuted = info.getBool("directorSpeakerMuted");
			}
			if (!update.hasDisplayMuted && info.hasKey("directorDisplayMuted")) {
				update.hasDisplayMuted = true;
				update.displayMuted = info.getBool("directorDisplayMuted");
			}
		}
	} catch (const std::exception &e) {
		logError("Failed to parse director audio/display state: %s", e.what());
	}

	return update;
}

DirectorTransformStateUpdate VDONinjaDataChannel::parseDirectorTransformState(const std::string &rawMessage) const
{
	DirectorTransformStateUpdate update;

	try {
		JsonParser json(rawMessage);
		if (json.hasKey("mirrorGuestState")) {
			update.hasMirror = true;
			update.mirror = json.getBool("mirrorGuestState");
		}
		if (json.hasKey("mirrorGuestTarget")) {
			update.hasTarget = true;
			const std::string rawTarget = extractTopLevelRawJsonValue(rawMessage, "mirrorGuestTarget");
			if (rawTarget == "true") {
				update.targetSelf = true;
			} else if (rawTarget != "false" && rawTarget != "null") {
				update.target = json.getString("mirrorGuestTarget");
			}
		}
		if (json.hasKey("rotate_video")) {
			update.hasRotation = true;
			update.rotationDegrees = json.getInt("rotate_video");
		}
		parseOfficialRotateCommand(json, update);
		if (json.hasKey("info")) {
			JsonParser info(json.getObject("info"));
			if (!update.hasMirror && info.hasKey("directorMirror")) {
				update.hasMirror = true;
				update.mirror = info.getBool("directorMirror");
			}
			if (info.hasKey("directorFlip")) {
				update.hasFlip = true;
				update.flip = info.getBool("directorFlip");
			}
			if (!update.hasRotation && info.hasKey("rotate_video")) {
				update.hasRotation = true;
				update.rotationDegrees = info.getInt("rotate_video");
			}
		}
	} catch (const std::exception &e) {
		logError("Failed to parse director mirror/flip state: %s", e.what());
	}

	return update;
}

RecoveryControlUpdate VDONinjaDataChannel::parseRecoveryControl(const std::string &rawMessage) const
{
	RecoveryControlUpdate update;

	try {
		JsonParser json(rawMessage);
		if (json.hasKey("refreshVideo")) {
			update.hasRefreshVideo = true;
			update.refreshVideo = true;
		}
		if (json.hasKey("refreshMicrophone")) {
			update.hasRefreshMicrophone = true;
			update.refreshMicrophone = true;
		}
		if (json.hasKey("refreshConnection")) {
			update.hasRefreshConnection = true;
			update.refreshConnection = true;
		}
		if (json.hasKey("refreshAll")) {
			update.hasRefreshAll = true;
			update.refreshAll = true;
		}
		if (json.hasKey("restartWhip")) {
			update.hasRestartWhip = true;
			update.restartWhip = true;
		}
	} catch (const std::exception &e) {
		logError("Failed to parse recovery-control state: %s", e.what());
	}

	return update;
}

MeshControlUpdate VDONinjaDataChannel::parseMeshControl(const std::string &rawMessage) const
{
	MeshControlUpdate update;

	try {
		JsonParser json(rawMessage);
		if (json.hasKey("reconnectPeer")) {
			update.hasReconnectPeer = true;
			update.reconnectPeer = json.getString("reconnectPeer");
		}
		if (json.hasKey("getConnectionMap")) {
			update.hasGetConnectionMap = true;
			update.getConnectionMap = json.getBool("getConnectionMap");
		}
		if (json.hasKey("connectionMap")) {
			update.hasConnectionMap = true;
			update.connectionMapJson = json.getRaw("connectionMap");
		}
	} catch (const std::exception &e) {
		logError("Failed to parse mesh-control state: %s", e.what());
	}

	return update;
}

std::string VDONinjaDataChannel::prepareSignalingMessage(const std::string &rawMessage,
                                                         const std::string &senderId) const
{
	if (senderId.empty()) {
		return rawMessage;
	}

	try {
		JsonParser json(rawMessage);
		if (!json.hasKey("description") && !json.hasKey("candidate") && !json.hasKey("candidates")) {
			return rawMessage;
		}
		if (json.hasKey("UUID") || json.hasKey("uuid") || json.hasKey("from")) {
			return rawMessage;
		}
		return appendTopLevelStringField(rawMessage, "UUID", senderId);
	} catch (const std::exception &) {
		return rawMessage;
	}
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
	builder.add("keyframe", true);
	return builder.build();
}

std::string VDONinjaDataChannel::createPongMessage(const std::string &rawTokenJson)
{
	JsonBuilder builder;
	builder.addRaw("pong", rawTokenJson.empty() ? "null" : rawTokenJson);
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
		const bool keyframeRequested = hasOfficialKeyframeRequest(json) && allowsIndependentKeyframeFanout(msg.type);
		if (keyframeRequested) {
			OnKeyframeRequestCallback callback;
			{
				std::lock_guard<std::mutex> lock(mutex_);
				callback = onKeyframeRequest_;
			}
			if (callback) {
				callback(senderId);
			}
		}

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
		case DataMessageType::ObsState:
			break;
		case DataMessageType::MediaControl:
			break;
		case DataMessageType::ScreenShareState:
			break;
		case DataMessageType::DirectorVideoState:
			break;
		case DataMessageType::DirectorAudioState:
			break;
		case DataMessageType::DirectorTransformState:
			break;
		case DataMessageType::RequestKeyframe:
			break;
		case DataMessageType::StatsRequest:
			break;
		case DataMessageType::Signaling:
			break;
		case DataMessageType::Ping:
			break;
		case DataMessageType::Pong:
			break;
		case DataMessageType::IceRestartRequest:
			break;
		case DataMessageType::RecoveryControl:
			break;
		case DataMessageType::MeshControl:
			break;
		case DataMessageType::UnsupportedControl:
			break;
		case DataMessageType::Hangup:
			break;
		case DataMessageType::PeerBye:
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

std::string VDONinjaDataChannel::extractInboundPlaybackHint(const std::string &rawMessage) const
{
	if (rawMessage.empty()) {
		return "";
	}

	try {
		JsonParser json(rawMessage);
		return extractPlaybackHintRecursive(json, 0);
	} catch (const std::exception &) {
		return "";
	}
}

void VDONinjaDataChannel::parseChatMessage(const std::string &senderId, const JsonParser &json)
{
	std::string message = json.getString("chat", json.getString("chatMessage"));

	logDebug("Chat from %s: %s", senderId.c_str(), message.c_str());

	OnChatMessageCallback callback;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		callback = onChatMessage_;
	}
	if (callback) {
		callback(senderId, message);
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

	OnTallyChangeCallback callback;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		peerTallies_[senderId] = state;
		callback = onTallyChange_;
	}

	logDebug("Tally from %s: program=%d, preview=%d", senderId.c_str(), state.program, state.preview);

	if (callback) {
		callback(senderId, state);
	}
}

void VDONinjaDataChannel::parseMuteMessage(const std::string &senderId, const JsonParser &json)
{
	MuteStateUpdate update;
	update.hasAudioMuted = json.hasKey("audioMuted") || json.hasKey("muteState") || json.hasKey("muted");
	update.audioMuted = json.getBool("audioMuted", json.getBool("muteState", json.getBool("muted")));
	update.hasVideoMuted = json.hasKey("videoMuted");
	update.videoMuted = json.getBool("videoMuted");
	if (json.hasKey("info")) {
		JsonParser info(json.getObject("info"));
		if (!update.hasAudioMuted && info.hasKey("muted")) {
			update.hasAudioMuted = true;
			update.audioMuted = info.getBool("muted");
		}
		if (!update.hasVideoMuted && info.hasKey("video_muted_init")) {
			update.hasVideoMuted = true;
			update.videoMuted = info.getBool("video_muted_init");
		}
	}

	logDebug("Mute from %s: audio=%d, video=%d", senderId.c_str(), update.audioMuted, update.videoMuted);

	OnMuteChangeCallback callback;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		callback = onMuteChange_;
	}
	if (callback) {
		callback(senderId, update.audioMuted, update.videoMuted);
	}
}

void VDONinjaDataChannel::parseCustomMessage(const std::string &senderId, const JsonParser &json)
{
	std::string data = json.getString("data");

	OnCustomDataCallback callback;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		callback = onCustomData_;
	}
	if (callback) {
		callback(senderId, data);
	}
}

void VDONinjaDataChannel::setOnChatMessage(OnChatMessageCallback callback)
{
	std::lock_guard<std::mutex> lock(mutex_);
	onChatMessage_ = callback;
}
void VDONinjaDataChannel::setOnTallyChange(OnTallyChangeCallback callback)
{
	std::lock_guard<std::mutex> lock(mutex_);
	onTallyChange_ = callback;
}
void VDONinjaDataChannel::setOnMuteChange(OnMuteChangeCallback callback)
{
	std::lock_guard<std::mutex> lock(mutex_);
	onMuteChange_ = callback;
}
void VDONinjaDataChannel::setOnCustomData(OnCustomDataCallback callback)
{
	std::lock_guard<std::mutex> lock(mutex_);
	onCustomData_ = callback;
}
void VDONinjaDataChannel::setOnKeyframeRequest(OnKeyframeRequestCallback callback)
{
	std::lock_guard<std::mutex> lock(mutex_);
	onKeyframeRequest_ = callback;
}
void VDONinjaDataChannel::setOnRemoteControl(OnRemoteControlCallback callback)
{
	std::lock_guard<std::mutex> lock(mutex_);
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

	OnRemoteControlCallback callback;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		callback = onRemoteControl_;
	}
	if (callback) {
		callback(action, value);
	}
}

} // namespace vdoninja
