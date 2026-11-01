/*
 * OBS VDO.Ninja Plugin
 * WHEP viewer hosting through the whep.vdo.ninja broker
 *
 * Accepts WHEP offers relayed by the broker, answers them with per-viewer
 * publisher connections, and retires viewers when their media connection closes.
 */

#pragma once

#include <atomic>
#include <string>
#include <thread>

namespace vdoninja
{
class VDONinjaPeerManager;

// The service thread owns broker messages and viewer admission. RTC callbacks
// only enqueue messages; they never retain or call the output/peer manager.
class VDONinjaWhepHost
{
public:
	explicit VDONinjaWhepHost(VDONinjaPeerManager &manager);
	~VDONinjaWhepHost();
	// An empty endpoint resolves through brokerEndpoint().
	void start(const std::string &token, const std::string &endpoint = "");
	void stop();
	// The public broker, unless VDONINJA_WHEP_ENDPOINT names a loopback ws:// copy for validation runs.
	static std::string brokerEndpoint(const std::string &requested = "");

private:
	void run(std::string token, std::string endpoint);
	VDONinjaPeerManager &manager_;
	std::atomic<bool> stopping_{false};
	std::thread worker_;
};
} // namespace vdoninja
