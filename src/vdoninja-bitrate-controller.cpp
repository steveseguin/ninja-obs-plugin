/*
 * OBS VDO.Ninja Plugin
 * Conservative REMB-driven bitrate controller
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#include "vdoninja-bitrate-controller.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace vdoninja
{

BitrateController::BitrateController(BitrateControllerConfig config)
    : config_(config), currentBitrateBitsPerSecond_(config.maximumBitrateBitsPerSecond)
{
	if (config_.minimumBitrateBitsPerSecond == 0 ||
	    config_.maximumBitrateBitsPerSecond < config_.minimumBitrateBitsPerSecond || config_.safetyPercent == 0 ||
	    config_.decreaseThresholdPercent == 0 || config_.increaseThresholdPercent <= 100 ||
	    config_.maximumDecreaseStepPercent == 0 || config_.maximumDecreaseStepPercent > 100 ||
	    config_.maximumIncreaseStepPercent == 0 || config_.requiredDecreaseSamples == 0 ||
	    config_.requiredIncreaseSamples == 0 || config_.decreaseCooldown.count() < 0 ||
	    config_.increaseCooldown.count() < 0) {
		throw std::invalid_argument("Bitrate controller settings are invalid");
	}
}

std::optional<uint64_t> BitrateController::observe(std::optional<uint64_t> estimatedBitrateBitsPerSecond,
                                                   Clock::time_point now)
{
	if (!estimatedBitrateBitsPerSecond || *estimatedBitrateBitsPerSecond == 0) {
		consecutiveDecreaseSamples_ = 0;
		consecutiveIncreaseSamples_ = 0;
		return std::nullopt;
	}

	const uint64_t desired = std::clamp(scaled(*estimatedBitrateBitsPerSecond, config_.safetyPercent),
	                                    config_.minimumBitrateBitsPerSecond, config_.maximumBitrateBitsPerSecond);
	const uint64_t decreaseThreshold = scaled(currentBitrateBitsPerSecond_, config_.decreaseThresholdPercent);
	const uint64_t boundedEstimate = std::min(*estimatedBitrateBitsPerSecond, config_.maximumBitrateBitsPerSecond);
	if (boundedEstimate < decreaseThreshold) {
		consecutiveIncreaseSamples_ = 0;
		if (consecutiveDecreaseSamples_ < std::numeric_limits<uint16_t>::max()) {
			++consecutiveDecreaseSamples_;
		}
		if (consecutiveDecreaseSamples_ < config_.requiredDecreaseSamples ||
		    !cooldownElapsed(now, config_.decreaseCooldown)) {
			return std::nullopt;
		}
		consecutiveDecreaseSamples_ = 0;
		const uint64_t minimumStepTarget =
		    scaled(currentBitrateBitsPerSecond_, static_cast<uint16_t>(100U - config_.maximumDecreaseStepPercent));
		const uint64_t target = std::max(desired, minimumStepTarget);
		if (target == currentBitrateBitsPerSecond_) {
			return std::nullopt;
		}
		currentBitrateBitsPerSecond_ = target;
		lastChangeAt_ = now;
		return currentBitrateBitsPerSecond_;
	}
	consecutiveDecreaseSamples_ = 0;

	const uint64_t increaseThreshold = scaled(currentBitrateBitsPerSecond_, config_.increaseThresholdPercent);
	if (desired <= increaseThreshold || currentBitrateBitsPerSecond_ >= config_.maximumBitrateBitsPerSecond) {
		consecutiveIncreaseSamples_ = 0;
		return std::nullopt;
	}

	if (consecutiveIncreaseSamples_ < std::numeric_limits<uint16_t>::max()) {
		++consecutiveIncreaseSamples_;
	}
	if (consecutiveIncreaseSamples_ < config_.requiredIncreaseSamples ||
	    !cooldownElapsed(now, config_.increaseCooldown)) {
		return std::nullopt;
	}

	const uint64_t maximumStep =
	    scaled(currentBitrateBitsPerSecond_, static_cast<uint16_t>(100U + config_.maximumIncreaseStepPercent));
	const uint64_t target = std::min({desired, maximumStep, config_.maximumBitrateBitsPerSecond});
	consecutiveIncreaseSamples_ = 0;
	if (target <= currentBitrateBitsPerSecond_) {
		return std::nullopt;
	}
	currentBitrateBitsPerSecond_ = target;
	lastChangeAt_ = now;
	return currentBitrateBitsPerSecond_;
}

void BitrateController::reset()
{
	currentBitrateBitsPerSecond_ = config_.maximumBitrateBitsPerSecond;
	consecutiveDecreaseSamples_ = 0;
	consecutiveIncreaseSamples_ = 0;
	lastChangeAt_.reset();
}

uint64_t BitrateController::scaled(uint64_t value, uint16_t percent) const
{
	const long double result = static_cast<long double>(value) * static_cast<long double>(percent) / 100.0L;
	if (result >= static_cast<long double>(std::numeric_limits<uint64_t>::max())) {
		return std::numeric_limits<uint64_t>::max();
	}
	return static_cast<uint64_t>(std::floor(result));
}

bool BitrateController::cooldownElapsed(Clock::time_point now, std::chrono::milliseconds cooldown) const
{
	return !lastChangeAt_ || now < *lastChangeAt_ || now - *lastChangeAt_ >= cooldown;
}

} // namespace vdoninja
