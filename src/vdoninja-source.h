/*
 * OBS VDO.Ninja Plugin
 * Source module for viewing streams from VDO.Ninja
 */

#pragma once

#include <obs-module.h>

#include <atomic>
#include <deque>
#include <functional>
#include <thread>
#include <unordered_set>

#include "vdoninja-alpha-sync.h"
#include "vdoninja-common.h"
#include "vdoninja-data-channel.h"
#include "vdoninja-peer-manager.h"
#include "vdoninja-reliability.h"
#include "vdoninja-signaling.h"

extern "C" {
struct AVBufferRef;
struct AVCodecContext;
struct AVFrame;
struct AVPacket;
struct SwsContext;
struct SwrContext;
}

namespace vdoninja
{

// Video codec used by the native receiver.
enum class NativeVideoCodec {
	H264,
	VP9,
};

#if defined(VDONINJA_NATIVE_MEDIA_INTEGRATION_TEST)
enum class NativeMediaTestStage {
	Identity,
	PreAssembly,
	PreDecode,
	PrePair,
	PreOutput,
	PreCommit,
	CommitAuthorized,
	SuppressionRequest,
	SuppressionAttempt,
	MutePublished,
	SuppressionResetRequest,
	PreStallClear,
	DimensionUpdateMidpoint,
	PendingBundlePrimaryAttached,
	PendingBundlePacketRejected,
};

struct NativeMediaTestOutput {
	std::vector<uint8_t> bgra;
	uint32_t width = 0;
	uint32_t height = 0;
	uint32_t rtpTimestamp = 0;
	uint64_t outputTimestampNs = 0;
	bool hasAlpha = false;
};

struct NativeMediaTestSnapshot {
	bool primaryAssemblyActive = false;
	bool alphaAssemblyActive = false;
	bool primaryDecoderAllocated = false;
	bool alphaDecoderAllocated = false;
	size_t primaryAssemblyBytes = 0;
	size_t alphaAssemblyBytes = 0;
	size_t pendingPrimaryFrames = 0;
	size_t pendingAlphaFrames = 0;
	int retainedVideoFrames = 0;
	int primaryRequestedThreadCount = -1;
	int primaryRequestedThreadType = -1;
	int primaryActiveThreadType = 0;
	int alphaRequestedThreadCount = -1;
	int alphaRequestedThreadType = -1;
	int alphaActiveThreadType = 0;
	bool videoOutputActive = false;
	int64_t lastVideoTimeMs = 0;
	uint32_t outputWidth = 0;
	uint32_t outputHeight = 0;
	bool videoSuppressed = false;
	bool audioMuted = false;
	bool sourceAudioActive = false;
	bool mediaVideoMuted = false;
	bool directorVideoMuted = false;
	bool virtualHangup = false;
	int acceptedPeerCleanups = 0;
	int peerRetirements = 0;
	int peerRetrySchedules = 0;
	int dataChannelOpenActions = 0;
	int ambiguousSessionlessCleanups = 0;
	int targetedPeerByes = 0;
	int legacyStreamRemovalActions = 0;
};

struct NativeMediaTestTag {
};

struct NativeMediaTestTrackSnapshot {
	const rtc::Track *video = nullptr;
	const rtc::Track *alpha = nullptr;
	const rtc::Track *audio = nullptr;
};
#endif

void vdoninja_source_child_audio_capture(void *param, obs_source_t *source, const struct audio_data *audioData,
                                         bool muted);
void vdoninja_source_child_audio_activate(void *param, calldata_t *calldata);
void vdoninja_source_child_audio_deactivate(void *param, calldata_t *calldata);

class VDONinjaSource
{
public:
	VDONinjaSource(obs_data_t *settings, obs_source_t *source);
#if defined(VDONINJA_NATIVE_MEDIA_INTEGRATION_TEST)
	explicit VDONinjaSource(NativeMediaTestTag);
#endif
	~VDONinjaSource();

