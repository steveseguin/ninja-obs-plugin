#include "vdoninja-guest-return-widget.h"

#include <obs-frontend-api.h>

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTimer>
#include <QUuid>
#include <QVBoxLayout>

#include <util/config-file.h>

#include "vdoninja-output.h"

namespace vdoninja
{

GuestReturnWidget::GuestReturnWidget(QWidget *parent) : QGroupBox(obs_module_text("GuestReturn.Title"), parent)
{
	setObjectName("VDONinjaGuestReturn");
	auto *layout = new QVBoxLayout(this);
	auto *help = new QLabel(obs_module_text("GuestReturn.Intro"), this);
	help->setWordWrap(true);
	layout->addWidget(help);
	settingsPanel_ = new QWidget(this);
	auto *form = new QFormLayout(settingsPanel_);
	form->setContentsMargins(0, 0, 0, 0);
	form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
	form->setRowWrapPolicy(QFormLayout::WrapLongRows);
	streamId_ = new QLineEdit(settingsPanel_);
	streamId_->setObjectName("GuestReturnStreamID");
	password_ = new QLineEdit(settingsPanel_);
	password_->setObjectName("GuestReturnPassword");
	password_->setEchoMode(QLineEdit::PasswordEchoOnEdit);
	room_ = new QLineEdit(settingsPanel_);
	room_->setObjectName("GuestReturnRoomID");
	track_ = new QSpinBox(settingsPanel_);
	track_->setObjectName("GuestReturnAudioTrack");
	track_->setRange(1, 6);
	track_->setValue(2);
	track_->setToolTip(obs_module_text("GuestReturn.Help"));
	track_->setAccessibleDescription(obs_module_text("GuestReturn.Help"));
	encoder_ = new QComboBox(settingsPanel_);
	encoder_->setObjectName("GuestReturnEncoder");
	encoder_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
	encoder_->setMinimumContentsLength(12);
	bitrate_ = new QSpinBox(settingsPanel_);
	bitrate_->setObjectName("GuestReturnBitrate");
	bitrate_->setRange(100, 50000);
	bitrate_->setSingleStep(100);
	bitrate_->setSuffix(" kbps");
	// OBS themes set a spin-box minimum in QSS, overriding setMinimumWidth().
	// Reserve room for the suffix and arrows so narrow translated forms wrap.
	bitrate_->setStyleSheet("min-width: 140px;");
	maxViewers_ = new QSpinBox(settingsPanel_);
	maxViewers_->setObjectName("GuestReturnMaxViewers");
	maxViewers_->setRange(1, 50);
	maxViewers_->setToolTip(obs_module_text("GuestReturn.CapacityHelp"));
	size_ = new QComboBox(settingsPanel_);
	size_->setObjectName("GuestReturnSize");
	size_->addItem(obs_module_text("GuestReturn.MatchOBS"), 0);
	for (int height : {1080, 720, 540, 360})
		size_->addItem(QString::fromUtf8(obs_module_text("GuestReturn.MaxHeight")).arg(height), height);
	size_->setToolTip(obs_module_text("GuestReturn.SizeHelp"));
	frameRate_ = new QComboBox(settingsPanel_);
	frameRate_->setObjectName("GuestReturnFrameRate");
	frameRate_->addItem(obs_module_text("GuestReturn.MatchOBS"), 1);
	for (int divisor : {2, 3, 4})
		frameRate_->addItem(QString::fromUtf8(obs_module_text("GuestReturn.DivideFPS")).arg(divisor), divisor);
	frameRate_->setToolTip(obs_module_text("GuestReturn.FPSHelp"));
	form->addRow(obs_module_text("StreamID"), streamId_);
	form->addRow(obs_module_text("Password"), password_);
	form->addRow(obs_module_text("RoomID"), room_);
	form->addRow(obs_module_text("GuestReturn.AudioTrack"), track_);
	form->addRow(obs_module_text("GuestReturn.Encoder"), encoder_);
	form->addRow(obs_module_text("GuestReturn.Bitrate"), bitrate_);
	form->addRow(obs_module_text("GuestReturn.Size"), size_);
	form->addRow(obs_module_text("GuestReturn.FrameRate"), frameRate_);
	form->addRow(obs_module_text("MaxViewers"), maxViewers_);
	layout->addWidget(settingsPanel_);
	auto *advancedToggle = new QPushButton(obs_module_text("GuestReturn.Advanced"), this);
	advancedToggle->setObjectName("GuestReturnAdvanced");
	advancedToggle->setCheckable(true);
	layout->addWidget(advancedToggle);
	advancedPanel_ = new QWidget(this);
	auto *advanced = new QFormLayout(advancedPanel_);
	advanced->setContentsMargins(0, 0, 0, 0);
	advanced->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
	advanced->setRowWrapPolicy(QFormLayout::WrapLongRows);
	signaling_ = new QLineEdit(advancedPanel_);
	signaling_->setObjectName("GuestReturnSignaling");
	signaling_->setToolTip(obs_module_text("SignalingServer.OptionalHelp"));
	salt_ = new QLineEdit(advancedPanel_);
	salt_->setObjectName("GuestReturnSalt");
	salt_->setToolTip(obs_module_text("Salt.OptionalHelp"));
	iceServers_ = new QLineEdit(advancedPanel_);
	iceServers_->setObjectName("GuestReturnICEServers");
	iceServers_->setToolTip(obs_module_text("CustomICEServers.Help"));
	iceServers_->setEchoMode(QLineEdit::PasswordEchoOnEdit);
	forceTurn_ = new QCheckBox(obs_module_text("ForceTURN"), advancedPanel_);
	forceTurn_->setObjectName("GuestReturnForceTURN");
	forceTurn_->setToolTip(obs_module_text("ForceTURN.Description"));
	advanced->addRow(obs_module_text("SignalingServer"), signaling_);
	advanced->addRow(obs_module_text("Salt"), salt_);
	advanced->addRow(obs_module_text("CustomICEServers"), iceServers_);
	advanced->addRow(forceTurn_);
	// Expose the field label, rather than its current value, to accessibility APIs.
	for (auto *fields : {form, advanced}) {
		for (int row = 0; row < fields->rowCount(); ++row) {
			auto *labelItem = fields->itemAt(row, QFormLayout::LabelRole);
			auto *fieldItem = fields->itemAt(row, QFormLayout::FieldRole);
			if (!labelItem || !fieldItem || !fieldItem->widget())
				continue;
			if (auto *label = qobject_cast<QLabel *>(labelItem->widget()))
				fieldItem->widget()->setAccessibleName(label->text());
		}
	}
	auto *networkHelp = new QLabel(obs_module_text("GuestReturn.NetworkHelp"), advancedPanel_);
	networkHelp->setWordWrap(true);
	advanced->addRow(networkHelp);
	advancedPanel_->setVisible(false);
	connect(advancedToggle, &QPushButton::toggled, advancedPanel_, &QWidget::setVisible);
	layout->addWidget(advancedPanel_);
	auto *buttons = new QHBoxLayout;
	start_ = new QPushButton(obs_module_text("GuestReturn.Start"), this);
	start_->setObjectName("GuestReturnStart");
	stop_ = new QPushButton(obs_module_text("GuestReturn.Stop"), this);
	stop_->setObjectName("GuestReturnStop");
	buttons->addWidget(start_);
	buttons->addWidget(stop_);
	layout->addLayout(buttons);
	copy_ = new QPushButton(obs_module_text("GuestReturn.Copy"), this);
	copy_->setObjectName("GuestReturnCopyLink");
	layout->addWidget(copy_);
	copyInvite_ = new QPushButton(obs_module_text("GuestReturn.CopyInvite"), this);
	copyInvite_->setObjectName("GuestReturnCopyInvite");
	copyInvite_->setToolTip(obs_module_text("GuestReturn.InviteHelp"));
	layout->addWidget(copyInvite_);
	status_ = new QLabel(this);
	status_->setObjectName("GuestReturnStatus");
	status_->setWordWrap(true);
	layout->addWidget(status_);
	connect(start_, &QPushButton::clicked, this, [this] {
		saveSettings();
		publisher_.start(readSettings());
		refresh();
	});
	connect(stop_, &QPushButton::clicked, this, [this] { stop(); });
	connect(copy_, &QPushButton::clicked, this, [this] {
		const auto settings = publisher_.busy() ? publisher_.settings() : readSettings();
		QApplication::clipboard()->setText(QString::fromStdString(buildPublishUrl(settings.identity)));
	});
	connect(copyInvite_, &QPushButton::clicked, this, [this] {
		const auto settings = publisher_.busy() ? publisher_.settings() : readSettings();
		QApplication::clipboard()->setText(QString::fromStdString(buildRoomGuestInviteUrl(settings.identity)));
	});
	for (auto *edit : {streamId_, password_, room_, signaling_, salt_, iceServers_})
		connect(edit, &QLineEdit::editingFinished, this, [this] { saveSettings(); });
	for (auto *edit : {streamId_, room_})
		connect(edit, &QLineEdit::textChanged, this, [this] { refresh(); });
	for (auto *spin : {track_, bitrate_, maxViewers_})
		connect(spin, QOverload<int>::of(&QSpinBox::valueChanged), this, [this] { saveSettings(); });
	connect(forceTurn_, &QCheckBox::toggled, this, [this] { saveSettings(); });
	connect(encoder_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this] { saveSettings(); });
	for (auto *combo : {size_, frameRate_})
		connect(combo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this] { saveSettings(); });
	auto *timer = new QTimer(this);
	connect(timer, &QTimer::timeout, this, [this] { refresh(); });
	timer->start(1000);
}

