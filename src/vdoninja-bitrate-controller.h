/*
 * OBS VDO.Ninja Plugin
 * Conservative REMB-driven bitrate controller
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#pragma once

#include <chrono>
#include <cstdint>
#include <optional>

namespace vdoninja
{

struct BitrateControllerConfig {
	uint64_t minimumBitrateBitsPerSecond = 500000;
	uint64_t maximumBitrateBitsPerSecond = 4000000;
	uint16_t safetyPercent = 90;
	uint16_t decreaseThresholdPercent = 90;
	uint16_t increaseThresholdPercent = 115;
	uint16_t maximumDecreaseStepPercent = 50;
	uint16_t maximumIncreaseStepPercent = 15;
	uint16_t requiredDecreaseSamples = 2;
	uint16_t requiredIncreaseSamples = 3;
	std::chrono::milliseconds decreaseCooldown{1000};
	std::chrono::milliseconds increaseCooldown{5000};
};

// Decreases quickly, increases slowly, and never changes without a fresh
// aggregate estimate supplied by the caller.
class BitrateController
{
public:
	using Clock = std::chrono::steady_clock;

	explicit BitrateController(BitrateControllerConfig config);

	std::optional<uint64_t> observe(std::optional<uint64_t> estimatedBitrateBitsPerSecond,
	                                Clock::time_point now = Clock::now());
	uint64_t currentBitrateBitsPerSecond() const noexcept { return currentBitrateBitsPerSecond_; }
	void reset();

private:
	uint64_t scaled(uint64_t value, uint16_t percent) const;
	bool cooldownElapsed(Clock::time_point now, std::chrono::milliseconds cooldown) const;

	const BitrateControllerConfig config_;
	uint64_t currentBitrateBitsPerSecond_;
	uint16_t consecutiveDecreaseSamples_ = 0;
	uint16_t consecutiveIncreaseSamples_ = 0;
	std::optional<Clock::time_point> lastChangeAt_;
};

} // namespace vdoninja
