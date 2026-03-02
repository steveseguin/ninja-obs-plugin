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

#include "vdoninja-auto-scene-manager.h"
#include "vdoninja-common.h"
#include "vdoninja-data-channel.h"
#include "vdoninja-peer-manager.h"
#include "vdoninja-signaling.h"

namespace vdoninja
{

class VDONinjaOutput
{
public:
	struct ViewerRuntimeSnapshot {
		std::string uuid;
		std::string streamId;
		std::string role;
		std::string state;
		bool hasDataChannel = false;
		std::string lastStats;
		int64_t lastStatsTimestampMs = 0;
	};

	VDONinjaOutput(obs_data_t *settings, obs_output_t *output);
	~VDONinjaOutput();

	// OBS output lifecycle
	bool start();
	void stop();
	void data(encoder_packet *packet);

	// Get statistics
	uint64_t getTotalBytes() const;
	int getConnectTime() const;
	int getViewerCount() const;
	int getMaxViewers() const;
	bool isRunning() const;
	bool isConnected() const;
	int64_t getUptimeMs() const;
	OutputSettings getSettingsSnapshot() const;
	std::vector<ViewerRuntimeSnapshot> getViewerSnapshots() const;

	// Tally aggregation across all peers
	TallyState getAggregatedTally() const;

	// Remote control enabled flag (read from settings)
	bool isRemoteControlEnabled() const;

	// Update settings
	void update(obs_data_t *settings);

private:
	// Initialize from settings
	void loadSettings(obs_data_t *settings);

	// Start/stop thread functions
	void startThread(OutputSettings settingsSnap);
	void stopThread();

	// Handle encoding
	void processAudioPacket(encoder_packet *packet);
	void processVideoPacket(encoder_packet *packet);
	void sendInitialPeerInfo(const std::string &uuid);
	void primeViewerWithCachedKeyframe(const std::string &uuid);
	std::string buildInitialInfoMessage() const;
	std::string buildObsStateMessage() const;
	void sendObsStateToPeer(const std::string &uuid);
	void queueObsStateToPeer(const std::string &uuid);

	// OBS output handle
	obs_output_t *output_;

	// Settings (protected by settingsMutex_)
	OutputSettings settings_;
	mutable std::mutex settingsMutex_;

	// Components
	std::unique_ptr<VDONinjaSignaling> signaling_;
	std::unique_ptr<VDONinjaPeerManager> peerManager_;
	std::unique_ptr<VDOAutoSceneManager> autoSceneManager_;
	VDONinjaDataChannel dataChannel_;

	// State
	std::atomic<bool> running_{false};
	std::atomic<bool> connected_{false};
	std::atomic<bool> capturing_{false};
	std::atomic<bool> stopping_{false};
	std::thread startStopThread_;

	// Statistics
	std::atomic<uint64_t> totalBytes_{0};
	std::atomic<int64_t> connectTimeMs_{0};
	std::atomic<int64_t> startTimeMs_{0};

	// Data-channel telemetry cache (per peer)
	mutable std::mutex telemetryMutex_;
	std::map<std::string, std::string> lastPeerStats_;
	std::map<std::string, int64_t> lastPeerStatsTimestampMs_;

	// Latest keyframe cache for fast viewer warm-up and keyframe requests.
	mutable std::mutex keyframeCacheMutex_;
	std::vector<uint8_t> cachedKeyframe_;
	uint32_t cachedKeyframeTimestamp_ = 0;
	uint32_t lastVideoRtpTimestamp_ = 0;
	bool hasLastVideoRtpTimestamp_ = false;
	uint32_t lastAudioRtpTimestamp_ = 0;
	bool hasLastAudioRtpTimestamp_ = false;

	// OBS can provide multiple encoded audio tracks. VDO.Ninja publish uses one
	// Opus stream, so we forward exactly one selected track index.
	size_t selectedAudioTrackIdx_ = 0;
	std::atomic<uint64_t> droppedAudioPacketsOtherTracks_{0};
};

// OBS output info registration
extern obs_output_info vdoninja_output_info;

} // namespace vdoninja
