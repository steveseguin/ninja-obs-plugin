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
	Chat,                   // Chat message
	Tally,                  // Tally light state
	RequestKeyframe,        // Request keyframe from publisher
	Mute,                   // Mute state change
	ObsState,               // OBS/source scene state from official VDO.Ninja clients
	MediaControl,           // Bitrate/resolution control hints from official clients
	ScreenShareState,       // Screen-share state hints from official clients
	DirectorVideoState,     // Director-controlled video hide/virtual hangup state
	DirectorAudioState,     // Director-controlled speaker/display mute state
	DirectorTransformState, // Director-controlled mirror/flip/rotation state
	Stats,                  // Connection statistics
	StatsRequest,           // Request connection statistics
	Signaling,              // SDP/ICE sent directly over data channel
	Ping,                   // Liveness ping
	Pong,                   // Liveness response
	IceRestartRequest,      // Request peer ICE restart/recovery
	RecoveryControl,        // Official remote/director stream recovery controls
	MeshControl,            // Official mesh reconnect/map controls
	UnsupportedControl,     // Official control-center command not implemented by this native endpoint
	Hangup,                 // Remote/director hangup request
	PeerBye,                // Peer cleanup/close request
	RemoteControl,          // Remote control command (scene, streaming, recording)
	Custom                  // Custom application data
};

enum class StatsRequestMode { None, Immediate, ContinuousStart, ContinuousStop };

// Tally state
struct TallyState {
	bool program = false; // On-air (red)
	bool preview = false; // Preview (green)
};

// Mute state update preserving which official fields were actually present.
struct MuteStateUpdate {
	bool hasAudioMuted = false;
	bool audioMuted = false;
	bool hasVideoMuted = false;
	bool videoMuted = false;
};

// Media-control hints preserving which official fields were present.
struct MediaControlUpdate {
	bool hasVideoBitrate = false;
	int videoBitrateKbps = 0;
	bool hasAudioBitrate = false;
	int audioBitrateKbps = 0;
	bool hasTargetVideoBitrate = false;
	int targetVideoBitrateKbps = 0;
	bool hasTargetAudioBitrate = false;
	int targetAudioBitrateKbps = 0;
	bool hasOptimizedBitrate = false;
	int optimizedBitrateKbps = 0;
	bool hasRequestResolution = false;
	bool hasRequestWidth = false;
	int requestWidth = 0;
	bool hasRequestHeight = false;
	int requestHeight = 0;
	bool hasRequestScale = false;
	int requestScale = 0;
	bool hasRequestCover = false;
	bool requestCover = false;
};

// Screen-share state preserving which official fields were present.
struct ScreenShareStateUpdate {
	bool hasScreenShareState = false;
	bool screenShareState = false;
	bool hasScreenStopped = false;
	bool screenStopped = false;
};

// Director-controlled video state preserving which official fields were present.
struct DirectorVideoStateUpdate {
	bool hasDirectVideoMuted = false;
	bool directVideoMuted = false;
	bool hasVirtualHangup = false;
	bool virtualHangup = false;
	bool hasRemoteVideoMuted = false;
	bool remoteVideoMuted = false;
	bool hasTarget = false;
	bool targetSelf = false;
	std::string target;
};

// Receiver-side video suppression fields that official VDO.Ninja tracks
// independently before deciding whether remote video should render.
struct ReceiverVideoSuppressionUpdate {
	bool hasMediaVideoMuted = false;
	bool mediaVideoMuted = false;
	bool hasDirectorVideoMuted = false;
	bool directorVideoMuted = false;
	bool hasDirectorVideoTarget = false;
	bool directorVideoTargetSelf = false;
	std::string directorVideoTarget;
	bool hasVirtualHangup = false;
	bool virtualHangup = false;
};

// Director-controlled audio/display state preserving which official fields were present.
struct DirectorAudioStateUpdate {
	bool hasSpeakerMuted = false;
	bool speakerMuted = false;
	bool hasDisplayMuted = false;
	bool displayMuted = false;
};

// Director-controlled mirror/flip/rotation state preserving which official fields were present.
struct DirectorTransformStateUpdate {
	bool hasMirror = false;
	bool mirror = false;
	bool hasFlip = false;
	bool flip = false;
	bool hasRotation = false;
	int rotationDegrees = 0;
	bool hasRotateCommand = false;
	bool rotateToggle = false;
	bool rotateReset = false;
	bool hasRotateCommandDegrees = false;
	int rotateCommandDegrees = 0;
	bool hasTarget = false;
	bool targetSelf = false;
	std::string target;
};

// Official stream recovery controls preserving which fields were present.
struct RecoveryControlUpdate {
	bool hasRefreshVideo = false;
	bool refreshVideo = false;
	bool hasRefreshMicrophone = false;
	bool refreshMicrophone = false;
	bool hasRefreshConnection = false;
	bool refreshConnection = false;
	bool hasRefreshAll = false;
	bool refreshAll = false;
	bool hasRestartWhip = false;
	bool restartWhip = false;
};