	void update(obs_data_t *settings);
	void activate();
	void deactivate();
	void show();
	void hide();
	void videoTick(float seconds);
	void videoRender(gs_effect_t *effect);
	uint32_t getWidth() const;
	uint32_t getHeight() const;
	bool isConnected() const;
	std::string getStreamId() const;
	obs_source_t *obsSourceHandle() const;
	obs_source_t *acquireActiveChildSource() const;

#if defined(VDONINJA_NATIVE_MEDIA_INTEGRATION_TEST)
	using NativeMediaTestStageHook =
	    std::function<void(NativeMediaTestStage stage, bool alpha, uint32_t rtpTimestamp, uint64_t mediaEpoch)>;
	using NativeMediaTestOutputHook = std::function<void(NativeMediaTestOutput output)>;
	using NativeMediaTestAudioOutputHook = std::function<void(uint64_t timestampNs)>;
	using NativeMediaTestClearOutputHook = std::function<void(bool hadVideo, std::string reason)>;
	using NativeMediaTestSendPacketHook = std::function<int(AVCodecContext *decoder, const AVPacket *packet)>;
	using NativeMediaTestReceiveFrameHook = std::function<int(AVCodecContext *decoder, AVFrame *frame)>;
	using NativeMediaTestSignalingLifecycleHook = std::function<void(
	    const SignalingLifecycleEvent &event, const std::optional<PeerEventIdentity> &claimedIdentity)>;

