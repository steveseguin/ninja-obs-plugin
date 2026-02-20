/*
 * OBS VDO.Ninja Plugin
 * OBS scene/source automation for inbound VDO.Ninja streams
 */

#include "vdoninja-auto-scene-manager.h"

#include <obs-frontend-api.h>

#include <cctype>

#include "vdoninja-layout.h"
#include "vdoninja-utils.h"

namespace vdoninja
{

namespace
{

void runUiTaskThunk(void *param)
{
	auto *fn = static_cast<std::function<void()> *>(param);
	(*fn)();
	delete fn;
}

} // namespace

VDOAutoSceneManager::~VDOAutoSceneManager()
{
	stop();
}

void VDOAutoSceneManager::configure(const AutoInboundSettings &settings)
{
	std::lock_guard<std::mutex> lock(stateMutex_);
	settings_ = settings;
}

void VDOAutoSceneManager::setOwnStreamIds(const std::vector<std::string> &streamIds)
{
	std::lock_guard<std::mutex> lock(stateMutex_);
	ownStreamIds_.clear();
	for (const auto &streamId : streamIds) {
		if (!streamId.empty()) {
			ownStreamIds_.insert(streamId);
		}
	}
}

void VDOAutoSceneManager::start()
{
	std::lock_guard<std::mutex> lock(stateMutex_);
	if (!settings_.enabled) {
		return;
	}

	running_ = true;
	managedStreamIds_.clear();
}

void VDOAutoSceneManager::stop()
{
	std::set<std::string> managedStreamsSnapshot;
	bool removeOnDisconnect = false;
	{
		std::lock_guard<std::mutex> lock(stateMutex_);
		if (!running_) {
			return;
		}

		running_ = false;
		removeOnDisconnect = settings_.removeOnDisconnect;
		managedStreamsSnapshot = managedStreamIds_;
		managedStreamIds_.clear();
	}

	if (!removeOnDisconnect) {
		return;
	}

	for (const auto &streamId : managedStreamsSnapshot) {
		const std::string sourceName = sourceNameForStream(streamId);
		runOnUiThread([sourceName]() {
			obs_source_t *source = obs_get_source_by_name(sourceName.c_str());
			if (!source) {
				return;
			}
			obs_source_remove(source);
			obs_source_release(source);
		});
	}
}

void VDOAutoSceneManager::onRoomListing(const std::vector<std::string> &streamIds)
{
	{
		std::lock_guard<std::mutex> lock(stateMutex_);
		if (!running_) {
			return;
		}
	}

	for (const auto &streamId : streamIds) {
		onStreamAdded(streamId);
	}
}

void VDOAutoSceneManager::onStreamAdded(const std::string &streamId)
{
	bool running = false;
	{
		std::lock_guard<std::mutex> lock(stateMutex_);
		running = running_;
	}
	if (!running || streamId.empty() || isOwnStream(streamId)) {
		return;
	}

	{
		std::lock_guard<std::mutex> lock(stateMutex_);
		managedStreamIds_.insert(streamId);
	}

	const std::string sourceName = sourceNameForStream(streamId);
	const std::string sourceUrl = buildSourceUrl(streamId);
	bool switchScene = false;
	int sourceWidth = 1920;
	int sourceHeight = 1080;
	{
		std::lock_guard<std::mutex> lock(stateMutex_);
		switchScene = settings_.switchToSceneOnNewStream;
		sourceWidth = settings_.width;
		sourceHeight = settings_.height;
	}

	runOnUiThread([this, sourceName, sourceUrl, switchScene, sourceWidth, sourceHeight]() {
		obs_source_t *sceneSource = resolveTargetSceneSource();
		obs_scene_t *scene = sceneSource ? obs_scene_from_source(sceneSource) : nullptr;

		obs_data_t *settings = obs_data_create();
		obs_data_set_string(settings, "url", sourceUrl.c_str());
		obs_data_set_int(settings, "width", sourceWidth);
		obs_data_set_int(settings, "height", sourceHeight);
		obs_data_set_int(settings, "fps", 30);
		obs_data_set_bool(settings, "reroute_audio", true);
		obs_data_set_bool(settings, "restart_when_active", false);
		obs_data_set_bool(settings, "shutdown", false);

		obs_source_t *source = obs_get_source_by_name(sourceName.c_str());
		if (source) {
			obs_source_update(source, settings);
		} else {
			source = obs_source_create("browser_source", sourceName.c_str(), settings, nullptr);
		}

		if (source && scene) {
			obs_sceneitem_t *item = obs_scene_find_source(scene, sourceName.c_str());
			if (!item) {
				item = obs_scene_add(scene, source);
			}
			if (item) {
				obs_sceneitem_set_visible(item, true);
			}
		}

		if (switchScene && sceneSource) {
			obs_frontend_set_current_scene(sceneSource);
		}

		if (source) {
			obs_source_release(source);
		}
		obs_data_release(settings);
		if (sceneSource) {
			obs_source_release(sceneSource);
		}
	});

	AutoLayoutMode layoutMode = AutoLayoutMode::Grid;
	{
		std::lock_guard<std::mutex> lock(stateMutex_);
		layoutMode = settings_.layoutMode;
	}
	if (layoutMode == AutoLayoutMode::Grid) {
		runOnUiThread([this]() { applyLayoutForManagedSources(); });
	}
}

void VDOAutoSceneManager::onStreamRemoved(const std::string &streamId)
{
	bool removeSource = false;
	{
		std::lock_guard<std::mutex> lock(stateMutex_);
		if (streamId.empty()) {
			return;
		}

		managedStreamIds_.erase(streamId);
		removeSource = settings_.removeOnDisconnect;
	}

	const std::string sourceName = sourceNameForStream(streamId);

	runOnUiThread([this, sourceName, removeSource]() {
		obs_source_t *source = obs_get_source_by_name(sourceName.c_str());
		obs_source_t *sceneSource = resolveTargetSceneSource();
		obs_scene_t *scene = sceneSource ? obs_scene_from_source(sceneSource) : nullptr;

		if (source && removeSource) {
			obs_source_remove(source);
		} else if (scene) {
			obs_sceneitem_t *item = obs_scene_find_source(scene, sourceName.c_str());
			if (item) {
				obs_sceneitem_set_visible(item, false);
			}
		}

		if (source) {
			obs_source_release(source);
		}
		if (sceneSource) {
			obs_source_release(sceneSource);
		}
	});

	AutoLayoutMode layoutMode = AutoLayoutMode::Grid;
	{
		std::lock_guard<std::mutex> lock(stateMutex_);
		layoutMode = settings_.layoutMode;
	}
	if (layoutMode == AutoLayoutMode::Grid) {
		runOnUiThread([this]() { applyLayoutForManagedSources(); });
	}
}

void VDOAutoSceneManager::runOnUiThread(const std::function<void()> &fn) const
{
	auto *heapFn = new std::function<void()>(fn);
	obs_queue_task(OBS_TASK_UI, runUiTaskThunk, heapFn, true);
}

bool VDOAutoSceneManager::isOwnStream(const std::string &streamId) const
{
	std::lock_guard<std::mutex> lock(stateMutex_);
	return ownStreamIds_.find(streamId) != ownStreamIds_.end();
}

std::string VDOAutoSceneManager::sourceNameForStream(const std::string &streamId) const
{
	std::string prefix;
	{
		std::lock_guard<std::mutex> lock(stateMutex_);
		prefix = settings_.sourcePrefix;
	}
	if (prefix.empty()) {
		prefix = "VDO";
	}
	return prefix + "_Cam_" + sanitizeNameToken(streamId);
}

std::string VDOAutoSceneManager::buildSourceUrl(const std::string &streamId) const
{
	// Accept direct WHEP URLs when signaling metadata provides one.
	if (streamId.rfind("http://", 0) == 0 || streamId.rfind("https://", 0) == 0) {
		return streamId;
	}

	if (streamId.rfind("whep:", 0) == 0) {
		return streamId.substr(5);
	}

	std::string baseUrl;
	std::string password;
	{
		std::lock_guard<std::mutex> lock(stateMutex_);
		baseUrl = settings_.baseUrl;
		password = settings_.password;
	}
	if (baseUrl.empty()) {
		baseUrl = "https://vdo.ninja";
	}

	std::string url = baseUrl + "/?view=" + urlEncode(streamId);
	if (!password.empty()) {
		url += "&password=" + urlEncode(password);
	}
	return url;
}

std::string VDOAutoSceneManager::sanitizeNameToken(const std::string &input)
{
	std::string output;
	output.reserve(input.size());
	for (char c : input) {
		const unsigned char uc = static_cast<unsigned char>(c);
		if (std::isalnum(uc) || c == '_' || c == '-') {
			output.push_back(c);
		} else {
			output.push_back('_');
		}
	}
	return output;
}

obs_source_t *VDOAutoSceneManager::resolveTargetSceneSource() const
{
	obs_source_t *sceneSource = nullptr;
	std::string targetScene;
	{
		std::lock_guard<std::mutex> lock(stateMutex_);
		targetScene = settings_.targetScene;
	}

	if (!targetScene.empty()) {
		sceneSource = obs_get_source_by_name(targetScene.c_str());
	}

	if (!sceneSource) {
		sceneSource = obs_frontend_get_current_scene();
	}

	return sceneSource;
}

void VDOAutoSceneManager::applyLayoutForManagedSources() const
{
	obs_source_t *sceneSource = resolveTargetSceneSource();
	if (!sceneSource) {
		return;
	}

	obs_scene_t *scene = obs_scene_from_source(sceneSource);
	if (!scene) {
		obs_source_release(sceneSource);
		return;
	}

	obs_video_info ovi = {};
	const bool gotVideoInfo = obs_get_video_info(&ovi);
	int fallbackWidth = 1920;
	int fallbackHeight = 1080;
	std::set<std::string> managedStreamIds;
	{
		std::lock_guard<std::mutex> lock(stateMutex_);
		fallbackWidth = settings_.width;
		fallbackHeight = settings_.height;
		managedStreamIds = managedStreamIds_;
	}

	const uint32_t canvasWidth = gotVideoInfo ? ovi.base_width : static_cast<uint32_t>(fallbackWidth);
	const uint32_t canvasHeight = gotVideoInfo ? ovi.base_height : static_cast<uint32_t>(fallbackHeight);

	std::vector<obs_sceneitem_t *> items;
	for (const auto &streamId : managedStreamIds) {
		obs_sceneitem_t *item = obs_scene_find_source(scene, sourceNameForStream(streamId).c_str());
		if (item) {
			items.push_back(item);
		}
	}

	auto layout = buildGridLayout(items.size(), canvasWidth, canvasHeight);
	for (size_t i = 0; i < items.size() && i < layout.size(); ++i) {
		obs_sceneitem_t *item = items[i];
		obs_source_t *itemSource = obs_sceneitem_get_source(item);
		if (!itemSource) {
			continue;
		}

		uint32_t sourceWidth = obs_source_get_width(itemSource);
		uint32_t sourceHeight = obs_source_get_height(itemSource);
		if (sourceWidth == 0) {
			sourceWidth = static_cast<uint32_t>(layout[i].width);
		}
		if (sourceHeight == 0) {
			sourceHeight = static_cast<uint32_t>(layout[i].height);
		}

		struct vec2 pos = {layout[i].x, layout[i].y};
		struct vec2 scale = {layout[i].width / static_cast<float>(sourceWidth),
		                     layout[i].height / static_cast<float>(sourceHeight)};
		obs_sceneitem_set_pos(item, &pos);
		obs_sceneitem_set_scale(item, &scale);
		obs_sceneitem_set_visible(item, true);
	}

	obs_source_release(sceneSource);
}

} // namespace vdoninja
