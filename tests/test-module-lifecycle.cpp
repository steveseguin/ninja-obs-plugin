/*
 * Unit tests for process-global libdatachannel module lifecycle ordering
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "vdoninja-module-lifecycle.h"

using namespace vdoninja;

namespace
{

std::shared_future<void> readyFuture()
{
	std::promise<void> promise;
	promise.set_value();
	return promise.get_future().share();
}

struct ModuleLifecycleHarness {
	std::vector<std::string> events;
	std::vector<std::pair<ModuleLifecycleLogLevel, std::string>> logs;
	std::vector<std::shared_future<void>> cleanupFutures;
	bool loggerActive = false;
	int pluginLoggerCallbacks = 0;
	int attachCalls = 0;
	int detachCalls = 0;
	int preloadCalls = 0;
	int cleanupCalls = 0;

	ModuleLifecycleOperations operations()
	{
		return {
		    [this]() {
			    events.emplace_back("attach-logger");
			    loggerActive = true;
			    ++attachCalls;
		    },
		    [this]() {
			    events.emplace_back("detach-logger");
			    loggerActive = false;
			    ++detachCalls;
		    },
		    [this]() {
			    events.emplace_back("preload-rtc");
			    ++preloadCalls;
		    },
		    [this]() {
			    events.emplace_back("cleanup-rtc");
			    const size_t index = static_cast<size_t>(cleanupCalls++);
			    return index < cleanupFutures.size() ? cleanupFutures[index] : readyFuture();
		    },
		    [this](ModuleLifecycleLogLevel level, const std::string &message) { logs.emplace_back(level, message); },
		};
	}

	void emitRtcLog()
	{
		if (loggerActive) {
			++pluginLoggerCallbacks;
		}
	}
};

TEST(ModuleLifecycleTest, EmptyPreloadExceptionStillRollsBackInitialization)
{
	ModuleLifecycleHarness harness;
	auto operations = harness.operations();
	operations.preloadRtc = []() { throw std::runtime_error(""); };
	VDONinjaModuleLifecycle lifecycle(operations, std::chrono::milliseconds(10));

	EXPECT_FALSE(lifecycle.load());
	EXPECT_EQ(lifecycle.snapshot().phase, ModuleLifecyclePhase::Unloaded);
	EXPECT_FALSE(harness.loggerActive);
	EXPECT_EQ(harness.detachCalls, 1);
	EXPECT_EQ(harness.cleanupCalls, 1);
	EXPECT_FALSE(lifecycle.tryAcquireInstance(ModuleInstanceKind::Source));
}

TEST(ModuleLifecycleTest, EmptyCleanupExceptionStillBlocksReload)
{
	ModuleLifecycleHarness harness;
	std::promise<void> cleanupPromise;
	cleanupPromise.set_exception(std::make_exception_ptr(std::runtime_error("")));
	harness.cleanupFutures.push_back(cleanupPromise.get_future().share());
	VDONinjaModuleLifecycle lifecycle(harness.operations(), std::chrono::milliseconds(10));
	ASSERT_TRUE(lifecycle.load());

	lifecycle.unload();
	EXPECT_EQ(lifecycle.snapshot().phase, ModuleLifecyclePhase::CleanupFailed);
	EXPECT_FALSE(lifecycle.load());
	EXPECT_EQ(harness.preloadCalls, 1);
}

TEST(ModuleLifecycleTest, UnloadDetachesLoggerAndDefersCleanupUntilEveryPluginInstanceIsDestroyed)
{
	ModuleLifecycleHarness harness;
	VDONinjaModuleLifecycle lifecycle(harness.operations(), std::chrono::milliseconds(10));
	ASSERT_TRUE(lifecycle.load());
	ASSERT_TRUE(lifecycle.tryAcquireInstance(ModuleInstanceKind::Source));
	ASSERT_TRUE(lifecycle.tryAcquireInstance(ModuleInstanceKind::Output));
	ASSERT_TRUE(lifecycle.tryAcquireInstance(ModuleInstanceKind::ControlCenter));

	lifecycle.unload();
	harness.emitRtcLog();
	EXPECT_EQ(harness.detachCalls, 1);
	EXPECT_EQ(harness.pluginLoggerCallbacks, 0);
	EXPECT_EQ(harness.cleanupCalls, 0);
	EXPECT_EQ(lifecycle.snapshot().phase, ModuleLifecyclePhase::UnloadPending);

	lifecycle.releaseInstance(ModuleInstanceKind::ControlCenter);
	lifecycle.releaseInstance(ModuleInstanceKind::Source);
	EXPECT_EQ(harness.cleanupCalls, 0);
	lifecycle.releaseInstance(ModuleInstanceKind::Output);

	EXPECT_EQ(harness.cleanupCalls, 1);
	EXPECT_EQ(lifecycle.snapshot().phase, ModuleLifecyclePhase::Unloaded);
	ASSERT_GE(harness.events.size(), 4u);
	EXPECT_EQ(harness.events[harness.events.size() - 2], "detach-logger");
	EXPECT_EQ(harness.events.back(), "cleanup-rtc");
}

TEST(ModuleLifecycleTest, RepeatedUnloadAndReloadRunsOneCleanupPerLoadedGeneration)
{
	ModuleLifecycleHarness harness;
	VDONinjaModuleLifecycle lifecycle(harness.operations(), std::chrono::milliseconds(10));

	ASSERT_TRUE(lifecycle.load());
	lifecycle.unload();
	lifecycle.unload();
	ASSERT_TRUE(lifecycle.load());
	lifecycle.unload();

	EXPECT_EQ(harness.attachCalls, 2);
	EXPECT_EQ(harness.detachCalls, 2);
	EXPECT_EQ(harness.preloadCalls, 2);
	EXPECT_EQ(harness.cleanupCalls, 2);
	EXPECT_EQ(lifecycle.snapshot().loadGeneration, 2u);
}

TEST(ModuleLifecycleTest, ReloadIsRejectedUntilPendingInstancesReachTheCleanupBoundary)
{
	ModuleLifecycleHarness harness;
	VDONinjaModuleLifecycle lifecycle(harness.operations(), std::chrono::milliseconds(10));
	ASSERT_TRUE(lifecycle.load());
	ASSERT_TRUE(lifecycle.tryAcquireInstance(ModuleInstanceKind::Source));
	lifecycle.unload();

	EXPECT_FALSE(lifecycle.load());
	EXPECT_EQ(harness.attachCalls, 1);
	EXPECT_EQ(harness.cleanupCalls, 0);
	lifecycle.releaseInstance(ModuleInstanceKind::Source);
	EXPECT_EQ(harness.cleanupCalls, 1);
	EXPECT_TRUE(lifecycle.load());
	EXPECT_EQ(harness.attachCalls, 2);
	lifecycle.unload();
}

TEST(ModuleLifecycleTest, CleanupTimeoutBlocksReloadUntilTheOriginalCleanupFutureCompletes)
{
	ModuleLifecycleHarness harness;
	auto cleanupPromise = std::make_shared<std::promise<void>>();
	harness.cleanupFutures.push_back(cleanupPromise->get_future().share());
	VDONinjaModuleLifecycle lifecycle(harness.operations(), std::chrono::milliseconds(1));
	ASSERT_TRUE(lifecycle.load());
	lifecycle.unload();

	EXPECT_EQ(lifecycle.snapshot().phase, ModuleLifecyclePhase::CleanupTimedOut);
	EXPECT_FALSE(lifecycle.load());
	EXPECT_EQ(harness.cleanupCalls, 1);
	cleanupPromise->set_value();
	EXPECT_TRUE(lifecycle.load());
	EXPECT_EQ(harness.cleanupCalls, 1);
	EXPECT_EQ(harness.attachCalls, 2);
	lifecycle.unload();
}

TEST(ModuleLifecycleTest, CleanupFailureIsReportedAndPermanentlyBlocksUnsafeReload)
{
	ModuleLifecycleHarness harness;
	std::promise<void> cleanupPromise;
	cleanupPromise.set_exception(std::make_exception_ptr(std::runtime_error("cleanup failed")));
	harness.cleanupFutures.push_back(cleanupPromise.get_future().share());
	VDONinjaModuleLifecycle lifecycle(harness.operations(), std::chrono::milliseconds(10));
	ASSERT_TRUE(lifecycle.load());
	lifecycle.unload();

	EXPECT_EQ(lifecycle.snapshot().phase, ModuleLifecyclePhase::CleanupFailed);
	EXPECT_FALSE(lifecycle.load());
	EXPECT_EQ(harness.attachCalls, 1);
	EXPECT_FALSE(harness.logs.empty());
}

TEST(ModuleLifecycleTest, InstanceCreationIsRejectedAfterUnloadBegins)
{
	ModuleLifecycleHarness harness;
	VDONinjaModuleLifecycle lifecycle(harness.operations(), std::chrono::milliseconds(10));
	ASSERT_TRUE(lifecycle.load());
	ASSERT_TRUE(lifecycle.tryAcquireInstance(ModuleInstanceKind::Output));
	lifecycle.unload();

	EXPECT_FALSE(lifecycle.tryAcquireInstance(ModuleInstanceKind::Source));
	EXPECT_FALSE(lifecycle.tryAcquireInstance(ModuleInstanceKind::ControlCenter));
	lifecycle.releaseInstance(ModuleInstanceKind::Output);
	EXPECT_EQ(harness.cleanupCalls, 1);
}

TEST(ModuleLifecycleTest, LastReleaseCannotStartCleanupUntilLoggerDetachReturns)
{
	std::mutex detachMutex;
	std::condition_variable detachCv;
	bool detachEntered = false;
	bool releaseDetach = false;
	std::atomic<int> cleanupCalls{0};
	ModuleLifecycleOperations operations{
	    []() {},
	    [&]() {
		    std::unique_lock<std::mutex> lock(detachMutex);
		    detachEntered = true;
		    detachCv.notify_all();
		    detachCv.wait(lock, [&]() { return releaseDetach; });
	    },
	    []() {},
	    [&]() {
		    cleanupCalls.fetch_add(1);
		    return readyFuture();
	    },
	    {},
	};
	VDONinjaModuleLifecycle lifecycle(std::move(operations), std::chrono::milliseconds(10));
	ASSERT_TRUE(lifecycle.load());
	ASSERT_TRUE(lifecycle.tryAcquireInstance(ModuleInstanceKind::Output));

	std::thread unloadThread([&]() { lifecycle.unload(); });
	{
		std::unique_lock<std::mutex> lock(detachMutex);
		ASSERT_TRUE(detachCv.wait_for(lock, std::chrono::seconds(2), [&]() { return detachEntered; }));
	}
	auto releaseFuture = std::async(std::launch::async, [&]() {
		lifecycle.releaseInstance(ModuleInstanceKind::Output);
		return lifecycle.snapshot().phase;
	});
	EXPECT_EQ(releaseFuture.wait_for(std::chrono::milliseconds(200)), std::future_status::ready);
	EXPECT_EQ(cleanupCalls.load(), 0);
	EXPECT_EQ(releaseFuture.get(), ModuleLifecyclePhase::Unloading);
	{
		std::lock_guard<std::mutex> lock(detachMutex);
		releaseDetach = true;
	}
	detachCv.notify_all();
	unloadThread.join();

	EXPECT_EQ(cleanupCalls.load(), 1);
	EXPECT_EQ(lifecycle.snapshot().phase, ModuleLifecyclePhase::Unloaded);
}

TEST(ModuleLifecycleTest, ExternalAttachOperationDoesNotHoldTheLifecycleStateMutex)
{
	std::mutex attachMutex;
	std::condition_variable attachCv;
	bool attachEntered = false;
	bool releaseAttach = false;
	VDONinjaModuleLifecycle *lifecyclePtr = nullptr;
	ModuleLifecycleOperations operations{
	    [&]() {
		    std::unique_lock<std::mutex> lock(attachMutex);
		    attachEntered = true;
		    attachCv.notify_all();
		    attachCv.wait(lock, [&]() { return releaseAttach; });
	    },
	    []() {},
	    []() {},
	    []() { return readyFuture(); },
	    {},
	};
	VDONinjaModuleLifecycle lifecycle(std::move(operations), std::chrono::milliseconds(10));
	lifecyclePtr = &lifecycle;
	std::thread loadThread([&]() { EXPECT_TRUE(lifecycle.load()); });
	{
		std::unique_lock<std::mutex> lock(attachMutex);
		ASSERT_TRUE(attachCv.wait_for(lock, std::chrono::seconds(2), [&]() { return attachEntered; }));
	}
	auto snapshotFuture = std::async(std::launch::async, [&]() { return lifecyclePtr->snapshot(); });
	EXPECT_EQ(snapshotFuture.wait_for(std::chrono::milliseconds(200)), std::future_status::ready);
	EXPECT_EQ(snapshotFuture.get().phase, ModuleLifecyclePhase::Loading);
	{
		std::lock_guard<std::mutex> lock(attachMutex);
		releaseAttach = true;
	}
	attachCv.notify_all();
	loadThread.join();
	lifecycle.unload();
}

TEST(ModuleLifecycleTest, ExternalLogCallbackDoesNotHoldTheLifecycleStateMutex)
{
	std::mutex logMutex;
	std::condition_variable logCv;
	bool blockNextLog = false;
	bool logEntered = false;
	bool releaseLog = false;
	ModuleLifecycleOperations operations{
	    []() {},
	    []() {},
	    []() {},
	    []() { return readyFuture(); },
	    [&](ModuleLifecycleLogLevel, const std::string &) {
		    std::unique_lock<std::mutex> lock(logMutex);
		    if (!blockNextLog) {
			    return;
		    }
		    logEntered = true;
		    logCv.notify_all();
		    logCv.wait(lock, [&]() { return releaseLog; });
	    },
	};
	VDONinjaModuleLifecycle lifecycle(std::move(operations), std::chrono::milliseconds(10));
	ASSERT_TRUE(lifecycle.load());
	ASSERT_TRUE(lifecycle.tryAcquireInstance(ModuleInstanceKind::Source));
	lifecycle.unload();
	{
		std::lock_guard<std::mutex> lock(logMutex);
		blockNextLog = true;
	}
	std::thread rejectedLoad([&]() { EXPECT_FALSE(lifecycle.load()); });
	{
		std::unique_lock<std::mutex> lock(logMutex);
		ASSERT_TRUE(logCv.wait_for(lock, std::chrono::seconds(2), [&]() { return logEntered; }));
	}
	auto snapshotFuture = std::async(std::launch::async, [&]() { return lifecycle.snapshot(); });
	EXPECT_EQ(snapshotFuture.wait_for(std::chrono::milliseconds(200)), std::future_status::ready);
	EXPECT_EQ(snapshotFuture.get().phase, ModuleLifecyclePhase::UnloadPending);
	{
		std::lock_guard<std::mutex> lock(logMutex);
		releaseLog = true;
	}
	logCv.notify_all();
	rejectedLoad.join();
	lifecycle.releaseInstance(ModuleInstanceKind::Source);
}

TEST(ModuleLifecycleTest, TwentyFiveLifecycleGenerationsDoNotLeakOperationsAcrossReloads)
{
	ModuleLifecycleHarness harness;
	VDONinjaModuleLifecycle lifecycle(harness.operations(), std::chrono::milliseconds(10));
	for (int generation = 0; generation < 25; ++generation) {
		ASSERT_TRUE(lifecycle.load()) << generation;
		ASSERT_TRUE(lifecycle.tryAcquireInstance(ModuleInstanceKind::Source)) << generation;
		lifecycle.releaseInstance(ModuleInstanceKind::Source);
		lifecycle.unload();
		EXPECT_EQ(lifecycle.snapshot().phase, ModuleLifecyclePhase::Unloaded) << generation;
	}

	EXPECT_EQ(harness.attachCalls, 25);
	EXPECT_EQ(harness.detachCalls, 25);
	EXPECT_EQ(harness.preloadCalls, 25);
	EXPECT_EQ(harness.cleanupCalls, 25);
	EXPECT_EQ(lifecycle.snapshot().loadGeneration, 25u);
}

TEST(ModuleLifecycleTest, FailedCreateRollbackAndDuplicateDestroyCannotDoubleCleanup)
{
	ModuleLifecycleHarness harness;
	VDONinjaModuleLifecycle lifecycle(harness.operations(), std::chrono::milliseconds(10));
	ASSERT_TRUE(lifecycle.load());
	VDONinjaModuleInstanceBoundary instances(lifecycle);
	int object = 0;
	int destroyCalls = 0;
	void *created = instances.create(
	    ModuleInstanceKind::Source, [&]() -> void * { return &object; }, [&](void *) { ++destroyCalls; });
	ASSERT_EQ(created, &object);
	EXPECT_EQ(lifecycle.snapshot().totalLiveInstances, 1u);
	lifecycle.unload();
	lifecycle.unload();
	EXPECT_TRUE(instances.destroy(ModuleInstanceKind::Source, created, [&](void *) {
		++destroyCalls;
		EXPECT_EQ(lifecycle.snapshot().phase, ModuleLifecyclePhase::UnloadPending);
		EXPECT_EQ(harness.cleanupCalls, 0);
	}));
	EXPECT_FALSE(instances.destroy(ModuleInstanceKind::Source, created, [&](void *) { ++destroyCalls; }));

	EXPECT_EQ(harness.detachCalls, 1);
	EXPECT_EQ(harness.cleanupCalls, 1);
	EXPECT_EQ(destroyCalls, 1);
	EXPECT_EQ(lifecycle.snapshot().totalLiveInstances, 0u);
}

TEST(ModuleLifecycleTest, InstanceBoundaryRollsBackNullAndThrowingCreateCallbacks)
{
	ModuleLifecycleHarness harness;
	VDONinjaModuleLifecycle lifecycle(harness.operations(), std::chrono::milliseconds(10));
	ASSERT_TRUE(lifecycle.load());
	VDONinjaModuleInstanceBoundary instances(lifecycle);
	int rollbackDestroys = 0;

	EXPECT_EQ(instances.create(
	              ModuleInstanceKind::Output, []() -> void * { return nullptr; }, [&](void *) { ++rollbackDestroys; }),
	          nullptr);
	EXPECT_EQ(instances.create(
	              ModuleInstanceKind::ControlCenter, []() -> void * { throw std::runtime_error("create failed"); },
	              [&](void *) { ++rollbackDestroys; }),
	          nullptr);
	EXPECT_EQ(lifecycle.snapshot().totalLiveInstances, 0u);
	EXPECT_EQ(rollbackDestroys, 0);
	lifecycle.unload();
	EXPECT_EQ(harness.cleanupCalls, 1);
}

TEST(ModuleLifecycleTest, InstanceBoundaryReleasesPermitEvenWhenOriginalDestroyThrows)
{
	ModuleLifecycleHarness harness;
	VDONinjaModuleLifecycle lifecycle(harness.operations(), std::chrono::milliseconds(10));
	ASSERT_TRUE(lifecycle.load());
	VDONinjaModuleInstanceBoundary instances(lifecycle);
	int object = 0;
	void *created = instances.create(
	    ModuleInstanceKind::Output, [&]() -> void * { return &object; }, [](void *) {});
	ASSERT_EQ(created, &object);
	lifecycle.unload();

	EXPECT_TRUE(instances.destroy(ModuleInstanceKind::Output, created,
	                              [](void *) { throw std::runtime_error("destroy failed"); }));
	EXPECT_EQ(lifecycle.snapshot().phase, ModuleLifecyclePhase::Unloaded);
	EXPECT_EQ(lifecycle.snapshot().totalLiveInstances, 0u);
	EXPECT_EQ(harness.cleanupCalls, 1);
}

TEST(ModuleLifecycleTest, InstanceBoundaryPermitCoversCreatorThatOverlapsModuleUnload)
{
	ModuleLifecycleHarness harness;
	VDONinjaModuleLifecycle lifecycle(harness.operations(), std::chrono::milliseconds(10));
	ASSERT_TRUE(lifecycle.load());
	VDONinjaModuleInstanceBoundary instances(lifecycle);
	std::mutex createMutex;
	std::condition_variable createCv;
	bool createEntered = false;
	bool finishCreate = false;
	int object = 0;

	auto createFuture = std::async(std::launch::async, [&] {
		return instances.create(
		    ModuleInstanceKind::Source,
		    [&]() -> void * {
			    std::unique_lock lock(createMutex);
			    createEntered = true;
			    createCv.notify_all();
			    createCv.wait(lock, [&] { return finishCreate; });
			    return &object;
		    },
		    [](void *) {});
	});
	bool creatorReachedCallback = false;
	{
		std::unique_lock lock(createMutex);
		creatorReachedCallback = createCv.wait_for(lock, std::chrono::seconds(2), [&] { return createEntered; });
	}
	if (!creatorReachedCallback) {
		{
			std::lock_guard lock(createMutex);
			finishCreate = true;
		}
		createCv.notify_all();
		(void)createFuture.wait_for(std::chrono::seconds(2));
		FAIL() << "tracked creator did not reach the original callback";
		return;
	}

	lifecycle.unload();
	EXPECT_EQ(lifecycle.snapshot().phase, ModuleLifecyclePhase::UnloadPending);
	EXPECT_EQ(lifecycle.snapshot().totalLiveInstances, 1u);
	EXPECT_EQ(harness.cleanupCalls, 0);
	{
		std::lock_guard lock(createMutex);
		finishCreate = true;
	}
	createCv.notify_all();
	ASSERT_EQ(createFuture.wait_for(std::chrono::seconds(2)), std::future_status::ready);
	void *created = createFuture.get();
	ASSERT_EQ(created, &object);

	EXPECT_TRUE(instances.destroy(ModuleInstanceKind::Source, created, [](void *) {}));
	EXPECT_EQ(harness.cleanupCalls, 1);
	EXPECT_EQ(lifecycle.snapshot().phase, ModuleLifecyclePhase::Unloaded);
}

TEST(ModuleLifecycleTest, PreloadFailureDetachesLoggerCleansUpAndAllowsACompleteRetry)
{
	int attachCalls = 0;
	int detachCalls = 0;
	int preloadCalls = 0;
	int cleanupCalls = 0;
	bool failPreload = true;
	ModuleLifecycleOperations operations{
	    [&]() { ++attachCalls; },
	    [&]() { ++detachCalls; },
	    [&]() {
		    ++preloadCalls;
		    if (failPreload) {
			    throw std::runtime_error("preload failed");
		    }
	    },
	    [&]() {
		    ++cleanupCalls;
		    return readyFuture();
	    },
	    {},
	};
	VDONinjaModuleLifecycle lifecycle(std::move(operations), std::chrono::milliseconds(10));
	EXPECT_FALSE(lifecycle.load());
	EXPECT_EQ(lifecycle.snapshot().phase, ModuleLifecyclePhase::Unloaded);
	EXPECT_EQ(attachCalls, 1);
	EXPECT_EQ(detachCalls, 1);
	EXPECT_EQ(cleanupCalls, 1);

	failPreload = false;
	EXPECT_TRUE(lifecycle.load());
	lifecycle.unload();
	EXPECT_EQ(attachCalls, 2);
	EXPECT_EQ(detachCalls, 2);
	EXPECT_EQ(preloadCalls, 2);
	EXPECT_EQ(cleanupCalls, 2);
}

TEST(ModuleLifecycleTest, LoggerAttachFailureStillAttemptsSynchronousDetachBeforeReturning)
{
	int attachCalls = 0;
	int detachCalls = 0;
	int preloadCalls = 0;
	int cleanupCalls = 0;
	ModuleLifecycleOperations operations{
	    [&]() {
		    ++attachCalls;
		    throw std::runtime_error("logger attach failed after partial publication");
	    },
	    [&]() { ++detachCalls; },
	    [&]() { ++preloadCalls; },
	    [&]() {
		    ++cleanupCalls;
		    return readyFuture();
	    },
	    {},
	};
	VDONinjaModuleLifecycle lifecycle(std::move(operations), std::chrono::milliseconds(10));

	EXPECT_FALSE(lifecycle.load());
	EXPECT_EQ(lifecycle.snapshot().phase, ModuleLifecyclePhase::Unloaded);
	EXPECT_FALSE(lifecycle.snapshot().loggerAttached);
	EXPECT_EQ(attachCalls, 1);
	EXPECT_EQ(detachCalls, 1);
	EXPECT_EQ(preloadCalls, 0);
	EXPECT_EQ(cleanupCalls, 0);
}

TEST(ModuleLifecycleTest, InvalidCleanupFutureFailsClosedAndBlocksReload)
{
	ModuleLifecycleOperations operations{
	    []() {}, []() {}, []() {}, []() { return std::shared_future<void>{}; }, {},
	};
	VDONinjaModuleLifecycle lifecycle(std::move(operations), std::chrono::milliseconds(10));
	ASSERT_TRUE(lifecycle.load());
	lifecycle.unload();
	EXPECT_EQ(lifecycle.snapshot().phase, ModuleLifecyclePhase::CleanupFailed);
	EXPECT_FALSE(lifecycle.load());
}

} // namespace
