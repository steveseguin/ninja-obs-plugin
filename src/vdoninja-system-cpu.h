/*
 * OBS VDO.Ninja Plugin
 * System CPU usage sampling helpers
 */

#pragma once

#include <cstdint>
#include <optional>

namespace vdoninja
{

struct SystemCpuTimes {
	uint64_t idle = 0;
	uint64_t total = 0;
};

std::optional<SystemCpuTimes> readSystemCpuTimes();
std::optional<double> computeSystemCpuPercent(const SystemCpuTimes &previous, const SystemCpuTimes &current);
const char *systemCpuStatusColor(double usagePercent);

class SystemCpuSampler
{
public:
	std::optional<double> query();
	void reset();

private:
	bool hasPrevious_ = false;
	SystemCpuTimes previous_;
};

} // namespace vdoninja