	void setNativeMediaTestStageHook(NativeMediaTestStageHook hook);
	void setNativeMediaTestOutputHook(NativeMediaTestOutputHook hook);
	void setNativeMediaTestAudioOutputHook(NativeMediaTestAudioOutputHook hook);
	void setNativeMediaTestClearOutputHook(NativeMediaTestClearOutputHook hook);
	void setNativeMediaTestVideoDecoderHooks(NativeMediaTestSendPacketHook sendHook,
	                                         NativeMediaTestReceiveFrameHook receiveHook);
	void setNativeMediaTestAlphaDecoderHooks(NativeMediaTestSendPacketHook sendHook,
	                                         NativeMediaTestReceiveFrameHook receiveHook);
	void feedNativeMediaTestVp9AccessUnit(bool alpha, const std::vector<uint8_t> &accessUnit, uint32_t rtpTimestamp);
	void feedNativeMediaTestVp9Packet(bool alpha, const std::vector<uint8_t> &payload, uint32_t rtpTimestamp,
	                                  bool startOfFrame, bool endOfFrame);
	void transitionNativeMediaTestPipeline(bool alphaActive, bool enableOutput = true);
	void applyNativeMediaTestVideoSuppression(bool suppressed);
	void applyNativeMediaTestVideoSuppressionUpdate(const ReceiverVideoSuppressionUpdate &update);
	void resetNativeMediaTestState();
	void applyNativeMediaTestPeerCleanup(const PeerEventIdentity &identity);
	void applyNativeMediaTestSignalingCleanup(VDONinjaPeerManager &manager, const std::string &uuid,
	                                          const std::string &session);
	void applyNativeMediaTestSignalingLifecycleEvent(VDONinjaPeerManager &manager,
	                                                 const SignalingLifecycleEvent &event);
	void applyNativeMediaTestLegacyStreamRemoved(const std::string &streamId, const std::string &uuid);
	void setNativeMediaTestSignalingLifecycleHook(NativeMediaTestSignalingLifecycleHook hook);
	void setNativeMediaTestStreamId(const std::string &streamId);
	void bindNativeMediaTestSignaling(VDONinjaSignaling &signaling, VDONinjaPeerManager &manager);
	void setNativeMediaTestSignaling(std::unique_ptr<VDONinjaSignaling> signaling);
	bool advanceNativeMediaTestPeerIdentity(const PeerEventIdentity &identity);
	void emitNativeMediaTestAudioFrame(uint64_t timestampNs);
	void ageNativeMediaTestVideoOutput(int64_t ageMs);
	void updateNativeMediaTestDimensions(uint32_t width, uint32_t height);
	NativeMediaTestSnapshot nativeMediaTestSnapshot();
	NativeMediaTestTrackSnapshot nativeMediaTestTrackSnapshot();
	void bindNativeMediaTestPeerManager(VDONinjaPeerManager &manager);
	bool nativeMediaTestCanAcquireVideoCommitState();
	int nativeMediaTestRejectedTrackEventCount() const;
	uint64_t nativeMediaTestEpoch() const;
#endif

private:
	void loadSettings(obs_data_t *settings);
	bool isInternalNativeSource() const;
	bool usingNativeReceiver() const;
	void connect();
	void disconnect();
	void connectionThread();
	void requestViewStream(const char *reason, bool resetRetryCount = false);
	void scheduleViewRetry(const char *reason, int delayMs, bool resetRetryCount = false);
	void cancelViewRetry();
	void resetViewRetryState();
	void serviceViewRetry();
	VDONinjaPeerManager *activePeerManager() const;
	void sendViewerPreferencesToPeer(const std::string &uuid, const char *reason);
	void sendViewerPreferencesToPeer(const PeerEventIdentity &identity, const char *reason);
	void handlePeerDataChannelOpen(const PeerEventIdentity &identity, const std::shared_ptr<rtc::DataChannel> &dc);
	void requestNativeTargetBitrate(const char *reason);
	void handlePeerDataChannelMessage(const PeerEventIdentity &identity, const std::string &message);
	void handlePeerControlState(const PeerEventIdentity &identity, const MuteStateUpdate *muteUpdate,
	                            const ReceiverVideoSuppressionUpdate *videoUpdate);
	void handleReceiverVideoSuppressionState(const std::string &uuid, const ReceiverVideoSuppressionUpdate &update);
	void handleSignalingAlert(const std::string &message);
	void configureSignalingLifecycleCallbacks(VDONinjaSignaling &signaling, VDONinjaPeerManager &manager);
	void handleAcceptedSignalingLifecycleEvent(const AcceptedSignalingLifecycleEvent &event);
	bool applyPeerCleanupSignal(const PeerEventIdentity &identity, const char *reason);
	void handlePeerCleanupSignal(const PeerEventIdentity &identity);
	void handleSignalingPeerCleanup(VDONinjaPeerManager &manager, const std::string &uuid, const std::string &session);
	void handleStreamRemovedSignal(const std::string &streamId, const std::string &uuid);
	void handlePeerDisconnected(const PeerEventIdentity &identity);
	void handlePeerDisconnectedAccepted(const PeerEventIdentity &identity, bool scheduleRetry);
	enum class PeerEventLane { Unordered, DataMessage, DataChannelOpen };
	bool acceptPeerEventIdentityLocked(const PeerEventIdentity &identity, bool terminalEvent = false,
	                                   PeerEventLane lane = PeerEventLane::Unordered);
	void resetPeerGenerationSuppressionStateLocked(const PeerEventIdentity &identity);
	void publishActivePeerControlStateLocked(const char *reason);
	void markNativePeerConnectedIfReady(const PeerEventIdentity &identity, const char *reason);
	void markNativePeerConnectedIfReadyAccepted(const PeerEventIdentity &identity, const char *reason);
	bool matchesTargetStreamId(const std::string &streamId) const;
	void clearNativeVideoOutput(const char *reason);
	void clearNativeVideoOutputLocked(const char *reason);
	void onVideoTrack(const PeerEventIdentity &identity, std::shared_ptr<rtc::Track> track);
	void onAlphaVideoTrack(const PeerEventIdentity &identity, std::shared_ptr<rtc::Track> track,
	                       std::shared_ptr<rtc::Track> retiredTrack = nullptr);
	void onAudioTrack(const PeerEventIdentity &identity, std::shared_ptr<rtc::Track> track);
	void handleTrackSlotEvent(const TrackSlotEvent &event);
	void handleAudioTrackRetired(const TrackSlotEvent &event);
	void handleVideoTrackClosed(const PeerEventIdentity &identity, const std::shared_ptr<rtc::Track> &track,
	                            bool alphaTrack, const char *reason);
	struct PendingPeerTrackBundle;
	void deferPeerTrackLocked(const PeerEventIdentity &identity, TrackType type,
	                          const std::shared_ptr<rtc::Track> &track);
	void removePendingPeerTrackLocked(const TrackSlotEvent &event);
	std::optional<PeerEventIdentity> adoptNextPendingPeerBundleIfOwnerless(const char *reason);
	void processVideoRtpPacket(const uint8_t *packetData, size_t packetSize, uint64_t mediaEpoch);
	void processVP9RtpPacket(const uint8_t *payload, size_t payloadSize, uint32_t rtpTimestamp, uint64_t mediaEpoch);
	void processAlphaRtpPacket(const uint8_t *packetData, size_t packetSize, uint64_t mediaEpoch);
	void processAlphaVP9RtpPacket(const uint8_t *payload, size_t payloadSize, uint32_t rtpTimestamp,
	                              uint64_t mediaEpoch);
	void processAudioRtpPacket(const uint8_t *packetData, size_t packetSize);
	void processVideoData(const uint8_t *data, size_t size, uint32_t rtpTimestamp, uint64_t mediaEpoch);
	void processAlphaVideoData(const uint8_t *data, size_t size, uint32_t rtpTimestamp, uint64_t mediaEpoch);
	void processAudioData(const uint8_t *data, size_t size, uint32_t rtpTimestamp);
	bool initializeVideoDecoder();
	bool initializeAlphaDecoder();
	bool initializeAudioDecoder(int sampleRate, int channels);
	void resetVideoDecoder();
	void resetAlphaDecoder();
	void resetAudioDecoder();
	void resetVideoDecoderStorageLocked();
	void resetAlphaDecoderStorageLocked();
	void resetMediaPipelineStateLocked();
	void completeMediaPipelineTransition(const char *reason, bool enableOutput);
	void resetNativeState();
	void outputDecodedVideoFrame(const AVFrame *frame, uint32_t rtpTimestamp, uint64_t mediaEpoch);
	void outputPairedVideoFrame(AlphaFramePair pair, bool completedByAlpha);
	void outputDecodedVideoFrameLocked(const AVFrame *frame, uint32_t rtpTimestamp, const PendingAlphaFrame *alphaFrame,
	                                   uint64_t mediaEpoch);
	void handleDecodedAlphaFrame(PendingAlphaFrame frame, uint64_t mediaEpoch);
	std::shared_ptr<AVFrame> retainVideoFrame(const AVFrame *frame);
	int sendVideoPacket(AVCodecContext *decoder, const AVPacket *packet);
	int receiveVideoFrame(AVCodecContext *decoder, AVFrame *frame);
	int sendAlphaPacket(AVCodecContext *decoder, const AVPacket *packet);
	int receiveAlphaFrame(AVCodecContext *decoder, AVFrame *frame);
	bool hasNativeVideoOutputTarget() const;
	struct OutputDimensions {
		uint32_t width = 0;
		uint32_t height = 0;
	};
	OutputDimensions outputDimensions() const;
	void publishOutputDimensions(uint32_t width, uint32_t height);
#if defined(VDONINJA_NATIVE_MEDIA_INTEGRATION_TEST)
	void runNativeMediaTestStage(NativeMediaTestStage stage, bool alpha, uint32_t rtpTimestamp, uint64_t mediaEpoch);
#endif
	void outputDecodedAudioFrame(const AVFrame *frame, uint64_t timestampNs);
	uint64_t mapAudioTimestamp(uint32_t rtpTimestamp);
	void ensureNativeReceiverSource();
	void releaseNativeReceiverSource();
	void updateNativeReceiverSource();
	void ensureBrowserSource();
	void releaseBrowserSource();
	void updateBrowserSource();
	void releaseChildSources();
	void updateWrapperChildSource();
	void syncChildLifecycleState(obs_source_t *child);
	void detachChildLifecycleState(obs_source_t *child);
	std::string buildViewerUrl() const;
	void onChildAudioCaptured(const struct audio_data *audioData, bool muted);
	void onChildAudioActivated();
	void onChildAudioDeactivated();
	void setObsSourceAudioActive(bool active);
	void drainAsyncCallbacks();
	friend void vdoninja_source_child_audio_capture(void *param, obs_source_t *source,
	                                                const struct audio_data *audioData, bool muted);
	friend void vdoninja_source_child_audio_activate(void *param, calldata_t *calldata);
	friend void vdoninja_source_child_audio_deactivate(void *param, calldata_t *calldata);

