/*
 * OBS VDO.Ninja Plugin
 * Multi-peer connection manager implementation
 */

#include "vdoninja-peer-manager.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <sstream>
#include <random>

#include "vdoninja-utils.h"

namespace vdoninja
{

namespace
{

constexpr uint8_t kH264PayloadType = 96;
constexpr uint8_t kH264FuAType = 28;
constexpr size_t kMaxRtpPayloadSize = 1200;

std::string viewerSignalingKey(const std::string &uuid, const std::string &session)
{
	return session.empty() ? uuid : (uuid + ":" + session);
}

std::string wrapTargetedPeerMessage(const std::string &uuid, const std::string &session, const std::string &message)
{
	const std::string trimmedMessage = trim(message);
	if (uuid.empty() || trimmedMessage.size() < 2 || trimmedMessage.front() != '{' || trimmedMessage.back() != '}') {
		return message;
	}

	JsonBuilder envelope;
	envelope.add("UUID", uuid);
	if (!session.empty()) {
		envelope.add("session", session);
	}

	std::string wrapped = envelope.build();
	const std::string body = trim(trimmedMessage.substr(1, trimmedMessage.size() - 2));
	if (body.empty()) {
		return wrapped;
	}

	if (!wrapped.empty() && wrapped.back() == '}') {
		wrapped.pop_back();
	}
	return wrapped + "," + body + "}";
}

std::string codecNameLower(const std::string &codec)
{
	std::string lower = codec;
	std::transform(lower.begin(), lower.end(), lower.begin(),
	               [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
	return lower;
}

TrackType classifyIncomingTrack(const std::shared_ptr<PeerInfo> &peer, const std::shared_ptr<rtc::Track> &track)
{
	if (!track) {
		return TrackType::Video;
	}

	const auto desc = track->description();
	if (desc.type() == "audio") {
		return TrackType::Audio;
	}

	const std::string trackMid = track->mid();
	if (!trackMid.empty()) {
		if (trackMid == "video-alpha") {
			return TrackType::AlphaVideo;
		}
		if (peer) {
			if (peer->alphaVideoTrack && peer->alphaVideoTrack->mid() == trackMid) {
				return TrackType::AlphaVideo;
			}
			if (peer->videoTrack && peer->videoTrack->mid() == trackMid) {
				return TrackType::Video;
			}
		}
	}

	if (peer && peer->videoTrack && peer->alphaVideoTrack && track != peer->videoTrack &&
	    track == peer->alphaVideoTrack) {
		return TrackType::AlphaVideo;
	}

	return TrackType::Video;
}

const SdpOfferedCodec *findPreferredOfferedCodec(const SdpOfferedMediaSection &section, const char *codecName)
{
	const std::string target = codecNameLower(codecName);
	const SdpOfferedCodec *fallback = nullptr;
	for (const auto &codec : section.codecs) {
		if (codecNameLower(codec.codec) != target) {
			continue;
		}
		if (!fallback) {
			fallback = &codec;
		}
		if (target == "h264" &&
		    codecNameLower(codec.formatParameters).find("packetization-mode=1") != std::string::npos) {
			return &codec;
		}
	}
	return fallback;
}

const SdpOfferedCodec *findAssociatedRtxCodec(const SdpOfferedMediaSection &section, int primaryPayloadType)
{
	for (const auto &codec : section.codecs) {
		if (codecNameLower(codec.codec) == "rtx" && codec.associatedPayloadType == primaryPayloadType) {
			return &codec;
		}
	}
	return nullptr;
}

std::string constrainViewerOfferToNativeCodecs(const std::string &sdp)
{
	std::string filtered = stripUnsupportedTransportCcFeedback(sdp);
	if (filtered != sdp) {
		logInfo(
		    "Stripped transport-cc feedback/extensions from native viewer offer to prefer REMB-compatible feedback");
	}
	return filtered;
}

std::string normalizeEscapedSdpLineEndings(const std::string &sdp)
{
	const bool hasActualLineBreaks = sdp.find('\n') != std::string::npos || sdp.find('\r') != std::string::npos;
	const bool hasEscapedLineBreaks =
	    sdp.find("\\r\\n") != std::string::npos || sdp.find("\\n") != std::string::npos ||
	    sdp.find("\\r") != std::string::npos;
	if (hasActualLineBreaks || !hasEscapedLineBreaks) {
		return sdp;
	}

	std::string normalized;
	normalized.reserve(sdp.size());
	for (size_t i = 0; i < sdp.size(); ++i) {
		if (sdp[i] != '\\' || i + 1 >= sdp.size()) {
			normalized.push_back(sdp[i]);
			continue;
		}

		const char next = sdp[i + 1];
		switch (next) {
		case 'r':
			normalized.push_back('\r');
			++i;
			break;
		case 'n':
			normalized.push_back('\n');
			++i;
			break;
		case '\\':
			normalized.push_back('\\');
			++i;
			break;
		default:
			normalized.push_back(sdp[i]);
			break;
		}
	}

	return normalized;
}

std::string describeOfferedSections(const std::vector<SdpOfferedMediaSection> &sections)
{
	std::ostringstream summary;
	for (size_t i = 0; i < sections.size(); ++i) {
		if (i > 0) {
			summary << "; ";
		}

		const auto &section = sections[i];
		summary << section.type << "(mid=" << (section.mid.empty() ? "?" : section.mid) << " codecs=";
		for (size_t codecIndex = 0; codecIndex < section.codecs.size(); ++codecIndex) {
			if (codecIndex > 0) {
				summary << ",";
			}
			summary << section.codecs[codecIndex].codec << "/" << section.codecs[codecIndex].payloadType;
		}
		summary << ")";
	}

	return summary.str();
}

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

void clearTrackCallbacks(const std::shared_ptr<rtc::Track> &track)
{
	if (!track) {
		return;
	}

	try {
		track->resetCallbacks();
		track->setMediaHandler(nullptr);
	} catch (const std::exception &) {
	}
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
	installLocalDescriptionCallback(peer);
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
	installLocalDescriptionCallback(peer);

	{
		std::lock_guard<std::mutex> lock(peersMutex_);
		peers_[uuid] = peer;
	}

	logInfo("Created viewer connection for publisher: %s", uuid.c_str());
	return peer;
}

void VDONinjaPeerManager::installLocalDescriptionCallback(const std::shared_ptr<PeerInfo> &peer)
{
	if (!peer || !peer->pc || peer->localDescriptionCallbackInstalled) {
		return;
	}

	peer->localDescriptionCallbackInstalled = true;
	auto weakPeer = std::weak_ptr<PeerInfo>(peer);
	const std::string uuid = peer->uuid;

	peer->pc->onLocalDescription([this, weakPeer, uuid](rtc::Description description) {
		if (shuttingDown_) {
			return;
		}

		auto peer = weakPeer.lock();
		if (!peer || !signaling_) {
			return;
		}

		const std::string sdp = std::string(description);
		if (sdp.empty()) {
			logWarning("Ignoring empty local %s for %s", description.typeString().c_str(), uuid.c_str());
			return;
		}

		switch (description.type()) {
		case rtc::Description::Type::Offer:
			if (peer->type != ConnectionType::Publisher) {
				logDebug("Ignoring local offer generated for viewer peer %s", uuid.c_str());
				break;
			}
			signaling_->sendOffer(uuid, sdp, peer->session);
			logInfo("Sent offer to %s (session %s)", uuid.c_str(), peer->session.c_str());
			break;
		case rtc::Description::Type::Answer:
			if (peer->type != ConnectionType::Viewer) {
				logDebug("Ignoring local answer generated for publisher peer %s", uuid.c_str());
				break;
			}
			if (peer->signalingDataChannel) {
				signaling_->sendAnswerViaDataChannel(peer->signalingDataChannel, uuid, sdp, peer->session);
			} else {
				signaling_->sendAnswer(uuid, sdp, peer->session);
			}
			logInfo("Sent answer to %s", uuid.c_str());
			break;
		default:
			logDebug("Ignoring local description type '%s' for %s", description.typeString().c_str(), uuid.c_str());
			break;
		}
	});
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
				OnPeerDisconnectedCallback cb;
				{
					std::lock_guard<std::mutex> callbackLock(callbackMutex_);
					cb = onPeerDisconnected_;
				}
				if (cb) {
					cb(uuid);
				}
			}
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

		const TrackType type = classifyIncomingTrack(peer, track);
		if (type == TrackType::Audio) {
			peer->audioTrack = track;
		} else if (type == TrackType::AlphaVideo) {
			peer->alphaVideoTrack = track;
		} else {
			peer->videoTrack = track;
		}

		const char *typeLabel =
		    type == TrackType::Audio ? "audio" : (type == TrackType::AlphaVideo ? "alpha video" : "video");
		logInfo("Received %s track from %s (mid=%s)", typeLabel, uuid.c_str(), track ? track->mid().c_str() : "");

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

void VDONinjaPeerManager::prepareViewerTracks(const std::shared_ptr<PeerInfo> &peer, const std::string &offerSdp)
{
	if (!peer || !peer->pc || offerSdp.empty()) {
		logWarning("Skipping native recvonly track preparation because peer or SDP was empty");
		return;
	}

	const auto offeredSections = parseOfferedMediaSections(offerSdp);
	logInfo("Native viewer offer for %s parsed into %zu media sections: %s", peer->uuid.c_str(),
	        offeredSections.size(), describeOfferedSections(offeredSections).c_str());
	if (offeredSections.empty()) {
		const bool hasActualLineBreaks =
		    offerSdp.find('\n') != std::string::npos || offerSdp.find('\r') != std::string::npos;
		const bool hasEscapedLineBreaks =
		    offerSdp.find("\\r\\n") != std::string::npos || offerSdp.find("\\n") != std::string::npos ||
		    offerSdp.find("\\r") != std::string::npos;
		logWarning("Native viewer offer for %s contained no audio/video media sections (bytes=%zu, actual_newlines=%s, "
		           "escaped_newlines=%s)",
		           peer->uuid.c_str(), offerSdp.size(), hasActualLineBreaks ? "yes" : "no",
		           hasEscapedLineBreaks ? "yes" : "no");
	}
	OnTrackCallback trackCallback;
	{
		std::lock_guard<std::mutex> callbackLock(callbackMutex_);
		trackCallback = onTrack_;
	}

	const int requestedVideoBitrateKbps = std::max(1, bitrate_ / 1000);

	for (const auto &section : offeredSections) {
		try {
			if (section.type == "video") {
				if (peer->videoTrack) {
					// Second video section: treat as alpha track if VP9 and no alpha track yet.
					if (!peer->alphaVideoTrack) {
						const SdpOfferedCodec *alphaCodec = findPreferredOfferedCodec(section, "vp9");
						if (alphaCodec) {
							rtc::Description::Video receiveAlpha(section.mid.empty() ? "video-alpha" : section.mid,
							                                     rtc::Description::Direction::RecvOnly);
							receiveAlpha.addVP9Codec(alphaCodec->payloadType);
							auto alphaTrack = peer->pc->addTrack(receiveAlpha);
							peer->alphaVideoTrack = alphaTrack;
							logInfo("Prepared native recvonly VP9 alpha video track for %s (mid=%s)",
							        peer->uuid.c_str(), alphaTrack ? alphaTrack->mid().c_str() : "");
							if (trackCallback && alphaTrack) {
								trackCallback(peer->uuid, TrackType::AlphaVideo, alphaTrack);
							}
						}
					}
					continue;
				}

				// Prefer VP9 for alpha-capable decoding; fall back to H.264.
				const SdpOfferedCodec *videoCodec = findPreferredOfferedCodec(section, "vp9");
				const bool useVP9 = (videoCodec != nullptr);
				if (!videoCodec) {
					videoCodec = findPreferredOfferedCodec(section, "h264");
				}
				if (!videoCodec) {
					logWarning("Remote offer for %s did not include VP9 or H.264 in video section %s",
					           peer->uuid.c_str(), section.mid.c_str());
					continue;
				}

				rtc::Description::Video receiveVideo(section.mid.empty() ? "video" : section.mid,
				                                     rtc::Description::Direction::RecvOnly);
				if (useVP9) {
					receiveVideo.addVP9Codec(videoCodec->payloadType);
				} else if (videoCodec->formatParameters.empty()) {
					receiveVideo.addH264Codec(videoCodec->payloadType);
				} else {
					receiveVideo.addH264Codec(videoCodec->payloadType, videoCodec->formatParameters);
				}
				if (const SdpOfferedCodec *rtxCodec = findAssociatedRtxCodec(section, videoCodec->payloadType)) {
					const unsigned int rtxClockRate =
					    rtxCodec->clockRate > 0 ? static_cast<unsigned int>(rtxCodec->clockRate) : 90000u;
					receiveVideo.addRtxCodec(rtxCodec->payloadType, videoCodec->payloadType, rtxClockRate);
				}
				receiveVideo.setBitrate(requestedVideoBitrateKbps);
				auto track = peer->pc->addTrack(receiveVideo);
				peer->videoTrack = track;
				logInfo("Prepared native recvonly %s video track for %s (mid=%s, bitrate=%d kbps)",
				        useVP9 ? "VP9" : "H.264", peer->uuid.c_str(), track ? track->mid().c_str() : "",
				        requestedVideoBitrateKbps);
				if (trackCallback && track) {
					trackCallback(peer->uuid, TrackType::Video, track);
				}
			} else if (section.type == "audio") {
				if (peer->audioTrack) {
					continue;
				}

				const SdpOfferedCodec *audioCodec = findPreferredOfferedCodec(section, "opus");
				if (!audioCodec) {
					logWarning("Remote offer for %s did not include Opus in audio section %s", peer->uuid.c_str(),
					           section.mid.c_str());
					continue;
				}

				rtc::Description::Audio receiveAudio(section.mid.empty() ? "audio" : section.mid,
				                                     rtc::Description::Direction::RecvOnly);
				if (audioCodec->formatParameters.empty()) {
					receiveAudio.addOpusCodec(audioCodec->payloadType);
				} else {
					receiveAudio.addOpusCodec(audioCodec->payloadType, audioCodec->formatParameters);
				}
				auto track = peer->pc->addTrack(receiveAudio);
				peer->audioTrack = track;
				logInfo("Prepared native recvonly audio track for %s (mid=%s)", peer->uuid.c_str(),
				        track ? track->mid().c_str() : "");
				if (trackCallback && track) {
					trackCallback(peer->uuid, TrackType::Audio, track);
				}
			}
		} catch (const std::exception &e) {
			logWarning("Failed to prepare native recvonly %s track for %s: %s", section.type.c_str(),
			           peer->uuid.c_str(), e.what());
		}
	}
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
	{
		std::lock_guard<std::mutex> lock(pendingViewerSignalingMutex_);
		auto it = pendingViewerSignalingDataChannels_.find(viewerSignalingKey(uuid, session));
		if (it != pendingViewerSignalingDataChannels_.end()) {
			peer->signalingDataChannel = it->second;
			pendingViewerSignalingDataChannels_.erase(it);
		}
	}

	// Set remote description (the offer)
	const std::string constrainedSdp = normalizeEscapedSdpLineEndings(constrainViewerOfferToNativeCodecs(sdp));
	try {
		const bool hasActualLineBreaks =
		    constrainedSdp.find('\n') != std::string::npos || constrainedSdp.find('\r') != std::string::npos;
		const bool hasEscapedLineBreaks =
		    constrainedSdp.find("\\r\\n") != std::string::npos || constrainedSdp.find("\\n") != std::string::npos ||
		    constrainedSdp.find("\\r") != std::string::npos;
		logInfo("Applying native viewer offer for %s (session=%s, bytes=%zu, actual_newlines=%s, escaped_newlines=%s, "
		        "signaling=%d)",
		        uuid.c_str(), session.c_str(), constrainedSdp.size(), hasActualLineBreaks ? "yes" : "no",
		        hasEscapedLineBreaks ? "yes" : "no", static_cast<int>(peer->pc->signalingState()));
		prepareViewerTracks(peer, constrainedSdp);
		logInfo("Prepared native viewer tracks for %s (video=%s, alpha=%s, audio=%s)", uuid.c_str(),
		        peer->videoTrack ? "yes" : "no", peer->alphaVideoTrack ? "yes" : "no",
		        peer->audioTrack ? "yes" : "no");
		peer->pc->setRemoteDescription(rtc::Description(constrainedSdp, rtc::Description::Type::Offer));
		logInfo("Applied remote offer for %s; signaling state is now %d", uuid.c_str(),
		        static_cast<int>(peer->pc->signalingState()));
	} catch (const std::exception &e) {
		logError("Failed to apply remote offer for %s: %s", uuid.c_str(), e.what());
		disconnectPeer(uuid);
	} catch (...) {
		logError("Failed to apply remote offer for %s: unknown exception", uuid.c_str());
		disconnectPeer(uuid);
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

	if (peer->type != ConnectionType::Publisher) {
		logDebug("Ignoring signaling answer for viewer peer %s", uuid.c_str());
		return;
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

	peer->pc->setLocalDescription();
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
	std::shared_ptr<rtc::DataChannel> signalingDataChannel;

	{
		std::lock_guard<std::mutex> lock(candidateMutex_);
		auto it = candidateBundles_.find(uuid);
		if (it == candidateBundles_.end() || it->second.candidates.empty()) {
			return;
		}
		bundle = std::move(it->second);
		candidateBundles_.erase(it);
	}
	{
		std::lock_guard<std::mutex> lock(peersMutex_);
		auto it = peers_.find(uuid);
		if (it != peers_.end() && it->second) {
			signalingDataChannel = it->second->signalingDataChannel;
		}
	}

	// Send all bundled candidates
	for (const auto &cand : bundle.candidates) {
		if (signalingDataChannel) {
			signaling_->sendIceCandidateViaDataChannel(signalingDataChannel, uuid, std::get<0>(cand), std::get<1>(cand),
			                                           bundle.session);
		} else {
			signaling_->sendIceCandidate(uuid, std::get<0>(cand), std::get<1>(cand), bundle.session);
		}
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
	// Collect peers to close outside the lock to avoid deadlock:
	// pc->close() triggers onStateChange callback which also acquires peersMutex_.
	std::vector<std::shared_ptr<PeerInfo>> toClose;
	{
		std::lock_guard<std::mutex> lock(peersMutex_);
		auto it = peers_.begin();
		while (it != peers_.end()) {
			if (it->second->type == ConnectionType::Viewer && it->second->streamId == streamId) {
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
	logInfo("Stopped viewing stream: %s", streamId.c_str());
}

bool VDONinjaPeerManager::disconnectPeer(const std::string &uuid)
{
	std::shared_ptr<PeerInfo> peer;
	{
		std::lock_guard<std::mutex> lock(peersMutex_);
		auto it = peers_.find(uuid);
		if (it == peers_.end()) {
			return false;
		}
		peer = it->second;
		peers_.erase(it);
	}

	if (!peer) {
		return false;
	}

	clearPeerCallbacks(peer);
	try {
		if (peer->pc) {
			peer->pc->close();
		}
	} catch (const std::exception &) {
	}
	peer->audioTrack.reset();
	peer->videoTrack.reset();
	peer->dataChannel.reset();
	peer->signalingDataChannel.reset();
	peer->pc.reset();
	logInfo("Disconnected peer: %s", uuid.c_str());
	return true;
}

void VDONinjaPeerManager::clearPeerCallbacks(const std::shared_ptr<PeerInfo> &peer) const
{
	if (!peer || !peer->pc) {
		return;
	}

	try {
		peer->pc->onStateChange(nullptr);
		peer->pc->onLocalDescription(nullptr);
		peer->pc->onLocalCandidate(nullptr);
		peer->pc->onGatheringStateChange(nullptr);
		peer->pc->onTrack(nullptr);
		peer->pc->onDataChannel(nullptr);
	} catch (const std::exception &) {
	}

	clearTrackCallbacks(peer->videoTrack);
	clearTrackCallbacks(peer->audioTrack);

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
	if (it == peers_.end() || !it->second) {
		return;
	}

	try {
		if (it->second->hasDataChannel && it->second->dataChannel) {
			it->second->dataChannel->send(message);
			return;
		}
		if (it->second->type == ConnectionType::Viewer && it->second->signalingDataChannel) {
			it->second->signalingDataChannel->send(wrapTargetedPeerMessage(uuid, it->second->session, message));
		}
	} catch (const std::exception &e) {
		logError("Failed to send data to %s: %s", uuid.c_str(), e.what());
	}
}

void VDONinjaPeerManager::bindViewerSignalingDataChannel(const std::string &transportPeerUuid,
                                                         const std::string &targetUuid,
                                                         const std::string &targetSession)
{
	if (transportPeerUuid.empty() || targetUuid.empty()) {
		return;
	}

	std::shared_ptr<rtc::DataChannel> transportDataChannel;
	{
		std::lock_guard<std::mutex> lock(peersMutex_);
		auto transportIt = peers_.find(transportPeerUuid);
		if (transportIt != peers_.end() && transportIt->second) {
			transportDataChannel = transportIt->second->dataChannel;
		}
	}
	if (!transportDataChannel) {
		return;
	}

	{
		std::lock_guard<std::mutex> lock(peersMutex_);
		auto targetIt = peers_.find(targetUuid);
		if (targetIt != peers_.end() && targetIt->second) {
			if (targetSession.empty() || targetIt->second->session.empty() ||
			    targetIt->second->session == targetSession) {
				targetIt->second->signalingDataChannel = transportDataChannel;
				return;
			}
		}
	}

	std::lock_guard<std::mutex> lock(pendingViewerSignalingMutex_);
	pendingViewerSignalingDataChannels_[viewerSignalingKey(targetUuid, targetSession)] = transportDataChannel;
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
