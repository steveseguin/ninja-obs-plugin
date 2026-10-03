/*
 * Match OBS's Windows timer resolution for standalone pacing tests.
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#include <Windows.h>
#include <gtest/gtest.h>
#include <mmsystem.h>

namespace
{

class WindowsTimerResolution : public testing::Environment
{
public:
	void SetUp() override
	{
		// OBS requests 1 ms resolution. Without that process-local request,
		// repeated short pacer waits can consume a repair's entire deadline.
		active_ = timeBeginPeriod(1) == TIMERR_NOERROR;
		ASSERT_TRUE(active_);
	}

	void TearDown() override
	{
		if (active_)
			timeEndPeriod(1);
	}

private:
	bool active_ = false;
};

[[maybe_unused]] testing::Environment *const timerEnvironment =
    testing::AddGlobalTestEnvironment(new WindowsTimerResolution);

} // namespace
