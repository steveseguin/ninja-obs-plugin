/*
 * OBS VDO.Ninja Plugin
 * WebSocket signaling client for VDO.Ninja
 */

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <thread>
#include <utility>

#include "vdoninja-common.h"
#include "vdoninja-reliability.h"
#include "vdoninja-signaling-protocol.h"
#include "vdoninja-utils.h"

namespace rtc
{
class WebSocket;
}

namespace vdoninja
{

// Message types from VDO.Ninja signaling server
enum class SignalMessageType {
	Unknown,
	Listing,   // Room member listing
	Offer,     // SDP offer from peer
	Answer,    // SDP answer from peer
	Candidate, // ICE candidate
	Request,   // Request from server (e.g., sendOffer)
	Alert,     // Alert/error message
	Error,     // Error response
	VideoAddedToRoom,
	VideoRemovedFromRoom,
	Transferred, // User transferred to another room
	Ping,
	Pong
};

enum class SignalingLifecycleEventKind { PeerCleanup, StreamRemoved };

// Correlation metadata owned by the signaling transport. A missing session is
// deliberately uncorrelated: signaling must never relabel a UUID-only event
// with a receiver's current session. socketEpoch/wsSequence are zero only for
// messages injected through processIncomingMessage() rather than WebSocket.
struct SignalingLifecycleEvent {
	SignalingLifecycleEventKind kind = SignalingLifecycleEventKind::PeerCleanup;
	uint64_t socketEpoch = 0;
	uint64_t wsSequence = 0;
	std::string uuid;
	std::optional<std::string> session;
	std::string streamId;

	bool hasCorrelatedSession() const { return session.has_value(); }
	bool hasWebSocketOrigin() const { return socketEpoch != 0 && wsSequence != 0; }
};

using OnSignalingLifecycleEventCallback = std::function<void(const SignalingLifecycleEvent &event)>;

// Signaling client for VDO.Ninja WebSocket server
class VDONinjaSignaling
{
public:
	VDONinjaSignaling();
	~VDONinjaSignaling();

	// Connection management
	bool connect(const std::string &wssHost = DEFAULT_WSS_HOST);
	void disconnect();
	bool isConnected() const;

	// Room management
	bool joinRoom(const std::string &roomId, const std::string &password = "", bool claimDirector = false);
	bool leaveRoom();
	bool isInRoom() const;
	std::string getCurrentRoomId() const;
	std::vector<std::string> getCurrentRoomMembers() const;

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
	void sendIceCandidate(const std::string &uuid, const std::string &candidate, const std::string &mid,
	                      const std::string &session, const std::string &candidateType = "local");
	void sendAnswerViaDataChannel(const std::shared_ptr<rtc::DataChannel> &dc, const std::string &uuid,
	                              const std::string &sdp, const std::string &session);
	bool sendIceCandidateViaDataChannel(const std::shared_ptr<rtc::DataChannel> &dc, const std::string &uuid,
	                                    const std::string &candidate, const std::string &mid,
	                                    const std::string &session, const std::string &candidateType = "local");

	// Reuse signaling parsing for messages received over alternate transports
	void processIncomingMessage(const std::string &message);

	// Event callbacks may call disconnect(). A reconnect requested before the
	// callback returns is rejected and must be retried by the owner afterward.
	// Destroying this signaling instance from inside its own callback is not a
	// supported lifetime transition; defer owner destruction until callback exit.
	void setOnConnected(OnConnectedCallback callback);
	void setOnDisconnected(OnDisconnectedCallback callback);
	void setOnError(OnErrorCallback callback);
	void setOnOffer(OnOfferCallback callback);
	void setOnAnswer(OnAnswerCallback callback);
	void setOnOfferRequest(OnOfferRequestCallback callback);
	void setOnIceRestartRequest(OnIceRestartRequestCallback callback);
	void setOnIceCandidate(OnIceCandidateCallback callback);
	void setOnRoomJoined(OnRoomJoinedCallback callback);
	void setOnStreamAdded(OnStreamAddedCallback callback);
	void setOnStreamRemoved(OnStreamRemovedCallback callback);
	void setOnPeerCleanup(OnPeerCleanupCallback callback);
	// Additive migration API. An integrator must choose one manager-owned
	// lifecycle path; do not install both this callback and legacy cleanup
	// callbacks for the same consumer or cleanup would be applied twice.
	void setOnLifecycleEvent(OnSignalingLifecycleEventCallback callback);
	void setOnData(OnDataCallback callback);

