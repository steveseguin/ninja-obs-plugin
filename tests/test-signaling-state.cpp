/*
 * Unit tests for signaling client state transitions
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#include <rtc/rtc.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "vdoninja-signaling.h"

using namespace vdoninja;

using ::testing::ElementsAre;
using ::testing::Pair;

TEST(SignalingStateTest, ReplacedSocketOpenCannotRepublishConnectedState)
{
	VDONinjaSignaling signaling;
	std::atomic<int> connectedCount{0};
	signaling.setOnConnected([&]() { connectedCount.fetch_add(1); });

	auto ws1 = signaling.beginSocketAttemptForTesting();
	auto ws2 = signaling.beginSocketAttemptForTesting();
	ws2.onOpen();
	ASSERT_TRUE(signaling.isConnected());
	ASSERT_EQ(connectedCount.load(), 1);

	ws1.onOpen();

	EXPECT_TRUE(signaling.isConnected());
	EXPECT_EQ(connectedCount.load(), 1);
}

TEST(SignalingStateTest, ReplacedSocketMessageCannotDeliverPeerLifecycle)
{
	VDONinjaSignaling signaling;
	std::atomic<int> cleanupCount{0};
	std::atomic<int> lifecycleCount{0};
	signaling.setOnPeerCleanup([&](const std::string &, const std::string &) { cleanupCount.fetch_add(1); });
	signaling.setOnLifecycleEvent([&](const SignalingLifecycleEvent &) { lifecycleCount.fetch_add(1); });

	auto ws1 = signaling.beginSocketAttemptForTesting();
	auto ws2 = signaling.beginSocketAttemptForTesting();
	ws2.onOpen();

	ws1.onMessage(R"({"bye":true,"UUID":"peer-from-ws1"})");

	EXPECT_TRUE(signaling.isConnected());
	EXPECT_EQ(cleanupCount.load(), 0);
	EXPECT_EQ(lifecycleCount.load(), 0);
}

TEST(SignalingStateTest, ReplacedSocketListingCannotCommitAfterCurrentEpochChanges)
{
	VDONinjaSignaling signaling;
	std::mutex pauseMutex;
	std::condition_variable pauseCv;
	bool staleCommitReached = false;
	bool releaseStaleCommit = false;
	std::atomic<int> hookCalls{0};
	signaling.setBeforeSocketStateCommitForTesting([&]() {
		if (hookCalls.fetch_add(1) != 0) {
			return;
		}
		std::unique_lock<std::mutex> lock(pauseMutex);
		staleCommitReached = true;
		pauseCv.notify_all();
		pauseCv.wait(lock, [&]() { return releaseStaleCommit; });
	});

	auto ws1 = signaling.beginSocketAttemptForTesting();
	std::thread staleMessage(
	    [&]() { ws1.onMessage(R"({"request":"listing","list":[{"UUID":"old-peer","streamID":"old-stream"}]})"); });
	{
		std::unique_lock<std::mutex> lock(pauseMutex);
		EXPECT_TRUE(pauseCv.wait_for(lock, std::chrono::seconds(2), [&]() { return staleCommitReached; }));
	}

	auto ws2 = signaling.beginSocketAttemptForTesting();
	ws2.onMessage(R"({"request":"listing","list":[{"UUID":"new-peer","streamID":"new-stream"}]})");
	EXPECT_THAT(signaling.getCurrentRoomMembers(), ElementsAre("new-stream"));
	{
		std::lock_guard<std::mutex> lock(pauseMutex);
		releaseStaleCommit = true;
	}
	pauseCv.notify_all();
	staleMessage.join();
	signaling.setBeforeSocketStateCommitForTesting({});

	EXPECT_THAT(signaling.getCurrentRoomMembers(), ElementsAre("new-stream"));
}

TEST(SignalingStateTest, ReplacedSocketAlertCannotCommitReconnectPolicyAfterCurrentEpochChanges)
{
	VDONinjaSignaling signaling;
	std::mutex pauseMutex;
	std::condition_variable pauseCv;
	bool staleCommitReached = false;
	bool releaseStaleCommit = false;
	std::atomic<int> hookCalls{0};
	signaling.setBeforeSocketStateCommitForTesting([&]() {
		if (hookCalls.fetch_add(1) != 0) {
			return;
		}
		std::unique_lock<std::mutex> lock(pauseMutex);
		staleCommitReached = true;
		pauseCv.notify_all();
		pauseCv.wait(lock, [&]() { return releaseStaleCommit; });
	});

	auto ws1 = signaling.beginSocketAttemptForTesting();
	std::thread staleMessage([&]() { ws1.onMessage(R"({"request":"alert","message":"Room already claimed"})"); });
	{
		std::unique_lock<std::mutex> lock(pauseMutex);
		EXPECT_TRUE(pauseCv.wait_for(lock, std::chrono::seconds(2), [&]() { return staleCommitReached; }));
	}

	auto ws2 = signaling.beginSocketAttemptForTesting();
	ws2.onOpen();
	EXPECT_FALSE(signaling.reconnectSuppressedForTesting());
	{
		std::lock_guard<std::mutex> lock(pauseMutex);
		releaseStaleCommit = true;
	}
	pauseCv.notify_all();
	staleMessage.join();
	signaling.setBeforeSocketStateCommitForTesting({});

	EXPECT_FALSE(signaling.reconnectSuppressedForTesting());
}

TEST(SignalingStateTest, ReplacedSocketCloseCannotDisconnectOrScheduleReconnect)
{
	std::atomic<int> disconnectedCount{0};
	VDONinjaSignaling signaling;
	signaling.setOnDisconnected([&]() { disconnectedCount.fetch_add(1); });

	auto ws1 = signaling.beginSocketAttemptForTesting();
	ws1.onOpen();
	auto ws2 = signaling.beginSocketAttemptForTesting();
	ws2.onOpen();
	ASSERT_FALSE(signaling.reconnectRequestedForTesting());

	ws1.onClosed();

	EXPECT_TRUE(signaling.isConnected());
	EXPECT_FALSE(signaling.reconnectRequestedForTesting());
	EXPECT_EQ(disconnectedCount.load(), 0);
}

TEST(SignalingStateTest, ReplacedSocketErrorCannotAffectCurrentConnection)
{
	VDONinjaSignaling signaling;
	std::atomic<int> errorCount{0};
	signaling.setOnError([&](const std::string &) { errorCount.fetch_add(1); });

	auto ws1 = signaling.beginSocketAttemptForTesting();
	ws1.onOpen();
	auto ws2 = signaling.beginSocketAttemptForTesting();
	ws2.onOpen();

	ws1.onError("delayed WS1 error");

	EXPECT_TRUE(signaling.isConnected());
	EXPECT_FALSE(signaling.reconnectRequestedForTesting());
	EXPECT_EQ(errorCount.load(), 0);
}

TEST(SignalingStateTest, ReplacedPreOpenErrorCannotCloseCurrentWebSocketHandle)
{
	VDONinjaSignaling signaling;
	std::atomic<int> errorCount{0};
	signaling.setAutoReconnect(false, 0);
	signaling.setOnError([&](const std::string &) { errorCount.fetch_add(1); });

	auto ws1 = signaling.beginSocketAttemptForTesting();
	ASSERT_TRUE(signaling.connect("wss://unit.test"));
	ASSERT_TRUE(signaling.isConnected());

	ws1.onError("delayed pre-open WS1 error");

	EXPECT_TRUE(signaling.isConnected());
	EXPECT_FALSE(signaling.reconnectRequestedForTesting());
	EXPECT_EQ(errorCount.load(), 0);
	signaling.disconnect();
}

TEST(SignalingStateTest, ReplacedSocketReconnectTimerCannotProceed)
{
	VDONinjaSignaling signaling;
	auto ws1 = signaling.beginSocketAttemptForTesting();
	auto ws2 = signaling.beginSocketAttemptForTesting();

	ASSERT_EQ(signaling.currentSocketEpochForTesting(), ws2.socketEpoch);
	EXPECT_LT(ws1.socketEpoch, ws2.socketEpoch);
	EXPECT_FALSE(ws1.reconnectTimerMayProceed());
	EXPECT_TRUE(ws2.reconnectTimerMayProceed());
}

TEST(SignalingStateTest, CurrentSocketCallbacksRetainExistingBehavior)
{
	VDONinjaSignaling signaling;
	std::atomic<int> connectedCount{0};
	std::atomic<int> cleanupCount{0};
	std::atomic<int> errorCount{0};
	std::atomic<int> disconnectedCount{0};
	signaling.setOnConnected([&]() { connectedCount.fetch_add(1); });
	signaling.setOnPeerCleanup([&](const std::string &, const std::string &) { cleanupCount.fetch_add(1); });
	signaling.setOnError([&](const std::string &) { errorCount.fetch_add(1); });
	signaling.setOnDisconnected([&]() { disconnectedCount.fetch_add(1); });

	auto ws = signaling.beginSocketAttemptForTesting();
	ws.onOpen();
	ws.onMessage(R"({"request":"cleanup","UUID":"peer-current","session":"session-current"})");
	ws.onError("current socket error");
	ws.onClosed();

	EXPECT_FALSE(signaling.isConnected());
	EXPECT_TRUE(signaling.reconnectRequestedForTesting());
	EXPECT_TRUE(ws.reconnectTimerMayProceed());
	EXPECT_EQ(connectedCount.load(), 1);
	EXPECT_EQ(cleanupCount.load(), 1);
	EXPECT_EQ(errorCount.load(), 1);
	EXPECT_EQ(disconnectedCount.load(), 1);
}

TEST(SignalingStateTest, LifecycleEnvelopePreservesSocketOrderAndOptionalSession)
{
	VDONinjaSignaling signaling;
	std::vector<SignalingLifecycleEvent> events;
	signaling.setOnLifecycleEvent([&](const SignalingLifecycleEvent &event) { events.push_back(event); });

	auto ws = signaling.beginSocketAttemptForTesting();
	ws.onOpen();
	ws.onMessage(R"({"request":"cleanup","UUID":"peer-clean","session":"session-clean"})");
	ws.onMessage(R"({"bye":true,"UUID":"peer-bye"})");
	ws.onMessage(
	    R"({"request":"videoRemovedFromRoom","UUID":"peer-stream","streamID":"cam-1","session":"session-stream"})");
	ws.onMessage(R"({"videoRemovedFromRoom":true,"UUID":"peer-streamless"})");

	ASSERT_EQ(events.size(), 4u);
	for (size_t i = 0; i < events.size(); ++i) {
		EXPECT_TRUE(events[i].hasWebSocketOrigin());
		EXPECT_EQ(events[i].socketEpoch, ws.socketEpoch);
		if (i > 0) {
			EXPECT_LT(events[i - 1].wsSequence, events[i].wsSequence);
		}
	}

	EXPECT_EQ(events[0].kind, SignalingLifecycleEventKind::PeerCleanup);
	EXPECT_EQ(events[0].uuid, "peer-clean");
	ASSERT_TRUE(events[0].hasCorrelatedSession());
	EXPECT_EQ(*events[0].session, "session-clean");

	EXPECT_EQ(events[1].kind, SignalingLifecycleEventKind::PeerCleanup);
	EXPECT_EQ(events[1].uuid, "peer-bye");
	EXPECT_FALSE(events[1].hasCorrelatedSession());
	EXPECT_FALSE(events[1].session.has_value());

	EXPECT_EQ(events[2].kind, SignalingLifecycleEventKind::StreamRemoved);
	EXPECT_EQ(events[2].uuid, "peer-stream");
	EXPECT_EQ(events[2].streamId, "cam-1");
	ASSERT_TRUE(events[2].hasCorrelatedSession());
	EXPECT_EQ(*events[2].session, "session-stream");

	EXPECT_EQ(events[3].kind, SignalingLifecycleEventKind::StreamRemoved);
	EXPECT_EQ(events[3].uuid, "peer-streamless");
	EXPECT_TRUE(events[3].streamId.empty());
	EXPECT_FALSE(events[3].hasCorrelatedSession());
}

TEST(SignalingStateTest, LifecycleSocketEpochAndSequenceIncreaseAcrossReplacement)
{
	VDONinjaSignaling signaling;
	std::vector<SignalingLifecycleEvent> events;
	signaling.setOnLifecycleEvent([&](const SignalingLifecycleEvent &event) { events.push_back(event); });

	auto ws1 = signaling.beginSocketAttemptForTesting();
	ws1.onOpen();
	ws1.onMessage(R"({"bye":true,"UUID":"peer-ws1"})");
	auto ws2 = signaling.beginSocketAttemptForTesting();
	ws2.onOpen();
	ws2.onMessage(R"({"bye":true,"UUID":"peer-ws2"})");

	ASSERT_EQ(events.size(), 2u);
	EXPECT_LT(events[0].socketEpoch, events[1].socketEpoch);
	EXPECT_LT(events[0].wsSequence, events[1].wsSequence);
	EXPECT_EQ(events[0].socketEpoch, ws1.socketEpoch);
	EXPECT_EQ(events[1].socketEpoch, ws2.socketEpoch);
}

TEST(SignalingStateTest, AlternateTransportLifecycleIsExplicitlySocketUncorrelated)
{
	VDONinjaSignaling signaling;
	std::vector<SignalingLifecycleEvent> events;
	signaling.setOnLifecycleEvent([&](const SignalingLifecycleEvent &event) { events.push_back(event); });

	signaling.processIncomingMessage(R"({"bye":true,"UUID":"peer-datachannel"})");

	ASSERT_EQ(events.size(), 1u);
	EXPECT_FALSE(events[0].hasWebSocketOrigin());
	EXPECT_EQ(events[0].socketEpoch, 0u);
	EXPECT_EQ(events[0].wsSequence, 0u);
	EXPECT_EQ(events[0].uuid, "peer-datachannel");
	EXPECT_FALSE(events[0].session.has_value());
}

TEST(SignalingStateTest, ReentrantDisconnectStopsRemainingLegacyLifecycleDelivery)
{
	VDONinjaSignaling signaling;
	std::atomic<int> lifecycleCount{0};
	std::atomic<int> legacyCleanupCount{0};
	signaling.setOnLifecycleEvent([&](const SignalingLifecycleEvent &) {
		lifecycleCount.fetch_add(1);
		signaling.disconnect();
	});
	signaling.setOnPeerCleanup([&](const std::string &, const std::string &) { legacyCleanupCount.fetch_add(1); });

	auto ws = signaling.beginSocketAttemptForTesting();
	ws.onOpen();
	ws.onMessage(R"({"bye":true,"UUID":"peer-current"})");

	EXPECT_EQ(lifecycleCount.load(), 1);
	EXPECT_EQ(legacyCleanupCount.load(), 0);
	EXPECT_FALSE(signaling.isConnected());
}

TEST(SignalingStateTest, UserCallbackDoesNotHoldEpochLeaseAcrossCrossThreadDisconnect)
{
	VDONinjaSignaling signaling;
	std::mutex completionMutex;
	std::condition_variable completionCv;
	bool disconnectComplete = false;
	bool completedBeforeCallbackReturned = false;
	std::thread disconnectThread;

	signaling.setOnConnected([&]() {
		disconnectThread = std::thread([&]() {
			signaling.disconnect();
			{
				std::lock_guard<std::mutex> lock(completionMutex);
				disconnectComplete = true;
			}
			completionCv.notify_one();
		});

		std::unique_lock<std::mutex> lock(completionMutex);
		completedBeforeCallbackReturned =
		    completionCv.wait_for(lock, std::chrono::seconds(2), [&]() { return disconnectComplete; });
	});

	auto ws = signaling.beginSocketAttemptForTesting();
	ws.onOpen();
	if (disconnectThread.joinable()) {
		disconnectThread.join();
	}

	EXPECT_TRUE(completedBeforeCallbackReturned);
	EXPECT_TRUE(disconnectComplete);
	EXPECT_FALSE(signaling.isConnected());
}

TEST(SignalingStateTest, OnConnectedCanDisconnectDirectlyWithoutJoiningItsOwnWorker)
{
	VDONinjaSignaling signaling;
	std::atomic<bool> disconnectReturned{false};
	std::atomic<int> errorCount{0};
	signaling.setOnError([&](const std::string &) { errorCount.fetch_add(1); });
	signaling.setOnConnected([&]() {
		signaling.disconnect();
		disconnectReturned.store(true);
	});

	EXPECT_FALSE(signaling.connect("wss://unit.test"));

	EXPECT_TRUE(disconnectReturned.load());
	EXPECT_FALSE(signaling.isConnected());
	EXPECT_FALSE(signaling.reconnectRequestedForTesting());
	EXPECT_EQ(errorCount.load(), 0);
}

TEST(SignalingStateTest, OnConnectedCanWaitForCrossThreadDisconnectBeforeReturning)
{
	VDONinjaSignaling signaling;
	std::mutex completionMutex;
	std::condition_variable completionCv;
	bool disconnectComplete = false;
	bool completedBeforeCallbackReturned = false;
	std::thread disconnectThread;
	std::atomic<int> errorCount{0};
	signaling.setOnError([&](const std::string &) { errorCount.fetch_add(1); });
	signaling.setOnConnected([&]() {
		disconnectThread = std::thread([&]() {
			signaling.disconnect();
			{
				std::lock_guard<std::mutex> lock(completionMutex);
				disconnectComplete = true;
			}
			completionCv.notify_one();
		});

		std::unique_lock<std::mutex> lock(completionMutex);
		completedBeforeCallbackReturned =
		    completionCv.wait_for(lock, std::chrono::seconds(2), [&]() { return disconnectComplete; });
	});

	EXPECT_FALSE(signaling.connect("wss://unit.test"));
	if (disconnectThread.joinable()) {
		disconnectThread.join();
	}

	EXPECT_TRUE(completedBeforeCallbackReturned);
	EXPECT_TRUE(disconnectComplete);
	EXPECT_FALSE(signaling.isConnected());
	EXPECT_FALSE(signaling.reconnectRequestedForTesting());
	EXPECT_EQ(errorCount.load(), 0);

	// Reconnect must first join the deferred worker; no detached thread may
	// retain a raw signaling pointer after disconnect returns.
	signaling.setOnConnected({});
	EXPECT_TRUE(signaling.connect("wss://unit.test/reconnected"));
	signaling.disconnect();
}

TEST(SignalingStateTest, CrossThreadReconnectFailsFastWhileSocketCallbackWaitsForCaller)
{
	VDONinjaSignaling signaling;
	std::mutex completionMutex;
	std::condition_variable completionCv;
	bool reconnectComplete = false;
	bool completedBeforeCallbackReturned = false;
	std::thread reconnectThread;
	std::atomic<int> callbackCount{0};
	std::atomic<bool> reconnectResult{true};
	signaling.setOnConnected([&]() {
		if (callbackCount.fetch_add(1) != 0) {
			return;
		}
		reconnectThread = std::thread([&]() {
			signaling.disconnect();
			reconnectResult.store(signaling.connect("wss://unit.test/cross-thread-reconnect"));
			{
				std::lock_guard<std::mutex> lock(completionMutex);
				reconnectComplete = true;
			}
			completionCv.notify_one();
		});

		std::unique_lock<std::mutex> lock(completionMutex);
		completedBeforeCallbackReturned =
		    completionCv.wait_for(lock, std::chrono::seconds(2), [&]() { return reconnectComplete; });
	});

	(void)signaling.connect("wss://unit.test");
	if (reconnectThread.joinable()) {
		reconnectThread.join();
	}
	if (reconnectResult.load()) {
		signaling.disconnect();
	}

	EXPECT_TRUE(completedBeforeCallbackReturned);
	EXPECT_TRUE(reconnectComplete);
	EXPECT_FALSE(reconnectResult.load());
	EXPECT_FALSE(signaling.isConnected());

	signaling.setOnConnected({});
	EXPECT_TRUE(signaling.connect("wss://unit.test/external-reconnect"));
	signaling.disconnect();
}

TEST(SignalingStateTest, SocketCallbackThreadReconnectReturnsWithoutJoiningWorkerWaitingForCallback)
{
	VDONinjaSignaling signaling;
	signaling.setAutoReconnect(false, 0);
	ASSERT_TRUE(signaling.connect("wss://unit.test"));
	std::atomic<bool> reconnectReturned{false};
	std::atomic<bool> reconnectResult{true};

	std::thread callbackThread([&]() {
		signaling.invokeSocketUserCallbackForTesting([&]() {
			signaling.disconnect();
			reconnectResult.store(signaling.connect("wss://unit.test/callback-reconnect"));
			reconnectReturned.store(true);
		});
	});
	callbackThread.join();

	EXPECT_TRUE(reconnectReturned.load());
	EXPECT_FALSE(reconnectResult.load());
	EXPECT_FALSE(signaling.isConnected());
	EXPECT_TRUE(signaling.connect("wss://unit.test/external-reconnect"));
	signaling.disconnect();
}

TEST(SignalingStateTest, ExternalDestructionWaitsForSocketCallbackAndJoinsWorker)
{
	auto signaling = std::make_unique<VDONinjaSignaling>();
	signaling->setAutoReconnect(false, 0);
	ASSERT_TRUE(signaling->connect("wss://unit.test"));
	VDONinjaSignaling *rawSignaling = signaling.get();
	std::mutex callbackMutex;
	std::condition_variable callbackCv;
	bool callbackEntered = false;
	bool releaseCallback = false;
	std::atomic<bool> destructionReturned{false};

	std::thread callbackThread([&]() {
		rawSignaling->invokeSocketUserCallbackForTesting([&]() {
			std::unique_lock<std::mutex> lock(callbackMutex);
			callbackEntered = true;
			callbackCv.notify_all();
			callbackCv.wait(lock, [&]() { return releaseCallback; });
		});
	});
	bool enteredBeforeTimeout = false;
	{
		std::unique_lock<std::mutex> lock(callbackMutex);
		enteredBeforeTimeout = callbackCv.wait_for(lock, std::chrono::seconds(2), [&]() { return callbackEntered; });
	}
	EXPECT_TRUE(enteredBeforeTimeout);
	if (!enteredBeforeTimeout) {
		{
			std::lock_guard<std::mutex> lock(callbackMutex);
			releaseCallback = true;
		}
		callbackCv.notify_all();
		callbackThread.join();
		return;
	}
	std::thread destructionThread([&]() {
		signaling.reset();
		destructionReturned.store(true);
	});
	std::this_thread::sleep_for(std::chrono::milliseconds(100));
	EXPECT_FALSE(destructionReturned.load());
	{
		std::lock_guard<std::mutex> lock(callbackMutex);
		releaseCallback = true;
	}
	callbackCv.notify_all();

	callbackThread.join();
	destructionThread.join();
	EXPECT_TRUE(destructionReturned.load());
}

TEST(SignalingStateTest, IncrementalRoomStreamEventsUpdateMemberSnapshot)
{
	VDONinjaSignaling signaling;
	std::vector<std::pair<std::string, std::string>> addedStreams;
	std::vector<std::pair<std::string, std::string>> removedStreams;

	signaling.setOnStreamAdded(
	    [&](const std::string &streamId, const std::string &uuid) { addedStreams.emplace_back(streamId, uuid); });
	signaling.setOnStreamRemoved(
	    [&](const std::string &streamId, const std::string &uuid) { removedStreams.emplace_back(streamId, uuid); });

	signaling.processIncomingMessage(R"({"request":"listing","list":[{"UUID":"peer-1","streamID":"cam_1"}]})");
	EXPECT_TRUE(signaling.isInRoom());
	EXPECT_THAT(signaling.getCurrentRoomMembers(), ElementsAre("cam_1"));

	signaling.processIncomingMessage(R"({"request":"someonejoined","UUID":"peer-2","streamID":"cam_2"})");
	EXPECT_THAT(addedStreams, ElementsAre(Pair("cam_2", "peer-2")));
	EXPECT_THAT(signaling.getCurrentRoomMembers(), ElementsAre("cam_1", "cam_2"));

	signaling.processIncomingMessage(R"({"request":"videoRemovedFromRoom","UUID":"peer-2","streamID":"cam_2"})");
	EXPECT_THAT(removedStreams, ElementsAre(Pair("cam_2", "peer-2")));
	EXPECT_THAT(signaling.getCurrentRoomMembers(), ElementsAre("cam_1"));
}

TEST(SignalingStateTest, OfficialIceRestartRequestDispatchesIceRestartRequest)
{
	VDONinjaSignaling signaling;
	std::string seenUuid;
	std::string seenSession;

	signaling.setOnIceRestartRequest([&](const std::string &uuid, const std::string &session) {
		seenUuid = uuid;
		seenSession = session;
	});

	signaling.processIncomingMessage(R"({"iceRestartRequest":true,"UUID":"viewer-3","session":"sess-3"})");

	EXPECT_EQ(seenUuid, "viewer-3");
	EXPECT_EQ(seenSession, "sess-3");
}

TEST(SignalingStateTest, NaturalTransportCloseNotifiesDisconnectedExactlyOnce)
{
	VDONinjaSignaling signaling;
	std::atomic<int> connectedCount{0};
	std::atomic<int> disconnectedCount{0};

	signaling.setAutoReconnect(false, 0);
	signaling.setOnConnected([&]() { connectedCount.fetch_add(1); });
	signaling.setOnDisconnected([&]() { disconnectedCount.fetch_add(1); });

	ASSERT_TRUE(signaling.connect("wss://unit.test"));
	EXPECT_EQ(connectedCount.load(), 1);
	ASSERT_TRUE(rtc::WebSocket::simulateRemoteClose());

	for (int attempt = 0; attempt < 50 && disconnectedCount.load() == 0; ++attempt) {
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
	}

	EXPECT_FALSE(signaling.isConnected());
	EXPECT_EQ(disconnectedCount.load(), 1);
	signaling.disconnect();
	EXPECT_EQ(disconnectedCount.load(), 1);
}

TEST(SignalingStateTest, EncryptionFailureDoesNotSendPlaintextOverDataChannel)
{
	VDONinjaSignaling signaling;
	std::string error;
	signaling.setOnError([&](const std::string &message) { error = message; });
	ASSERT_TRUE(signaling.connect("wss://unit.test"));
	ASSERT_TRUE(signaling.publishStream("stream-1", "secret"));

	auto channel = std::make_shared<rtc::DataChannel>();
	VDONinjaSignaling::setEncryptionFailureForTesting(true);
	signaling.sendAnswerViaDataChannel(channel, "peer-1", "v=0\r\n", "session-1");
	VDONinjaSignaling::setEncryptionFailureForTesting(false);

	EXPECT_TRUE(channel->lastMessage().empty());
	EXPECT_EQ(error, "Failed to encrypt answer SDP for datachannel");
	signaling.disconnect();
}

TEST(SignalingStateTest, OfferAndIceEncryptionFailuresAreReportedAndFailClosed)
{
	VDONinjaSignaling signaling;
	std::vector<std::string> errors;
	signaling.setOnError([&](const std::string &message) { errors.push_back(message); });
	ASSERT_TRUE(signaling.connect("wss://unit.test"));
	ASSERT_TRUE(signaling.publishStream("stream-1", "secret"));

	auto channel = std::make_shared<rtc::DataChannel>();
	VDONinjaSignaling::setEncryptionFailureForTesting(true);
	signaling.sendOffer("peer-1", "v=0\r\n", "session-1");
	signaling.sendIceCandidate("peer-1", "candidate:1 1 udp 1 127.0.0.1 5000 typ host", "video", "session-1");
	const bool sentViaDataChannel = signaling.sendIceCandidateViaDataChannel(
	    channel, "peer-1", "candidate:1 1 udp 1 127.0.0.1 5000 typ host", "video", "session-1");
	VDONinjaSignaling::setEncryptionFailureForTesting(false);

	EXPECT_FALSE(sentViaDataChannel);
	EXPECT_TRUE(channel->lastMessage().empty());
	EXPECT_THAT(errors, ElementsAre("Failed to encrypt offer SDP", "Failed to encrypt ICE candidate",
	                                "Failed to encrypt ICE candidate for datachannel"));
	signaling.disconnect();
}
