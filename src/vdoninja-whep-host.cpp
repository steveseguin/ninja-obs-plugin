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
	worker_ = std::thread(&VDONinjaWhepHost::run, this, token, brokerEndpoint(endpoint));
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
	};
	struct Viewer {
		std::string uuid;
		std::string callback;
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
			const auto &caFile = VDONinjaSignaling::getCaCertificateFile();
			if (!caFile.empty())
				config.caCertificatePemFile = caFile;
			socket = std::make_shared<rtc::WebSocket>(config);
			socket->onMessage([weakInbox](rtc::message_variant message) {
				const auto queue = weakInbox.lock();
				const auto text = std::get_if<std::string>(&message);
				if (!queue || !text)
					return;
				std::lock_guard<std::mutex> lock(queue->mutex);
				if (queue->messages.size() < 256)
					queue->messages.push_back(*text);
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
				}
				for (const auto &message : messages) {
					const JsonParser data(message);
					const auto id = data.getString("sessionId");
					const auto type = data.getString("type");
					if (id.empty() || id.size() > 128)
						continue;
					auto existing = viewers.find(id);
					if (type == "delete") {
						if (existing != viewers.end()) {
							manager_.disconnectPeer(existing->second.uuid);
							viewers.erase(existing);
						}
					} else if (type == "post" && existing == viewers.end()) {
						const auto callback = data.getString("get");
						const auto uuid = "whep_" + generateUUID();
						if (manager_.acceptWhepViewer(uuid, data.getString("sdp"))) {
							viewers.emplace(id, Viewer{uuid, callback, currentTimeMs()});
						} else {
							socket->send(
							    JsonBuilder()
							        .addRaw("callback", JsonBuilder().add("get", callback).add("result", "").build())
							        .build());
						}
					} else if (type == "candidate" && existing != viewers.end()) {
						const JsonParser candidate(data.getObject("candidate"));
						auto line = candidate.getString("candidate");
						if (line.rfind("a=", 0) == 0)
							line.erase(0, 2);
						try {
							manager_.addWhepCandidate(existing->second.uuid, line, candidate.getString("sdpMid"));
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
						socket->send(JsonBuilder().add("type", "delete").add("sessionId", it->first).build());
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
