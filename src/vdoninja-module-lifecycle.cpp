/*
 * OBS VDO.Ninja Plugin
 * Process-global libdatachannel lifecycle coordination
 */

#include "vdoninja-module-lifecycle.h"

#include <exception>
#include <stdexcept>
#include <utility>

namespace vdoninja
{

VDONinjaModuleLifecycle::VDONinjaModuleLifecycle(ModuleLifecycleOperations operations,
                                                 std::chrono::milliseconds cleanupTimeout)
    : operations_(std::move(operations)), cleanupTimeout_(cleanupTimeout)
{
	if (!operations_.attachLogger || !operations_.detachLogger || !operations_.preloadRtc ||
	    !operations_.beginRtcCleanup) {
		throw std::invalid_argument("module lifecycle operations are incomplete");
	}
	if (cleanupTimeout_ < std::chrono::milliseconds::zero()) {
		throw std::invalid_argument("module lifecycle cleanup timeout cannot be negative");
	}
}

bool VDONinjaModuleLifecycle::load()
{
	bool resolveTimedOut = false;
	bool rejected = false;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (phase_ == ModuleLifecyclePhase::Loaded) {
			return true;
		}
		if (phase_ == ModuleLifecyclePhase::CleanupTimedOut) {
			phase_ = ModuleLifecyclePhase::Cleaning;
			resolveTimedOut = true;
		} else if (phase_ == ModuleLifecyclePhase::Unloaded && totalLiveInstancesLocked() == 0) {
			phase_ = ModuleLifecyclePhase::Loading;
		} else {
			rejected = true;
		}
	}
	if (rejected) {
		emitLog(ModuleLifecycleLogLevel::Error, "Cannot load while the previous module lifecycle is unfinished");
		return false;
	}

	if (resolveTimedOut) {
		if (!resolveTimedOutCleanup()) {
			return false;
		}
		return load();
	}

	bool attachAttempted = false;
	bool preloadAttempted = false;
	std::string failure;
	try {
		attachAttempted = true;
		operations_.attachLogger();
		{
			std::lock_guard<std::mutex> lock(mutex_);
			loggerAttached_ = true;
		}
		preloadAttempted = true;
		operations_.preloadRtc();
	} catch (const std::exception &error) {
		failure = error.what();
	} catch (...) {
		failure = "unknown exception";
	}

	if (!failure.empty()) {
		// An attach operation may throw after partially publishing its callback.
		// Always attempt a synchronous detach once attach was entered.
		if (attachAttempted) {
			try {
				operations_.detachLogger();
			} catch (...) {
				emitLog(ModuleLifecycleLogLevel::Error,
				        "Logger detach also failed while rolling back module RTC initialization");
			}
		}
		bool cleanupRequired = false;
		{
			std::lock_guard<std::mutex> lock(mutex_);
			loggerAttached_ = false;
			cleanupRequired = preloadAttempted;
			phase_ = cleanupRequired ? ModuleLifecyclePhase::Cleaning : ModuleLifecyclePhase::Unloaded;
		}
		emitLog(ModuleLifecycleLogLevel::Error, "Failed to initialize module RTC lifecycle: " + failure);
		if (cleanupRequired) {
			executeCleanup("load rollback");
		}
		return false;
	}

	uint64_t generation = 0;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		phase_ = ModuleLifecyclePhase::Loaded;
		generation = ++loadGeneration_;
	}
	emitLog(ModuleLifecycleLogLevel::Info,
	        "Module RTC lifecycle loaded (generation " + std::to_string(generation) + ")");
	return true;
}

