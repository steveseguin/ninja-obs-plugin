#include "vdoninja-dock.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QMessageBox>
#include <QSpinBox>
#include <QVBoxLayout>

#include <util/config-file.h>

#include "plugin-main.h"
#include "vdoninja-output.h"
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

VDONinjaDock::~VDONinjaDock()
{
	saveSettings();
}

void VDONinjaDock::setupUi()
{
	QWidget *container = new QWidget(this);
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
	spinMaxViewers->setToolTip(obs_module_text_vdo("MaxViewers.Description"));

	QPushButton *btnGen = new QPushButton(obs_module_text_vdo("VDONinja.Dock.GenerateID"), grpCreds);
	connect(btnGen, &QPushButton::clicked, this, &VDONinjaDock::onGenerateIdClicked);

	form->addRow(obs_module_text_vdo("StreamID"), editStreamId);
	form->addRow(obs_module_text_vdo("RoomID"), editRoomId);
	form->addRow(obs_module_text_vdo("Password"), editPassword);
	form->addRow(obs_module_text_vdo("VDONinja.Dock.MaxViewers"), spinMaxViewers);
	form->addRow("", btnGen);

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
	statusLayout->addWidget(lblChat);

	layout->addWidget(grpStatus);

	layout->addStretch();
	setWidget(container);
}

void VDONinjaDock::loadSettings()
{
	config_t *config = obs_frontend_get_profile_config();
	if (!config)
		return;

	const char *sid = config_get_string(config, "VDONinja", "StreamID");
	const char *rid = config_get_string(config, "VDONinja", "RoomID");
	const char *pass = config_get_string(config, "VDONinja", "Password");

	if (sid && *sid)
		editStreamId->setText(sid);
	else
		editStreamId->setText(QString::fromStdString(generateSessionId()));

	if (rid && *rid)
		editRoomId->setText(rid);
	if (pass && *pass)
		editPassword->setText(pass);

	int maxV = static_cast<int>(config_get_int(config, "VDONinja", "MaxViewers"));
	if (maxV >= 1 && maxV <= 50)
		spinMaxViewers->setValue(maxV);
	else
		spinMaxViewers->setValue(10);

	chkAutoAddFeeds->setChecked(config_get_bool(config, "VDONinja", "AutoAddFeeds"));
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
	config_set_bool(config, "VDONinja", "AutoAddFeeds", chkAutoAddFeeds->isChecked());
	config_save(config);
}

QString VDONinjaDock::buildUrl(bool push) const
{
	QString sid = editStreamId->text().trimmed();
	if (sid.isEmpty())
		return "";

	QString rid = editRoomId->text().trimmed();
	QString pass = editPassword->text().trimmed();

	QString url = "https://vdo.ninja/?";
	url += push ? "push=" : "view=";
	url += QString::fromStdString(urlEncode(sid.toStdString()));

	if (!rid.isEmpty())
		url += "&room=" + QString::fromStdString(urlEncode(rid.toStdString()));
	if (!pass.isEmpty())
		url += "&password=" + QString::fromStdString(urlEncode(pass.toStdString()));

	return url;
}

void VDONinjaDock::onGenerateIdClicked()
{
	editStreamId->setText(QString::fromStdString(generateSessionId()));
}

void VDONinjaDock::onCopyViewLink()
{
	QString url = buildUrl(false);
	if (!url.isEmpty()) {
		QApplication::clipboard()->setText(url);
		lblStatus->setText(obs_module_text_vdo("VDONinja.Dock.LinkCopied"));
	}
}

void VDONinjaDock::onCopyPushLink()
{
	QString url = buildUrl(true);
	if (!url.isEmpty()) {
		QApplication::clipboard()->setText(url);
		lblStatus->setText(obs_module_text_vdo("VDONinja.Dock.LinkCopied"));
	}
}

void VDONinjaDock::onGoLiveClicked()
{
	saveSettings();

	// Create settings object to pass to activation helper
	obs_data_t *settings = obs_data_create();
	obs_data_set_string(settings, "stream_id", editStreamId->text().toUtf8().constData());
	obs_data_set_string(settings, "room_id", editRoomId->text().toUtf8().constData());
	obs_data_set_string(settings, "password", editPassword->text().toUtf8().constData());
	obs_data_set_int(settings, "max_viewers", spinMaxViewers->value());
	obs_data_set_bool(settings, "enable_remote", false);

	// Auto-inbound settings: only enable if a room ID is set
	QString roomId = editRoomId->text().trimmed();
	if (chkAutoAddFeeds->isChecked() && !roomId.isEmpty()) {
		obs_data_set_bool(settings, "auto_inbound_enabled", true);
		obs_data_set_string(settings, "auto_inbound_room_id", roomId.toUtf8().constData());
	}

	// Use the shared activation helper which backs up and restores the previous service.
	activateVdoNinjaServiceFromSettings(settings, false, false);
	obs_data_release(settings);

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
	editRoomId->setEnabled(!streaming);
	editPassword->setEnabled(!streaming);
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
	saveSettings();
}

} // namespace vdoninja
