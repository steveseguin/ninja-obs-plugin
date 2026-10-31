#pragma once

#include <obs.h>

#include <atomic>
#include <string>
#include <vector>

#include "vdoninja-publish-settings.h"

namespace vdoninja
{

struct GuestReturnSettings {
	PublishIdentity identity;
	std::string videoEncoder;
	int audioTrack = 2; // OBS mixer, one-based; the encoded output always uses slot zero.
	int videoBitrate = 2500;
	int maxViewers = 10;
	std::string customIceServers;
	bool forceTurn = false;
	int maxHeight = 0; // Zero keeps the OBS output size; otherwise only scale down.
	int frameRateDivisor = 1;
};

struct GuestReturnEncoder {
	std::string id;
	std::string name;
};

// Owned by the dock, not its visibility. All methods except the signal handler
// run on the frontend thread. Never borrows a frontend service or encoder.
class GuestReturnOutput
{
public:
	~GuestReturnOutput();
	GuestReturnOutput() = default;
	GuestReturnOutput(const GuestReturnOutput &) = delete;
	GuestReturnOutput &operator=(const GuestReturnOutput &) = delete;
	static std::vector<GuestReturnEncoder> videoEncoders();
	bool start(const GuestReturnSettings &settings);
	void stop();
	void clearError();
	bool busy() const { return output_ && !stopped_.load(); }
	obs_output_t *output() const { return output_; } // Borrowed, frontend thread only.
	const GuestReturnSettings &settings() const { return settings_; }
	std::string error() const;

private:
	static void onStopped(void *data, calldata_t *params);
	obs_output_t *output_ = nullptr;
	obs_service_t *service_ = nullptr;
	GuestReturnSettings settings_;
	std::atomic<bool> stopped_{true};
	std::atomic<int> stopCode_{0};
	bool startAttempted_ = false;
	std::string error_;
};

} // namespace vdoninja
