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

bool encryptAesCbcHex(const std::string &plaintext, const std::string &phrase, std::string &cipherHex,
                      std::string &vectorHex)
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
	const bool ok = EVP_EncryptInit_ex(ctx, EVP_aes_256_cbc(), nullptr, key.data(), iv) == 1 &&
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
	if (!hexToBytes(sha256(phrase), key) || key.size() != 32 || !hexToBytes(cipherHex, cipher) ||
	    !hexToBytes(vectorHex, iv) || iv.size() != 16) {
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

std::string resolveEffectivePassword(const std::string &password, const std::string &defaultPassword, bool &disabled)
{
	const std::string trimmedPassword = trim(password);
	if (isPasswordDisabledToken(trimmedPassword)) {
		disabled = true;
		return "";
	}

	disabled = false;
	if (trimmedPassword.empty()) {
		return defaultPassword;
	}
	return trimmedPassword;
}

std::string asciiLower(std::string value)
{
	for (char &c : value) {
		c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	}
	return value;
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
	if (shouldRun_) {
		logWarning("Signaling connection thread is already running");
		return connected_;
	}

	{
		std::lock_guard<std::mutex> lock(stateMutex_);
		wssHost_ = wssHost;
	}
	shouldRun_ = true;
	needsReconnect_ = false;

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
	{
		std::lock_guard<std::mutex> lock(handleMutex_);
		if (wsHandle_) {
			auto ws = static_cast<std::shared_ptr<rtc::WebSocket> *>(wsHandle_);
			(*ws)->close();
			delete ws;
			wsHandle_ = nullptr;
		}
	}

	// Wait for thread to finish
	if (wsThread_.joinable()) {
		wsThread_.join();
	}

	// Reset state
	{
		std::lock_guard<std::mutex> lock(stateMutex_);
		currentRoom_ = RoomInfo{};
		publishedStream_ = StreamInfo{};
		viewingStreams_.clear();
	}

	logInfo("Disconnected from signaling server");

	OnDisconnectedCallback cb;
	{
		std::lock_guard<std::mutex> lock(callbackMutex_);
		cb = onDisconnected_;
	}
	if (cb) {
		cb();
	}
}

bool VDONinjaSignaling::isConnected() const
{
	return connected_;
}

void VDONinjaSignaling::wsThreadFunc()
{
	int reconnectAttempts = 0;

	while (shouldRun_) {
		std::string host;
		bool autoReconnect;
		int maxAttempts;
		{
			std::lock_guard<std::mutex> lock(stateMutex_);
			host = wssHost_;
			autoReconnect = autoReconnect_;
			maxAttempts = maxReconnectAttempts_;
		}

		logInfo("Connecting to signaling server: %s", host.c_str());
		needsReconnect_ = false;

		try {
			auto ws = std::make_shared<rtc::WebSocket>();
			{
				std::lock_guard<std::mutex> lock(handleMutex_);
				wsHandle_ = new std::shared_ptr<rtc::WebSocket>(ws);
			}

			ws->onOpen([this, &reconnectAttempts]() {
				logInfo("WebSocket connected to signaling server");
				connected_ = true;
				reconnectAttempts = 0;

				OnConnectedCallback cb;
				{
					std::lock_guard<std::mutex> lock(callbackMutex_);
					cb = onConnected_;
				}
				if (cb) {
					cb();
				}
			});

			ws->onClosed([this]() {
				logInfo("WebSocket closed");
				connected_ = false;
				needsReconnect_ = true;
				// Wake send loop so it can exit
				std::lock_guard<std::mutex> lock(sendMutex_);
				sendCv_.notify_all();
			});

			ws->onError([this](const std::string &error) {
				logError("WebSocket error: %s", error.c_str());
				OnErrorCallback cb;
				{
					std::lock_guard<std::mutex> lock(callbackMutex_);
					cb = onError_;
				}
				if (cb) {
					cb(error);
				}
			});

			ws->onMessage([this](auto data) {
				if (std::holds_alternative<std::string>(data)) {
					processMessage(std::get<std::string>(data));
				}
			});

			ws->open(host);

			// Main loop - process send queue
			while (shouldRun_ && !needsReconnect_) {
				std::unique_lock<std::mutex> lock(sendMutex_);
				sendCv_.wait_for(lock, std::chrono::milliseconds(100),
				                 [this] { return !sendQueue_.empty() || !shouldRun_ || needsReconnect_.load(); });

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

			// Clean up this connection
			{
				std::lock_guard<std::mutex> lock(handleMutex_);
				if (wsHandle_) {
					auto stored = static_cast<std::shared_ptr<rtc::WebSocket> *>(wsHandle_);
					delete stored;
					wsHandle_ = nullptr;
				}
			}
		} catch (const std::exception &e) {
			logError("WebSocket thread error: %s", e.what());
			connected_ = false;

			OnErrorCallback cb;
			{
				std::lock_guard<std::mutex> lock(callbackMutex_);
				cb = onError_;
			}
			if (cb) {
				cb(e.what());
			}

			std::lock_guard<std::mutex> lock(handleMutex_);
			if (wsHandle_) {
				auto stored = static_cast<std::shared_ptr<rtc::WebSocket> *>(wsHandle_);
				delete stored;
				wsHandle_ = nullptr;
			}
		}

		// Reconnect logic (iterative, not recursive)
		if (!shouldRun_ || !autoReconnect || !needsReconnect_) {
			break;
		}

		reconnectAttempts++;
		if (reconnectAttempts > maxAttempts) {
			logError("Max reconnection attempts reached");
			OnErrorCallback cb;
			{
				std::lock_guard<std::mutex> lock(callbackMutex_);
				cb = onError_;
			}
			if (cb) {
				cb("Max reconnection attempts reached");
			}
			break;
		}

		const int exponentialDelay = std::min(1000 * (1 << reconnectAttempts), 30000);
		const int delay = std::max(exponentialDelay, MIN_RECONNECT_INTERVAL_MS);
		logInfo("Reconnecting in %d ms (attempt %d/%d)", delay, reconnectAttempts, maxAttempts);

		// Sleep in small increments so we can respond to shouldRun_ going false
		for (int waited = 0; waited < delay && shouldRun_; waited += 100) {
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
		}
	}
}

void VDONinjaSignaling::processMessage(const std::string &message)
{
	logDebug("Received: %s", message.c_str());

	auto dispatchParsed = [this](const ParsedSignalMessage &parsed) {
		switch (parsed.kind) {
		case ParsedSignalKind::Listing: {
			logInfo("Received room listing");
			std::vector<std::string> members;
			{
				std::lock_guard<std::mutex> lock(stateMutex_);
				currentRoom_.isJoined = true;
				currentRoom_.members = parsed.listingMembers;
				members = currentRoom_.members;
			}
			OnRoomJoinedCallback cb;
			{
				std::lock_guard<std::mutex> lock(callbackMutex_);
				cb = onRoomJoined_;
			}
			if (cb) {
				cb(members);
			}
			break;
		}
		case ParsedSignalKind::Offer: {
			logInfo("Received offer from %s", parsed.uuid.c_str());
			OnOfferCallback cb;
			{
				std::lock_guard<std::mutex> lock(callbackMutex_);
				cb = onOffer_;
			}
			if (cb) {
				cb(parsed.uuid, parsed.sdp, parsed.session);
			}
			break;
		}
		case ParsedSignalKind::Answer: {
			logInfo("Received answer from %s", parsed.uuid.c_str());
			OnAnswerCallback cb;
			{
				std::lock_guard<std::mutex> lock(callbackMutex_);
				cb = onAnswer_;
			}
			if (cb) {
				cb(parsed.uuid, parsed.sdp, parsed.session);
			}
			break;
		}
		case ParsedSignalKind::Candidate: {
			logDebug("Received ICE candidate from %s", parsed.uuid.c_str());
			OnIceCandidateCallback cb;
			{
				std::lock_guard<std::mutex> lock(callbackMutex_);
				cb = onIceCandidate_;
			}
			if (cb) {
				cb(parsed.uuid, parsed.candidate, parsed.mid, parsed.session);
			}
			break;
		}
		case ParsedSignalKind::CandidatesBundle: {
			logDebug("Received ICE candidate bundle from %s", parsed.uuid.c_str());
			OnIceCandidateCallback cb;
			{
				std::lock_guard<std::mutex> lock(callbackMutex_);
				cb = onIceCandidate_;
			}
			if (cb) {
				for (const auto &candidate : parsed.candidates) {
					cb(parsed.uuid, candidate.candidate, candidate.mid, parsed.session);
				}
			}
			break;
		}
		case ParsedSignalKind::Request:
			handleRequest(parsed);
			break;
		case ParsedSignalKind::Alert: {
			logWarning("Server alert: %s", parsed.alert.c_str());
			OnErrorCallback cb;
			{
				std::lock_guard<std::mutex> lock(callbackMutex_);
				cb = onError_;
			}
			if (cb) {
				cb(parsed.alert);
			}
			break;
		}
		case ParsedSignalKind::VideoAddedToRoom: {
			logInfo("Stream added to room: %s by %s", parsed.streamId.c_str(), parsed.uuid.c_str());
			OnStreamAddedCallback cb;
			{
				std::lock_guard<std::mutex> lock(callbackMutex_);
				cb = onStreamAdded_;
			}
			if (cb) {
				cb(parsed.streamId, parsed.uuid);
			}
			break;
		}
		case ParsedSignalKind::VideoRemovedFromRoom: {
			logInfo("Stream removed from room: %s by %s", parsed.streamId.c_str(), parsed.uuid.c_str());
			OnStreamRemovedCallback cb;
			{
				std::lock_guard<std::mutex> lock(callbackMutex_);
				cb = onStreamRemoved_;
			}
			if (cb) {
				cb(parsed.streamId, parsed.uuid);
			}
			break;
		}
		default:
			logDebug("Unknown message type");
			break;
		}
	};

	const std::string activePassword = getActiveSignalingPassword();
	std::string processSalt;
	{
		std::lock_guard<std::mutex> lock(stateMutex_);
		processSalt = salt_;
	}
	JsonParser raw(message);
	if (!activePassword.empty() && raw.hasKey("vector")) {
		const std::string phrase = activePassword + processSalt;
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
	const std::string requestLower = asciiLower(message.request);

	// VDO.Ninja requests publisher offers with offerSDP/sendoffer/play. For custom
	// signaling compatibility, accept joinroom only when the request also carries
	// a stream identifier; plain joinroom events are room-admission flow.
	const bool joinroomOfferCompat = requestLower == "joinroom" && !message.streamId.empty();
	if (requestLower == "offersdp" || requestLower == "sendoffer" || requestLower == "play" || joinroomOfferCompat) {
		OnOfferRequestCallback cb;
		{
			std::lock_guard<std::mutex> lock(callbackMutex_);
			cb = onOfferRequest_;
		}
		if (cb) {
			cb(message.uuid, message.session);
		}
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

bool VDONinjaSignaling::joinRoom(const std::string &roomId, const std::string &password, bool claimDirector)
{
	if (!connected_) {
		logError("Cannot join room - not connected");
		return false;
	}

	std::string defaultPw;
	std::string salt;
	{
		std::lock_guard<std::mutex> lock(stateMutex_);
		defaultPw = defaultPassword_;
		salt = salt_;
	}

	bool passwordDisabled = false;
	std::string effectivePassword = resolveEffectivePassword(password, defaultPw, passwordDisabled);
	std::string hashedRoom = passwordDisabled ? roomId : hashRoomId(roomId, effectivePassword, salt);

	{
		std::lock_guard<std::mutex> lock(stateMutex_);
		currentRoom_.roomId = roomId;
		currentRoom_.hashedRoomId = hashedRoom;
		currentRoom_.password = passwordDisabled ? "" : effectivePassword;
	}

	JsonBuilder msg;
	msg.add("request", "joinroom");
	msg.add("roomid", hashedRoom);
	if (claimDirector) {
		msg.add("claim", true);
	}

	sendMessage(msg.build());
	logInfo("Joining room: %s (resolved: %s, claim: %s)", roomId.c_str(), hashedRoom.c_str(),
	        claimDirector ? "true" : "false");

	return true;
}

bool VDONinjaSignaling::leaveRoom()
{
	{
		std::lock_guard<std::mutex> lock(stateMutex_);
		if (!currentRoom_.isJoined) {
			return true;
		}
	}

	JsonBuilder msg;
	msg.add("request", "leaveroom");

	sendMessage(msg.build());

	{
		std::lock_guard<std::mutex> lock(stateMutex_);
		currentRoom_ = RoomInfo{};
	}

	logInfo("Left room");
	return true;
}

bool VDONinjaSignaling::isInRoom() const
{
	std::lock_guard<std::mutex> lock(stateMutex_);
	return currentRoom_.isJoined;
}

std::string VDONinjaSignaling::getCurrentRoomId() const
{
	std::lock_guard<std::mutex> lock(stateMutex_);
	return currentRoom_.roomId;
}

bool VDONinjaSignaling::publishStream(const std::string &streamId, const std::string &password)
{
	if (!connected_) {
		logError("Cannot publish - not connected");
		return false;
	}

	std::string defaultPw;
	std::string salt;
	{
		std::lock_guard<std::mutex> lock(stateMutex_);
		defaultPw = defaultPassword_;
		salt = salt_;
	}

	bool passwordDisabled = false;
	std::string effectivePassword = resolveEffectivePassword(password, defaultPw, passwordDisabled);
	std::string hashedStream = passwordDisabled ? streamId : hashStreamId(streamId, effectivePassword, salt);

	{
		std::lock_guard<std::mutex> lock(stateMutex_);
		publishedStream_.streamId = streamId;
		publishedStream_.hashedStreamId = hashedStream;
		publishedStream_.password = passwordDisabled ? "" : effectivePassword;
		publishedStream_.isViewing = false;
		publishedStream_.isPublishing = true;
	}

	JsonBuilder msg;
	msg.add("request", "seed");
	msg.add("streamID", hashedStream);

	sendMessage(msg.build());
	logInfo("Publishing stream: %s (hashed: %s)", streamId.c_str(), hashedStream.c_str());

	return true;
}

bool VDONinjaSignaling::unpublishStream()
{
	std::string hashedStreamId;
	{
		std::lock_guard<std::mutex> lock(stateMutex_);
		if (!publishedStream_.isPublishing) {
			return true;
		}
		hashedStreamId = publishedStream_.hashedStreamId;
	}

	JsonBuilder msg;
	msg.add("request", "unseed");
	msg.add("streamID", hashedStreamId);

	sendMessage(msg.build());

	{
		std::lock_guard<std::mutex> lock(stateMutex_);
		publishedStream_ = StreamInfo{};
	}

	logInfo("Unpublished stream");
	return true;
}

bool VDONinjaSignaling::isPublishing() const
{
	std::lock_guard<std::mutex> lock(stateMutex_);
	return publishedStream_.isPublishing;
}

std::string VDONinjaSignaling::getPublishedStreamId() const
{
	std::lock_guard<std::mutex> lock(stateMutex_);
	return publishedStream_.streamId;
}

bool VDONinjaSignaling::viewStream(const std::string &streamId, const std::string &password)
{
	if (!connected_) {
		logError("Cannot view stream - not connected");
		return false;
	}

	std::string defaultPw;
	std::string salt;
	{
		std::lock_guard<std::mutex> lock(stateMutex_);
		defaultPw = defaultPassword_;
		salt = salt_;
	}

	bool passwordDisabled = false;
	std::string effectivePassword = resolveEffectivePassword(password, defaultPw, passwordDisabled);
	std::string hashedStream = passwordDisabled ? streamId : hashStreamId(streamId, effectivePassword, salt);

	StreamInfo stream;
	stream.streamId = streamId;
	stream.hashedStreamId = hashedStream;
	stream.password = passwordDisabled ? "" : effectivePassword;
	stream.isViewing = true;

	{
		std::lock_guard<std::mutex> lock(stateMutex_);
		viewingStreams_[streamId] = stream;
	}

	JsonBuilder msg;
	msg.add("request", "play");
	msg.add("streamID", hashedStream);

	sendMessage(msg.build());
	logInfo("Requesting to view stream: %s (hashed: %s)", streamId.c_str(), hashedStream.c_str());

	return true;
}

bool VDONinjaSignaling::stopViewing(const std::string &streamId)
{
	std::string hashedStreamId;
	{
		std::lock_guard<std::mutex> lock(stateMutex_);
		auto it = viewingStreams_.find(streamId);
		if (it == viewingStreams_.end()) {
			return true;
		}
		hashedStreamId = it->second.hashedStreamId;
		viewingStreams_.erase(it);
	}

	JsonBuilder msg;
	msg.add("request", "stopPlay");
	msg.add("streamID", hashedStreamId);

	sendMessage(msg.build());

	logInfo("Stopped viewing stream: %s", streamId.c_str());
	return true;
}

std::string VDONinjaSignaling::getActiveSignalingPassword() const
{
	std::lock_guard<std::mutex> lock(stateMutex_);
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

	std::string hashedStreamId;
	std::string salt;
	{
		std::lock_guard<std::mutex> lock(stateMutex_);
		if (publishedStream_.isPublishing && !publishedStream_.hashedStreamId.empty()) {
			hashedStreamId = publishedStream_.hashedStreamId;
		}
		salt = salt_;
	}

	JsonBuilder msg;
	msg.add("UUID", uuid);
	msg.add("session", session);
	if (!hashedStreamId.empty()) {
		msg.add("streamID", hashedStreamId);
	}

	const std::string activePassword = getActiveSignalingPassword();
	if (!activePassword.empty()) {
		std::string encryptedDescription;
		std::string vector;
		if (encryptAesCbcHex(description.build(), activePassword + salt, encryptedDescription, vector)) {
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

	std::string salt;
	{
		std::lock_guard<std::mutex> lock(stateMutex_);
		salt = salt_;
	}

	JsonBuilder msg;
	msg.add("UUID", uuid);
	msg.add("session", session);

	const std::string activePassword = getActiveSignalingPassword();
	if (!activePassword.empty()) {
		std::string encryptedDescription;
		std::string vector;
		if (encryptAesCbcHex(description.build(), activePassword + salt, encryptedDescription, vector)) {
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
	std::string salt;
	{
		std::lock_guard<std::mutex> lock(stateMutex_);
		salt = salt_;
	}

	JsonBuilder msg;
	msg.add("UUID", uuid);
	msg.add("type", "local");
	msg.add("session", session);

	std::string normalizedCandidate = candidate;
	if (normalizedCandidate.rfind("a=", 0) == 0) {
		normalizedCandidate.erase(0, 2);
	}

	const std::string activePassword = getActiveSignalingPassword();
	if (!activePassword.empty()) {
		JsonBuilder candidatePayload;
		candidatePayload.add("candidate", normalizedCandidate);
		candidatePayload.add("mid", mid);
		candidatePayload.add("sdpMid", mid);

		std::string encryptedCandidate;
		std::string vector;
		if (encryptAesCbcHex(candidatePayload.build(), activePassword + salt, encryptedCandidate, vector)) {
			msg.add("candidate", encryptedCandidate);
			msg.add("vector", vector);
		} else {
			logWarning("Failed to encrypt ICE candidate; sending plaintext");
			JsonBuilder candidateObj;
			candidateObj.add("candidate", normalizedCandidate);
			candidateObj.add("mid", mid);
			candidateObj.add("sdpMid", mid);
			msg.addRaw("candidate", candidateObj.build());
		}
	} else {
		JsonBuilder candidateObj;
		candidateObj.add("candidate", normalizedCandidate);
		candidateObj.add("mid", mid);
		candidateObj.add("sdpMid", mid);
		msg.addRaw("candidate", candidateObj.build());
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
	std::lock_guard<std::mutex> lock(callbackMutex_);
	onConnected_ = callback;
}
void VDONinjaSignaling::setOnDisconnected(OnDisconnectedCallback callback)
{
	std::lock_guard<std::mutex> lock(callbackMutex_);
	onDisconnected_ = callback;
}
void VDONinjaSignaling::setOnError(OnErrorCallback callback)
{
	std::lock_guard<std::mutex> lock(callbackMutex_);
	onError_ = callback;
}
void VDONinjaSignaling::setOnOffer(OnOfferCallback callback)
{
	std::lock_guard<std::mutex> lock(callbackMutex_);
	onOffer_ = callback;
}
void VDONinjaSignaling::setOnAnswer(OnAnswerCallback callback)
{
	std::lock_guard<std::mutex> lock(callbackMutex_);
	onAnswer_ = callback;
}
void VDONinjaSignaling::setOnOfferRequest(OnOfferRequestCallback callback)
{
	std::lock_guard<std::mutex> lock(callbackMutex_);
	onOfferRequest_ = callback;
}
void VDONinjaSignaling::setOnIceCandidate(OnIceCandidateCallback callback)
{
	std::lock_guard<std::mutex> lock(callbackMutex_);
	onIceCandidate_ = callback;
}
void VDONinjaSignaling::setOnRoomJoined(OnRoomJoinedCallback callback)
{
	std::lock_guard<std::mutex> lock(callbackMutex_);
	onRoomJoined_ = callback;
}
void VDONinjaSignaling::setOnStreamAdded(OnStreamAddedCallback callback)
{
	std::lock_guard<std::mutex> lock(callbackMutex_);
	onStreamAdded_ = callback;
}
void VDONinjaSignaling::setOnStreamRemoved(OnStreamRemovedCallback callback)
{
	std::lock_guard<std::mutex> lock(callbackMutex_);
	onStreamRemoved_ = callback;
}
void VDONinjaSignaling::setOnData(OnDataCallback callback)
{
	std::lock_guard<std::mutex> lock(callbackMutex_);
	onData_ = callback;
}

void VDONinjaSignaling::setSalt(const std::string &salt)
{
	std::lock_guard<std::mutex> lock(stateMutex_);
	salt_ = trim(salt);
	if (salt_.empty()) {
		salt_ = DEFAULT_SALT;
	}
}
void VDONinjaSignaling::setDefaultPassword(const std::string &password)
{
	std::lock_guard<std::mutex> lock(stateMutex_);
	defaultPassword_ = password;
}
void VDONinjaSignaling::setAutoReconnect(bool enable, int maxAttempts)
{
	std::lock_guard<std::mutex> lock(stateMutex_);
	autoReconnect_ = enable;
	maxReconnectAttempts_ = maxAttempts;
}

std::string VDONinjaSignaling::getLocalUUID() const
{
	return localUUID_;
}

} // namespace vdoninja
