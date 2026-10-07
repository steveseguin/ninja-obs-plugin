#include "vdoninja-dock.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QMessageBox>
#include <QScrollArea>
#include <QSizePolicy>
#include <QSpinBox>
#include <QToolButton>
#include <QVBoxLayout>
#include <cstring>
#include <optional>

#include <util/config-file.h>

#include "plugin-main.h"
#include "vdoninja-output.h"
#include "vdoninja-publish-obs.h"
#include "vdoninja-update-checker.h"
#include "vdoninja-utils.h"

namespace vdoninja
{

static const char *obs_module_text_vdo(const char *key)
{
	const char *text = obs_module_text(key);
	return (text && *text) ? text : key;
}

VDONinjaDock::VDONinjaDock(QWidget *parent) : QDockWidget(parent)
{
	setObjectName("VDONinjaStudioDock");
	setWindowTitle(obs_module_text_vdo("VDONinja.Studio.Title"));
	setAllowedAreas(Qt::AllDockWidgetAreas);

	setupUi();
	loadSettings();
	char *cachePath = obs_module_config_path("update-check.ini");
	updateChecker_ = new VDONinjaUpdateChecker(PLUGIN_VERSION, QString::fromUtf8(cachePath ? cachePath : ""), this);
	bfree(cachePath);
	connect(updateChecker_, &VDONinjaUpdateChecker::statusChanged, this, &VDONinjaDock::updateVersionStatus);
	updateVersionStatus();

	statsTimer = new QTimer(this);
	connect(statsTimer, &QTimer::timeout, this, &VDONinjaDock::updateStats);
	statsTimer->start(1000);

	chatClearTimer = new QTimer(this);
	chatClearTimer->setSingleShot(true);
	connect(chatClearTimer, &QTimer::timeout, this, [this]() {
		lblChat->clear();
		lblChat->setVisible(false);
	});
}

VDONinjaDock::~VDONinjaDock() {}

void VDONinjaDock::shutdown()
{
	if (updateChecker_)
		updateChecker_->shutdown();
	saveSettings();
	if (statsTimer) {
		statsTimer->stop();
	}
	if (chatClearTimer) {
		chatClearTimer->stop();
	}
}

void VDONinjaDock::setupUi()
{
	QScrollArea *scrollArea = new QScrollArea(this);
	scrollArea->setWidgetResizable(true);
	scrollArea->setFrameShape(QFrame::NoFrame);
	scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	scrollArea->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
	scrollArea->setMinimumSize(0, 0);
	scrollArea->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

	QWidget *container = new QWidget(scrollArea);
	QVBoxLayout *layout = new QVBoxLayout(container);
	layout->setContentsMargins(10, 10, 10, 10);
	layout->setSpacing(8);

	// Credentials Group
	QGroupBox *grpCreds = new QGroupBox(obs_module_text_vdo("VDONinja.Dock.SessionSetup"), container);
	QFormLayout *form = new QFormLayout(grpCreds);

	editStreamId = new QLineEdit(grpCreds);
	editRoomId = new QLineEdit(grpCreds);
	editPassword = new QLineEdit(grpCreds);
	editPassword->setEchoMode(QLineEdit::PasswordEchoOnEdit);

	spinMaxViewers = new QSpinBox(grpCreds);
	spinMaxViewers->setRange(1, 50);
	spinMaxViewers->setValue(10);
	spinMaxViewers->setMinimumWidth(60);
	spinMaxViewers->setToolTip(obs_module_text_vdo("MaxViewers.Description"));

	btnGenerateId = new QPushButton(obs_module_text_vdo("VDONinja.Dock.GenerateID"), grpCreds);
	connect(btnGenerateId, &QPushButton::clicked, this, &VDONinjaDock::onGenerateIdClicked);

	form->addRow(obs_module_text_vdo("StreamID"), editStreamId);
	form->addRow(obs_module_text_vdo("RoomID"), editRoomId);
	form->addRow(obs_module_text_vdo("Password"), editPassword);
	form->addRow(obs_module_text_vdo("VDONinja.Dock.MaxViewers"), spinMaxViewers);
	form->addRow("", btnGenerateId);

	for (auto *edit : {editStreamId, editRoomId, editPassword}) {
		connect(edit, &QLineEdit::textEdited, this, [this]() { editsPending_ = true; });
	}
	connect(editStreamId, &QLineEdit::editingFinished, this, &VDONinjaDock::onSettingsChanged);
	connect(editRoomId, &QLineEdit::editingFinished, this, &VDONinjaDock::onSettingsChanged);
	connect(editPassword, &QLineEdit::editingFinished, this, &VDONinjaDock::onSettingsChanged);
	connect(spinMaxViewers, QOverload<int>::of(&QSpinBox::valueChanged), this, &VDONinjaDock::onSettingsChanged);

	layout->addWidget(grpCreds);

	// Options Group
	QGroupBox *grpOptions = new QGroupBox(obs_module_text_vdo("VDONinja.Dock.Options"), container);
	QVBoxLayout *optLayout = new QVBoxLayout(grpOptions);

	chkAutoAddFeeds = new QCheckBox(obs_module_text_vdo("VDONinja.Dock.AutoAddFeeds"), grpOptions);
	chkAutoAddFeeds->setChecked(false);
	chkAutoAddFeeds->setToolTip(obs_module_text_vdo("VDONinja.Dock.AutoAddFeeds.Tooltip"));
	optLayout->addWidget(chkAutoAddFeeds);

	connect(chkAutoAddFeeds, &QCheckBox::toggled, this, &VDONinjaDock::onSettingsChanged);

	layout->addWidget(grpOptions);

	auto *advancedToggle = new QToolButton(container);
	advancedToggle->setText(obs_module_text_vdo("AdvancedSettings"));
	advancedToggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
	advancedToggle->setCheckable(true);
	advancedToggle->setArrowType(Qt::RightArrow);
	layout->addWidget(advancedToggle);
	auto *advanced = new QWidget(container);
	auto *advancedLayout = new QFormLayout(advanced);
	editUdpPorts = new QLineEdit(advanced);
	editUdpPorts->setObjectName("VDONinjaUdpPorts");
	editUdpPorts->setPlaceholderText(obs_module_text_vdo("UDPPorts.Auto"));
	editUdpPorts->setToolTip(obs_module_text_vdo("UDPPorts.Help"));
	advancedLayout->addRow(obs_module_text_vdo("UDPPorts"), editUdpPorts);
	auto *portHelp = new QLabel(obs_module_text_vdo("UDPPorts.Help"), advanced);
	portHelp->setWordWrap(true);
	advancedLayout->addRow(portHelp);
	layout->addWidget(advanced);
	advanced->hide();
	connect(advancedToggle, &QToolButton::toggled, advanced, [advancedToggle, advanced](bool expanded) {
		advancedToggle->setArrowType(expanded ? Qt::DownArrow : Qt::RightArrow);
		advanced->setVisible(expanded);
	});
	connect(editUdpPorts, &QLineEdit::textChanged, this, [portHelp](const QString &text) {
		const bool valid = parseUdpPortRange(text.toStdString()).has_value();
		portHelp->setText(obs_module_text_vdo(valid ? "UDPPorts.Help" : "UDPPorts.Invalid"));
		portHelp->setStyleSheet(valid ? "" : "color: #ff6666;");
	});
	connect(editUdpPorts, &QLineEdit::textEdited, this, [this]() { editsPending_ = true; });
	connect(editUdpPorts, &QLineEdit::editingFinished, this, &VDONinjaDock::onSettingsChanged);

	// Actions Group
	QGroupBox *grpActions = new QGroupBox(obs_module_text_vdo("Actions"), container);
	QHBoxLayout *actionLayout = new QHBoxLayout(grpActions);

	btnGoLive = new QPushButton(obs_module_text_vdo("VDONinja.Dock.GoLive"), grpActions);
	btnGoLive->setProperty("themeID", "success"); // OBS standard theme property for green buttons
	btnGoLive->setMinimumHeight(35);
	connect(btnGoLive, &QPushButton::clicked, this, &VDONinjaDock::onGoLiveClicked);

	btnStop = new QPushButton(obs_module_text_vdo("Stop"), grpActions);
	btnStop->setProperty("themeID", "error"); // OBS standard theme property for red buttons
	btnStop->setMinimumHeight(35);
	connect(btnStop, &QPushButton::clicked, this, &VDONinjaDock::onStopClicked);

	actionLayout->addWidget(btnGoLive);
	actionLayout->addWidget(btnStop);

	layout->addWidget(grpActions);

	// Links Group
	QGroupBox *grpLinks = new QGroupBox(obs_module_text_vdo("VDONinja.Dock.Links"), container);
	QVBoxLayout *linkLayout = new QVBoxLayout(grpLinks);

	QPushButton *btnView = new QPushButton(obs_module_text_vdo("VDONinja.Dock.CopyViewLink"), grpLinks);
	QPushButton *btnPush = new QPushButton(obs_module_text_vdo("VDONinja.Dock.CopyPushLink"), grpLinks);

	connect(btnView, &QPushButton::clicked, this, &VDONinjaDock::onCopyViewLink);
	connect(btnPush, &QPushButton::clicked, this, &VDONinjaDock::onCopyPushLink);

	linkLayout->addWidget(btnView);
	linkLayout->addWidget(btnPush);

	layout->addWidget(grpLinks);

	// Status Group
	QGroupBox *grpStatus = new QGroupBox(obs_module_text_vdo("VDONinja.Status"), container);
	QVBoxLayout *statusLayout = new QVBoxLayout(grpStatus);

	lblStatus = new QLabel(obs_module_text_vdo("Ready"), grpStatus);
	lblStatus->setWordWrap(true);
	lblStatus->setAlignment(Qt::AlignCenter);
	lblStatus->setStyleSheet("font-weight: bold; font-size: 14px;");

	// Tally indicator
	lblTally = new QLabel(grpStatus);
	lblTally->setAlignment(Qt::AlignCenter);
	lblTally->setFixedHeight(24);
	lblTally->setVisible(false);

	lblStats = new QLabel(obs_module_text_vdo("VDONinja.Dock.Waiting"), grpStatus);
	lblStats->setWordWrap(true);
	lblStats->setAlignment(Qt::AlignCenter);

	lblSystemCpu = new QLabel(grpStatus);
	lblSystemCpu->setAlignment(Qt::AlignCenter);
	lblSystemCpu->setStyleSheet("font-weight: bold; font-size: 12px; color: #888888;");

	// Chat display
	lblChat = new QLabel(grpStatus);
	lblChat->setWordWrap(true);
	lblChat->setAlignment(Qt::AlignLeft);
	lblChat->setMaximumHeight(60);
	lblChat->setStyleSheet("color: #cccccc; font-size: 11px; padding: 2px 4px;");
	lblChat->setVisible(false);

	statusLayout->addWidget(lblStatus);
	statusLayout->addWidget(lblTally);
	statusLayout->addWidget(lblStats);
	statusLayout->addWidget(lblSystemCpu);
	statusLayout->addWidget(lblChat);

	layout->addWidget(grpStatus);

	layout->addStretch();
	scrollArea->setWidget(container);

	// Keep the version visible even when the session settings need scrolling.
	QWidget *body = new QWidget(this);
	QVBoxLayout *bodyLayout = new QVBoxLayout(body);
	bodyLayout->setContentsMargins(0, 0, 0, 0);
	bodyLayout->addWidget(scrollArea, 1);
	lblVersion = new QLabel(body);
	lblVersion->setObjectName("VDONinjaVersionStatus");
	lblVersion->setTextFormat(Qt::RichText);
	lblVersion->setWordWrap(true);
	lblVersion->setAlignment(Qt::AlignCenter);
	lblVersion->setMargin(6);
	lblVersion->setTextInteractionFlags(Qt::TextBrowserInteraction);
	lblVersion->setOpenExternalLinks(true);
	bodyLayout->addWidget(lblVersion);
	setWidget(body);
}

void VDONinjaDock::updateVersionStatus()
{
	const auto &result = updateChecker_->result();
	QString status;
	switch (result.status) {
	case UpdateCheckStatus::Checking:
		status = obs_module_text_vdo("VDONinja.Update.Checking");
		break;
	case UpdateCheckStatus::UpToDate:
		status = obs_module_text_vdo("VDONinja.Update.UpToDate");
		break;
	case UpdateCheckStatus::UpdateAvailable:
		status = QString::fromUtf8(obs_module_text_vdo("VDONinja.Update.Available")).arg(result.latestVersion);
		break;
	case UpdateCheckStatus::Unavailable:
		status = obs_module_text_vdo("VDONinja.Update.Unavailable");
		break;
	}
	lblVersion->setText(QStringLiteral("VDO.Ninja v%1<br>%2 &middot; <a href=\"%3\">%4</a>")
	                        .arg(QString::fromUtf8(PLUGIN_VERSION).toHtmlEscaped(), status.toHtmlEscaped(),
	                             QString::fromUtf8(kPluginReleasesUrl),
	                             QString::fromUtf8(obs_module_text_vdo("VDONinja.Update.Releases")).toHtmlEscaped()));
}

void VDONinjaDock::loadSettings()
{
	loadingSettings_ = true;
	config_t *config = obs_frontend_get_profile_config();
	if (!config) {
		loadingSettings_ = false;
		return;
	}

	const char *sid = config_get_string(config, "VDONinja", "StreamID");
	const char *rid = config_get_string(config, "VDONinja", "RoomID");
	const char *pass = config_get_string(config, "VDONinja", "Password");

	if (sid && *sid)
		editStreamId->setText(sid);
	else
		editStreamId->setText(QString::fromStdString(generateSessionId()));

	editRoomId->setText(rid ? rid : "");
	editPassword->setText(pass ? pass : "");
	const char *udpPorts = config_get_string(config, "VDONinja", "UDPPorts");
	editUdpPorts->setText(udpPorts ? udpPorts : "auto");

	int maxV = static_cast<int>(config_get_int(config, "VDONinja", "MaxViewers"));
	if (maxV >= 1 && maxV <= 50)
		spinMaxViewers->setValue(maxV);
	else
		spinMaxViewers->setValue(10);

	chkAutoAddFeeds->setChecked(config_get_bool(config, "VDONinja", "AutoAddFeeds"));
	syncFromActiveService();
	loadingSettings_ = false;
	// Save the first generated ID immediately, even before the first stream.
	saveSettings();
}

void VDONinjaDock::reloadProfileSettings()
{
	editsPending_ = false;
	loadSettings();
}

void VDONinjaDock::saveSettings()
{
	config_t *config = obs_frontend_get_profile_config();
	if (!config)
		return;

	config_set_string(config, "VDONinja", "StreamID", editStreamId->text().toUtf8().constData());
	config_set_string(config, "VDONinja", "RoomID", editRoomId->text().toUtf8().constData());
	config_set_string(config, "VDONinja", "Password", editPassword->text().toUtf8().constData());
	config_set_int(config, "VDONinja", "MaxViewers", spinMaxViewers->value());
	config_set_string(config, "VDONinja", "UDPPorts", editUdpPorts->text().toUtf8().constData());
	config_set_bool(config, "VDONinja", "AutoAddFeeds", chkAutoAddFeeds->isChecked());
	config_save(config);
}

bool VDONinjaDock::loadFromServiceSettings(obs_data_t *serviceSettings)
{
	if (!serviceSettings) {
		return false;
	}

	QString sid = QString::fromUtf8(obs_data_get_string(serviceSettings, "stream_id")).trimmed();
	if (sid.isEmpty()) {
		return false;
	}

	const bool wasLoading = loadingSettings_;
	loadingSettings_ = true;
	editStreamId->setText(sid);
	editRoomId->setText(QString::fromUtf8(obs_data_get_string(serviceSettings, "room_id")).trimmed());
	const QString servicePassword = QString::fromUtf8(obs_data_get_string(serviceSettings, "password"));
	editPassword->setText(servicePassword);
	// The common Stream dialog can rebuild a key-only service. Retain the
	// profile's selection when that service has no port field of its own.
	if (obs_data_has_user_value(serviceSettings, "udp_port_range"))
		editUdpPorts->setText(QString::fromUtf8(obs_data_get_string(serviceSettings, "udp_port_range")));

	const int maxV = static_cast<int>(obs_data_get_int(serviceSettings, "max_viewers"));
	if (maxV >= 1 && maxV <= 50) {
		spinMaxViewers->setValue(maxV);
	}

	chkAutoAddFeeds->setChecked(obs_data_get_bool(serviceSettings, "auto_inbound_enabled"));
	loadingSettings_ = wasLoading;
	return true;
}

void VDONinjaDock::syncFromActiveService()
{
	obs_data_t *settings = copyPublishServiceSettings(obs_frontend_get_streaming_service());
	if (!settings)
		return;
	if (loadFromServiceSettings(settings) && !loadingSettings_)
		saveSettings();
	obs_data_release(settings);
}

QString VDONinjaDock::buildUrl(bool push) const
{
	// While live, copy the immutable output snapshot, never unsent editor values.
	obs_output_t *output = obs_frontend_get_streaming_output();
	std::string liveUrl;
	if (output && std::strcmp(obs_output_get_id(output), "vdoninja_output") == 0) {
		auto *vdo = static_cast<VDONinjaOutput *>(obs_obj_get_data(output));
		if (vdo && vdo->isRunning()) {
			const auto snap = vdo->getSettingsSnapshot();
			liveUrl = buildPublishUrl({snap.streamId, snap.password, snap.roomId, snap.salt, snap.wssHost}, push);
		}
	}
	if (output)
		obs_output_release(output);
	if (!liveUrl.empty())
		return QString::fromStdString(liveUrl);

	obs_data_t *settings = copyPublishServiceSettings(obs_frontend_get_streaming_service());
	PublishIdentity identity = readPublishIdentity(settings);
	if (settings)
		obs_data_release(settings);
	identity.streamId = editStreamId->text().trimmed().toStdString();
	identity.password = editPassword->text().toStdString();
	identity.roomId = editRoomId->text().trimmed().toStdString();
	return QString::fromStdString(buildPublishUrl(identity, push));
}

bool VDONinjaDock::applySettingsToService(bool activate)
{
	obs_service_t *service = obs_frontend_get_streaming_service();
	if (!activate && !isVdoNinjaPublishService(service))
		return false;
	obs_data_t *settings = copyPublishServiceSettings(service);
	if (!settings)
		settings = obs_data_create();
	PublishIdentity identity = readPublishIdentity(settings);
	identity.streamId = editStreamId->text().trimmed().toStdString();
	if (identity.streamId.empty()) {
		identity.streamId = generateSessionId();
		editStreamId->setText(QString::fromStdString(identity.streamId));
	}
	identity.roomId = editRoomId->text().trimmed().toStdString();
	identity.password = editPassword->text().toStdString();
	writePublishIdentity(settings, identity);
	obs_data_set_int(settings, "max_viewers", spinMaxViewers->value());
	obs_data_set_string(settings, "udp_port_range", editUdpPorts->text().trimmed().toUtf8().constData());
	const bool autoInbound = chkAutoAddFeeds->isChecked() && !identity.roomId.empty();
	obs_data_set_bool(settings, "auto_inbound_enabled", autoInbound);
	if (autoInbound) {
		obs_data_set_string(settings, "auto_inbound_room_id", identity.roomId.c_str());
		obs_data_set_string(settings, "auto_inbound_password", identity.password.c_str());
	}
	bool applied = true;
	if (activate) {
		applied = activateVdoNinjaServiceFromSettings(settings, false, false);
	} else {
		obs_service_update(service, settings);
		obs_frontend_save_streaming_service();
	}
	obs_data_release(settings);
	if (applied)
		editsPending_ = false;
	saveSettings();
	return applied;
}

void VDONinjaDock::prepareStreaming()
{
	// OBS emits STREAMING_STARTING after encoder setup. Copy session settings here;
	// rate-control preservation is handled before setup by the service catalog.
	if (loadingSettings_)
		return;
	obs_data_t *settings = copyPublishServiceSettings(obs_frontend_get_streaming_service());
	if (!settings)
		return;
	const bool missingId = readPublishIdentity(settings).streamId.empty();
	obs_data_release(settings);
	if (!editsPending_ && !missingId)
		syncFromActiveService();
	applySettingsToService(false);
}

void VDONinjaDock::onGenerateIdClicked()
{
	if (obs_frontend_streaming_active())
		return;
	editStreamId->setText(QString::fromStdString(generateSessionId()));
	onSettingsChanged();
}

void VDONinjaDock::onCopyViewLink()
{
	if (obs_frontend_streaming_active()) {
		syncFromActiveService();
	}

	QString url = buildUrl(false);
	if (!url.isEmpty()) {
		QApplication::clipboard()->setText(url);
		lblStatus->setText(obs_module_text_vdo("VDONinja.Dock.LinkCopied"));
	}
}

void VDONinjaDock::onCopyPushLink()
{
	if (obs_frontend_streaming_active()) {
		syncFromActiveService();
	}

	QString url = buildUrl(true);
	if (!url.isEmpty()) {
		QApplication::clipboard()->setText(url);
		lblStatus->setText(obs_module_text_vdo("VDONinja.Dock.LinkCopied"));
	}
}

void VDONinjaDock::onGoLiveClicked()
{
	if (obs_frontend_streaming_active()) {
		lblStatus->setText(obs_module_text_vdo("VDONinja.Dock.AlreadyLive"));
		return;
	}

	// Settings -> Stream may have changed while the dock was idle. Unsent dock
	// edits win; otherwise refresh before applying, including the UDP selection.
	if (!editsPending_)
		syncFromActiveService();
	btnGoLive->setEnabled(false);
	if (!parseUdpPortRange(editUdpPorts->text().toStdString())) {
		lblStatus->setText(obs_module_text_vdo("UDPPorts.Invalid"));
		btnGoLive->setEnabled(true);
		return;
	}
	const bool configured = applySettingsToService(true);

	if (!configured) {
		lblStatus->setText(obs_module_text_vdo("VDONinja.Dock.ConfigFailed"));
		btnGoLive->setEnabled(true);
		return;
	}

	obs_frontend_streaming_start();
	lblStatus->setText(obs_module_text_vdo("VDONinja.Dock.Starting"));
}

void VDONinjaDock::onStopClicked()
{
	obs_frontend_streaming_stop();
	lblStatus->setText(obs_module_text_vdo("VDONinja.Dock.Stopping"));
}

static QString formatBytes(uint64_t bytes)
{
	if (bytes >= 1073741824ULL) { // >= 1 GB
		return QString::number(static_cast<double>(bytes) / 1073741824.0, 'f', 2) + " GB";
	} else if (bytes >= 1048576ULL) { // >= 1 MB
		return QString::number(static_cast<double>(bytes) / 1048576.0, 'f', 1) + " MB";
	} else {
		return QString::number(bytes / 1024) + " KB";
	}
}

static QString formatUptime(int64_t uptimeMs)
{
	int64_t totalSec = uptimeMs / 1000;
	int64_t hours = totalSec / 3600;
	int64_t minutes = (totalSec % 3600) / 60;
	int64_t seconds = totalSec % 60;

	if (hours > 0) {
		return QString("%1:%2:%3").arg(hours).arg(minutes, 2, 10, QChar('0')).arg(seconds, 2, 10, QChar('0'));
	}
	return QString("%1:%2").arg(minutes, 2, 10, QChar('0')).arg(seconds, 2, 10, QChar('0'));
}

void VDONinjaDock::updateStats()
{
	bool streaming = obs_frontend_streaming_active();
	btnGoLive->setEnabled(!streaming);
	btnStop->setEnabled(streaming);

	// Lock settings while streaming is active
	editStreamId->setEnabled(!streaming);
	btnGenerateId->setEnabled(!streaming);
	editRoomId->setEnabled(!streaming);
	editPassword->setEnabled(!streaming);
	editUdpPorts->setEnabled(!streaming);
	spinMaxViewers->setEnabled(!streaming);
	chkAutoAddFeeds->setEnabled(!streaming);

	if (streaming) {
		lblStatus->setText("LIVE");
		lblStatus->setStyleSheet("font-weight: bold; font-size: 14px; color: #ff3333;");
	} else {
		lblStatus->setText(obs_module_text_vdo("Stopped"));
		lblStatus->setStyleSheet("font-weight: bold; font-size: 14px; color: #888888;");
		lblTally->setVisible(false);
	}

	obs_output_t *output = obs_frontend_get_streaming_output();
	if (output) {
		uint64_t bytes = obs_output_get_total_bytes(output);

		const char *id = obs_output_get_id(output);
		bool isVdo = id && strcmp(id, "vdoninja_output") == 0;
		VDONinjaOutput *vdo = nullptr;
		if (isVdo) {
			vdo = static_cast<VDONinjaOutput *>(obs_obj_get_data(output));
		}

		// Uptime
		int64_t uptimeMs = vdo ? vdo->getUptimeMs() : (obs_output_get_connect_time_ms(output));

		QString stats = QString("%1: %2\n%3: %4")
		                    .arg(obs_module_text_vdo("VDONinja.Dock.Sent"))
		                    .arg(formatBytes(bytes))
		                    .arg(obs_module_text_vdo("VDONinja.Dock.Uptime"))
		                    .arg(formatUptime(uptimeMs));

		if (vdo) {
			int viewers = vdo->getViewerCount();
			int maxV = vdo->getMaxViewers();
			stats += QString("\n%1: %2 / %3").arg(obs_module_text_vdo("VDONinja.Dock.Viewers")).arg(viewers).arg(maxV);

			// Tally indicator
			TallyState tally = vdo->getAggregatedTally();
			if (tally.program) {
				lblTally->setText(obs_module_text_vdo("VDONinja.Dock.OnAir"));
				lblTally->setStyleSheet("background: #ff0000; color: white; font-weight: bold; "
				                        "border-radius: 8px; padding: 2px 8px; font-size: 12px;");
				lblTally->setVisible(true);
			} else if (tally.preview) {
				lblTally->setText(obs_module_text_vdo("VDONinja.Dock.Preview"));
				lblTally->setStyleSheet("background: #00cc00; color: white; font-weight: bold; "
				                        "border-radius: 8px; padding: 2px 8px; font-size: 12px;");
				lblTally->setVisible(true);
			} else {
				lblTally->setVisible(false);
			}
		}

		lblStats->setText(stats);
		obs_output_release(output);
	} else {
		lblStats->setText(obs_module_text_vdo("VDONinja.Dock.NoStats"));
		lblTally->setVisible(false);
	}

	std::optional<double> systemCpu = systemCpuSampler.query();
	if (systemCpu) {
		const double usage = *systemCpu;

		lblSystemCpu->setText(
		    QString("%1: %2%").arg(obs_module_text_vdo("VDONinja.Dock.SystemCpu")).arg(usage, 0, 'f', 0));
		lblSystemCpu->setStyleSheet(
		    QString("font-weight: bold; font-size: 12px; color: %1;").arg(systemCpuStatusColor(usage)));
	} else {
		lblSystemCpu->setText(QString("%1: --%").arg(obs_module_text_vdo("VDONinja.Dock.SystemCpu")));
		lblSystemCpu->setStyleSheet("font-weight: bold; font-size: 12px; color: #888888;");
	}
}

void VDONinjaDock::onChatReceived(const QString &sender, const QString &message)
{
	QString display = QString("<b>%1:</b> %2").arg(sender.toHtmlEscaped(), message.toHtmlEscaped());
	lblChat->setText(display);
	lblChat->setVisible(true);

	chatClearTimer->start(10000);
}

void VDONinjaDock::onSettingsChanged()
{
	if (loadingSettings_)
		return;
	editsPending_ = true;
	saveSettings();
	if (!obs_frontend_streaming_active())
		applySettingsToService(false);
}

} // namespace vdoninja
