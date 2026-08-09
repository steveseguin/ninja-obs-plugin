/*
 * OBS VDO.Ninja Plugin
 * Multi-peer connection manager
 *
 * Manages multiple WebRTC peer connections for both publishing (multiple viewers)
 * and viewing (multiple publishers) scenarios.
 */

#pragma once

#include <rtc/rtc.hpp>

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>

#include "vdoninja-audio-red.h"
#include "vdoninja-common.h"
#include "vdoninja-ice-candidate-queue.h"
#include "vdoninja-rtcp-feedback.h"
#include "vdoninja-rtp-pacer.h"
#include "vdoninja-rtp-send-tracker.h"
#include "vdoninja-signaling.h"
#include "vdoninja-track-utils.h"

namespace vdoninja
{

class PeerManagerOwnerSession;

// Media track info
struct MediaTrack {
	TrackType type;
	std::shared_ptr<rtc::Track> track;
	std::string mid;
	uint32_t ssrc = 0;
	uint16_t sequenceNumber = 0;
	uint32_t timestamp = 0;
};

// Callbacks for peer events
struct PeerEventIdentity {
	std::string uuid;
	std::string session;
	uint64_t generation = 0;
	uint64_t sequence = 0;
};
struct AcceptedSignalingLifecycleEvent {
	SignalingLifecycleEventKind kind = SignalingLifecycleEventKind::PeerCleanup;
	uint64_t socketEpoch = 0;
	uint64_t wsSequence = 0;
	PeerEventIdentity identity;
	std::string streamId;
};
enum class SignalingLifecycleDisposition {
	Accepted,
	AmbiguousSessionless,
	Stale,
	NonDestructiveHint,
	NoSubscriber,
};
using OnPeerConnectedCallback = std::function<void(const PeerEventIdentity &identity)>;
using OnPeerDisconnectedCallback = std::function<void(const PeerEventIdentity &identity)>;
using OnAcceptedSignalingLifecycleEventCallback = std::function<void(const AcceptedSignalingLifecycleEvent &event)>;
struct TrackSlotEvent {
	std::string uuid;
	std::string session;
	TrackType type = TrackType::Video;
	uint64_t generation = 0;
	uint64_t revision = 0;
	uint64_t sequence = 0;
	std::shared_ptr<rtc::Track> track;
	std::shared_ptr<rtc::Track> retiredTrack;
};
using OnTrackCallback = std::function<void(const TrackSlotEvent &event)>;
using OnDataChannelCallback =
    std::function<void(const PeerEventIdentity &identity, std::shared_ptr<rtc::DataChannel> dc)>;
using OnDataChannelMessageCallback = std::function<void(const PeerEventIdentity &identity, const std::string &message)>;
using OnKeyframeRequestCallback = std::function<void(const std::string &uuid)>;

enum class PeerManagerCompletionKind {
	OwnerSession,
	SignalingOffer,
	SignalingAnswer,
	SignalingOfferRequest,
	SignalingIceRestartRequest,
	SignalingIceCandidate,
	SignalingPeerCleanup,
	PeerConnectionState,
	PeerConnectionLocalDescription,
	PeerConnectionLocalCandidate,
	PeerConnectionGatheringState,
	PeerConnectionTrack,
	PeerConnectionDataChannel,
	DataChannelClosed,
	DataChannelError,
	DataChannelOpen,
	DataChannelMessage,
	TrackClosed,
	TrackError,
	VideoFeedback,
	AudioFeedback,
};
enum class NativeMediaTestOwnerSessionStage {
	BeforePermit,
	PermitAcquired,
	PermitRejected,
	BeforeDetach,
	AfterDetach,
	WaitingForPermits,
	WorkDrained,
};

#if defined(VDONINJA_NATIVE_MEDIA_INTEGRATION_TEST)
enum class NativeMediaTestDataChannelStage {
	IncomingEntered,
	BeforeCallbacksInstalled,
	IncomingReturning,
	AfterErrorCallbackInstalled,
	BeforeDeferredCallbacksDrained,
	DeferredCallbackQueuedDuringDrain,
	BeforeAliasCommit,
	BeforePendingAliasCommit,
	BeforePendingAliasConsume,
	BeforeCallbackCleanup,
};
#endif

// Peer snapshot for runtime diagnostics / UI.
struct PeerSnapshot {
	std::string uuid;
	std::string streamId;
	ConnectionType type = ConnectionType::Publisher;
	ConnectionState state = ConnectionState::New;
	bool hasDataChannel = false;
	bool audioSendEnabled = true;
	bool videoSendEnabled = true;
};

class VDONinjaPeerManager
{
public:
	VDONinjaPeerManager();
	~VDONinjaPeerManager();

