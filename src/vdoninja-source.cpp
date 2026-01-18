/*
 * OBS VDO.Ninja Plugin
 * Source module implementation
 */

#include "vdoninja-source.h"
#include <util/threading.h>

namespace vdoninja {

// OBS source callbacks
static const char *vdoninja_source_getname(void *)
{
    return obs_module_text("VDONinjaSource");
}

static void *vdoninja_source_create(obs_data_t *settings, obs_source_t *source)
{
    try {
        auto *vdo = new VDONinjaSource(settings, source);
        return vdo;
    } catch (const std::exception &e) {
        logError("Failed to create VDO.Ninja source: %s", e.what());
        return nullptr;
    }
}

static void vdoninja_source_destroy(void *data)
{
    auto *vdo = static_cast<VDONinjaSource *>(data);
    delete vdo;
}

static void vdoninja_source_update(void *data, obs_data_t *settings)
{
    auto *vdo = static_cast<VDONinjaSource *>(data);
    vdo->update(settings);
}

static void vdoninja_source_activate(void *data)
{
    auto *vdo = static_cast<VDONinjaSource *>(data);
    vdo->activate();
}

static void vdoninja_source_deactivate(void *data)
{
    auto *vdo = static_cast<VDONinjaSource *>(data);
    vdo->deactivate();
}

static void vdoninja_source_video_tick(void *data, float seconds)
{
    auto *vdo = static_cast<VDONinjaSource *>(data);
    vdo->videoTick(seconds);
}

static void vdoninja_source_video_render(void *data, gs_effect_t *effect)
{
    auto *vdo = static_cast<VDONinjaSource *>(data);
    vdo->videoRender(effect);
}

static uint32_t vdoninja_source_get_width(void *data)
{
    auto *vdo = static_cast<VDONinjaSource *>(data);
    return vdo->getWidth();
}

static uint32_t vdoninja_source_get_height(void *data)
{
    auto *vdo = static_cast<VDONinjaSource *>(data);
    return vdo->getHeight();
}

static obs_properties_t *vdoninja_source_properties(void *)
{
    obs_properties_t *props = obs_properties_create();

    obs_properties_add_text(props, "stream_id", obs_module_text("StreamID"), OBS_TEXT_DEFAULT);
    obs_properties_add_text(props, "room_id", obs_module_text("RoomID"), OBS_TEXT_DEFAULT);
    obs_properties_add_text(props, "password", obs_module_text("Password"), OBS_TEXT_PASSWORD);
    obs_properties_add_text(props, "wss_host", obs_module_text("SignalingServer"), OBS_TEXT_DEFAULT);

    obs_properties_add_bool(props, "enable_data_channel", obs_module_text("EnableDataChannel"));
    obs_properties_add_bool(props, "auto_reconnect", obs_module_text("AutoReconnect"));
    obs_properties_add_bool(props, "force_turn", obs_module_text("ForceTURN"));

    obs_properties_add_int(props, "width", obs_module_text("Width"), 320, 4096, 1);
    obs_properties_add_int(props, "height", obs_module_text("Height"), 240, 2160, 1);

    return props;
}

static void vdoninja_source_defaults(obs_data_t *settings)
{
    obs_data_set_default_string(settings, "stream_id", "");
    obs_data_set_default_string(settings, "room_id", "");
    obs_data_set_default_string(settings, "password", "");
    obs_data_set_default_string(settings, "wss_host", DEFAULT_WSS_HOST);
    obs_data_set_default_bool(settings, "enable_data_channel", true);
    obs_data_set_default_bool(settings, "auto_reconnect", true);
    obs_data_set_default_bool(settings, "force_turn", false);
    obs_data_set_default_int(settings, "width", 1920);
    obs_data_set_default_int(settings, "height", 1080);
}

// Source info structure
obs_source_info vdoninja_source_info = {
    .id = "vdoninja_source",
    .type = OBS_SOURCE_TYPE_INPUT,
    .output_flags = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_AUDIO | OBS_SOURCE_DO_NOT_DUPLICATE,
    .get_name = vdoninja_source_getname,
    .create = vdoninja_source_create,
    .destroy = vdoninja_source_destroy,
    .update = vdoninja_source_update,
    .activate = vdoninja_source_activate,
    .deactivate = vdoninja_source_deactivate,
    .video_tick = vdoninja_source_video_tick,
    .video_render = vdoninja_source_video_render,
    .get_width = vdoninja_source_get_width,
    .get_height = vdoninja_source_get_height,
    .get_defaults = vdoninja_source_defaults,
    .get_properties = vdoninja_source_properties,
};

// Implementation

VDONinjaSource::VDONinjaSource(obs_data_t *settings, obs_source_t *source)
    : source_(source)
{
    loadSettings(settings);

    signaling_ = std::make_unique<VDONinjaSignaling>();
    peerManager_ = std::make_unique<VDONinjaPeerManager>();

    logInfo("VDO.Ninja source created");
}

VDONinjaSource::~VDONinjaSource()
{
    deactivate();

    // Clean up graphics resources
    obs_enter_graphics();
    if (texture_) {
        gs_texture_destroy(texture_);
        texture_ = nullptr;
    }
    obs_leave_graphics();

    if (scaler_) {
        video_scaler_destroy(scaler_);
        scaler_ = nullptr;
    }

    logInfo("VDO.Ninja source destroyed");
}

void VDONinjaSource::loadSettings(obs_data_t *settings)
{
    settings_.streamId = obs_data_get_string(settings, "stream_id");
    settings_.roomId = obs_data_get_string(settings, "room_id");
    settings_.password = obs_data_get_string(settings, "password");
    settings_.wssHost = obs_data_get_string(settings, "wss_host");

    if (settings_.wssHost.empty()) {
        settings_.wssHost = DEFAULT_WSS_HOST;
    }

    settings_.enableDataChannel = obs_data_get_bool(settings, "enable_data_channel");
    settings_.autoReconnect = obs_data_get_bool(settings, "auto_reconnect");
    settings_.forceTurn = obs_data_get_bool(settings, "force_turn");

    width_ = static_cast<uint32_t>(obs_data_get_int(settings, "width"));
    height_ = static_cast<uint32_t>(obs_data_get_int(settings, "height"));

    if (width_ == 0) width_ = 1920;
    if (height_ == 0) height_ = 1080;
}

void VDONinjaSource::update(obs_data_t *settings)
{
    bool wasActive = active_;

    if (wasActive) {
        deactivate();
    }

    loadSettings(settings);

    if (wasActive) {
        activate();
    }
}

void VDONinjaSource::activate()
{
    if (active_) return;

    active_ = true;
    connect();

    logInfo("VDO.Ninja source activated");
}

void VDONinjaSource::deactivate()
{
    if (!active_) return;

    active_ = false;
    disconnect();

    logInfo("VDO.Ninja source deactivated");
}

void VDONinjaSource::connect()
{
    if (settings_.streamId.empty()) {
        logWarning("Stream ID is required");
        return;
    }

    connectionThread_ = std::thread(&VDONinjaSource::connectionThread, this);
}

void VDONinjaSource::disconnect()
{
    connected_ = false;

    if (signaling_) {
        if (signaling_->isPublishing()) {
            signaling_->unpublishStream();
        }
        if (signaling_->isInRoom()) {
            signaling_->leaveRoom();
        }
        signaling_->disconnect();
    }

    if (connectionThread_.joinable()) {
        connectionThread_.join();
    }

    // Clear buffers
    {
        std::lock_guard<std::mutex> lock(videoMutex_);
        videoFrames_.clear();
    }
    {
        std::lock_guard<std::mutex> lock(audioMutex_);
        audioBuffers_.clear();
    }
}

void VDONinjaSource::connectionThread()
{
    logInfo("Connecting to VDO.Ninja stream: %s", settings_.streamId.c_str());

    // Initialize peer manager
    peerManager_->initialize(signaling_.get());
    peerManager_->setEnableDataChannel(settings_.enableDataChannel);
    peerManager_->setForceTurn(settings_.forceTurn);

    // Set up track callback
    peerManager_->setOnTrack([this](const std::string &uuid, TrackType type, std::shared_ptr<rtc::Track> track) {
        if (type == TrackType::Video) {
            onVideoTrack(uuid, track);
        } else {
            onAudioTrack(uuid, track);
        }
    });

    peerManager_->setOnPeerConnected([this](const std::string &uuid) {
        logInfo("Connected to publisher: %s", uuid.c_str());
        connected_ = true;
    });

    peerManager_->setOnPeerDisconnected([this](const std::string &uuid) {
        logInfo("Disconnected from publisher: %s", uuid.c_str());
        connected_ = false;
    });

    // Set up signaling callbacks
    signaling_->setOnConnected([this]() {
        logInfo("Connected to signaling server");

        // Join room if specified
        if (!settings_.roomId.empty()) {
            signaling_->joinRoom(settings_.roomId, settings_.password);
        }

        // Request to view the stream
        signaling_->viewStream(settings_.streamId, settings_.password);
        peerManager_->startViewing(settings_.streamId);
    });

    signaling_->setOnDisconnected([this]() {
        logInfo("Disconnected from signaling server");
        connected_ = false;
    });

    signaling_->setOnError([this](const std::string &error) {
        logError("Signaling error: %s", error.c_str());
    });

    signaling_->setOnStreamAdded([this](const std::string &streamId, const std::string &uuid) {
        // If this is our target stream, start viewing
        if (streamId == settings_.streamId || hashStreamId(settings_.streamId, settings_.password, DEFAULT_SALT) == streamId) {
            logInfo("Target stream appeared in room, connecting...");
            signaling_->viewStream(settings_.streamId, settings_.password);
        }
    });

    // Configure reconnection
    signaling_->setAutoReconnect(settings_.autoReconnect, DEFAULT_RECONNECT_ATTEMPTS);

    // Connect
    if (!signaling_->connect(settings_.wssHost)) {
        logError("Failed to connect to signaling server");
        return;
    }

    // Keep thread alive while active
    while (active_) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

void VDONinjaSource::onVideoTrack(const std::string &uuid, std::shared_ptr<rtc::Track> track)
{
    logInfo("Received video track from %s", uuid.c_str());

    track->onMessage([this](auto data) {
        if (std::holds_alternative<rtc::binary>(data)) {
            auto &binary = std::get<rtc::binary>(data);
            processVideoData(reinterpret_cast<const uint8_t*>(binary.data()), binary.size());
        }
    }, nullptr);
}

void VDONinjaSource::onAudioTrack(const std::string &uuid, std::shared_ptr<rtc::Track> track)
{
    logInfo("Received audio track from %s", uuid.c_str());

    track->onMessage([this](auto data) {
        if (std::holds_alternative<rtc::binary>(data)) {
            auto &binary = std::get<rtc::binary>(data);
            processAudioData(reinterpret_cast<const uint8_t*>(binary.data()), binary.size());
        }
    }, nullptr);
}

void VDONinjaSource::processVideoData(const uint8_t *data, size_t size)
{
    // This is where you'd decode the video (RTP -> H.264 -> raw frames)
    // For now, we'll create a placeholder implementation
    // A full implementation would use libav/ffmpeg for decoding

    if (size < 12) return; // Minimum RTP header

    // Skip RTP header (12 bytes minimum)
    const uint8_t *payload = data + 12;
    size_t payloadSize = size - 12;

    // The actual implementation would:
    // 1. Parse RTP header to get timestamp, sequence number
    // 2. Reassemble fragmented packets (FU-A for H.264)
    // 3. Feed NAL units to decoder
    // 4. Get decoded frames and push to OBS

    // For demonstration, we'll just update timing
    lastVideoTime_ = currentTimeMs();

    // TODO: Implement actual video decoding
    // This requires integrating with FFmpeg or similar decoder
}

void VDONinjaSource::processAudioData(const uint8_t *data, size_t size)
{
    // Similar to video - decode RTP -> Opus -> PCM
    if (size < 12) return;

    const uint8_t *payload = data + 12;
    size_t payloadSize = size - 12;

    lastAudioTime_ = currentTimeMs();

    // TODO: Implement actual audio decoding
    // This requires Opus decoder
}

void VDONinjaSource::pushVideoFrame(const VideoFrame &frame)
{
    std::lock_guard<std::mutex> lock(videoMutex_);

    if (videoFrames_.size() >= MAX_VIDEO_FRAMES) {
        videoFrames_.pop_front();
    }
    videoFrames_.push_back(frame);
}

bool VDONinjaSource::popVideoFrame(VideoFrame &frame)
{
    std::lock_guard<std::mutex> lock(videoMutex_);

    if (videoFrames_.empty()) {
        return false;
    }

    frame = std::move(videoFrames_.front());
    videoFrames_.pop_front();
    return true;
}

void VDONinjaSource::pushAudioBuffer(const AudioBuffer &buffer)
{
    std::lock_guard<std::mutex> lock(audioMutex_);

    if (audioBuffers_.size() >= MAX_AUDIO_BUFFERS) {
        audioBuffers_.pop_front();
    }
    audioBuffers_.push_back(buffer);
}

void VDONinjaSource::videoTick(float seconds)
{
    UNUSED_PARAMETER(seconds);

    if (!active_ || !connected_) return;

    // Process any pending video frames
    VideoFrame frame;
    while (popVideoFrame(frame)) {
        // Convert to OBS video frame and output
        obs_source_frame obsFrame = {};
        obsFrame.width = frame.width;
        obsFrame.height = frame.height;
        obsFrame.format = frame.format;
        obsFrame.timestamp = frame.timestamp;
        obsFrame.data[0] = frame.data.data();
        obsFrame.linesize[0] = frame.linesize;

        obs_source_output_video(source_, &obsFrame);
    }
}

void VDONinjaSource::videoRender(gs_effect_t *effect)
{
    UNUSED_PARAMETER(effect);

    if (!texture_) return;

    gs_effect_t *defaultEffect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
    gs_technique_t *tech = gs_effect_get_technique(defaultEffect, "Draw");

    gs_technique_begin(tech);
    gs_technique_begin_pass(tech, 0);

    gs_effect_set_texture(gs_effect_get_param_by_name(defaultEffect, "image"), texture_);
    gs_draw_sprite(texture_, 0, width_, height_);

    gs_technique_end_pass(tech);
    gs_technique_end(tech);
}

void VDONinjaSource::audioRender(obs_source_audio *audio)
{
    UNUSED_PARAMETER(audio);

    if (!active_ || !connected_) return;

    // Output any pending audio
    std::lock_guard<std::mutex> lock(audioMutex_);

    while (!audioBuffers_.empty()) {
        auto &buffer = audioBuffers_.front();

        obs_source_audio obsAudio = {};
        obsAudio.data[0] = buffer.data.data();
        obsAudio.frames = static_cast<uint32_t>(buffer.data.size() / (buffer.channels * 2)); // 16-bit samples
        obsAudio.speakers = buffer.channels == 2 ? SPEAKERS_STEREO : SPEAKERS_MONO;
        obsAudio.samples_per_sec = buffer.sampleRate;
        obsAudio.format = AUDIO_FORMAT_16BIT;
        obsAudio.timestamp = buffer.timestamp;

        obs_source_output_audio(source_, &obsAudio);

        audioBuffers_.pop_front();
    }
}

uint32_t VDONinjaSource::getWidth() const
{
    return width_;
}

uint32_t VDONinjaSource::getHeight() const
{
    return height_;
}

bool VDONinjaSource::isConnected() const
{
    return connected_;
}

std::string VDONinjaSource::getStreamId() const
{
    return settings_.streamId;
}

} // namespace vdoninja
