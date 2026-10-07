#include "vdoninja-update-checker.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSettings>
#include <array>
#include <optional>

#include <curl/curl.h>

namespace vdoninja
{
namespace
{
constexpr qint64 kCheckIntervalSeconds = 24 * 60 * 60;
constexpr qint64 kMaxResponseBytes = 1024 * 1024;

// OBS ships libcurl, but its Qt bundle does not necessarily include a TLS backend.
// Drive curl's nonblocking multi API from the event loop; no worker can outlive the module.
class CurlReleaseRequest : public PluginReleaseRequest
{
public:
	explicit CurlReleaseRequest(QObject *parent) : PluginReleaseRequest(parent)
	{
		initialized_ = curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK;
		pollTimer_.setInterval(50);
		connect(&pollTimer_, &QTimer::timeout, this, &CurlReleaseRequest::poll);
	}

	~CurlReleaseRequest() override
	{
		cancel();
		if (initialized_)
			curl_global_cleanup();
	}

	void start(const QString &installedVersion) override
	{
		cancel();
		body_.clear();
		// Synchronous DNS in an unusual custom curl build must never stall the OBS UI.
		if (!initialized_ || !(curl_version_info(CURLVERSION_NOW)->features & CURL_VERSION_ASYNCHDNS)) {
			emit finished(0, {});
			return;
		}
		easy_ = curl_easy_init();
		multi_ = curl_multi_init();
		if (!easy_ || !multi_) {
			cancel();
			emit finished(0, {});
			return;
		}
		headers_ = curl_slist_append(nullptr, "Accept: application/vnd.github+json");
		if (headers_) {
			auto *headers = curl_slist_append(headers_, "X-GitHub-Api-Version: 2022-11-28");
			if (!headers) {
				cancel();
				emit finished(0, {});
				return;
			}
			headers_ = headers;
		}
		const auto agent = "obs-vdoninja/" + installedVersion.toUtf8();
		if (!headers_ || curl_easy_setopt(easy_, CURLOPT_URL, kPluginLatestReleaseApi) != CURLE_OK ||
		    curl_easy_setopt(easy_, CURLOPT_USERAGENT, agent.constData()) != CURLE_OK ||
		    curl_easy_setopt(easy_, CURLOPT_HTTPHEADER, headers_) != CURLE_OK ||
		    curl_easy_setopt(easy_, CURLOPT_NOSIGNAL, 1L) != CURLE_OK ||
		    curl_easy_setopt(easy_, CURLOPT_CONNECTTIMEOUT_MS, 10000L) != CURLE_OK ||
		    curl_easy_setopt(easy_, CURLOPT_TIMEOUT_MS, 15000L) != CURLE_OK ||
		    curl_easy_setopt(easy_, CURLOPT_WRITEFUNCTION, &CurlReleaseRequest::receive) != CURLE_OK ||
		    curl_easy_setopt(easy_, CURLOPT_WRITEDATA, this) != CURLE_OK ||
		    curl_multi_add_handle(multi_, easy_) != CURLM_OK) {
			cancel();
			emit finished(0, {});
			return;
		}
		pollTimer_.start();
	}

	void cancel() override
	{
		pollTimer_.stop();
		if (multi_ && easy_)
			curl_multi_remove_handle(multi_, easy_);
		if (easy_)
			curl_easy_cleanup(easy_);
		if (multi_)
			curl_multi_cleanup(multi_);
		curl_slist_free_all(headers_);
		easy_ = nullptr;
		multi_ = nullptr;
		headers_ = nullptr;
	}

private:
	static size_t receive(char *data, size_t size, size_t count, void *context)
	{
		auto *self = static_cast<CurlReleaseRequest *>(context);
		const auto remaining = static_cast<size_t>(kMaxResponseBytes - self->body_.size());
		if (size && count > remaining / size)
			return 0;
		const size_t bytes = size * count;
		self->body_.append(data, static_cast<int>(bytes));
		return bytes;
	}

	void poll()
	{
		int running = 0;
		const auto result = curl_multi_perform(multi_, &running);
		if (result == CURLM_OK && running)
			return;
		long status = 0;
		int messages = 0;
		if (result == CURLM_OK) {
			while (auto *message = curl_multi_info_read(multi_, &messages)) {
				if (message->msg == CURLMSG_DONE && message->data.result == CURLE_OK)
					curl_easy_getinfo(easy_, CURLINFO_RESPONSE_CODE, &status);
			}
		}
		cancel();
		emit finished(static_cast<int>(status), body_);
	}