GuestReturnSettings GuestReturnWidget::readSettings() const
{
	GuestReturnSettings settings;
	settings.identity = {streamId_->text().trimmed().toStdString(), password_->text().toStdString(),
	                     room_->text().trimmed().toStdString(), salt_->text().trimmed().toStdString(),
	                     signaling_->text().trimmed().toStdString()};
	settings.audioTrack = track_->value();
	settings.videoBitrate = bitrate_->value();
	settings.maxViewers = maxViewers_->value();
	settings.customIceServers = iceServers_->text().trimmed().toStdString();
	settings.forceTurn = forceTurn_->isChecked();
	settings.videoEncoder = encoder_->currentData().toString().toStdString();
	settings.maxHeight = size_->currentData().toInt();
	settings.frameRateDivisor = frameRate_->currentData().toInt();
	return settings;
}

void GuestReturnWidget::loadSettings()
{
	loading_ = true;
	publisher_.clearError();
	auto *config = obs_frontend_get_profile_config();
	if (!config) {
		loading_ = false;
		return;
	}
	const auto get = [config](const char *key) {
		const char *value = config_get_string(config, "VDONinjaGuestReturn", key);
		return QString::fromUtf8(value ? value : "");
	};
	streamId_->setText(get("StreamID"));
	if (streamId_->text().isEmpty())
		streamId_->setText("return" + QUuid::createUuid().toString(QUuid::Id128).left(12));
	password_->setText(get("Password"));
	room_->setText(get("RoomID"));
	signaling_->setText(get("SignalingServer"));
	salt_->setText(get("Salt"));
	iceServers_->setText(get("CustomICEServers"));
	forceTurn_->setChecked(config_get_bool(config, "VDONinjaGuestReturn", "ForceTURN"));
	size_->setCurrentIndex(
	    std::max(0, size_->findData(static_cast<int>(config_get_int(config, "VDONinjaGuestReturn", "MaxHeight")))));
	frameRate_->setCurrentIndex(std::max(
	    0, frameRate_->findData(static_cast<int>(config_get_int(config, "VDONinjaGuestReturn", "FrameRateDivisor")))));
	const auto maxViewers = config_get_int(config, "VDONinjaGuestReturn", "MaxViewers");
	maxViewers_->setValue(maxViewers >= 1 && maxViewers <= 50 ? static_cast<int>(maxViewers) : 10);
	const auto track = config_get_int(config, "VDONinjaGuestReturn", "AudioTrack");
	track_->setValue(track >= 1 && track <= 6 ? static_cast<int>(track) : 2);
	const auto bitrate = config_get_int(config, "VDONinjaGuestReturn", "VideoBitrate");
	bitrate_->setValue(bitrate >= 100 && bitrate <= 50000 ? static_cast<int>(bitrate) : 2500);
	encoder_->clear();
	for (const auto &encoder : GuestReturnOutput::videoEncoders())
		encoder_->addItem(QString::fromStdString(encoder.name), QString::fromStdString(encoder.id));
	const auto savedEncoder = get("VideoEncoder");
	if (!savedEncoder.isEmpty()) {
		int index = encoder_->findData(savedEncoder);
		if (index < 0) {
			encoder_->addItem(QString::fromUtf8(obs_module_text("GuestReturn.MissingEncoder")).arg(savedEncoder),
			                  savedEncoder);
			index = encoder_->count() - 1;
		}
		encoder_->setCurrentIndex(index);
	}
	loading_ = false;
	saveSettings();
	refresh();
}

