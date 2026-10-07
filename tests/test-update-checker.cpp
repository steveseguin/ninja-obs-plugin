#include <QCoreApplication>
#include <QDateTime>
#include <QEventLoop>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QTemporaryDir>

#include <gtest/gtest.h>

#include "vdoninja-update-checker.h"

using namespace vdoninja;

namespace
{
QByteArray releaseJson(const QString &tag, bool prerelease = false, bool draft = false)
{
	return QJsonDocument(QJsonObject{{"tag_name", tag}, {"prerelease", prerelease}, {"draft", draft}})
	    .toJson(QJsonDocument::Compact);
}

class FakeNetwork : public PluginReleaseRequest
{
public:
	int requests = 0;
	int httpStatus = 200;
	bool complete = true;
	bool canceled = false;
	QByteArray payload = releaseJson("v1.2.0");
	QString installedVersion;

	void start(const QString &version) override
	{
		++requests;
		installedVersion = version;
		canceled = false;
		if (complete) {
			QTimer::singleShot(0, this, [this] {
				if (!canceled)
					emit finished(httpStatus, payload);
			});
		}
	}
	void cancel() override { canceled = true; }
};

class UpdateCheckerTest : public ::testing::Test
{
protected:
	static void SetUpTestSuite()
	{
		if (QCoreApplication::instance())
			return;
		static int argc = 1;
		static char name[] = "vdoninja-update-check-tests";
		static char *argv[] = {name, nullptr};
		static QCoreApplication app(argc, argv);
	}

	QString cachePath() const { return directory_.filePath("update-check.ini"); }
	void seedCache(qint64 timestamp, const QByteArray &release)
	{
		QSettings cache(cachePath(), QSettings::IniFormat);
		cache.setValue("LastAttempt", timestamp);
		cache.setValue("Release", release);
	}
	void waitForResult(VDONinjaUpdateChecker &checker, int timeoutMs = 1000)
	{
		if (checker.result().status != UpdateCheckStatus::Checking)
			return;
		QEventLoop loop;
		QObject::connect(&checker, &VDONinjaUpdateChecker::statusChanged, &loop, [&] {
			if (checker.result().status != UpdateCheckStatus::Checking)
				loop.quit();
		});
		QTimer::singleShot(timeoutMs, &loop, &QEventLoop::quit);
		loop.exec();
		EXPECT_NE(checker.result().status, UpdateCheckStatus::Checking);
	}

	QTemporaryDir directory_;
	FakeNetwork network_;
};
} // namespace

TEST(PluginRelease, ComparesNumericVersionsInsteadOfStrings)
{
	for (const auto *tag : {"v1.1.10", "1.2.0", "v2.0.0", "v10.0.0"}) {
		SCOPED_TRACE(tag);
		EXPECT_EQ(evaluatePluginRelease(releaseJson(tag), "1.1.9").status, UpdateCheckStatus::UpdateAvailable);
	}
}

TEST(PluginRelease, EqualOrOlderStableReleasesAreUpToDate)
{
	for (const auto *tag : {"1.1.73", "v1.1.73", "v1.1.9", "v1.0.999", "v0.99.99"}) {
		SCOPED_TRACE(tag);
		EXPECT_EQ(evaluatePluginRelease(releaseJson(tag), "1.1.73").status, UpdateCheckStatus::UpToDate);
	}
}

TEST(PluginRelease, IgnoresBuildMetadata)
{
	EXPECT_EQ(evaluatePluginRelease(releaseJson("v1.1.73+build.42"), "1.1.73+local").status,
	          UpdateCheckStatus::UpToDate);
}

TEST(PluginRelease, StableReleaseCanReplaceInstalledPrereleaseWithoutDowngrading)
{
	EXPECT_EQ(evaluatePluginRelease(releaseJson("v1.2.0"), "1.2.0-rc.1").status, UpdateCheckStatus::UpdateAvailable);
	EXPECT_EQ(evaluatePluginRelease(releaseJson("v1.1.73"), "1.2.0-beta.1").status, UpdateCheckStatus::UpToDate);
}

TEST(PluginRelease, RejectsPrereleaseAndDraftFlagsEvenWithStableTags)
{
	EXPECT_EQ(evaluatePluginRelease(releaseJson("v9.0.0", true), "1.1.73").status, UpdateCheckStatus::Unavailable);
	EXPECT_EQ(evaluatePluginRelease(releaseJson("v9.0.0", false, true), "1.1.73").status,
	          UpdateCheckStatus::Unavailable);
}

TEST(PluginRelease, RejectsPrereleaseTagsEvenIfMarkedStable)
{
	for (const auto *tag : {"v9.0.0-alpha", "v9.0.0-beta.1", "v9.0.0-rc.1", "v9.0.0-test+build"}) {
		SCOPED_TRACE(tag);
		EXPECT_EQ(evaluatePluginRelease(releaseJson(tag), "1.1.73").status, UpdateCheckStatus::Unavailable);
	}
}