	// Configuration
	void setSalt(const std::string &salt);
	void setDefaultPassword(const std::string &password);
	void setAutoReconnect(bool enable, int maxAttempts = DEFAULT_RECONNECT_ATTEMPTS);

	// Get our UUID (assigned by server or generated locally)
	std::string getLocalUUID() const;

#ifdef TESTING_BUILD
	static void setEncryptionFailureForTesting(bool forceFailure);

	struct SocketCallbacksForTesting {
		uint64_t socketEpoch = 0;
		std::function<void()> onOpen;
		std::function<void(const std::string &)> onMessage;
		std::function<void()> onClosed;
		std::function<void(const std::string &)> onError;
		std::function<bool()> reconnectTimerMayProceed;
	};

	// Creates the same callback closures used by the production WebSocket path,
	// without opening a network transport. Retaining one bundle while creating a
	// second deterministically models a delayed callback from a replaced socket.
	SocketCallbacksForTesting beginSocketAttemptForTesting();
	bool reconnectRequestedForTesting() const;
	uint64_t currentSocketEpochForTesting() const;
	void setBeforeSocketStateCommitForTesting(std::function<void()> callback);
	bool reconnectSuppressedForTesting() const;
	void invokeSocketUserCallbackForTesting(std::function<void()> callback);
#endif

private:
	class UserCallbackGuard
	{
	public:
		UserCallbackGuard(VDONinjaSignaling &owner, uint64_t socketEpoch);
		~UserCallbackGuard();
		UserCallbackGuard(const UserCallbackGuard &) = delete;
		UserCallbackGuard &operator=(const UserCallbackGuard &) = delete;
		explicit operator bool() const { return active_; }

	private:
		VDONinjaSignaling *owner_ = nullptr;
		bool socketOrigin_ = false;
		bool active_ = false;
	};

	uint64_t beginSocketEpoch();
	uint64_t beginSocketEpochIfCurrent(uint64_t expectedSocketEpoch);
	std::unique_lock<std::mutex> acquireSocketCallback(uint64_t socketEpoch, uint64_t &wsSequence);
	bool isCurrentSocketEpoch(uint64_t socketEpoch) const;
	bool beginUserCallback(uint64_t socketEpoch, bool &socketOrigin);
	void endUserCallback(bool socketOrigin);
	template <typename Callback> void invokeUserCallback(uint64_t socketEpoch, Callback &&callback)
	{
		UserCallbackGuard guard(*this, socketEpoch);
		if (guard) {
			std::forward<Callback>(callback)();
		}
	}
	bool hasSocketUserCallbacks() const;
	void waitForSocketUserCallbacks();
	void waitForAllUserCallbacks();
	bool isUserCallbackOnCurrentThread() const;
	bool joinWebSocketThread();
	std::shared_ptr<rtc::WebSocket> takeWebSocketHandle(uint64_t expectedSocketEpoch = 0);
	void runBeforeSocketStateCommitForTesting();
	template <typename Operation> bool withCurrentSocketEpoch(uint64_t socketEpoch, Operation &&operation)
	{
		if (socketEpoch == 0) {
			std::forward<Operation>(operation)();
			return true;
		}
		std::lock_guard<std::mutex> lock(socketEpochMutex_);
		if (!isCurrentSocketEpoch(socketEpoch)) {
			return false;
		}
		std::forward<Operation>(operation)();
		return true;
	}
	template <typename Operation> bool commitSocketState(uint64_t socketEpoch, Operation &&operation)
	{
		runBeforeSocketStateCommitForTesting();
		return withCurrentSocketEpoch(socketEpoch, std::forward<Operation>(operation));
	}
	void handleSocketOpen(uint64_t socketEpoch, const std::shared_ptr<std::atomic<int>> &reconnectAttempts,
	                      const std::string &host, const std::shared_ptr<std::atomic<bool>> &opened);
	void handleSocketClosed(uint64_t socketEpoch, const std::string &host,
	                        const std::shared_ptr<std::atomic<bool>> &opened);
	void handleSocketError(uint64_t socketEpoch, const std::string &host,
	                       const std::shared_ptr<std::atomic<bool>> &opened, const std::string &error);
	void handleSocketMessage(uint64_t socketEpoch, const std::string &message);
	bool reconnectTimerMayProceed(uint64_t socketEpoch);

