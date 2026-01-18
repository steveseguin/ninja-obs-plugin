/*
 * OBS VDO.Ninja Plugin
 * Copyright (C) 2024 VDO.Ninja Community
 *
 * Common definitions and types shared across the plugin
 */

#pragma once

#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <map>
#include <mutex>
#include <atomic>
#include <optional>

// Forward declarations for libdatachannel
namespace rtc {
class PeerConnection;
class DataChannel;
class Track;
struct Configuration;
} // namespace rtc

namespace vdoninja {

// VDO.Ninja default configuration
constexpr const char *DEFAULT_WSS_HOST = "wss://wss.vdo.ninja";
constexpr const char *DEFAULT_SALT = "vdo.ninja";
constexpr const char *DEFAULT_PASSWORD = "someEncryptionKey123";
constexpr int DEFAULT_RECONNECT_ATTEMPTS = 5;
constexpr int ICE_CANDIDATE_BUNDLE_DELAY_MS = 70;

// Default STUN servers
const std::vector<std::string> DEFAULT_STUN_SERVERS = {
    "stun:stun.l.google.com:19302",
    "stun:stun.cloudflare.com:3478"
};

// Connection types
enum class ConnectionType {
    Publisher,  // We are sending media to a viewer
    Viewer      // We are receiving media from a publisher
};

// Connection state
enum class ConnectionState {
    New,
    Connecting,
    Connected,
    Disconnected,
    Failed,
    Closed
};

// Stream quality settings
struct StreamQuality {
    int width = 1920;
    int height = 1080;
    int fps = 30;
    int bitrate = 4000000;  // 4 Mbps default
};

// ICE server configuration
struct IceServer {
    std::string urls;
    std::string username;
    std::string credential;
};

// Peer connection info
struct PeerInfo {
    std::string uuid;
    std::string streamId;
    std::string session;
    ConnectionType type;
    ConnectionState state = ConnectionState::New;
    bool hasDataChannel = false;
    std::shared_ptr<rtc::PeerConnection> pc;
    std::shared_ptr<rtc::DataChannel> dataChannel;
};

// Room information
struct RoomInfo {
    std::string roomId;
    std::string hashedRoomId;
    std::string password;
    bool isJoined = false;
    std::vector<std::string> members;
};

// Stream information
struct StreamInfo {
    std::string streamId;
    std::string hashedStreamId;
    bool isPublishing = false;
    bool isViewing = false;
};

// Callbacks for signaling events
using OnConnectedCallback = std::function<void()>;
using OnDisconnectedCallback = std::function<void()>;
using OnErrorCallback = std::function<void(const std::string &error)>;
using OnOfferCallback = std::function<void(const std::string &uuid, const std::string &sdp, const std::string &session)>;
using OnAnswerCallback = std::function<void(const std::string &uuid, const std::string &sdp, const std::string &session)>;
using OnIceCandidateCallback = std::function<void(const std::string &uuid, const std::string &candidate, const std::string &mid, const std::string &session)>;
using OnRoomJoinedCallback = std::function<void(const std::vector<std::string> &members)>;
using OnStreamAddedCallback = std::function<void(const std::string &streamId, const std::string &uuid)>;
using OnStreamRemovedCallback = std::function<void(const std::string &streamId, const std::string &uuid)>;
using OnDataCallback = std::function<void(const std::string &uuid, const std::string &data)>;

// Video codec preferences
enum class VideoCodec {
    H264,
    VP8,
    VP9,
    AV1
};

// Audio codec preferences
enum class AudioCodec {
    Opus,
    PCMU,
    PCMA
};

// Plugin output settings
struct OutputSettings {
    std::string streamId;
    std::string roomId;
    std::string password;
    std::string wssHost = DEFAULT_WSS_HOST;
    VideoCodec videoCodec = VideoCodec::H264;
    AudioCodec audioCodec = AudioCodec::Opus;
    StreamQuality quality;
    bool enableDataChannel = true;
    bool autoReconnect = true;
    int maxViewers = 10;  // Max simultaneous P2P connections
    std::vector<IceServer> customIceServers;
    bool forceTurn = false;
};

// Plugin source settings
struct SourceSettings {
    std::string streamId;
    std::string roomId;
    std::string password;
    std::string wssHost = DEFAULT_WSS_HOST;
    bool enableDataChannel = true;
    bool autoReconnect = true;
    std::vector<IceServer> customIceServers;
    bool forceTurn = false;
};

// Utility function declarations (implemented in vdoninja-utils.cpp)
std::string generateUUID();
std::string generateSessionId();
std::string hashStreamId(const std::string &streamId, const std::string &password, const std::string &salt = DEFAULT_SALT);
std::string hashRoomId(const std::string &roomId, const std::string &password, const std::string &salt = DEFAULT_SALT);
std::string sanitizeStreamId(const std::string &streamId);
std::string toJson(const std::map<std::string, std::string> &data);
std::map<std::string, std::string> fromJson(const std::string &json);

} // namespace vdoninja
