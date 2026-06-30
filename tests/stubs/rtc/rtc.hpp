/*
 * Minimal libdatachannel stubs for signaling unit tests.
 */

#pragma once

#include <chrono>
#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace rtc
{

class DataChannel
{
public:
	bool isOpen() const { return open_; }
	void send(std::string message) { lastMessage_ = std::move(message); }

	std::string lastMessage() const { return lastMessage_; }

private:
	bool open_ = true;
	std::string lastMessage_;
};

class WebSocket
{
public:
	using Binary = std::vector<std::byte>;
	using Message = std::variant<std::string, Binary>;

	struct Configuration {
		bool disableTlsVerification = false;
		std::optional<std::string> proxyServer;
		std::vector<std::string> protocols;
		std::chrono::milliseconds connectionTimeout{0};
		std::optional<std::chrono::milliseconds> pingInterval;
		std::optional<int> maxOutstandingPings;
		std::optional<std::string> caCertificatePemFile;
		std::optional<std::string> certificatePemFile;
		std::optional<std::string> keyPemFile;
		std::optional<std::string> keyPemPass;
		std::optional<size_t> maxMessageSize;
	};

	explicit WebSocket(const Configuration &) {}

	void onOpen(std::function<void()> callback) { onOpen_ = std::move(callback); }
	void onClosed(std::function<void()> callback) { onClosed_ = std::move(callback); }
	void onError(std::function<void(const std::string &)> callback) { onError_ = std::move(callback); }

	template <typename Callback> void onMessage(Callback &&callback)
	{
		onMessage_ = [fn = std::forward<Callback>(callback)](Message message) mutable { fn(std::move(message)); };
	}

	void open(const std::string &)
	{
		open_ = true;
		if (onOpen_) {
			onOpen_();
		}
	}

	bool isOpen() const { return open_; }
	void send(std::string message) { lastMessage_ = std::move(message); }

	void close()
	{
		const bool wasOpen = open_;
		open_ = false;
		if (wasOpen && onClosed_) {
			onClosed_();
		}
	}

private:
	bool open_ = false;
	std::function<void()> onOpen_;
	std::function<void()> onClosed_;
	std::function<void(const std::string &)> onError_;
	std::function<void(Message)> onMessage_;
	std::string lastMessage_;
};

} // namespace rtc
