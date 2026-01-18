/*
 * OBS VDO.Ninja Plugin
 * Multi-peer connection manager implementation
 */

#include "vdoninja-peer-manager.h"

#include <random>

namespace vdoninja
{

VDONinjaPeerManager::VDONinjaPeerManager()
{
	// Generate random SSRCs for audio/video
	std::random_device rd;
	std::mt19937 gen(rd());
	std::uniform_int_distribution<uint32_t> dis(1, 0xFFFFFFFF);

	audioSsrc_ = dis(gen);
	videoSsrc_ = dis(gen);

	logInfo("Peer manager created with audio SSRC: %u, video SSRC: %u", audioSsrc_, videoSsrc_);
}

VDONinjaPeerManager::~VDONinjaPeerManager()
{
	stopPublishing();

	// Close all peer connections
	std::lock_guard<std::mutex> lock(peersMutex_);
	for (auto &pair : peers_) {
		if (pair.second->pc) {
			pair.second->pc->close();
		}
	}
	peers_.clear();
}

void VDONinjaPeerManager::initialize(VDONinjaSignaling *signaling)
{
	signaling_ = signaling;

	// Set up signaling callbacks
	signaling_->setOnOffer([this](const std::string &uuid, const std::string &sdp, const std::string &session) {
		onSignalingOffer(uuid, sdp, session);
	});

	signaling_->setOnAnswer([this](const std::string &uuid, const std::string &sdp, const std::string &session) {
		onSignalingAnswer(uuid, sdp, session);
	});

	signaling_->setOnIceCandidate(
	    [this](const std::string &uuid, const std::string &candidate, const std::string &mid,
	           const std::string &session) { onSignalingIceCandidate(uuid, candidate, mid, session); });

	logInfo("Peer manager initialized with signaling client");
}

void VDONinjaPeerManager::setIceServers(const std::vector<IceServer> &servers)
{
	iceServers_ = servers;
}

void VDONinjaPeerManager::setForceTurn(bool force)
{
	forceTurn_ = force;
}

rtc::Configuration VDONinjaPeerManager::getRtcConfig() const
{
	rtc::Configuration config;

	// Add STUN servers
	for (const auto &stun : DEFAULT_STUN_SERVERS) {
		config.iceServers.push_back({stun});
	}

	// Add custom ICE servers
	for (const auto &server : iceServers_) {
		rtc::IceServer iceServer;
		iceServer.hostname = server.urls;
		if (!server.username.empty()) {
			iceServer.username = server.username;
			iceServer.password = server.credential;
		}
		config.iceServers.push_back(iceServer);
	}

	if (forceTurn_) {
		config.iceTransportPolicy = rtc::TransportPolicy::Relay;
	}

	return config;
}

bool VDONinjaPeerManager::startPublishing(int maxViewers)
{
	if (publishing_) {
		logWarning("Already publishing");
		return true;
	}

	maxViewers_ = maxViewers;
	publishing_ = true;

	logInfo("Started publishing, max viewers: %d", maxViewers);
	return true;
}

void VDONinjaPeerManager::stopPublishing()
{
	if (!publishing_)
		return;

	publishing_ = false;

	// Close all viewer connections
	std::lock_guard<std::mutex> lock(peersMutex_);
	auto it = peers_.begin();
	while (it != peers_.end()) {
		if (it->second->type == ConnectionType::Publisher) {
			if (it->second->pc) {
				it->second->pc->close();
			}
			it = peers_.erase(it);
		} else {
			++it;
		}
	}

	logInfo("Stopped publishing");
}

bool VDONinjaPeerManager::isPublishing() const
{
	return publishing_;
}

int VDONinjaPeerManager::getViewerCount() const
{
	std::lock_guard<std::mutex> lock(peersMutex_);
	int count = 0;
	for (const auto &pair : peers_) {
		if (pair.second->type == ConnectionType::Publisher && pair.second->state == ConnectionState::Connected) {
			count++;
		}
	}
	return count;
}

std::shared_ptr<PeerInfo> VDONinjaPeerManager::createPublisherConnection(const std::string &uuid)
{
	auto config = getRtcConfig();
	auto pc = std::make_shared<rtc::PeerConnection>(config);

	auto peer = std::make_shared<PeerInfo>();
	peer->uuid = uuid;
	peer->type = ConnectionType::Publisher;
	peer->session = generateSessionId();
	peer->pc = pc;

	setupPeerConnectionCallbacks(peer);
	setupPublisherTracks(peer);

	{
		std::lock_guard<std::mutex> lock(peersMutex_);
		peers_[uuid] = peer;
	}

	logInfo("Created publisher connection for viewer: %s", uuid.c_str());
	return peer;
}

std::shared_ptr<PeerInfo> VDONinjaPeerManager::createViewerConnection(const std::string &uuid)
{
	auto config = getRtcConfig();
	auto pc = std::make_shared<rtc::PeerConnection>(config);

	auto peer = std::make_shared<PeerInfo>();
	peer->uuid = uuid;
	peer->type = ConnectionType::Viewer;
	peer->session = generateSessionId();
	peer->pc = pc;

	setupPeerConnectionCallbacks(peer);

	{
		std::lock_guard<std::mutex> lock(peersMutex_);
		peers_[uuid] = peer;
	}

	logInfo("Created viewer connection for publisher: %s", uuid.c_str());
	return peer;
}

void VDONinjaPeerManager::setupPeerConnectionCallbacks(std::shared_ptr<PeerInfo> peer)
{
	auto weakPeer = std::weak_ptr<PeerInfo>(peer);
	std::string uuid = peer->uuid;

	peer->pc->onStateChange([this, weakPeer, uuid](rtc::PeerConnection::State state) {
		auto peer = weakPeer.lock();
		if (!peer)
			return;

		switch (state) {
		case rtc::PeerConnection::State::New:
			peer->state = ConnectionState::New;
			break;
		case rtc::PeerConnection::State::Connecting:
			peer->state = ConnectionState::Connecting;
			logInfo("Peer %s connecting", uuid.c_str());
			break;
		case rtc::PeerConnection::State::Connected:
			peer->state = ConnectionState::Connected;
			logInfo("Peer %s connected", uuid.c_str());
			if (onPeerConnected_) {
				onPeerConnected_(uuid);
			}
			break;
		case rtc::PeerConnection::State::Disconnected:
			peer->state = ConnectionState::Disconnected;
			logInfo("Peer %s disconnected", uuid.c_str());
			if (onPeerDisconnected_) {
				onPeerDisconnected_(uuid);
			}
			break;
		case rtc::PeerConnection::State::Failed:
			peer->state = ConnectionState::Failed;
			logError("Peer %s connection failed", uuid.c_str());
			if (onPeerDisconnected_) {
				onPeerDisconnected_(uuid);
			}
			break;
		case rtc::PeerConnection::State::Closed:
			peer->state = ConnectionState::Closed;
			logInfo("Peer %s closed", uuid.c_str());
			break;
		}
	});

	peer->pc->onLocalCandidate([this, weakPeer, uuid](rtc::Candidate candidate) {
		auto peer = weakPeer.lock();
		if (!peer)
			return;

		// Bundle candidates before sending
		std::lock_guard<std::mutex> lock(candidateMutex_);
		auto &bundle = candidateBundles_[uuid];
		bundle.candidates.push_back({std::string(candidate), candidate.mid()});
		bundle.lastUpdate = currentTimeMs();
		bundle.session = peer->session;

		// Schedule sending after delay
		// In a real implementation, use a timer. Here we send immediately if enough time passed.
		if (bundle.candidates.size() >= 5) {
			bundleAndSendCandidates(uuid);
		}
	});

	peer->pc->onGatheringStateChange([this, uuid](rtc::PeerConnection::GatheringState state) {
		if (state == rtc::PeerConnection::GatheringState::Complete) {
			logInfo("ICE gathering complete for %s", uuid.c_str());
			bundleAndSendCandidates(uuid);
		}
	});

	peer->pc->onTrack([this, weakPeer, uuid](std::shared_ptr<rtc::Track> track) {
		auto peer = weakPeer.lock();
		if (!peer)
			return;

		// Determine track type from description
		auto desc = track->description();
		TrackType type = TrackType::Video;
		if (desc.find("audio") != std::string::npos) {
			type = TrackType::Audio;
		}

		logInfo("Received %s track from %s", type == TrackType::Audio ? "audio" : "video", uuid.c_str());

		if (onTrack_) {
			onTrack_(uuid, type, track);
		}
	});

	peer->pc->onDataChannel([this, weakPeer, uuid](std::shared_ptr<rtc::DataChannel> dc) {
		auto peer = weakPeer.lock();
		if (!peer)
			return;

		peer->dataChannel = dc;
		peer->hasDataChannel = true;

		dc->onMessage([this, uuid](auto data) {
			if (std::holds_alternative<std::string>(data)) {
				if (onDataChannelMessage_) {
					onDataChannelMessage_(uuid, std::get<std::string>(data));
				}
			}
		});

		logInfo("Data channel opened with %s", uuid.c_str());

		if (onDataChannel_) {
			onDataChannel_(uuid, dc);
		}
	});
}

void VDONinjaPeerManager::setupPublisherTracks(std::shared_ptr<PeerInfo> peer)
{
	// Set up video track
	rtc::Description::Video videoDesc("video", rtc::Description::Direction::SendOnly);

	// Configure based on selected codec
	switch (videoCodec_) {
	case VideoCodec::H264:
		videoDesc.addH264Codec(96);
		break;
	case VideoCodec::VP8:
		videoDesc.addVP8Codec(96);
		break;
	case VideoCodec::VP9:
		videoDesc.addVP9Codec(96);
		break;
	case VideoCodec::AV1:
		// AV1 support depends on libdatachannel version
		videoDesc.addH264Codec(96); // Fallback
		break;
	}

	videoDesc.addSSRC(videoSsrc_, "video-stream");
	auto videoTrack = peer->pc->addTrack(videoDesc);

	// Set up audio track
	rtc::Description::Audio audioDesc("audio", rtc::Description::Direction::SendOnly);
	audioDesc.addOpusCodec(111);
	audioDesc.addSSRC(audioSsrc_, "audio-stream");
	auto audioTrack = peer->pc->addTrack(audioDesc);

	// Create data channel if enabled
	if (enableDataChannel_) {
		auto dc = peer->pc->createDataChannel("vdo-data");
		peer->dataChannel = dc;
		peer->hasDataChannel = true;

		dc->onOpen([this, uuid = peer->uuid]() { logInfo("Data channel opened for %s", uuid.c_str()); });

		dc->onMessage([this, uuid = peer->uuid](auto data) {
			if (std::holds_alternative<std::string>(data)) {
				if (onDataChannelMessage_) {
					onDataChannelMessage_(uuid, std::get<std::string>(data));
				}
			}
		});
	}

	logDebug("Set up publisher tracks for %s", peer->uuid.c_str());
}

void VDONinjaPeerManager::onSignalingOffer(const std::string &uuid, const std::string &sdp, const std::string &session)
{
	// We received an offer - this happens when we're viewing a stream
	std::shared_ptr<PeerInfo> peer;

	{
		std::lock_guard<std::mutex> lock(peersMutex_);
		auto it = peers_.find(uuid);
		if (it != peers_.end()) {
			peer = it->second;
			// Verify session matches
			if (!peer->session.empty() && peer->session != session) {
				logWarning("Session mismatch for %s, ignoring offer", uuid.c_str());
				return;
			}
		}
	}

	if (!peer) {
		peer = createViewerConnection(uuid);
	}

	peer->session = session;

	// Set remote description (the offer)
	peer->pc->setRemoteDescription(rtc::Description(sdp, rtc::Description::Type::Offer));

	// Create and send answer
	peer->pc->setLocalDescription(rtc::Description::Type::Answer);

	auto localDesc = peer->pc->localDescription();
	if (localDesc) {
		signaling_->sendAnswer(uuid, std::string(*localDesc), session);
		logInfo("Sent answer to %s", uuid.c_str());
	}
}

void VDONinjaPeerManager::onSignalingAnswer(const std::string &uuid, const std::string &sdp, const std::string &session)
{
	// We received an answer - this happens when we're publishing and a viewer connected
	std::shared_ptr<PeerInfo> peer;

	{
		std::lock_guard<std::mutex> lock(peersMutex_);
		auto it = peers_.find(uuid);
		if (it == peers_.end()) {
			logWarning("Received answer for unknown peer: %s", uuid.c_str());
			return;
		}
		peer = it->second;
	}

	// Verify session
	if (!peer->session.empty() && peer->session != session) {
		logWarning("Session mismatch for %s, ignoring answer", uuid.c_str());
		return;
	}

	// Set remote description (the answer)
	peer->pc->setRemoteDescription(rtc::Description(sdp, rtc::Description::Type::Answer));
	logInfo("Set remote answer for %s", uuid.c_str());
}

void VDONinjaPeerManager::onSignalingIceCandidate(const std::string &uuid, const std::string &candidate,
                                                  const std::string &mid, const std::string &session)
{
	std::shared_ptr<PeerInfo> peer;

	{
		std::lock_guard<std::mutex> lock(peersMutex_);
		auto it = peers_.find(uuid);
		if (it == peers_.end()) {
			logWarning("Received ICE candidate for unknown peer: %s", uuid.c_str());
			return;
		}
		peer = it->second;
	}

	// Verify session
	if (!peer->session.empty() && peer->session != session) {
		logDebug("Session mismatch for ICE candidate from %s", uuid.c_str());
		return;
	}

	// Add remote candidate
	peer->pc->addRemoteCandidate(rtc::Candidate(candidate, mid));
	logDebug("Added ICE candidate from %s", uuid.c_str());
}

void VDONinjaPeerManager::bundleAndSendCandidates(const std::string &uuid)
{
	CandidateBundle bundle;

	{
		std::lock_guard<std::mutex> lock(candidateMutex_);
		auto it = candidateBundles_.find(uuid);
		if (it == candidateBundles_.end() || it->second.candidates.empty()) {
			return;
		}
		bundle = std::move(it->second);
		candidateBundles_.erase(it);
	}

	// Send all bundled candidates
	for (const auto &cand : bundle.candidates) {
		signaling_->sendIceCandidate(uuid, std::get<0>(cand), std::get<1>(cand), bundle.session);
	}

	logDebug("Sent %zu bundled ICE candidates to %s", bundle.candidates.size(), uuid.c_str());
}

void VDONinjaPeerManager::sendAudioFrame(const uint8_t *data, size_t size, uint32_t timestamp)
{
	if (!publishing_)
		return;

	std::lock_guard<std::mutex> lock(peersMutex_);

	for (auto &pair : peers_) {
		auto &peer = pair.second;
		if (peer->type != ConnectionType::Publisher || peer->state != ConnectionState::Connected) {
			continue;
		}

		// Get the audio track and send data
		// The actual RTP packetization would be more complex
		// This is a simplified version
		try {
			auto tracks = peer->pc->tracks();
			for (auto &track : tracks) {
				auto desc = track->description();
				if (desc.find("audio") != std::string::npos) {
					// Create RTP packet (simplified)
					std::vector<uint8_t> rtpPacket;
					rtpPacket.reserve(12 + size);

					// RTP header
					rtpPacket.push_back(0x80); // V=2, P=0, X=0, CC=0
					rtpPacket.push_back(111);  // PT=111 (Opus), M=0
					rtpPacket.push_back((audioSeq_ >> 8) & 0xFF);
					rtpPacket.push_back(audioSeq_ & 0xFF);
					audioSeq_++;

					// Timestamp
					uint32_t ts = timestamp ? timestamp : audioTimestamp_;
					rtpPacket.push_back((ts >> 24) & 0xFF);
					rtpPacket.push_back((ts >> 16) & 0xFF);
					rtpPacket.push_back((ts >> 8) & 0xFF);
					rtpPacket.push_back(ts & 0xFF);
					audioTimestamp_ = ts + 960; // 48kHz, 20ms frames

					// SSRC
					rtpPacket.push_back((audioSsrc_ >> 24) & 0xFF);
					rtpPacket.push_back((audioSsrc_ >> 16) & 0xFF);
					rtpPacket.push_back((audioSsrc_ >> 8) & 0xFF);
					rtpPacket.push_back(audioSsrc_ & 0xFF);

					// Payload
					rtpPacket.insert(rtpPacket.end(), data, data + size);

					track->send(reinterpret_cast<const std::byte *>(rtpPacket.data()), rtpPacket.size());
					break;
				}
			}
		} catch (const std::exception &e) {
			logError("Failed to send audio to %s: %s", pair.first.c_str(), e.what());
		}
	}
}

void VDONinjaPeerManager::sendVideoFrame(const uint8_t *data, size_t size, uint32_t timestamp, bool keyframe)
{
	if (!publishing_)
		return;

	std::lock_guard<std::mutex> lock(peersMutex_);

	for (auto &pair : peers_) {
		auto &peer = pair.second;
		if (peer->type != ConnectionType::Publisher || peer->state != ConnectionState::Connected) {
			continue;
		}

		try {
			auto tracks = peer->pc->tracks();
			for (auto &track : tracks) {
				auto desc = track->description();
				if (desc.find("video") != std::string::npos) {
					// Create RTP packet (simplified - real impl needs fragmentation for large
					// frames)
					std::vector<uint8_t> rtpPacket;
					rtpPacket.reserve(12 + size);

					// RTP header
					rtpPacket.push_back(0x80);                        // V=2, P=0, X=0, CC=0
					rtpPacket.push_back(keyframe ? (96 | 0x80) : 96); // PT=96, M=1 for keyframe
					rtpPacket.push_back((videoSeq_ >> 8) & 0xFF);
					rtpPacket.push_back(videoSeq_ & 0xFF);
					videoSeq_++;

					// Timestamp
					uint32_t ts = timestamp ? timestamp : videoTimestamp_;
					rtpPacket.push_back((ts >> 24) & 0xFF);
					rtpPacket.push_back((ts >> 16) & 0xFF);
					rtpPacket.push_back((ts >> 8) & 0xFF);
					rtpPacket.push_back(ts & 0xFF);
					videoTimestamp_ = ts + 3000; // 90kHz clock, ~30fps

					// SSRC
					rtpPacket.push_back((videoSsrc_ >> 24) & 0xFF);
					rtpPacket.push_back((videoSsrc_ >> 16) & 0xFF);
					rtpPacket.push_back((videoSsrc_ >> 8) & 0xFF);
					rtpPacket.push_back(videoSsrc_ & 0xFF);

					// Payload
					rtpPacket.insert(rtpPacket.end(), data, data + size);

					track->send(reinterpret_cast<const std::byte *>(rtpPacket.data()), rtpPacket.size());
					break;
				}
			}
		} catch (const std::exception &e) {
			logError("Failed to send video to %s: %s", pair.first.c_str(), e.what());
		}
	}
}

bool VDONinjaPeerManager::startViewing(const std::string &streamId)
{
	// Request to view stream through signaling
	// The peer connection will be created when we receive an offer
	logInfo("Started viewing stream: %s", streamId.c_str());
	return true;
}

void VDONinjaPeerManager::stopViewing(const std::string &streamId)
{
	// Find and close connections associated with this stream
	std::lock_guard<std::mutex> lock(peersMutex_);
	auto it = peers_.begin();
	while (it != peers_.end()) {
		if (it->second->type == ConnectionType::Viewer && it->second->streamId == streamId) {
			if (it->second->pc) {
				it->second->pc->close();
			}
			it = peers_.erase(it);
		} else {
			++it;
		}
	}
	logInfo("Stopped viewing stream: %s", streamId.c_str());
}

void VDONinjaPeerManager::sendDataToAll(const std::string &message)
{
	std::lock_guard<std::mutex> lock(peersMutex_);
	for (auto &pair : peers_) {
		if (pair.second->hasDataChannel && pair.second->dataChannel) {
			try {
				pair.second->dataChannel->send(message);
			} catch (const std::exception &e) {
				logError("Failed to send data to %s: %s", pair.first.c_str(), e.what());
			}
		}
	}
}

void VDONinjaPeerManager::sendDataToPeer(const std::string &uuid, const std::string &message)
{
	std::lock_guard<std::mutex> lock(peersMutex_);
	auto it = peers_.find(uuid);
	if (it != peers_.end() && it->second->hasDataChannel && it->second->dataChannel) {
		try {
			it->second->dataChannel->send(message);
		} catch (const std::exception &e) {
			logError("Failed to send data to %s: %s", uuid.c_str(), e.what());
		}
	}
}

void VDONinjaPeerManager::setOnPeerConnected(OnPeerConnectedCallback callback)
{
	onPeerConnected_ = callback;
}
void VDONinjaPeerManager::setOnPeerDisconnected(OnPeerDisconnectedCallback callback)
{
	onPeerDisconnected_ = callback;
}
void VDONinjaPeerManager::setOnTrack(OnTrackCallback callback)
{
	onTrack_ = callback;
}
void VDONinjaPeerManager::setOnDataChannel(OnDataChannelCallback callback)
{
	onDataChannel_ = callback;
}
void VDONinjaPeerManager::setOnDataChannelMessage(OnDataChannelMessageCallback callback)
{
	onDataChannelMessage_ = callback;
}

std::vector<std::string> VDONinjaPeerManager::getConnectedPeers() const
{
	std::vector<std::string> result;
	std::lock_guard<std::mutex> lock(peersMutex_);
	for (const auto &pair : peers_) {
		if (pair.second->state == ConnectionState::Connected) {
			result.push_back(pair.first);
		}
	}
	return result;
}

ConnectionState VDONinjaPeerManager::getPeerState(const std::string &uuid) const
{
	std::lock_guard<std::mutex> lock(peersMutex_);
	auto it = peers_.find(uuid);
	if (it != peers_.end()) {
		return it->second->state;
	}
	return ConnectionState::Closed;
}

void VDONinjaPeerManager::setVideoCodec(VideoCodec codec)
{
	videoCodec_ = codec;
}
void VDONinjaPeerManager::setAudioCodec(AudioCodec codec)
{
	audioCodec_ = codec;
}
void VDONinjaPeerManager::setBitrate(int bitrate)
{
	bitrate_ = bitrate;
}
void VDONinjaPeerManager::setEnableDataChannel(bool enable)
{
	enableDataChannel_ = enable;
}

} // namespace vdoninja
