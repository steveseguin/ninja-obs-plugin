/*
 * OBS VDO.Ninja Plugin
 * Source module for viewing streams from VDO.Ninja
 */

#pragma once

#include <obs-module.h>

#include <atomic>
#include <unordered_set>
#include <thread>

#include "vdoninja-common.h"
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

void vdoninja_source_child_audio_capture(void *param, obs_source_t *source, const struct audio_data *audioData,
                                         bool muted);
void vdoninja_source_child_audio_activate(void *param, calldata_t *calldata);
void vdoninja_source_child_audio_deactivate(void *param, calldata_t *calldata);

class VDONinjaSource
{
public:
	VDONinjaSource(obs_data_t *settings, obs_source_t *source);
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
	obs_source_t *getActiveChildSource() const;

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
	void handleSignalingAlert(const std::string &message);
	void handlePeerCleanupSignal(const std::string &uuid);
	void handlePeerDisconnected(const std::string &uuid);
	void onVideoTrack(const std::string &uuid, std::shared_ptr<rtc::Track> track);
	void onAudioTrack(const std::string &uuid, std::shared_ptr<rtc::Track> track);
	void processVideoRtpPacket(const uint8_t *packetData, size_t packetSize);
	void processVideoData(const uint8_t *data, size_t size, uint32_t rtpTimestamp);
	void processAudioData(const uint8_t *data, size_t size, uint32_t rtpTimestamp);
	bool initializeVideoDecoder();
	bool initializeAudioDecoder(int sampleRate, int channels);
	void resetVideoDecoder();
	void resetAudioDecoder();
	void resetNativeState();
	void outputDecodedVideoFrame(const AVFrame *frame, uint64_t timestampNs);
	void outputDecodedAudioFrame(const AVFrame *frame, uint64_t timestampNs);
	uint64_t mapVideoTimestamp(uint32_t rtpTimestamp);
	uint64_t mapAudioTimestamp(uint32_t rtpTimestamp);
	void ensureNativeReceiverSource();
	void releaseNativeReceiverSource();
	void updateNativeReceiverSource();
	void ensureBrowserSource();
	void releaseBrowserSource();
	void updateBrowserSource();
	void releaseChildSources();
	void updateWrapperChildSource();
	std::string buildViewerUrl() const;
	void onChildAudioCaptured(const struct audio_data *audioData, bool muted);
	void onChildAudioActivated();
	void onChildAudioDeactivated();
	friend void vdoninja_source_child_audio_capture(void *param, obs_source_t *source, const struct audio_data *audioData,
	                                                bool muted);
	friend void vdoninja_source_child_audio_activate(void *param, calldata_t *calldata);
	friend void vdoninja_source_child_audio_deactivate(void *param, calldata_t *calldata);

	obs_source_t *source_ = nullptr;
	SourceSettings settings_;
	bool internalNativeSource_ = false;
	std::unique_ptr<VDONinjaSignaling> signaling_;
	std::unique_ptr<VDONinjaPeerManager> peerManager_;
	std::atomic<bool> active_{false};
	std::atomic<bool> showing_{false};
	std::atomic<bool> connected_{false};
	std::atomic<bool> nativeRunning_{false};
	std::atomic<bool> loggedFirstVideoRtpPacket_{false};
	std::atomic<bool> loggedFirstVideoPacket_{false};
	std::atomic<bool> loggedFirstDecodedVideoFrame_{false};
	std::atomic<bool> loggedFirstAudioPacket_{false};
	std::atomic<bool> loggedFirstDecodedAudioFrame_{false};
	std::thread connectionThread_;
	obs_source_t *browserSource_ = nullptr;
	obs_source_t *nativeReceiverSource_ = nullptr;
	std::string browserSourceName_;
	std::string nativeReceiverSourceName_;
	std::shared_ptr<rtc::Track> videoTrack_;
	std::shared_ptr<rtc::Track> audioTrack_;
	std::string videoTrackPeerUuid_;
	std::string audioTrackPeerUuid_;
	std::unordered_set<uint8_t> videoRedPayloadTypes_;
	uint32_t width_ = 1920;
	uint32_t height_ = 1080;
	std::mutex nativeStateMutex_;
	std::mutex videoAssemblyMutex_;
	std::mutex videoDecodeMutex_;
	std::mutex audioDecodeMutex_;
	std::vector<uint8_t> videoAssemblyBuffer_;
	uint32_t videoAssemblyTimestamp_ = 0;
	bool videoAssemblyActive_ = false;
	AVCodecContext *videoDecoder_ = nullptr;
	AVFrame *videoFrame_ = nullptr;
	AVFrame *videoTransferFrame_ = nullptr;
	AVPacket *videoPacket_ = nullptr;
	SwsContext *videoScaleContext_ = nullptr;
	AVCodecContext *audioDecoder_ = nullptr;
	AVFrame *audioFrame_ = nullptr;
	AVPacket *audioPacket_ = nullptr;
	SwrContext *audioResampleContext_ = nullptr;
	int audioSampleRate_ = 48000;
	int audioChannels_ = 2;
	int audioResampleInputFormat_ = -1;
	int audioResampleInputRate_ = 0;
	int audioResampleInputChannels_ = 0;
	uint32_t videoBaseRtpTimestamp_ = 0;
	uint64_t videoBaseTimestampNs_ = 0;
	uint64_t lastVideoTimestampNs_ = 0;
	bool videoTimingInitialized_ = false;
	int lastDecodedVideoWidth_ = 0;
	int lastDecodedVideoHeight_ = 0;
	int videoHwPixelFormat_ = -1;
	bool videoHwDecodeConfigured_ = false;
	bool videoHwDecodeDisabled_ = false;
	bool videoHwStatusLogged_ = false;
	std::string videoHwDeviceName_;
	uint32_t audioBaseRtpTimestamp_ = 0;
	uint64_t audioBaseTimestampNs_ = 0;
	uint64_t lastAudioTimestampNs_ = 0;
	bool audioTimingInitialized_ = false;
	int64_t lastVideoTime_ = 0;
	int64_t lastAudioTime_ = 0;
	int64_t lastKeyframeRequestTime_ = 0;
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