void VDONinjaModuleLifecycle::unload()
{
	bool ignored = false;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (phase_ != ModuleLifecyclePhase::Loaded) {
			ignored = true;
		} else {
			phase_ = ModuleLifecyclePhase::Unloading;
		}
	}
	if (ignored) {
		emitLog(ModuleLifecycleLogLevel::Debug, "Ignoring duplicate or inactive module unload request");
		return;
	}

	try {
		operations_.detachLogger();
	} catch (const std::exception &error) {
		emitLog(ModuleLifecycleLogLevel::Error, std::string("Failed to detach libdatachannel logger: ") + error.what());
	} catch (...) {
		emitLog(ModuleLifecycleLogLevel::Error, "Failed to detach libdatachannel logger: unknown exception");
	}

	size_t liveInstances = 0;
	bool startCleanup = false;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		loggerAttached_ = false;
		liveInstances = totalLiveInstancesLocked();
		if (liveInstances == 0) {
			phase_ = ModuleLifecyclePhase::Cleaning;
			startCleanup = true;
		} else {
			phase_ = ModuleLifecyclePhase::UnloadPending;
		}
	}

	if (liveInstances != 0) {
		emitLog(ModuleLifecycleLogLevel::Info,
		        "Deferring RTC cleanup until " + std::to_string(liveInstances) + " plugin instance(s) are destroyed");
	}
	if (startCleanup) {
		executeCleanup("module unload");
	}
}

bool VDONinjaModuleLifecycle::tryAcquireInstance(ModuleInstanceKind kind)
{
	const size_t index = kindIndex(kind);
	std::lock_guard<std::mutex> lock(mutex_);
	if (phase_ != ModuleLifecyclePhase::Loaded) {
		return false;
	}
	++liveInstances_[index];
	return true;
}

void VDONinjaModuleLifecycle::releaseInstance(ModuleInstanceKind kind)
{
	const size_t index = kindIndex(kind);
	bool underflow = false;
	bool startCleanup = false;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		auto &count = liveInstances_[index];
		if (count == 0) {
			underflow = true;
		} else {
			--count;
			// If unload is still detaching the logger, its caller owns the
			// transition after detach returns. Cleanup may only be reserved
			// here once the phase proves detach has completed.
			if (phase_ == ModuleLifecyclePhase::UnloadPending && totalLiveInstancesLocked() == 0) {
				phase_ = ModuleLifecyclePhase::Cleaning;
				startCleanup = true;
			}
		}
	}

	if (underflow) {
		emitLog(ModuleLifecycleLogLevel::Error, "Module instance lifecycle underflow");
		return;
	}
	if (startCleanup) {
		executeCleanup("last plugin instance destruction");
	}
}

ModuleLifecycleSnapshot VDONinjaModuleLifecycle::snapshot() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	ModuleLifecycleSnapshot result;
	result.phase = phase_;
	result.liveInstances = liveInstances_;
	result.totalLiveInstances = totalLiveInstancesLocked();
	result.loadGeneration = loadGeneration_;
	result.loggerAttached = loggerAttached_;
	return result;
}

size_t VDONinjaModuleLifecycle::kindIndex(ModuleInstanceKind kind)
{
	const size_t index = static_cast<size_t>(kind);
	if (index >= static_cast<size_t>(ModuleInstanceKind::Count)) {
		throw std::out_of_range("invalid module instance kind");
	}
	return index;
}

size_t VDONinjaModuleLifecycle::totalLiveInstancesLocked() const
{
	size_t total = 0;
	for (const size_t count : liveInstances_) {
		total += count;
	}
	return total;
}

void VDONinjaModuleLifecycle::emitLog(ModuleLifecycleLogLevel level, const std::string &message) const noexcept
{
	if (!operations_.log) {
		return;
	}
	try {
		operations_.log(level, message);
	} catch (...) {
		// Lifecycle diagnostics must never escape an OBS C ABI callback.
	}
}