	bool initialized_ = false;
	CURL *easy_ = nullptr;
	CURLM *multi_ = nullptr;
	curl_slist *headers_ = nullptr;
	QTimer pollTimer_;
	QByteArray body_;
};

struct Version {
	std::array<qulonglong, 3> parts;
	bool prerelease = false;
};

std::optional<Version> parseVersion(const QString &text)
{
	static const QRegularExpression pattern(
	    R"(^v?(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(?:-([0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*))?(?:\+([0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*))?$)");
	const auto match = pattern.match(text);
	if (!match.hasMatch() || match.capturedLength() != text.size())
		return std::nullopt;

	Version version;
	for (int i = 0; i < 3; ++i) {
		bool ok = false;
		version.parts[i] = match.captured(i + 1).toULongLong(&ok);
		if (!ok)
			return std::nullopt;
	}
	version.prerelease = !match.captured(4).isEmpty();
	for (const auto &identifier : match.captured(4).split('.')) {
		if (identifier.size() > 1 && identifier.startsWith('0') &&
		    identifier.indexOf(QRegularExpression("[^0-9]")) == -1)
			return std::nullopt;
	}
	return version;
}

qint64 secondsUntilCheck(qint64 lastAttempt)
{
	const qint64 now = QDateTime::currentSecsSinceEpoch();
	// A clock correction or corrupt cache must not suppress checks indefinitely.
	if (lastAttempt <= 0 || lastAttempt > now || now - lastAttempt >= kCheckIntervalSeconds)
		return 0;
	return kCheckIntervalSeconds - (now - lastAttempt);
}
} // namespace

UpdateCheckResult evaluatePluginRelease(const QByteArray &json, const QString &installedVersion)
{
	const auto document = QJsonDocument::fromJson(json);
	if (!document.isObject())
		return {};
	const auto release = document.object();
	if (!release["draft"].isBool() || release["draft"].toBool() || !release["prerelease"].isBool() ||
	    release["prerelease"].toBool() || !release["tag_name"].isString())
		return {};

	const auto tag = release["tag_name"].toString();
	const auto latest = parseVersion(tag);
	const auto installed = parseVersion(installedVersion);
	// Also reject prerelease tags if a test release was accidentally marked stable.
	if (!latest || latest->prerelease || !installed)
		return {};
	const bool newer = latest->parts > installed->parts || (latest->parts == installed->parts && installed->prerelease);
	return {newer ? UpdateCheckStatus::UpdateAvailable : UpdateCheckStatus::UpToDate,
	        tag.startsWith('v') ? tag.mid(1) : tag};
}

VDONinjaUpdateChecker::VDONinjaUpdateChecker(const QString &installedVersion, const QString &cachePath, QObject *parent,
                                             PluginReleaseRequest *request)
    : QObject(parent), installedVersion_(installedVersion), cachePath_(cachePath),
      request_(request ? request : new CurlReleaseRequest(this))
{
	if (!cachePath_.isEmpty()) {
		QSettings cache(cachePath_, QSettings::IniFormat);
		lastAttempt_ = cache.value("LastAttempt", 0).toLongLong();
		if (secondsUntilCheck(lastAttempt_) > 0)
			result_ = evaluatePluginRelease(cache.value("Release").toByteArray(), installedVersion_);
	}
	if (secondsUntilCheck(lastAttempt_) == 0)
		result_.status = UpdateCheckStatus::Checking;

	timer_.setSingleShot(true);
	connect(&timer_, &QTimer::timeout, this, &VDONinjaUpdateChecker::checkForUpdates);
	requestTimeout_.setSingleShot(true);
	connect(&requestTimeout_, &QTimer::timeout, this, [this] {
		request_->cancel();
		finishCheck(0, {});
	});
	connect(request_, &PluginReleaseRequest::finished, this, &VDONinjaUpdateChecker::finishCheck);
	// Give OBS time to finish startup; all requests then run asynchronously.
	timer_.start(static_cast<int>(secondsUntilCheck(lastAttempt_) * 1000 + 5000));
}

VDONinjaUpdateChecker::~VDONinjaUpdateChecker()
{
	shutdown();
}

void VDONinjaUpdateChecker::checkForUpdates()
{
	if (stopped_ || pending_)
		return;
	const auto remaining = secondsUntilCheck(lastAttempt_);
	if (remaining > 0) {
		timer_.start(static_cast<int>(remaining * 1000));
		return;
	}

	lastAttempt_ = QDateTime::currentSecsSinceEpoch();
	// Persist attempts too, so offline restarts and rate limits cannot cause a request loop.
	saveCache({});
	result_ = {UpdateCheckStatus::Checking, {}};
	emit statusChanged();
	pending_ = true;
	requestTimeout_.start(15000);
	request_->start(installedVersion_);
}

void VDONinjaUpdateChecker::finishCheck(int httpStatus, const QByteArray &body)
{
	if (stopped_ || !pending_)
		return;
	pending_ = false;
	requestTimeout_.stop();
	QByteArray release;
	result_ = {};
	if (httpStatus == 200 && body.size() <= kMaxResponseBytes) {
		result_ = evaluatePluginRelease(body, installedVersion_);
		if (result_.status != UpdateCheckStatus::Unavailable) {
			// Store only the metadata needed for the next launch, not release notes or asset URLs.
			QJsonObject metadata;
			metadata["tag_name"] = result_.latestVersion;
			metadata["draft"] = false;
			metadata["prerelease"] = false;
			release = QJsonDocument(metadata).toJson(QJsonDocument::Compact);
		}
	}
	saveCache(release);
	timer_.start(static_cast<int>(kCheckIntervalSeconds * 1000));
	emit statusChanged();
}

void VDONinjaUpdateChecker::saveCache(const QByteArray &release)
{
	if (cachePath_.isEmpty() || !QDir().mkpath(QFileInfo(cachePath_).absolutePath()))
		return;
	QSettings cache(cachePath_, QSettings::IniFormat);
	cache.setValue("LastAttempt", lastAttempt_);
	cache.setValue("Release", release);
}

void VDONinjaUpdateChecker::shutdown()
{
	stopped_ = true;
	timer_.stop();
	requestTimeout_.stop();
	request_->cancel();
	pending_ = false;
}

} // namespace vdoninja
