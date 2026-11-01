#pragma once

#include <obs-frontend-api.h>

#include <QCheckBox>
#include <QDockWidget>
#include <QElapsedTimer>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>

#include "vdoninja-system-cpu.h"

namespace vdoninja
{

class VDONinjaUpdateChecker;
class GuestReturnWidget;

class VDONinjaDock : public QDockWidget
{
	Q_OBJECT

public:
	explicit VDONinjaDock(QWidget *parent = nullptr);
	~VDONinjaDock();
	void syncFromActiveService();
	void prepareStreaming();
	void reloadProfileSettings();
	void shutdown();
	void stopGuestReturn();

	// Called from output thread (via obs_queue_task) to show chat messages
	void onChatReceived(const QString &sender, const QString &message);

private slots:
	void onGoLiveClicked();
	void onStopClicked();
	void onGenerateIdClicked();
	void onCopyViewLink();
	void onCopyPushLink();
	void onCopyWhepLink();
	void updateStats();
	void onSettingsChanged();

private:
	void setupUi();
	void updateVersionStatus();
	void showLinkMessage(const char *key);
	void loadSettings();
	void saveSettings();
	bool applySettingsToService(bool activate);
	QString buildUrl(bool push) const;
	bool loadFromServiceSettings(obs_data_t *serviceSettings);

	// Session Setup
	QLineEdit *editStreamId;
	QLineEdit *editRoomId;
	QLineEdit *editPassword;
	QSpinBox *spinMaxViewers;
	QLineEdit *editUdpPorts;
	QLineEdit *editWhepToken;
	QPushButton *btnGenerateId;

	// Actions
	QPushButton *btnGoLive;
	QPushButton *btnStop;

	// Options
	QCheckBox *chkAutoAddFeeds;

	// Status
	QLabel *lblStatus;
	QLabel *lblLinkMessage;
	QLabel *lblTally;
	QLabel *lblStats;
	QLabel *lblSystemCpu;
	QLabel *lblChat;
	QLabel *lblVersion;
	QElapsedTimer linkMessageTimer_;
	bool linkMessageStreaming_ = false;
	VDONinjaUpdateChecker *updateChecker_ = nullptr;
	GuestReturnWidget *guestReturn_ = nullptr;

	QTimer *statsTimer;
	QTimer *chatClearTimer;
	SystemCpuSampler systemCpuSampler;
	bool loadingSettings_ = true;
	bool editsPending_ = false;
};

} // namespace vdoninja
