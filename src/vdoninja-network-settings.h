#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace vdoninja
{

struct UdpPortRange {
	uint16_t first = 1024;
	uint16_t last = 65535;
	bool automatic = true;
};

// Empty/Auto preserves the existing ICE configuration. Custom bounds are
// inclusive; a single port is represented by equal bounds. Invalid input must
// prevent publishing rather than silently allocating outside the requested range.
std::optional<UdpPortRange> parseUdpPortRange(const std::string &text);

} // namespace vdoninja
