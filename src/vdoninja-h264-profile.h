/*
 * OBS VDO.Ninja Plugin
 * H.264 profile-level-id extraction and safe fallback selection
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace vdoninja
{

// Extracts profile_idc, profile_iop, and level_idc from an SPS carried as
// Annex-B, 4-byte length-prefixed AVCC, a decoder configuration record, or a
// single SPS NAL unit.
std::optional<std::string> deriveH264ProfileLevelId(const uint8_t *data, size_t size);

// Used only until real SPS bytes are available. The fallback advertises
// constrained baseline with the first H.264 level whose frame-size and
// macroblock-rate limits cover the configured stream.
std::string fallbackH264ProfileLevelId(uint32_t width, uint32_t height, uint32_t fpsNumerator,
                                       uint32_t fpsDenominator = 1);

bool isValidH264ProfileLevelId(const std::string &profileLevelId);
std::string h264FmtpForProfileLevelId(const std::string &profileLevelId);

} // namespace vdoninja