void VDONinjaModuleLifecycle::executeCleanup(const char *trigger) noexcept
{
	emitLog(ModuleLifecycleLogLevel::Info, std::string("Starting process-global libdatachannel cleanup at ") + trigger);
	std::shared_future<void> future;
	try {
		future = operations_.beginRtcCleanup();
	} catch (const std::exception &error) {
		{
			std::lock_guard<std::mutex> lock(mutex_);
			phase_ = ModuleLifecyclePhase::CleanupFailed;
		}
		emitLog(ModuleLifecycleLogLevel::Error,
		        std::string("Failed to start process-global libdatachannel cleanup: ") + error.what());
		return;
	} catch (...) {
		{
			std::lock_guard<std::mutex> lock(mutex_);
			phase_ = ModuleLifecyclePhase::CleanupFailed;
		}
		emitLog(ModuleLifecycleLogLevel::Error,
		        "Failed to start process-global libdatachannel cleanup: unknown exception");
		return;
	}

	if (!future.valid()) {
		{
			std::lock_guard<std::mutex> lock(mutex_);
			phase_ = ModuleLifecyclePhase::CleanupFailed;
		}
		emitLog(ModuleLifecycleLogLevel::Error, "libdatachannel cleanup returned an invalid completion future");
		return;
	}
	{
		std::lock_guard<std::mutex> lock(mutex_);
		cleanupFuture_ = future;
	}

	std::future_status status;
	try {
		status = future.wait_for(cleanupTimeout_);
	} catch (const std::exception &error) {
		{
			std::lock_guard<std::mutex> lock(mutex_);
			phase_ = ModuleLifecyclePhase::CleanupFailed;
		}
		emitLog(ModuleLifecycleLogLevel::Error,
		        std::string("Failed while waiting for process-global libdatachannel cleanup: ") + error.what());
		return;
	} catch (...) {
		{
			std::lock_guard<std::mutex> lock(mutex_);
			phase_ = ModuleLifecyclePhase::CleanupFailed;
		}
		emitLog(ModuleLifecycleLogLevel::Error,
		        "Failed while waiting for process-global libdatachannel cleanup: unknown exception");
		return;
	}

	if (status == std::future_status::timeout) {
		{
			std::lock_guard<std::mutex> lock(mutex_);
			phase_ = ModuleLifecyclePhase::CleanupTimedOut;
		}
		emitLog(ModuleLifecycleLogLevel::Error, "Timed out waiting for process-global libdatachannel cleanup after " +
		                                            std::to_string(cleanupTimeout_.count()) +
		                                            " ms; reload is blocked until cleanup completes");
		return;
	}
	if (status == std::future_status::deferred) {
		{
			std::lock_guard<std::mutex> lock(mutex_);
			phase_ = ModuleLifecyclePhase::CleanupFailed;
		}
		emitLog(ModuleLifecycleLogLevel::Error,
		        "libdatachannel cleanup returned a deferred future; refusing an unbounded wait");
		return;
	}
	(void)completeCleanupFuture(future, "within the unload wait budget");
}

bool VDONinjaModuleLifecycle::completeCleanupFuture(const std::shared_future<void> &future,
                                                    const char *completionContext) noexcept
{
	std::string failure;
	try {
		future.get();
	} catch (const std::exception &error) {
		failure = error.what();
	} catch (...) {
		failure = "unknown exception";
	}

	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (failure.empty()) {
			cleanupFuture_ = {};
			phase_ = ModuleLifecyclePhase::Unloaded;
		} else {
			phase_ = ModuleLifecyclePhase::CleanupFailed;
		}
	}
	if (!failure.empty()) {
		emitLog(ModuleLifecycleLogLevel::Error, "Process-global libdatachannel cleanup failed: " + failure);
		return false;
	}
	emitLog(ModuleLifecycleLogLevel::Info,
	        std::string("Process-global libdatachannel cleanup completed ") + completionContext);
	return true;
}

bool VDONinjaModuleLifecycle::resolveTimedOutCleanup() noexcept
{
	std::shared_future<void> future;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		future = cleanupFuture_;
	}
	if (!future.valid()) {
		{
			std::lock_guard<std::mutex> lock(mutex_);
			phase_ = ModuleLifecyclePhase::CleanupFailed;
		}
		emitLog(ModuleLifecycleLogLevel::Error, "Timed-out cleanup lost its completion future; reload is unsafe");
		return false;
	}

	std::future_status status;
	try {
		status = future.wait_for(std::chrono::milliseconds::zero());
	} catch (...) {
		{
			std::lock_guard<std::mutex> lock(mutex_);
			phase_ = ModuleLifecyclePhase::CleanupFailed;
		}
		emitLog(ModuleLifecycleLogLevel::Error, "Failed to poll the timed-out cleanup future; reload is unsafe");
		return false;
	}
	if (status != std::future_status::ready) {
		{
			std::lock_guard<std::mutex> lock(mutex_);
			phase_ = ModuleLifecyclePhase::CleanupTimedOut;
		}
		emitLog(ModuleLifecycleLogLevel::Warning,
		        "Cannot reload while the previous process-global libdatachannel cleanup is still running");
		return false;
	}
	return completeCleanupFuture(future, "after the original timeout");
}

} // namespace vdoninja
