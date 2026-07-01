/*
 * OBS VDO.Ninja Plugin
 * System CPU usage sampling helpers
 */

#include "vdoninja-system-cpu.h"

#include <algorithm>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#include <mach/mach_host.h>
#include <mach/processor_info.h>
#elif defined(__linux__)
#include <fstream>
#include <string>
#endif

namespace vdoninja
{

std::optional<SystemCpuTimes> readSystemCpuTimes()
{
#ifdef _WIN32
	FILETIME idleTime = {};
	FILETIME kernelTime = {};
	FILETIME userTime = {};
	if (!GetSystemTimes(&idleTime, &kernelTime, &userTime)) {
		return std::nullopt;
	}

	auto toUint64 = [](const FILETIME &time) {
		ULARGE_INTEGER value;
		value.LowPart = time.dwLowDateTime;
		value.HighPart = time.dwHighDateTime;
		return value.QuadPart;
	};

	const uint64_t idle = toUint64(idleTime);
	const uint64_t kernel = toUint64(kernelTime);
	const uint64_t user = toUint64(userTime);
	return SystemCpuTimes{idle, kernel + user};
#elif defined(__APPLE__)
	natural_t cpuCount = 0;
	processor_info_array_t cpuInfo = nullptr;
	mach_msg_type_number_t cpuInfoCount = 0;
	const kern_return_t result =
	    host_processor_info(mach_host_self(), PROCESSOR_CPU_LOAD_INFO, &cpuCount, &cpuInfo, &cpuInfoCount);
	if (result != KERN_SUCCESS || !cpuInfo) {
		return std::nullopt;
	}

	SystemCpuTimes times;
	const auto load = reinterpret_cast<processor_cpu_load_info_t>(cpuInfo);
	for (natural_t i = 0; i < cpuCount; ++i) {
		const uint64_t user = load[i].cpu_ticks[CPU_STATE_USER];
		const uint64_t system = load[i].cpu_ticks[CPU_STATE_SYSTEM];
		const uint64_t idle = load[i].cpu_ticks[CPU_STATE_IDLE];
		const uint64_t nice = load[i].cpu_ticks[CPU_STATE_NICE];
		times.idle += idle;
		times.total += user + system + idle + nice;
	}

	vm_deallocate(mach_task_self(), reinterpret_cast<vm_address_t>(cpuInfo),
	              static_cast<vm_size_t>(cpuInfoCount * sizeof(integer_t)));
	return times;
#elif defined(__linux__)
	std::ifstream stat("/proc/stat");
	std::string label;
	uint64_t user = 0;
	uint64_t nice = 0;
	uint64_t system = 0;
	uint64_t idle = 0;
	uint64_t iowait = 0;
	uint64_t irq = 0;
	uint64_t softirq = 0;
	uint64_t steal = 0;
	if (!(stat >> label >> user >> nice >> system >> idle >> iowait >> irq >> softirq >> steal) || label != "cpu") {
		return std::nullopt;
	}

	const uint64_t totalIdle = idle + iowait;
	const uint64_t totalBusy = user + nice + system + irq + softirq + steal;
	return SystemCpuTimes{totalIdle, totalIdle + totalBusy};
#else
	return std::nullopt;
#endif
}

std::optional<double> computeSystemCpuPercent(const SystemCpuTimes &previous, const SystemCpuTimes &current)
{
	if (current.total < previous.total || current.idle < previous.idle) {
		return std::nullopt;
	}

	const uint64_t totalDelta = current.total - previous.total;
	const uint64_t idleDelta = current.idle - previous.idle;
	if (totalDelta == 0 || idleDelta > totalDelta) {
		return std::nullopt;
	}

	const double busyDelta = static_cast<double>(totalDelta - idleDelta);
	return std::clamp((busyDelta * 100.0) / static_cast<double>(totalDelta), 0.0, 100.0);
}

const char *systemCpuStatusColor(double usagePercent)
{
	if (usagePercent >= 90.0) {
		return "#ff3333";
	}
	if (usagePercent >= 85.0) {
		return "#ff9900";
	}
	if (usagePercent >= 70.0) {
		return "#d6b600";
	}
	return "#cccccc";
}

std::optional<double> SystemCpuSampler::query()
{
	std::optional<SystemCpuTimes> current = readSystemCpuTimes();
	if (!current) {
		return std::nullopt;
	}

	if (!hasPrevious_) {
		previous_ = *current;
		hasPrevious_ = true;
		return std::nullopt;
	}

	std::optional<double> usage = computeSystemCpuPercent(previous_, *current);
	previous_ = *current;
	return usage;
}

void SystemCpuSampler::reset()
{
	hasPrevious_ = false;
	previous_ = {};
}

} // namespace vdoninja