TEST(PluginRelease, RejectsMalformedAndIncompleteResponses)
{
	for (const auto *json :
	     {"", "not json", "[]", "null", "{}", R"({"message":"API rate limit exceeded"})", R"({"tag_name":"v9.0.0"})",
	      R"({"tag_name":"v9.0.0","draft":false})", R"({"tag_name":"v9.0.0","draft":false,"prerelease":"false"})",
	      R"({"tag_name":900,"draft":false,"prerelease":false})"}) {
		SCOPED_TRACE(json);
		EXPECT_EQ(evaluatePluginRelease(json, "1.1.73").status, UpdateCheckStatus::Unavailable);
	}
}

TEST(PluginRelease, RejectsMalformedVersionsAndOverflow)
{
	for (const auto *tag : {"latest", "v1.2", "1.2.3.4", "01.2.3", "1.02.3", "1.2.03", "-1.2.3", "1.2.3junk", " 1.2.3",
	                        "1.2.3\n", "1.2.3+", "1.2.3+<b>", "1.2.3-rc..1", "18446744073709551616.2.3"}) {
		SCOPED_TRACE(tag);
		EXPECT_EQ(evaluatePluginRelease(releaseJson(tag), "1.1.73").status, UpdateCheckStatus::Unavailable);
		EXPECT_EQ(evaluatePluginRelease(releaseJson("v9.0.0"), tag).status, UpdateCheckStatus::Unavailable);
	}
	EXPECT_EQ(evaluatePluginRelease(releaseJson("v9.0.0"), "1.2.3-01").status, UpdateCheckStatus::Unavailable);
}

TEST_F(UpdateCheckerTest, ChecksAsynchronouslyAndCachesResult)
{
	VDONinjaUpdateChecker checker("1.1.73", cachePath(), nullptr, &network_);
	EXPECT_EQ(network_.requests, 0);
	checker.checkForUpdates();
	EXPECT_EQ(checker.result().status, UpdateCheckStatus::Checking);
	EXPECT_EQ(network_.requests, 1);
	EXPECT_EQ(network_.installedVersion, "1.1.73");
	waitForResult(checker);
	EXPECT_EQ(checker.result().status, UpdateCheckStatus::UpdateAvailable);
	EXPECT_EQ(checker.result().latestVersion, "1.2.0");

	checker.checkForUpdates();
	VDONinjaUpdateChecker restarted("1.1.73", cachePath(), nullptr, &network_);
	restarted.checkForUpdates();
	EXPECT_EQ(restarted.result().status, UpdateCheckStatus::UpdateAvailable);
	EXPECT_EQ(network_.requests, 1);
}

TEST_F(UpdateCheckerTest, StartsAutomaticallyAfterStartupDelay)
{
	VDONinjaUpdateChecker checker("1.1.73", cachePath(), nullptr, &network_);
	waitForResult(checker, 7000);
	EXPECT_EQ(network_.requests, 1);
	EXPECT_EQ(checker.result().status, UpdateCheckStatus::UpdateAvailable);
}

TEST_F(UpdateCheckerTest, AutomaticallyRechecksWhenCachedResultExpires)
{
	seedCache(QDateTime::currentSecsSinceEpoch() - 86399, releaseJson("v1.1.73"));
	VDONinjaUpdateChecker checker("1.1.73", cachePath(), nullptr, &network_);
	QEventLoop loop;
	QObject::connect(&checker, &VDONinjaUpdateChecker::statusChanged, &loop, [&] {
		if (checker.result().status == UpdateCheckStatus::UpdateAvailable)
			loop.quit();
	});
	QTimer::singleShot(8000, &loop, &QEventLoop::quit);
	loop.exec();
	EXPECT_EQ(network_.requests, 1);
	EXPECT_EQ(checker.result().status, UpdateCheckStatus::UpdateAvailable);
}

TEST_F(UpdateCheckerTest, CachedReleaseIsComparedWithNewlyInstalledVersion)
{
	seedCache(QDateTime::currentSecsSinceEpoch(), releaseJson("v1.2.0"));
	VDONinjaUpdateChecker checker("1.2.0", cachePath(), nullptr, &network_);
	checker.checkForUpdates();
	EXPECT_EQ(checker.result().status, UpdateCheckStatus::UpToDate);
	EXPECT_EQ(network_.requests, 0);
}