	// Initialize with signaling client
	void initialize(VDONinjaSignaling *signaling);

	// Configure ICE servers
	void setIceServers(const std::vector<IceServer> &servers);
	void setForceTurn(bool force);

	// Publishing mode - create offers for incoming viewers
	bool startPublishing(int maxViewers = 10);
	void stopPublishing();
	bool isPublishing() const;
	int getViewerCount() const;
	int getMaxViewers() const;
	bool requestIceRestart(const std::string &uuid, const std::string &session = "");

	// Send media to all connected peers (viewers)
	void sendAudioFrame(const uint8_t *data, size_t size, uint32_t timestamp);
	void sendVideoFrame(const uint8_t *data, size_t size, uint32_t timestamp, bool keyframe);
	void requireLiveKeyframeForAll();
	// cachedReplay identifies the cached startup keyframe. It is only safe before
	// a peer first synchronizes, never for recovery after packet loss.
	bool sendVideoFrameToPeer(const std::string &uuid, const uint8_t *data, size_t size, uint32_t timestamp,
	                          bool keyframe, bool cachedReplay = false);
	bool notePeerKeyframeRequest(const std::string &uuid);
	bool setPeerMediaSendEnabled(const std::string &uuid, bool hasVideo, bool videoEnabled, bool hasAudio,
	                             bool audioEnabled, bool *videoBecameEnabled = nullptr);

	// Viewing mode - receive media from publishers
	bool startViewing(const std::string &streamId);
	void stopViewing(const std::string &streamId);
	bool disconnectPeer(const std::string &uuid);
	bool disconnectPeer(const PeerEventIdentity &identity);

	// Release peers retired from RTC callbacks once they are safely out of band.
	// Call periodically from a non-RTC thread (e.g. a service loop).
	void runDeferredCleanup();

	// Data channel
	void sendDataToAll(const std::string &message);
	void sendDataToPeer(const std::string &uuid, const std::string &message);
	void sendDataToPeer(const PeerEventIdentity &identity, const std::string &message);
	void bindViewerSignalingDataChannel(const std::string &transportPeerUuid, const std::string &targetUuid,
	                                    const std::string &targetSession);

	// Peer events
	void setOnPeerConnected(OnPeerConnectedCallback callback);
	void setOnPeerDisconnected(OnPeerDisconnectedCallback callback);
	void setOnTrack(OnTrackCallback callback);
	void setOnDataChannel(OnDataChannelCallback callback);
	void setOnDataChannelMessage(OnDataChannelMessageCallback callback);
	void setOnKeyframeRequest(OnKeyframeRequestCallback callback);

	// Get peer info
	std::vector<std::string> getConnectedPeers() const;
	std::vector<PeerSnapshot> getPeerSnapshots() const;
	ConnectionState getPeerState(const std::string &uuid) const;
	std::optional<PeerEventIdentity> getPeerIdentity(const std::string &uuid) const;
	std::optional<PeerEventIdentity> claimPeerEventIdentity(const std::string &uuid,
	                                                        const std::string &expectedSession = "",
	                                                        uint64_t expectedGeneration = 0);
	std::optional<PeerEventIdentity> claimSignalingPeerCleanupIdentity(const std::string &uuid,
	                                                                   const std::string &session,
	                                                                   bool *ambiguousReuse = nullptr);
	SignalingLifecycleDisposition processSignalingLifecycleEvent(const SignalingLifecycleEvent &event);
	void setOnAcceptedSignalingLifecycleEvent(OnAcceptedSignalingLifecycleEventCallback callback);

