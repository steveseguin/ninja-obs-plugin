/*
 * OBS VDO.Ninja Plugin
 * WHEP viewer hosting implementation
 */

#include "vdoninja-whep-host.h"

#include <chrono>
#include <cstdlib>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <sstream>

#include "vdoninja-peer-manager.h"
#include "vdoninja-signaling.h"
#include "vdoninja-utils.h"

namespace vdoninja
{
VDONinjaWhepHost::VDONinjaWhepHost(VDONinjaPeerManager &manager) : manager_(manager) {}
VDONinjaWhepHost::~VDONinjaWhepHost()
{
	stop();
}

std::string VDONinjaWhepHost::brokerEndpoint(const std::string &requested)
{
	if (!requested.empty())
		return requested;
	if (const char *env = std::getenv("VDONINJA_WHEP_ENDPOINT")) {
		const std::string endpoint(env);
		if (endpoint.rfind("ws://127.0.0.1:", 0) == 0)
			return endpoint;
	}
	return "wss://whep.vdo.ninja";
}

void VDONinjaWhepHost::start(const std::string &token, const std::string &endpoint)
{
	stop();
	if (token.empty())
		return;
	stopping_ = false;
	// The existing server routes the escaped URL path literally. Match the
	// copied endpoint even when an advanced key contains spaces or slashes.
	worker_ = std::thread(&VDONinjaWhepHost::run, this, urlEncode(token), brokerEndpoint(endpoint));
}

void VDONinjaWhepHost::stop()
{
	stopping_ = true;
	if (worker_.joinable())
		worker_.join();
}

void VDONinjaWhepHost::run(std::string token, std::string endpoint)
{
	struct Inbox {
		std::mutex mutex;
		std::deque<std::string> messages;
		size_t bytes = 0;
	};
	struct Viewer {
		std::string uuid;
		std::string callback;
		std::set<std::string> iceUfrags;
		int64_t created;
	};
	while (!stopping_) {
		auto inbox = std::make_shared<Inbox>();
		const std::weak_ptr<Inbox> weakInbox = inbox;
		std::map<std::string, Viewer> viewers;
		std::shared_ptr<rtc::WebSocket> socket;
		try {
			rtc::WebSocket::Configuration config{};
			config.disableTlsVerification = false;
			// Bound memory before parsing or queueing untrusted player input.
			config.maxMessageSize = 2 * 1024 * 1024;
			const auto &caFile = VDONinjaSignaling::getCaCertificateFile();
			if (!caFile.empty())
				config.caCertificatePemFile = caFile;
			socket = std::make_shared<rtc::WebSocket>(config);
			socket->onMessage([weakInbox](rtc::message_variant message) {
				const auto queue = weakInbox.lock();
				const auto text = std::get_if<std::string>(&message);
				if (!queue || !text || text->size() > 2 * 1024 * 1024)
					return;
				std::lock_guard<std::mutex> lock(queue->mutex);
				if (queue->messages.size() < 256 && queue->bytes + text->size() <= 8 * 1024 * 1024) {
					queue->messages.push_back(*text);
					queue->bytes += text->size();
				}
			});
			socket->open(endpoint);
			const auto connecting = currentTimeMs();
			while (!stopping_ && !socket->isOpen() && !socket->isClosed() && currentTimeMs() - connecting < 10000)
				std::this_thread::sleep_for(std::chrono::milliseconds(50));
			if (socket->isOpen()) {
				socket->send(JsonBuilder().add("join", token).build());
				logInfo("WHEP link hosting connected to %s", endpoint.c_str());
			}
			while (!stopping_ && socket->isOpen()) {
				std::deque<std::string> messages;
				{
					std::lock_guard<std::mutex> lock(inbox->mutex);
					messages.swap(inbox->messages);
					inbox->bytes = 0;
				}
				for (const auto &message : messages) {
					if (stopping_)
						break;
					const JsonParser data(message);
					const auto type = data.getString("type");
					if (data.getString("streamID") != token)
						continue;
					const auto callback = data.getString("get");
					const auto reply = [&](const std::string &result) {
						if (!callback.empty() && callback.size() <= 128)
							socket->send(JsonBuilder()
							                 .addRaw("callback",
							                         JsonBuilder().add("get", callback).add("result", result).build())
							                 .build());
					};
					if (type == "delete") {
						// This server forwards a stream-wide DELETE with a fresh callback,
						// not the originating POST or a particular peer. Acknowledge it;
						// DTLS/ICE closure retires the player without disconnecting others.
						reply("");
					} else if (type == "post") {
						if (callback.empty() || callback.size() > 128 || viewers.count(callback))
							continue;
						const auto offer = data.getString("sdp");
						if (offer.size() > 1024 * 1024 || offer.rfind("v=0", 0) != 0 ||
						    offer.find('\0') != std::string::npos) {
							reply("");
							continue;
						}
						std::set<std::string> iceUfrags;
						std::istringstream lines(offer);
						std::string line;
						while (std::getline(lines, line)) {
							if (line.rfind("a=ice-ufrag:", 0) == 0)
								iceUfrags.insert(trim(line.substr(12)));
						}
						const auto uuid = "whep_" + generateUUID();
						if (manager_.acceptWhepViewer(uuid, offer)) {
							viewers.emplace(callback, Viewer{uuid, callback, iceUfrags, currentTimeMs()});
						} else {
							reply("");
						}
					} else if (type == "candidate") {
						const JsonParser candidate(data.getObject("candidate"));
						const auto ufrag = candidate.getString("usernameFragment");
						const Viewer *target = nullptr;
						for (const auto &entry : viewers) {
							if ((!ufrag.empty() && entry.second.iceUfrags.count(ufrag)) ||
							    (ufrag.empty() && viewers.size() == 1)) {
								if (target) {
									target = nullptr;
									break;
								}
								target = &entry.second;
							}
						}
						if (!target)
							continue;
						auto line = candidate.getString("candidate");
						const auto mid = candidate.getString("sdpMid");
						if (line.size() > 4096 || mid.size() > 256 || line.find('\0') != std::string::npos)
							continue;
						if (line.rfind("a=", 0) == 0)
							line.erase(0, 2);
						try {
							manager_.addWhepCandidate(target->uuid, line, mid);
						} catch (const std::exception &) {
							logWarning("Ignoring an invalid WHEP ICE candidate");
						}
					}
				}
				std::set<std::string> live;
				for (const auto &peer : manager_.getPeerSnapshots())
					live.insert(peer.uuid);
				for (auto it = viewers.begin(); it != viewers.end();) {
					auto &viewer = it->second;
					if (!viewer.callback.empty()) {
						// An unreachable STUN server must not consume the broker's whole request timeout.
						const auto answer =
						    manager_.getWhepAnswer(viewer.uuid, currentTimeMs() - viewer.created >= 7500);
						if (!answer.empty()) {
							socket->send(
							    JsonBuilder()
							        .addRaw("callback",
							                JsonBuilder().add("get", viewer.callback).add("result", answer).build())
							        .build());
							viewer.callback.clear();
						}
					}
					if (!live.count(viewer.uuid) ||
					    (!viewer.callback.empty() && currentTimeMs() - viewer.created > 8500)) {
						manager_.disconnectPeer(viewer.uuid);
						if (!viewer.callback.empty())
							socket->send(
							    JsonBuilder()
							        .addRaw("callback",
							                JsonBuilder().add("get", viewer.callback).add("result", "").build())
							        .build());
						it = viewers.erase(it);
					} else
						++it;
				}
				manager_.runDeferredCleanup();
				std::this_thread::sleep_for(std::chrono::milliseconds(25));
			}
		} catch (const std::exception &e) {
			logWarning("WHEP host connection ended: %s", e.what());
		}
		if (socket) {
			socket->onMessage(nullptr);
			socket->close();
		}
		for (const auto &entry : viewers)
			manager_.disconnectPeer(entry.second.uuid);
		for (int i = 0; i < 40 && !stopping_; ++i)
			std::this_thread::sleep_for(std::chrono::milliseconds(50));
	}
}
} // namespace vdoninja