TEST_F(UpdateCheckerTest, ExpiredCacheTriggersFreshRequest)
{
	seedCache(QDateTime::currentSecsSinceEpoch() - 86401, releaseJson("v1.1.73"));
	VDONinjaUpdateChecker checker("1.1.73", cachePath(), nullptr, &network_);
	EXPECT_EQ(checker.result().status, UpdateCheckStatus::Checking);
	checker.checkForUpdates();
	waitForResult(checker);
	EXPECT_EQ(checker.result().status, UpdateCheckStatus::UpdateAvailable);
	EXPECT_EQ(network_.requests, 1);
}

TEST_F(UpdateCheckerTest, FutureTimestampDoesNotSuppressChecks)
{
	seedCache(QDateTime::currentSecsSinceEpoch() + 86400, releaseJson("v1.1.73"));
	VDONinjaUpdateChecker checker("1.1.73", cachePath(), nullptr, &network_);
	checker.checkForUpdates();
	waitForResult(checker);
	EXPECT_EQ(network_.requests, 1);
}

TEST_F(UpdateCheckerTest, NetworkFailureIsUnavailableAndThrottledAcrossRestarts)
{
	network_.httpStatus = 0;
	VDONinjaUpdateChecker checker("1.1.73", cachePath(), nullptr, &network_);
	checker.checkForUpdates();
	waitForResult(checker);
	EXPECT_EQ(checker.result().status, UpdateCheckStatus::Unavailable);
	VDONinjaUpdateChecker restarted("1.1.73", cachePath(), nullptr, &network_);
	restarted.checkForUpdates();
	EXPECT_EQ(restarted.result().status, UpdateCheckStatus::Unavailable);
	EXPECT_EQ(network_.requests, 1);
}

TEST_F(UpdateCheckerTest, HttpErrorsCannotReportUpToDateEvenWithValidJson)
{
	for (int status : {301, 403, 404, 429, 500}) {
		network_.httpStatus = status;
		VDONinjaUpdateChecker checker("1.2.0", {}, nullptr, &network_);
		checker.checkForUpdates();
		waitForResult(checker);
		EXPECT_EQ(checker.result().status, UpdateCheckStatus::Unavailable);
	}
}

TEST_F(UpdateCheckerTest, FailedRefreshDoesNotReuseOldUpToDateStatus)
{
	seedCache(QDateTime::currentSecsSinceEpoch() - 86401, releaseJson("v1.1.73"));
	network_.payload = "invalid json";
	VDONinjaUpdateChecker checker("1.1.73", cachePath(), nullptr, &network_);
	checker.checkForUpdates();
	waitForResult(checker);
	EXPECT_EQ(checker.result().status, UpdateCheckStatus::Unavailable);
	VDONinjaUpdateChecker restarted("1.1.73", cachePath(), nullptr, &network_);
	EXPECT_EQ(restarted.result().status, UpdateCheckStatus::Unavailable);
}

TEST_F(UpdateCheckerTest, PrereleaseCannotBeCachedAsAnUpdate)
{
	network_.payload = releaseJson("v9.0.0", true);
	VDONinjaUpdateChecker checker("1.1.73", cachePath(), nullptr, &network_);
	checker.checkForUpdates();
	waitForResult(checker);
	EXPECT_EQ(checker.result().status, UpdateCheckStatus::Unavailable);
	VDONinjaUpdateChecker restarted("1.1.73", cachePath(), nullptr, &network_);
	EXPECT_EQ(restarted.result().status, UpdateCheckStatus::Unavailable);
}

TEST_F(UpdateCheckerTest, OversizedResponseIsAborted)
{
	network_.payload = QByteArray(1024 * 1024 + 1, ' ');
	VDONinjaUpdateChecker checker("1.1.73", {}, nullptr, &network_);
	checker.checkForUpdates();
	waitForResult(checker);
	EXPECT_EQ(checker.result().status, UpdateCheckStatus::Unavailable);
}

TEST_F(UpdateCheckerTest, ShutdownCancelsInFlightRequestWithoutNotifications)
{
	network_.complete = false;
	VDONinjaUpdateChecker checker("1.1.73", cachePath(), nullptr, &network_);
	checker.checkForUpdates();
	checker.checkForUpdates();
	EXPECT_EQ(network_.requests, 1);
	int notifications = 0;
	QObject::connect(&checker, &VDONinjaUpdateChecker::statusChanged, [&] { ++notifications; });
	checker.shutdown();
	EXPECT_TRUE(network_.canceled);
	checker.checkForUpdates();
	EXPECT_EQ(network_.requests, 1);
	EXPECT_EQ(notifications, 0);
}

TEST_F(UpdateCheckerTest, RequestHasAnAbsoluteTimeout)
{
	network_.complete = false;
	VDONinjaUpdateChecker checker("1.1.73", {}, nullptr, &network_);
	checker.checkForUpdates();
	waitForResult(checker, 17000);
	EXPECT_EQ(checker.result().status, UpdateCheckStatus::Unavailable);
}