	// WebSocket handling (using a simple implementation)
	void wsThreadFunc(uint64_t initialSocketEpoch);
	void processMessage(const std::string &message, uint64_t socketEpoch = 0, uint64_t wsSequence = 0);
	void sendMessage(const std::string &message);
	void queueMessage(const std::string &message);
	void clearSendQueue();
	void applyServerAlertPolicy(const std::string &alert);
	void notifyDisconnected();
	void notifyError(const std::string &error);

	// Message handlers
	void handleRequest(const ParsedSignalMessage &message, uint64_t socketEpoch);
	std::string getActiveSignalingPassword() const;

	// Internal state
	std::string wssHost_;
	std::vector<std::string> wssHosts_;
	size_t activeWssHostIndex_ = 0;
	std::string salt_ = DEFAULT_SALT;
	std::string defaultPassword_ = DEFAULT_PASSWORD;
	std::string localUUID_;
	std::string deferredConnectionError_;

	// Room state (protected by stateMutex_)
	RoomInfo currentRoom_;
	StreamInfo publishedStream_;
	std::map<std::string, StreamInfo> viewingStreams_;

	// Connection state
	std::atomic<bool> connected_{false};
	std::atomic<bool> shouldRun_{false};
	std::atomic<bool> needsReconnect_{false};
	std::atomic<bool> initialConnectionFinished_{false};
	std::atomic<bool> disconnectNotified_{true};

	// Config (protected by stateMutex_)
	bool autoReconnect_ = true;
	int maxReconnectAttempts_ = DEFAULT_RECONNECT_ATTEMPTS;
	bool reconnectSuppressedByServer_ = false;
	int64_t reconnectDeferredUntilMs_ = 0;

	// Threading
	std::thread wsThread_;
	std::mutex wsThreadJoinMutex_;
	std::mutex sendMutex_;
	std::queue<std::string> sendQueue_;
	std::condition_variable sendCv_;

	// WebSocket handle (protected by handleMutex_)
	void *wsHandle_ = nullptr;
	uint64_t wsHandleEpoch_ = 0;
	std::mutex handleMutex_;

	// Identifies the only WebSocket attempt allowed to mutate shared signaling
	// state. wsSequence_ orders accepted callbacks across the client lifetime.
	// Admission serializes internal callback state commits with socket
	// replacement. It is always released before RTC or user callbacks.
	mutable std::mutex socketEpochMutex_;
	std::atomic<uint64_t> socketEpoch_{0};
	uint64_t wsSequence_ = 0;

	// Tracks external callbacks separately from callback registration storage.
	// disconnect() may defer socket close/join while a callback is active, and
	// reconnect/destruction subsequently wait for the worker to finish.
	mutable std::mutex userCallbackMutex_;
	std::condition_variable userCallbackCv_;
	size_t userCallbacksInFlight_ = 0;
	size_t socketUserCallbacksInFlight_ = 0;
	std::map<std::thread::id, size_t> userCallbackThreads_;

#ifdef TESTING_BUILD
	std::mutex socketStateCommitHookMutex_;
	std::function<void()> beforeSocketStateCommitForTesting_;
#endif

	// Protects room/stream/config state
	mutable std::mutex stateMutex_;

	// Protects all callback members
	mutable std::mutex callbackMutex_;

	// Callbacks (protected by callbackMutex_)
	OnConnectedCallback onConnected_;
	OnDisconnectedCallback onDisconnected_;
	OnErrorCallback onError_;
	OnOfferCallback onOffer_;
	OnAnswerCallback onAnswer_;
	OnOfferRequestCallback onOfferRequest_;
	OnIceRestartRequestCallback onIceRestartRequest_;
	OnIceCandidateCallback onIceCandidate_;
	OnRoomJoinedCallback onRoomJoined_;
	OnStreamAddedCallback onStreamAdded_;
	OnStreamRemovedCallback onStreamRemoved_;
	OnPeerCleanupCallback onPeerCleanup_;
	OnSignalingLifecycleEventCallback onLifecycleEvent_;
	OnDataCallback onData_;
};

} // namespace vdoninja
