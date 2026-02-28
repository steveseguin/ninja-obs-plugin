#pragma once

#include <QDockWidget>
#include <QLineEdit>
#include <QPushButton>
#include <QLabel>
#include <QTimer>
#include <QVBoxLayout>
#include <QGroupBox>

#include <obs-frontend-api.h>

namespace vdoninja {

class VDONinjaDock : public QDockWidget {
    Q_OBJECT

public:
    explicit VDONinjaDock(QWidget *parent = nullptr);
    ~VDONinjaDock();

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

    QLineEdit *editStreamId;
    QLineEdit *editRoomId;
    QLineEdit *editPassword;
    
    QPushButton *btnGoLive;
    QPushButton *btnStop;
    
    QLabel *lblStatus;
    QLabel *lblStats;
    
    QTimer *statsTimer;
};

} // namespace vdoninja
