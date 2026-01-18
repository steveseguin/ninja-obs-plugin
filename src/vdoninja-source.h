/*
 * OBS VDO.Ninja Plugin
 * Source module for viewing streams from VDO.Ninja
 *
 * This creates an OBS source that can pull video/audio from VDO.Ninja streams,
 * similar to browser sources or media sources.
 */

#pragma once

#include <obs-module.h>

#include <atomic>
#include <deque>
#include <thread>

#include <media-io/video-scaler.h>

#include "vdoninja-common.h"
#include "vdoninja-peer-manager.h"
#include "vdoninja-signaling.h"

namespace vdoninja
{

// Frame buffer for decoded video
struct VideoFrame {
	std::vector<uint8_t> data;
	uint32_t width = 0;
	uint32_t height = 0;
	uint32_t linesize = 0;
	int64_t timestamp = 0;
	video_format format = VIDEO_FORMAT_NONE;
};

// Audio buffer for decoded audio
struct AudioBuffer {
	std::vector<uint8_t> data;
	uint32_t sampleRate = 48000;
	uint32_t channels = 2;
	int64_t timestamp = 0;
};

class VDONinjaSource
{
public:
	VDONinjaSource(obs_data_t *settings, obs_source_t *source);
	~VDONinjaSource();

	// OBS source lifecycle
	void update(obs_data_t *settings);
	void activate();
	void deactivate();

	// Video output
	void videoTick(float seconds);
	void videoRender(gs_effect_t *effect);

	// Audio output
	void audioRender(obs_source_audio *audio);

	// Get dimensions
	uint32_t getWidth() const;
	uint32_t getHeight() const;

	// Connection state
	bool isConnected() const;
	std::string getStreamId() const;

private:
	// Load settings
	void loadSettings(obs_data_t *settings);

	// Connection management
	void connect();
	void disconnect();
	void connectionThread();

	// Media handling
	void onVideoTrack(const std::string &uuid, std::shared_ptr<rtc::Track> track);
	void onAudioTrack(const std::string &uuid, std::shared_ptr<rtc::Track> track);
	void processVideoData(const uint8_t *data, size_t size);
	void processAudioData(const uint8_t *data, size_t size);

	// Frame management
	void pushVideoFrame(const VideoFrame &frame);
	bool popVideoFrame(VideoFrame &frame);
	void pushAudioBuffer(const AudioBuffer &buffer);

	// OBS source handle
	obs_source_t *source_;

	// Settings
	SourceSettings settings_;

	// Components
	std::unique_ptr<VDONinjaSignaling> signaling_;
	std::unique_ptr<VDONinjaPeerManager> peerManager_;

	// State
	std::atomic<bool> active_{false};
	std::atomic<bool> connected_{false};
	std::thread connectionThread_;

	// Video state
	uint32_t width_ = 1920;
	uint32_t height_ = 1080;
	gs_texture_t *texture_ = nullptr;
	video_scaler_t *scaler_ = nullptr;

	// Frame buffers
	std::deque<VideoFrame> videoFrames_;
	std::deque<AudioBuffer> audioBuffers_;
	std::mutex videoMutex_;
	std::mutex audioMutex_;
	static constexpr size_t MAX_VIDEO_FRAMES = 30;
	static constexpr size_t MAX_AUDIO_BUFFERS = 100;

	// Timing
	int64_t lastVideoTime_ = 0;
	int64_t lastAudioTime_ = 0;
};

// OBS source info registration
extern obs_source_info vdoninja_source_info;

} // namespace vdoninja