	// Configuration
	void setVideoCodec(VideoCodec codec);
	void setAudioCodec(AudioCodec codec);
	void setH264ProfileLevelId(const std::string &profileLevelId);
	void setBitrate(int bitrate);
	void setVideoProtectionMode(VideoProtectionMode mode);
	void setAudioRedEnabled(bool enable);
	void setEnableDataChannel(bool enable);
	RtcpFeedbackStats takeVideoFeedbackStats();
	std::optional<uint64_t> minimumRecentRembBitrate(std::chrono::milliseconds maxAge) const;
	RtpPacerStats takeVideoPacerStats();
	RtpSendStats takeAudioSendStats();
	AudioRedStats takeAudioRedStats();

#if defined(VDONINJA_NATIVE_MEDIA_INTEGRATION_TEST)
	using NativeMediaTestTrackCommitHook = std::function<void(
	    const std::string &uuid, TrackType type, const std::shared_ptr<rtc::Track> &track, uint64_t generation)>;
	using NativeMediaTestPeerDispatchHook = std::function<void(const std::shared_ptr<PeerInfo> &peer)>;
	using NativeMediaTestTrackDispatchHook = std::function<void(const TrackSlotEvent &event, bool hadSubscriber)>;
	using NativeMediaTestTrackLifecycleHook =
	    std::function<void(const std::shared_ptr<PeerInfo> &peer, TrackType type,
	                       const std::shared_ptr<rtc::Track> &track, uint64_t revision)>;
	using NativeMediaTestTrackHandleHook = std::function<void(const std::shared_ptr<rtc::Track> &track)>;
	using NativeMediaTestDataChannelLifecycleHook =
	    std::function<void(NativeMediaTestDataChannelStage stage, const std::shared_ptr<PeerInfo> &peer,
	                       const std::shared_ptr<rtc::DataChannel> &dc, uint64_t revision)>;
	using NativeMediaTestSignalingLifecycleHook = std::function<void(
	    const SignalingLifecycleEvent &event, const std::optional<PeerEventIdentity> &claimedIdentity)>;
	using NativeMediaTestOwnerSessionHook =
	    std::function<void(NativeMediaTestOwnerSessionStage stage, PeerManagerCompletionKind kind, const void *handle)>;
	std::shared_ptr<PeerInfo> createNativeMediaTestViewerPeer(const std::string &uuid,
	                                                          const std::string &session = "test-session");
	std::shared_ptr<PeerInfo> createNativeMediaTestPublisherPeer(const std::string &uuid,
	                                                             const std::string &session = "test-session");
	void receiveNativeMediaTestTrack(const std::shared_ptr<PeerInfo> &peer, const std::shared_ptr<rtc::Track> &track);
	void prepareNativeMediaTestViewerTracks(const std::shared_ptr<PeerInfo> &peer, const std::string &offerSdp);
	void retireNativeMediaTestPeer(const std::shared_ptr<PeerInfo> &peer);
	void dispatchNativeMediaTestPeerDisconnected(const std::shared_ptr<PeerInfo> &peer);
	void dispatchNativeMediaTestDataChannelMessage(const std::shared_ptr<PeerInfo> &peer, const std::string &message);
	void dispatchNativeMediaTestDataChannelOpen(const std::shared_ptr<PeerInfo> &peer,
	                                            const std::shared_ptr<rtc::DataChannel> &dc);
	void receiveNativeMediaTestDataChannel(const std::shared_ptr<PeerInfo> &peer,
	                                       const std::shared_ptr<rtc::DataChannel> &dc);
	void dispatchNativeMediaTestDataChannelError(const std::shared_ptr<PeerInfo> &peer,
	                                             const std::shared_ptr<rtc::DataChannel> &dc,
	                                             const std::string &error = "test-error");
	size_t nativeMediaTestPendingViewerSignalingDataChannelCount() const;
	size_t nativeMediaTestPendingDataChannelCallbackCleanupCount(const std::shared_ptr<PeerInfo> &peer) const;
	void consumeNativeMediaTestPendingViewerSignalingDataChannel(const std::shared_ptr<PeerInfo> &peer,
	                                                             const std::string &session);
	void retireNativeMediaTestTrackSlot(const std::shared_ptr<PeerInfo> &peer, TrackType type);
	void dispatchNativeMediaTestTrackError(const std::shared_ptr<PeerInfo> &peer, TrackType type);
	bool nativeMediaTestPeerRegistryLockAvailable();
	void setNativeMediaTestTrackCommitHook(NativeMediaTestTrackCommitHook hook);
	void setNativeMediaTestTrackDispatchHook(NativeMediaTestTrackDispatchHook hook);
	void setNativeMediaTestTrackLifecycleHook(NativeMediaTestTrackLifecycleHook hook);
	void setNativeMediaTestTrackBeforeInstallHook(NativeMediaTestTrackHandleHook hook);
	void setNativeMediaTestTrackCleanupHook(NativeMediaTestTrackHandleHook hook);
	void setNativeMediaTestDataChannelLifecycleHook(NativeMediaTestDataChannelLifecycleHook hook);
	void setNativeMediaTestSignalingLifecycleHook(NativeMediaTestSignalingLifecycleHook hook);
	void setNativeMediaTestPeerDisconnectDispatchHook(NativeMediaTestPeerDispatchHook hook);
	void setNativeMediaTestPeerDataDispatchHook(NativeMediaTestPeerDispatchHook hook);
	void setNativeMediaTestPeerDataDispatchCompleteHook(NativeMediaTestPeerDispatchHook hook);
	void setNativeMediaTestPeerDataOpenDispatchHook(NativeMediaTestPeerDispatchHook hook);
	void setNativeMediaTestOwnerSessionHook(NativeMediaTestOwnerSessionHook hook);
	void failNextNativeMediaTestOwnerSessionFunctionRegistration(PeerManagerCompletionKind kind);
	std::function<void()> nativeMediaTestVideoFeedbackCompletion(const std::shared_ptr<PeerInfo> &peer) const;
#endif

private:
	struct PublisherMediaState {
		bool audioSendEnabled = true;
		bool videoSendEnabled = true;
		VideoKeyframeGate videoKeyframeGate;
		uint16_t audioSeq = 0;
		uint16_t videoSeq = 0;
		uint32_t audioTimestamp = 0;
		uint32_t videoTimestamp = 0;
	};

