/*
 * OBS VDO.Ninja Plugin
 * Output module for publishing streams to VDO.Ninja
 *
 * This creates an OBS output that can be used as a streaming destination,
 * similar to RTMP or WHIP outputs.
 */

#pragma once

#include <obs-module.h>

#include <atomic>
#include <thread>

#include "vdoninja-common.h"
#include "vdoninja-peer-manager.h"
#include "vdoninja-signaling.h"

namespace vdoninja
{

class VDONinjaOutput
{
public:
	VDONinjaOutput(obs_data_t *settings, obs_output_t *output);
	~VDONinjaOutput();

	// OBS output lifecycle
	bool start();
	void stop(bool signal = true);
	void data(encoder_packet *packet);

	// Get statistics
	uint64_t getTotalBytes() const;
	int getConnectTime() const;
	int getViewerCount() const;

	// Update settings
	void update(obs_data_t *settings);

private:
	// Initialize from settings
	void loadSettings(obs_data_t *settings);

	// Start/stop thread functions
	void startThread();
	void stopThread();

	// Handle encoding
	void processAudioPacket(encoder_packet *packet);
	void processVideoPacket(encoder_packet *packet);

	// OBS output handle
	obs_output_t *output_;

	// Settings
	OutputSettings settings_;

	// Components
	std::unique_ptr<VDONinjaSignaling> signaling_;
	std::unique_ptr<VDONinjaPeerManager> peerManager_;

	// State
	std::atomic<bool> running_{false};
	std::atomic<bool> connected_{false};
	std::thread startStopThread_;

	// Statistics
	std::atomic<uint64_t> totalBytes_{0};
	int64_t connectTimeMs_ = 0;
	int64_t startTimeMs_ = 0;

	// Encoder info
	video_t *video_ = nullptr;
	audio_t *audio_ = nullptr;
	const char *videoCodecName_ = nullptr;
	const char *audioCodecName_ = nullptr;
};

// OBS output info registration
extern obs_output_info vdoninja_output_info;

} // namespace vdoninja
