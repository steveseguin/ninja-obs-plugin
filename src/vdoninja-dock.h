#pragma once

#include <QDockWidget>
#include <QLineEdit>
#include <QPushButton>
#include <QLabel>
#include <QTimer>
#include <QVBoxLayout>
#include <QGroupBox>
#include <QSpinBox>
#include <QCheckBox>

#include <obs-frontend-api.h>

namespace vdoninja {

class VDONinjaDock : public QDockWidget {
    Q_OBJECT

public:
    explicit VDONinjaDock(QWidget *parent = nullptr);
    ~VDONinjaDock();

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
    QString buildUrl(bool push) const;

    // Session Setup
    QLineEdit *editStreamId;
    QLineEdit *editRoomId;
    QLineEdit *editPassword;
    QSpinBox *spinMaxViewers;

    // Actions
    QPushButton *btnGoLive;
    QPushButton *btnStop;

    // Options
    QCheckBox *chkEnableRemote;
    QCheckBox *chkAutoAddFeeds;

    // Status
    QLabel *lblStatus;
    QLabel *lblTally;
    QLabel *lblStats;
    QLabel *lblChat;

    QTimer *statsTimer;
    QTimer *chatClearTimer;
};

} // namespace vdoninja
