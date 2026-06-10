/*
 * OBS VDO.Ninja Plugin
 * OBS scene/source automation for inbound VDO.Ninja streams
 */

#pragma once

#include <obs-module.h>

#include <functional>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "vdoninja-common.h"

namespace vdoninja
{

class VDOAutoSceneManager
{
public:
	VDOAutoSceneManager() = default;
	~VDOAutoSceneManager();

	void configure(const AutoInboundSettings &settings);
	void setOwnStreamIds(const std::vector<std::string> &streamIds);
	void start();
	void stop();

	void onRoomListing(const std::vector<std::string> &streamIds);
	void onStreamAdded(const std::string &streamId);
	void onStreamRemoved(const std::string &streamId);

private:
	void runOnUiThread(const std::function<void()> &fn) const;
	bool isOwnStream(const std::string &streamId) const;
	std::string sourceNameForStream(const std::string &streamId) const;
	std::string buildSourceUrl(const std::string &streamId) const;
	void queueLayoutRefresh() const;
	static std::string makeSourceName(std::string prefix, const std::string &streamId);
	static std::string sanitizeNameToken(const std::string &input);

	AutoInboundSettings settings_;
	bool running_ = false;
	mutable std::mutex stateMutex_;
	std::set<std::string> ownStreamIds_;
	std::set<std::string> managedStreamIds_;
};

} // namespace vdoninja
