#include "vdoninja-dock.h"
#include "vdoninja-utils.h"
#include "vdoninja-output.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QApplication>
#include <QClipboard>
#include <QMessageBox>
#include <QCheckBox>

#include <util/config-file.h>

namespace vdoninja {

static const char *obs_module_text_vdo(const char *key)
{
	const char *text = obs_module_text(key);
	return (text && *text) ? text : key;
}

VDONinjaDock::VDONinjaDock(QWidget *parent)
    : QDockWidget(parent)
{
    setObjectName("VDONinjaStudioDock");
    setWindowTitle(obs_module_text_vdo("VDONinja.Studio.Title"));
    setAllowedAreas(Qt::AllDockWidgetAreas);
    
    setupUi();
    loadSettings();
    
    statsTimer = new QTimer(this);
    connect(statsTimer, &QTimer::timeout, this, &VDONinjaDock::updateStats);
    statsTimer->start(1000);
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
    
    QPushButton *btnGen = new QPushButton(obs_module_text_vdo("VDONinja.Dock.GenerateID"), grpCreds);
    connect(btnGen, &QPushButton::clicked, this, &VDONinjaDock::onGenerateIdClicked);
    
    form->addRow(obs_module_text_vdo("StreamID"), editStreamId);
    form->addRow(obs_module_text_vdo("RoomID"), editRoomId);
    form->addRow(obs_module_text_vdo("Password"), editPassword);
    form->addRow("", btnGen);
    
    layout->addWidget(grpCreds);
    
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
    
    lblStats = new QLabel(obs_module_text_vdo("VDONinja.Dock.Waiting"), grpStatus);
    lblStats->setWordWrap(true);
    lblStats->setAlignment(Qt::AlignCenter);
    
    statusLayout->addWidget(lblStatus);
    statusLayout->addWidget(lblStats);
    
    layout->addWidget(grpStatus);
    
    layout->addStretch();
    setWidget(container);
}

void VDONinjaDock::loadSettings()
{
    config_t *config = obs_frontend_get_profile_config();
    if (!config) return;
    
    const char *sid = config_get_string(config, "VDONinja", "StreamID");
    const char *rid = config_get_string(config, "VDONinja", "RoomID");
    const char *pass = config_get_string(config, "VDONinja", "Password");
    
    if (sid && *sid) editStreamId->setText(sid);
    else editStreamId->setText(QString::fromStdString(generateSessionId()));
    
    if (rid && *rid) editRoomId->setText(rid);
    if (pass && *pass) editPassword->setText(pass);
}

void VDONinjaDock::saveSettings()
{
    config_t *config = obs_frontend_get_profile_config();
    if (!config) return;
    
    config_set_string(config, "VDONinja", "StreamID", editStreamId->text().toUtf8().constData());
    config_set_string(config, "VDONinja", "RoomID", editRoomId->text().toUtf8().constData());
    config_set_string(config, "VDONinja", "Password", editPassword->text().toUtf8().constData());
    config_save(config);
}

QString VDONinjaDock::buildUrl(bool push) const
{
    QString sid = editStreamId->text().trimmed();
    if (sid.isEmpty()) return "";
    
    QString rid = editRoomId->text().trimmed();
    QString pass = editPassword->text().trimmed();
    
    QString url = "https://vdo.ninja/?";
    url += push ? "push=" : "view=";
    url += sid;
    
    if (!rid.isEmpty()) url += "&room=" + rid;
    if (!pass.isEmpty()) url += "&password=" + pass;
    
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

// Forward declaration of activation helper from plugin-main.cpp
namespace {
    extern bool activateVdoNinjaServiceFromSettings(obs_data_t *sourceSettings, bool generateStreamIdIfMissing, bool temporarySwitch);
}

void VDONinjaDock::onGoLiveClicked()
{
    saveSettings();
    
    // Create settings object to pass to activation helper
    obs_data_t *settings = obs_data_create();
    obs_data_set_string(settings, "stream_id", editStreamId->text().toUtf8().constData());
    obs_data_set_string(settings, "room_id", editRoomId->text().toUtf8().constData());
    obs_data_set_string(settings, "password", editPassword->text().toUtf8().constData());
    
    // Use the existing activation logic to switch OBS to VDO.Ninja
    // Note: We need to make this function accessible. For now, we'll implement a robust switch.
    obs_service_t *vdoService = obs_service_create("vdoninja_service", "default_service", settings, nullptr);
    if (vdoService) {
        obs_frontend_set_streaming_service(vdoService);
        obs_frontend_save_streaming_service();
        obs_service_release(vdoService);
    }
    obs_data_release(settings);
    
    obs_frontend_streaming_start();
    lblStatus->setText(obs_module_text_vdo("VDONinja.Dock.Starting"));
}

void VDONinjaDock::onStopClicked()
{
    obs_frontend_streaming_stop();
    lblStatus->setText(obs_module_text_vdo("VDONinja.Dock.Stopping"));
}

void VDONinjaDock::updateStats()
{
    bool streaming = obs_frontend_streaming_active();
    btnGoLive->setEnabled(!streaming);
    btnStop->setEnabled(streaming);
    
    if (streaming) {
        lblStatus->setText("LIVE");
        lblStatus->setStyleSheet("font-weight: bold; font-size: 14px; color: #ff3333;");
    } else {
        lblStatus->setText(obs_module_text_vdo("Stopped"));
        lblStatus->setStyleSheet("font-weight: bold; font-size: 14px; color: #888888;");
    }
    
    obs_output_t *output = obs_frontend_get_streaming_output();
    if (output) {
        uint64_t bytes = obs_output_get_total_bytes(output);
        int64_t uptime = obs_output_get_connect_time_ms(output) / 1000;
        
        QString stats = QString("%1: %2 KB\n%3: %4s")
            .arg(obs_module_text_vdo("VDONinja.Dock.Sent"))
            .arg(bytes / 1024)
            .arg(obs_module_text_vdo("VDONinja.Dock.Uptime"))
            .arg(uptime);
        
        void *typeData = obs_output_get_type_data(output);
        if (typeData) {
            // We can't safely cast to VDONinjaOutput without knowing if it's our output
            const char *id = obs_output_get_id(output);
            if (id && strcmp(id, "vdoninja_output") == 0) {
                auto *vdo = static_cast<VDONinjaOutput*>(typeData);
                stats += QString("\n%1: %2").arg(obs_module_text_vdo("MaxViewers")).arg(vdo->getViewerCount());
            }
        }
        
        lblStats->setText(stats);
        obs_output_release(output);
    } else {
        lblStats->setText(obs_module_text_vdo("VDONinja.Dock.NoStats"));
    }
}

void VDONinjaDock::onSettingsChanged()
{
    saveSettings();
}

} // namespace vdoninja