void GuestReturnWidget::saveSettings()
{
	if (loading_)
		return;
	auto *config = obs_frontend_get_profile_config();
	if (!config)
		return;
	const auto settings = readSettings();
	config_set_string(config, "VDONinjaGuestReturn", "StreamID", settings.identity.streamId.c_str());
	config_set_string(config, "VDONinjaGuestReturn", "Password", settings.identity.password.c_str());
	config_set_string(config, "VDONinjaGuestReturn", "RoomID", settings.identity.roomId.c_str());
	config_set_string(config, "VDONinjaGuestReturn", "SignalingServer", settings.identity.wssHost.c_str());
	config_set_string(config, "VDONinjaGuestReturn", "Salt", settings.identity.salt.c_str());
	config_set_string(config, "VDONinjaGuestReturn", "CustomICEServers", settings.customIceServers.c_str());
	config_set_bool(config, "VDONinjaGuestReturn", "ForceTURN", settings.forceTurn);
	config_set_int(config, "VDONinjaGuestReturn", "MaxViewers", settings.maxViewers);
	config_set_string(config, "VDONinjaGuestReturn", "VideoEncoder", settings.videoEncoder.c_str());
	config_set_int(config, "VDONinjaGuestReturn", "AudioTrack", settings.audioTrack);
	config_set_int(config, "VDONinjaGuestReturn", "VideoBitrate", settings.videoBitrate);
	config_set_int(config, "VDONinjaGuestReturn", "MaxHeight", settings.maxHeight);
	config_set_int(config, "VDONinjaGuestReturn", "FrameRateDivisor", settings.frameRateDivisor);
	config_save(config);
}

