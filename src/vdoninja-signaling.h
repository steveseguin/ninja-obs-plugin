/*
 * OBS VDO.Ninja Plugin
 * WebSocket signaling client for VDO.Ninja
 */

#pragma once

#include "vdoninja-common.h"
#include "vdoninja-utils.h"
#include <thread>
#include <queue>
#include <condition_variable>

namespace vdoninja {

// Message types from VDO.Ninja signaling server
enum class SignalMessageType {
    Unknown,
    Listing,       // Room member listing
    Offer,         // SDP offer from peer
    Answer,        // SDP answer from peer
    Candidate,     // ICE candidate
    Request,       // Request from server (e.g., sendOffer)
    Alert,         // Alert/error message
    Error,         // Error response
    VideoAddedToRoom,
    VideoRemovedFromRoom,
    Transferred,   // User transferred to another room
    Ping,
    Pong
};

// Signaling client for VDO.Ninja WebSocket server
class VDONinjaSignaling {
public:
    VDONinjaSignaling();
    ~VDONinjaSignaling();

    // Connection management
    bool connect(const std::string &wssHost = DEFAULT_WSS_HOST);
    void disconnect();
    bool isConnected() const;

    // Room management
    bool joinRoom(const std::string &roomId, const std::string &password = "");
    bool leaveRoom();
    bool isInRoom() const;
    std::string getCurrentRoomId() const;

    // Stream publishing
    bool publishStream(const std::string &streamId, const std::string &password = "");
    bool unpublishStream();
    bool isPublishing() const;
    std::string getPublishedStreamId() const;

    // Stream viewing
    bool viewStream(const std::string &streamId, const std::string &password = "");
    bool stopViewing(const std::string &streamId);

    // WebRTC signaling
    void sendOffer(const std::string &uuid, const std::string &sdp, const std::string &session);
    void sendAnswer(const std::string &uuid, const std::string &sdp, const std::string &session);
    void sendIceCandidate(const std::string &uuid, const std::string &candidate,
                          const std::string &mid, const std::string &session);

    // Data channel messaging
    void sendDataMessage(const std::string &uuid, const std::string &data);

    // Event callbacks
    void setOnConnected(OnConnectedCallback callback);
    void setOnDisconnected(OnDisconnectedCallback callback);
    void setOnError(OnErrorCallback callback);
    void setOnOffer(OnOfferCallback callback);
    void setOnAnswer(OnAnswerCallback callback);
    void setOnIceCandidate(OnIceCandidateCallback callback);
    void setOnRoomJoined(OnRoomJoinedCallback callback);
    void setOnStreamAdded(OnStreamAddedCallback callback);
    void setOnStreamRemoved(OnStreamRemovedCallback callback);
    void setOnData(OnDataCallback callback);

    // Configuration
    void setSalt(const std::string &salt);
    void setDefaultPassword(const std::string &password);
    void setAutoReconnect(bool enable, int maxAttempts = DEFAULT_RECONNECT_ATTEMPTS);

    // Get our UUID (assigned by server or generated locally)
    std::string getLocalUUID() const;

private:
    // WebSocket handling (using a simple implementation)
    void wsThreadFunc();
    void processMessage(const std::string &message);
    void sendMessage(const std::string &message);
    void queueMessage(const std::string &message);

    // Message handlers
    void handleListing(const JsonParser &json);
    void handleOffer(const JsonParser &json);
    void handleAnswer(const JsonParser &json);
    void handleCandidate(const JsonParser &json);
    void handleRequest(const JsonParser &json);
    void handleAlert(const JsonParser &json);
    void handleVideoAddedToRoom(const JsonParser &json);
    void handleVideoRemovedFromRoom(const JsonParser &json);

    // Reconnection logic
    void attemptReconnect();

    // Internal state
    std::string wssHost_;
    std::string salt_ = DEFAULT_SALT;
    std::string defaultPassword_ = DEFAULT_PASSWORD;
    std::string localUUID_;

    // Room state
    RoomInfo currentRoom_;
    StreamInfo publishedStream_;
    std::map<std::string, StreamInfo> viewingStreams_;

    // Connection state
    std::atomic<bool> connected_{false};
    std::atomic<bool> shouldRun_{false};
    bool autoReconnect_ = true;
    int reconnectAttempts_ = 0;
    int maxReconnectAttempts_ = DEFAULT_RECONNECT_ATTEMPTS;

    // Threading
    std::thread wsThread_;
    std::mutex sendMutex_;
    std::queue<std::string> sendQueue_;
    std::condition_variable sendCv_;

    // WebSocket handle (platform-specific, abstracted)
    void *wsHandle_ = nullptr;

    // Callbacks
    OnConnectedCallback onConnected_;
    OnDisconnectedCallback onDisconnected_;
    OnErrorCallback onError_;
    OnOfferCallback onOffer_;
    OnAnswerCallback onAnswer_;
    OnIceCandidateCallback onIceCandidate_;
    OnRoomJoinedCallback onRoomJoined_;
    OnStreamAddedCallback onStreamAdded_;
    OnStreamRemovedCallback onStreamRemoved_;
    OnDataCallback onData_;
};

} // namespace vdoninja