	// Create a new peer connection for a viewer (we send media to them)
	std::shared_ptr<PeerInfo> createPublisherConnection(const std::string &uuid, const std::string &session = "",
	                                                    const PublisherMediaState *initialMediaState = nullptr,
	                                                    bool registerPeer = true);

	// Create a new peer connection for viewing (we receive media from them)
	std::shared_ptr<PeerInfo> createViewerConnection(const std::string &uuid);

	// Handle signaling events
	void onSignalingOffer(const std::string &uuid, const std::string &sdp, const std::string &session);
	void onSignalingAnswer(const std::string &uuid, const std::string &sdp, const std::string &session);
	void onSignalingOfferRequest(const std::string &uuid, const std::string &session);
	void onSignalingIceCandidate(const std::string &uuid, const std::string &candidate, const std::string &mid,
	                             const std::string &session);
	void drainPendingRemoteIceCandidates(const std::shared_ptr<PeerInfo> &peer);

	// Setup peer connection callbacks
	void setupPeerConnectionCallbacks(std::shared_ptr<PeerInfo> peer);
	void dispatchPeerDisconnected(const std::shared_ptr<PeerInfo> &peer);
	void dispatchDataChannelOpen(const std::shared_ptr<PeerInfo> &peer, const std::shared_ptr<rtc::DataChannel> &dc,
	                             uint64_t generation = 0, uint64_t revision = 0);
	void dispatchDataChannelMessage(const std::shared_ptr<PeerInfo> &peer, const std::string &message,
	                                const std::shared_ptr<rtc::DataChannel> &dc = nullptr, uint64_t generation = 0,
	                                uint64_t revision = 0);
	void handleIncomingDataChannel(const std::shared_ptr<PeerInfo> &peer, const std::shared_ptr<rtc::DataChannel> &dc,
	                               bool allowUnregisteredPeer = false);
	bool installDataChannelCallbacks(const std::shared_ptr<PeerInfo> &peer, const std::shared_ptr<rtc::DataChannel> &dc,
	                                 uint64_t generation, uint64_t revision, bool transportOpenObserved,
	                                 bool allowUnregisteredPeer = false);
	bool isDataChannelLeaseCurrent(const std::shared_ptr<PeerInfo> &peer, const std::shared_ptr<rtc::DataChannel> &dc,
	                               uint64_t generation, uint64_t revision, bool requireOpen = false,
	                               bool allowUnregisteredPeer = false) const;
	void handleDataChannelOpen(const std::weak_ptr<PeerInfo> &weakPeer,
	                           const std::weak_ptr<rtc::DataChannel> &weakDataChannel, uint64_t generation,
	                           uint64_t revision);
	void handleDataChannelMessage(const std::weak_ptr<PeerInfo> &weakPeer,
	                              const std::weak_ptr<rtc::DataChannel> &weakDataChannel, uint64_t generation,
	                              uint64_t revision, rtc::message_variant data);
	void handleDataChannelTerminal(const std::weak_ptr<PeerInfo> &weakPeer,
	                               const std::weak_ptr<rtc::DataChannel> &weakDataChannel, uint64_t generation,
	                               uint64_t revision, const char *reason, const std::string &error = "");
	void clearRetiredDataChannelCallbacksIfUnused(const std::shared_ptr<PeerInfo> &peer,
	                                              const std::shared_ptr<rtc::DataChannel> &dc);
	void drainRetiredDataChannelCallbackCleanupForHandle(const std::weak_ptr<PeerInfo> &weakPeer,
	                                                     const std::weak_ptr<rtc::DataChannel> &weakDataChannel);
	void purgeDataChannelAliasesForLease(const std::string &transportUuid, uint64_t transportGeneration,
	                                     const std::shared_ptr<rtc::DataChannel> &dc, uint64_t revision);
	void retirePeerDataChannel(const std::shared_ptr<PeerInfo> &peer);
	void consumePendingViewerSignalingDataChannel(const std::shared_ptr<PeerInfo> &peer, const std::string &session);
	void handleIncomingTrack(const std::shared_ptr<PeerInfo> &peer, const std::shared_ptr<rtc::Track> &track);
	bool updateTrackSlot(const std::shared_ptr<PeerInfo> &peer, TrackType type,
	                     const std::shared_ptr<rtc::Track> &track, TrackSlotEvent &event,
	                     const std::shared_ptr<rtc::Track> &expectedTrack = nullptr, bool requireExpected = false,
	                     uint64_t expectedRevision = 0);
	void dispatchCommittedTrackSlotEvent(const std::shared_ptr<PeerInfo> &peer, const TrackSlotEvent &event);
	void dispatchTrackSlotEvent(const TrackSlotEvent &event);
	bool installTrackLifecycleCallbacks(const std::shared_ptr<PeerInfo> &peer, const TrackSlotEvent &event);
	void handleTrackTerminal(const std::weak_ptr<PeerInfo> &weakPeer, TrackType type,
	                         const std::weak_ptr<rtc::Track> &weakTrack, uint64_t generation, uint64_t revision,
	                         const char *reason, const std::string &error = "");
	bool isTrackSlotLeaseCurrent(const std::shared_ptr<PeerInfo> &peer, TrackType type,
	                             const std::shared_ptr<rtc::Track> &track, uint64_t generation,
	                             uint64_t revision) const;
	void clearRetiredTrackCallbacksIfUnused(const TrackSlotEvent &event);
	void clearTrackSlots(const std::shared_ptr<PeerInfo> &peer);
	void installLocalDescriptionCallback(const std::shared_ptr<PeerInfo> &peer);
#if defined(VDONINJA_NATIVE_MEDIA_INTEGRATION_TEST)
	void invokeNativeMediaTestDataChannelLifecycleHook(NativeMediaTestDataChannelStage stage,
	                                                   const std::shared_ptr<PeerInfo> &peer,
	                                                   const std::shared_ptr<rtc::DataChannel> &dc, uint64_t revision);
#endif

