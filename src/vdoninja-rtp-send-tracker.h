/*
 * OBS VDO.Ninja Plugin
 * RTP send-result accounting
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <utility>

namespace vdoninja
{

struct RtpSendStats {
	uint64_t sentPackets = 0;
	uint64_t sendFailures = 0;
};

// Tracks both explicit false returns and exceptions from an RTP transport.
// libdatachannel can reject a send without throwing, so exception-only
// accounting silently misses real packet loss.
class RtpSendTracker
{
public:
	template <typename SendCallback> bool send(SendCallback &&callback)
	{
		try {
			const bool sent = static_cast<bool>(std::forward<SendCallback>(callback)());
			if (sent) {
				sentPackets_.fetch_add(1, std::memory_order_relaxed);
			} else {
				sendFailures_.fetch_add(1, std::memory_order_relaxed);
			}
			return sent;
		} catch (...) {
			sendFailures_.fetch_add(1, std::memory_order_relaxed);
			throw;
		}
	}

	RtpSendStats take()
	{
		return {
		    sentPackets_.exchange(0, std::memory_order_relaxed),
		    sendFailures_.exchange(0, std::memory_order_relaxed),
		};
	}

	void reset()
	{
		sentPackets_.store(0, std::memory_order_relaxed);
		sendFailures_.store(0, std::memory_order_relaxed);
	}

private:
	std::atomic<uint64_t> sentPackets_{0};
	std::atomic<uint64_t> sendFailures_{0};
};

} // namespace vdoninja