// Official mesh reconnect/map controls preserving which fields were present.
struct MeshControlUpdate {
	bool hasReconnectPeer = false;
	std::string reconnectPeer;
	bool hasGetConnectionMap = false;
	bool getConnectionMap = false;
	bool hasConnectionMap = false;
	std::string connectionMapJson;
};

// Data message structure
struct DataMessage {
	DataMessageType type = DataMessageType::Unknown;
	std::string senderId;
	std::string data;
	StatsRequestMode statsRequestMode = StatsRequestMode::None;
	int64_t timestamp = 0;
};

// Callbacks
using OnChatMessageCallback = std::function<void(const std::string &senderId, const std::string &message)>;
using OnTallyChangeCallback = std::function<void(const std::string &streamId, const TallyState &state)>;
using OnMuteChangeCallback = std::function<void(const std::string &senderId, bool audioMuted, bool videoMuted)>;
using OnCustomDataCallback = std::function<void(const std::string &senderId, const std::string &data)>;
using OnKeyframeRequestCallback = std::function<void(const std::string &senderId)>;
using OnRemoteControlCallback = std::function<void(const std::string &action, const std::string &value)>;

class VDONinjaDataChannel
{
public:
	VDONinjaDataChannel();
	~VDONinjaDataChannel();

	// Parse incoming data channel message
	DataMessage parseMessage(const std::string &rawMessage);
	MuteStateUpdate parseMuteState(const std::string &rawMessage) const;
	MediaControlUpdate parseMediaControl(const std::string &rawMessage) const;
	ScreenShareStateUpdate parseScreenShareState(const std::string &rawMessage) const;
	DirectorVideoStateUpdate parseDirectorVideoState(const std::string &rawMessage) const;
	ReceiverVideoSuppressionUpdate parseReceiverVideoSuppression(const std::string &rawMessage) const;
	bool receiverDirectorVideoAppliesToPeer(const ReceiverVideoSuppressionUpdate &update,
	                                        const std::string &peerUuid) const;
	DirectorAudioStateUpdate parseDirectorAudioState(const std::string &rawMessage) const;
	DirectorTransformStateUpdate parseDirectorTransformState(const std::string &rawMessage) const;
	RecoveryControlUpdate parseRecoveryControl(const std::string &rawMessage) const;
	MeshControlUpdate parseMeshControl(const std::string &rawMessage) const;
	std::string prepareSignalingMessage(const std::string &rawMessage, const std::string &senderId) const;
	bool hasKeyframeRequest(const std::string &rawMessage) const;
	std::string recoveryControlRejectionName(const std::string &rawMessage) const;
	std::string unsupportedControlName(const std::string &rawMessage) const;

	// Create outgoing messages
	std::string createChatMessage(const std::string &message);
	std::string createTallyMessage(const TallyState &state);
	std::string createMuteMessage(bool audioMuted, bool videoMuted);
	std::string createKeyframeRequest();
	std::string createPongMessage(const std::string &rawTokenJson);
	std::string createCustomMessage(const std::string &type, const std::string &data);

	// Handle incoming message (dispatches to appropriate callback)
	void handleMessage(const std::string &senderId, const std::string &rawMessage);

	// Extract an inbound playback hint from known VDO.Ninja data-channel payload
	// formats. Returns empty string if no usable hint was found.
	std::string extractInboundPlaybackHint(const std::string &rawMessage) const;

	// Set callbacks
	void setOnChatMessage(OnChatMessageCallback callback);
	void setOnTallyChange(OnTallyChangeCallback callback);
	void setOnMuteChange(OnMuteChangeCallback callback);
	void setOnCustomData(OnCustomDataCallback callback);
	void setOnKeyframeRequest(OnKeyframeRequestCallback callback);
	void setOnRemoteControl(OnRemoteControlCallback callback);

	// Tally light management
	void setLocalTally(const TallyState &state);
	TallyState getLocalTally() const;
	TallyState getPeerTally(const std::string &peerId) const;
	std::map<std::string, TallyState> getAllPeerTallies() const;

private:
	// Parse specific message types
	void parseChatMessage(const std::string &senderId, const JsonParser &json);
	void parseTallyMessage(const std::string &senderId, const JsonParser &json);
	void parseMuteMessage(const std::string &senderId, const JsonParser &json);
	void parseCustomMessage(const std::string &senderId, const JsonParser &json);
	void parseRemoteControlMessage(const std::string &senderId, const JsonParser &json);

	// Callbacks
	OnChatMessageCallback onChatMessage_;
	OnTallyChangeCallback onTallyChange_;
	OnMuteChangeCallback onMuteChange_;
	OnCustomDataCallback onCustomData_;
	OnKeyframeRequestCallback onKeyframeRequest_;
	OnRemoteControlCallback onRemoteControl_;

	// State and callbacks protected by mutex_.
	TallyState localTally_;
	std::map<std::string, TallyState> peerTallies_;
	mutable std::mutex mutex_;
};

} // namespace vdoninja
