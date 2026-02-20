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

#include <chrono>

namespace vdoninja
{

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

	ParsedSignalMessage parsed;
	std::string error;
	if (!parseSignalingMessage(message, parsed, &error)) {
		logError("Failed to parse message: %s", error.c_str());
		return;
	}

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
}

void VDONinjaSignaling::handleRequest(const ParsedSignalMessage &message)
{
	logInfo("Received request: %s from %s", message.request.c_str(), message.uuid.c_str());

	if ((message.request == "offerSDP" || message.request == "sendOffer") && onOfferRequest_) {
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

	std::string effectivePassword = password.empty() ? defaultPassword_ : password;
	std::string hashedRoom = hashRoomId(roomId, effectivePassword, salt_);

	currentRoom_.roomId = roomId;
	currentRoom_.hashedRoomId = hashedRoom;
	currentRoom_.password = effectivePassword;

	JsonBuilder msg;
	msg.add("request", "joinroom");
	msg.add("roomid", hashedRoom);
	msg.add("claim", true);

	sendMessage(msg.build());
	logInfo("Joining room: %s (hashed: %s)", roomId.c_str(), hashedRoom.c_str());

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

	std::string effectivePassword = password.empty() ? defaultPassword_ : password;
	std::string hashedStream = hashStreamId(streamId, effectivePassword, salt_);

	publishedStream_.streamId = streamId;
	publishedStream_.hashedStreamId = hashedStream;
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

	std::string effectivePassword = password.empty() ? defaultPassword_ : password;
	std::string hashedStream = hashStreamId(streamId, effectivePassword, salt_);

	StreamInfo stream;
	stream.streamId = streamId;
	stream.hashedStreamId = hashedStream;
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

void VDONinjaSignaling::sendOffer(const std::string &uuid, const std::string &sdp, const std::string &session)
{
	JsonBuilder description;
	description.add("type", "offer");
	description.add("sdp", sdp);

	JsonBuilder msg;
	msg.add("UUID", uuid);
	msg.addRaw("description", description.build());
	msg.add("sdp", sdp);
	msg.add("type", "offer");
	msg.add("session", session);

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
	msg.addRaw("description", description.build());
	msg.add("sdp", sdp);
	msg.add("type", "answer");
	msg.add("session", session);

	sendMessage(msg.build());
	logDebug("Sent answer to %s", uuid.c_str());
}

void VDONinjaSignaling::sendIceCandidate(const std::string &uuid, const std::string &candidate, const std::string &mid,
                                         const std::string &session)
{
	JsonBuilder msg;
	msg.add("UUID", uuid);
	msg.add("candidate", candidate);
	msg.add("mid", mid);
	msg.add("session", session);

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
	salt_ = salt;
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
