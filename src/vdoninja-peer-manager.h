/*
 * OBS VDO.Ninja Plugin
 * Multi-peer connection manager
 *
 * Manages multiple WebRTC peer connections for both publishing (multiple viewers)
 * and viewing (multiple publishers) scenarios.
 */

#pragma once

#include <rtc/rtc.hpp>

#include <functional>
#include <map>
#include <mutex>

#include "vdoninja-common.h"
#include "vdoninja-signaling.h"

namespace vdoninja
{

// Track types
enum class TrackType { Audio, Video };

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
using OnPeerConnectedCallback = std::function<void(const std::string &uuid)>;
using OnPeerDisconnectedCallback = std::function<void(const std::string &uuid)>;
using OnTrackCallback = std::function<void(const std::string &uuid, TrackType type, std::shared_ptr<rtc::Track> track)>;
using OnDataChannelCallback = std::function<void(const std::string &uuid, std::shared_ptr<rtc::DataChannel> dc)>;
using OnDataChannelMessageCallback = std::function<void(const std::string &uuid, const std::string &message)>;

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

	// Send media to all connected peers (viewers)
	void sendAudioFrame(const uint8_t *data, size_t size, uint32_t timestamp);
	void sendVideoFrame(const uint8_t *data, size_t size, uint32_t timestamp, bool keyframe);

	// Viewing mode - receive media from publishers
	bool startViewing(const std::string &streamId);
	void stopViewing(const std::string &streamId);

	// Data channel
	void sendDataToAll(const std::string &message);
	void sendDataToPeer(const std::string &uuid, const std::string &message);

	// Peer events
	void setOnPeerConnected(OnPeerConnectedCallback callback);
	void setOnPeerDisconnected(OnPeerDisconnectedCallback callback);
	void setOnTrack(OnTrackCallback callback);
	void setOnDataChannel(OnDataChannelCallback callback);
	void setOnDataChannelMessage(OnDataChannelMessageCallback callback);

	// Get peer info
	std::vector<std::string> getConnectedPeers() const;
	ConnectionState getPeerState(const std::string &uuid) const;

	// Configuration
	void setVideoCodec(VideoCodec codec);
	void setAudioCodec(AudioCodec codec);
	void setBitrate(int bitrate);
	void setEnableDataChannel(bool enable);

private:
	// Create a new peer connection for a viewer (we send media to them)
	std::shared_ptr<PeerInfo> createPublisherConnection(const std::string &uuid);

	// Create a new peer connection for viewing (we receive media from them)
	std::shared_ptr<PeerInfo> createViewerConnection(const std::string &uuid);

	// Handle signaling events
	void onSignalingOffer(const std::string &uuid, const std::string &sdp, const std::string &session);
	void onSignalingAnswer(const std::string &uuid, const std::string &sdp, const std::string &session);
	void onSignalingIceCandidate(const std::string &uuid, const std::string &candidate, const std::string &mid,
	                             const std::string &session);

	// Setup peer connection callbacks
	void setupPeerConnectionCallbacks(std::shared_ptr<PeerInfo> peer);

	// Setup tracks for publishing
	void setupPublisherTracks(std::shared_ptr<PeerInfo> peer);

	// ICE candidate bundling
	void bundleAndSendCandidates(const std::string &uuid);

	// Get RTC configuration
	rtc::Configuration getRtcConfig() const;

	// Signaling client (not owned)
	VDONinjaSignaling *signaling_ = nullptr;

	// Peer connections
	std::map<std::string, std::shared_ptr<PeerInfo>> peers_;
	mutable std::mutex peersMutex_;

	// ICE configuration
	std::vector<IceServer> iceServers_;
	bool forceTurn_ = false;

	// Publishing state
	std::atomic<bool> publishing_{false};
	int maxViewers_ = 10;

	// Codec and quality settings
	VideoCodec videoCodec_ = VideoCodec::H264;
	AudioCodec audioCodec_ = AudioCodec::Opus;
	int bitrate_ = 4000000;
	bool enableDataChannel_ = true;

	// Audio/Video SSRC for outgoing media
	uint32_t audioSsrc_ = 0;
	uint32_t videoSsrc_ = 0;
	uint16_t audioSeq_ = 0;
	uint16_t videoSeq_ = 0;
	uint32_t audioTimestamp_ = 0;
	uint32_t videoTimestamp_ = 0;

	// ICE candidate bundling
	struct CandidateBundle {
		std::vector<std::tuple<std::string, std::string>> candidates; // (candidate, mid)
		int64_t lastUpdate = 0;
		std::string session;
	};
	std::map<std::string, CandidateBundle> candidateBundles_;
	std::mutex candidateMutex_;

	// Callbacks
	OnPeerConnectedCallback onPeerConnected_;
	OnPeerDisconnectedCallback onPeerDisconnected_;
	OnTrackCallback onTrack_;
	OnDataChannelCallback onDataChannel_;
	OnDataChannelMessageCallback onDataChannelMessage_;
};

} // namespace vdoninja