void GuestReturnWidget::stop()
{
	publisher_.stop();
	refresh();
}

void GuestReturnWidget::refresh()
{
	const bool busy = publisher_.busy();
	settingsPanel_->setEnabled(!busy);
	advancedPanel_->setEnabled(!busy);
	start_->setEnabled(!busy);
	stop_->setEnabled(busy);
	copy_->setEnabled(busy || !streamId_->text().trimmed().isEmpty());
	const auto settings = busy ? publisher_.settings() : readSettings();
	copyInvite_->setEnabled(!buildRoomGuestInviteUrl(settings.identity).empty());
	const auto error = publisher_.error();
	if (!error.empty()) {
		status_->setText(obs_module_text(error.c_str()));
	} else if (busy) {
		auto *vdo = static_cast<VDONinjaOutput *>(obs_obj_get_data(publisher_.output()));
		status_->setText(vdo && vdo->isConnected() ? QString::fromUtf8(obs_module_text("GuestReturn.Live"))
		                                                 .arg(publisher_.settings().audioTrack)
		                                                 .arg(vdo->getViewerCount())
		                                                 .arg(publisher_.settings().maxViewers)
		                                           : QString::fromUtf8(obs_module_text("GuestReturn.Connecting")));
	} else {
		status_->setText(obs_module_text("GuestReturn.Idle"));
	}
	// A failed asynchronous start can still own initialized encoders and workers.
	if (!busy && publisher_.output())
		publisher_.stop();
}

} // namespace vdoninja
