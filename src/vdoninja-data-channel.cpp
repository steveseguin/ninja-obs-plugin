/*
 * OBS VDO.Ninja Plugin
 * Data channel implementation
 */

#include "vdoninja-data-channel.h"

namespace vdoninja {

VDONinjaDataChannel::VDONinjaDataChannel()
{
    logDebug("Data channel handler created");
}

VDONinjaDataChannel::~VDONinjaDataChannel()
{
}

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
        } else if (json.hasKey("tally") || json.hasKey("tallyOn") || json.hasKey("tallyOff")) {
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
    builder.add("timestamp", static_cast<int>(currentTimeMs()));
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
    builder.add("timestamp", static_cast<int>(currentTimeMs()));
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

    logDebug("Tally from %s: program=%d, preview=%d",
             senderId.c_str(), state.program, state.preview);

    if (onTallyChange_) {
        onTallyChange_(senderId, state);
    }
}

void VDONinjaDataChannel::parseMuteMessage(const std::string &senderId, const JsonParser &json)
{
    bool audioMuted = json.getBool("audioMuted", json.getBool("muted"));
    bool videoMuted = json.getBool("videoMuted");

    logDebug("Mute from %s: audio=%d, video=%d",
             senderId.c_str(), audioMuted, videoMuted);

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

void VDONinjaDataChannel::setOnChatMessage(OnChatMessageCallback callback) { onChatMessage_ = callback; }
void VDONinjaDataChannel::setOnTallyChange(OnTallyChangeCallback callback) { onTallyChange_ = callback; }
void VDONinjaDataChannel::setOnMuteChange(OnMuteChangeCallback callback) { onMuteChange_ = callback; }
void VDONinjaDataChannel::setOnCustomData(OnCustomDataCallback callback) { onCustomData_ = callback; }
void VDONinjaDataChannel::setOnKeyframeRequest(OnKeyframeRequestCallback callback) { onKeyframeRequest_ = callback; }

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

} // namespace vdoninja
