#pragma once

#include <obs-frontend-api.h>

#include <QCheckBox>
#include <QDockWidget>
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

	// Called from output thread (via obs_queue_task) to show chat messages
	void onChatReceived(const QString &sender, const QString &message);

private slots:
	void onGoLiveClicked();
	void onStopClicked();
	void onGenerateIdClicked();
	void onCopyViewLink();
	void onCopyPushLink();
	void updateStats();
	void onSettingsChanged();

private:
	void setupUi();
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

	// Actions
	QPushButton *btnGoLive;
	QPushButton *btnStop;

	// Options
	QCheckBox *chkAutoAddFeeds;

	// Status
	QLabel *lblStatus;
	QLabel *lblTally;
	QLabel *lblStats;
	QLabel *lblSystemCpu;
	QLabel *lblChat;

	QTimer *statsTimer;
	QTimer *chatClearTimer;
	SystemCpuSampler systemCpuSampler;
	bool loadingSettings_ = true;
	bool editsPending_ = false;
};

} // namespace vdoninja