	// Setup tracks for publishing
	void setupPublisherTracks(std::shared_ptr<PeerInfo> peer);
	void prepareViewerTracks(const std::shared_ptr<PeerInfo> &peer, const std::string &offerSdp);
	void clearPeerCallbacks(const std::shared_ptr<PeerInfo> &peer);
	void releasePeerResources(const std::shared_ptr<PeerInfo> &peer);
	void retirePeerForDeferredCleanup(const std::string &uuid, const std::shared_ptr<PeerInfo> &peer);
	void pruneRetiredPeers(int64_t minAgeMs);
	bool isCurrentPeer(const std::shared_ptr<PeerInfo> &peer) const;

	// ICE candidate bundling
	void bundleAndSendCandidates(const std::shared_ptr<PeerInfo> &peer);
	bool sendAudioFrameToPeer(const std::string &uuid, const std::shared_ptr<PeerInfo> &peer, const uint8_t *data,
	                          size_t size, uint32_t timestamp);
	bool sendVideoFrameToPeerHandle(const std::string &uuid, const std::shared_ptr<PeerInfo> &peer, const uint8_t *data,
	                                size_t size, uint32_t timestamp, bool keyframe, bool cachedReplay = false);

	// Get RTC configuration
	rtc::Configuration getRtcConfig() const;

