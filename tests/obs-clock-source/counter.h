#pragma once
#include <cmath>
#include <cstdint>

inline uint16_t obsClockMarker(uint64_t elapsedNs, uint32_t numerator, uint32_t denominator)
{
	if (!numerator || !denominator)
		return 0;
	const long double frames = static_cast<long double>(elapsedNs) * numerator / (1000000000.0L * denominator);
	// Bound the floating-point value before conversion: converting a large
	// float directly to uint16_t is undefined, rather than a modulo operation.
	return static_cast<uint16_t>(std::fmod(std::floor(frames + 0.5L), 65536.0L));
}
