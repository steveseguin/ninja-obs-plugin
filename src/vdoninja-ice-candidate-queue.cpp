/*
 * OBS VDO.Ninja Plugin
 * Bounded queue for remote ICE candidates that arrive before their peer/SDP
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#include "vdoninja-ice-candidate-queue.h"

#include <algorithm>
#include <utility>

namespace vdoninja
{

PendingRemoteIceCandidateQueue::PendingRemoteIceCandidateQueue(size_t maxCandidatesPerPeer, int64_t ttlMs,
                                                               size_t maxPeerQueues, size_t maxQueuedBytes,
                                                               size_t maxCandidateBytes)
    : maxCandidatesPerPeer_(std::max<size_t>(1, maxCandidatesPerPeer)), ttlMs_(std::max<int64_t>(1, ttlMs)),
      maxPeerQueues_(std::max<size_t>(1, maxPeerQueues)), maxQueuedBytes_(std::max<size_t>(1, maxQueuedBytes)),
      maxCandidateBytes_(std::max<size_t>(1, maxCandidateBytes))
{
}

PendingRemoteIceCandidatePushResult PendingRemoteIceCandidateQueue::push(const std::string &uuid,
                                                                         PendingRemoteIceCandidate candidate)
{
	if (uuid.empty() || candidate.candidate.empty()) {
		return {};
	}

	const size_t candidateBytes = storedBytes(uuid, candidate);
	if (candidate.candidate.size() > maxCandidateBytes_ || candidateBytes > maxQueuedBytes_) {
		return {};
	}

	std::lock_guard<std::mutex> lock(mutex_);
	pruneExpiredLocked(candidate.queuedAtMs);

	bool droppedQueuedData = false;
	auto peerIt = candidatesByPeer_.find(uuid);
	if (peerIt != candidatesByPeer_.end() && peerIt->second.size() >= maxCandidatesPerPeer_) {
		queuedBytes_ -= storedBytes(uuid, peerIt->second.front());
		peerIt->second.pop_front();
		droppedQueuedData = true;
	}

	while (!candidatesByPeer_.empty() && queuedBytes_ + candidateBytes > maxQueuedBytes_) {
		evictOldestCandidateLocked();
		droppedQueuedData = true;
	}

	peerIt = candidatesByPeer_.find(uuid);
	while (peerIt == candidatesByPeer_.end() && candidatesByPeer_.size() >= maxPeerQueues_) {
		evictOldestPeerLocked();
		droppedQueuedData = true;
		peerIt = candidatesByPeer_.find(uuid);
	}

	auto &candidates = candidatesByPeer_[uuid];
	candidates.push_back(std::move(candidate));
	queuedBytes_ += candidateBytes;
	return {true, droppedQueuedData};
}

std::vector<PendingRemoteIceCandidate>
PendingRemoteIceCandidateQueue::takeCompatible(const std::string &uuid, const std::string &session, int64_t nowMs)
{
	std::vector<PendingRemoteIceCandidate> result;
	std::lock_guard<std::mutex> lock(mutex_);
	pruneExpiredLocked(nowMs);

	auto peerIt = candidatesByPeer_.find(uuid);
	if (peerIt == candidatesByPeer_.end()) {
		return result;
	}

	auto &candidates = peerIt->second;
	auto it = candidates.begin();
	while (it != candidates.end()) {
		const bool compatible = session.empty() || it->session.empty() || it->session == session;
		if (!compatible) {
			++it;
			continue;
		}
		queuedBytes_ -= storedBytes(uuid, *it);
		result.push_back(std::move(*it));
		it = candidates.erase(it);
	}

	if (candidates.empty()) {
		candidatesByPeer_.erase(peerIt);
	}
	return result;
}

void PendingRemoteIceCandidateQueue::erase(const std::string &uuid)
{
	std::lock_guard<std::mutex> lock(mutex_);
	auto peerIt = candidatesByPeer_.find(uuid);
	if (peerIt != candidatesByPeer_.end()) {
		erasePeerLocked(peerIt);
	}
}

void PendingRemoteIceCandidateQueue::clear()
{
	std::lock_guard<std::mutex> lock(mutex_);
	candidatesByPeer_.clear();
	queuedBytes_ = 0;
}

size_t PendingRemoteIceCandidateQueue::size(const std::string &uuid) const
{
	std::lock_guard<std::mutex> lock(mutex_);
	const auto it = candidatesByPeer_.find(uuid);
	return it == candidatesByPeer_.end() ? 0 : it->second.size();
}

size_t PendingRemoteIceCandidateQueue::peerCount() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return candidatesByPeer_.size();
}

size_t PendingRemoteIceCandidateQueue::queuedBytes() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return queuedBytes_;
}

size_t PendingRemoteIceCandidateQueue::storedBytes(const std::string &uuid, const PendingRemoteIceCandidate &candidate)
{
	return uuid.size() + candidate.candidate.size() + candidate.mid.size() + candidate.session.size();
}

void PendingRemoteIceCandidateQueue::erasePeerLocked(
    std::map<std::string, std::deque<PendingRemoteIceCandidate>>::iterator peerIt)
{
	for (const auto &candidate : peerIt->second) {
		queuedBytes_ -= storedBytes(peerIt->first, candidate);
	}
	candidatesByPeer_.erase(peerIt);
}

void PendingRemoteIceCandidateQueue::evictOldestCandidateLocked()
{
	auto oldestPeerIt = candidatesByPeer_.end();
	for (auto peerIt = candidatesByPeer_.begin(); peerIt != candidatesByPeer_.end(); ++peerIt) {
		if (peerIt->second.empty()) {
			continue;
		}
		if (oldestPeerIt == candidatesByPeer_.end() ||
		    peerIt->second.front().queuedAtMs < oldestPeerIt->second.front().queuedAtMs) {
			oldestPeerIt = peerIt;
		}
	}

	if (oldestPeerIt == candidatesByPeer_.end()) {
		return;
	}

	queuedBytes_ -= storedBytes(oldestPeerIt->first, oldestPeerIt->second.front());
	oldestPeerIt->second.pop_front();
	if (oldestPeerIt->second.empty()) {
		candidatesByPeer_.erase(oldestPeerIt);
	}
}

void PendingRemoteIceCandidateQueue::evictOldestPeerLocked()
{
	auto oldestPeerIt = candidatesByPeer_.end();
	for (auto peerIt = candidatesByPeer_.begin(); peerIt != candidatesByPeer_.end(); ++peerIt) {
		if (peerIt->second.empty()) {
			continue;
		}
		if (oldestPeerIt == candidatesByPeer_.end() ||
		    peerIt->second.front().queuedAtMs < oldestPeerIt->second.front().queuedAtMs) {
			oldestPeerIt = peerIt;
		}
	}

	if (oldestPeerIt != candidatesByPeer_.end()) {
		erasePeerLocked(oldestPeerIt);
	}
}

void PendingRemoteIceCandidateQueue::pruneExpiredLocked(int64_t nowMs)
{
	auto peerIt = candidatesByPeer_.begin();
	while (peerIt != candidatesByPeer_.end()) {
		auto &candidates = peerIt->second;
		while (!candidates.empty() && nowMs >= candidates.front().queuedAtMs &&
		       nowMs - candidates.front().queuedAtMs > ttlMs_) {
			queuedBytes_ -= storedBytes(peerIt->first, candidates.front());
			candidates.pop_front();
		}
		if (candidates.empty()) {
			peerIt = candidatesByPeer_.erase(peerIt);
		} else {
			++peerIt;
		}
	}
}

} // namespace vdoninja