	// Count publisher peers that should consume viewer slots.
	int getPublisherSlotCount() const;

	// Signaling client (not owned)
	VDONinjaSignaling *signaling_ = nullptr;

	// Peer connections
	std::map<std::string, std::shared_ptr<PeerInfo>> peers_;
	std::map<std::string, uint64_t> peerGenerationRegistrationCounts_; // Guarded by peersMutex_.
	uint64_t latestSignalingLifecycleSocketEpoch_ = 0;                 // Guarded by peersMutex_.
	uint64_t latestSignalingLifecycleWsSequence_ = 0;                  // Guarded by peersMutex_.
	mutable std::mutex peersMutex_;
	struct RetiredPeer {
		std::string uuid;
		std::shared_ptr<PeerInfo> peer;
		int64_t retiredAtMs = 0;
	};
	std::vector<RetiredPeer> retiredPeers_;
	std::mutex retiredPeersMutex_;

	// ICE configuration
	std::vector<IceServer> iceServers_;
	bool forceTurn_ = false;

	// Publishing state
	std::atomic<bool> publishing_{false};
	int maxViewers_ = 10;

	// Codec and quality settings
	VideoCodec videoCodec_ = VideoCodec::H264;
	AudioCodec audioCodec_ = AudioCodec::Opus;
	mutable std::mutex codecMutex_;
	std::string h264ProfileLevelId_ = "42e01f";
	std::atomic<int> bitrate_{4000000};
	VideoProtectionMode videoProtectionMode_ = VideoProtectionMode::Off;
	std::atomic<bool> audioRedEnabled_{false};
	bool enableDataChannel_ = true;
	std::shared_ptr<RtpSharedPacerBudget> videoPacerBudget_;

