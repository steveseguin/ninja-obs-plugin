/*
 * OBS VDO.Ninja Plugin
 * WebSocket signaling client implementation
 *
 * This uses a simplified WebSocket client. In production, you would use
 * a library like websocketpp, libwebsockets, or boost::beast.
 * For this reference implementation, we'll use libdatachannel's WebSocket support.
 */

#include "vdoninja-signaling.h"

#include <rtc/rtc.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <initializer_list>
#include <vector>

#if __has_include(<openssl/evp.h>) && __has_include(<openssl/rand.h>)
#define VDONINJA_HAS_OPENSSL 1
#include <openssl/evp.h>
#include <openssl/rand.h>
#else
#define VDONINJA_HAS_OPENSSL 0
#endif

namespace vdoninja
{

namespace
{

std::string getAnyString(const JsonParser &json, const std::initializer_list<const char *> &keys)
{
	for (const char *key : keys) {
		if (json.hasKey(key)) {
			return json.getString(key);
		}
	}
	return "";
}

std::string bytesToHex(const uint8_t *data, size_t size)
{
	static const char hex[] = "0123456789abcdef";
	std::string out;
	out.reserve(size * 2);
	for (size_t i = 0; i < size; ++i) {
		out.push_back(hex[(data[i] >> 4) & 0x0F]);
		out.push_back(hex[data[i] & 0x0F]);
	}
	return out;
}

bool hexToBytes(const std::string &hex, std::vector<uint8_t> &out)
{
	if ((hex.size() % 2) != 0) {
		return false;
	}

	auto nibble = [](char c) -> int {
		if (c >= '0' && c <= '9') {
			return c - '0';
		}
		if (c >= 'a' && c <= 'f') {
			return 10 + (c - 'a');
		}
		if (c >= 'A' && c <= 'F') {
			return 10 + (c - 'A');
		}
		return -1;
	};

	out.clear();
	out.reserve(hex.size() / 2);
	for (size_t i = 0; i < hex.size(); i += 2) {
		const int hi = nibble(hex[i]);
		const int lo = nibble(hex[i + 1]);
		if (hi < 0 || lo < 0) {
			return false;
		}
		out.push_back(static_cast<uint8_t>((hi << 4) | lo));
	}
	return true;
}

bool encryptAesCbcHex(const std::string &plaintext, const std::string &phrase, std::string &cipherHex, std::string &vectorHex)
{
#if VDONINJA_HAS_OPENSSL
	if (phrase.empty()) {
		return false;
	}

	std::vector<uint8_t> key;
	if (!hexToBytes(sha256(phrase), key) || key.size() != 32) {
		return false;
	}

	uint8_t iv[16] = {};
	if (RAND_bytes(iv, static_cast<int>(sizeof(iv))) != 1) {
		return false;
	}

	EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
	if (!ctx) {
		return false;
	}

	std::vector<uint8_t> out(plaintext.size() + EVP_MAX_BLOCK_LENGTH);
	int outLen1 = 0;
	int outLen2 = 0;
	const bool ok =
	    EVP_EncryptInit_ex(ctx, EVP_aes_256_cbc(), nullptr, key.data(), iv) == 1 &&
	    EVP_EncryptUpdate(ctx, out.data(), &outLen1, reinterpret_cast<const uint8_t *>(plaintext.data()),
	                      static_cast<int>(plaintext.size())) == 1 &&
	    EVP_EncryptFinal_ex(ctx, out.data() + outLen1, &outLen2) == 1;

	EVP_CIPHER_CTX_free(ctx);
	if (!ok) {
		return false;
	}

	out.resize(static_cast<size_t>(outLen1 + outLen2));
	cipherHex = bytesToHex(out.data(), out.size());
	vectorHex = bytesToHex(iv, sizeof(iv));
	return true;
#else
	(void)plaintext;
	(void)phrase;
	(void)cipherHex;
	(void)vectorHex;
	return false;
#endif
}

bool decryptAesCbcHex(const std::string &cipherHex, const std::string &vectorHex, const std::string &phrase,
                      std::string &plaintext)
{
#if VDONINJA_HAS_OPENSSL
	if (phrase.empty()) {
		return false;
	}

	std::vector<uint8_t> key;
	std::vector<uint8_t> cipher;
	std::vector<uint8_t> iv;
	if (!hexToBytes(sha256(phrase), key) || key.size() != 32 || !hexToBytes(cipherHex, cipher) || !hexToBytes(vectorHex, iv) ||
	    iv.size() != 16) {
		return false;
	}

	EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
	if (!ctx) {
		return false;
	}

	std::vector<uint8_t> out(cipher.size() + EVP_MAX_BLOCK_LENGTH);
	int outLen1 = 0;
	int outLen2 = 0;
	const bool ok = EVP_DecryptInit_ex(ctx, EVP_aes_256_cbc(), nullptr, key.data(), iv.data()) == 1 &&
	                EVP_DecryptUpdate(ctx, out.data(), &outLen1, cipher.data(), static_cast<int>(cipher.size())) == 1 &&
	                EVP_DecryptFinal_ex(ctx, out.data() + outLen1, &outLen2) == 1;

	EVP_CIPHER_CTX_free(ctx);
	if (!ok) {
		return false;
	}

	out.resize(static_cast<size_t>(outLen1 + outLen2));
	plaintext.assign(reinterpret_cast<const char *>(out.data()), out.size());
	return true;
#else
	(void)cipherHex;
	(void)vectorHex;
	(void)phrase;
	(void)plaintext;
	return false;
#endif
}

bool isExplicitlyDisabledPassword(const std::string &password)
{
	if (password.empty()) {
		return false;
	}

	std::string lowered;
	lowered.reserve(password.size());
	for (char c : trim(password)) {
		lowered += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	}

	return lowered == "false" || lowered == "0" || lowered == "off" || lowered == "no";
}

std::string resolveEffectivePassword(const std::string &password, const std::string &defaultPassword, bool &disabled)
{
	const std::string trimmedPassword = trim(password);
	if (isExplicitlyDisabledPassword(trimmedPassword)) {
		disabled = true;
		return "";
	}

	disabled = false;
	if (trimmedPassword.empty()) {
		return defaultPassword;
	}
	return trimmedPassword;
}

} // namespace

VDONinjaSignaling::VDONinjaSignaling()
{
	localUUID_ = generateUUID();
	logInfo("Signaling client created with UUID: %s", localUUID_.c_str());
}

VDONinjaSignaling::~VDONinjaSignaling()
{
	disconnect();
}

bool VDONinjaSignaling::connect(const std::string &wssHost)
{
	if (connected_) {
		logWarning("Already connected to signaling server");
		return true;
	}

	wssHost_ = wssHost;
	shouldRun_ = true;
	reconnectAttempts_ = 0;

	// Start WebSocket thread
	wsThread_ = std::thread(&VDONinjaSignaling::wsThreadFunc, this);

	// Wait briefly for connection
	for (int i = 0; i < 50 && !connected_; i++) {
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}

	return connected_;
}

void VDONinjaSignaling::disconnect()
{
	shouldRun_ = false;
	connected_ = false;

	// Signal send thread to exit
	{
		std::lock_guard<std::mutex> lock(sendMutex_);
		sendCv_.notify_all();
	}

	// Close WebSocket
	if (wsHandle_) {
		auto ws = static_cast<std::shared_ptr<rtc::WebSocket> *>(wsHandle_);
		(*ws)->close();
		delete ws;
		wsHandle_ = nullptr;
	}

	// Wait for thread to finish
	if (wsThread_.joinable()) {
		wsThread_.join();
	}

	// Reset state
	currentRoom_ = RoomInfo{};
	publishedStream_ = StreamInfo{};
	viewingStreams_.clear();

	logInfo("Disconnected from signaling server");

	if (onDisconnected_) {
		onDisconnected_();
	}
}

bool VDONinjaSignaling::isConnected() const
{
	return connected_;
}

void VDONinjaSignaling::wsThreadFunc()
{
	logInfo("Connecting to signaling server: %s", wssHost_.c_str());

	try {
		auto ws = std::make_shared<rtc::WebSocket>();
		wsHandle_ = new std::shared_ptr<rtc::WebSocket>(ws);

		ws->onOpen([this]() {
			logInfo("WebSocket connected to signaling server");
			connected_ = true;
			reconnectAttempts_ = 0;
			if (onConnected_) {
				onConnected_();
			}
		});

		ws->onClosed([this]() {
			logInfo("WebSocket closed");
			connected_ = false;
			if (shouldRun_ && autoReconnect_) {
				attemptReconnect();
			}
		});

		ws->onError([this](const std::string &error) {
			logError("WebSocket error: %s", error.c_str());
			if (onError_) {
				onError_(error);
			}
		});

		ws->onMessage([this](auto data) {
			if (std::holds_alternative<std::string>(data)) {
				processMessage(std::get<std::string>(data));
			}
		});

		ws->open(wssHost_);

		// Main loop - process send queue
		while (shouldRun_) {
			std::unique_lock<std::mutex> lock(sendMutex_);
			sendCv_.wait_for(lock, std::chrono::milliseconds(100),
			                 [this] { return !sendQueue_.empty() || !shouldRun_; });

			while (!sendQueue_.empty() && connected_) {
				std::string msg = sendQueue_.front();
				sendQueue_.pop();
				lock.unlock();

				try {
					ws->send(msg);
					logDebug("Sent: %s", msg.c_str());
				} catch (const std::exception &e) {
					logError("Failed to send message: %s", e.what());
				}

				lock.lock();
			}
		}
	} catch (const std::exception &e) {
		logError("WebSocket thread error: %s", e.what());
		connected_ = false;
		if (onError_) {
			onError_(e.what());
		}
	}
}

void VDONinjaSignaling::attemptReconnect()
{
	if (reconnectAttempts_ >= maxReconnectAttempts_) {
		logError("Max reconnection attempts reached");
		if (onError_) {
			onError_("Max reconnection attempts reached");
		}
		return;
	}

	reconnectAttempts_++;
	int delay = std::min(1000 * (1 << reconnectAttempts_), 30000); // Exponential backoff, max 30s

	logInfo("Reconnecting in %d ms (attempt %d/%d)", delay, reconnectAttempts_, maxReconnectAttempts_);
	std::this_thread::sleep_for(std::chrono::milliseconds(delay));

	if (shouldRun_) {
		// Clean up old connection
		if (wsHandle_) {
			auto ws = static_cast<std::shared_ptr<rtc::WebSocket> *>(wsHandle_);
			delete ws;
			wsHandle_ = nullptr;
		}

		// Restart connection in same thread
		wsThreadFunc();
	}
}

void VDONinjaSignaling::processMessage(const std::string &message)
{
	logDebug("Received: %s", message.c_str());

	auto dispatchParsed = [this](const ParsedSignalMessage &parsed) {
		switch (parsed.kind) {
		case ParsedSignalKind::Listing:
			logInfo("Received room listing");
			currentRoom_.isJoined = true;
			currentRoom_.members = parsed.listingMembers;
			if (onRoomJoined_) {
				onRoomJoined_(currentRoom_.members);
			}
			break;
		case ParsedSignalKind::Offer:
			logInfo("Received offer from %s", parsed.uuid.c_str());
			if (onOffer_) {
				onOffer_(parsed.uuid, parsed.sdp, parsed.session);
			}
			break;
		case ParsedSignalKind::Answer:
			logInfo("Received answer from %s", parsed.uuid.c_str());
			if (onAnswer_) {
				onAnswer_(parsed.uuid, parsed.sdp, parsed.session);
			}
			break;
		case ParsedSignalKind::Candidate:
			logDebug("Received ICE candidate from %s", parsed.uuid.c_str());
			if (onIceCandidate_) {
				onIceCandidate_(parsed.uuid, parsed.candidate, parsed.mid, parsed.session);
			}
			break;
		case ParsedSignalKind::CandidatesBundle:
			logDebug("Received ICE candidate bundle from %s", parsed.uuid.c_str());
			if (onIceCandidate_) {
				for (const auto &candidate : parsed.candidates) {
					onIceCandidate_(parsed.uuid, candidate.candidate, candidate.mid, parsed.session);
				}
			}
			break;
		case ParsedSignalKind::Request:
			handleRequest(parsed);
			break;
		case ParsedSignalKind::Alert:
			logWarning("Server alert: %s", parsed.alert.c_str());
			if (onError_) {
				onError_(parsed.alert);
			}
			break;
		case ParsedSignalKind::VideoAddedToRoom:
			logInfo("Stream added to room: %s by %s", parsed.streamId.c_str(), parsed.uuid.c_str());
			if (onStreamAdded_) {
				onStreamAdded_(parsed.streamId, parsed.uuid);
			}
			break;
		case ParsedSignalKind::VideoRemovedFromRoom:
			logInfo("Stream removed from room: %s by %s", parsed.streamId.c_str(), parsed.uuid.c_str());
			if (onStreamRemoved_) {
				onStreamRemoved_(parsed.streamId, parsed.uuid);
			}
			break;
		default:
			logDebug("Unknown message type");
			break;
		}
	};

	const std::string activePassword = getActiveSignalingPassword();
	JsonParser raw(message);
	if (!activePassword.empty() && raw.hasKey("vector")) {
		const std::string phrase = activePassword + salt_;
		const std::string vector = raw.getString("vector");

		ParsedSignalMessage decryptedParsed;
		decryptedParsed.uuid = getAnyString(raw, {"UUID", "uuid"});
		decryptedParsed.session = getAnyString(raw, {"session"});

		if (raw.hasKey("description")) {
			const std::string encryptedDescription = raw.getRaw("description");
			if (!encryptedDescription.empty() && encryptedDescription[0] != '{') {
				std::string decryptedDescription;
				if (!decryptAesCbcHex(encryptedDescription, vector, phrase, decryptedDescription)) {
					logWarning("Failed to decrypt incoming SDP description");
					return;
				}

				JsonParser desc(decryptedDescription);
				decryptedParsed.type = getAnyString(desc, {"type"});
				decryptedParsed.sdp = getAnyString(desc, {"sdp"});
				if (decryptedParsed.type == "offer") {
					decryptedParsed.kind = ParsedSignalKind::Offer;
					dispatchParsed(decryptedParsed);
					return;
				}
				if (decryptedParsed.type == "answer") {
					decryptedParsed.kind = ParsedSignalKind::Answer;
					dispatchParsed(decryptedParsed);
					return;
				}
			}
		}

		if (raw.hasKey("candidate")) {
			const std::string encryptedCandidate = raw.getRaw("candidate");
			if (!encryptedCandidate.empty() && encryptedCandidate[0] != '{') {
				std::string decryptedCandidate;
				if (!decryptAesCbcHex(encryptedCandidate, vector, phrase, decryptedCandidate)) {
					logWarning("Failed to decrypt incoming ICE candidate");
					return;
				}

				JsonParser candidateJson(decryptedCandidate);
				decryptedParsed.kind = ParsedSignalKind::Candidate;
				decryptedParsed.candidate = getAnyString(candidateJson, {"candidate"});
				decryptedParsed.mid = getAnyString(candidateJson, {"mid", "sdpMid", "smid", "rmid"});
				dispatchParsed(decryptedParsed);
				return;
			}
		}

		if (raw.hasKey("candidates")) {
			const std::string encryptedCandidates = raw.getRaw("candidates");
			if (!encryptedCandidates.empty() && encryptedCandidates[0] != '[' && encryptedCandidates[0] != '{') {
				std::string decryptedCandidates;
				if (!decryptAesCbcHex(encryptedCandidates, vector, phrase, decryptedCandidates)) {
					logWarning("Failed to decrypt incoming ICE candidate bundle");
					return;
				}

				JsonParser wrapped("{\"candidates\":" + decryptedCandidates + "}");
				decryptedParsed.kind = ParsedSignalKind::CandidatesBundle;
				for (const auto &rawEntry : wrapped.getArray("candidates")) {
					if (rawEntry.empty()) {
						continue;
					}

					if (rawEntry[0] == '{') {
						JsonParser candidateJson(rawEntry);
						ParsedCandidate candidate;
						candidate.candidate = getAnyString(candidateJson, {"candidate"});
						candidate.mid = getAnyString(candidateJson, {"mid", "sdpMid", "smid", "rmid"});
						if (!candidate.candidate.empty()) {
							decryptedParsed.candidates.push_back(candidate);
						}
					} else {
						ParsedCandidate candidate;
						candidate.candidate = rawEntry;
						candidate.mid = getAnyString(raw, {"mid", "sdpMid", "smid", "rmid"});
						if (!candidate.candidate.empty()) {
							decryptedParsed.candidates.push_back(candidate);
						}
					}
				}

				dispatchParsed(decryptedParsed);
				return;
			}
		}
	}

	ParsedSignalMessage parsed;
	std::string error;
	if (!parseSignalingMessage(message, parsed, &error)) {
		logError("Failed to parse message: %s", error.c_str());
		return;
	}
	dispatchParsed(parsed);
}

void VDONinjaSignaling::handleRequest(const ParsedSignalMessage &message)
{
	logInfo("Received request: %s from %s", message.request.c_str(), message.uuid.c_str());

	// VDO.Ninja can request publisher offers with different request labels depending
	// on endpoint/flow.
	if ((message.request == "offerSDP" || message.request == "sendOffer" || message.request == "play" ||
	     message.request == "joinroom") &&
	    onOfferRequest_) {
		onOfferRequest_(message.uuid, message.session);
	}
}

void VDONinjaSignaling::sendMessage(const std::string &message)
{
	if (!connected_) {
		logWarning("Cannot send message - not connected");
		return;
	}

	std::lock_guard<std::mutex> lock(sendMutex_);
	sendQueue_.push(message);
	sendCv_.notify_one();
}

void VDONinjaSignaling::queueMessage(const std::string &message)
{
	sendMessage(message);
}

bool VDONinjaSignaling::joinRoom(const std::string &roomId, const std::string &password)
{
	if (!connected_) {
		logError("Cannot join room - not connected");
		return false;
	}

	bool passwordDisabled = false;
	std::string effectivePassword = resolveEffectivePassword(password, defaultPassword_, passwordDisabled);
	std::string hashedRoom = passwordDisabled ? hashRoomId(roomId, "", salt_) : hashRoomId(roomId, effectivePassword, salt_);

	currentRoom_.roomId = roomId;
	currentRoom_.hashedRoomId = hashedRoom;
	currentRoom_.password = passwordDisabled ? "" : effectivePassword;

	JsonBuilder msg;
	msg.add("request", "joinroom");
	msg.add("roomid", hashedRoom);
	msg.add("claim", true);

	sendMessage(msg.build());
	logInfo("Joining room: %s (resolved: %s)", roomId.c_str(), hashedRoom.c_str());

	return true;
}

bool VDONinjaSignaling::leaveRoom()
{
	if (!currentRoom_.isJoined) {
		return true;
	}

	JsonBuilder msg;
	msg.add("request", "leaveroom");

	sendMessage(msg.build());
	currentRoom_ = RoomInfo{};

	logInfo("Left room");
	return true;
}

bool VDONinjaSignaling::isInRoom() const
{
	return currentRoom_.isJoined;
}

std::string VDONinjaSignaling::getCurrentRoomId() const
{
	return currentRoom_.roomId;
}

bool VDONinjaSignaling::publishStream(const std::string &streamId, const std::string &password)
{
	if (!connected_) {
		logError("Cannot publish - not connected");
		return false;
	}

	bool passwordDisabled = false;
	std::string effectivePassword = resolveEffectivePassword(password, defaultPassword_, passwordDisabled);
	std::string hashedStream = passwordDisabled ? sanitizeStreamId(streamId) : hashStreamId(streamId, effectivePassword, salt_);

	publishedStream_.streamId = streamId;
	publishedStream_.hashedStreamId = hashedStream;
	publishedStream_.password = passwordDisabled ? "" : effectivePassword;
	publishedStream_.isViewing = false;
	publishedStream_.isPublishing = true;

	JsonBuilder msg;
	msg.add("request", "seed");
	msg.add("streamID", hashedStream);

	sendMessage(msg.build());
	logInfo("Publishing stream: %s (hashed: %s)", streamId.c_str(), hashedStream.c_str());

	return true;
}

bool VDONinjaSignaling::unpublishStream()
{
	if (!publishedStream_.isPublishing) {
		return true;
	}

	JsonBuilder msg;
	msg.add("request", "unseed");
	msg.add("streamID", publishedStream_.hashedStreamId);

	sendMessage(msg.build());
	publishedStream_ = StreamInfo{};

	logInfo("Unpublished stream");
	return true;
}

bool VDONinjaSignaling::isPublishing() const
{
	return publishedStream_.isPublishing;
}

std::string VDONinjaSignaling::getPublishedStreamId() const
{
	return publishedStream_.streamId;
}

bool VDONinjaSignaling::viewStream(const std::string &streamId, const std::string &password)
{
	if (!connected_) {
		logError("Cannot view stream - not connected");
		return false;
	}

	bool passwordDisabled = false;
	std::string effectivePassword = resolveEffectivePassword(password, defaultPassword_, passwordDisabled);
	std::string hashedStream = passwordDisabled ? sanitizeStreamId(streamId) : hashStreamId(streamId, effectivePassword, salt_);

	StreamInfo stream;
	stream.streamId = streamId;
	stream.hashedStreamId = hashedStream;
	stream.password = passwordDisabled ? "" : effectivePassword;
	stream.isViewing = true;
	viewingStreams_[streamId] = stream;

	JsonBuilder msg;
	msg.add("request", "play");
	msg.add("streamID", hashedStream);

	sendMessage(msg.build());
	logInfo("Requesting to view stream: %s (hashed: %s)", streamId.c_str(), hashedStream.c_str());

	return true;
}

bool VDONinjaSignaling::stopViewing(const std::string &streamId)
{
	auto it = viewingStreams_.find(streamId);
	if (it == viewingStreams_.end()) {
		return true;
	}

	JsonBuilder msg;
	msg.add("request", "stopPlay");
	msg.add("streamID", it->second.hashedStreamId);

	sendMessage(msg.build());
	viewingStreams_.erase(it);

	logInfo("Stopped viewing stream: %s", streamId.c_str());
	return true;
}

std::string VDONinjaSignaling::getActiveSignalingPassword() const
{
	if (publishedStream_.isPublishing && !publishedStream_.password.empty()) {
		return publishedStream_.password;
	}

	for (const auto &entry : viewingStreams_) {
		if (entry.second.isViewing && !entry.second.password.empty()) {
			return entry.second.password;
		}
	}

	if (currentRoom_.isJoined && !currentRoom_.password.empty()) {
		return currentRoom_.password;
	}

	return "";
}

void VDONinjaSignaling::sendOffer(const std::string &uuid, const std::string &sdp, const std::string &session)
{
	JsonBuilder description;
	description.add("type", "offer");
	description.add("sdp", sdp);

	JsonBuilder msg;
	msg.add("UUID", uuid);
	msg.add("session", session);
	if (publishedStream_.isPublishing && !publishedStream_.hashedStreamId.empty()) {
		msg.add("streamID", publishedStream_.hashedStreamId);
	}

	const std::string activePassword = getActiveSignalingPassword();
	if (!activePassword.empty()) {
		std::string encryptedDescription;
		std::string vector;
		if (encryptAesCbcHex(description.build(), activePassword + salt_, encryptedDescription, vector)) {
			msg.add("description", encryptedDescription);
			msg.add("vector", vector);
		} else {
			logWarning("Failed to encrypt offer SDP; sending plaintext");
			msg.addRaw("description", description.build());
			msg.add("sdp", sdp);
			msg.add("type", "offer");
		}
	} else {
		msg.addRaw("description", description.build());
		msg.add("sdp", sdp);
		msg.add("type", "offer");
	}

	sendMessage(msg.build());
	logDebug("Sent offer to %s", uuid.c_str());
}

void VDONinjaSignaling::sendAnswer(const std::string &uuid, const std::string &sdp, const std::string &session)
{
	JsonBuilder description;
	description.add("type", "answer");
	description.add("sdp", sdp);

	JsonBuilder msg;
	msg.add("UUID", uuid);
	msg.add("session", session);

	const std::string activePassword = getActiveSignalingPassword();
	if (!activePassword.empty()) {
		std::string encryptedDescription;
		std::string vector;
		if (encryptAesCbcHex(description.build(), activePassword + salt_, encryptedDescription, vector)) {
			msg.add("description", encryptedDescription);
			msg.add("vector", vector);
		} else {
			logWarning("Failed to encrypt answer SDP; sending plaintext");
			msg.addRaw("description", description.build());
			msg.add("sdp", sdp);
			msg.add("type", "answer");
		}
	} else {
		msg.addRaw("description", description.build());
		msg.add("sdp", sdp);
		msg.add("type", "answer");
	}

	sendMessage(msg.build());
	logDebug("Sent answer to %s", uuid.c_str());
}

void VDONinjaSignaling::sendIceCandidate(const std::string &uuid, const std::string &candidate, const std::string &mid,
                                         const std::string &session)
{
	JsonBuilder msg;
	msg.add("UUID", uuid);
	msg.add("session", session);

	const std::string activePassword = getActiveSignalingPassword();
	if (!activePassword.empty()) {
		JsonBuilder candidatePayload;
		candidatePayload.add("candidate", candidate);
		candidatePayload.add("sdpMid", mid);

		std::string encryptedCandidate;
		std::string vector;
		if (encryptAesCbcHex(candidatePayload.build(), activePassword + salt_, encryptedCandidate, vector)) {
			msg.add("candidate", encryptedCandidate);
			msg.add("vector", vector);
		} else {
			logWarning("Failed to encrypt ICE candidate; sending plaintext");
			msg.add("candidate", candidate);
			msg.add("mid", mid);
		}
	} else {
		msg.add("candidate", candidate);
		msg.add("mid", mid);
	}

	sendMessage(msg.build());
	logDebug("Sent ICE candidate to %s", uuid.c_str());
}

void VDONinjaSignaling::sendDataMessage(const std::string &uuid, const std::string &data)
{
	JsonBuilder msg;
	msg.add("UUID", uuid);
	msg.add("data", data);

	sendMessage(msg.build());
}

void VDONinjaSignaling::setOnConnected(OnConnectedCallback callback)
{
	onConnected_ = callback;
}
void VDONinjaSignaling::setOnDisconnected(OnDisconnectedCallback callback)
{
	onDisconnected_ = callback;
}
void VDONinjaSignaling::setOnError(OnErrorCallback callback)
{
	onError_ = callback;
}
void VDONinjaSignaling::setOnOffer(OnOfferCallback callback)
{
	onOffer_ = callback;
}
void VDONinjaSignaling::setOnAnswer(OnAnswerCallback callback)
{
	onAnswer_ = callback;
}
void VDONinjaSignaling::setOnOfferRequest(OnOfferRequestCallback callback)
{
	onOfferRequest_ = callback;
}
void VDONinjaSignaling::setOnIceCandidate(OnIceCandidateCallback callback)
{
	onIceCandidate_ = callback;
}
void VDONinjaSignaling::setOnRoomJoined(OnRoomJoinedCallback callback)
{
	onRoomJoined_ = callback;
}
void VDONinjaSignaling::setOnStreamAdded(OnStreamAddedCallback callback)
{
	onStreamAdded_ = callback;
}
void VDONinjaSignaling::setOnStreamRemoved(OnStreamRemovedCallback callback)
{
	onStreamRemoved_ = callback;
}
void VDONinjaSignaling::setOnData(OnDataCallback callback)
{
	onData_ = callback;
}

void VDONinjaSignaling::setSalt(const std::string &salt)
{
	salt_ = trim(salt);
	if (salt_.empty()) {
		salt_ = DEFAULT_SALT;
	}
}
void VDONinjaSignaling::setDefaultPassword(const std::string &password)
{
	defaultPassword_ = password;
}
void VDONinjaSignaling::setAutoReconnect(bool enable, int maxAttempts)
{
	autoReconnect_ = enable;
	maxReconnectAttempts_ = maxAttempts;
}

std::string VDONinjaSignaling::getLocalUUID() const
{
	return localUUID_;
}

} // namespace vdoninja
