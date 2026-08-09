/*
 * OBS VDO.Ninja Plugin
 * Multi-peer connection manager implementation
 */

#include "vdoninja-peer-manager.h"

#include <algorithm>
#include <cctype>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <new>
#include <random>
#include <sstream>
#include <stdexcept>
#include <unordered_set>
#include <vector>

#include "vdoninja-audio-red.h"
#include "vdoninja-h264-profile.h"
#include "vdoninja-rtcp-feedback.h"
#include "vdoninja-rtp-repair.h"
#include "vdoninja-rtp-utils.h"
#include "vdoninja-utils.h"

namespace vdoninja
{

class PeerManagerOwnerSession
{
public:
	struct InstalledFunction {
		PeerManagerCompletionKind kind = PeerManagerCompletionKind::PeerConnectionState;
		const void *handle = nullptr;
		std::function<void()> detach;
	};

	class WorkPermit
	{
	public:
		WorkPermit() = default;
		WorkPermit(const WorkPermit &) = delete;
		WorkPermit &operator=(const WorkPermit &) = delete;
		WorkPermit(WorkPermit &&other) noexcept : session_(std::move(other.session_)), owner_(other.owner_)
		{
			other.owner_ = nullptr;
		}
		WorkPermit &operator=(WorkPermit &&other) noexcept
		{
			if (this != &other) {
				release();
				session_ = std::move(other.session_);
				owner_ = other.owner_;
				other.owner_ = nullptr;
			}
			return *this;
		}
		~WorkPermit() { release(); }

		explicit operator bool() const noexcept { return owner_ != nullptr; }
		VDONinjaPeerManager *owner() const noexcept { return owner_; }

	private:
		friend class PeerManagerOwnerSession;
		WorkPermit(std::shared_ptr<PeerManagerOwnerSession> session, VDONinjaPeerManager *owner)
		    : session_(std::move(session)), owner_(owner)
		{
		}

		void release() noexcept
		{
			if (session_) {
				session_->releaseWork();
				session_.reset();
			}
			owner_ = nullptr;
		}

		std::shared_ptr<PeerManagerOwnerSession> session_;
		VDONinjaPeerManager *owner_ = nullptr;
	};

	explicit PeerManagerOwnerSession(VDONinjaPeerManager *owner) : owner_(owner) {}

	static WorkPermit acquire(const std::weak_ptr<PeerManagerOwnerSession> &weakSession, PeerManagerCompletionKind kind,
	                          const void *handle)
	{
		auto session = weakSession.lock();
		if (!session) {
			return {};
		}
		session->invokeTestHook(NativeMediaTestOwnerSessionStage::BeforePermit, kind, handle);

		VDONinjaPeerManager *owner = nullptr;
		{
			std::lock_guard<std::mutex> lock(session->workMutex_);
			if (session->admittingWork_ && session->owner_) {
				++session->activeWork_;
				owner = session->owner_;
			}
		}
		if (!owner) {
			session->invokeTestHook(NativeMediaTestOwnerSessionStage::PermitRejected, kind, handle);
			return {};
		}
		WorkPermit permit(session, owner);
		session->invokeTestHook(NativeMediaTestOwnerSessionStage::PermitAcquired, kind, handle);
		return permit;
	}

	void closeWorkAdmission()
	{
		std::lock_guard<std::mutex> lock(workMutex_);
		admittingWork_ = false;
	}

	bool registerInstalledFunction(PeerManagerCompletionKind kind, const void *handle, std::function<void()> detach)
	{
		std::lock_guard<std::mutex> lock(functionMutex_);
		if (!admittingFunctions_) {
			return false;
		}
#if defined(VDONINJA_NATIVE_MEDIA_INTEGRATION_TEST)
		if (failedRegistrationKind_ && *failedRegistrationKind_ == kind) {
			failedRegistrationKind_.reset();
			throw std::bad_alloc();
		}
#endif
		installedFunctions_.push_back({kind, handle, std::move(detach)});
		return true;
	}

	std::vector<InstalledFunction> closeFunctionAdmissionAndTakeDetachers()
	{
		std::lock_guard<std::mutex> lock(functionMutex_);
		admittingFunctions_ = false;
		std::vector<InstalledFunction> result;
		result.swap(installedFunctions_);
		return result;
	}

#if defined(VDONINJA_NATIVE_MEDIA_INTEGRATION_TEST)
	void failNextFunctionRegistration(PeerManagerCompletionKind kind)
	{
		std::lock_guard<std::mutex> lock(functionMutex_);
		failedRegistrationKind_ = kind;
	}

	void setTestHook(VDONinjaPeerManager::NativeMediaTestOwnerSessionHook hook)
	{
		std::lock_guard<std::mutex> lock(testHookMutex_);
		testHook_ = std::move(hook);
	}
#endif

	void invokeTestHook(NativeMediaTestOwnerSessionStage stage, PeerManagerCompletionKind kind, const void *handle)
	{
#if defined(VDONINJA_NATIVE_MEDIA_INTEGRATION_TEST)
		VDONinjaPeerManager::NativeMediaTestOwnerSessionHook hook;
		{
			std::lock_guard<std::mutex> lock(testHookMutex_);
			hook = testHook_;
		}
		if (hook) {
			hook(stage, kind, handle);
		}
#else
		(void)stage;
		(void)kind;
		(void)handle;
#endif
	}

	void waitForCurrentWork()
	{
		std::unique_lock<std::mutex> lock(workMutex_);
		if (activeWork_ != 0) {
			lock.unlock();
			invokeTestHook(NativeMediaTestOwnerSessionStage::WaitingForPermits, PeerManagerCompletionKind::OwnerSession,
			               nullptr);
			lock.lock();
		}
		workDrained_.wait(lock, [this]() { return activeWork_ == 0; });
		owner_ = nullptr;
		lock.unlock();
		invokeTestHook(NativeMediaTestOwnerSessionStage::WorkDrained, PeerManagerCompletionKind::OwnerSession, nullptr);
	}

private:
	void releaseWork() noexcept
	{
		std::lock_guard<std::mutex> lock(workMutex_);
		if (activeWork_ != 0 && --activeWork_ == 0) {
			workDrained_.notify_all();
		}
	}

	std::mutex workMutex_;
	std::condition_variable workDrained_;
	VDONinjaPeerManager *owner_ = nullptr;
	size_t activeWork_ = 0;
	bool admittingWork_ = true;

	std::mutex functionMutex_;
	std::vector<InstalledFunction> installedFunctions_;
	bool admittingFunctions_ = true;
#if defined(VDONINJA_NATIVE_MEDIA_INTEGRATION_TEST)
	std::optional<PeerManagerCompletionKind> failedRegistrationKind_;
	std::mutex testHookMutex_;
	VDONinjaPeerManager::NativeMediaTestOwnerSessionHook testHook_;
#endif
};

namespace
{

template <typename Detach>
void detachInstalledFunction(const std::shared_ptr<PeerManagerOwnerSession> &session, PeerManagerCompletionKind kind,
                             const void *handle, Detach &detach) noexcept
{
	if (session) {
		try {
			session->invokeTestHook(NativeMediaTestOwnerSessionStage::BeforeDetach, kind, handle);
		} catch (...) {
		}
	}
	try {
		detach();
	} catch (...) {
	}
	if (session) {
		try {
			session->invokeTestHook(NativeMediaTestOwnerSessionStage::AfterDetach, kind, handle);
		} catch (...) {
		}
	}
}

template <typename Detach>
void registerInstalledFunction(const std::shared_ptr<PeerManagerOwnerSession> &session, PeerManagerCompletionKind kind,
                               const void *handle, Detach &&detach)
{
	try {
		// Copy from the original detacher so it remains callable if std::function
		// materialization or registry growth throws after the external setter has
		// already installed its completion function.
		std::function<void()> detachFunction(detach);
		if (session && session->registerInstalledFunction(kind, handle, std::move(detachFunction))) {
			return;
		}
	} catch (...) {
		detachInstalledFunction(session, kind, handle, detach);
		throw;
	}
	detachInstalledFunction(session, kind, handle, detach);
}

constexpr uint8_t kH264PayloadType = 96;
constexpr uint8_t kOpusPayloadType = kDefaultOpusPayloadType;
constexpr uint8_t kAudioRedPayloadType = kDefaultAudioRedPayloadType;
constexpr uint8_t kH264FuAType = 28;
constexpr size_t kMaxRtpPayloadSize = 1200;
constexpr int64_t kRetiredPeerCleanupDelayMs = 1000;
constexpr uint32_t kVideoClockRate = 90000;
constexpr uint32_t kAudioClockRate = 48000;
constexpr auto kVideoPacerInterval = std::chrono::milliseconds(2);
constexpr size_t kAggregateVideoPacerBurstBytes = 4U * 1024U;
constexpr size_t kObservedDataChannelHistoryLimit = 16;

thread_local const rtc::DataChannel *activeManagerDataChannelCallback = nullptr;

class ScopedActiveDataChannelCallback
{
public:
	explicit ScopedActiveDataChannelCallback(const rtc::DataChannel *channel)
	    : previous_(activeManagerDataChannelCallback)
	{
		activeManagerDataChannelCallback = channel;
	}

	~ScopedActiveDataChannelCallback() { activeManagerDataChannelCallback = previous_; }

private:
	const rtc::DataChannel *previous_ = nullptr;
};

class DataChannelCallbackInstallState
{
public:
	enum class Action { Dispatch, DeferredInstalling, DeferredDraining, Drop };
	using DeferredCallback = std::function<void()>;

	Action submit(DeferredCallback &callback)
	{
		std::lock_guard<std::mutex> lock(mutex_);
		switch (phase_) {
		case Phase::Installing:
			deferred_.push_back(std::move(callback));
			return Action::DeferredInstalling;
		case Phase::Draining:
			deferred_.push_back(std::move(callback));
			return Action::DeferredDraining;
		case Phase::Active:
			return Action::Dispatch;
		case Phase::Cancelled:
		default:
			return Action::Drop;
		}
	}

	void prepend(DeferredCallback callback)
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (phase_ == Phase::Installing) {
			deferred_.insert(deferred_.begin(), std::move(callback));
		}
	}

	bool beginDrain(bool active)
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (!active) {
			phase_ = Phase::Cancelled;
			deferred_.clear();
			return false;
		}
		phase_ = Phase::Draining;
		return true;
	}

	void drain()
	{
		while (true) {
			std::vector<DeferredCallback> batch;
			{
				std::lock_guard<std::mutex> lock(mutex_);
				if (phase_ != Phase::Draining) {
					return;
				}
				if (deferred_.empty()) {
					phase_ = Phase::Active;
					return;
				}
				batch.swap(deferred_);
			}
			for (auto &callback : batch) {
				callback();
			}
		}
	}

private:
	enum class Phase { Installing, Draining, Active, Cancelled };
	std::mutex mutex_;
	Phase phase_ = Phase::Installing;
	std::vector<DeferredCallback> deferred_;
};

PeerEventIdentity peerEventIdentity(const std::shared_ptr<PeerInfo> &peer)
{
	return peer ? PeerEventIdentity{peer->uuid, peer->session, peer->generation,
	                                peer->nextEventSequence.load(std::memory_order_acquire)}
	            : PeerEventIdentity{};
}

PeerEventIdentity nextPeerEventIdentity(const std::shared_ptr<PeerInfo> &peer)
{
	return peer ? PeerEventIdentity{peer->uuid, peer->session, peer->generation,
	                                peer->nextEventSequence.fetch_add(1, std::memory_order_acq_rel) + 1}
	            : PeerEventIdentity{};
}

uint32_t rtpPacketTimestamp(const RtpPacketPacer::Packet &packet)
{
	if (packet.size() < 8) {
		return 0;
	}
	return (static_cast<uint32_t>(packet[4]) << 24) | (static_cast<uint32_t>(packet[5]) << 16) |
	       (static_cast<uint32_t>(packet[6]) << 8) | static_cast<uint32_t>(packet[7]);
}

// The caller must hold peer.mediaMutex. RTP sequence numbers are assigned when
// complete frames enter the pacer so NACK history remains deterministic. A
// recovery purge removes only an unsent tail; reclaiming exactly that tail
// prevents the next live keyframe from exposing artificial sequence gaps.
void reclaimDiscardedVideoSequenceNumbers(PeerInfo &peer, size_t discardedPackets)
{
	peer.videoSeq = rewindRtpSequenceNumber(peer.videoSeq, discardedPackets);
}

class RtcpTelemetryHandler final : public rtc::MediaHandler
{
public:
	explicit RtcpTelemetryHandler(std::shared_ptr<RtcpFeedbackTracker> tracker) : tracker_(std::move(tracker)) {}

	void incoming(rtc::message_vector &messages, const rtc::message_callback &) override
	{
		const auto tracker = tracker_;
		if (!tracker) {
			return;
		}

		const uint32_t compactNtpNow = RtcpFeedbackTracker::currentCompactNtp();
		for (const auto &message : messages) {
			if (!message || message->type != rtc::Message::Control) {
				continue;
			}
			tracker->observe(reinterpret_cast<const uint8_t *>(message->data()), message->size(), compactNtpNow);
		}
	}

private:
	std::shared_ptr<RtcpFeedbackTracker> tracker_;
};

class PacedNackResponder final : public rtc::MediaHandler
{
public:
	PacedNackResponder(uint32_t mediaSsrc, std::weak_ptr<RtpPacketPacer> pacer,
	                   std::shared_ptr<RtcpFeedbackTracker> tracker)
	    : mediaSsrc_(mediaSsrc), pacer_(std::move(pacer)), tracker_(std::move(tracker))
	{
	}

	void incoming(rtc::message_vector &messages, const rtc::message_callback &send) override
	{
		const auto pacer = pacer_.lock();
		if (!pacer) {
			return;
		}

		const auto self = std::static_pointer_cast<PacedNackResponder>(shared_from_this());
		const std::weak_ptr<PacedNackResponder> weakSelf = self;
		for (const auto &message : messages) {
			if (!message || message->type != rtc::Message::Control) {
				continue;
			}

			const auto requested =
			    parseRtcpNackRequests(reinterpret_cast<const uint8_t *>(message->data()), message->size(), mediaSsrc_);
			for (const uint16_t sequenceNumber : requested) {
				auto packet = cache_.find(sequenceNumber);
				tracker_->noteNackCacheResult(packet.has_value());
				if (!packet) {
					continue;
				}

				{
					std::lock_guard<std::mutex> lock(pendingMutex_);
					if (!pendingRepairs_.insert(sequenceNumber).second) {
						continue;
					}
				}

				auto directSend = [send](RtpPacketPacer::Packet &&repairPacket) {
					rtc::binary payload(repairPacket.size());
					std::memcpy(payload.data(), repairPacket.data(), repairPacket.size());
					send(rtc::make_message(std::move(payload)));
					return true;
				};
				const auto tracker = tracker_;
				const bool queued = pacer->enqueueRepair(
				    std::move(*packet), std::move(directSend),
				    [weakSelf, tracker, sequenceNumber](RtpPacerRepairOutcome outcome) {
					    if (const auto responder = weakSelf.lock()) {
						    std::lock_guard<std::mutex> lock(responder->pendingMutex_);
						    responder->pendingRepairs_.erase(sequenceNumber);
					    }
					    if (outcome == RtpPacerRepairOutcome::Expired) {
						    tracker->noteRetransmissionExpired();
					    } else {
						    tracker->noteRetransmissionCompleted(outcome == RtpPacerRepairOutcome::Sent);
					    }
				    });
				tracker_->noteRetransmissionQueued(queued);
				if (!queued) {
					std::lock_guard<std::mutex> lock(pendingMutex_);
					pendingRepairs_.erase(sequenceNumber);
				}
			}
		}
	}

	void outgoing(rtc::message_vector &messages, const rtc::message_callback &) override
	{
		for (const auto &message : messages) {
			if (!message || message->type == rtc::Message::Control) {
				continue;
			}
			cache_.store(reinterpret_cast<const uint8_t *>(message->data()), message->size());
		}
	}

private:
	const uint32_t mediaSsrc_;
	std::weak_ptr<RtpPacketPacer> pacer_;
	std::shared_ptr<RtcpFeedbackTracker> tracker_;
	RtpRetransmissionCache cache_;
	std::mutex pendingMutex_;
	std::unordered_set<uint16_t> pendingRepairs_;
};

template <typename Fn> void runRtcCallbackNoexcept(const char *context, Fn &&fn)
{
	try {
		fn();
	} catch (const std::exception &e) {
		logError("%s threw exception: %s", context, e.what());
	} catch (...) {
		logError("%s threw unknown exception", context);
	}
}

bool isTerminalPeerState(ConnectionState state)
{
	switch (state) {
	case ConnectionState::Disconnected:
	case ConnectionState::Failed:
	case ConnectionState::Closed:
		return true;
	case ConnectionState::New:
	case ConnectionState::Connecting:
	case ConnectionState::Connected:
	default:
		return false;
	}
}

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

// The caller must hold peer->mediaMutex whenever peer is non-null.
TrackType classifyIncomingTrack(const std::shared_ptr<PeerInfo> &peer, const std::shared_ptr<rtc::Track> &track)
{
	if (!track) {
		return TrackType::Video;
	}

	const auto desc = track->description();
	const std::string videoMid = (peer && peer->videoTrack) ? peer->videoTrack->mid() : "";
	const std::string alphaMid = (peer && peer->alphaVideoTrack) ? peer->alphaVideoTrack->mid() : "";
	const bool matchesAlphaTrackHandle = peer && peer->alphaVideoTrack && track == peer->alphaVideoTrack;
	return classifyIncomingTrackKind(desc.type(), track->mid(), videoMid, alphaMid, matchesAlphaTrackHandle);
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
		logInfo("Stripped transport-cc feedback/extensions unsupported by the native receiver");
	}
	return filtered;
}

