#pragma once

#include <QGroupBox>

#include "vdoninja-guest-return.h"

class QComboBox;
class QCheckBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;

namespace vdoninja
{

class GuestReturnWidget : public QGroupBox
{
public:
	explicit GuestReturnWidget(QWidget *parent);
	void loadSettings();
	void stop();

private:
	GuestReturnSettings readSettings() const;
	void saveSettings();
	void refresh();
	GuestReturnOutput publisher_;
	QWidget *settingsPanel_;
	QLineEdit *streamId_;
	QLineEdit *password_;
	QLineEdit *room_;
	QLineEdit *signaling_;
	QLineEdit *salt_;
	QLineEdit *iceServers_;
	QCheckBox *forceTurn_;
	QWidget *advancedPanel_;
	QComboBox *encoder_;
	QComboBox *size_;
	QComboBox *frameRate_;
	QSpinBox *track_;
	QSpinBox *bitrate_;
	QSpinBox *maxViewers_;
	QPushButton *start_;
	QPushButton *stop_;
	QPushButton *copy_;
	QPushButton *copyInvite_;
	QLabel *status_;
	bool loading_ = true;
};

} // namespace vdoninja
