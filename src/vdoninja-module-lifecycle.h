/*
 * OBS VDO.Ninja Plugin
 * Process-global libdatachannel lifecycle coordination
 */

#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <functional>
#include <future>
#include <mutex>
#include <string>
#include <unordered_set>
#include <utility>

namespace vdoninja
{

enum class ModuleInstanceKind : size_t { Source = 0, Output, ControlCenter, Count };

enum class ModuleLifecyclePhase {
	Unloaded,
	Loading,
	Loaded,
	Unloading,
	UnloadPending,
	Cleaning,
	CleanupTimedOut,
	CleanupFailed,
};

enum class ModuleLifecycleLogLevel { Debug, Info, Warning, Error };

struct ModuleLifecycleOperations {
	std::function<void()> attachLogger;
	std::function<void()> detachLogger;
	std::function<void()> preloadRtc;
	std::function<std::shared_future<void>()> beginRtcCleanup;
	std::function<void(ModuleLifecycleLogLevel, const std::string &)> log;
};

struct ModuleLifecycleSnapshot {
	ModuleLifecyclePhase phase = ModuleLifecyclePhase::Unloaded;
	std::array<size_t, static_cast<size_t>(ModuleInstanceKind::Count)> liveInstances{};
	size_t totalLiveInstances = 0;
	uint64_t loadGeneration = 0;
	bool loggerAttached = false;
};

class VDONinjaModuleLifecycle
{
public:
	explicit VDONinjaModuleLifecycle(ModuleLifecycleOperations operations,
	                                 std::chrono::milliseconds cleanupTimeout = std::chrono::seconds(10));

	bool load();
	void unload();
	bool tryAcquireInstance(ModuleInstanceKind kind);
	void releaseInstance(ModuleInstanceKind kind);
	ModuleLifecycleSnapshot snapshot() const;

private:
	static size_t kindIndex(ModuleInstanceKind kind);
	size_t totalLiveInstancesLocked() const;
	void emitLog(ModuleLifecycleLogLevel level, const std::string &message) const noexcept;
	void executeCleanup(const char *trigger) noexcept;
	bool completeCleanupFuture(const std::shared_future<void> &future, const char *completionContext) noexcept;
	bool resolveTimedOutCleanup() noexcept;

	ModuleLifecycleOperations operations_;
	std::chrono::milliseconds cleanupTimeout_;
	mutable std::mutex mutex_;
	ModuleLifecyclePhase phase_ = ModuleLifecyclePhase::Unloaded;
	std::array<size_t, static_cast<size_t>(ModuleInstanceKind::Count)> liveInstances_{};
	std::shared_future<void> cleanupFuture_;
	uint64_t loadGeneration_ = 0;
	bool loggerAttached_ = false;
};

// Owns the one-to-one relationship between OBS instance callbacks and module
// lifecycle permits. The original OBS destroy callback always runs while its
// permit is still live; only then may the final RTC cleanup boundary run.
class VDONinjaModuleInstanceBoundary
{
public:
	explicit VDONinjaModuleInstanceBoundary(VDONinjaModuleLifecycle &lifecycle) : lifecycle_(lifecycle) {}

	template <typename Creator, typename RollbackDestroyer>
	void *create(ModuleInstanceKind kind, Creator &&creator, RollbackDestroyer &&rollbackDestroyer) noexcept
	{
		try {
			if (!lifecycle_.tryAcquireInstance(kind)) {
				return nullptr;
			}
		} catch (...) {
			return nullptr;
		}

		void *instance = nullptr;
		try {
			instance = std::forward<Creator>(creator)();
		} catch (...) {
			releasePermit(kind);
			return nullptr;
		}
		if (!instance) {
			releasePermit(kind);
			return nullptr;
		}

		const size_t index = static_cast<size_t>(kind);
		try {
			std::lock_guard<std::mutex> lock(mutex_);
			const auto [_, inserted] = liveInstances_[index].insert(instance);
			if (inserted) {
				return instance;
			}
		} catch (...) {
			try {
				std::forward<RollbackDestroyer>(rollbackDestroyer)(instance);
			} catch (...) {
			}
			releasePermit(kind);
			return nullptr;
		}

		// A creator must never return the address of an already-live instance.
		// Do not destroy that address here: it still belongs to the first create.
		releasePermit(kind);
		return nullptr;
	}

	template <typename Destroyer> bool destroy(ModuleInstanceKind kind, void *instance, Destroyer &&destroyer) noexcept
	{
		if (!instance) {
			return false;
		}
		const size_t index = static_cast<size_t>(kind);
		if (index >= liveInstances_.size()) {
			return false;
		}
		{
			std::lock_guard<std::mutex> lock(mutex_);
			if (liveInstances_[index].erase(instance) == 0) {
				return false;
			}
		}

		try {
			std::forward<Destroyer>(destroyer)(instance);
		} catch (...) {
		}
		releasePermit(kind);
		return true;
	}

private:
	void releasePermit(ModuleInstanceKind kind) noexcept
	{
		try {
			lifecycle_.releaseInstance(kind);
		} catch (...) {
		}
	}

	VDONinjaModuleLifecycle &lifecycle_;
	std::mutex mutex_;
	std::array<std::unordered_set<void *>, static_cast<size_t>(ModuleInstanceKind::Count)> liveInstances_;
};

} // namespace vdoninja