std::string normalizeEscapedSdpLineEndings(const std::string &sdp)
{
	const bool hasActualLineBreaks = sdp.find('\n') != std::string::npos || sdp.find('\r') != std::string::npos;
	const bool hasEscapedLineBreaks = sdp.find("\\r\\n") != std::string::npos || sdp.find("\\n") != std::string::npos ||
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

bool appendRtpPacket(std::vector<rtc::binary> &packets, uint16_t &sequence, uint32_t timestamp, uint32_t ssrc,
                     bool marker, const uint8_t *payload, size_t payloadSize)
{
	if (!payload || payloadSize == 0) {
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
	packets.push_back(std::move(packet));
	return true;
}

bool buildH264FrameRtpPackets(std::vector<rtc::binary> &packets, uint16_t &sequence, uint32_t timestamp, uint32_t ssrc,
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
			if (!appendRtpPacket(packets, sequence, timestamp, ssrc, lastNalInFrame, nal.data, nal.size)) {
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

			if (!appendRtpPacket(packets, sequence, timestamp, ssrc, marker, payload.data(), payload.size())) {
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

void clearTrackLifecycleCallbacks(const std::shared_ptr<rtc::Track> &track)
{
	if (!track) {
		return;
	}

	try {
		track->onClosed(nullptr);
	} catch (const std::exception &) {
	}
	try {
		track->onError(nullptr);
	} catch (const std::exception &) {
	}
}

void clearPeerConnectionCallbacks(const std::shared_ptr<rtc::PeerConnection> &pc)
{
	if (!pc) {
		return;
	}

	try {
		pc->onStateChange(nullptr);
		pc->onLocalDescription(nullptr);
		pc->onLocalCandidate(nullptr);
		pc->onGatheringStateChange(nullptr);
		pc->onTrack(nullptr);
		pc->onDataChannel(nullptr);
	} catch (const std::exception &) {
	}
}

void clearDataChannelCallbacks(const std::shared_ptr<rtc::DataChannel> &dataChannel)
{
	if (!dataChannel) {
		return;
	}

	// Clear only the callbacks owned by the peer manager. DataChannel::resetCallbacks()
	// also resets libdatachannel's internal open-trigger state, so an already-open
	// channel could never resume message delivery if the exact handle is adopted again.
	try {
		dataChannel->onClosed(nullptr);
	} catch (const std::exception &) {
	}
	try {
		dataChannel->onError(nullptr);
	} catch (const std::exception &) {
	}
	try {
		dataChannel->onOpen(nullptr);
	} catch (const std::exception &) {
	}
	try {
		dataChannel->onMessage(nullptr);
	} catch (const std::exception &) {
	}
}

} // namespace

VDONinjaPeerManager::VDONinjaPeerManager()
    : videoPacerBudget_(std::make_shared<RtpSharedPacerBudget>(kAggregateVideoPacerBurstBytes)),
      ownerSession_(std::make_shared<PeerManagerOwnerSession>(this))
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
	const auto ownerSession = ownerSession_;
	if (ownerSession) {
		// Linearize shutdown before touching any subscriber or peer state. A
		// completion that has not yet acquired a permit can no longer enter.
		ownerSession->closeWorkAdmission();
	}
	shuttingDown_.store(true, std::memory_order_release);
	publishing_.store(false, std::memory_order_release);

	// Close function admission and snapshot every detacher before waiting. An
	// installer already holding a work permit that loses this race observes
	// closed function admission and detaches its own setter.
	if (ownerSession) {
		auto installedFunctions = ownerSession->closeFunctionAdmissionAndTakeDetachers();
		// Drain admitted work with no manager, peer, or function-registry lock
		// held. Manager destruction is a control-thread operation: a permitted
		// callback cannot synchronously destroy the owner whose permit it still
		// holds.
		ownerSession->waitForCurrentWork();

		// Callback setters can synchronously replay libdatachannel state. Invoke
		// them only after admitted work drains, and without a registry lock, so an
		// installed completion function is never detached while it is executing.
		for (auto &installed : installedFunctions) {
			detachInstalledFunction(ownerSession, installed.kind, installed.handle, installed.detach);
		}
	}

	// Subscribers are owner state and may have been snapshotted by admitted
	// callbacks, so release them only after all permits have drained.
	{
		std::lock_guard<std::mutex> callbackLock(callbackMutex_);
		onPeerConnected_ = nullptr;
		onPeerDisconnected_ = nullptr;
		onTrack_ = nullptr;
		onDataChannel_ = nullptr;
		onDataChannelMessage_ = nullptr;
		onKeyframeRequest_ = nullptr;
		onAcceptedSignalingLifecycleEvent_ = nullptr;
#if defined(VDONINJA_NATIVE_MEDIA_INTEGRATION_TEST)
		nativeMediaTestVideoFeedbackCompletions_.clear();
#endif
	}

	// Close all peer connections outside the map lock so RTC teardown cannot
	// re-enter peersMutex_ through a synchronous state callback.
	std::vector<std::shared_ptr<PeerInfo>> toRelease;
	{
		std::lock_guard<std::mutex> lock(peersMutex_);
		std::lock_guard<std::mutex> candidateLock(candidateMutex_);
		for (auto &pair : peers_) {
			if (pair.second) {
				pair.second->signalingActive.store(false);
			}
			toRelease.push_back(pair.second);
		}
		peers_.clear();
		candidateBundles_.clear();
	}
	for (auto &peer : toRelease) {
		releasePeerResources(peer);
	}
	pruneRetiredPeers(0);
	pendingRemoteIceCandidates_.clear();
	ownerSession_.reset();
}

void VDONinjaPeerManager::initialize(VDONinjaSignaling *signaling)
{
	signaling_ = signaling;
	const auto ownerSession = ownerSession_;
	const std::weak_ptr<PeerManagerOwnerSession> weakOwnerSession = ownerSession;

	// Set up signaling callbacks
	signaling_->setOnOffer([weakOwnerSession, signaling](const std::string &uuid, const std::string &sdp,
	                                                     const std::string &session) {
		auto permit =
		    PeerManagerOwnerSession::acquire(weakOwnerSession, PeerManagerCompletionKind::SignalingOffer, signaling);
		if (permit) {
			permit.owner()->onSignalingOffer(uuid, sdp, session);
		}
	});
	registerInstalledFunction(ownerSession, PeerManagerCompletionKind::SignalingOffer, signaling,
	                          [signaling]() { signaling->setOnOffer(nullptr); });

	signaling_->setOnAnswer([weakOwnerSession, signaling](const std::string &uuid, const std::string &sdp,
	                                                      const std::string &session) {
		auto permit =
		    PeerManagerOwnerSession::acquire(weakOwnerSession, PeerManagerCompletionKind::SignalingAnswer, signaling);
		if (permit) {
			permit.owner()->onSignalingAnswer(uuid, sdp, session);
		}
	});
	registerInstalledFunction(ownerSession, PeerManagerCompletionKind::SignalingAnswer, signaling,
	                          [signaling]() { signaling->setOnAnswer(nullptr); });

	signaling_->setOnOfferRequest([weakOwnerSession, signaling](const std::string &uuid, const std::string &session) {
		auto permit = PeerManagerOwnerSession::acquire(weakOwnerSession,
		                                               PeerManagerCompletionKind::SignalingOfferRequest, signaling);
		if (permit) {
			permit.owner()->onSignalingOfferRequest(uuid, session);
		}
	});
	registerInstalledFunction(ownerSession, PeerManagerCompletionKind::SignalingOfferRequest, signaling,
	                          [signaling]() { signaling->setOnOfferRequest(nullptr); });

	signaling_->setOnIceRestartRequest(
	    [weakOwnerSession, signaling](const std::string &uuid, const std::string &session) {
		    auto permit = PeerManagerOwnerSession::acquire(
		        weakOwnerSession, PeerManagerCompletionKind::SignalingIceRestartRequest, signaling);
		    if (permit) {
			    permit.owner()->requestIceRestart(uuid, session);
		    }
	    });
	registerInstalledFunction(ownerSession, PeerManagerCompletionKind::SignalingIceRestartRequest, signaling,
	                          [signaling]() { signaling->setOnIceRestartRequest(nullptr); });

	signaling_->setOnIceCandidate([weakOwnerSession, signaling](const std::string &uuid, const std::string &candidate,
	                                                            const std::string &mid, const std::string &session) {
		auto permit = PeerManagerOwnerSession::acquire(weakOwnerSession,
		                                               PeerManagerCompletionKind::SignalingIceCandidate, signaling);
		if (permit) {
			permit.owner()->onSignalingIceCandidate(uuid, candidate, mid, session);
		}
	});
	registerInstalledFunction(ownerSession, PeerManagerCompletionKind::SignalingIceCandidate, signaling,
	                          [signaling]() { signaling->setOnIceCandidate(nullptr); });

	signaling_->setOnPeerCleanup([weakOwnerSession, signaling](const std::string &uuid, const std::string &session) {
		auto permit = PeerManagerOwnerSession::acquire(weakOwnerSession,
		                                               PeerManagerCompletionKind::SignalingPeerCleanup, signaling);
		if (!permit) {
			return;
		}
		auto *manager = permit.owner();
		bool ambiguousReuse = false;
		const auto identity = manager->claimSignalingPeerCleanupIdentity(uuid, session, &ambiguousReuse);
		if (ambiguousReuse) {
			logWarning("Ignoring ambiguous sessionless cleanup for manager-observed reused peer %s", uuid.c_str());
			return;
		}
		if (identity) {
			manager->disconnectPeer(*identity);
		}
	});
	registerInstalledFunction(ownerSession, PeerManagerCompletionKind::SignalingPeerCleanup, signaling,
	                          [signaling]() { signaling->setOnPeerCleanup(nullptr); });

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
	rtc::Configuration config{};
	config.proxyServer.reset();
	config.bindAddress.reset();
	config.certificateType = rtc::CertificateType::Default;
	config.iceTransportPolicy = rtc::TransportPolicy::All;
	config.enableIceTcp = false;
	config.enableIceUdpMux = false;
	config.disableAutoNegotiation = false;
	config.forceMediaTransport = false;
	config.portRangeBegin = 1024;
	config.portRangeEnd = 65535;
	config.mtu.reset();
	config.maxMessageSize.reset();

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
			try {
				rtc::IceServer iceServer(server.urls);
				if (!server.username.empty()) {
					iceServer.username = server.username;
					iceServer.password = server.credential;
				}
				config.iceServers.push_back(iceServer);
				if (hasTurnScheme(server.urls)) {
					hasTurnServer = true;
				}
			} catch (const std::exception &e) {
				logWarning("Ignoring invalid custom ICE server '%s': %s", server.urls.c_str(), e.what());
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
	pruneRetiredPeers(kRetiredPeerCleanupDelayMs);

	if (publishing_) {
		logWarning("Already publishing");
		return true;
	}

	maxViewers_ = maxViewers;
	audioSendTracker_.reset();
	takeAudioRedStats();
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
		std::lock_guard<std::mutex> candidateLock(candidateMutex_);
		auto it = peers_.begin();
		while (it != peers_.end()) {
			if (it->second->type == ConnectionType::Publisher) {
				it->second->signalingActive.store(false);
				candidateBundles_.erase(it->second->generation);
				toClose.push_back(it->second);
				it = peers_.erase(it);
			} else {
				++it;
			}
		}
	}

	for (auto &peer : toClose) {
		releasePeerResources(peer);
	}
	pruneRetiredPeers(kRetiredPeerCleanupDelayMs);

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

bool VDONinjaPeerManager::requestIceRestart(const std::string &uuid, const std::string &requestedSession)
{
	if (!publishing_) {
		logDebug("Ignoring ICE restart request from %s while not publishing", uuid.c_str());
		return false;
	}

	if (uuid.empty()) {
		logWarning("Ignoring ICE restart request without UUID");
		return false;
	}

	std::shared_ptr<PeerInfo> peer;
	{
		std::lock_guard<std::mutex> lock(peersMutex_);
		auto it = peers_.find(uuid);
		if (it != peers_.end()) {
			peer = it->second;
		}
	}

	if (!peer || !peer->pc || peer->type != ConnectionType::Publisher) {
		logWarning("Ignoring ICE restart request for unknown publisher peer %s", uuid.c_str());
		return false;
	}

	if (peer->cleanupRetired.load() || isTerminalPeerState(peer->state.load())) {
		logInfo("Ignoring ICE restart request for retired/terminal peer %s", uuid.c_str());
		return false;
	}
	if (!requestedSession.empty() && !peer->session.empty() && requestedSession != peer->session) {
		logInfo("Ignoring ICE restart request for stale session on peer %s", uuid.c_str());
		return false;
	}

	if (peer->pc->signalingState() != rtc::PeerConnection::SignalingState::Stable) {
		logInfo("Ignoring ICE restart request for peer %s while signaling state is not stable", uuid.c_str());
		return false;
	}

	const std::string session =
	    !requestedSession.empty() ? requestedSession : (peer->session.empty() ? generateSessionId() : peer->session);
	PublisherMediaState mediaState;
	{
		std::lock_guard<std::mutex> mediaLock(peer->mediaMutex);
		mediaState.audioSendEnabled = peer->audioSendEnabled;
		mediaState.videoSendEnabled = peer->videoSendEnabled;
		mediaState.videoKeyframeGate = peer->videoKeyframeGate;
		mediaState.audioSeq = peer->audioSeq;
		mediaState.videoSeq = peer->videoSeq;
		mediaState.audioTimestamp = peer->audioTimestamp;
		mediaState.videoTimestamp = peer->videoTimestamp;
	}

	// This libdatachannel version has no restartIce API and reuses the existing
	// ICE transport for another ordinary offer. Rebuild the connection so the
	// new offer has fresh ICE credentials and gathers a fresh candidate set. Build
	// it detached first so construction failure leaves the existing peer intact.
	std::shared_ptr<PeerInfo> replacement;
	try {
		replacement = createPublisherConnection(uuid, session, &mediaState, false);
	} catch (const std::exception &e) {
		logError("Failed to prepare replacement publisher peer %s for ICE restart: %s", uuid.c_str(), e.what());
		return false;
	} catch (...) {
		logError("Failed to prepare replacement publisher peer %s for ICE restart: unknown exception", uuid.c_str());
		return false;
	}

	std::unique_lock<std::mutex> audioSendLock(peer->audioSendMutex, std::defer_lock);
	std::unique_lock<std::mutex> videoSendLock(peer->videoSendMutex, std::defer_lock);
	std::lock(audioSendLock, videoSendLock);
	std::unique_lock<std::mutex> oldMediaLock(peer->mediaMutex);

	if (peer->cleanupRetired.load() || isTerminalPeerState(peer->state.load()) || !peer->pc ||
	    peer->pc->signalingState() != rtc::PeerConnection::SignalingState::Stable) {
		oldMediaLock.unlock();
		audioSendLock.unlock();
		videoSendLock.unlock();
		retirePeerForDeferredCleanup(uuid, replacement);
		logInfo("ICE restart for peer %s was superseded while its replacement was being prepared", uuid.c_str());
		return false;
	}

	// Refresh the state while outgoing sends are paused, then synchronize both the
	// manual packetizer fields and the libdatachannel RTP configs.
	mediaState.audioSendEnabled = peer->audioSendEnabled;
	mediaState.videoSendEnabled = peer->videoSendEnabled;
	mediaState.videoKeyframeGate = peer->videoKeyframeGate;
	mediaState.audioSeq = peer->audioSeq;
	mediaState.videoSeq = peer->videoSeq;
	mediaState.audioTimestamp = peer->audioTimestamp;
	mediaState.videoTimestamp = peer->videoTimestamp;
	{
		std::lock_guard<std::mutex> replacementMediaLock(replacement->mediaMutex);
		replacement->audioSendEnabled = mediaState.audioSendEnabled;
		replacement->videoSendEnabled = mediaState.videoSendEnabled;
		replacement->videoKeyframeGate = mediaState.videoKeyframeGate;
		replacement->audioSeq = mediaState.audioSeq;
		replacement->videoSeq = mediaState.videoSeq;
		replacement->audioTimestamp = mediaState.audioTimestamp;
		replacement->videoTimestamp = mediaState.videoTimestamp;
		if (replacement->audioRtpConfig) {
			replacement->audioRtpConfig->sequenceNumber = mediaState.audioSeq;
			replacement->audioRtpConfig->timestamp = mediaState.audioTimestamp;
		}
		if (replacement->videoRtpConfig) {
			replacement->videoRtpConfig->sequenceNumber = mediaState.videoSeq;
			replacement->videoRtpConfig->timestamp = mediaState.videoTimestamp;
		}
	}

	bool swapped = false;
	{
		std::lock_guard<std::mutex> lock(peersMutex_);
		auto it = peers_.find(uuid);
		if (it != peers_.end() && it->second == peer) {
			std::lock_guard<std::mutex> candidateLock(candidateMutex_);
			peer->signalingActive.store(false);
			replacement->signalingActive.store(true);
			it->second = replacement;
			++peerGenerationRegistrationCounts_[uuid];
			swapped = true;
		}
	}
	if (!swapped) {
		oldMediaLock.unlock();
		audioSendLock.unlock();
		videoSendLock.unlock();
		retirePeerForDeferredCleanup(uuid, replacement);
		logInfo("ICE restart for peer %s was superseded by another connection", uuid.c_str());
		return false;
	}

	std::string offerError;
	try {
		replacement->localOfferRequested.store(true);
		replacement->localOfferDispatched.store(false);
		std::string cachedOffer;
		{
			std::lock_guard<std::mutex> negotiationLock(replacement->negotiationMutex);
			cachedOffer = replacement->lastLocalOfferSdp;
		}

		if (!cachedOffer.empty()) {
			std::lock_guard<std::mutex> candidateLock(candidateMutex_);
			if (!signaling_ || !replacement->signalingActive.load() || replacement->cleanupRetired.load()) {
				throw std::runtime_error("replacement peer was retired before its offer could be sent");
			}
			signaling_->sendOffer(uuid, cachedOffer, replacement->session);
			replacement->localOfferDispatched.store(true);
		} else if (replacement->pc->signalingState() == rtc::PeerConnection::SignalingState::Stable) {
			replacement->pc->setLocalDescription(rtc::Description::Type::Offer);
		} else {
			throw std::runtime_error("replacement peer did not produce an offer from a stable signaling state");
		}
	} catch (const std::exception &e) {
		offerError = e.what();
	} catch (...) {
		offerError = "unknown exception";
	}

	if (!offerError.empty()) {
		bool offerWasDispatched = false;
		{
			std::lock_guard<std::mutex> candidateLock(candidateMutex_);
			offerWasDispatched = replacement->localOfferDispatched.load();
		}
		if (!offerWasDispatched) {
			bool restoredOldPeer = false;
			{
				std::lock_guard<std::mutex> lock(peersMutex_);
				auto it = peers_.find(uuid);
				if (it != peers_.end() && it->second == replacement && !peer->cleanupRetired.load() &&
				    !isTerminalPeerState(peer->state.load())) {
					std::lock_guard<std::mutex> candidateLock(candidateMutex_);
					replacement->signalingActive.store(false);
					peer->signalingActive.store(true);
					it->second = peer;
					restoredOldPeer = true;
				}
			}
			retirePeerForDeferredCleanup(uuid, replacement);
			if (restoredOldPeer) {
				bundleAndSendCandidates(peer);
			} else {
				retirePeerForDeferredCleanup(uuid, peer);
			}
			logError("Failed to rebuild publisher peer %s for ICE restart: %s%s", uuid.c_str(), offerError.c_str(),
			         restoredOldPeer ? "; retained existing peer" : "");
			return false;
		}
		logWarning("Replacement offer for peer %s was dispatched despite a later setup error: %s", uuid.c_str(),
		           offerError.c_str());
	}

	retirePeerForDeferredCleanup(uuid, peer);
	bundleAndSendCandidates(replacement);
	logInfo("Rebuilt publisher peer %s for ICE restart (session %s)", uuid.c_str(), session.c_str());
	return true;
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

bool VDONinjaPeerManager::setPeerMediaSendEnabled(const std::string &uuid, bool hasVideo, bool videoEnabled,
                                                  bool hasAudio, bool audioEnabled, bool *videoBecameEnabled)
{
	if (videoBecameEnabled) {
		*videoBecameEnabled = false;
	}

	if (uuid.empty() || (!hasVideo && !hasAudio)) {
		return false;
	}

	std::shared_ptr<PeerInfo> peer;
	{
		std::lock_guard<std::mutex> lock(peersMutex_);
		auto it = peers_.find(uuid);
		if (it != peers_.end()) {
			peer = it->second;
		}
	}

	if (!peer || peer->type != ConnectionType::Publisher) {
		return false;
	}

	bool changed = false;
	bool finalVideoEnabled = true;
	bool finalAudioEnabled = true;
	{
		std::lock_guard<std::mutex> mediaLock(peer->mediaMutex);
		if (peer->cleanupRetired.load() || isTerminalPeerState(peer->state.load())) {
			return false;
		}

		if (hasVideo && peer->videoSendEnabled != videoEnabled) {
			const bool wasEnabled = peer->videoSendEnabled;
			peer->videoSendEnabled = videoEnabled;
			changed = true;
			if (!wasEnabled && videoEnabled) {
				peer->videoKeyframeGate.resetForCachedPrime();
				if (videoBecameEnabled) {
					*videoBecameEnabled = true;
				}
			}
		}
		if (hasAudio && peer->audioSendEnabled != audioEnabled) {
			peer->audioSendEnabled = audioEnabled;
			changed = true;
		}
		finalVideoEnabled = peer->videoSendEnabled;
		finalAudioEnabled = peer->audioSendEnabled;
	}

	if (changed) {
		logInfo("Peer %s media send state: video=%s audio=%s", uuid.c_str(), finalVideoEnabled ? "enabled" : "disabled",
		        finalAudioEnabled ? "enabled" : "disabled");
	}

	return true;
}

std::shared_ptr<PeerInfo> VDONinjaPeerManager::createPublisherConnection(const std::string &uuid,
                                                                         const std::string &session,
                                                                         const PublisherMediaState *initialMediaState,
                                                                         bool registerPeer)
{
	pruneRetiredPeers(kRetiredPeerCleanupDelayMs);

	auto config = getRtcConfig();
	auto pc = std::make_shared<rtc::PeerConnection>(config);

	auto peer = std::make_shared<PeerInfo>();
	peer->uuid = uuid;
	peer->generation = nextPeerGeneration_.fetch_add(1);
	peer->type = ConnectionType::Publisher;
	peer->session = session.empty() ? generateSessionId() : session;
	peer->pc = pc;
	if (initialMediaState) {
		peer->audioSendEnabled = initialMediaState->audioSendEnabled;
		peer->videoSendEnabled = initialMediaState->videoSendEnabled;
		peer->videoKeyframeGate = initialMediaState->videoKeyframeGate;
		peer->audioSeq = initialMediaState->audioSeq;
		peer->videoSeq = initialMediaState->videoSeq;
		peer->audioTimestamp = initialMediaState->audioTimestamp;
		peer->videoTimestamp = initialMediaState->videoTimestamp;
	} else {
		peer->videoKeyframeGate.resetForCachedPrime();
		peer->audioSeq = audioSeq_.fetch_add(1);
		peer->videoSeq = videoSeq_.fetch_add(1);
		peer->audioTimestamp = audioTimestamp_;
		peer->videoTimestamp = videoTimestamp_;
	}
	{
		std::lock_guard<std::mutex> lock(candidateMutex_);
		candidateBundles_[peer->generation].session = peer->session;
	}

	try {
		setupPeerConnectionCallbacks(peer);
		installLocalDescriptionCallback(peer);
		setupPublisherTracks(peer);
	} catch (...) {
		peer->cleanupRetired.store(true);
		clearPeerCallbacks(peer);
		std::lock_guard<std::mutex> candidateLock(candidateMutex_);
		candidateBundles_.erase(peer->generation);
		throw;
	}

	if (registerPeer) {
		std::shared_ptr<PeerInfo> concurrentPeer;
		{
			std::lock_guard<std::mutex> lock(peersMutex_);
			auto it = peers_.find(uuid);
			if (it != peers_.end() && it->second) {
				concurrentPeer = it->second;
			} else {
				std::lock_guard<std::mutex> candidateLock(candidateMutex_);
				peer->signalingActive.store(true);
				peers_[uuid] = peer;
				++peerGenerationRegistrationCounts_[uuid];
			}
		}
		if (concurrentPeer) {
			retirePeerForDeferredCleanup(uuid, peer);
			logDebug("Publisher connection creation for %s reused a peer registered concurrently", uuid.c_str());
			return concurrentPeer;
		}
	}

	logInfo("%s publisher connection for viewer: %s", registerPeer ? "Created" : "Prepared replacement", uuid.c_str());
	return peer;
}

std::shared_ptr<PeerInfo> VDONinjaPeerManager::createViewerConnection(const std::string &uuid)
{
	pruneRetiredPeers(kRetiredPeerCleanupDelayMs);

	auto config = getRtcConfig();
	auto pc = std::make_shared<rtc::PeerConnection>(config);

	auto peer = std::make_shared<PeerInfo>();
	peer->uuid = uuid;
	peer->generation = nextPeerGeneration_.fetch_add(1);
	peer->type = ConnectionType::Viewer;
	peer->session = generateSessionId();
	peer->pc = pc;

	try {
		setupPeerConnectionCallbacks(peer);
		installLocalDescriptionCallback(peer);
	} catch (...) {
		peer->cleanupRetired.store(true);
		clearPeerCallbacks(peer);
		std::lock_guard<std::mutex> candidateLock(candidateMutex_);
		candidateBundles_.erase(peer->generation);
		throw;
	}

	std::shared_ptr<PeerInfo> concurrentPeer;
	{
		std::lock_guard<std::mutex> lock(peersMutex_);
		auto it = peers_.find(uuid);
		if (it != peers_.end() && it->second) {
			concurrentPeer = it->second;
		} else {
			std::lock_guard<std::mutex> candidateLock(candidateMutex_);
			peer->signalingActive.store(true);
			peers_[uuid] = peer;
			++peerGenerationRegistrationCounts_[uuid];
		}
	}
	if (concurrentPeer) {
		retirePeerForDeferredCleanup(uuid, peer);
		logDebug("Viewer connection creation for %s reused a peer registered concurrently", uuid.c_str());
		return concurrentPeer;
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
	auto weakPc = std::weak_ptr<rtc::PeerConnection>(peer->pc);
	const void *pcHandle = peer->pc.get();
	const auto ownerSession = ownerSession_;
	const std::weak_ptr<PeerManagerOwnerSession> weakOwnerSession = ownerSession;
	const std::string uuid = peer->uuid;

	peer->pc->onLocalDescription([weakOwnerSession, weakPeer, uuid, pcHandle](rtc::Description description) {
		auto permit = PeerManagerOwnerSession::acquire(
		    weakOwnerSession, PeerManagerCompletionKind::PeerConnectionLocalDescription, pcHandle);
		if (!permit) {
			return;
		}
		auto *manager = permit.owner();
		runRtcCallbackNoexcept("PeerConnection::onLocalDescription", [&]() {
			if (manager->shuttingDown_) {
				return;
			}

			auto peer = weakPeer.lock();
			if (!peer || !manager->signaling_) {
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
				{
					std::lock_guard<std::mutex> negotiationLock(peer->negotiationMutex);
					peer->lastLocalOfferSdp = sdp;
				}
				if (!peer->localOfferRequested.load()) {
					break;
				}
				{
					std::lock_guard<std::mutex> candidateLock(manager->candidateMutex_);
					if (peer->cleanupRetired.load() || !peer->signalingActive.load()) {
						break;
					}
					manager->signaling_->sendOffer(uuid, sdp, peer->session);
					peer->localOfferDispatched.store(true);
				}
				logInfo("Sent offer to %s (session %s)", uuid.c_str(), peer->session.c_str());
				break;
			case rtc::Description::Type::Answer:
				if (peer->type != ConnectionType::Viewer) {
					logDebug("Ignoring local answer generated for publisher peer %s", uuid.c_str());
					break;
				}
				{
					std::shared_ptr<rtc::DataChannel> signalingDataChannel;
					{
						std::lock_guard<std::mutex> mediaLock(peer->mediaMutex);
						signalingDataChannel = peer->signalingDataChannel;
					}
					std::lock_guard<std::mutex> candidateLock(manager->candidateMutex_);
					if (peer->cleanupRetired.load() || !peer->signalingActive.load()) {
						break;
					}
					if (signalingDataChannel) {
						manager->signaling_->sendAnswerViaDataChannel(signalingDataChannel, uuid, sdp, peer->session);
					} else {
						manager->signaling_->sendAnswer(uuid, sdp, peer->session);
					}
				}
				logInfo("Sent answer to %s", uuid.c_str());
				break;
			default:
				logDebug("Ignoring local description type '%s' for %s", description.typeString().c_str(), uuid.c_str());
				break;
			}
		});
	});
	registerInstalledFunction(ownerSession, PeerManagerCompletionKind::PeerConnectionLocalDescription, pcHandle,
	                          [weakPc]() {
		                          if (const auto pc = weakPc.lock()) {
			                          pc->onLocalDescription(nullptr);
		                          }
	                          });
}

void VDONinjaPeerManager::setupPeerConnectionCallbacks(std::shared_ptr<PeerInfo> peer)
{
	auto weakPeer = std::weak_ptr<PeerInfo>(peer);
	auto weakPc = std::weak_ptr<rtc::PeerConnection>(peer->pc);
	const void *pcHandle = peer->pc.get();
	const auto ownerSession = ownerSession_;
	const std::weak_ptr<PeerManagerOwnerSession> weakOwnerSession = ownerSession;
	std::string uuid = peer->uuid;

	peer->pc->onStateChange([weakOwnerSession, weakPeer, uuid, pcHandle](rtc::PeerConnection::State state) {
		auto permit = PeerManagerOwnerSession::acquire(weakOwnerSession, PeerManagerCompletionKind::PeerConnectionState,
		                                               pcHandle);
		if (!permit) {
			return;
		}
		auto *manager = permit.owner();
		runRtcCallbackNoexcept("PeerConnection::onStateChange", [&]() {
			if (manager->shuttingDown_) {
				return;
			}
			auto peer = weakPeer.lock();
			if (!peer)
				return;
			if (peer->cleanupRetired.load() || !manager->isCurrentPeer(peer)) {
				return;
			}

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
				peer->terminalStateTimeMs.store(0);
				peer->disconnectNotified.store(false);
				peer->cleanupRetired.store(false);
				logInfo("Peer %s connected", uuid.c_str());
				OnPeerConnectedCallback cb;
				{
					std::lock_guard<std::mutex> callbackLock(manager->callbackMutex_);
					cb = manager->onPeerConnected_;
				}
				if (cb) {
					cb(nextPeerEventIdentity(peer));
				}
				break;
			}
			case rtc::PeerConnection::State::Disconnected: {
				peer->state = ConnectionState::Disconnected;
				peer->terminalStateTimeMs.store(currentTimeMs());
				logInfo("Peer %s disconnected", uuid.c_str());
				if (!peer->disconnectNotified.exchange(true)) {
					manager->dispatchPeerDisconnected(peer);
				}
				manager->retirePeerForDeferredCleanup(uuid, peer);
				break;
			}
			case rtc::PeerConnection::State::Failed: {
				peer->state = ConnectionState::Failed;
				peer->terminalStateTimeMs.store(currentTimeMs());
				logError("Peer %s connection failed", uuid.c_str());
				if (!peer->disconnectNotified.exchange(true)) {
					manager->dispatchPeerDisconnected(peer);
				}
				manager->retirePeerForDeferredCleanup(uuid, peer);
				break;
			}
			case rtc::PeerConnection::State::Closed:
				peer->state = ConnectionState::Closed;
				peer->terminalStateTimeMs.store(currentTimeMs());
				logInfo("Peer %s closed", uuid.c_str());
				if (!peer->disconnectNotified.exchange(true)) {
					manager->dispatchPeerDisconnected(peer);
				}
				manager->retirePeerForDeferredCleanup(uuid, peer);
				break;
			}
		});
	});
	registerInstalledFunction(ownerSession, PeerManagerCompletionKind::PeerConnectionState, pcHandle, [weakPc]() {
		if (const auto pc = weakPc.lock()) {
			pc->onStateChange(nullptr);
		}
	});

	peer->pc->onLocalCandidate([weakOwnerSession, weakPeer, uuid, pcHandle](rtc::Candidate candidate) {
		auto permit = PeerManagerOwnerSession::acquire(
		    weakOwnerSession, PeerManagerCompletionKind::PeerConnectionLocalCandidate, pcHandle);
		if (!permit) {
			return;
		}
		auto *manager = permit.owner();
		runRtcCallbackNoexcept("PeerConnection::onLocalCandidate", [&]() {
			if (manager->shuttingDown_) {
				return;
			}
			auto peer = weakPeer.lock();
			if (!peer)
				return;
			// Send each newly gathered candidate promptly. VDO.Ninja accepts the
			// individual candidate envelope and early trickle avoids waiting for
			// gathering completion on routes where only one candidate is produced.
			{
				std::lock_guard<std::mutex> lock(manager->candidateMutex_);
				if (peer->cleanupRetired.load() || isTerminalPeerState(peer->state.load())) {
					return;
				}
				auto &bundle = manager->candidateBundles_[peer->generation];
				bundle.session = peer->session;
				bundle.candidates.push_back({std::string(candidate), candidate.mid()});
				bundle.lastUpdate = currentTimeMs();
			}
			manager->bundleAndSendCandidates(peer);
		});
	});
	registerInstalledFunction(ownerSession, PeerManagerCompletionKind::PeerConnectionLocalCandidate, pcHandle,
	                          [weakPc]() {
		                          if (const auto pc = weakPc.lock()) {
			                          pc->onLocalCandidate(nullptr);
		                          }
	                          });

	peer->pc->onGatheringStateChange(
	    [weakOwnerSession, weakPeer, uuid, pcHandle](rtc::PeerConnection::GatheringState state) {
		    auto permit = PeerManagerOwnerSession::acquire(
		        weakOwnerSession, PeerManagerCompletionKind::PeerConnectionGatheringState, pcHandle);
		    if (!permit) {
			    return;
		    }
		    auto *manager = permit.owner();
		    runRtcCallbackNoexcept("PeerConnection::onGatheringStateChange", [&]() {
			    if (manager->shuttingDown_) {
				    return;
			    }
			    if (state == rtc::PeerConnection::GatheringState::Complete) {
				    logInfo("ICE gathering complete for %s", uuid.c_str());
				    auto peer = weakPeer.lock();
				    if (peer) {
					    manager->bundleAndSendCandidates(peer);
				    }
			    }
		    });
	    });
	registerInstalledFunction(ownerSession, PeerManagerCompletionKind::PeerConnectionGatheringState, pcHandle,
	                          [weakPc]() {
		                          if (const auto pc = weakPc.lock()) {
			                          pc->onGatheringStateChange(nullptr);
		                          }
	                          });

	peer->pc->onTrack([weakOwnerSession, weakPeer, pcHandle](std::shared_ptr<rtc::Track> track) {
		auto permit = PeerManagerOwnerSession::acquire(weakOwnerSession, PeerManagerCompletionKind::PeerConnectionTrack,
		                                               pcHandle);
		if (!permit) {
			return;
		}
		auto *manager = permit.owner();
		runRtcCallbackNoexcept("PeerConnection::onTrack", [&]() {
			if (manager->shuttingDown_) {
				return;
			}
			auto peer = weakPeer.lock();
			if (!peer)
				return;
			if (peer->cleanupRetired.load() || isTerminalPeerState(peer->state.load()) ||
			    !manager->isCurrentPeer(peer)) {
				return;
			}

			manager->handleIncomingTrack(peer, track);
		});
	});
	registerInstalledFunction(ownerSession, PeerManagerCompletionKind::PeerConnectionTrack, pcHandle, [weakPc]() {
		if (const auto pc = weakPc.lock()) {
			pc->onTrack(nullptr);
		}
	});

	peer->pc->onDataChannel([weakOwnerSession, weakPeer, pcHandle](std::shared_ptr<rtc::DataChannel> dc) {
		auto permit = PeerManagerOwnerSession::acquire(weakOwnerSession,
		                                               PeerManagerCompletionKind::PeerConnectionDataChannel, pcHandle);
		if (!permit) {
			return;
		}
		auto *manager = permit.owner();
		runRtcCallbackNoexcept("PeerConnection::onDataChannel", [&]() {
			if (manager->shuttingDown_) {
				return;
			}
			auto peer = weakPeer.lock();
			if (!peer)
				return;
			if (peer->cleanupRetired.load() || isTerminalPeerState(peer->state.load()) ||
			    !manager->isCurrentPeer(peer)) {
				return;
			}

			manager->handleIncomingDataChannel(peer, dc);
		});
	});
	registerInstalledFunction(ownerSession, PeerManagerCompletionKind::PeerConnectionDataChannel, pcHandle, [weakPc]() {
		if (const auto pc = weakPc.lock()) {
			pc->onDataChannel(nullptr);
		}
	});
}

void VDONinjaPeerManager::handleIncomingDataChannel(const std::shared_ptr<PeerInfo> &peer,
                                                    const std::shared_ptr<rtc::DataChannel> &dc,
                                                    bool allowUnregisteredPeer)
{
	if (!peer || !dc) {
		return;
	}

	const std::string label = dc->label();
	if (!label.empty() && label != "sendChannel") {
		logDebug("Ignoring non-control DataChannel '%s' from %s", label.c_str(), peer->uuid.c_str());
		clearDataChannelCallbacks(dc);
		return;
	}

	std::shared_ptr<rtc::DataChannel> retired;
	uint64_t retiredRevision = 0;
	uint64_t revision = 0;
	bool transportOpenObserved = false;
	bool rejected = false;
	{
		std::lock_guard<std::recursive_mutex> callbackMutationLock(peer->dataChannelCallbackMutationMutex);
		std::lock_guard<std::recursive_mutex> lifecycleLock(peer->dataChannelLifecycleMutex);
		std::lock_guard<std::mutex> lock(peersMutex_);
		const auto current = peers_.find(peer->uuid);
		const bool registeredCurrent = current != peers_.end() && current->second == peer;
		if ((!registeredCurrent && !allowUnregisteredPeer) || (current != peers_.end() && current->second != peer) ||
		    peer->cleanupRetired.load() || isTerminalPeerState(peer->state.load())) {
			rejected = true;
		} else {
			std::lock_guard<std::mutex> mediaLock(peer->mediaMutex);
			if (peer->dataChannel == dc) {
				return;
			}
			retired = peer->dataChannel;
			retiredRevision = peer->dataChannelRevision;
			revision = ++peer->dataChannelRevision;
			peer->dataChannel = dc;
			peer->hasDataChannel = true;
			peer->dataChannelOpenDispatched = false;
			peer->dataChannelOpenDispatchPending = false;
			for (auto it = peer->dataChannelsWithObservedOpen.begin();
			     it != peer->dataChannelsWithObservedOpen.end();) {
				const auto opened = it->lock();
				if (!opened) {
					it = peer->dataChannelsWithObservedOpen.erase(it);
					continue;
				}
				if (opened == dc) {
					transportOpenObserved = true;
				}
				++it;
			}
		}
	}
	if (rejected) {
		clearDataChannelCallbacks(dc);
		return;
	}

#if defined(VDONINJA_NATIVE_MEDIA_INTEGRATION_TEST)
	invokeNativeMediaTestDataChannelLifecycleHook(NativeMediaTestDataChannelStage::IncomingEntered, peer, dc, revision);
	invokeNativeMediaTestDataChannelLifecycleHook(NativeMediaTestDataChannelStage::BeforeCallbacksInstalled, peer, dc,
	                                              revision);
#endif

	if (retired) {
		purgeDataChannelAliasesForLease(peer->uuid, peer->generation, retired, retiredRevision);
		clearRetiredDataChannelCallbacksIfUnused(peer, retired);
	}
	if (!isDataChannelLeaseCurrent(peer, dc, peer->generation, revision, false, allowUnregisteredPeer)) {
		clearRetiredDataChannelCallbacksIfUnused(peer, dc);
		return;
	}
	installDataChannelCallbacks(peer, dc, peer->generation, revision, transportOpenObserved, allowUnregisteredPeer);
#if defined(VDONINJA_NATIVE_MEDIA_INTEGRATION_TEST)
	invokeNativeMediaTestDataChannelLifecycleHook(NativeMediaTestDataChannelStage::IncomingReturning, peer, dc,
	                                              revision);
#endif
}

bool VDONinjaPeerManager::isDataChannelLeaseCurrent(const std::shared_ptr<PeerInfo> &peer,
                                                    const std::shared_ptr<rtc::DataChannel> &dc, uint64_t generation,
                                                    uint64_t revision, bool requireOpen,
                                                    bool allowUnregisteredPeer) const
{
	if (!peer || !dc || generation == 0 || revision == 0) {
		return false;
	}
	std::lock_guard<std::mutex> lock(peersMutex_);
	const auto current = peers_.find(peer->uuid);
	const bool registeredCurrent = current != peers_.end() && current->second == peer;
	if ((!registeredCurrent && !allowUnregisteredPeer) || (current != peers_.end() && current->second != peer) ||
	    peer->generation != generation || peer->cleanupRetired.load() || isTerminalPeerState(peer->state.load())) {
		return false;
	}
	std::lock_guard<std::mutex> mediaLock(peer->mediaMutex);
	return peer->dataChannel == dc && peer->dataChannelRevision == revision &&
	       (!requireOpen || peer->dataChannelOpenDispatched);
}

bool VDONinjaPeerManager::installDataChannelCallbacks(const std::shared_ptr<PeerInfo> &peer,
                                                      const std::shared_ptr<rtc::DataChannel> &dc, uint64_t generation,
                                                      uint64_t revision, bool transportOpenObserved,
                                                      bool allowUnregisteredPeer)
{
	const std::weak_ptr<PeerInfo> weakPeer = peer;
	const std::weak_ptr<rtc::DataChannel> weakDataChannel = dc;
	const void *dataChannelHandle = dc.get();
	const auto ownerSession = ownerSession_;
	const std::weak_ptr<PeerManagerOwnerSession> weakOwnerSession = ownerSession;
	const auto callbackState = std::make_shared<DataChannelCallbackInstallState>();
	bool installed = false;
	bool closedDuringInstall = false;
	std::string installError;
	{
		// Callback mutation is distinct from lease state. libdatachannel callback setters
		// can synchronously replay open/message/terminal events, so setters must never run
		// while the lease lifecycle lock is held.
		std::lock_guard<std::recursive_mutex> callbackMutationLock(peer->dataChannelCallbackMutationMutex);
		const auto stillCurrent = [&]() {
			std::lock_guard<std::recursive_mutex> lifecycleLock(peer->dataChannelLifecycleMutex);
			return isDataChannelLeaseCurrent(peer, dc, generation, revision, false, allowUnregisteredPeer);
		};
		try {
			if (!stillCurrent()) {
				throw std::runtime_error("DataChannel lease was replaced before callback installation");
			}
			dc->onClosed([weakOwnerSession, weakPeer, weakDataChannel, callbackState, generation, revision,
			              dataChannelHandle]() {
				auto permit = PeerManagerOwnerSession::acquire(
				    weakOwnerSession, PeerManagerCompletionKind::DataChannelClosed, dataChannelHandle);
				if (!permit) {
					return;
				}
				auto *manager = permit.owner();
				{
					ScopedActiveDataChannelCallback activeCallback(weakDataChannel.lock().get());
					DataChannelCallbackInstallState::DeferredCallback dispatch =
					    [weakOwnerSession, weakPeer, weakDataChannel, generation, revision, dataChannelHandle]() {
						    auto dispatchPermit = PeerManagerOwnerSession::acquire(
						        weakOwnerSession, PeerManagerCompletionKind::DataChannelClosed, dataChannelHandle);
						    if (!dispatchPermit) {
							    return;
						    }
						    auto *dispatchManager = dispatchPermit.owner();
						    runRtcCallbackNoexcept("DataChannel::onClosed", [&]() {
							    dispatchManager->handleDataChannelTerminal(weakPeer, weakDataChannel, generation,
							                                               revision, "datachannel-closed");
						    });
					    };
					const auto action = callbackState->submit(dispatch);
					if (action == DataChannelCallbackInstallState::Action::Dispatch) {
						dispatch();
					}
				}
				manager->drainRetiredDataChannelCallbackCleanupForHandle(weakPeer, weakDataChannel);
			});
			registerInstalledFunction(ownerSession, PeerManagerCompletionKind::DataChannelClosed, dataChannelHandle,
			                          [weakDataChannel]() {
				                          if (const auto channel = weakDataChannel.lock()) {
					                          channel->onClosed(nullptr);
				                          }
			                          });
			if (!stillCurrent()) {
				throw std::runtime_error("DataChannel lease was replaced after onClosed installation");
			}
			dc->onError([weakOwnerSession, weakPeer, weakDataChannel, callbackState, generation, revision,
			             dataChannelHandle](std::string error) {
				auto permit = PeerManagerOwnerSession::acquire(
				    weakOwnerSession, PeerManagerCompletionKind::DataChannelError, dataChannelHandle);
				if (!permit) {
					return;
				}
				auto *manager = permit.owner();
				{
					ScopedActiveDataChannelCallback activeCallback(weakDataChannel.lock().get());
					DataChannelCallbackInstallState::DeferredCallback dispatch =
					    [weakOwnerSession, weakPeer, weakDataChannel, generation, revision, error = std::move(error),
					     dataChannelHandle]() {
						    auto dispatchPermit = PeerManagerOwnerSession::acquire(
						        weakOwnerSession, PeerManagerCompletionKind::DataChannelError, dataChannelHandle);
						    if (!dispatchPermit) {
							    return;
						    }
						    auto *dispatchManager = dispatchPermit.owner();
						    runRtcCallbackNoexcept("DataChannel::onError", [&]() {
							    dispatchManager->handleDataChannelTerminal(weakPeer, weakDataChannel, generation,
							                                               revision, "datachannel-error", error);
						    });
					    };
					const auto action = callbackState->submit(dispatch);
					if (action == DataChannelCallbackInstallState::Action::Dispatch) {
						dispatch();
					}
				}
				manager->drainRetiredDataChannelCallbackCleanupForHandle(weakPeer, weakDataChannel);
			});
			registerInstalledFunction(ownerSession, PeerManagerCompletionKind::DataChannelError, dataChannelHandle,
			                          [weakDataChannel]() {
				                          if (const auto channel = weakDataChannel.lock()) {
					                          channel->onError(nullptr);
				                          }
			                          });
			if (!stillCurrent()) {
				throw std::runtime_error("DataChannel lease was replaced after onError installation");
			}
#if defined(VDONINJA_NATIVE_MEDIA_INTEGRATION_TEST)
			invokeNativeMediaTestDataChannelLifecycleHook(NativeMediaTestDataChannelStage::AfterErrorCallbackInstalled,
			                                              peer, dc, revision);
#endif
			if (!stillCurrent()) {
				throw std::runtime_error("DataChannel lease was replaced during callback installation");
			}
			dc->onOpen([weakOwnerSession, weakPeer, weakDataChannel, callbackState, generation, revision,
			            dataChannelHandle]() {
				auto permit = PeerManagerOwnerSession::acquire(
				    weakOwnerSession, PeerManagerCompletionKind::DataChannelOpen, dataChannelHandle);
				if (!permit) {
					return;
				}
				auto *manager = permit.owner();
				{
					ScopedActiveDataChannelCallback activeCallback(weakDataChannel.lock().get());
					DataChannelCallbackInstallState::DeferredCallback dispatch =
					    [weakOwnerSession, weakPeer, weakDataChannel, generation, revision, dataChannelHandle]() {
						    auto dispatchPermit = PeerManagerOwnerSession::acquire(
						        weakOwnerSession, PeerManagerCompletionKind::DataChannelOpen, dataChannelHandle);
						    if (!dispatchPermit) {
							    return;
						    }
						    auto *dispatchManager = dispatchPermit.owner();
						    runRtcCallbackNoexcept("DataChannel::onOpen", [&]() {
							    dispatchManager->handleDataChannelOpen(weakPeer, weakDataChannel, generation, revision);
						    });
					    };
					const auto action = callbackState->submit(dispatch);
					if (action == DataChannelCallbackInstallState::Action::Dispatch) {
						dispatch();
					}
				}
				manager->drainRetiredDataChannelCallbackCleanupForHandle(weakPeer, weakDataChannel);
			});
			registerInstalledFunction(ownerSession, PeerManagerCompletionKind::DataChannelOpen, dataChannelHandle,
			                          [weakDataChannel]() {
				                          if (const auto channel = weakDataChannel.lock()) {
					                          channel->onOpen(nullptr);
				                          }
			                          });
			if (!stillCurrent()) {
				throw std::runtime_error("DataChannel lease was replaced after onOpen installation");
			}
			dc->onMessage([weakOwnerSession, weakPeer, weakDataChannel, callbackState, generation, revision,
			               dataChannelHandle](auto data) {
				auto permit = PeerManagerOwnerSession::acquire(
				    weakOwnerSession, PeerManagerCompletionKind::DataChannelMessage, dataChannelHandle);
				if (!permit) {
					return;
				}
				auto *manager = permit.owner();
				{
					ScopedActiveDataChannelCallback activeCallback(weakDataChannel.lock().get());
					DataChannelCallbackInstallState::DeferredCallback dispatch =
					    [weakOwnerSession, weakPeer, weakDataChannel, generation, revision, data = std::move(data),
					     dataChannelHandle]() mutable {
						    auto dispatchPermit = PeerManagerOwnerSession::acquire(
						        weakOwnerSession, PeerManagerCompletionKind::DataChannelMessage, dataChannelHandle);
						    if (!dispatchPermit) {
							    return;
						    }
						    auto *dispatchManager = dispatchPermit.owner();
						    runRtcCallbackNoexcept("DataChannel::onMessage", [&]() {
							    dispatchManager->handleDataChannelMessage(weakPeer, weakDataChannel, generation,
							                                              revision, std::move(data));
						    });
					    };
					const auto action = callbackState->submit(dispatch);
					if (action == DataChannelCallbackInstallState::Action::Dispatch) {
						dispatch();
					}
#if defined(VDONINJA_NATIVE_MEDIA_INTEGRATION_TEST)
					if (action == DataChannelCallbackInstallState::Action::DeferredDraining) {
						manager->invokeNativeMediaTestDataChannelLifecycleHook(
						    NativeMediaTestDataChannelStage::DeferredCallbackQueuedDuringDrain, weakPeer.lock(),
						    weakDataChannel.lock(), revision);
					}
#endif
				}
				manager->drainRetiredDataChannelCallbackCleanupForHandle(weakPeer, weakDataChannel);
			});
			registerInstalledFunction(ownerSession, PeerManagerCompletionKind::DataChannelMessage, dataChannelHandle,
			                          [weakDataChannel]() {
				                          if (const auto channel = weakDataChannel.lock()) {
					                          channel->onMessage(nullptr);
				                          }
			                          });
			if (!stillCurrent()) {
				throw std::runtime_error("DataChannel lease was replaced after onMessage installation");
			}
			closedDuringInstall = dc->isClosed();
			installed = !closedDuringInstall;
		} catch (const std::exception &e) {
			installError = e.what();
		}
	}

	// A previously observed transport-open must precede any messages queued while its
	// callbacks were absent. It is not itself an application-open commit; the exact
	// dispatch below still has to linearize against the current lease.
	if (installed && transportOpenObserved) {
		callbackState->prepend([weakOwnerSession, weakPeer, weakDataChannel, generation, revision,
		                        dataChannelHandle]() {
			auto permit = PeerManagerOwnerSession::acquire(weakOwnerSession, PeerManagerCompletionKind::DataChannelOpen,
			                                               dataChannelHandle);
			if (!permit) {
				return;
			}
			auto *manager = permit.owner();
			runRtcCallbackNoexcept("DataChannel::observed-open-replay", [&]() {
				manager->handleDataChannelOpen(weakPeer, weakDataChannel, generation, revision);
			});
		});
	}

	// The phase transition and all replay dispatch happen after callback mutation and
	// lifecycle locks are released. New callbacks remain queued during draining, so they
	// cannot overtake an earlier deferred open or message.
	if (!callbackState->beginDrain(installed)) {
		if (closedDuringInstall) {
			handleDataChannelTerminal(weakPeer, weakDataChannel, generation, revision,
			                          "datachannel-closed-during-callback-install");
		} else if (!installError.empty() &&
		           isDataChannelLeaseCurrent(peer, dc, generation, revision, false, allowUnregisteredPeer)) {
			logWarning("Failed to install DataChannel callbacks for %s: %s", peer->uuid.c_str(), installError.c_str());
			handleDataChannelTerminal(weakPeer, weakDataChannel, generation, revision,
			                          "datachannel-callback-install-error", installError);
		}
		clearRetiredDataChannelCallbacksIfUnused(peer, dc);
		return false;
	}
#if defined(VDONINJA_NATIVE_MEDIA_INTEGRATION_TEST)
	invokeNativeMediaTestDataChannelLifecycleHook(NativeMediaTestDataChannelStage::BeforeDeferredCallbacksDrained, peer,
	                                              dc, revision);
#endif
	callbackState->drain();
	return true;
}

void VDONinjaPeerManager::handleDataChannelOpen(const std::weak_ptr<PeerInfo> &weakPeer,
                                                const std::weak_ptr<rtc::DataChannel> &weakDataChannel,
                                                uint64_t generation, uint64_t revision)
{
	if (shuttingDown_) {
		return;
	}
	const auto peer = weakPeer.lock();
	const auto dc = weakDataChannel.lock();
	if (!peer || !dc) {
		return;
	}
	bool dispatchCurrentOpen = false;
	{
		std::lock_guard<std::recursive_mutex> lifecycleLock(peer->dataChannelLifecycleMutex);
		std::lock_guard<std::mutex> lock(peersMutex_);
		const auto current = peers_.find(peer->uuid);
		if (current == peers_.end() || current->second != peer || peer->generation != generation ||
		    peer->cleanupRetired.load() || isTerminalPeerState(peer->state.load())) {
			return;
		}
		std::lock_guard<std::mutex> mediaLock(peer->mediaMutex);
		auto &observed = peer->dataChannelsWithObservedOpen;
		observed.erase(std::remove_if(observed.begin(), observed.end(),
		                              [](const std::weak_ptr<rtc::DataChannel> &opened) { return opened.expired(); }),
		               observed.end());
		const bool alreadyRecorded =
		    std::any_of(observed.begin(), observed.end(),
		                [&dc](const std::weak_ptr<rtc::DataChannel> &opened) { return opened.lock() == dc; });
		if (!alreadyRecorded) {
			if (observed.size() >= kObservedDataChannelHistoryLimit) {
				observed.erase(observed.begin());
			}
			observed.emplace_back(dc);
		}
		if (peer->dataChannel == dc && peer->dataChannelRevision == revision && !peer->dataChannelOpenDispatched &&
		    !peer->dataChannelOpenDispatchPending) {
			peer->dataChannelOpenDispatchPending = true;
			dispatchCurrentOpen = true;
		}
	}
	if (dispatchCurrentOpen) {
		dispatchDataChannelOpen(peer, dc, generation, revision);
	}
}

void VDONinjaPeerManager::handleDataChannelMessage(const std::weak_ptr<PeerInfo> &weakPeer,
                                                   const std::weak_ptr<rtc::DataChannel> &weakDataChannel,
                                                   uint64_t generation, uint64_t revision, rtc::message_variant data)
{
	if (shuttingDown_ || !std::holds_alternative<std::string>(data)) {
		return;
	}
	const auto peer = weakPeer.lock();
	const auto dc = weakDataChannel.lock();
	if (!peer || !dc) {
		return;
	}
	{
		std::lock_guard<std::recursive_mutex> lifecycleLock(peer->dataChannelLifecycleMutex);
		if (!isDataChannelLeaseCurrent(peer, dc, generation, revision, true)) {
			return;
		}
	}
	dispatchDataChannelMessage(peer, std::get<std::string>(data), dc, generation, revision);
}

void VDONinjaPeerManager::handleDataChannelTerminal(const std::weak_ptr<PeerInfo> &weakPeer,
                                                    const std::weak_ptr<rtc::DataChannel> &weakDataChannel,
                                                    uint64_t generation, uint64_t revision, const char *reason,
                                                    const std::string &error)
{
	const auto peer = weakPeer.lock();
	const auto dc = weakDataChannel.lock();
	if (!peer || !dc) {
		return;
	}
	bool retired = false;
	{
		std::lock_guard<std::recursive_mutex> lifecycleLock(peer->dataChannelLifecycleMutex);
		{
			std::lock_guard<std::mutex> lock(peersMutex_);
			const auto current = peers_.find(peer->uuid);
			if (current == peers_.end() || current->second != peer || peer->generation != generation) {
				return;
			}
			std::lock_guard<std::mutex> mediaLock(peer->mediaMutex);
			if (peer->dataChannel != dc || peer->dataChannelRevision != revision) {
				return;
			}
			peer->dataChannel.reset();
			peer->hasDataChannel = false;
			peer->dataChannelOpenDispatched = false;
			peer->dataChannelOpenDispatchPending = false;
			peer->dataChannelsWithObservedOpen.erase(
			    std::remove_if(peer->dataChannelsWithObservedOpen.begin(), peer->dataChannelsWithObservedOpen.end(),
			                   [&dc](const std::weak_ptr<rtc::DataChannel> &opened) {
				                   const auto handle = opened.lock();
				                   return !handle || handle == dc;
			                   }),
			    peer->dataChannelsWithObservedOpen.end());
			++peer->dataChannelRevision;
			retired = true;
		}
	}
	if (!retired) {
		return;
	}
	purgeDataChannelAliasesForLease(peer->uuid, generation, dc, revision);
	if (!error.empty()) {
		logWarning("DataChannel for %s failed (%s): %s", peer->uuid.c_str(), reason ? reason : "terminal",
		           error.c_str());
		try {
			dc->close();
		} catch (const std::exception &) {
		}
	}
	clearRetiredDataChannelCallbacksIfUnused(peer, dc);
}

void VDONinjaPeerManager::clearRetiredDataChannelCallbacksIfUnused(const std::shared_ptr<PeerInfo> &peer,
                                                                   const std::shared_ptr<rtc::DataChannel> &dc)
{
	if (!peer || !dc) {
		return;
	}
#if defined(VDONINJA_NATIVE_MEDIA_INTEGRATION_TEST)
	invokeNativeMediaTestDataChannelLifecycleHook(NativeMediaTestDataChannelStage::BeforeCallbackCleanup, peer, dc, 0);
#endif
	if (activeManagerDataChannelCallback == dc.get()) {
		std::lock_guard<std::mutex> mediaLock(peer->mediaMutex);
		const auto &pending = peer->retiredDataChannelsPendingCallbackCleanup;
		if (std::find(pending.begin(), pending.end(), dc) == pending.end()) {
			peer->retiredDataChannelsPendingCallbackCleanup.push_back(dc);
		}
		return;
	}

	std::lock_guard<std::recursive_mutex> callbackMutationLock(peer->dataChannelCallbackMutationMutex);
	bool currentHandle = false;
	{
		std::lock_guard<std::recursive_mutex> lifecycleLock(peer->dataChannelLifecycleMutex);
		{
			std::lock_guard<std::mutex> lock(peersMutex_);
			const auto current = peers_.find(peer->uuid);
			if (current != peers_.end() && current->second == peer && !peer->cleanupRetired.load() &&
			    !isTerminalPeerState(peer->state.load())) {
				std::lock_guard<std::mutex> mediaLock(peer->mediaMutex);
				if (peer->dataChannel == dc) {
					currentHandle = true;
				}
			}
		}
	}
	if (!currentHandle) {
		clearDataChannelCallbacks(dc);
	}
	{
		std::lock_guard<std::mutex> mediaLock(peer->mediaMutex);
		auto &pending = peer->retiredDataChannelsPendingCallbackCleanup;
		pending.erase(std::remove(pending.begin(), pending.end(), dc), pending.end());
	}
}

void VDONinjaPeerManager::drainRetiredDataChannelCallbackCleanupForHandle(
    const std::weak_ptr<PeerInfo> &weakPeer, const std::weak_ptr<rtc::DataChannel> &weakDataChannel)
{
	const auto peer = weakPeer.lock();
	const auto dc = weakDataChannel.lock();
	if (!peer || !dc) {
		return;
	}
	bool pending = false;
	{
		std::lock_guard<std::mutex> mediaLock(peer->mediaMutex);
		const auto &retired = peer->retiredDataChannelsPendingCallbackCleanup;
		pending = std::find(retired.begin(), retired.end(), dc) != retired.end();
	}
	if (pending) {
		clearRetiredDataChannelCallbacksIfUnused(peer, dc);
	}
}

void VDONinjaPeerManager::purgeDataChannelAliasesForLease(const std::string &transportUuid,
                                                          uint64_t transportGeneration,
                                                          const std::shared_ptr<rtc::DataChannel> &dc,
                                                          uint64_t revision)
{
	if (transportUuid.empty() || transportGeneration == 0 || !dc || revision == 0) {
		return;
	}
	std::lock_guard<std::mutex> aliasLock(dataChannelAliasMutex_);
	std::lock_guard<std::mutex> lock(peersMutex_);
	for (const auto &entry : peers_) {
		if (!entry.second) {
			continue;
		}
		std::lock_guard<std::mutex> mediaLock(entry.second->mediaMutex);
		if (entry.second->signalingDataChannel == dc &&
		    entry.second->signalingDataChannelTransportUuid == transportUuid &&
		    entry.second->signalingDataChannelTransportGeneration == transportGeneration &&
		    entry.second->signalingDataChannelRevision == revision) {
			entry.second->signalingDataChannel.reset();
			entry.second->signalingDataChannelTransportUuid.clear();
			entry.second->signalingDataChannelTransportGeneration = 0;
			entry.second->signalingDataChannelRevision = 0;
		}
	}
	std::lock_guard<std::mutex> pendingLock(pendingViewerSignalingMutex_);
	for (auto it = pendingViewerSignalingDataChannels_.begin(); it != pendingViewerSignalingDataChannels_.end();) {
		const auto &route = it->second;
		if (route.channel == dc && route.transportUuid == transportUuid &&
		    route.transportGeneration == transportGeneration && route.dataChannelRevision == revision) {
			it = pendingViewerSignalingDataChannels_.erase(it);
		} else {
			++it;
		}
	}
}

void VDONinjaPeerManager::retirePeerDataChannel(const std::shared_ptr<PeerInfo> &peer)
{
	if (!peer) {
		return;
	}
	std::shared_ptr<rtc::DataChannel> retired;
	uint64_t retiredRevision = 0;
	{
		std::lock_guard<std::recursive_mutex> lifecycleLock(peer->dataChannelLifecycleMutex);
		{
			std::lock_guard<std::mutex> aliasLock(dataChannelAliasMutex_);
			std::lock_guard<std::mutex> lock(peersMutex_);
			{
				std::lock_guard<std::mutex> mediaLock(peer->mediaMutex);
				retired = peer->dataChannel;
				retiredRevision = peer->dataChannelRevision;
				if (retired) {
					peer->dataChannel.reset();
					peer->hasDataChannel = false;
					peer->dataChannelOpenDispatched = false;
					peer->dataChannelOpenDispatchPending = false;
					++peer->dataChannelRevision;
				}
				peer->signalingDataChannel.reset();
				peer->signalingDataChannelTransportUuid.clear();
				peer->signalingDataChannelTransportGeneration = 0;
				peer->signalingDataChannelRevision = 0;
				peer->dataChannelsWithObservedOpen.clear();
			}
			if (retired) {
				for (const auto &entry : peers_) {
					if (!entry.second || entry.second == peer) {
						continue;
					}
					std::lock_guard<std::mutex> mediaLock(entry.second->mediaMutex);
					if (entry.second->signalingDataChannel == retired &&
					    entry.second->signalingDataChannelTransportUuid == peer->uuid &&
					    entry.second->signalingDataChannelTransportGeneration == peer->generation &&
					    entry.second->signalingDataChannelRevision == retiredRevision) {
						entry.second->signalingDataChannel.reset();
						entry.second->signalingDataChannelTransportUuid.clear();
						entry.second->signalingDataChannelTransportGeneration = 0;
						entry.second->signalingDataChannelRevision = 0;
					}
				}
				std::lock_guard<std::mutex> pendingLock(pendingViewerSignalingMutex_);
				for (auto it = pendingViewerSignalingDataChannels_.begin();
				     it != pendingViewerSignalingDataChannels_.end();) {
					const auto &route = it->second;
					if (route.channel == retired && route.transportUuid == peer->uuid &&
					    route.transportGeneration == peer->generation && route.dataChannelRevision == retiredRevision) {
						it = pendingViewerSignalingDataChannels_.erase(it);
					} else {
						++it;
					}
				}
			}
		}
	}
	if (retired) {
		clearRetiredDataChannelCallbacksIfUnused(peer, retired);
	}
}

#if defined(VDONINJA_NATIVE_MEDIA_INTEGRATION_TEST)
void VDONinjaPeerManager::invokeNativeMediaTestDataChannelLifecycleHook(NativeMediaTestDataChannelStage stage,
                                                                        const std::shared_ptr<PeerInfo> &peer,
                                                                        const std::shared_ptr<rtc::DataChannel> &dc,
                                                                        uint64_t revision)
{
	NativeMediaTestDataChannelLifecycleHook hook;
	{
		std::lock_guard<std::mutex> callbackLock(callbackMutex_);
		hook = nativeMediaTestDataChannelLifecycleHook_;
	}
	if (hook) {
		hook(stage, peer, dc, revision);
	}
}
#endif

void VDONinjaPeerManager::consumePendingViewerSignalingDataChannel(const std::shared_ptr<PeerInfo> &peer,
                                                                   const std::string &session)
{
	if (!peer) {
		return;
	}
	ViewerSignalingDataChannelRoute route;
	{
		std::lock_guard<std::mutex> aliasLock(dataChannelAliasMutex_);
		std::lock_guard<std::mutex> lock(pendingViewerSignalingMutex_);
		auto it = pendingViewerSignalingDataChannels_.find(viewerSignalingKey(peer->uuid, session));
		if (it == pendingViewerSignalingDataChannels_.end()) {
			return;
		}
		route = it->second;
		pendingViewerSignalingDataChannels_.erase(it);
	}
#if defined(VDONINJA_NATIVE_MEDIA_INTEGRATION_TEST)
	invokeNativeMediaTestDataChannelLifecycleHook(NativeMediaTestDataChannelStage::BeforePendingAliasConsume, peer,
	                                              route.channel, route.dataChannelRevision);
#endif
	std::lock_guard<std::mutex> aliasLock(dataChannelAliasMutex_);
	std::lock_guard<std::mutex> lock(peersMutex_);
	const auto targetIt = peers_.find(peer->uuid);
	const auto transportIt = peers_.find(route.transportUuid);
	if (targetIt == peers_.end() || targetIt->second != peer || transportIt == peers_.end() || !transportIt->second ||
	    transportIt->second->generation != route.transportGeneration) {
		return;
	}
	const auto transport = transportIt->second;
	std::unique_lock<std::mutex> transportMediaLock(transport->mediaMutex);
	if (transport->dataChannel != route.channel || transport->dataChannelRevision != route.dataChannelRevision ||
	    !transport->dataChannelOpenDispatched) {
		return;
	}
	if (transport == peer) {
		peer->signalingDataChannel = route.channel;
		peer->signalingDataChannelTransportUuid = route.transportUuid;
		peer->signalingDataChannelTransportGeneration = route.transportGeneration;
		peer->signalingDataChannelRevision = route.dataChannelRevision;
		return;
	}
	std::lock_guard<std::mutex> targetMediaLock(peer->mediaMutex);
	peer->signalingDataChannel = route.channel;
	peer->signalingDataChannelTransportUuid = route.transportUuid;
	peer->signalingDataChannelTransportGeneration = route.transportGeneration;
	peer->signalingDataChannelRevision = route.dataChannelRevision;
}

void VDONinjaPeerManager::dispatchPeerDisconnected(const std::shared_ptr<PeerInfo> &peer)
{
	if (!peer || peer->cleanupRetired.load() || !isCurrentPeer(peer)) {
		return;
	}

	const PeerEventIdentity identity = nextPeerEventIdentity(peer);
	OnPeerDisconnectedCallback callback;
#if defined(VDONINJA_NATIVE_MEDIA_INTEGRATION_TEST)
	NativeMediaTestPeerDispatchHook testHook;
#endif
	{
		std::lock_guard<std::mutex> callbackLock(callbackMutex_);
		callback = onPeerDisconnected_;
#if defined(VDONINJA_NATIVE_MEDIA_INTEGRATION_TEST)
		testHook = nativeMediaTestPeerDisconnectDispatchHook_;
#endif
	}
#if defined(VDONINJA_NATIVE_MEDIA_INTEGRATION_TEST)
	if (testHook) {
		testHook(peer);
	}
#endif
	if (callback) {
		callback(identity);
	}
}

void VDONinjaPeerManager::dispatchDataChannelOpen(const std::shared_ptr<PeerInfo> &peer,
                                                  const std::shared_ptr<rtc::DataChannel> &dc, uint64_t generation,
                                                  uint64_t revision)
{
	if (!peer || !dc || peer->cleanupRetired.load() || isTerminalPeerState(peer->state.load()) ||
	    !isCurrentPeer(peer)) {
		return;
	}

	const PeerEventIdentity identity = nextPeerEventIdentity(peer);
	OnDataChannelCallback callback;
#if defined(VDONINJA_NATIVE_MEDIA_INTEGRATION_TEST)
	NativeMediaTestPeerDispatchHook testHook;
#endif
	{
		std::lock_guard<std::mutex> callbackLock(callbackMutex_);
		callback = onDataChannel_;
#if defined(VDONINJA_NATIVE_MEDIA_INTEGRATION_TEST)
		testHook = nativeMediaTestPeerDataOpenDispatchHook_;
#endif
	}
#if defined(VDONINJA_NATIVE_MEDIA_INTEGRATION_TEST)
	if (testHook) {
		testHook(peer);
	}
#endif
	bool committed = false;
	{
		std::lock_guard<std::recursive_mutex> lifecycleLock(peer->dataChannelLifecycleMutex);
		std::lock_guard<std::mutex> lock(peersMutex_);
		const auto current = peers_.find(peer->uuid);
		if (current != peers_.end() && current->second == peer && peer->generation == generation &&
		    !peer->cleanupRetired.load() && !isTerminalPeerState(peer->state.load())) {
			std::lock_guard<std::mutex> mediaLock(peer->mediaMutex);
			if (peer->dataChannel == dc && peer->dataChannelRevision == revision && !peer->dataChannelOpenDispatched &&
			    peer->dataChannelOpenDispatchPending) {
				// This is the application-open linearization point. Transport-open
				// observation is retained separately and never authorizes messages.
				peer->dataChannelOpenDispatched = true;
				peer->dataChannelOpenDispatchPending = false;
				committed = true;
			}
		}
	}
	if (!committed) {
		return;
	}
	logInfo("Data channel opened with %s", peer->uuid.c_str());
	if (callback) {
		callback(identity, dc);
	}
}

void VDONinjaPeerManager::dispatchDataChannelMessage(const std::shared_ptr<PeerInfo> &peer, const std::string &message,
                                                     const std::shared_ptr<rtc::DataChannel> &dc, uint64_t generation,
                                                     uint64_t revision)
{
	if (!peer || peer->cleanupRetired.load() || isTerminalPeerState(peer->state.load()) || !isCurrentPeer(peer)) {
		return;
	}

	const PeerEventIdentity identity = nextPeerEventIdentity(peer);
	OnDataChannelMessageCallback callback;
#if defined(VDONINJA_NATIVE_MEDIA_INTEGRATION_TEST)
	NativeMediaTestPeerDispatchHook testHook;
	NativeMediaTestPeerDispatchHook completionHook;
#endif
	{
		std::lock_guard<std::mutex> callbackLock(callbackMutex_);
		callback = onDataChannelMessage_;
#if defined(VDONINJA_NATIVE_MEDIA_INTEGRATION_TEST)
		testHook = nativeMediaTestPeerDataDispatchHook_;
		completionHook = nativeMediaTestPeerDataDispatchCompleteHook_;
#endif
	}
#if defined(VDONINJA_NATIVE_MEDIA_INTEGRATION_TEST)
	if (testHook) {
		testHook(peer);
	}
#endif
	bool exactCurrent = true;
	if (generation != 0 && revision != 0) {
		std::lock_guard<std::recursive_mutex> lifecycleLock(peer->dataChannelLifecycleMutex);
		exactCurrent = isDataChannelLeaseCurrent(peer, dc, generation, revision, true);
	}
	if (exactCurrent && callback) {
		callback(identity, message);
	}
#if defined(VDONINJA_NATIVE_MEDIA_INTEGRATION_TEST)
	if (completionHook) {
		completionHook(peer);
	}
#endif
}

void VDONinjaPeerManager::handleIncomingTrack(const std::shared_ptr<PeerInfo> &peer,
                                              const std::shared_ptr<rtc::Track> &track)
{
	if (!peer || peer->cleanupRetired.load() || isTerminalPeerState(peer->state.load()) || !isCurrentPeer(peer)) {
		clearTrackCallbacks(track);
		return;
	}

	TrackType type = TrackType::Video;
	TrackSlotEvent event;
	bool rejected = false;
	{
		std::lock_guard<std::mutex> mediaLock(peer->mediaMutex);
		if (peer->cleanupRetired.load() || isTerminalPeerState(peer->state.load())) {
			rejected = true;
		} else {
			type = classifyIncomingTrack(peer, track);
			std::shared_ptr<rtc::Track> *slot = &peer->videoTrack;
			uint64_t *revision = &peer->videoTrackRevision;
			if (type == TrackType::Audio) {
				slot = &peer->audioTrack;
				revision = &peer->audioTrackRevision;
			} else if (type == TrackType::AlphaVideo) {
				slot = &peer->alphaVideoTrack;
				revision = &peer->alphaVideoTrackRevision;
			}
			if (*slot == track) {
				return;
			}
			event = {
			    peer->uuid, peer->session, type, peer->generation, ++(*revision), nextPeerEventIdentity(peer).sequence,
			    track,      *slot};
			*slot = track;
		}
	}
	if (rejected) {
		clearTrackCallbacks(track);
		return;
	}

	const char *typeLabel =
	    type == TrackType::Audio ? "audio" : (type == TrackType::AlphaVideo ? "alpha video" : "video");
	logInfo("Received %s track from %s (mid=%s)", typeLabel, peer->uuid.c_str(), track ? track->mid().c_str() : "");
	dispatchCommittedTrackSlotEvent(peer, event);
}

bool VDONinjaPeerManager::updateTrackSlot(const std::shared_ptr<PeerInfo> &peer, TrackType type,
                                          const std::shared_ptr<rtc::Track> &track, TrackSlotEvent &event,
                                          const std::shared_ptr<rtc::Track> &expectedTrack, bool requireExpected,
                                          uint64_t expectedRevision)

{
	if (!peer) {
		return false;
	}

	std::lock_guard<std::mutex> mediaLock(peer->mediaMutex);
	if (track && (peer->cleanupRetired.load() || isTerminalPeerState(peer->state.load()))) {
		return false;
	}
	std::shared_ptr<rtc::Track> *slot = &peer->videoTrack;
	uint64_t *revision = &peer->videoTrackRevision;
	if (type == TrackType::Audio) {
		slot = &peer->audioTrack;
		revision = &peer->audioTrackRevision;
	} else if (type == TrackType::AlphaVideo) {
		slot = &peer->alphaVideoTrack;
		revision = &peer->alphaVideoTrackRevision;
	}
	if (*slot == track) {
		return false;
	}
	if (requireExpected && *slot != expectedTrack) {
		return false;
	}
	if (expectedRevision != 0 && *revision != expectedRevision) {
		return false;
	}

	event.uuid = peer->uuid;
	event.session = peer->session;
	event.type = type;
	event.generation = peer->generation;
	event.revision = ++(*revision);
	event.sequence = nextPeerEventIdentity(peer).sequence;
	event.track = track;
	event.retiredTrack = *slot;
	*slot = track;
	return true;
}

bool VDONinjaPeerManager::isTrackSlotLeaseCurrent(const std::shared_ptr<PeerInfo> &peer, TrackType type,
                                                  const std::shared_ptr<rtc::Track> &track, uint64_t generation,
                                                  uint64_t revision) const
{
	if (!peer || !track || generation == 0 || revision == 0) {
		return false;
	}

	std::lock_guard<std::mutex> peersLock(peersMutex_);
	const auto peerIt = peers_.find(peer->uuid);
	if (peerIt == peers_.end() || peerIt->second != peer || peer->generation != generation ||
	    peer->cleanupRetired.load()) {
		return false;
	}

	std::lock_guard<std::mutex> mediaLock(peer->mediaMutex);
	const std::shared_ptr<rtc::Track> *slot = &peer->videoTrack;
	const uint64_t *slotRevision = &peer->videoTrackRevision;
	if (type == TrackType::Audio) {
		slot = &peer->audioTrack;
		slotRevision = &peer->audioTrackRevision;
	} else if (type == TrackType::AlphaVideo) {
		slot = &peer->alphaVideoTrack;
		slotRevision = &peer->alphaVideoTrackRevision;
	}
	return *slot == track && *slotRevision == revision;
}

void VDONinjaPeerManager::handleTrackTerminal(const std::weak_ptr<PeerInfo> &weakPeer, TrackType type,
                                              const std::weak_ptr<rtc::Track> &weakTrack, uint64_t generation,
                                              uint64_t revision, const char *reason, const std::string &error)
{
	auto peer = weakPeer.lock();
	auto track = weakTrack.lock();
	if (!peer || !track) {
		return;
	}

#if defined(VDONINJA_NATIVE_MEDIA_INTEGRATION_TEST)
	NativeMediaTestTrackLifecycleHook lifecycleHook;
	{
		std::lock_guard<std::mutex> callbackLock(callbackMutex_);
		lifecycleHook = nativeMediaTestTrackLifecycleHook_;
	}
	if (lifecycleHook) {
		lifecycleHook(peer, type, track, revision);
	}
#endif

	TrackSlotEvent event;
	{
		std::lock_guard<std::mutex> peersLock(peersMutex_);
		const auto peerIt = peers_.find(peer->uuid);
		if (peerIt == peers_.end() || peerIt->second != peer || peer->generation != generation ||
		    peer->cleanupRetired.load()) {
			return;
		}

		std::lock_guard<std::mutex> mediaLock(peer->mediaMutex);
		std::shared_ptr<rtc::Track> *slot = &peer->videoTrack;
		uint64_t *slotRevision = &peer->videoTrackRevision;
		if (type == TrackType::Audio) {
			slot = &peer->audioTrack;
			slotRevision = &peer->audioTrackRevision;
		} else if (type == TrackType::AlphaVideo) {
			slot = &peer->alphaVideoTrack;
			slotRevision = &peer->alphaVideoTrackRevision;
		}
		if (*slot != track || *slotRevision != revision) {
			return;
		}

		event.uuid = peer->uuid;
		event.session = peer->session;
		event.type = type;
		event.generation = peer->generation;
		event.revision = ++(*slotRevision);
		event.sequence = nextPeerEventIdentity(peer).sequence;
		event.retiredTrack = track;
		slot->reset();
	}

	const char *typeLabel =
	    type == TrackType::Audio ? "audio" : (type == TrackType::AlphaVideo ? "alpha video" : "video");
	if (!error.empty()) {
		logWarning("Native receiver %s track for %s retired after %s: %s", typeLabel, peer->uuid.c_str(),
		           reason ? reason : "terminal event", error.c_str());
		// libdatachannel can report a terminal media error without marking the
		// Track closed. Closing the exact retired handle prevents a later
		// emplaceTrack(mid) renegotiation from reusing the failed implementation.
		try {
			track->close();
		} catch (const std::exception &) {
		}
	} else {
		logInfo("Native receiver %s track for %s retired after %s", typeLabel, peer->uuid.c_str(),
		        reason ? reason : "terminal event");
	}
	dispatchTrackSlotEvent(event);
}

bool VDONinjaPeerManager::installTrackLifecycleCallbacks(const std::shared_ptr<PeerInfo> &peer,
                                                         const TrackSlotEvent &event)
{
	if (!peer || !event.track || event.generation == 0 || event.revision == 0) {
		return false;
	}

	const auto weakPeer = std::weak_ptr<PeerInfo>(peer);
	const auto weakTrack = std::weak_ptr<rtc::Track>(event.track);
	const void *trackHandle = event.track.get();
	const auto ownerSession = ownerSession_;
	const std::weak_ptr<PeerManagerOwnerSession> weakOwnerSession = ownerSession;
	const TrackType type = event.type;
	const uint64_t generation = event.generation;
	const uint64_t revision = event.revision;
	event.track->onClosed([weakOwnerSession, weakPeer, weakTrack, type, generation, revision, trackHandle]() {
		auto permit =
		    PeerManagerOwnerSession::acquire(weakOwnerSession, PeerManagerCompletionKind::TrackClosed, trackHandle);
		if (!permit) {
			return;
		}
		auto *manager = permit.owner();
		runRtcCallbackNoexcept("Track::onClosed", [&]() {
			manager->handleTrackTerminal(weakPeer, type, weakTrack, generation, revision, "track-closed");
		});
	});
	registerInstalledFunction(ownerSession, PeerManagerCompletionKind::TrackClosed, trackHandle, [weakTrack]() {
		if (const auto track = weakTrack.lock()) {
			track->onClosed(nullptr);
		}
	});
	if (!isTrackSlotLeaseCurrent(peer, type, event.track, generation, revision)) {
		return false;
	}
	event.track->onError(
	    [weakOwnerSession, weakPeer, weakTrack, type, generation, revision, trackHandle](std::string error) {
		    auto permit =
		        PeerManagerOwnerSession::acquire(weakOwnerSession, PeerManagerCompletionKind::TrackError, trackHandle);
		    if (!permit) {
			    return;
		    }
		    auto *manager = permit.owner();
		    runRtcCallbackNoexcept("Track::onError", [&]() {
			    manager->handleTrackTerminal(weakPeer, type, weakTrack, generation, revision, "track-error", error);
		    });
	    });
	registerInstalledFunction(ownerSession, PeerManagerCompletionKind::TrackError, trackHandle, [weakTrack]() {
		if (const auto track = weakTrack.lock()) {
			track->onError(nullptr);
		}
	});
	return isTrackSlotLeaseCurrent(peer, type, event.track, generation, revision);
}

void VDONinjaPeerManager::dispatchCommittedTrackSlotEvent(const std::shared_ptr<PeerInfo> &peer,
                                                          const TrackSlotEvent &event)
{
	if (!event.track) {
		dispatchTrackSlotEvent(event);
		return;
	}

#if defined(VDONINJA_NATIVE_MEDIA_INTEGRATION_TEST)
	NativeMediaTestTrackHandleHook beforeInstallHook;
	{
		std::lock_guard<std::mutex> callbackLock(callbackMutex_);
		beforeInstallHook = nativeMediaTestTrackBeforeInstallHook_;
	}
	if (beforeInstallHook) {
		beforeInstallHook(event.track);
	}
#endif
	if (!installTrackLifecycleCallbacks(peer, event)) {
		clearRetiredTrackCallbacksIfUnused(event);
		return;
	}
#if defined(VDONINJA_NATIVE_MEDIA_INTEGRATION_TEST)
	NativeMediaTestTrackCommitHook commitHook;
	{
		std::lock_guard<std::mutex> callbackLock(callbackMutex_);
		commitHook = nativeMediaTestTrackCommitHook_;
	}
	if (commitHook && event.track) {
		commitHook(event.uuid, event.type, event.track, event.generation);
	}
#endif

	if (event.track->isClosed()) {
		handleTrackTerminal(peer, event.type, event.track, event.generation, event.revision,
		                    "closed-during-lifecycle-install");
	}
	if (!isTrackSlotLeaseCurrent(peer, event.type, event.track, event.generation, event.revision)) {
		clearRetiredTrackCallbacksIfUnused(event);
		return;
	}
	dispatchTrackSlotEvent(event);
}

void VDONinjaPeerManager::dispatchTrackSlotEvent(const TrackSlotEvent &event)
{

	OnTrackCallback callback;
#if defined(VDONINJA_NATIVE_MEDIA_INTEGRATION_TEST)
	NativeMediaTestTrackDispatchHook dispatchHook;
#endif
	{
		std::lock_guard<std::mutex> callbackLock(callbackMutex_);
		callback = onTrack_;
#if defined(VDONINJA_NATIVE_MEDIA_INTEGRATION_TEST)
		dispatchHook = nativeMediaTestTrackDispatchHook_;
#endif
	}
#if defined(VDONINJA_NATIVE_MEDIA_INTEGRATION_TEST)
	if (dispatchHook) {
		dispatchHook(event, static_cast<bool>(callback));
	}
#endif
	if (callback) {
		callback(event);
	}
	clearRetiredTrackCallbacksIfUnused(event);
}

void VDONinjaPeerManager::clearRetiredTrackCallbacksIfUnused(const TrackSlotEvent &event)
{
	struct CurrentLease {
		std::shared_ptr<PeerInfo> peer;
		TrackSlotEvent event;
	};
	const auto currentLeaseForHandle =
	    [this, &event](const std::shared_ptr<rtc::Track> &handle) -> std::optional<CurrentLease> {
		std::lock_guard<std::mutex> peersLock(peersMutex_);
		const auto peerIt = peers_.find(event.uuid);
		if (peerIt == peers_.end() || !peerIt->second || peerIt->second->cleanupRetired.load()) {
			return std::nullopt;
		}
		const auto &peer = peerIt->second;
		std::lock_guard<std::mutex> mediaLock(peer->mediaMutex);
		TrackType type = TrackType::Video;
		uint64_t revision = peer->videoTrackRevision;
		if (peer->videoTrack != handle) {
			if (peer->alphaVideoTrack == handle) {
				type = TrackType::AlphaVideo;
				revision = peer->alphaVideoTrackRevision;
			} else if (peer->audioTrack == handle) {
				type = TrackType::Audio;
				revision = peer->audioTrackRevision;
			} else {
				return std::nullopt;
			}
		}
		return CurrentLease{peer, {peer->uuid, peer->session, type, peer->generation, revision, 0, handle, nullptr}};
	};

	// Callback setters synchronize with callbacks already in flight, so they
	// must never run under peersMutex_ or mediaMutex. If a same-handle re-add
	// races the unlocked detach, re-check and reinstall the exact current lease;
	// libdatachannel's stored callbacks replay any close/error from that gap.
	std::shared_ptr<rtc::Track> previousHandle;
	for (const auto &handle : {event.track, event.retiredTrack}) {
		if (!handle || handle == previousHandle) {
			continue;
		}
		previousHandle = handle;
		if (currentLeaseForHandle(handle)) {
			continue;
		}
#if defined(VDONINJA_NATIVE_MEDIA_INTEGRATION_TEST)
		NativeMediaTestTrackHandleHook cleanupHook;
		{
			std::lock_guard<std::mutex> callbackLock(callbackMutex_);
			cleanupHook = nativeMediaTestTrackCleanupHook_;
		}
		if (cleanupHook) {
			cleanupHook(handle);
		}
#endif
		while (true) {
			clearTrackLifecycleCallbacks(handle);
			const auto current = currentLeaseForHandle(handle);
			if (!current) {
				break;
			}
			if (installTrackLifecycleCallbacks(current->peer, current->event)) {
				break;
			}
			// The captured lease was replaced while callbacks were installed.
			// Remove that stale capture and converge on whichever exact lease is
			// current now; its replacement event owns cleanup after success.
		}
	}
}

void VDONinjaPeerManager::clearTrackSlots(const std::shared_ptr<PeerInfo> &peer)
{
	if (!peer) {
		return;
	}
	std::vector<TrackSlotEvent> events;
	for (const TrackType type : {TrackType::Video, TrackType::AlphaVideo, TrackType::Audio}) {
		TrackSlotEvent event;
		if (updateTrackSlot(peer, type, nullptr, event)) {
			events.push_back(std::move(event));
		}
	}
	for (const auto &event : events) {
		dispatchTrackSlotEvent(event);
	}
}

void VDONinjaPeerManager::setupPublisherTracks(std::shared_ptr<PeerInfo> peer)
{
	// Set up video track
	rtc::Description::Video videoDesc("video", rtc::Description::Direction::SendOnly);
	std::string h264ProfileLevelId;
	{
		std::lock_guard<std::mutex> codecLock(codecMutex_);
		h264ProfileLevelId = h264ProfileLevelId_;
	}
	// Keep the SDP offer on libdatachannel's WebRTC compatibility profile.
	// Advertising the encoder's High profile here prevents some VDO.Ninja
	// browser viewers from completing peer connection setup on macOS.
	videoDesc.addH264Codec(kH264PayloadType);
	videoDesc.addSSRC(videoSsrc_, "video-stream");
	const auto videoTrack = peer->pc->addTrack(videoDesc);
	{
		std::lock_guard<std::mutex> mediaLock(peer->mediaMutex);
		peer->videoTrack = videoTrack;
	}
	peer->videoRtpConfig =
	    std::make_shared<rtc::RtpPacketizationConfig>(videoSsrc_, "video-stream", kH264PayloadType, kVideoClockRate);
	peer->videoRtpConfig->sequenceNumber = peer->videoSeq;
	peer->videoRtpConfig->timestamp = peer->videoTimestamp;
	peer->videoSrReporter = std::make_shared<rtc::RtcpSrReporter>(peer->videoRtpConfig);
	peer->videoFeedbackTracker = std::make_shared<RtcpFeedbackTracker>(videoSsrc_);
	auto weakPeer = std::weak_ptr<PeerInfo>(peer);
	const auto ownerSession = ownerSession_;
	const std::weak_ptr<PeerManagerOwnerSession> weakOwnerSession = ownerSession;
	const void *videoFeedbackHandle = videoTrack.get();
	std::function<void()> videoFeedbackCompletion = [weakOwnerSession, weakPeer, uuid = peer->uuid,
	                                                 videoFeedbackHandle]() {
		auto permit = PeerManagerOwnerSession::acquire(weakOwnerSession, PeerManagerCompletionKind::VideoFeedback,
		                                               videoFeedbackHandle);
		if (!permit) {
			return;
		}
		auto *manager = permit.owner();
		runRtcCallbackNoexcept("PliHandler", [&]() {
			if (manager->shuttingDown_) {
				return;
			}
			auto peer = weakPeer.lock();
			if (!peer || peer->cleanupRetired.load() || isTerminalPeerState(peer->state.load()) ||
			    !manager->isCurrentPeer(peer)) {
				return;
			}
			size_t discardedFrames = 0;
			size_t discardedPackets = 0;
			{
				std::lock_guard<std::mutex> sendLock(peer->videoSendMutex);
				std::shared_ptr<RtpPacketPacer> pacer;
				VideoKeyframeGate::DecoderRequestDisposition disposition;
				{
					std::lock_guard<std::mutex> mediaLock(peer->mediaMutex);
					if (peer->cleanupRetired.load() || isTerminalPeerState(peer->state.load())) {
						return;
					}
					disposition = peer->videoKeyframeGate.onDecoderKeyframeRequest();
					pacer = peer->videoPacer;
				}
				if (pacer && disposition == VideoKeyframeGate::DecoderRequestDisposition::RequireNewLiveKeyframe) {
					discardedFrames = pacer->discardQueuedMediaFramesAfterCurrent(&discardedPackets);
					if (discardedPackets != 0) {
						std::lock_guard<std::mutex> mediaLock(peer->mediaMutex);
						if (peer->videoPacer == pacer) {
							reclaimDiscardedVideoSequenceNumbers(*peer, discardedPackets);
						}
					}
				}
			}
			if (discardedFrames != 0) {
				logInfo("PLI recovery discarded %zu stale queued video frames/%zu unsent packets for viewer %s",
				        discardedFrames, discardedPackets, uuid.c_str());
			}
			OnKeyframeRequestCallback cb;
			{
				std::lock_guard<std::mutex> callbackLock(manager->callbackMutex_);
				cb = manager->onKeyframeRequest_;
			}
			if (cb) {
				cb(uuid);
			}
		});
	};
#if defined(VDONINJA_NATIVE_MEDIA_INTEGRATION_TEST)
	{
		std::lock_guard<std::mutex> callbackLock(callbackMutex_);
		nativeMediaTestVideoFeedbackCompletions_[peer->generation] = videoFeedbackCompletion;
	}
#endif
	auto videoPliHandler = std::make_shared<rtc::PliHandler>(videoFeedbackCompletion);
	const int currentEncoderBitrate = bitrate_.load(std::memory_order_acquire);
	RtpPacketDuplicationConfig duplicationConfig;
	duplicationConfig.mode = videoProtectionMode_;
	duplicationConfig.averageBitrateBitsPerSecond =
	    videoProtectionBitrateForEncoderRate(currentEncoderBitrate, videoProtectionMode_);
	const uint64_t pacerBitrate = videoPacerBitrateForEncoderAndProtectionRate(
	    currentEncoderBitrate, duplicationConfig.averageBitrateBitsPerSecond);
	const auto pacerTrack = videoTrack;
	const auto pacerRtpConfig = peer->videoRtpConfig;
	const auto pacerSrReporter = peer->videoSrReporter;
	peer->videoPacer = std::make_shared<RtpPacketPacer>(
	    pacerBitrate, kVideoPacerInterval,
	    [pacerTrack, pacerRtpConfig, pacerSrReporter, hasTimestamp = false,
	     lastTimestamp = uint32_t{0}](RtpPacketPacer::Packet &&packet) mutable {
		    const uint32_t timestamp = rtpPacketTimestamp(packet);
		    if (!hasTimestamp || timestamp != lastTimestamp) {
			    hasTimestamp = true;
			    lastTimestamp = timestamp;
			    pacerRtpConfig->timestamp = timestamp;
			    if (isRtcpSenderReportDue(timestamp, pacerSrReporter->lastReportedTimestamp(), kVideoClockRate)) {
				    pacerSrReporter->setNeedsToReport();
			    }
		    }
		    return pacerTrack->send(std::move(packet));
	    },
	    0, videoPacerBudget_, duplicationConfig);
	peer->videoSrReporter->addToChain(std::make_shared<RtcpTelemetryHandler>(peer->videoFeedbackTracker));
	peer->videoSrReporter->addToChain(
	    std::make_shared<PacedNackResponder>(videoSsrc_, peer->videoPacer, peer->videoFeedbackTracker));
	peer->videoSrReporter->addToChain(videoPliHandler);
	videoTrack->setMediaHandler(peer->videoSrReporter);
	const std::weak_ptr<rtc::Track> weakVideoTrack = videoTrack;
	registerInstalledFunction(ownerSession, PeerManagerCompletionKind::VideoFeedback, videoFeedbackHandle,
	                          [weakVideoTrack]() {
		                          if (const auto track = weakVideoTrack.lock()) {
			                          track->setMediaHandler(nullptr);
		                          }
	                          });
	logInfo("Viewer %s video RTP pacer: %.1f Mbps, %zu KB per %lld ms batch, %zu KB queue limit, %zu KB shared "
	        "aggregate burst, encoder H.264 profile-level-id=%s, SDP uses the WebRTC compatibility profile",
	        peer->uuid.c_str(), static_cast<double>(pacerBitrate) / 1000000.0,
	        peer->videoPacer->batchBudgetBytes() / 1024, static_cast<long long>(kVideoPacerInterval.count()),
	        peer->videoPacer->maxQueueBytes() / 1024, videoPacerBudget_->burstBudgetBytes() / 1024,
	        h264ProfileLevelId.c_str());
	if (videoProtectionMode_ != VideoProtectionMode::Off) {
		logInfo("Viewer %s paced RTP duplication: %s, %.1f Mbps average repair budget, %lld ms separation, %lld ms "
		        "deadline (not RTP RED/FEC)",
		        peer->uuid.c_str(), videoProtectionModeName(videoProtectionMode_),
		        static_cast<double>(duplicationConfig.averageBitrateBitsPerSecond) / 1000000.0,
		        static_cast<long long>(duplicationConfig.delay.count()),
		        static_cast<long long>(duplicationConfig.maxAge.count()));
	}

	// Set up audio track
	rtc::Description::Audio audioDesc("audio", rtc::Description::Direction::SendOnly);
	if (audioRedEnabled_.load(std::memory_order_acquire) && audioCodec_ == AudioCodec::Opus) {
		audioDesc.addAudioCodec(kAudioRedPayloadType, "red", "111/111");
	}
	audioDesc.addOpusCodec(kOpusPayloadType);
	audioDesc.addSSRC(audioSsrc_, "audio-stream");
	const auto audioTrack = peer->pc->addTrack(audioDesc);
	{
		std::lock_guard<std::mutex> mediaLock(peer->mediaMutex);
		peer->audioTrack = audioTrack;
	}
	peer->audioRtpConfig =
	    std::make_shared<rtc::RtpPacketizationConfig>(audioSsrc_, "audio-stream", kOpusPayloadType, kAudioClockRate);
	peer->audioRtpConfig->sequenceNumber = peer->audioSeq;
	peer->audioRtpConfig->timestamp = peer->audioTimestamp;
	peer->audioSrReporter = std::make_shared<rtc::RtcpSrReporter>(peer->audioRtpConfig);
	peer->audioSrReporter->addToChain(std::make_shared<rtc::RtcpNackResponder>());
	audioTrack->setMediaHandler(peer->audioSrReporter);
	const std::weak_ptr<rtc::Track> weakAudioTrack = audioTrack;
	registerInstalledFunction(ownerSession, PeerManagerCompletionKind::AudioFeedback, audioTrack.get(),
	                          [weakAudioTrack]() {
		                          if (const auto track = weakAudioTrack.lock()) {
			                          track->setMediaHandler(nullptr);
		                          }
	                          });

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
		handleIncomingDataChannel(peer, dc, true);
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
	logInfo("Native viewer offer for %s parsed into %zu media sections: %s", peer->uuid.c_str(), offeredSections.size(),
	        describeOfferedSections(offeredSections).c_str());
	if (offeredSections.empty()) {
		const bool hasActualLineBreaks =
		    offerSdp.find('\n') != std::string::npos || offerSdp.find('\r') != std::string::npos;
		const bool hasEscapedLineBreaks = offerSdp.find("\\r\\n") != std::string::npos ||
		                                  offerSdp.find("\\n") != std::string::npos ||
		                                  offerSdp.find("\\r") != std::string::npos;
		logWarning("Native viewer offer for %s contained no audio/video media sections (bytes=%zu, actual_newlines=%s, "
		           "escaped_newlines=%s)",
		           peer->uuid.c_str(), offerSdp.size(), hasActualLineBreaks ? "yes" : "no",
		           hasEscapedLineBreaks ? "yes" : "no");
	}
	const bool alphaSectionActive = offerHasActiveVp9AlphaSection(offeredSections);
	TrackSlotEvent retiredAlphaEvent;
	if (!alphaSectionActive) {
		updateTrackSlot(peer, TrackType::AlphaVideo, nullptr, retiredAlphaEvent);
	}
	if (retiredAlphaEvent.retiredTrack) {
		logInfo("Renegotiated offer removed the active VP9 alpha section for %s", peer->uuid.c_str());
		dispatchTrackSlotEvent(retiredAlphaEvent);
	}

	const int requestedVideoBitrateKbps = std::max(1, bitrate_.load(std::memory_order_acquire) / 1000);
	size_t offeredVideoIndex = 0;

	for (const auto &section : offeredSections) {
		try {
			if (!offeredMediaSectionCanSend(section)) {
				logDebug("Skipping remote %s section %s because it cannot send media (port=%d, direction=%s)",
				         section.type.c_str(), section.mid.c_str(), section.port, section.direction.c_str());
				continue;
			}
			if (section.type == "video") {
				const size_t currentVideoIndex = offeredVideoIndex++;
				std::shared_ptr<rtc::Track> currentVideoTrack;
				std::shared_ptr<rtc::Track> currentAlphaTrack;
				std::string primaryMid;
				{
					std::lock_guard<std::mutex> mediaLock(peer->mediaMutex);
					currentVideoTrack = peer->videoTrack;
					currentAlphaTrack = peer->alphaVideoTrack;
					primaryMid = currentVideoTrack ? currentVideoTrack->mid() : "";
				}
				if (isExistingPrimaryVideoSection(currentVideoIndex, section.mid, primaryMid,
				                                  static_cast<bool>(currentVideoTrack))) {
					logDebug("Reusing existing primary video section for %s (mid=%s)", peer->uuid.c_str(),
					         section.mid.c_str());
					continue;
				}
				if (currentVideoTrack) {
					// Second video section: treat as alpha track if VP9 and no alpha track yet.
					if (!currentAlphaTrack) {
						const SdpOfferedCodec *alphaCodec = findPreferredOfferedCodec(section, "vp9");
						if (alphaCodec) {
							rtc::Description::Video receiveAlpha(section.mid.empty() ? "video-alpha" : section.mid,
							                                     rtc::Description::Direction::RecvOnly);
							receiveAlpha.addVP9Codec(alphaCodec->payloadType);
							auto alphaTrack = peer->pc->addTrack(receiveAlpha);
							const std::string alphaTrackMid = alphaTrack ? alphaTrack->mid() : "";
							TrackType installedType = TrackType::AlphaVideo;
							TrackSlotEvent installedEvent;
							bool installed = false;
							std::shared_ptr<rtc::Track> rejectedTrack;
							if (!alphaTrackMid.empty() && alphaTrackMid == section.mid) {
								installed = alphaTrack && updateTrackSlot(peer, TrackType::AlphaVideo, alphaTrack,
								                                          installedEvent, currentAlphaTrack, true);
								if (!installed) {
									rejectedTrack = alphaTrack;
								}
							} else {
								installedType = TrackType::Video;
								installed = alphaTrack && updateTrackSlot(peer, TrackType::Video, alphaTrack,
								                                          installedEvent, currentVideoTrack, true);
								if (!installed) {
									rejectedTrack = alphaTrack;
								}
							}
							if (installed && !rejectedTrack && alphaTrack) {
								if (installedType == TrackType::AlphaVideo) {
									logInfo("Prepared native recvonly VP9 alpha video track for %s (mid=%s)",
									        peer->uuid.c_str(), alphaTrackMid.c_str());
								} else {
									logInfo("Prepared renegotiated VP9 alpha receive handle for %s but libdatachannel "
									        "bound it to primary mid=%s",
									        peer->uuid.c_str(),
									        alphaTrackMid.empty() ? "(unset)" : alphaTrackMid.c_str());
								}
								dispatchCommittedTrackSlotEvent(peer, installedEvent);
							}
							clearTrackCallbacks(rejectedTrack);
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
				TrackSlotEvent installedEvent;
				bool installed = false;
				std::shared_ptr<rtc::Track> rejectedTrack;
				installed = track && updateTrackSlot(peer, TrackType::Video, track, installedEvent, nullptr, true);
				if (!installed) {
					rejectedTrack = track;
				}
				if (installed) {
					logInfo("Prepared native recvonly %s video track for %s (mid=%s, bitrate=%d kbps)",
					        useVP9 ? "VP9" : "H.264", peer->uuid.c_str(), track ? track->mid().c_str() : "",
					        requestedVideoBitrateKbps);
					dispatchCommittedTrackSlotEvent(peer, installedEvent);
				}
				clearTrackCallbacks(rejectedTrack);
			} else if (section.type == "audio") {
				{
					std::lock_guard<std::mutex> mediaLock(peer->mediaMutex);
					if (peer->audioTrack) {
						continue;
					}
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
				TrackSlotEvent installedEvent;
				bool installed = false;
				std::shared_ptr<rtc::Track> rejectedTrack;
				installed = track && updateTrackSlot(peer, TrackType::Audio, track, installedEvent, nullptr, true);
				if (!installed) {
					rejectedTrack = track;
				}
				if (installed) {
					logInfo("Prepared native recvonly audio track for %s (mid=%s)", peer->uuid.c_str(),
					        track ? track->mid().c_str() : "");
					dispatchCommittedTrackSlotEvent(peer, installedEvent);
				}
				clearTrackCallbacks(rejectedTrack);
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
	std::shared_ptr<PeerInfo> stalePeer;
	std::string staleReason;

	{
		std::lock_guard<std::mutex> lock(peersMutex_);
		auto it = peers_.find(uuid);
		if (it != peers_.end()) {
			peer = it->second;
			if (!peer) {
				peers_.erase(it);
				peer = nullptr;
			} else {
				const bool sessionRotated = !session.empty() && !peer->session.empty() && peer->session != session;
				const bool staleState = peer->cleanupRetired.load() || isTerminalPeerState(peer->state.load());
				if (sessionRotated || staleState) {
					stalePeer = peer;
					staleReason = sessionRotated ? "session-rotated" : "stale-state";
					std::lock_guard<std::mutex> candidateLock(candidateMutex_);
					peer->signalingActive.store(false);
					peers_.erase(it);
					peer = nullptr;
				}
			}
		}
	}

	if (stalePeer) {
		logInfo("Recreating native viewer peer %s (%s)", uuid.c_str(), staleReason.c_str());
		retirePeerForDeferredCleanup(uuid, stalePeer);
	}

	if (!peer) {
		peer = createViewerConnection(uuid);
	}

	peer->session = session;
	{
		std::lock_guard<std::mutex> lock(candidateMutex_);
		candidateBundles_[peer->generation].session = session;
	}
	consumePendingViewerSignalingDataChannel(peer, session);

	// Set remote description (the offer)
	const std::string constrainedSdp = normalizeEscapedSdpLineEndings(constrainViewerOfferToNativeCodecs(sdp));
	peer->remoteDescriptionSet.store(false);
	try {
		const bool hasActualLineBreaks =
		    constrainedSdp.find('\n') != std::string::npos || constrainedSdp.find('\r') != std::string::npos;
		const bool hasEscapedLineBreaks = constrainedSdp.find("\\r\\n") != std::string::npos ||
		                                  constrainedSdp.find("\\n") != std::string::npos ||
		                                  constrainedSdp.find("\\r") != std::string::npos;
		logInfo("Applying native viewer offer for %s (session=%s, bytes=%zu, actual_newlines=%s, escaped_newlines=%s, "
		        "signaling=%d)",
		        uuid.c_str(), session.c_str(), constrainedSdp.size(), hasActualLineBreaks ? "yes" : "no",
		        hasEscapedLineBreaks ? "yes" : "no", static_cast<int>(peer->pc->signalingState()));
		prepareViewerTracks(peer, constrainedSdp);
		bool hasVideoTrack = false;
		bool hasAlphaVideoTrack = false;
		bool hasAudioTrack = false;
		{
			std::lock_guard<std::mutex> mediaLock(peer->mediaMutex);
			hasVideoTrack = static_cast<bool>(peer->videoTrack);
			hasAlphaVideoTrack = static_cast<bool>(peer->alphaVideoTrack);
			hasAudioTrack = static_cast<bool>(peer->audioTrack);
		}
		logInfo("Prepared native viewer tracks for %s (video=%s, alpha=%s, audio=%s)", uuid.c_str(),
		        hasVideoTrack ? "yes" : "no", hasAlphaVideoTrack ? "yes" : "no", hasAudioTrack ? "yes" : "no");
		peer->pc->setRemoteDescription(rtc::Description(constrainedSdp, rtc::Description::Type::Offer));
		peer->remoteDescriptionSet.store(true);
		drainPendingRemoteIceCandidates(peer);
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
		if (it == peers_.end() || !it->second) {
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
	if (isTerminalPeerState(state)) {
		logDebug("Ignoring answer for terminal peer: %s", uuid.c_str());
		return;
	}
	const bool sessionMismatch = !session.empty() && !peer->session.empty() && peer->session != session;
	if (sessionMismatch) {
		logWarning("Session mismatch for %s, ignoring answer", uuid.c_str());
		return;
	}
	if (!session.empty()) {
		std::lock_guard<std::mutex> lock(candidateMutex_);
		candidateBundles_[peer->generation].session = session;
	}

	const bool useAudioRed = audioRedEnabled_.load(std::memory_order_acquire) && audioCodec_ == AudioCodec::Opus &&
	                         answerSelectsAudioRed(sdp, kAudioRedPayloadType, kOpusPayloadType);

	// Set remote description (the answer)
	peer->remoteDescriptionSet.store(false);
	try {
		{
			std::lock_guard<std::mutex> mediaLock(peer->mediaMutex);
			peer->useAudioRed = useAudioRed;
			peer->hasPreviousOpusPayload = false;
			peer->previousOpusPayload.clear();
			peer->previousOpusTimestamp = 0;
			if (peer->audioRtpConfig) {
				peer->audioRtpConfig->payloadType = useAudioRed ? kAudioRedPayloadType : kOpusPayloadType;
			}
		}
		peer->pc->setRemoteDescription(rtc::Description(sdp, rtc::Description::Type::Answer));
		peer->remoteDescriptionSet.store(true);
		drainPendingRemoteIceCandidates(peer);
		if (audioRedEnabled_.load(std::memory_order_acquire)) {
			logInfo("Set remote answer for %s; audio transport negotiated %s", uuid.c_str(),
			        useAudioRed ? "RFC 2198 RED with one previous Opus frame" : "plain Opus fallback");
		} else {
			logInfo("Set remote answer for %s", uuid.c_str());
		}
	} catch (const std::exception &e) {
		logError("Failed to apply remote answer for %s: %s", uuid.c_str(), e.what());
	} catch (...) {
		logError("Failed to apply remote answer for %s: unknown exception", uuid.c_str());
	}
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
				const bool staleState = isTerminalPeerState(peer->state.load());
				if (sessionRotated || staleState) {
					stalePeer = peer;
					staleReason = sessionRotated ? "session-rotated" : "stale-state";
					std::lock_guard<std::mutex> candidateLock(candidateMutex_);
					peer->signalingActive.store(false);
					peers_.erase(it);
					peer = nullptr;
				}
			}
		}
	}

	if (stalePeer) {
		logInfo("Recreating viewer peer %s (%s)", uuid.c_str(), staleReason.c_str());
		retirePeerForDeferredCleanup(uuid, stalePeer);
	}

	if (!peer) {
		if (getPublisherSlotCount() >= maxViewers_) {
			logWarning("Rejecting offer request from %s - max viewers reached (%d)", uuid.c_str(), maxViewers_);
			return;
		}
		peer = createPublisherConnection(uuid, session);
	}

	// Ignore duplicate offer requests for a peer/session that is already negotiating
	// or connected. Re-entering setLocalDescription on the same PeerConnection while
	// the first local offer is still in flight can race libdatachannel internals.
	const ConnectionState state = peer->state.load();
	const bool sameSession = session.empty() || peer->session.empty() || peer->session == session;
	bool expectedOfferRequested = false;
	const bool claimedOfferRequest =
	    peer->localOfferRequested.compare_exchange_strong(expectedOfferRequested, true, std::memory_order_acq_rel);
	const bool duplicateActiveOffer = sameSession && !isTerminalPeerState(state) && !claimedOfferRequest;
	if (duplicateActiveOffer) {
		std::string cachedOffer;
		{
			std::lock_guard<std::mutex> negotiationLock(peer->negotiationMutex);
			cachedOffer = peer->lastLocalOfferSdp;
		}
		bool resentCachedOffer = false;
		if (state != ConnectionState::Connected && !cachedOffer.empty() && signaling_) {
			std::lock_guard<std::mutex> candidateLock(candidateMutex_);
			if (peer->signalingActive.load() && !peer->cleanupRetired.load()) {
				signaling_->sendOffer(uuid, cachedOffer, peer->session);
				resentCachedOffer = true;
			}
		}
		if (resentCachedOffer) {
			logInfo("Resent cached offer for duplicate request from peer %s (state=%d)", uuid.c_str(),
			        static_cast<int>(state));
		} else {
			logInfo("Ignoring duplicate offer request for peer %s (state=%d, cached_offer=%s)", uuid.c_str(),
			        static_cast<int>(state), cachedOffer.empty() ? "no" : "yes");
		}
		return;
	}

	if (!session.empty()) {
		peer->session = session;
	} else if (peer->session.empty()) {
		peer->session = generateSessionId();
	}
	{
		std::lock_guard<std::mutex> lock(candidateMutex_);
		candidateBundles_[peer->generation].session = peer->session;
	}

	// Only force keyframe wait when (re)establishing an inactive peer/session.
	if (!hadExistingPeer || state != ConnectionState::Connected) {
		std::lock_guard<std::mutex> mediaLock(peer->mediaMutex);
		peer->videoKeyframeGate.resetForCachedPrime();
	}

	try {
		std::string cachedOffer;
		{
			std::lock_guard<std::mutex> negotiationLock(peer->negotiationMutex);
			cachedOffer = peer->lastLocalOfferSdp;
		}
		peer->localOfferDispatched.store(false);
		if (!cachedOffer.empty()) {
			std::lock_guard<std::mutex> candidateLock(candidateMutex_);
			if (!signaling_ || !peer->signalingActive.load() || peer->cleanupRetired.load()) {
				throw std::runtime_error("peer was retired before its cached offer could be sent");
			}
			signaling_->sendOffer(uuid, cachedOffer, peer->session);
			peer->localOfferDispatched.store(true);
		} else {
			peer->pc->setLocalDescription();
		}
	} catch (const std::exception &e) {
		peer->localOfferRequested.store(false);
		{
			std::lock_guard<std::mutex> negotiationLock(peer->negotiationMutex);
			peer->lastLocalOfferSdp.clear();
		}
		logError("Failed to create local offer for %s: %s", uuid.c_str(), e.what());
	} catch (...) {
		peer->localOfferRequested.store(false);
		{
			std::lock_guard<std::mutex> negotiationLock(peer->negotiationMutex);
			peer->lastLocalOfferSdp.clear();
		}
		logError("Failed to create local offer for %s: unknown exception", uuid.c_str());
	}
}

void VDONinjaPeerManager::onSignalingIceCandidate(const std::string &uuid, const std::string &candidate,
                                                  const std::string &mid, const std::string &session)
{
	std::shared_ptr<PeerInfo> peer;
	auto queueCandidate = [&]() {
		const auto result = pendingRemoteIceCandidates_.push(uuid, {candidate, mid, session, currentTimeMs()});
		if (!result.accepted) {
			logDebug("Rejected queued ICE candidate for peer %s because it was empty or exceeded queue limits",
			         uuid.c_str());
		}
		return result;
	};

	{
		std::lock_guard<std::mutex> lock(peersMutex_);
		auto it = peers_.find(uuid);
		if (it == peers_.end()) {
			const auto result = queueCandidate();
			if (result.accepted) {
				logDebug("Queued ICE candidate for peer %s before peer creation%s", uuid.c_str(),
				         result.droppedQueuedData ? " (evicted older queued data at cap)" : "");
			}
			return;
		}
		peer = it->second;
	}

	const ConnectionState state = peer->state.load();
	if (isTerminalPeerState(state)) {
		if (queueCandidate().accepted) {
			logDebug("Queued ICE candidate for terminal peer %s in case a replacement offer follows", uuid.c_str());
		}
		return;
	}
	const bool sessionMismatch = !session.empty() && !peer->session.empty() && peer->session != session;
	if (sessionMismatch) {
		if (queueCandidate().accepted) {
			logDebug("Queued ICE candidate from %s for a different session", uuid.c_str());
		}
		return;
	}
	if (!peer->remoteDescriptionSet.load()) {
		const auto result = queueCandidate();
		if (result.accepted) {
			logDebug("Queued ICE candidate from %s until remote description is set%s", uuid.c_str(),
			         result.droppedQueuedData ? " (evicted older queued data at cap)" : "");
		}
		return;
	}

	// Add remote candidate
	try {
		peer->pc->addRemoteCandidate(rtc::Candidate(candidate, mid));
		logDebug("Added ICE candidate from %s", uuid.c_str());
	} catch (const std::exception &e) {
		logWarning("Failed to add ICE candidate from %s: %s", uuid.c_str(), e.what());
	} catch (...) {
		logWarning("Failed to add ICE candidate from %s: unknown exception", uuid.c_str());
	}
}

void VDONinjaPeerManager::drainPendingRemoteIceCandidates(const std::shared_ptr<PeerInfo> &peer)
{
	if (!peer || !peer->pc || !peer->remoteDescriptionSet.load()) {
		return;
	}

	auto candidates = pendingRemoteIceCandidates_.takeCompatible(peer->uuid, peer->session, currentTimeMs());
	if (candidates.empty()) {
		return;
	}

	size_t added = 0;
	for (const auto &candidate : candidates) {
		try {
			peer->pc->addRemoteCandidate(rtc::Candidate(candidate.candidate, candidate.mid));
			++added;
		} catch (const std::exception &e) {
			logWarning("Failed to add queued ICE candidate from %s: %s", peer->uuid.c_str(), e.what());
		} catch (...) {
			logWarning("Failed to add queued ICE candidate from %s: unknown exception", peer->uuid.c_str());
		}
	}
	logInfo("Drained %zu/%zu queued ICE candidates for %s", added, candidates.size(), peer->uuid.c_str());
}

void VDONinjaPeerManager::bundleAndSendCandidates(const std::shared_ptr<PeerInfo> &peer)
{
	if (!peer || !signaling_) {
		return;
	}

	CandidateBundle bundle;
	std::shared_ptr<rtc::DataChannel> signalingDataChannel;
	{
		std::lock_guard<std::mutex> mediaLock(peer->mediaMutex);
		signalingDataChannel = peer->signalingDataChannel;
	}
	{
		std::lock_guard<std::mutex> lock(candidateMutex_);
		if (peer->cleanupRetired.load() || !peer->signalingActive.load()) {
			return;
		}

		auto it = candidateBundles_.find(peer->generation);
		if (it == candidateBundles_.end() || it->second.candidates.empty()) {
			return;
		}
		bundle = std::move(it->second);
		candidateBundles_.erase(it);

		const std::string candidateType = peer->type == ConnectionType::Viewer ? "remote" : "local";
		for (const auto &cand : bundle.candidates) {
			if (signalingDataChannel &&
			    signaling_->sendIceCandidateViaDataChannel(signalingDataChannel, peer->uuid, std::get<0>(cand),
			                                               std::get<1>(cand), bundle.session, candidateType)) {
				continue;
			}
			signaling_->sendIceCandidate(peer->uuid, std::get<0>(cand), std::get<1>(cand), bundle.session,
			                             candidateType);
		}
	}

	logDebug("Sent %zu bundled ICE candidates to %s", bundle.candidates.size(), peer->uuid.c_str());
}

void VDONinjaPeerManager::sendAudioFrame(const uint8_t *data, size_t size, uint32_t timestamp)
{
	if (!publishing_)
		return;

	pruneRetiredPeers(kRetiredPeerCleanupDelayMs);

	std::vector<std::pair<std::string, std::shared_ptr<PeerInfo>>> targets;
	{
		std::lock_guard<std::mutex> lock(peersMutex_);
		for (auto &pair : peers_) {
			auto &peer = pair.second;
			if (!peer || peer->type != ConnectionType::Publisher || peer->state != ConnectionState::Connected) {
				continue;
			}
			targets.emplace_back(pair.first, peer);
		}
	}

	for (auto &target : targets) {
		sendAudioFrameToPeer(target.first, target.second, data, size, timestamp);
	}
}

bool VDONinjaPeerManager::sendAudioFrameToPeer(const std::string &uuid, const std::shared_ptr<PeerInfo> &peer,
                                               const uint8_t *data, size_t size, uint32_t timestamp)
{
	if (!peer || !data || size == 0) {
		return false;
	}
	std::lock_guard<std::mutex> sendLock(peer->audioSendMutex);

	std::shared_ptr<rtc::Track> track;
	rtc::binary packet;
	bool sentAudioRed = false;
	bool includedAudioRedundancy = false;
	size_t redundantAudioBytes = 0;
	{
		std::lock_guard<std::mutex> mediaLock(peer->mediaMutex);
		if (peer->cleanupRetired.load() || peer->type != ConnectionType::Publisher ||
		    peer->state != ConnectionState::Connected) {
			return false;
		}
		if (!peer->audioSendEnabled) {
			return false;
		}

		track = peer->audioTrack;
		if (!track) {
			return false;
		}

		if (peer->useAudioPacketizer) {
			packet = rtc::binary(size);
			std::memcpy(packet.data(), data, size);
		} else {
			uint32_t ts = timestamp ? timestamp : peer->audioTimestamp;
			if (peer->audioRtpConfig) {
				peer->audioRtpConfig->timestamp = ts;
				if (peer->audioSrReporter &&
				    isRtcpSenderReportDue(ts, peer->audioSrReporter->lastReportedTimestamp(), kAudioClockRate)) {
					peer->audioSrReporter->setNeedsToReport();
				}
			}
			peer->audioTimestamp = ts + 960; // 48kHz, 20ms frames

			std::vector<uint8_t> payload;
			uint8_t payloadType = kOpusPayloadType;
			if (peer->useAudioRed) {
				const AudioRedPayload redPayload = buildAudioRedPayload(
				    data, size, ts, peer->hasPreviousOpusPayload ? peer->previousOpusPayload.data() : nullptr,
				    peer->hasPreviousOpusPayload ? peer->previousOpusPayload.size() : 0, peer->previousOpusTimestamp,
				    kOpusPayloadType);
				if (redPayload.bytes.empty()) {
					logWarning("Unable to build audio RED payload for %s; dropping invalid encoded audio frame",
					           uuid.c_str());
					return false;
				}
				payload = redPayload.bytes;
				payloadType = kAudioRedPayloadType;
				sentAudioRed = true;
				includedAudioRedundancy = redPayload.includesRedundantBlock;
				redundantAudioBytes = redPayload.redundantBytes;
				peer->previousOpusPayload.assign(data, data + size);
				peer->previousOpusTimestamp = ts;
				peer->hasPreviousOpusPayload = true;
			} else {
				payload.assign(data, data + size);
			}

			const std::vector<uint8_t> rtpPacket =
			    buildOpusRtpPacket(payload.data(), payload.size(), payloadType, peer->audioSeq++, ts, audioSsrc_);
			packet = rtc::binary(rtpPacket.size());
			std::memcpy(packet.data(), rtpPacket.data(), rtpPacket.size());
		}
	}

	try {
		const bool sent = audioSendTracker_.send([&]() { return track->send(std::move(packet)); });
		if (sent && sentAudioRed) {
			audioRedPackets_.fetch_add(1, std::memory_order_relaxed);
			if (includedAudioRedundancy) {
				audioRedPacketsWithRedundancy_.fetch_add(1, std::memory_order_relaxed);
				audioRedRedundantBytes_.fetch_add(redundantAudioBytes, std::memory_order_relaxed);
			} else {
				audioRedPrimaryOnlyPackets_.fetch_add(1, std::memory_order_relaxed);
			}
		}
		return sent;
	} catch (const std::exception &e) {
		logError("Failed to send audio to %s: %s", uuid.c_str(), e.what());
		return false;
	}
}

void VDONinjaPeerManager::sendVideoFrame(const uint8_t *data, size_t size, uint32_t timestamp, bool keyframe)
{
	if (!publishing_)
		return;

	pruneRetiredPeers(kRetiredPeerCleanupDelayMs);

	std::vector<std::pair<std::string, std::shared_ptr<PeerInfo>>> targets;
	{
		std::lock_guard<std::mutex> lock(peersMutex_);
		for (auto &pair : peers_) {
			auto &peer = pair.second;
			if (!peer || peer->type != ConnectionType::Publisher || peer->state != ConnectionState::Connected) {
				continue;
			}
			targets.emplace_back(pair.first, peer);
		}
	}

	for (auto &target : targets) {
		sendVideoFrameToPeerHandle(target.first, target.second, data, size, timestamp, keyframe);
	}
}

void VDONinjaPeerManager::requireLiveKeyframeForAll()
{
	std::vector<std::shared_ptr<PeerInfo>> targets;
	{
		std::lock_guard<std::mutex> lock(peersMutex_);
		targets.reserve(peers_.size());
		for (const auto &entry : peers_) {
			if (entry.second && entry.second->type == ConnectionType::Publisher) {
				targets.push_back(entry.second);
			}
		}
	}

	for (const auto &peer : targets) {
		std::lock_guard<std::mutex> sendLock(peer->videoSendMutex);
		std::shared_ptr<RtpPacketPacer> pacer;
		{
			std::lock_guard<std::mutex> mediaLock(peer->mediaMutex);
			if (!peer->cleanupRetired.load() && !isTerminalPeerState(peer->state.load())) {
				// The upstream queue dropped a frame newer than anything already
				// in this peer's pacer. Invalidate every pending frame so a
				// queued keyframe from the prior recovery generation cannot be
				// followed by unusable deltas.
				peer->videoKeyframeGate.requireLiveKeyframe();
				pacer = peer->videoPacer;
			}
		}
		if (pacer) {
			size_t discardedPackets = 0;
			pacer->discardQueuedMediaFramesAfterCurrent(&discardedPackets);
			if (discardedPackets != 0) {
				std::lock_guard<std::mutex> mediaLock(peer->mediaMutex);
				if (peer->videoPacer == pacer) {
					reclaimDiscardedVideoSequenceNumbers(*peer, discardedPackets);
				}
			}
		}
	}
}

bool VDONinjaPeerManager::sendVideoFrameToPeer(const std::string &uuid, const uint8_t *data, size_t size,
                                               uint32_t timestamp, bool keyframe, bool cachedReplay)
{
	if (!publishing_ || uuid.empty() || !data || size == 0) {
		return false;
	}

	std::shared_ptr<PeerInfo> peer;
	{
		std::lock_guard<std::mutex> lock(peersMutex_);
		auto it = peers_.find(uuid);
		if (it == peers_.end()) {
			return false;
		}
		peer = it->second;
	}

	return sendVideoFrameToPeerHandle(uuid, peer, data, size, timestamp, keyframe, cachedReplay);
}

bool VDONinjaPeerManager::notePeerKeyframeRequest(const std::string &uuid)
{
	if (!publishing_ || uuid.empty()) {
		return false;
	}

	std::shared_ptr<PeerInfo> peer;
	{
		std::lock_guard<std::mutex> lock(peersMutex_);
		auto it = peers_.find(uuid);
		if (it == peers_.end()) {
			return false;
		}
		peer = it->second;
	}

	std::lock_guard<std::mutex> sendLock(peer->videoSendMutex);
	std::shared_ptr<RtpPacketPacer> pacer;
	VideoKeyframeGate::DecoderRequestDisposition disposition;
	{
		std::lock_guard<std::mutex> mediaLock(peer->mediaMutex);
		if (peer->cleanupRetired.load() || peer->type != ConnectionType::Publisher ||
		    isTerminalPeerState(peer->state.load())) {
			return false;
		}
		disposition = peer->videoKeyframeGate.onDecoderKeyframeRequest();
		pacer = peer->videoPacer;
	}
	if (pacer && disposition == VideoKeyframeGate::DecoderRequestDisposition::RequireNewLiveKeyframe) {
		size_t discardedPackets = 0;
		pacer->discardQueuedMediaFramesAfterCurrent(&discardedPackets);
		if (discardedPackets != 0) {
			std::lock_guard<std::mutex> mediaLock(peer->mediaMutex);
			if (peer->videoPacer == pacer) {
				reclaimDiscardedVideoSequenceNumbers(*peer, discardedPackets);
			}
		}
	}
	return true;
}

bool VDONinjaPeerManager::sendVideoFrameToPeerHandle(const std::string &uuid, const std::shared_ptr<PeerInfo> &peer,
                                                     const uint8_t *data, size_t size, uint32_t timestamp,
                                                     bool keyframe, bool cachedReplay)
{
	if (!peer || !data || size == 0) {
		return false;
	}
	std::lock_guard<std::mutex> sendLock(peer->videoSendMutex);

	std::vector<RtpPacketPacer::Packet> packets;
	{
		std::lock_guard<std::mutex> mediaLock(peer->mediaMutex);
		if (peer->cleanupRetired.load() || peer->type != ConnectionType::Publisher ||
		    peer->state != ConnectionState::Connected) {
			return false;
		}
		if (!peer->videoSendEnabled) {
			return false;
		}

		if (!peer->videoKeyframeGate.canQueueFrame(keyframe, cachedReplay)) {
			return false;
		}

		const auto pacer = peer->videoPacer;
		if (!peer->videoTrack || !pacer) {
			return false;
		}

		const uint32_t ts = timestamp ? timestamp : peer->videoTimestamp;
		uint16_t nextSequence = peer->videoSeq;
		if (!buildH264FrameRtpPackets(packets, nextSequence, ts, videoSsrc_, data, size)) {
			peer->videoKeyframeGate.requireLiveKeyframe();
			size_t discardedPackets = 0;
			pacer->discardQueuedMediaFramesAfterCurrent(&discardedPackets);
			reclaimDiscardedVideoSequenceNumbers(*peer, discardedPackets);
			logWarning("Could not packetize a complete H.264 frame for viewer %s; waiting for a live keyframe",
			           uuid.c_str());
			return false;
		}

		const bool wasAwaitingKeyframe = peer->videoKeyframeGate.isAwaitingKeyframe();
		VideoKeyframeGate::KeyframeTicket keyframeTicket = 0;
		if (keyframe) {
			keyframeTicket = peer->videoKeyframeGate.onKeyframeQueued(cachedReplay);
		}

		const std::weak_ptr<PeerInfo> weakPeer = peer;
		const std::weak_ptr<RtpPacketPacer> weakPacer = pacer;
		RtpPacerFrameInfo frameInfo;
		frameInfo.keyframe = keyframe;
		frameInfo.timestamp = ts;
		if (!pacer->enqueueFrame(
		        std::move(packets), frameInfo,
		        [weakPeer, weakPacer, uuid, cachedReplay, wasAwaitingKeyframe,
		         keyframeTicket](const RtpPacerFrameResult &result) {
			        const auto peer = weakPeer.lock();
			        if (!peer || peer->cleanupRetired.load()) {
				        return;
			        }

			        bool recovered = false;
			        size_t discardedFrames = 0;
			        {
				        std::lock_guard<std::mutex> sendLock(peer->videoSendMutex);
				        {
					        std::lock_guard<std::mutex> mediaLock(peer->mediaMutex);
					        if (result.info.keyframe) {
						        recovered = peer->videoKeyframeGate.onKeyframeSendCompleted(
						            keyframeTicket, result.success, cachedReplay);
					        } else if (!result.success) {
						        peer->videoKeyframeGate.requireLiveKeyframe(true);
					        }
				        }
				        if (!result.success) {
					        if (const auto activePacer = weakPacer.lock()) {
						        discardedFrames = activePacer->discardQueuedDeltaFramesUntilKeyframe();
					        }
				        }
			        }

			        if (!result.success) {
				        logWarning(
				            "Video RTP transport rejected a frame for viewer %s after %llu packets; discarded %zu "
				            "stale queued frames and waiting for a live keyframe",
				            uuid.c_str(), static_cast<unsigned long long>(result.sentPackets), discardedFrames);
			        } else if (result.info.keyframe && recovered && wasAwaitingKeyframe) {
				        logInfo("Viewer %s synchronized on fully sent %s keyframe in %llu ms", uuid.c_str(),
				                cachedReplay ? "cached" : "live",
				                static_cast<unsigned long long>(result.sendDurationMs));
			        }
		        })) {
			// A partially-sent encoded frame would invalidate the remainder of
			// the GOP, so enqueue is all-or-nothing. Suppress deltas until the
			// next live encoder keyframe if the bounded queue ever fills.
			peer->videoKeyframeGate.requireLiveKeyframe();
			size_t discardedPackets = 0;
			pacer->discardQueuedMediaFramesAfterCurrent(&discardedPackets);
			reclaimDiscardedVideoSequenceNumbers(*peer, discardedPackets);
			logWarning("Dropped complete video frame for viewer %s because its RTP pacer queue is full", uuid.c_str());
			return false;
		}

		peer->videoSeq = nextSequence;
		peer->videoTimestamp = ts + 3000; // 90kHz clock, ~30fps fallback cadence
	}
	return true;
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
		std::lock_guard<std::mutex> candidateLock(candidateMutex_);
		auto it = peers_.begin();
		while (it != peers_.end()) {
			if (it->second->type == ConnectionType::Viewer && it->second->streamId == streamId) {
				it->second->signalingActive.store(false);
				candidateBundles_.erase(it->second->generation);
				toClose.push_back(it->second);
				it = peers_.erase(it);
			} else {
				++it;
			}
		}
	}

	for (auto &peer : toClose) {
		releasePeerResources(peer);
	}
	pruneRetiredPeers(kRetiredPeerCleanupDelayMs);
	logInfo("Stopped viewing stream: %s", streamId.c_str());
}

bool VDONinjaPeerManager::disconnectPeer(const std::string &uuid)
{
	return disconnectPeer(PeerEventIdentity{uuid, "", 0});
}

bool VDONinjaPeerManager::disconnectPeer(const PeerEventIdentity &identity)
{
	if (identity.uuid.empty()) {
		return false;
	}
	pendingRemoteIceCandidates_.erase(identity.uuid);
	std::shared_ptr<PeerInfo> peer;
	{
		std::lock_guard<std::mutex> lock(peersMutex_);
		auto it = peers_.find(identity.uuid);
		if (it == peers_.end()) {
			return false;
		}
		peer = it->second;
		if (!peer || (identity.generation != 0 && peer->generation != identity.generation) ||
		    (!identity.session.empty() && peer->session != identity.session)) {
			return false;
		}
		std::lock_guard<std::mutex> candidateLock(candidateMutex_);
		peer->signalingActive.store(false);
		peers_.erase(it);
	}

	retirePeerDataChannel(peer);
	clearPeerCallbacks(peer);
	try {
		if (peer->pc) {
			peer->pc->close();
		}
	} catch (const std::exception &) {
	}
	// disconnectPeer is reachable from RTC callback threads (datachannel "bye",
	// signaling peer cleanup). Releasing the PeerConnection synchronously there
	// destroys RTC objects from their own callbacks, which has caused heap
	// corruption/crashes; defer the release like state-change cleanup does.
	retirePeerForDeferredCleanup(identity.uuid, peer);
	logInfo("Disconnected peer: %s", identity.uuid.c_str());
	return true;
}

void VDONinjaPeerManager::runDeferredCleanup()
{
	pruneRetiredPeers(kRetiredPeerCleanupDelayMs);
}

void VDONinjaPeerManager::releasePeerResources(const std::shared_ptr<PeerInfo> &peer)
{
	if (!peer) {
		return;
	}

	peer->cleanupRetired.store(true);
	retirePeerDataChannel(peer);
	std::vector<std::shared_ptr<rtc::DataChannel>> pendingDataChannelCleanup;
	{
		std::lock_guard<std::mutex> mediaLock(peer->mediaMutex);
		pendingDataChannelCleanup = peer->retiredDataChannelsPendingCallbackCleanup;
	}
	for (const auto &channel : pendingDataChannelCleanup) {
		clearRetiredDataChannelCallbacksIfUnused(peer, channel);
	}
	clearTrackSlots(peer);

	std::shared_ptr<rtc::PeerConnection> pc;
	std::shared_ptr<rtc::Track> audioTrack;
	std::shared_ptr<rtc::Track> videoTrack;
	std::shared_ptr<rtc::Track> alphaVideoTrack;
	std::shared_ptr<rtc::DataChannel> signalingDataChannel;
	std::shared_ptr<rtc::RtcpSrReporter> audioSrReporter;
	std::shared_ptr<rtc::RtcpSrReporter> videoSrReporter;
	std::shared_ptr<rtc::RtpPacketizationConfig> audioRtpConfig;
	std::shared_ptr<rtc::RtpPacketizationConfig> videoRtpConfig;
	std::shared_ptr<RtcpFeedbackTracker> videoFeedbackTracker;
	std::shared_ptr<RtpPacketPacer> videoPacer;
	{
		std::lock_guard<std::mutex> mediaLock(peer->mediaMutex);
		pc = peer->pc;
		audioTrack = peer->audioTrack;
		videoTrack = peer->videoTrack;
		alphaVideoTrack = peer->alphaVideoTrack;
		signalingDataChannel = peer->signalingDataChannel;
		audioSrReporter = peer->audioSrReporter;
		videoSrReporter = peer->videoSrReporter;
		audioRtpConfig = peer->audioRtpConfig;
		videoRtpConfig = peer->videoRtpConfig;
		videoFeedbackTracker = peer->videoFeedbackTracker;
		videoPacer = peer->videoPacer;
	}

	if (videoPacer) {
		videoPacer->stop();
	}
	clearPeerConnectionCallbacks(pc);
	clearTrackCallbacks(audioTrack);
	clearTrackCallbacks(videoTrack);
	clearTrackCallbacks(alphaVideoTrack);

	{
		std::lock_guard<std::mutex> mediaLock(peer->mediaMutex);
		peer->audioTrack.reset();
		peer->videoTrack.reset();
		peer->alphaVideoTrack.reset();
		peer->signalingDataChannel.reset();
		peer->signalingDataChannelTransportUuid.clear();
		peer->signalingDataChannelTransportGeneration = 0;
		peer->signalingDataChannelRevision = 0;
		peer->audioSrReporter.reset();
		peer->videoSrReporter.reset();
		peer->audioRtpConfig.reset();
		peer->videoRtpConfig.reset();
		peer->videoFeedbackTracker.reset();
		peer->videoPacer.reset();
		peer->pc.reset();
		peer->hasDataChannel = false;
		peer->dataChannelOpenDispatched = false;
		peer->dataChannelOpenDispatchPending = false;
	}
	{
		std::lock_guard<std::mutex> negotiationLock(peer->negotiationMutex);
		peer->lastLocalOfferSdp.clear();
	}

	signalingDataChannel.reset();
	audioSrReporter.reset();
	videoSrReporter.reset();
	audioRtpConfig.reset();
	videoRtpConfig.reset();
	videoFeedbackTracker.reset();
}

bool VDONinjaPeerManager::isCurrentPeer(const std::shared_ptr<PeerInfo> &peer) const
{
	if (!peer) {
		return false;
	}
	std::lock_guard<std::mutex> lock(peersMutex_);
	const auto it = peers_.find(peer->uuid);
	return it != peers_.end() && it->second == peer;
}

void VDONinjaPeerManager::retirePeerForDeferredCleanup(const std::string &uuid, const std::shared_ptr<PeerInfo> &peer)
{
	if (!peer) {
		return;
	}

	const int64_t now = currentTimeMs();
	if (peer->terminalStateTimeMs.load() == 0) {
		peer->terminalStateTimeMs.store(now);
	}

	const bool alreadyRetired = peer->cleanupRetired.exchange(true);
	{
		std::lock_guard<std::mutex> lock(peersMutex_);
		std::lock_guard<std::mutex> candidateLock(candidateMutex_);
		peer->signalingActive.store(false);
		auto it = peers_.find(uuid);
		if (it != peers_.end() && it->second == peer) {
			peers_.erase(it);
		}
		candidateBundles_.erase(peer->generation);
	}
	retirePeerDataChannel(peer);
	{
		std::lock_guard<std::mutex> aliasLock(dataChannelAliasMutex_);
		std::lock_guard<std::mutex> lock(pendingViewerSignalingMutex_);
		pendingViewerSignalingDataChannels_.erase(viewerSignalingKey(uuid, peer->session));
		pendingViewerSignalingDataChannels_.erase(viewerSignalingKey(uuid, ""));
	}

	if (alreadyRetired) {
		return;
	}

	clearTrackSlots(peer);

	// Keep RTC objects alive until a non-RTC callback path can clear callbacks
	// and release them. Destroying a PeerConnection from its own state callback
	// has caused heap corruption/crashes in long-running publish sessions.
	std::lock_guard<std::mutex> lock(retiredPeersMutex_);
	retiredPeers_.push_back({uuid, peer, now});
}

void VDONinjaPeerManager::pruneRetiredPeers(int64_t minAgeMs)
{
	std::vector<RetiredPeer> toRelease;
	const int64_t now = currentTimeMs();
	{
		std::lock_guard<std::mutex> lock(retiredPeersMutex_);
		auto it = retiredPeers_.begin();
		while (it != retiredPeers_.end()) {
			if (minAgeMs > 0 && now - it->retiredAtMs < minAgeMs) {
				++it;
				continue;
			}
			toRelease.push_back(*it);
			it = retiredPeers_.erase(it);
		}
	}

	for (const auto &retiredPeer : toRelease) {
		releasePeerResources(retiredPeer.peer);
	}
}

void VDONinjaPeerManager::clearPeerCallbacks(const std::shared_ptr<PeerInfo> &peer)
{
	if (!peer) {
		return;
	}

	std::shared_ptr<rtc::PeerConnection> pc;
	std::shared_ptr<rtc::Track> videoTrack;
	std::shared_ptr<rtc::Track> alphaVideoTrack;
	std::shared_ptr<rtc::Track> audioTrack;
	std::shared_ptr<rtc::DataChannel> dataChannel;
	{
		std::lock_guard<std::mutex> mediaLock(peer->mediaMutex);
		pc = peer->pc;
		videoTrack = peer->videoTrack;
		alphaVideoTrack = peer->alphaVideoTrack;
		audioTrack = peer->audioTrack;
		dataChannel = peer->dataChannel;
	}
	clearPeerConnectionCallbacks(pc);
	clearTrackCallbacks(videoTrack);
	clearTrackCallbacks(alphaVideoTrack);
	clearTrackCallbacks(audioTrack);
	if (dataChannel) {
		if (activeManagerDataChannelCallback == dataChannel.get()) {
			std::lock_guard<std::mutex> mediaLock(peer->mediaMutex);
			const auto &pending = peer->retiredDataChannelsPendingCallbackCleanup;
			if (std::find(pending.begin(), pending.end(), dataChannel) == pending.end()) {
				peer->retiredDataChannelsPendingCallbackCleanup.push_back(dataChannel);
			}
		} else {
			std::lock_guard<std::recursive_mutex> callbackMutationLock(peer->dataChannelCallbackMutationMutex);
			clearDataChannelCallbacks(dataChannel);
		}
	}
}

void VDONinjaPeerManager::sendDataToAll(const std::string &message)
{
	pruneRetiredPeers(kRetiredPeerCleanupDelayMs);

	struct SendTarget {
		std::string uuid;
		std::shared_ptr<rtc::DataChannel> channel;
	};
	std::vector<SendTarget> targets;

	{
		std::lock_guard<std::mutex> lock(peersMutex_);
		for (auto &pair : peers_) {
			if (!pair.second || isTerminalPeerState(pair.second->state.load())) {
				continue;
			}
			std::lock_guard<std::mutex> mediaLock(pair.second->mediaMutex);
			if (!pair.second->hasDataChannel || !pair.second->dataChannel || !pair.second->dataChannelOpenDispatched) {
				continue;
			}
			targets.push_back({pair.first, pair.second->dataChannel});
		}
	}

	for (const auto &target : targets) {
		try {
			if (!target.channel->isOpen()) {
				continue;
			}
			target.channel->send(message);
		} catch (const std::exception &e) {
			logError("Failed to send data to %s: %s", target.uuid.c_str(), e.what());
		}
	}
}

void VDONinjaPeerManager::sendDataToPeer(const std::string &uuid, const std::string &message)
{
	pruneRetiredPeers(kRetiredPeerCleanupDelayMs);

	std::shared_ptr<rtc::DataChannel> targetChannel;
	std::string targetMessage;
	{
		std::lock_guard<std::mutex> lock(peersMutex_);
		auto it = peers_.find(uuid);
		if (it == peers_.end() || !it->second || isTerminalPeerState(it->second->state.load())) {
			return;
		}

		std::lock_guard<std::mutex> mediaLock(it->second->mediaMutex);
		if (it->second->hasDataChannel && it->second->dataChannel && it->second->dataChannelOpenDispatched) {
			targetChannel = it->second->dataChannel;
			targetMessage = message;
		} else if (it->second->type == ConnectionType::Viewer && it->second->signalingDataChannel) {
			targetChannel = it->second->signalingDataChannel;
			targetMessage = wrapTargetedPeerMessage(uuid, it->second->session, message);
		} else {
			return;
		}
	}

	try {
		if (!targetChannel->isOpen()) {
			return;
		}
		targetChannel->send(targetMessage);
	} catch (const std::exception &e) {
		logError("Failed to send data to %s: %s", uuid.c_str(), e.what());
	}
}

void VDONinjaPeerManager::sendDataToPeer(const PeerEventIdentity &identity, const std::string &message)
{
	if (identity.uuid.empty() || identity.generation == 0) {
		return;
	}
	pruneRetiredPeers(kRetiredPeerCleanupDelayMs);

	std::shared_ptr<rtc::DataChannel> targetChannel;
	std::string targetMessage;
	{
		std::lock_guard<std::mutex> lock(peersMutex_);
		const auto it = peers_.find(identity.uuid);
		if (it == peers_.end() || !it->second || it->second->generation != identity.generation ||
		    it->second->session != identity.session || isTerminalPeerState(it->second->state.load())) {
			return;
		}

		std::lock_guard<std::mutex> mediaLock(it->second->mediaMutex);
		if (it->second->hasDataChannel && it->second->dataChannel && it->second->dataChannelOpenDispatched) {
			targetChannel = it->second->dataChannel;
			targetMessage = message;
		} else if (it->second->type == ConnectionType::Viewer && it->second->signalingDataChannel) {
			targetChannel = it->second->signalingDataChannel;
			targetMessage = wrapTargetedPeerMessage(identity.uuid, identity.session, message);
		} else {
			return;
		}
	}

	try {
		if (targetChannel->isOpen()) {
			targetChannel->send(targetMessage);
		}
	} catch (const std::exception &e) {
		logError("Failed to send identity-bound data to %s generation %llu: %s", identity.uuid.c_str(),
		         static_cast<unsigned long long>(identity.generation), e.what());
	}
}

void VDONinjaPeerManager::bindViewerSignalingDataChannel(const std::string &transportPeerUuid,
                                                         const std::string &targetUuid,
                                                         const std::string &targetSession)
{
	if (transportPeerUuid.empty() || targetUuid.empty()) {
		return;
	}

	ViewerSignalingDataChannelRoute route;
	bool targetWasEligible = false;
	{
		std::lock_guard<std::mutex> aliasLock(dataChannelAliasMutex_);
		std::lock_guard<std::mutex> lock(peersMutex_);
		const auto transportIt = peers_.find(transportPeerUuid);
		if (transportIt == peers_.end() || !transportIt->second) {
			return;
		}
		const auto transport = transportIt->second;
		std::lock_guard<std::mutex> transportMediaLock(transport->mediaMutex);
		if (!transport->dataChannel || !transport->dataChannelOpenDispatched) {
			return;
		}
		route = {transport->uuid, transport->generation, transport->dataChannelRevision, transport->dataChannel};
		const auto targetIt = peers_.find(targetUuid);
		if (targetIt != peers_.end() && targetIt->second &&
		    (targetSession.empty() || targetIt->second->session.empty() ||
		     targetIt->second->session == targetSession)) {
			targetWasEligible = true;
		}
	}

#if defined(VDONINJA_NATIVE_MEDIA_INTEGRATION_TEST)
	invokeNativeMediaTestDataChannelLifecycleHook(targetWasEligible
	                                                  ? NativeMediaTestDataChannelStage::BeforeAliasCommit
	                                                  : NativeMediaTestDataChannelStage::BeforePendingAliasCommit,
	                                              nullptr, route.channel, route.dataChannelRevision);
#endif

	std::lock_guard<std::mutex> aliasLock(dataChannelAliasMutex_);
	std::lock_guard<std::mutex> lock(peersMutex_);
	const auto transportIt = peers_.find(route.transportUuid);
	if (transportIt == peers_.end() || !transportIt->second ||
	    transportIt->second->generation != route.transportGeneration) {
		return;
	}
	const auto transport = transportIt->second;
	std::unique_lock<std::mutex> transportMediaLock(transport->mediaMutex);
	if (transport->dataChannel != route.channel || transport->dataChannelRevision != route.dataChannelRevision ||
	    !transport->dataChannelOpenDispatched) {
		return;
	}
	const auto targetIt = peers_.find(targetUuid);
	if (targetIt != peers_.end() && targetIt->second &&
	    (targetSession.empty() || targetIt->second->session.empty() || targetIt->second->session == targetSession)) {
		const auto target = targetIt->second;
		if (target != transport) {
			std::lock_guard<std::mutex> targetMediaLock(target->mediaMutex);
			target->signalingDataChannel = route.channel;
			target->signalingDataChannelTransportUuid = route.transportUuid;
			target->signalingDataChannelTransportGeneration = route.transportGeneration;
			target->signalingDataChannelRevision = route.dataChannelRevision;
		} else {
			target->signalingDataChannel = route.channel;
			target->signalingDataChannelTransportUuid = route.transportUuid;
			target->signalingDataChannelTransportGeneration = route.transportGeneration;
			target->signalingDataChannelRevision = route.dataChannelRevision;
		}
		return;
	}

	std::lock_guard<std::mutex> pendingLock(pendingViewerSignalingMutex_);
	constexpr size_t kMaxPendingViewerSignalingChannels = 32;
	while (pendingViewerSignalingDataChannels_.size() >= kMaxPendingViewerSignalingChannels) {
		pendingViewerSignalingDataChannels_.erase(pendingViewerSignalingDataChannels_.begin());
	}
	pendingViewerSignalingDataChannels_[viewerSignalingKey(targetUuid, targetSession)] = route;
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
#if defined(VDONINJA_NATIVE_MEDIA_INTEGRATION_TEST)
std::shared_ptr<PeerInfo> VDONinjaPeerManager::createNativeMediaTestViewerPeer(const std::string &uuid,
                                                                               const std::string &session)
{
	auto peer = createViewerConnection(uuid);
	if (peer) {
		peer->session = session;
	}
	return peer;
}

std::shared_ptr<PeerInfo> VDONinjaPeerManager::createNativeMediaTestPublisherPeer(const std::string &uuid,
                                                                                  const std::string &session)
{
	return createPublisherConnection(uuid, session);
}

void VDONinjaPeerManager::receiveNativeMediaTestTrack(const std::shared_ptr<PeerInfo> &peer,
                                                      const std::shared_ptr<rtc::Track> &track)
{
	handleIncomingTrack(peer, track);
}

void VDONinjaPeerManager::prepareNativeMediaTestViewerTracks(const std::shared_ptr<PeerInfo> &peer,
                                                             const std::string &offerSdp)
{
	prepareViewerTracks(peer, offerSdp);
}

void VDONinjaPeerManager::retireNativeMediaTestPeer(const std::shared_ptr<PeerInfo> &peer)
{
	if (peer) {
		retirePeerForDeferredCleanup(peer->uuid, peer);
	}
}

void VDONinjaPeerManager::dispatchNativeMediaTestPeerDisconnected(const std::shared_ptr<PeerInfo> &peer)
{
	dispatchPeerDisconnected(peer);
}

void VDONinjaPeerManager::dispatchNativeMediaTestDataChannelMessage(const std::shared_ptr<PeerInfo> &peer,
                                                                    const std::string &message)
{
	dispatchDataChannelMessage(peer, message);
}

void VDONinjaPeerManager::dispatchNativeMediaTestDataChannelOpen(const std::shared_ptr<PeerInfo> &peer,
                                                                 const std::shared_ptr<rtc::DataChannel> &dc)
{
	uint64_t revision = 0;
	if (peer) {
		std::lock_guard<std::mutex> mediaLock(peer->mediaMutex);
		revision = peer->dataChannelRevision;
	}
	handleDataChannelOpen(peer, dc, peer ? peer->generation : 0, revision);
}

void VDONinjaPeerManager::receiveNativeMediaTestDataChannel(const std::shared_ptr<PeerInfo> &peer,
                                                            const std::shared_ptr<rtc::DataChannel> &dc)
{
	handleIncomingDataChannel(peer, dc);
}

void VDONinjaPeerManager::dispatchNativeMediaTestDataChannelError(const std::shared_ptr<PeerInfo> &peer,
                                                                  const std::shared_ptr<rtc::DataChannel> &dc,
                                                                  const std::string &error)
{
	uint64_t revision = 0;
	if (peer) {
		std::lock_guard<std::mutex> mediaLock(peer->mediaMutex);
		revision = peer->dataChannelRevision;
	}
	handleDataChannelTerminal(peer, dc, peer ? peer->generation : 0, revision, "test-datachannel-error", error);
}

size_t VDONinjaPeerManager::nativeMediaTestPendingViewerSignalingDataChannelCount() const
{
	std::lock_guard<std::mutex> aliasLock(dataChannelAliasMutex_);
	std::lock_guard<std::mutex> lock(pendingViewerSignalingMutex_);
	return pendingViewerSignalingDataChannels_.size();
}

size_t
VDONinjaPeerManager::nativeMediaTestPendingDataChannelCallbackCleanupCount(const std::shared_ptr<PeerInfo> &peer) const
{
	if (!peer) {
		return 0;
	}
	std::lock_guard<std::mutex> mediaLock(peer->mediaMutex);
	return peer->retiredDataChannelsPendingCallbackCleanup.size();
}

void VDONinjaPeerManager::consumeNativeMediaTestPendingViewerSignalingDataChannel(const std::shared_ptr<PeerInfo> &peer,
                                                                                  const std::string &session)
{
	consumePendingViewerSignalingDataChannel(peer, session);
}

void VDONinjaPeerManager::retireNativeMediaTestTrackSlot(const std::shared_ptr<PeerInfo> &peer, TrackType type)
{
	TrackSlotEvent event;
	if (updateTrackSlot(peer, type, nullptr, event)) {
		dispatchTrackSlotEvent(event);
	}
}

void VDONinjaPeerManager::dispatchNativeMediaTestTrackError(const std::shared_ptr<PeerInfo> &peer, TrackType type)
{
	if (!peer) {
		return;
	}
	std::shared_ptr<rtc::Track> track;
	uint64_t revision = 0;
	{
		std::lock_guard<std::mutex> mediaLock(peer->mediaMutex);
		if (type == TrackType::Audio) {
			track = peer->audioTrack;
			revision = peer->audioTrackRevision;
		} else if (type == TrackType::AlphaVideo) {
			track = peer->alphaVideoTrack;
			revision = peer->alphaVideoTrackRevision;
		} else {
			track = peer->videoTrack;
			revision = peer->videoTrackRevision;
		}
	}
	handleTrackTerminal(peer, type, track, peer->generation, revision, "test-track-error", "injected error");
}

bool VDONinjaPeerManager::nativeMediaTestPeerRegistryLockAvailable()
{
	if (!peersMutex_.try_lock()) {
		return false;
	}
	peersMutex_.unlock();
	return true;
}

void VDONinjaPeerManager::setNativeMediaTestTrackCommitHook(NativeMediaTestTrackCommitHook hook)
{
	std::lock_guard<std::mutex> callbackLock(callbackMutex_);
	nativeMediaTestTrackCommitHook_ = std::move(hook);
}

void VDONinjaPeerManager::setNativeMediaTestTrackDispatchHook(NativeMediaTestTrackDispatchHook hook)
{
	std::lock_guard<std::mutex> callbackLock(callbackMutex_);
	nativeMediaTestTrackDispatchHook_ = std::move(hook);
}

void VDONinjaPeerManager::setNativeMediaTestTrackLifecycleHook(NativeMediaTestTrackLifecycleHook hook)
{
	std::lock_guard<std::mutex> callbackLock(callbackMutex_);
	nativeMediaTestTrackLifecycleHook_ = std::move(hook);
}

void VDONinjaPeerManager::setNativeMediaTestTrackBeforeInstallHook(NativeMediaTestTrackHandleHook hook)
{
	std::lock_guard<std::mutex> callbackLock(callbackMutex_);
	nativeMediaTestTrackBeforeInstallHook_ = std::move(hook);
}

void VDONinjaPeerManager::setNativeMediaTestTrackCleanupHook(NativeMediaTestTrackHandleHook hook)
{
	std::lock_guard<std::mutex> callbackLock(callbackMutex_);
	nativeMediaTestTrackCleanupHook_ = std::move(hook);
}

void VDONinjaPeerManager::setNativeMediaTestDataChannelLifecycleHook(NativeMediaTestDataChannelLifecycleHook hook)
{
	std::lock_guard<std::mutex> callbackLock(callbackMutex_);
	nativeMediaTestDataChannelLifecycleHook_ = std::move(hook);
}

void VDONinjaPeerManager::setNativeMediaTestSignalingLifecycleHook(NativeMediaTestSignalingLifecycleHook hook)
{
	std::lock_guard<std::mutex> callbackLock(callbackMutex_);
	nativeMediaTestSignalingLifecycleHook_ = std::move(hook);
}

void VDONinjaPeerManager::setNativeMediaTestPeerDisconnectDispatchHook(NativeMediaTestPeerDispatchHook hook)
{
	std::lock_guard<std::mutex> callbackLock(callbackMutex_);
	nativeMediaTestPeerDisconnectDispatchHook_ = std::move(hook);
}

void VDONinjaPeerManager::setNativeMediaTestPeerDataDispatchHook(NativeMediaTestPeerDispatchHook hook)
{
	std::lock_guard<std::mutex> callbackLock(callbackMutex_);
	nativeMediaTestPeerDataDispatchHook_ = std::move(hook);
}

void VDONinjaPeerManager::setNativeMediaTestPeerDataDispatchCompleteHook(NativeMediaTestPeerDispatchHook hook)
{
	std::lock_guard<std::mutex> callbackLock(callbackMutex_);
	nativeMediaTestPeerDataDispatchCompleteHook_ = std::move(hook);
}

void VDONinjaPeerManager::setNativeMediaTestPeerDataOpenDispatchHook(NativeMediaTestPeerDispatchHook hook)
{
	std::lock_guard<std::mutex> callbackLock(callbackMutex_);
	nativeMediaTestPeerDataOpenDispatchHook_ = std::move(hook);
}

void VDONinjaPeerManager::setNativeMediaTestOwnerSessionHook(NativeMediaTestOwnerSessionHook hook)
{
	if (ownerSession_) {
		ownerSession_->setTestHook(std::move(hook));
	}
}

void VDONinjaPeerManager::failNextNativeMediaTestOwnerSessionFunctionRegistration(PeerManagerCompletionKind kind)
{
	if (ownerSession_) {
		ownerSession_->failNextFunctionRegistration(kind);
	}
}

std::function<void()>
VDONinjaPeerManager::nativeMediaTestVideoFeedbackCompletion(const std::shared_ptr<PeerInfo> &peer) const
{
	if (!peer) {
		return {};
	}
	std::lock_guard<std::mutex> callbackLock(callbackMutex_);
	const auto completion = nativeMediaTestVideoFeedbackCompletions_.find(peer->generation);
	return completion != nativeMediaTestVideoFeedbackCompletions_.end() ? completion->second : std::function<void()>{};
}
#endif
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
void VDONinjaPeerManager::setOnKeyframeRequest(OnKeyframeRequestCallback callback)
{
	std::lock_guard<std::mutex> callbackLock(callbackMutex_);
	onKeyframeRequest_ = callback;
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
		if (pair.second) {
			std::lock_guard<std::mutex> mediaLock(pair.second->mediaMutex);
			snapshot.hasDataChannel = pair.second->hasDataChannel && pair.second->dataChannelOpenDispatched;
		}
		if (pair.second) {
			std::lock_guard<std::mutex> mediaLock(pair.second->mediaMutex);
			snapshot.audioSendEnabled = pair.second->audioSendEnabled;
			snapshot.videoSendEnabled = pair.second->videoSendEnabled;
		}
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

std::optional<PeerEventIdentity> VDONinjaPeerManager::getPeerIdentity(const std::string &uuid) const
{
	std::lock_guard<std::mutex> lock(peersMutex_);
	const auto it = peers_.find(uuid);
	if (it == peers_.end() || !it->second) {
		return std::nullopt;
	}
	return peerEventIdentity(it->second);
}

std::optional<PeerEventIdentity> VDONinjaPeerManager::claimPeerEventIdentity(const std::string &uuid,
                                                                             const std::string &expectedSession,
                                                                             uint64_t expectedGeneration)
{
	std::lock_guard<std::mutex> lock(peersMutex_);
	const auto it = peers_.find(uuid);
	if (it == peers_.end() || !it->second || (!expectedSession.empty() && it->second->session != expectedSession) ||
	    (expectedGeneration != 0 && it->second->generation != expectedGeneration)) {
		return std::nullopt;
	}
	return nextPeerEventIdentity(it->second);
}

std::optional<PeerEventIdentity> VDONinjaPeerManager::claimSignalingPeerCleanupIdentity(const std::string &uuid,
                                                                                        const std::string &session,
                                                                                        bool *ambiguousReuse)
{
	if (ambiguousReuse) {
		*ambiguousReuse = false;
	}
	std::lock_guard<std::mutex> lock(peersMutex_);
	const auto peerIt = peers_.find(uuid);
	if (peerIt == peers_.end() || !peerIt->second || (!session.empty() && peerIt->second->session != session)) {
		return std::nullopt;
	}
	const auto historyIt = peerGenerationRegistrationCounts_.find(uuid);
	if (session.empty() && historyIt != peerGenerationRegistrationCounts_.end() && historyIt->second > 1) {
		if (ambiguousReuse) {
			*ambiguousReuse = true;
		}
		return std::nullopt;
	}
	return nextPeerEventIdentity(peerIt->second);
}

SignalingLifecycleDisposition VDONinjaPeerManager::processSignalingLifecycleEvent(const SignalingLifecycleEvent &event)
{
	const bool hasSocketEpoch = event.socketEpoch != 0;
	const bool hasWsSequence = event.wsSequence != 0;
	if (hasSocketEpoch != hasWsSequence) {
		logWarning("Ignoring signaling lifecycle event with incomplete socket ordering metadata");
		return SignalingLifecycleDisposition::Stale;
	}

	const bool websocketOrigin = hasSocketEpoch && hasWsSequence;
	const bool correlatedSession = event.session.has_value() && !event.session->empty();
	std::shared_ptr<PeerInfo> admittedPeer;
	std::optional<PeerEventIdentity> admittedIdentity;
	{
		std::lock_guard<std::mutex> lock(peersMutex_);
		if (websocketOrigin) {
			if (event.socketEpoch < latestSignalingLifecycleSocketEpoch_ ||
			    (event.socketEpoch == latestSignalingLifecycleSocketEpoch_ &&
			     event.wsSequence <= latestSignalingLifecycleWsSequence_)) {
				logDebug("Ignoring stale signaling lifecycle envelope epoch=%llu sequence=%llu",
				         static_cast<unsigned long long>(event.socketEpoch),
				         static_cast<unsigned long long>(event.wsSequence));
				return SignalingLifecycleDisposition::Stale;
			}
			if (event.socketEpoch > latestSignalingLifecycleSocketEpoch_) {
				latestSignalingLifecycleSocketEpoch_ = event.socketEpoch;
			}
			latestSignalingLifecycleWsSequence_ = event.wsSequence;
		}

		if (event.uuid.empty()) {
			logWarning("Treating UUID-less signaling stream removal for '%s' as a non-destructive hint",
			           event.streamId.c_str());
			return SignalingLifecycleDisposition::NonDestructiveHint;
		}

		const auto peerIt = peers_.find(event.uuid);
		if (peerIt == peers_.end() || !peerIt->second) {
			return SignalingLifecycleDisposition::NonDestructiveHint;
		}
		admittedPeer = peerIt->second;
		if ((correlatedSession && admittedPeer->session != *event.session) ||
		    admittedPeer->signalingLifecycleTerminalClaimed || admittedPeer->cleanupRetired.load() ||
		    isTerminalPeerState(admittedPeer->state.load())) {
			return SignalingLifecycleDisposition::Stale;
		}
		const auto historyIt = peerGenerationRegistrationCounts_.find(event.uuid);
		if (!correlatedSession && historyIt != peerGenerationRegistrationCounts_.end() && historyIt->second > 1) {
			logWarning("Treating ambiguous sessionless signaling lifecycle event for reused peer %s as a "
			           "non-destructive hint",
			           event.uuid.c_str());
			return SignalingLifecycleDisposition::AmbiguousSessionless;
		}
		admittedIdentity = nextPeerEventIdentity(admittedPeer);
	}

#if defined(VDONINJA_NATIVE_MEDIA_INTEGRATION_TEST)
	NativeMediaTestSignalingLifecycleHook testHook;
	{
		std::lock_guard<std::mutex> callbackLock(callbackMutex_);
		testHook = nativeMediaTestSignalingLifecycleHook_;
	}
	if (testHook) {
		testHook(event, admittedIdentity);
	}
#endif

	OnAcceptedSignalingLifecycleEventCallback callback;
	{
		std::lock_guard<std::mutex> callbackLock(callbackMutex_);
		callback = onAcceptedSignalingLifecycleEvent_;
	}
	if (!callback) {
		return SignalingLifecycleDisposition::NoSubscriber;
	}

	{
		std::lock_guard<std::mutex> lock(peersMutex_);
		if (websocketOrigin && (event.socketEpoch != latestSignalingLifecycleSocketEpoch_ ||
		                        event.wsSequence != latestSignalingLifecycleWsSequence_)) {
			return SignalingLifecycleDisposition::Stale;
		}
		const auto peerIt = peers_.find(event.uuid);
		if (!admittedIdentity || peerIt == peers_.end() || peerIt->second != admittedPeer ||
		    admittedPeer->generation != admittedIdentity->generation ||
		    (correlatedSession && admittedPeer->session != *event.session) ||
		    admittedPeer->signalingLifecycleTerminalClaimed || admittedPeer->cleanupRetired.load() ||
		    isTerminalPeerState(admittedPeer->state.load())) {
			return SignalingLifecycleDisposition::Stale;
		}
		const auto historyIt = peerGenerationRegistrationCounts_.find(event.uuid);
		if (!correlatedSession && historyIt != peerGenerationRegistrationCounts_.end() && historyIt->second > 1) {
			return SignalingLifecycleDisposition::AmbiguousSessionless;
		}
		admittedPeer->signalingLifecycleTerminalClaimed = true;
	}

	AcceptedSignalingLifecycleEvent accepted;
	accepted.kind = event.kind;
	accepted.socketEpoch = event.socketEpoch;
	accepted.wsSequence = event.wsSequence;
	accepted.identity = *admittedIdentity;
	accepted.streamId = event.streamId;
	runRtcCallbackNoexcept("Signaling lifecycle event callback", [&]() { callback(accepted); });
	(void)disconnectPeer(accepted.identity);
	return SignalingLifecycleDisposition::Accepted;
}

void VDONinjaPeerManager::setOnAcceptedSignalingLifecycleEvent(OnAcceptedSignalingLifecycleEventCallback callback)
{
	std::lock_guard<std::mutex> callbackLock(callbackMutex_);
	onAcceptedSignalingLifecycleEvent_ = std::move(callback);
}

void VDONinjaPeerManager::setVideoCodec(VideoCodec codec)
{
	if (codec != VideoCodec::H264) {
		logWarning("Only H.264 publisher video is currently supported; using H.264");
	}
	videoCodec_ = VideoCodec::H264;
}
void VDONinjaPeerManager::setAudioCodec(AudioCodec codec)
{
	audioCodec_ = codec;
}
void VDONinjaPeerManager::setH264ProfileLevelId(const std::string &profileLevelId)
{
	if (!isValidH264ProfileLevelId(profileLevelId)) {
		logWarning("Ignoring invalid H.264 profile-level-id '%s'", profileLevelId.c_str());
		return;
	}
	std::string normalized = profileLevelId;
	std::transform(normalized.begin(), normalized.end(), normalized.begin(),
	               [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
	std::lock_guard<std::mutex> codecLock(codecMutex_);
	h264ProfileLevelId_ = std::move(normalized);
}
void VDONinjaPeerManager::setBitrate(int bitrate)
{
	const int targetBitrate = std::max(bitrate, 1);
	bitrate_.store(targetBitrate, std::memory_order_release);

	std::vector<std::shared_ptr<RtpPacketPacer>> pacers;
	{
		std::lock_guard<std::mutex> lock(peersMutex_);
		pacers.reserve(peers_.size());
		for (const auto &entry : peers_) {
			const auto &peer = entry.second;
			if (!peer || peer->type != ConnectionType::Publisher) {
				continue;
			}
			std::lock_guard<std::mutex> mediaLock(peer->mediaMutex);
			if (peer->videoPacer) {
				pacers.push_back(peer->videoPacer);
			}
		}
	}

	const uint64_t duplicateBitrate = videoProtectionBitrateForEncoderRate(targetBitrate, videoProtectionMode_);
	const uint64_t pacerBitrate = videoPacerBitrateForEncoderAndProtectionRate(targetBitrate, duplicateBitrate);
	for (const auto &pacer : pacers) {
		pacer->updateBitrate(pacerBitrate, duplicateBitrate);
	}
}
void VDONinjaPeerManager::setVideoProtectionMode(VideoProtectionMode mode)
{
	videoProtectionMode_ = mode;
}
void VDONinjaPeerManager::setAudioRedEnabled(bool enable)
{
	audioRedEnabled_.store(enable, std::memory_order_release);
}
void VDONinjaPeerManager::setEnableDataChannel(bool enable)
{
	enableDataChannel_ = enable;
}

RtcpFeedbackStats VDONinjaPeerManager::takeVideoFeedbackStats()
{
	std::vector<std::shared_ptr<RtcpFeedbackTracker>> trackers;
	{
		std::lock_guard<std::mutex> lock(peersMutex_);
		trackers.reserve(peers_.size());
		for (const auto &entry : peers_) {
			const auto &peer = entry.second;
			if (!peer || peer->type != ConnectionType::Publisher) {
				continue;
			}
			std::lock_guard<std::mutex> mediaLock(peer->mediaMutex);
			if (peer->videoFeedbackTracker) {
				trackers.push_back(peer->videoFeedbackTracker);
			}
		}
	}

	RtcpFeedbackStats combined;
	for (const auto &tracker : trackers) {
		const RtcpFeedbackStats snapshot = tracker->take();
		combined.compoundPackets += snapshot.compoundPackets;
		combined.malformedPackets += snapshot.malformedPackets;
		combined.nackMessages += snapshot.nackMessages;
		combined.nackRequestedPackets += snapshot.nackRequestedPackets;
		combined.pliMessages += snapshot.pliMessages;
		combined.firMessages += snapshot.firMessages;
		combined.receiverReports += snapshot.receiverReports;
		combined.reportBlocks += snapshot.reportBlocks;
		combined.maxFractionLost = std::max(combined.maxFractionLost, snapshot.maxFractionLost);
		combined.maxCumulativeLost = std::max(combined.maxCumulativeLost, snapshot.maxCumulativeLost);
		combined.maxJitterTicks = std::max(combined.maxJitterTicks, snapshot.maxJitterTicks);
		combined.maxRttMs = std::max(combined.maxRttMs, snapshot.maxRttMs);
		combined.nackCacheHits += snapshot.nackCacheHits;
		combined.nackCacheMisses += snapshot.nackCacheMisses;
		combined.retransmissionsQueued += snapshot.retransmissionsQueued;
		combined.retransmissionsSent += snapshot.retransmissionsSent;
		combined.retransmissionsDropped += snapshot.retransmissionsDropped;
		combined.retransmissionsExpired += snapshot.retransmissionsExpired;
		combined.retransmissionSendFailures += snapshot.retransmissionSendFailures;
		combined.rembMessages += snapshot.rembMessages;
		if (snapshot.minRembBitrateBps > 0) {
			if (combined.minRembBitrateBps == 0) {
				combined.minRembBitrateBps = snapshot.minRembBitrateBps;
			} else {
				combined.minRembBitrateBps = std::min(combined.minRembBitrateBps, snapshot.minRembBitrateBps);
			}
			combined.maxRembBitrateBps = std::max(combined.maxRembBitrateBps, snapshot.maxRembBitrateBps);
		}
	}
	return combined;
}

std::optional<uint64_t> VDONinjaPeerManager::minimumRecentRembBitrate(std::chrono::milliseconds maxAge) const
{
	std::vector<std::shared_ptr<RtcpFeedbackTracker>> trackers;
	{
		std::lock_guard<std::mutex> lock(peersMutex_);
		trackers.reserve(peers_.size());
		for (const auto &entry : peers_) {
			const auto &peer = entry.second;
			if (!peer || peer->type != ConnectionType::Publisher || peer->state != ConnectionState::Connected) {
				continue;
			}
			std::lock_guard<std::mutex> mediaLock(peer->mediaMutex);
			if (peer->videoFeedbackTracker) {
				trackers.push_back(peer->videoFeedbackTracker);
			}
		}
	}

	if (trackers.empty()) {
		return std::nullopt;
	}
	std::optional<uint64_t> minimum;
	for (const auto &tracker : trackers) {
		const auto estimate = tracker->latestRemb(maxAge);
		if (!estimate || estimate->bitrateBitsPerSecond == 0) {
			// The adaptation policy is minimum-across-all-viewers. Do not
			// silently adapt from only the subset that happened to report.
			return std::nullopt;
		}
		minimum = minimum ? std::min(*minimum, estimate->bitrateBitsPerSecond) : estimate->bitrateBitsPerSecond;
	}
	return minimum;
}

RtpPacerStats VDONinjaPeerManager::takeVideoPacerStats()
{
	std::vector<std::shared_ptr<RtpPacketPacer>> pacers;
	{
		std::lock_guard<std::mutex> lock(peersMutex_);
		pacers.reserve(peers_.size());
		for (const auto &entry : peers_) {
			const auto &peer = entry.second;
			if (!peer || peer->type != ConnectionType::Publisher) {
				continue;
			}
			std::lock_guard<std::mutex> mediaLock(peer->mediaMutex);
			if (peer->videoPacer) {
				pacers.push_back(peer->videoPacer);
			}
		}
	}

	RtpPacerStats combined;
	for (const auto &pacer : pacers) {
		const RtpPacerStats snapshot = pacer->getStats(true);
		combined.queuedBytes += snapshot.queuedBytes;
		combined.queuedFrames += snapshot.queuedFrames;
		combined.maxQueuedBytes = std::max(combined.maxQueuedBytes, snapshot.maxQueuedBytes);
		combined.maxQueuedFrames = std::max(combined.maxQueuedFrames, snapshot.maxQueuedFrames);
		combined.maxBatchBytes = std::max(combined.maxBatchBytes, snapshot.maxBatchBytes);
		combined.maxPacketDelayMs = std::max(combined.maxPacketDelayMs, snapshot.maxPacketDelayMs);
		combined.maxFrameSendDurationMs = std::max(combined.maxFrameSendDurationMs, snapshot.maxFrameSendDurationMs);
		combined.maxKeyframeSendDurationMs =
		    std::max(combined.maxKeyframeSendDurationMs, snapshot.maxKeyframeSendDurationMs);
		combined.sentPackets += snapshot.sentPackets;
		combined.sentFrames += snapshot.sentFrames;
		combined.sentKeyframes += snapshot.sentKeyframes;
		combined.droppedFrames += snapshot.droppedFrames;
		combined.failedFrames += snapshot.failedFrames;
		combined.sendFailures += snapshot.sendFailures;
		combined.queuedRepairs += snapshot.queuedRepairs;
		combined.sentRepairs += snapshot.sentRepairs;
		combined.droppedRepairs += snapshot.droppedRepairs;
		combined.expiredRepairs += snapshot.expiredRepairs;
		combined.failedRepairs += snapshot.failedRepairs;
		combined.queuedDuplicates += snapshot.queuedDuplicates;
		combined.sentDuplicates += snapshot.sentDuplicates;
		combined.droppedDuplicates += snapshot.droppedDuplicates;
		combined.expiredDuplicates += snapshot.expiredDuplicates;
		combined.failedDuplicates += snapshot.failedDuplicates;
		combined.sentDuplicateBytes += snapshot.sentDuplicateBytes;
	}
	return combined;
}

RtpSendStats VDONinjaPeerManager::takeAudioSendStats()
{
	return audioSendTracker_.take();
}

AudioRedStats VDONinjaPeerManager::takeAudioRedStats()
{
	AudioRedStats stats;
	stats.packets = audioRedPackets_.exchange(0, std::memory_order_relaxed);
	stats.packetsWithRedundancy = audioRedPacketsWithRedundancy_.exchange(0, std::memory_order_relaxed);
	stats.primaryOnlyPackets = audioRedPrimaryOnlyPackets_.exchange(0, std::memory_order_relaxed);
	stats.redundantBytes = audioRedRedundantBytes_.exchange(0, std::memory_order_relaxed);
	return stats;
}

} // namespace vdoninja