	obs_source_t *source_ = nullptr;
	obs_weak_source_t *sourceWeak_ = nullptr;
	SourceSettings settings_;
	bool internalNativeSource_ = false;
	std::unique_ptr<VDONinjaSignaling> signaling_;
	std::unique_ptr<VDONinjaPeerManager> peerManager_;
	VDONinjaDataChannel dataChannel_;
	std::atomic<bool> active_{false};
	std::atomic<bool> showing_{false};
	std::atomic<bool> connected_{false};
	std::atomic<bool> nativeRunning_{false};
	std::atomic<bool> loggedFirstVideoRtpPacket_{false};
	std::atomic<bool> loggedFirstVideoPacket_{false};
	std::atomic<bool> loggedFirstDecodedVideoFrame_{false};
	std::atomic<bool> loggedVideoDecodeSubmitFailure_{false};
	std::atomic<bool> loggedFirstAudioPacket_{false};
	std::atomic<bool> loggedFirstDecodedAudioFrame_{false};
	std::atomic<bool> loggedAudioDecodeSubmitFailure_{false};
	std::atomic<bool> remoteAudioMuted_{false};
	std::atomic<bool> remoteVideoMuted_{false};
	bool remoteVideoSuppressedState_ = false; // Guarded by videoCommitStateMutex_.
	std::atomic<bool> remoteMediaVideoMuted_{false};
	std::atomic<bool> remoteDirectorVideoMuted_{false};
	std::atomic<bool> remoteVirtualHangup_{false};
	std::atomic<bool> sourceAudioActive_{false};
	std::atomic<bool> loggedFirstDecodedAlphaFrame_{false};
	std::atomic<bool> loggedAlphaDecodeSubmitFailure_{false};
	std::atomic<bool> loggedAlphaDecodeReceiveFailure_{false};
	std::thread connectionThread_;
	mutable std::mutex childSourceMutex_;
	obs_source_t *browserSource_ = nullptr;
	obs_source_t *nativeReceiverSource_ = nullptr;
	std::string browserSourceName_;
	std::string nativeReceiverSourceName_;
	std::shared_ptr<AsyncCallbackState<VDONinjaSource>> callbackState_;
	std::shared_ptr<rtc::Track> videoTrack_;
	std::shared_ptr<rtc::Track> alphaVideoTrack_;
	std::shared_ptr<rtc::Track> audioTrack_;
	std::string videoTrackPeerUuid_;
	std::string alphaVideoTrackPeerUuid_;
	std::string audioTrackPeerUuid_;
	uint64_t videoTrackPeerGeneration_ = 0;
	uint64_t alphaVideoTrackPeerGeneration_ = 0;
	uint64_t audioTrackPeerGeneration_ = 0;
	struct PendingPeerTrackBundle {
		PeerEventIdentity identity;
		uint64_t order = 0;
		std::shared_ptr<rtc::Track> video;
		std::shared_ptr<rtc::Track> alpha;
		std::shared_ptr<rtc::Track> audio;
	};
	std::unordered_map<std::string, std::unordered_map<uint64_t, PendingPeerTrackBundle>> pendingPeerTrackBundles_;
	uint64_t nextPendingPeerTrackOrder_ = 1;
	std::atomic<bool> peerTrackBundleAdoptionInProgress_{false};
	std::unordered_set<uint8_t> videoRedPayloadTypes_;
	bool childShowing_ = false;
	bool childActive_ = false;
	bool browserSourceConfigApplied_ = false;
	bool nativeReceiverConfigApplied_ = false;
	std::atomic<uint64_t> outputDimensionsPacked_{(uint64_t{1920} << 32) | uint64_t{1080}};
	uint32_t browserSourceWidth_ = 0;
	uint32_t browserSourceHeight_ = 0;
	uint32_t nativeReceiverWidth_ = 0;
	uint32_t nativeReceiverHeight_ = 0;
	std::string browserSourceUrl_;
	SourceSettings nativeReceiverSettings_;
	// Canonical two-track transition order:
	// native state -> primary assembly -> primary decode -> alpha assembly ->
	// alpha decode -> serialized video output -> alpha pairing -> audio decode.
	// RTC and OBS APIs must only be called after releasing the full barrier.
	std::mutex nativeStateMutex_;
	std::mutex trackEventApplyMutex_;
	struct TrackEventPosition {
		uint64_t generation = 0;
		uint64_t revision = 0;
	};
	std::unordered_map<std::string, TrackEventPosition> videoTrackEventPositions_;
	std::unordered_map<std::string, TrackEventPosition> alphaTrackEventPositions_;
	std::unordered_map<std::string, TrackEventPosition> audioTrackEventPositions_;
	struct PeerEventState {
		uint64_t generation = 0;
		uint64_t highestSequence = 0;
		uint64_t lastDataMessageSequence = 0;
		uint64_t lastDataChannelOpenSequence = 0;
		uint64_t seenGenerations = 0;
		bool terminal = false;
	};
	std::unordered_map<std::string, PeerEventState> peerEventStates_; // Guarded by trackEventApplyMutex_.
	struct PeerControlState {
		bool audioMuted = false;
		bool mediaVideoMuted = false;
		bool directorVideoMuted = false;
		bool virtualHangup = false;
	};
	std::unordered_map<std::string, std::unordered_map<uint64_t, PeerControlState>>
	    peerControlStates_; // Guarded by videoCommitStateMutex_.
	std::mutex videoAssemblyMutex_;
	std::mutex videoDecodeMutex_;
	std::mutex alphaAssemblyMutex_;
	std::mutex alphaDecodeMutex_;
	std::mutex alphaPairingMutex_;
	std::mutex videoOutputMutex_;
	std::mutex videoCommitStateMutex_;
	std::mutex audioDecodeMutex_;
	MediaEpochGate mediaEpochGate_;
	std::atomic<uint64_t> outputMediaEpoch_{0};
	NativeVideoCodec nativeVideoCodec_ = NativeVideoCodec::H264;
	std::vector<uint8_t> videoAssemblyBuffer_;
	uint32_t videoAssemblyTimestamp_ = 0;
	bool videoAssemblyActive_ = false;
	AVCodecContext *videoDecoder_ = nullptr;
	AVFrame *videoFrame_ = nullptr;
	AVFrame *videoTransferFrame_ = nullptr;
	AVPacket *videoPacket_ = nullptr;
	SwsContext *videoScaleContext_ = nullptr;
	// Alpha channel VP9 decode state
	std::atomic<bool> loggedFirstAlphaRtpPacket_{false};
	std::vector<uint8_t> alphaAssemblyBuffer_;
	uint32_t alphaAssemblyTimestamp_ = 0;
	bool alphaAssemblyActive_ = false;
	AVCodecContext *alphaDecoder_ = nullptr;
	AVFrame *alphaFrame_ = nullptr;
	AVPacket *alphaPacket_ = nullptr;
	AlphaFrameSynchronizer alphaFrameSynchronizer_;
	RtpOutputTimestampMapper videoTimestampMapper_;
	AVCodecContext *audioDecoder_ = nullptr;
	AVFrame *audioFrame_ = nullptr;
	AVPacket *audioPacket_ = nullptr;
	SwrContext *audioResampleContext_ = nullptr;
	int audioSampleRate_ = 48000;
	int audioChannels_ = 2;
	int audioResampleInputFormat_ = -1;
	int audioResampleInputRate_ = 0;
	int audioResampleInputChannels_ = 0;
	int lastDecodedVideoWidth_ = 0;
	int lastDecodedVideoHeight_ = 0;
	int videoHwPixelFormat_ = -1;
	bool videoHwDecodeConfigured_ = false;
	bool videoHwDecodeDisabled_ = false;
	bool videoHwStatusLogged_ = false;
	std::string videoHwDeviceName_;
	std::atomic<bool> alphaTrackActive_{false};
	std::atomic<bool> preferSoftwareVp9DecodeForAlpha_{false};
	std::atomic<bool> loggedAlphaSoftwareDecodeMode_{false};
	std::atomic<bool> loggedAlphaCompositionActive_{false};
	std::atomic<bool> loggedAlphaTimestampSyncWait_{false};
	std::atomic<bool> loggedAlphaTimestampMiss_{false};
	std::atomic<bool> loggedAlphaPixelFormatMismatch_{false};
	std::atomic<bool> loggedAlphaDimensionMismatch_{false};
	uint32_t audioBaseRtpTimestamp_ = 0;
	uint64_t audioBaseTimestampNs_ = 0;
	uint64_t lastAudioTimestampNs_ = 0;
	bool audioTimingInitialized_ = false;
	std::atomic<int64_t> lastVideoTime_{0};
	std::atomic<int64_t> lastAudioTime_{0};
	std::atomic<int64_t> lastKeyframeRequestTime_{0};
	std::atomic<bool> videoOutputActive_{false};
	std::atomic<bool> loggedVideoStallClear_{false};
#if defined(VDONINJA_NATIVE_MEDIA_INTEGRATION_TEST)
	VDONinjaPeerManager *nativeMediaTestPeerManager_ = nullptr;
	NativeMediaTestStageHook nativeMediaTestStageHook_;
	NativeMediaTestOutputHook nativeMediaTestOutputHook_;
	NativeMediaTestAudioOutputHook nativeMediaTestAudioOutputHook_;
	NativeMediaTestClearOutputHook nativeMediaTestClearOutputHook_;
	NativeMediaTestSendPacketHook nativeMediaTestSendPacketHook_;
	NativeMediaTestReceiveFrameHook nativeMediaTestReceiveFrameHook_;
	NativeMediaTestSendPacketHook nativeMediaTestAlphaSendPacketHook_;
	NativeMediaTestReceiveFrameHook nativeMediaTestAlphaReceiveFrameHook_;
	std::shared_ptr<std::atomic<int>> nativeMediaTestRetainedVideoFrames_ = std::make_shared<std::atomic<int>>(0);
	int nativeMediaTestPrimaryRequestedThreadCount_ = -1;
	int nativeMediaTestPrimaryRequestedThreadType_ = -1;
	int nativeMediaTestAlphaRequestedThreadCount_ = -1;
	int nativeMediaTestAlphaRequestedThreadType_ = -1;
	std::atomic<int> nativeMediaTestRejectedTrackEvents_{0};
	std::atomic<int> nativeMediaTestAcceptedPeerCleanups_{0};
	std::atomic<int> nativeMediaTestPeerRetirements_{0};
	std::atomic<int> nativeMediaTestPeerRetrySchedules_{0};
	std::atomic<int> nativeMediaTestDataChannelOpenActions_{0};
	std::atomic<int> nativeMediaTestAmbiguousSessionlessCleanups_{0};
	std::atomic<int> nativeMediaTestTargetedPeerByes_{0};
	std::atomic<int> nativeMediaTestLegacyStreamRemovalActions_{0};
#endif
	std::mutex retryStateMutex_;
	int viewRetryCount_ = 0;
	int64_t lastViewRequestTimeMs_ = 0;
	int64_t nextViewRetryTimeMs_ = 0;
	bool awaitingPeerConnection_ = false;
	bool suppressViewerRetry_ = false;
	std::string pendingViewRetryReason_;
};

extern obs_source_info vdoninja_source_info;
extern obs_source_info vdoninja_native_source_info;

} // namespace vdoninja
