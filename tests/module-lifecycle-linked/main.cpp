/*
 * Static libdatachannel module-boundary integration gate.
 *
 * This executable intentionally uses the same static libdatachannel package as
 * the OBS plugin. It catches shutdown defects that a fake lifecycle future or
 * logger callback cannot reproduce.
 */

#include <rtc/rtc.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

#include <plog/Log.h>

#include "vdoninja-module-lifecycle.h"

namespace
{

using namespace std::chrono_literals;
using vdoninja::ModuleInstanceKind;
using vdoninja::ModuleLifecycleLogLevel;
using vdoninja::ModuleLifecycleOperations;
using vdoninja::ModuleLifecyclePhase;
using vdoninja::VDONinjaModuleLifecycle;

void require(bool condition, const std::string &message)
{
	if (!condition) {
		throw std::runtime_error(message);
	}
}

void detachRtcLogger() noexcept
{
	try {
		rtc::InitLogger(rtc::LogLevel::None, nullptr);
	} catch (...) {
	}
}

void drainRtcCleanup() noexcept
{
	try {
		auto future = rtc::Cleanup();
		if (future.valid() && future.wait_for(10s) == std::future_status::ready) {
			future.get();
		}
	} catch (...) {
	}
}

void validateSynchronizedLoggerDetachAndGenerationIsolation()
{
	std::mutex mutex;
	std::condition_variable enteredCv;
	std::condition_variable releaseCv;
	bool callbackEntered = false;
	bool releaseCallback = false;
	std::atomic<int> firstGenerationCalls{0};
	std::atomic<bool> detachReturned{false};

	rtc::InitLogger(rtc::LogLevel::Warning, [&](rtc::LogLevel, std::string) {
		firstGenerationCalls.fetch_add(1, std::memory_order_relaxed);
		std::unique_lock lock(mutex);
		callbackEntered = true;
		enteredCv.notify_all();
		releaseCv.wait(lock, [&] { return releaseCallback; });
	});

	std::thread logThread([] { PLOG_WARNING << "module lifecycle linked gate: blocked callback"; });
	bool callbackWasEntered = false;
	{
		std::unique_lock lock(mutex);
		callbackWasEntered = enteredCv.wait_for(lock, 5s, [&] { return callbackEntered; });
	}

	std::thread detachThread;
	bool detachReturnedBeforeRelease = false;
	if (callbackWasEntered) {
		detachThread = std::thread([&] {
			rtc::InitLogger(rtc::LogLevel::None, nullptr);
			detachReturned.store(true, std::memory_order_release);
		});
		std::this_thread::sleep_for(100ms);
		detachReturnedBeforeRelease = detachReturned.load(std::memory_order_acquire);
	}
	{
		std::lock_guard lock(mutex);
		releaseCallback = true;
	}
	releaseCv.notify_all();
	logThread.join();
	if (detachThread.joinable()) {
		detachThread.join();
	} else {
		detachRtcLogger();
	}

	require(callbackWasEntered, "real plog warning did not enter the libdatachannel logger callback");
	require(!detachReturnedBeforeRelease,
	        "InitLogger(None, nullptr) returned while an earlier logger callback was still in flight");
	require(detachReturned.load(std::memory_order_acquire), "logger detach did not complete after callback exit");
	require(firstGenerationCalls.load(std::memory_order_relaxed) == 1,
	        "first logger generation received an unexpected number of callbacks");

	PLOG_WARNING << "module lifecycle linked gate: warning after detach";
	require(firstGenerationCalls.load(std::memory_order_relaxed) == 1,
	        "detached logger callback was invoked by a later plog warning");

	std::atomic<int> secondGenerationCalls{0};
	rtc::InitLogger(rtc::LogLevel::Warning, [&](rtc::LogLevel, std::string) { secondGenerationCalls.fetch_add(1); });
	PLOG_WARNING << "module lifecycle linked gate: second logger generation";
	require(firstGenerationCalls.load(std::memory_order_relaxed) == 1,
	        "the previous logger generation received a callback after reattach");
	require(secondGenerationCalls.load(std::memory_order_relaxed) == 1,
	        "the reattached logger generation did not receive exactly one callback");
	detachRtcLogger();
	PLOG_WARNING << "module lifecycle linked gate: warning after second detach";
	require(secondGenerationCalls.load(std::memory_order_relaxed) == 1,
	        "the second logger generation received a callback after detach");
}

void validateRealRtcTokenCleanupBoundary()
{
	rtc::Preload();
	auto peer = std::make_unique<rtc::PeerConnection>();
	auto dataChannel = peer->createDataChannel("module-lifecycle-linked-gate");
	auto cleanup = rtc::Cleanup();

	require(cleanup.valid(), "rtc::Cleanup returned an invalid future");
	require(cleanup.wait_for(100ms) == std::future_status::timeout,
	        "rtc::Cleanup completed while a PeerConnection token was still alive");

	dataChannel->close();
	dataChannel.reset();
	peer->close();
	peer.reset();
	require(cleanup.wait_for(10s) == std::future_status::ready,
	        "rtc::Cleanup did not complete within 10 seconds after PeerConnection destruction");
	cleanup.get();

#if RTC_ENABLE_WEBSOCKET
	rtc::Preload();
	auto socket = std::make_unique<rtc::WebSocket>();
	auto socketCleanup = rtc::Cleanup();
	require(socketCleanup.wait_for(100ms) == std::future_status::timeout,
	        "rtc::Cleanup completed while a WebSocket token was still alive");
	socket.reset();
	require(socketCleanup.wait_for(10s) == std::future_status::ready,
	        "rtc::Cleanup did not complete within 10 seconds after WebSocket destruction");
	socketCleanup.get();
#endif
}

void validateRealDeferredLifecycleCleanupAndUnlockedSnapshot()
{
	std::mutex cleanupMutex;
	std::condition_variable cleanupCv;
	bool cleanupStarted = false;
	std::atomic<int> attachCalls{0};
	std::atomic<int> detachCalls{0};
	std::atomic<int> preloadCalls{0};
	std::atomic<int> cleanupCalls{0};

	ModuleLifecycleOperations operations;
	operations.attachLogger = [&] {
		attachCalls.fetch_add(1);
		rtc::InitLogger(rtc::LogLevel::Warning, [](rtc::LogLevel, std::string) {});
	};
	operations.detachLogger = [&] {
		detachCalls.fetch_add(1);
		rtc::InitLogger(rtc::LogLevel::None, nullptr);
	};
	operations.preloadRtc = [&] {
		preloadCalls.fetch_add(1);
		rtc::Preload();
	};
	operations.beginRtcCleanup = [&] {
		cleanupCalls.fetch_add(1);
		{
			std::lock_guard lock(cleanupMutex);
			cleanupStarted = true;
		}
		cleanupCv.notify_all();
		return rtc::Cleanup();
	};
	operations.log = [](ModuleLifecycleLogLevel, const std::string &) {};

	VDONinjaModuleLifecycle lifecycle(std::move(operations), 10s);
	require(lifecycle.load(), "real lifecycle generation did not load");
	require(lifecycle.tryAcquireInstance(ModuleInstanceKind::Source), "source permit was rejected");
	require(lifecycle.tryAcquireInstance(ModuleInstanceKind::Output), "output permit was rejected");
	require(lifecycle.tryAcquireInstance(ModuleInstanceKind::ControlCenter), "Control Center permit was rejected");

	auto peer = std::make_unique<rtc::PeerConnection>();
	lifecycle.unload();
	auto pending = lifecycle.snapshot();
	require(pending.phase == ModuleLifecyclePhase::UnloadPending,
	        "unload did not defer cleanup for live plugin instances");
	require(pending.totalLiveInstances == 3, "unload lost a tracked plugin instance");
	require(detachCalls.load() == 1, "unload did not synchronously detach the real logger exactly once");
	require(cleanupCalls.load() == 0, "cleanup started before tracked instances were destroyed");

	lifecycle.releaseInstance(ModuleInstanceKind::Source);
	lifecycle.releaseInstance(ModuleInstanceKind::Output);
	auto finalRelease =
	    std::async(std::launch::async, [&] { lifecycle.releaseInstance(ModuleInstanceKind::ControlCenter); });
	bool cleanupWasStarted = false;
	{
		std::unique_lock lock(cleanupMutex);
		cleanupWasStarted = cleanupCv.wait_for(lock, 5s, [&] { return cleanupStarted; });
	}

	bool snapshotReturnedDuringRealWait = false;
	ModuleLifecyclePhase phaseDuringWait = ModuleLifecyclePhase::CleanupFailed;
	if (cleanupWasStarted) {
		auto snapshotFuture = std::async(std::launch::async, [&] { return lifecycle.snapshot(); });
		snapshotReturnedDuringRealWait = snapshotFuture.wait_for(1s) == std::future_status::ready;
		if (snapshotReturnedDuringRealWait) {
			phaseDuringWait = snapshotFuture.get().phase;
		}
	}

	peer->close();
	peer.reset();
	const bool releaseCompleted = finalRelease.wait_for(10s) == std::future_status::ready;
	if (releaseCompleted) {
		finalRelease.get();
	}

	require(cleanupWasStarted, "last tracked destroy did not enter the real rtc::Cleanup boundary");
	require(snapshotReturnedDuringRealWait, "snapshot blocked behind the real rtc::Cleanup wait");
	require(phaseDuringWait == ModuleLifecyclePhase::Cleaning,
	        "lifecycle did not reserve Cleaning while the real cleanup future was pending");
	require(releaseCompleted, "last tracked destroy did not finish after the RTC token was released");
	require(lifecycle.snapshot().phase == ModuleLifecyclePhase::Unloaded,
	        "lifecycle did not reach Unloaded after real cleanup completed");
	require(attachCalls.load() == 1 && detachCalls.load() == 1 && preloadCalls.load() == 1 && cleanupCalls.load() == 1,
	        "real lifecycle generation did not run exactly one attach/detach/preload/cleanup operation");
}

template <typename Function> bool runCase(const char *name, Function &&function)
{
	std::cout << "[ RUN      ] " << name << std::endl;
	try {
		function();
		std::cout << "[       OK ] " << name << std::endl;
		return true;
	} catch (const std::exception &error) {
		std::cerr << "[  FAILED  ] " << name << ": " << error.what() << std::endl;
		return false;
	} catch (...) {
		std::cerr << "[  FAILED  ] " << name << ": unknown exception" << std::endl;
		return false;
	}
}

} // namespace

int main()
{
	int failures = 0;
	failures += !runCase("RealLogger.SynchronizedDetachAndGenerationIsolation",
	                     validateSynchronizedLoggerDetachAndGenerationIsolation);
	failures += !runCase("RealRtc.PeerDataChannelAndWebSocketHoldCleanupToken", validateRealRtcTokenCleanupBoundary);
	failures += !runCase("RealLifecycle.DeferredCleanupAndUnlockedSnapshot",
	                     validateRealDeferredLifecycleCleanupAndUnlockedSnapshot);

	detachRtcLogger();
	drainRtcCleanup();
	if (failures == 0) {
		std::cout << "[  PASSED  ] 3 real static-libdatachannel lifecycle gates" << std::endl;
		return 0;
	}
	std::cerr << "[  FAILED  ] " << failures << " real static-libdatachannel lifecycle gate(s)" << std::endl;
	return 1;
}
