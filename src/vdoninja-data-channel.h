/*
 * OBS VDO.Ninja Plugin
 * Data channel support for bidirectional messaging
 *
 * Provides functionality for:
 * - Tally light support
 * - Chat messages
 * - Remote control commands
 * - Custom data exchange
 */

#pragma once

#include <functional>
#include <map>
#include <mutex>

#include "vdoninja-common.h"
#include "vdoninja-utils.h"

namespace vdoninja
{

// Data channel message types (matching VDO.Ninja protocol)
enum class DataMessageType {
	Unknown,
	Chat,            // Chat message
	Tally,           // Tally light state
	RequestKeyframe, // Request keyframe from publisher
	Mute,            // Mute state change
	Stats,           // Connection statistics
	Custom           // Custom application data
};

// Tally state
struct TallyState {
	bool program = false; // On-air (red)
	bool preview = false; // Preview (green)
};

// Data message structure
struct DataMessage {
	DataMessageType type = DataMessageType::Unknown;
	std::string senderId;
	std::string data;
	int64_t timestamp = 0;
};

// Callbacks
using OnChatMessageCallback = std::function<void(const std::string &senderId, const std::string &message)>;
using OnTallyChangeCallback = std::function<void(const std::string &streamId, const TallyState &state)>;
using OnMuteChangeCallback = std::function<void(const std::string &senderId, bool audioMuted, bool videoMuted)>;
using OnCustomDataCallback = std::function<void(const std::string &senderId, const std::string &data)>;
using OnKeyframeRequestCallback = std::function<void(const std::string &senderId)>;

class VDONinjaDataChannel
{
public:
	VDONinjaDataChannel();
	~VDONinjaDataChannel();

	// Parse incoming data channel message
	DataMessage parseMessage(const std::string &rawMessage);

	// Create outgoing messages
	std::string createChatMessage(const std::string &message);
	std::string createTallyMessage(const TallyState &state);
	std::string createMuteMessage(bool audioMuted, bool videoMuted);
	std::string createKeyframeRequest();
	std::string createCustomMessage(const std::string &type, const std::string &data);

	// Handle incoming message (dispatches to appropriate callback)
	void handleMessage(const std::string &senderId, const std::string &rawMessage);

	// Set callbacks
	void setOnChatMessage(OnChatMessageCallback callback);
	void setOnTallyChange(OnTallyChangeCallback callback);
	void setOnMuteChange(OnMuteChangeCallback callback);
	void setOnCustomData(OnCustomDataCallback callback);
	void setOnKeyframeRequest(OnKeyframeRequestCallback callback);

	// Tally light management
	void setLocalTally(const TallyState &state);
	TallyState getLocalTally() const;
	TallyState getPeerTally(const std::string &peerId) const;

private:
	// Parse specific message types
	void parseChatMessage(const std::string &senderId, const JsonParser &json);
	void parseTallyMessage(const std::string &senderId, const JsonParser &json);
	void parseMuteMessage(const std::string &senderId, const JsonParser &json);
	void parseCustomMessage(const std::string &senderId, const JsonParser &json);

	// Callbacks
	OnChatMessageCallback onChatMessage_;
	OnTallyChangeCallback onTallyChange_;
	OnMuteChangeCallback onMuteChange_;
	OnCustomDataCallback onCustomData_;
	OnKeyframeRequestCallback onKeyframeRequest_;

	// State
	TallyState localTally_;
	std::map<std::string, TallyState> peerTallies_;
	mutable std::mutex mutex_;
};

} // namespace vdoninja
