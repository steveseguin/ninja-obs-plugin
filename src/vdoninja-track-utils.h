/*
 * OBS VDO.Ninja Plugin
 * Shared track classification helpers
 */

#pragma once

#include <cstddef>
#include <string>

namespace vdoninja
{

enum class TrackType { Audio, Video, AlphaVideo };

inline bool isExistingPrimaryVideoSection(size_t offeredVideoIndex, const std::string &offeredMid,
                                          const std::string &primaryMid, bool hasPrimaryTrack)
{
	if (!hasPrimaryTrack) {
		return false;
	}
	if (!primaryMid.empty() && offeredMid == primaryMid) {
		return true;
	}
	// Renegotiation offers normally repeat every existing m-line. When a mid is
	// unavailable, the first non-alpha video section is still the primary track.
	return offeredVideoIndex == 0 && offeredMid != "video-alpha";
}

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