	// Audio/Video SSRC for outgoing media
	uint32_t audioSsrc_ = 0;
	uint32_t videoSsrc_ = 0;
	std::atomic<uint16_t> audioSeq_{0};
	std::atomic<uint16_t> videoSeq_{0};
	uint32_t audioTimestamp_ = 0;
	uint32_t videoTimestamp_ = 0;
	std::atomic<uint64_t> nextPeerGeneration_{1};
	RtpSendTracker audioSendTracker_;
	std::atomic<uint64_t> audioRedPackets_{0};
	std::atomic<uint64_t> audioRedPacketsWithRedundancy_{0};
	std::atomic<uint64_t> audioRedPrimaryOnlyPackets_{0};
	std::atomic<uint64_t> audioRedRedundantBytes_{0};

	// ICE candidate bundling
	struct CandidateBundle {
		std::vector<std::tuple<std::string, std::string>> candidates; // (candidate, mid)
		int64_t lastUpdate = 0;
		std::string session;
	};
	std::map<uint64_t, CandidateBundle> candidateBundles_;
	std::mutex candidateMutex_;
	PendingRemoteIceCandidateQueue pendingRemoteIceCandidates_;
	struct ViewerSignalingDataChannelRoute {
		std::string transportUuid;
		uint64_t transportGeneration = 0;
		uint64_t dataChannelRevision = 0;
		std::shared_ptr<rtc::DataChannel> channel;
	};
	std::map<std::string, ViewerSignalingDataChannelRoute> pendingViewerSignalingDataChannels_;
	mutable std::mutex pendingViewerSignalingMutex_;
	mutable std::mutex dataChannelAliasMutex_;
	std::atomic<bool> shuttingDown_{false};
	std::shared_ptr<PeerManagerOwnerSession> ownerSession_;

	// Callbacks
	mutable std::mutex callbackMutex_;
	OnPeerConnectedCallback onPeerConnected_;
	OnPeerDisconnectedCallback onPeerDisconnected_;
	OnTrackCallback onTrack_;
	OnDataChannelCallback onDataChannel_;
	OnDataChannelMessageCallback onDataChannelMessage_;
	OnKeyframeRequestCallback onKeyframeRequest_;
	OnAcceptedSignalingLifecycleEventCallback onAcceptedSignalingLifecycleEvent_;
#if defined(VDONINJA_NATIVE_MEDIA_INTEGRATION_TEST)
	NativeMediaTestTrackCommitHook nativeMediaTestTrackCommitHook_;
	NativeMediaTestTrackDispatchHook nativeMediaTestTrackDispatchHook_;
	NativeMediaTestTrackLifecycleHook nativeMediaTestTrackLifecycleHook_;
	NativeMediaTestTrackHandleHook nativeMediaTestTrackBeforeInstallHook_;
	NativeMediaTestTrackHandleHook nativeMediaTestTrackCleanupHook_;
	NativeMediaTestDataChannelLifecycleHook nativeMediaTestDataChannelLifecycleHook_;
	NativeMediaTestSignalingLifecycleHook nativeMediaTestSignalingLifecycleHook_;
	NativeMediaTestPeerDispatchHook nativeMediaTestPeerDisconnectDispatchHook_;
	NativeMediaTestPeerDispatchHook nativeMediaTestPeerDataDispatchHook_;
	NativeMediaTestPeerDispatchHook nativeMediaTestPeerDataDispatchCompleteHook_;
	NativeMediaTestPeerDispatchHook nativeMediaTestPeerDataOpenDispatchHook_;
	std::map<uint64_t, std::function<void()>> nativeMediaTestVideoFeedbackCompletions_;
#endif
};

} // namespace vdoninja
