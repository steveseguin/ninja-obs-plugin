/*
 * OBS VDO.Ninja Plugin
 * Pure state helpers for auto-inbound room listing reconciliation
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace vdoninja
{

struct AutoInboundListingDelta {
	std::vector<std::string> added;
	std::vector<std::string> removed;
};

inline AutoInboundListingDelta reconcileAutoInboundListing(const std::set<std::string> &managedStreamIds,
                                                           const std::set<std::string> &ownStreamIds,
                                                           const std::vector<std::string> &listedStreamIds)
{
	std::set<std::string> listed;
	for (const auto &streamId : listedStreamIds) {
		if (!streamId.empty() && ownStreamIds.find(streamId) == ownStreamIds.end()) {
			listed.insert(streamId);
		}
	}

	AutoInboundListingDelta delta;
	for (const auto &streamId : listed) {
		if (managedStreamIds.find(streamId) == managedStreamIds.end()) {
			delta.added.push_back(streamId);
		}
	}
	for (const auto &streamId : managedStreamIds) {
		if (listed.find(streamId) == listed.end()) {
			delta.removed.push_back(streamId);
		}
	}
	return delta;
}

class AutoInboundRemovalGraceState
{
public:
	explicit AutoInboundRemovalGraceState(int64_t graceMs = 10000) : graceMs_(graceMs > 0 ? graceMs : 1) {}

	void schedule(const std::string &streamId, int64_t nowMs)
	{
		if (!streamId.empty()) {
			deadlinesByStream_.emplace(streamId, nowMs + graceMs_);
		}
	}

	void cancel(const std::string &streamId) { deadlinesByStream_.erase(streamId); }
	void clear() { deadlinesByStream_.clear(); }

	int64_t nextDeadlineMs() const
	{
		int64_t nextDeadline = 0;
		for (const auto &entry : deadlinesByStream_) {
			if (nextDeadline == 0 || entry.second < nextDeadline) {
				nextDeadline = entry.second;
			}
		}
		return nextDeadline;
	}

	std::vector<std::string> takeDue(int64_t nowMs)
	{
		std::vector<std::string> due;
		auto it = deadlinesByStream_.begin();
		while (it != deadlinesByStream_.end()) {
			if (it->second <= nowMs) {
				due.push_back(it->first);
				it = deadlinesByStream_.erase(it);
			} else {
				++it;
			}
		}
		return due;
	}

	bool contains(const std::string &streamId) const
	{
		return deadlinesByStream_.find(streamId) != deadlinesByStream_.end();
	}

private:
	int64_t graceMs_;
	std::map<std::string, int64_t> deadlinesByStream_;
};

} // namespace vdoninja
