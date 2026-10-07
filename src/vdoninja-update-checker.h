#pragma once

#include <QObject>
#include <QString>
#include <QTimer>

namespace vdoninja
{

inline constexpr const char *kPluginReleasesUrl = "https://github.com/steveseguin/ninja-obs-plugin/releases";
inline constexpr const char *kPluginLatestReleaseApi =
    "https://api.github.com/repos/steveseguin/ninja-obs-plugin/releases/latest";

enum class UpdateCheckStatus { Checking, UpToDate, UpdateAvailable, Unavailable };

struct UpdateCheckResult {
	UpdateCheckStatus status = UpdateCheckStatus::Unavailable;
	QString latestVersion;
};

// Treat missing/malformed metadata as unknown, never as proof that we are up to date.
UpdateCheckResult evaluatePluginRelease(const QByteArray &json, const QString &installedVersion);

class PluginReleaseRequest : public QObject
{
	Q_OBJECT

public:
	using QObject::QObject;
	virtual void start(const QString &installedVersion) = 0;
	virtual void cancel() = 0;

signals:
	void finished(int httpStatus, const QByteArray &body);
};

class VDONinjaUpdateChecker : public QObject
{
	Q_OBJECT

public:
	VDONinjaUpdateChecker(const QString &installedVersion, const QString &cachePath, QObject *parent = nullptr,
	                      PluginReleaseRequest *request = nullptr);
	~VDONinjaUpdateChecker() override;
	const UpdateCheckResult &result() const { return result_; }
	void checkForUpdates();
	void shutdown();

signals:
	void statusChanged();

private:
	void finishCheck(int httpStatus, const QByteArray &body);
	void saveCache(const QByteArray &release);

	QString installedVersion_;
	QString cachePath_;
	PluginReleaseRequest *request_;
	QTimer timer_;
	QTimer requestTimeout_;
	qint64 lastAttempt_ = 0;
	bool pending_ = false;
	bool stopped_ = false;
	UpdateCheckResult result_;
};

} // namespace vdoninja
