/*
 * OBS VDO.Ninja Plugin
 * Shared track classification helpers
 */

#pragma once

#include <string>

namespace vdoninja
{

enum class TrackType { Audio, Video, AlphaVideo };

inline TrackType classifyIncomingTrackKind(const std::string &mediaType, const std::string &trackMid,
                                           const std::string &videoMid, const std::string &alphaMid,
                                           bool matchesAlphaTrackHandle)
{
	if (mediaType == "audio") {
		return TrackType::Audio;
	}

	const bool hasDistinctAlphaMid = !alphaMid.empty() && alphaMid != videoMid;

	if (!trackMid.empty()) {
		if (trackMid == "video-alpha") {
			return TrackType::AlphaVideo;
		}
		if (hasDistinctAlphaMid && trackMid == alphaMid) {
			return TrackType::AlphaVideo;
		}
		if (!videoMid.empty() && trackMid == videoMid) {
			return TrackType::Video;
		}
	}

	if (matchesAlphaTrackHandle && (trackMid.empty() || hasDistinctAlphaMid)) {
		return TrackType::AlphaVideo;
	}

	return TrackType::Video;
}

} // namespace vdoninja
