/*
 * OBS VDO.Ninja Plugin
 * Output module implementation
 */

#include "vdoninja-output.h"
#include <util/dstr.h>
#include <util/threading.h>

namespace vdoninja
{

// OBS output callbacks
static const char *vdoninja_output_getname(void *)
{
    return obs_module_text("VDONinjaOutput");
}

static void *vdoninja_output_create(obs_data_t *settings, obs_output_t *output)
{
    try {
        auto *vdo = new VDONinjaOutput(settings, output);
        return vdo;
    } catch (const std::exception &e) {
        logError("Failed to create VDO.Ninja output: %s", e.what());
        return nullptr;
    }
}

static void vdoninja_output_destroy(void *data)
{
    auto *vdo = static_cast<VDONinjaOutput *>(data);
    delete vdo;
}

static bool vdoninja_output_start(void *data)
{
    auto *vdo = static_cast<VDONinjaOutput *>(data);
    return vdo->start();
}

static void vdoninja_output_stop(void *data, uint64_t)
{
    auto *vdo = static_cast<VDONinjaOutput *>(data);
    vdo->stop();
}

static void vdoninja_output_data(void *data, encoder_packet *packet)
{
    auto *vdo = static_cast<VDONinjaOutput *>(data);
    vdo->data(packet);
}

static void vdoninja_output_update(void *data, obs_data_t *settings)
{
    auto *vdo = static_cast<VDONinjaOutput *>(data);
    vdo->update(settings);
}

static obs_properties_t *vdoninja_output_properties(void *)
{
    obs_properties_t *props = obs_properties_create();

    obs_properties_add_text(props, "stream_id", obs_module_text("StreamID"), OBS_TEXT_DEFAULT);
    obs_properties_add_text(props, "room_id", obs_module_text("RoomID"), OBS_TEXT_DEFAULT);
    obs_properties_add_text(props, "password", obs_module_text("Password"), OBS_TEXT_PASSWORD);
    obs_properties_add_text(props, "wss_host", obs_module_text("SignalingServer"),
                            OBS_TEXT_DEFAULT);

    obs_property_t *codec =
        obs_properties_add_list(props, "video_codec", obs_module_text("VideoCodec"),
                                OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(codec, "H.264", static_cast<int>(VideoCodec::H264));
    obs_property_list_add_int(codec, "VP8", static_cast<int>(VideoCodec::VP8));
    obs_property_list_add_int(codec, "VP9", static_cast<int>(VideoCodec::VP9));

    obs_properties_add_int(props, "bitrate", obs_module_text("Bitrate"), 500, 50000, 100);
    obs_properties_add_int(props, "max_viewers", obs_module_text("MaxViewers"), 1, 50, 1);
    obs_properties_add_bool(props, "enable_data_channel", obs_module_text("EnableDataChannel"));
    obs_properties_add_bool(props, "auto_reconnect", obs_module_text("AutoReconnect"));
    obs_properties_add_bool(props, "force_turn", obs_module_text("ForceTURN"));

    return props;
}

static void vdoninja_output_defaults(obs_data_t *settings)
{
    obs_data_set_default_string(settings, "stream_id", "");
    obs_data_set_default_string(settings, "room_id", "");
    obs_data_set_default_string(settings, "password", "");
    obs_data_set_default_string(settings, "wss_host", DEFAULT_WSS_HOST);
    obs_data_set_default_int(settings, "video_codec", static_cast<int>(VideoCodec::H264));
    obs_data_set_default_int(settings, "bitrate", 4000);
    obs_data_set_default_int(settings, "max_viewers", 10);
    obs_data_set_default_bool(settings, "enable_data_channel", true);
    obs_data_set_default_bool(settings, "auto_reconnect", true);
    obs_data_set_default_bool(settings, "force_turn", false);
}

static uint64_t vdoninja_output_total_bytes(void *data)
{
    auto *vdo = static_cast<VDONinjaOutput *>(data);
    return vdo->getTotalBytes();
}

static int vdoninja_output_connect_time(void *data)
{
    auto *vdo = static_cast<VDONinjaOutput *>(data);
    return vdo->getConnectTime();
}

// Output info structure
obs_output_info vdoninja_output_info = {
    .id = "vdoninja_output",
    .flags = OBS_OUTPUT_AV | OBS_OUTPUT_ENCODED | OBS_OUTPUT_SERVICE,
    .get_name = vdoninja_output_getname,
    .create = vdoninja_output_create,
    .destroy = vdoninja_output_destroy,
    .start = vdoninja_output_start,
    .stop = vdoninja_output_stop,
    .encoded_packet = vdoninja_output_data,
    .update = vdoninja_output_update,
    .get_defaults = vdoninja_output_defaults,
    .get_properties = vdoninja_output_properties,
    .get_total_bytes = vdoninja_output_total_bytes,
    .get_connect_time_ms = vdoninja_output_connect_time,
    .encoded_video_codecs = "h264",
    .encoded_audio_codecs = "opus",
    .protocols = "VDO.Ninja",
};

// Implementation

VDONinjaOutput::VDONinjaOutput(obs_data_t *settings, obs_output_t *output) : output_(output)
{
    loadSettings(settings);

    signaling_ = std::make_unique<VDONinjaSignaling>();
    peerManager_ = std::make_unique<VDONinjaPeerManager>();

    logInfo("VDO.Ninja output created");
}

VDONinjaOutput::~VDONinjaOutput()
{
    stop(false);
    logInfo("VDO.Ninja output destroyed");
}

void VDONinjaOutput::loadSettings(obs_data_t *settings)
{
    settings_.streamId = obs_data_get_string(settings, "stream_id");
    settings_.roomId = obs_data_get_string(settings, "room_id");
    settings_.password = obs_data_get_string(settings, "password");
    settings_.wssHost = obs_data_get_string(settings, "wss_host");

    if (settings_.wssHost.empty()) {
        settings_.wssHost = DEFAULT_WSS_HOST;
    }

    settings_.videoCodec = static_cast<VideoCodec>(obs_data_get_int(settings, "video_codec"));
    settings_.quality.bitrate = static_cast<int>(obs_data_get_int(settings, "bitrate")) * 1000;
    settings_.maxViewers = static_cast<int>(obs_data_get_int(settings, "max_viewers"));
    settings_.enableDataChannel = obs_data_get_bool(settings, "enable_data_channel");
    settings_.autoReconnect = obs_data_get_bool(settings, "auto_reconnect");
    settings_.forceTurn = obs_data_get_bool(settings, "force_turn");
}

void VDONinjaOutput::update(obs_data_t *settings)
{
    loadSettings(settings);
}

bool VDONinjaOutput::start()
{
    if (running_) {
        logWarning("Output already running");
        return false;
    }

    if (settings_.streamId.empty()) {
        logError("Stream ID is required");
        obs_output_signal_stop(output_, OBS_OUTPUT_INVALID_STREAM);
        return false;
    }

    running_ = true;
    startTimeMs_ = currentTimeMs();

    startStopThread_ = std::thread(&VDONinjaOutput::startThread, this);

    return true;
}

void VDONinjaOutput::startThread()
{
    logInfo("Starting VDO.Ninja output...");

    // Initialize peer manager
    peerManager_->initialize(signaling_.get());
    peerManager_->setVideoCodec(settings_.videoCodec);
    peerManager_->setAudioCodec(settings_.audioCodec);
    peerManager_->setBitrate(settings_.quality.bitrate);
    peerManager_->setEnableDataChannel(settings_.enableDataChannel);
    peerManager_->setForceTurn(settings_.forceTurn);

    // Set up callbacks
    signaling_->setOnConnected([this]() {
        logInfo("Connected to signaling server");

        // Join room if specified
        if (!settings_.roomId.empty()) {
            signaling_->joinRoom(settings_.roomId, settings_.password);
        }

        // Start publishing
        signaling_->publishStream(settings_.streamId, settings_.password);
        peerManager_->startPublishing(settings_.maxViewers);

        connected_ = true;
        connectTimeMs_ = currentTimeMs() - startTimeMs_;

        // Signal OBS that we're connected
        obs_output_begin_data_capture(output_, 0);
    });

    signaling_->setOnDisconnected([this]() {
        logInfo("Disconnected from signaling server");
        connected_ = false;

        if (running_ && settings_.autoReconnect) {
            logInfo("Will attempt to reconnect...");
        }
    });

    signaling_->setOnError(
        [this](const std::string &error) { logError("Signaling error: %s", error.c_str()); });

    peerManager_->setOnPeerConnected([this](const std::string &uuid) {
        logInfo("Viewer connected: %s (total: %d)", uuid.c_str(), peerManager_->getViewerCount());
    });

    peerManager_->setOnPeerDisconnected([this](const std::string &uuid) {
        logInfo("Viewer disconnected: %s (total: %d)", uuid.c_str(),
                peerManager_->getViewerCount());
    });

    // Configure reconnection
    signaling_->setAutoReconnect(settings_.autoReconnect, DEFAULT_RECONNECT_ATTEMPTS);

    // Connect to signaling server
    if (!signaling_->connect(settings_.wssHost)) {
        logError("Failed to connect to signaling server");
        obs_output_signal_stop(output_, OBS_OUTPUT_CONNECT_FAILED);
        running_ = false;
        return;
    }

    logInfo("VDO.Ninja output started successfully");
}

void VDONinjaOutput::stop(bool signal)
{
    if (!running_)
        return;

    running_ = false;
    connected_ = false;

    logInfo("Stopping VDO.Ninja output...");

    // Stop publishing
    peerManager_->stopPublishing();

    // Unpublish stream
    if (signaling_->isPublishing()) {
        signaling_->unpublishStream();
    }

    // Leave room
    if (signaling_->isInRoom()) {
        signaling_->leaveRoom();
    }

    // Disconnect
    signaling_->disconnect();

    // Wait for start thread to finish
    if (startStopThread_.joinable()) {
        startStopThread_.join();
    }

    // End data capture
    obs_output_end_data_capture(output_);

    if (signal) {
        obs_output_signal_stop(output_, OBS_OUTPUT_SUCCESS);
    }

    logInfo("VDO.Ninja output stopped");
}

void VDONinjaOutput::data(encoder_packet *packet)
{
    if (!running_ || !connected_)
        return;

    if (packet->type == OBS_ENCODER_VIDEO) {
        processVideoPacket(packet);
    } else if (packet->type == OBS_ENCODER_AUDIO) {
        processAudioPacket(packet);
    }

    totalBytes_ += packet->size;
}

void VDONinjaOutput::processVideoPacket(encoder_packet *packet)
{
    bool keyframe = packet->keyframe;
    uint32_t timestamp = static_cast<uint32_t>(packet->pts * 90); // Convert to 90kHz clock

    peerManager_->sendVideoFrame(packet->data, packet->size, timestamp, keyframe);
}

void VDONinjaOutput::processAudioPacket(encoder_packet *packet)
{
    uint32_t timestamp = static_cast<uint32_t>(packet->pts * 48); // Convert to 48kHz clock

    peerManager_->sendAudioFrame(packet->data, packet->size, timestamp);
}

uint64_t VDONinjaOutput::getTotalBytes() const
{
    return totalBytes_;
}

int VDONinjaOutput::getConnectTime() const
{
    return static_cast<int>(connectTimeMs_);
}

int VDONinjaOutput::getViewerCount() const
{
    return peerManager_ ? peerManager_->getViewerCount() : 0;
}

} // namespace vdoninja
