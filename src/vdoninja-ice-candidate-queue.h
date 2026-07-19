/*
 * OBS VDO.Ninja Plugin
 * Bounded queue for remote ICE candidates that arrive before their peer/SDP
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace vdoninja
{

struct PendingRemoteIceCandidate {
	std::string candidate;
	std::string mid;
	std::string session;
	int64_t queuedAtMs = 0;
};

struct PendingRemoteIceCandidatePushResult {
	bool accepted = false;
	bool droppedQueuedData = false;
};

class PendingRemoteIceCandidateQueue
{
public:
	explicit PendingRemoteIceCandidateQueue(size_t maxCandidatesPerPeer = 100, int64_t ttlMs = 30000,
	                                        size_t maxPeerQueues = 256, size_t maxQueuedBytes = 2 * 1024 * 1024,
	                                        size_t maxCandidateBytes = 16 * 1024);

	PendingRemoteIceCandidatePushResult push(const std::string &uuid, PendingRemoteIceCandidate candidate);
	std::vector<PendingRemoteIceCandidate> takeCompatible(const std::string &uuid, const std::string &session,
	                                                      int64_t nowMs);
	void erase(const std::string &uuid);
	void clear();
	size_t size(const std::string &uuid) const;
	size_t peerCount() const;
	size_t queuedBytes() const;

private:
	static size_t storedBytes(const std::string &uuid, const PendingRemoteIceCandidate &candidate);
	void erasePeerLocked(std::map<std::string, std::deque<PendingRemoteIceCandidate>>::iterator peerIt);
	void evictOldestCandidateLocked();
	void evictOldestPeerLocked();
	void pruneExpiredLocked(int64_t nowMs);

	size_t maxCandidatesPerPeer_;
	int64_t ttlMs_;
	size_t maxPeerQueues_;
	size_t maxQueuedBytes_;
	size_t maxCandidateBytes_;
	size_t queuedBytes_ = 0;
	mutable std::mutex mutex_;
	std::map<std::string, std::deque<PendingRemoteIceCandidate>> candidatesByPeer_;
};

} // namespace vdoninja
