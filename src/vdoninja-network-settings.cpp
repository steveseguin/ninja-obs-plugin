#include "vdoninja-network-settings.h"

#include <algorithm>
#include <cctype>
#include <charconv>

#include "vdoninja-utils.h"

namespace vdoninja
{

std::optional<UdpPortRange> parseUdpPortRange(const std::string &text)
{
	std::string value = trim(text);
	std::transform(value.begin(), value.end(), value.begin(),
	               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	if (value.empty() || value == "auto")
		return UdpPortRange{};

	auto parsePort = [](const std::string &part) -> std::optional<uint16_t> {
		const auto number = trim(part);
		unsigned int port = 0;
		const auto result = std::from_chars(number.data(), number.data() + number.size(), port);
		if (result.ec != std::errc{} || result.ptr != number.data() + number.size() || port == 0 || port > 65535)
			return std::nullopt;
		return static_cast<uint16_t>(port);
	};

	const auto separator = value.find('-');
	const auto first = parsePort(value.substr(0, separator));
	const auto last = separator == std::string::npos ? first : parsePort(value.substr(separator + 1));
	if (!first || !last || *first > *last)
		return std::nullopt;
	return UdpPortRange{*first, *last, false};
}

} // namespace vdoninja
