/*
 * OBS VDO.Ninja Plugin
 * Multi-peer connection manager implementation
 */

#include "vdoninja-peer-manager.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <random>

namespace vdoninja
{

namespace
{

constexpr uint8_t kH264PayloadType = 96;
constexpr uint8_t kH264FuAType = 28;
constexpr size_t kMaxRtpPayloadSize = 1200;

struct NalUnitView {
	const uint8_t *data = nullptr;
	size_t size = 0;
};

bool hasStartCodeAt(const uint8_t *data, size_t size, size_t pos, size_t &length)
{
	length = 0;
	if (!data || pos >= size) {
		return false;
	}

	if (pos + 3 <= size && data[pos] == 0x00 && data[pos + 1] == 0x00 && data[pos + 2] == 0x01) {
		length = 3;
		return true;
	}

	if (pos + 4 <= size && data[pos] == 0x00 && data[pos + 1] == 0x00 && data[pos + 2] == 0x00 &&
	    data[pos + 3] == 0x01) {
		length = 4;
		return true;
	}

	return false;
}

size_t findStartCode(const uint8_t *data, size_t size, size_t from, size_t &length)
{
	length = 0;
	if (!data || from >= size) {
		return size;
	}

	for (size_t pos = from; pos < size; ++pos) {
		if (hasStartCodeAt(data, size, pos, length)) {
			return pos;
		}
	}

	return size;
}

bool parseAnnexBNalus(const uint8_t *data, size_t size, std::vector<NalUnitView> &nalUnits)
{
	size_t startCodeLen = 0;
	size_t start = findStartCode(data, size, 0, startCodeLen);
	if (start == size) {
		return false;
	}

	while (start < size) {
		const size_t nalStart = start + startCodeLen;
		size_t nextStartCodeLen = 0;
		const size_t nextStart = findStartCode(data, size, nalStart, nextStartCodeLen);
		size_t nalEnd = nextStart;

		// Trim alignment zeros before the next start code.
		while (nalEnd > nalStart && data[nalEnd - 1] == 0x00) {
			--nalEnd;
		}

		if (nalEnd > nalStart) {
			nalUnits.push_back({data + nalStart, nalEnd - nalStart});
		}

		if (nextStart == size) {
			break;
		}

		start = nextStart;
		startCodeLen = nextStartCodeLen;
	}

	return !nalUnits.empty();
}

bool parseAvccNalus(const uint8_t *data, size_t size, std::vector<NalUnitView> &nalUnits)
{
	if (!data || size < 4) {
		return false;
	}

	size_t offset = 0;
	while (offset + 4 <= size) {
		const uint32_t nalSize =
		    (static_cast<uint32_t>(data[offset]) << 24) | (static_cast<uint32_t>(data[offset + 1]) << 16) |
		    (static_cast<uint32_t>(data[offset + 2]) << 8) | static_cast<uint32_t>(data[offset + 3]);
		offset += 4;

		if (nalSize == 0) {
			continue;
		}
		if (offset + nalSize > size) {
			return false;
		}

		nalUnits.push_back({data + offset, nalSize});
		offset += nalSize;
	}

	return offset == size && !nalUnits.empty();
}

bool extractH264Nalus(const uint8_t *data, size_t size, std::vector<NalUnitView> &nalUnits)
{
	nalUnits.clear();
	if (!data || size == 0) {
		return false;
	}

	if (parseAnnexBNalus(data, size, nalUnits)) {
		return true;
	}

	if (parseAvccNalus(data, size, nalUnits)) {
		return true;
	}

	// Fallback: treat as a single NAL payload.
	nalUnits.push_back({data, size});
	return true;
}

bool sendRtpPacket(const std::shared_ptr<rtc::Track> &track, uint16_t &sequence, uint32_t timestamp, uint32_t ssrc,
                   bool marker, const uint8_t *payload, size_t payloadSize)
{
	if (!track || !payload || payloadSize == 0) {
		return false;
	}

	rtc::binary packet;
	packet.reserve(12 + payloadSize);
	packet.push_back(static_cast<std::byte>(0x80)); // V=2, P=0, X=0, CC=0
	packet.push_back(static_cast<std::byte>(kH264PayloadType | (marker ? 0x80 : 0x00)));
	packet.push_back(static_cast<std::byte>((sequence >> 8) & 0xFF));
	packet.push_back(static_cast<std::byte>(sequence & 0xFF));
	sequence++;
	packet.push_back(static_cast<std::byte>((timestamp >> 24) & 0xFF));
	packet.push_back(static_cast<std::byte>((timestamp >> 16) & 0xFF));
	packet.push_back(static_cast<std::byte>((timestamp >> 8) & 0xFF));
	packet.push_back(static_cast<std::byte>(timestamp & 0xFF));
	packet.push_back(static_cast<std::byte>((ssrc >> 24) & 0xFF));
	packet.push_back(static_cast<std::byte>((ssrc >> 16) & 0xFF));
	packet.push_back(static_cast<std::byte>((ssrc >> 8) & 0xFF));
	packet.push_back(static_cast<std::byte>(ssrc & 0xFF));
	packet.insert(packet.end(), reinterpret_cast<const std::byte *>(payload),
	              reinterpret_cast<const std::byte *>(payload + payloadSize));
	track->send(std::move(packet));
	return true;
}

bool sendH264FrameRtp(const std::shared_ptr<rtc::Track> &track, uint16_t &sequence, uint32_t timestamp, uint32_t ssrc,
                      const uint8_t *data, size_t size)
{
	std::vector<NalUnitView> nalUnits;
	if (!extractH264Nalus(data, size, nalUnits)) {
		return false;
	}

	for (size_t i = 0; i < nalUnits.size(); ++i) {
		const NalUnitView &nal = nalUnits[i];
		if (!nal.data || nal.size == 0) {
			continue;
		}

		const bool lastNalInFrame = (i + 1 == nalUnits.size());
		if (nal.size <= kMaxRtpPayloadSize) {
			if (!sendRtpPacket(track, sequence, timestamp, ssrc, lastNalInFrame, nal.data, nal.size)) {
				return false;
			}
			continue;
		}

		// FU-A fragmentation for oversized NAL units.
		if (nal.size <= 1) {
			continue;
		}

		const uint8_t nalHeader = nal.data[0];
		const uint8_t fuIndicator = static_cast<uint8_t>((nalHeader & 0xE0) | kH264FuAType);
		const uint8_t nalType = static_cast<uint8_t>(nalHeader & 0x1F);
		const size_t maxChunk = kMaxRtpPayloadSize - 2;
		size_t offset = 1;

		while (offset < nal.size) {
			const size_t remaining = nal.size - offset;
			const size_t chunk = std::min(remaining, maxChunk);
			const bool start = (offset == 1);
			const bool end = (offset + chunk >= nal.size);
			const bool marker = end && lastNalInFrame;

			std::vector<uint8_t> payload;
			payload.reserve(2 + chunk);
			payload.push_back(fuIndicator);
			payload.push_back(static_cast<uint8_t>(nalType | (start ? 0x80 : 0x00) | (end ? 0x40 : 0x00)));
			payload.insert(payload.end(), nal.data + offset, nal.data + offset + chunk);

			if (!sendRtpPacket(track, sequence, timestamp, ssrc, marker, payload.data(), payload.size())) {
				return false;
			}

			offset += chunk;
		}
	}

	return true;
}

} // namespace

VDONinjaPeerManager::VDONinjaPeerManager()
{
	// Generate random SSRCs for audio/video
	std::random_device rd;
	std::mt19937 gen(rd());
	std::uniform_int_distribution<uint32_t> dis(1, 0xFFFFFFFF);

	audioSsrc_ = dis(gen);
	videoSsrc_ = dis(gen);
	while (videoSsrc_ == audioSsrc_) {
		videoSsrc_ = dis(gen);
	}

	logInfo("Peer manager created with audio SSRC: %u, video SSRC: %u", audioSsrc_, videoSsrc_);
}

VDONinjaPeerManager::~VDONinjaPeerManager()
{
	shuttingDown_ = true;
	stopPublishing();

	// Clear signaling callbacks that capture `this`
	if (signaling_) {
		signaling_->setOnOffer(nullptr);
		signaling_->setOnAnswer(nullptr);
		signaling_->setOnOfferRequest(nullptr);
		signaling_->setOnIceCandidate(nullptr);
	}

	// Clear own callbacks
	{
		std::lock_guard<std::mutex> callbackLock(callbackMutex_);
		onPeerConnected_ = nullptr;
		onPeerDisconnected_ = nullptr;
		onTrack_ = nullptr;
		onDataChannel_ = nullptr;
		onDataChannelMessage_ = nullptr;
	}

	// Close all peer connections
	std::lock_guard<std::mutex> lock(peersMutex_);
	for (auto &pair : peers_) {
		if (pair.second->pc) {
			clearPeerCallbacks(pair.second);
			pair.second->pc.reset();
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

	signaling_->setOnOfferRequest(
	    [this](const std::string &uuid, const std::string &session) { onSignalingOfferRequest(uuid, session); });

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
	bool hasTurnServer = false;

	auto hasTurnScheme = [](const std::string &url) {
		if (url.size() < 5) {
			return false;
		}
		std::string lower = url;
		std::transform(lower.begin(), lower.end(), lower.begin(),
		               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		return lower.rfind("turn:", 0) == 0 || lower.rfind("turns:", 0) == 0;
	};

	// If custom servers are set, use only those; otherwise use built-in defaults.
	if (iceServers_.empty()) {
		for (const auto &stun : DEFAULT_STUN_SERVERS) {
			config.iceServers.push_back({stun});
			if (hasTurnScheme(stun)) {
				hasTurnServer = true;
			}
		}
	} else {
		for (const auto &server : iceServers_) {
			rtc::IceServer iceServer(server.urls, "");
			if (!server.username.empty()) {
				iceServer.username = server.username;
				iceServer.password = server.credential;
			}
			config.iceServers.push_back(iceServer);
			if (hasTurnScheme(server.urls)) {
				hasTurnServer = true;
			}
		}
	}

	if (forceTurn_) {
		config.iceTransportPolicy = rtc::TransportPolicy::Relay;
		if (!hasTurnServer) {
			logWarning("Force TURN is enabled but no TURN servers are configured; connections may fail.");
		}
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

	// Collect peers to close outside the lock to avoid deadlock:
	// pc->close() triggers onStateChange callback which also acquires peersMutex_.
	std::vector<std::shared_ptr<PeerInfo>> toClose;
	{
		std::lock_guard<std::mutex> lock(peersMutex_);
		auto it = peers_.begin();
		while (it != peers_.end()) {
			if (it->second->type == ConnectionType::Publisher) {
				toClose.push_back(it->second);
				it = peers_.erase(it);
			} else {
				++it;
			}
		}
	}

	for (auto &peer : toClose) {
		clearPeerCallbacks(peer);
		peer->audioTrack.reset();
		peer->videoTrack.reset();
		peer->dataChannel.reset();
		peer->pc.reset();
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

int VDONinjaPeerManager::getMaxViewers() const
{
	return maxViewers_;
}

int VDONinjaPeerManager::getPublisherSlotCount() const
{
	std::lock_guard<std::mutex> lock(peersMutex_);
	int count = 0;
	for (const auto &pair : peers_) {
		if (pair.second->type != ConnectionType::Publisher) {
			continue;
		}
		if (countsTowardViewerLimit(pair.second->state)) {
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
	peer->awaitingVideoKeyframe = true;
	peer->audioSeq = audioSeq_++;
	peer->videoSeq = videoSeq_++;
	peer->audioTimestamp = audioTimestamp_;
	peer->videoTimestamp = videoTimestamp_;

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
		if (shuttingDown_) {
			return;
		}
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
		case rtc::PeerConnection::State::Connected: {
			peer->state = ConnectionState::Connected;
			logInfo("Peer %s connected", uuid.c_str());
			OnPeerConnectedCallback cb;
			{
				std::lock_guard<std::mutex> callbackLock(callbackMutex_);
				cb = onPeerConnected_;
			}
			if (cb) {
				cb(uuid);
			}
			break;
		}
		case rtc::PeerConnection::State::Disconnected: {
			peer->state = ConnectionState::Disconnected;
			logInfo("Peer %s disconnected", uuid.c_str());
			OnPeerDisconnectedCallback cb;
			{
				std::lock_guard<std::mutex> callbackLock(callbackMutex_);
				cb = onPeerDisconnected_;
			}
			if (cb) {
				cb(uuid);
			}
			{
				std::lock_guard<std::mutex> lock(peersMutex_);
				peers_.erase(uuid);
			}
			break;
		}
		case rtc::PeerConnection::State::Failed: {
			peer->state = ConnectionState::Failed;
			logError("Peer %s connection failed", uuid.c_str());
			OnPeerDisconnectedCallback cb;
			{
				std::lock_guard<std::mutex> callbackLock(callbackMutex_);
				cb = onPeerDisconnected_;
			}
			if (cb) {
				cb(uuid);
			}
			{
				std::lock_guard<std::mutex> lock(peersMutex_);
				peers_.erase(uuid);
			}
			break;
		}
		case rtc::PeerConnection::State::Closed:
			peer->state = ConnectionState::Closed;
			logInfo("Peer %s closed", uuid.c_str());
			{
				std::lock_guard<std::mutex> lock(peersMutex_);
				peers_.erase(uuid);
			}
			break;
		}
	});

	peer->pc->onLocalCandidate([this, weakPeer, uuid](rtc::Candidate candidate) {
		if (shuttingDown_) {
			return;
		}
		auto peer = weakPeer.lock();
		if (!peer)
			return;

		// Bundle candidates before sending
		bool flushNow = false;
		{
			std::lock_guard<std::mutex> lock(candidateMutex_);
			auto &bundle = candidateBundles_[uuid];
			bundle.candidates.push_back({std::string(candidate), candidate.mid()});
			bundle.lastUpdate = currentTimeMs();

			// Schedule sending after delay
			// In a real implementation, use a timer. Here we send immediately if enough time passed.
			flushNow = bundle.candidates.size() >= 5;
		}
		if (flushNow) {
			bundleAndSendCandidates(uuid);
		}
	});

	peer->pc->onGatheringStateChange([this, uuid](rtc::PeerConnection::GatheringState state) {
		if (shuttingDown_) {
			return;
		}
		if (state == rtc::PeerConnection::GatheringState::Complete) {
			logInfo("ICE gathering complete for %s", uuid.c_str());
			bundleAndSendCandidates(uuid);
		}
	});

	peer->pc->onTrack([this, weakPeer, uuid](std::shared_ptr<rtc::Track> track) {
		if (shuttingDown_) {
			return;
		}
		auto peer = weakPeer.lock();
		if (!peer)
			return;

		auto desc = track->description();
		TrackType type = TrackType::Video;
		if (desc.type() == "audio") {
			type = TrackType::Audio;
			peer->audioTrack = track;
		} else {
			peer->videoTrack = track;
		}

		logInfo("Received %s track from %s", type == TrackType::Audio ? "audio" : "video", uuid.c_str());

		OnTrackCallback cb;
		{
			std::lock_guard<std::mutex> callbackLock(callbackMutex_);
			cb = onTrack_;
		}
		if (cb) {
			cb(uuid, type, track);
		}
	});

	peer->pc->onDataChannel([this, weakPeer, uuid](std::shared_ptr<rtc::DataChannel> dc) {
		if (shuttingDown_) {
			return;
		}
		auto peer = weakPeer.lock();
		if (!peer)
			return;

		peer->dataChannel = dc;
		peer->hasDataChannel = true;

		dc->onMessage([this, uuid](auto data) {
			if (shuttingDown_) {
				return;
			}
			if (std::holds_alternative<std::string>(data)) {
				OnDataChannelMessageCallback cb;
				{
					std::lock_guard<std::mutex> callbackLock(callbackMutex_);
					cb = onDataChannelMessage_;
				}
				if (cb) {
					cb(uuid, std::get<std::string>(data));
				}
			}
		});

		logInfo("Data channel opened with %s", uuid.c_str());

		OnDataChannelCallback cb;
		{
			std::lock_guard<std::mutex> callbackLock(callbackMutex_);
			cb = onDataChannel_;
		}
		if (cb) {
			cb(uuid, dc);
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
	peer->videoTrack = peer->pc->addTrack(videoDesc);

	// Set up audio track
	rtc::Description::Audio audioDesc("audio", rtc::Description::Direction::SendOnly);
	audioDesc.addOpusCodec(111);
	audioDesc.addSSRC(audioSsrc_, "audio-stream");
	peer->audioTrack = peer->pc->addTrack(audioDesc);

	// OBS emits already-encoded Opus payloads; send manual RTP packets for maximum
	// compatibility across libdatachannel versions.
	peer->useAudioPacketizer = false;

	// OBS emits encoded H264 access units; do explicit RTP packetization here to
	// keep timestamping and fragmentation deterministic across libdatachannel
	// versions.
	peer->useVideoPacketizer = false;

	// Create data channel if enabled
	if (enableDataChannel_) {
		// VDO.Ninja expects publisher data channels to use "sendChannel".
		auto dc = peer->pc->createDataChannel("sendChannel");
		peer->dataChannel = dc;
		peer->hasDataChannel = true;

		dc->onOpen([this, uuid = peer->uuid, dc]() {
			if (shuttingDown_) {
				return;
			}
			logInfo("Data channel opened for %s", uuid.c_str());
			OnDataChannelCallback cb;
			{
				std::lock_guard<std::mutex> callbackLock(callbackMutex_);
				cb = onDataChannel_;
			}
			if (cb) {
				cb(uuid, dc);
			}
		});

		dc->onMessage([this, uuid = peer->uuid](auto data) {
			if (shuttingDown_) {
				return;
			}
			if (std::holds_alternative<std::string>(data)) {
				OnDataChannelMessageCallback cb;
				{
					std::lock_guard<std::mutex> callbackLock(callbackMutex_);
					cb = onDataChannelMessage_;
				}
				if (cb) {
					cb(uuid, std::get<std::string>(data));
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
	{
		std::lock_guard<std::mutex> lock(candidateMutex_);
		candidateBundles_[uuid].session = session;
	}

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

	const ConnectionState state = peer->state.load();
	const bool sessionMismatch = !session.empty() && !peer->session.empty() && peer->session != session;
	if (sessionMismatch && state == ConnectionState::Connected) {
		logWarning("Session mismatch for %s while connected, ignoring answer", uuid.c_str());
		return;
	}
	if (sessionMismatch) {
		logWarning("Session mismatch for %s while negotiating, accepting latest answer session", uuid.c_str());
	}
	if (!session.empty()) {
		std::lock_guard<std::mutex> lock(candidateMutex_);
		candidateBundles_[uuid].session = session;
	}

	// Set remote description (the answer)
	peer->pc->setRemoteDescription(rtc::Description(sdp, rtc::Description::Type::Answer));
	logInfo("Set remote answer for %s", uuid.c_str());
}

void VDONinjaPeerManager::onSignalingOfferRequest(const std::string &uuid, const std::string &session)
{
	if (!publishing_) {
		logDebug("Ignoring offer request from %s while not publishing", uuid.c_str());
		return;
	}

	if (uuid.empty()) {
		logWarning("Ignoring offer request without UUID");
		return;
	}

	std::shared_ptr<PeerInfo> peer;
	std::shared_ptr<PeerInfo> stalePeer;
	std::string staleReason;
	bool hadExistingPeer = false;
	{
		std::lock_guard<std::mutex> lock(peersMutex_);
		auto it = peers_.find(uuid);
		if (it != peers_.end()) {
			peer = it->second;
			hadExistingPeer = true;
			if (!peer) {
				peers_.erase(it);
				peer = nullptr;
			} else {
				const bool sessionRotated = !session.empty() && !peer->session.empty() && (peer->session != session);
				const bool staleState =
				    peer->state == ConnectionState::Failed || peer->state == ConnectionState::Closed;
				if (sessionRotated || staleState) {
					stalePeer = peer;
					staleReason = sessionRotated ? "session-rotated" : "stale-state";
					peers_.erase(it);
					peer = nullptr;
				}
			}
		}
	}

	if (stalePeer) {
		logInfo("Recreating viewer peer %s (%s)", uuid.c_str(), staleReason.c_str());
		clearPeerCallbacks(stalePeer);
		stalePeer->audioTrack.reset();
		stalePeer->videoTrack.reset();
		stalePeer->dataChannel.reset();
		stalePeer->pc.reset();

		std::lock_guard<std::mutex> lock(candidateMutex_);
		candidateBundles_.erase(uuid);
	}

	if (!peer) {
		if (getPublisherSlotCount() >= maxViewers_) {
			logWarning("Rejecting offer request from %s - max viewers reached (%d)", uuid.c_str(), maxViewers_);
			return;
		}
		peer = createPublisherConnection(uuid);
	}

	// Ignore duplicate offer requests for already-active peers. Re-negotiating here
	// can repeatedly force keyframe gating and appear as "video advances only on
	// click"/occasional frame updates.
	const ConnectionState state = peer->state.load();
	const bool sameSession = session.empty() || peer->session.empty() || peer->session == session;
	const bool activePeer = state == ConnectionState::Connected;
	if (hadExistingPeer && sameSession && activePeer) {
		logDebug("Ignoring duplicate offer request for active peer %s (state=%d)", uuid.c_str(),
		         static_cast<int>(state));
		return;
	}

	if (!session.empty()) {
		peer->session = session;
	} else if (peer->session.empty()) {
		peer->session = generateSessionId();
	}
	{
		std::lock_guard<std::mutex> lock(candidateMutex_);
		candidateBundles_[uuid].session = peer->session;
	}

	// Only force keyframe wait when (re)establishing an inactive peer/session.
	if (!hadExistingPeer || state != ConnectionState::Connected) {
		peer->awaitingVideoKeyframe = true;
	}

	peer->pc->setLocalDescription(rtc::Description::Type::Offer);

	auto localDesc = peer->pc->localDescription();
	if (!localDesc) {
		logWarning("No local offer available yet for %s", uuid.c_str());
		return;
	}

	signaling_->sendOffer(uuid, std::string(*localDesc), peer->session);
	logInfo("Sent offer to %s (session %s)", uuid.c_str(), peer->session.c_str());
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

	const ConnectionState state = peer->state.load();
	const bool sessionMismatch = !session.empty() && !peer->session.empty() && peer->session != session;
	if (sessionMismatch && state == ConnectionState::Connected) {
		logDebug("Session mismatch for ICE candidate from %s (connected peer), ignoring", uuid.c_str());
		return;
	}
	if (sessionMismatch) {
		logDebug("Session mismatch for ICE candidate from %s while negotiating, accepting", uuid.c_str());
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
			auto track = peer->audioTrack;
			if (!track) {
				continue;
			}

			if (peer->useAudioPacketizer) {
				rtc::binary opusFrame(size);
				std::memcpy(opusFrame.data(), data, size);
				track->send(std::move(opusFrame));
				continue;
			}

			// Create RTP packet (simplified)
			std::vector<uint8_t> rtpPacket;
			rtpPacket.reserve(12 + size);

			// RTP header
			rtpPacket.push_back(0x80); // V=2, P=0, X=0, CC=0
			rtpPacket.push_back(111);  // PT=111 (Opus), M=0
			rtpPacket.push_back((peer->audioSeq >> 8) & 0xFF);
			rtpPacket.push_back(peer->audioSeq & 0xFF);
			peer->audioSeq++;

			// Timestamp
			uint32_t ts = timestamp ? timestamp : peer->audioTimestamp;
			rtpPacket.push_back((ts >> 24) & 0xFF);
			rtpPacket.push_back((ts >> 16) & 0xFF);
			rtpPacket.push_back((ts >> 8) & 0xFF);
			rtpPacket.push_back(ts & 0xFF);
			peer->audioTimestamp = ts + 960; // 48kHz, 20ms frames

			// SSRC
			rtpPacket.push_back((audioSsrc_ >> 24) & 0xFF);
			rtpPacket.push_back((audioSsrc_ >> 16) & 0xFF);
			rtpPacket.push_back((audioSsrc_ >> 8) & 0xFF);
			rtpPacket.push_back(audioSsrc_ & 0xFF);

			// Payload
			rtpPacket.insert(rtpPacket.end(), data, data + size);

			rtc::binary packet(rtpPacket.size());
			std::memcpy(packet.data(), rtpPacket.data(), rtpPacket.size());
			track->send(std::move(packet));
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
		sendVideoFrameToPeerLocked(pair.first, peer, data, size, timestamp, keyframe);
	}
}

bool VDONinjaPeerManager::sendVideoFrameToPeer(const std::string &uuid, const uint8_t *data, size_t size,
                                               uint32_t timestamp, bool keyframe)
{
	if (!publishing_ || uuid.empty() || !data || size == 0) {
		return false;
	}

	std::lock_guard<std::mutex> lock(peersMutex_);
	auto it = peers_.find(uuid);
	if (it == peers_.end()) {
		return false;
	}

	return sendVideoFrameToPeerLocked(uuid, it->second, data, size, timestamp, keyframe);
}

bool VDONinjaPeerManager::sendVideoFrameToPeerLocked(const std::string &uuid, const std::shared_ptr<PeerInfo> &peer,
                                                     const uint8_t *data, size_t size, uint32_t timestamp,
                                                     bool keyframe)
{
	if (!peer || peer->type != ConnectionType::Publisher || peer->state != ConnectionState::Connected) {
		return false;
	}

	// Do not send delta frames to new/reconnected viewers until they get a keyframe.
	if (peer->awaitingVideoKeyframe && !keyframe) {
		return false;
	}

	if (peer->awaitingVideoKeyframe && keyframe) {
		peer->awaitingVideoKeyframe = false;
		logInfo("Viewer %s synchronized on keyframe", uuid.c_str());
	}

	try {
		auto track = peer->videoTrack;
		if (!track) {
			return false;
		}

		uint32_t ts = timestamp ? timestamp : peer->videoTimestamp;
		if (!sendH264FrameRtp(track, peer->videoSeq, ts, videoSsrc_, data, size)) {
			return false;
		}

		peer->videoTimestamp = ts + 3000; // 90kHz clock, ~30fps fallback cadence
		return true;
	} catch (const std::exception &e) {
		logError("Failed to send video to %s: %s", uuid.c_str(), e.what());
		return false;
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
			clearPeerCallbacks(it->second);
			it->second->audioTrack.reset();
			it->second->videoTrack.reset();
			it->second->dataChannel.reset();
			it->second->pc.reset();
			it = peers_.erase(it);
		} else {
			++it;
		}
	}
	logInfo("Stopped viewing stream: %s", streamId.c_str());
}

void VDONinjaPeerManager::clearPeerCallbacks(const std::shared_ptr<PeerInfo> &peer) const
{
	if (!peer || !peer->pc) {
		return;
	}

	try {
		peer->pc->onStateChange(nullptr);
		peer->pc->onLocalCandidate(nullptr);
		peer->pc->onGatheringStateChange(nullptr);
		peer->pc->onTrack(nullptr);
		peer->pc->onDataChannel(nullptr);
	} catch (const std::exception &) {
	}

	if (peer->dataChannel) {
		try {
			peer->dataChannel->onOpen(nullptr);
			peer->dataChannel->onMessage(nullptr);
		} catch (const std::exception &) {
		}
	}
}

void VDONinjaPeerManager::sendDataToAll(const std::string &message)
{
	std::lock_guard<std::mutex> lock(peersMutex_);
	for (auto &pair : peers_) {
		try {
			if (pair.second->hasDataChannel && pair.second->dataChannel) {
				pair.second->dataChannel->send(message);
			}
		} catch (const std::exception &e) {
			logError("Failed to send data to %s: %s", pair.first.c_str(), e.what());
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
	std::lock_guard<std::mutex> callbackLock(callbackMutex_);
	onPeerConnected_ = callback;
}
void VDONinjaPeerManager::setOnPeerDisconnected(OnPeerDisconnectedCallback callback)
{
	std::lock_guard<std::mutex> callbackLock(callbackMutex_);
	onPeerDisconnected_ = callback;
}
void VDONinjaPeerManager::setOnTrack(OnTrackCallback callback)
{
	std::lock_guard<std::mutex> callbackLock(callbackMutex_);
	onTrack_ = callback;
}
void VDONinjaPeerManager::setOnDataChannel(OnDataChannelCallback callback)
{
	std::lock_guard<std::mutex> callbackLock(callbackMutex_);
	onDataChannel_ = callback;
}
void VDONinjaPeerManager::setOnDataChannelMessage(OnDataChannelMessageCallback callback)
{
	std::lock_guard<std::mutex> callbackLock(callbackMutex_);
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

std::vector<PeerSnapshot> VDONinjaPeerManager::getPeerSnapshots() const
{
	std::vector<PeerSnapshot> snapshots;
	std::lock_guard<std::mutex> lock(peersMutex_);
	snapshots.reserve(peers_.size());
	for (const auto &pair : peers_) {
		PeerSnapshot snapshot;
		snapshot.uuid = pair.first;
		snapshot.streamId = pair.second ? pair.second->streamId : "";
		snapshot.type = pair.second ? pair.second->type : ConnectionType::Publisher;
		snapshot.state = pair.second ? pair.second->state.load() : ConnectionState::Closed;
		snapshot.hasDataChannel = pair.second && pair.second->hasDataChannel;
		snapshots.emplace_back(std::move(snapshot));
	}
	return snapshots;
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
