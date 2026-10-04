#pragma once

#include <cstdint>
#include <string>

namespace vdoninja
{

struct PublishIdentity {
	std::string streamId;
	std::string password;
	std::string roomId;
	std::string salt;
	std::string wssHost;
};

// Explicit fields win over the compatibility Stream Key. Defaults and generated
// IDs must only be filled after this parser has had a chance to read the key.
void parsePublishStreamKey(const std::string &key, PublishIdentity &identity, bool allowBareStreamId = true);
std::string buildPublishUrl(const PublishIdentity &identity, bool push = false);

bool isQualityRateControl(const std::string &rateControl);
int publishPacingBitrate(const std::string &rateControl, int64_t bitrateKbps, int64_t maxBitrateKbps,
                         int fallbackBitsPerSecond);
int64_t publishKeyframeInterval(int64_t seconds);

} // namespace vdoninja
