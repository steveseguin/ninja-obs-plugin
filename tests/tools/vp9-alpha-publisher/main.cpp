/*
 * VP9 Alpha Publisher Test Tool
 *
 * Connects to VDO.Ninja signaling as a publisher and sends two VP9 video
 * streams to any viewer that connects:
 *
 *   Track 1 (primary)  — YUV420P encoding of the visible colour content
 *   Track 2 (alpha)    — YUV420P encoding where Y = alpha channel, U = V = 128
 *
 * The OBS plugin's native receiver combines these into YUVA420P output.
 *
 * Usage:
 *   vp9-alpha-publisher --stream-id <id> [--password <pwd>]
 *                        [--wss wss://wss.vdo.ninja]
 *                        [--width 320] [--height 240] [--fps 30]
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#include <rtc/rtc.hpp>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "vdoninja-signaling.h"
#include "vdoninja-utils.h"

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------

static std::atomic<bool> g_running{true};

static void signalHandler(int)
{
	g_running = false;
}

// ---------------------------------------------------------------------------
// Logging helpers (the signaling/utils code calls blog() via obs-stubs)
// ---------------------------------------------------------------------------

static void printfLog(const char *prefix, const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	fprintf(stderr, "[%s] ", prefix);
	vfprintf(stderr, fmt, args);
	fprintf(stderr, "\n");
	va_end(args);
}

#define LOG_INFO(...) printfLog("INFO", __VA_ARGS__)
#define LOG_WARN(...) printfLog("WARN", __VA_ARGS__)
#define LOG_ERR(...) printfLog("ERROR", __VA_ARGS__)

// ---------------------------------------------------------------------------
// VP9 RTP packetization — minimal RFC 9628 descriptor (B/E flags only)
// ---------------------------------------------------------------------------

static constexpr size_t kMaxRtpPayload = 1150; // stay under typical MTU

static void sendVP9FrameRtp(const std::shared_ptr<rtc::Track> &track, uint16_t &seq, uint32_t ts,
                             uint32_t ssrc, uint8_t payloadType, const std::vector<uint8_t> &vp9Frame)
{
	if (!track || vp9Frame.empty()) {
		return;
	}

	const size_t total = vp9Frame.size();
	size_t offset = 0;
	bool first = true;

	while (offset < total) {
		const size_t remaining = total - offset;
		const bool last = (remaining <= kMaxRtpPayload);
		const size_t chunkLen = last ? remaining : kMaxRtpPayload;

		// 12-byte fixed RTP header + 1-byte VP9 descriptor + payload chunk
		std::vector<std::byte> pkt(12 + 1 + chunkLen);
		auto *p = reinterpret_cast<uint8_t *>(pkt.data());

		// Byte 0: V=2, P=0, X=0, CC=0
		p[0] = 0x80;
		// Byte 1: M=1 on last packet, PT
		p[1] = static_cast<uint8_t>(last ? (0x80 | payloadType) : payloadType);
		// Bytes 2-3: sequence number
		p[2] = static_cast<uint8_t>((seq >> 8) & 0xFF);
		p[3] = static_cast<uint8_t>(seq & 0xFF);
		++seq;
		// Bytes 4-7: timestamp (90 kHz)
		p[4] = static_cast<uint8_t>((ts >> 24) & 0xFF);
		p[5] = static_cast<uint8_t>((ts >> 16) & 0xFF);
		p[6] = static_cast<uint8_t>((ts >> 8) & 0xFF);
		p[7] = static_cast<uint8_t>(ts & 0xFF);
		// Bytes 8-11: SSRC
		p[8] = static_cast<uint8_t>((ssrc >> 24) & 0xFF);
		p[9] = static_cast<uint8_t>((ssrc >> 16) & 0xFF);
		p[10] = static_cast<uint8_t>((ssrc >> 8) & 0xFF);
		p[11] = static_cast<uint8_t>(ssrc & 0xFF);
		// Byte 12: VP9 descriptor (I=P=L=F=V=Z=0; B=first, E=last)
		uint8_t desc = 0;
		if (first)
			desc |= 0x08; // B bit
		if (last)
			desc |= 0x04; // E bit
		p[12] = desc;
		// Bitstream chunk
		std::memcpy(p + 13, vp9Frame.data() + offset, chunkLen);

		try {
			track->send(pkt);
		} catch (...) {
			break;
		}

		offset += chunkLen;
		first = false;
	}
}

// ---------------------------------------------------------------------------
// FFmpeg VP9 encoder
// ---------------------------------------------------------------------------

struct VP9Encoder {
	AVCodecContext *ctx = nullptr;
	AVFrame *frame = nullptr;
	AVPacket *pkt = nullptr;
	int width = 0;
	int height = 0;

	bool init(int w, int h, int fps)
	{
		const AVCodec *codec = avcodec_find_encoder_by_name("libvpx-vp9");
		if (!codec) {
			LOG_ERR("libvpx-vp9 encoder not available");
			return false;
		}

		ctx = avcodec_alloc_context3(codec);
		if (!ctx)
			return false;

		ctx->width = w;
		ctx->height = h;
		ctx->time_base = {1, fps};
		ctx->framerate = {fps, 1};
		ctx->pix_fmt = AV_PIX_FMT_YUV420P;
		ctx->bit_rate = 500000;
		ctx->gop_size = fps * 2; // keyframe every 2 s
		ctx->max_b_frames = 0;

		// VP9-specific: target quality / realtime mode
		av_opt_set(ctx->priv_data, "quality", "realtime", 0);
		av_opt_set_int(ctx->priv_data, "cpu-used", 8, 0);
		av_opt_set(ctx->priv_data, "deadline", "realtime", 0);

		if (avcodec_open2(ctx, codec, nullptr) < 0) {
			LOG_ERR("Failed to open libvpx-vp9 encoder");
			avcodec_free_context(&ctx);
			return false;
		}

		frame = av_frame_alloc();
		pkt = av_packet_alloc();
		if (!frame || !pkt) {
			close();
			return false;
		}

		frame->format = AV_PIX_FMT_YUV420P;
		frame->width = w;
		frame->height = h;
		if (av_image_alloc(frame->data, frame->linesize, w, h, AV_PIX_FMT_YUV420P, 32) < 0) {
			LOG_ERR("Failed to allocate encoder frame buffer");
			close();
			return false;
		}

		width = w;
		height = h;
		LOG_INFO("VP9 encoder initialized (%dx%d @ %d fps)", w, h, fps);
		return true;
	}

	// Encode the frame currently in `frame`. Returns encoded packets.
	std::vector<std::vector<uint8_t>> encode(int64_t pts)
	{
		std::vector<std::vector<uint8_t>> result;
		frame->pts = pts;

		if (avcodec_send_frame(ctx, frame) < 0)
			return result;

		while (true) {
			const int rc = avcodec_receive_packet(ctx, pkt);
			if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF)
				break;
			if (rc < 0)
				break;
			result.push_back(std::vector<uint8_t>(pkt->data, pkt->data + pkt->size));
			av_packet_unref(pkt);
		}
		return result;
	}

	void close()
	{
		if (frame) {
			av_freep(&frame->data[0]);
			av_frame_free(&frame);
		}
		if (pkt)
			av_packet_free(&pkt);
		if (ctx)
			avcodec_free_context(&ctx);
	}
};

// ---------------------------------------------------------------------------
// Test pattern generation — animated circle on checkerboard background
// ---------------------------------------------------------------------------

// Fill a YUVA420P test frame.
// Primary planes: moving coloured circle on a checkerboard background.
// Alpha plane:    the circle is fully opaque, background is 50% transparent.
static void generateTestFrame(uint8_t *yPlane, int yStride, uint8_t *uPlane, int uStride, uint8_t *vPlane,
                               int vStride, uint8_t *aPlane, int aStride, int w, int h, int frameNum)
{
	// Circle centre oscillates horizontally
	const float cx = static_cast<float>(w) * 0.5f + static_cast<float>(w) * 0.3f *
	                                                     std::sin(static_cast<float>(frameNum) * 0.05f);
	const float cy = static_cast<float>(h) * 0.5f;
	const float radius = static_cast<float>(std::min(w, h)) * 0.25f;

	// Luma plane (Y)
	for (int y = 0; y < h; ++y) {
		for (int x = 0; x < w; ++x) {
			const float dx = static_cast<float>(x) - cx;
			const float dy = static_cast<float>(y) - cy;
			const bool inCircle = (dx * dx + dy * dy) < (radius * radius);
			// Checkerboard: 8x8 squares alternating 80/160
			const bool checker = (((x / 8) + (y / 8)) & 1) != 0;
			yPlane[y * yStride + x] = inCircle ? 200u : (checker ? 80u : 160u);
		}
	}

	// Chroma planes (U, V) — greenish tint for background, orange for circle
	const int chromaW = w / 2;
	const int chromaH = h / 2;
	for (int cy2 = 0; cy2 < chromaH; ++cy2) {
		for (int cx2 = 0; cx2 < chromaW; ++cx2) {
			const float dx = static_cast<float>(cx2 * 2 + 1) - cx;
			const float dy = static_cast<float>(cy2 * 2 + 1) - cy;
			const bool inCircle = (dx * dx + dy * dy) < (radius * radius);
			uPlane[cy2 * uStride + cx2] = inCircle ? 90u : 110u;
			vPlane[cy2 * vStride + cx2] = inCircle ? 150u : 90u;
		}
	}

	// Alpha plane: circle fully opaque (255), background half-transparent (128)
	for (int ay = 0; ay < h; ++ay) {
		for (int ax = 0; ax < w; ++ax) {
			const float dx = static_cast<float>(ax) - cx;
			const float dy = static_cast<float>(ay) - cy;
			const bool inCircle = (dx * dx + dy * dy) < (radius * radius);
			aPlane[ay * aStride + ax] = inCircle ? 255u : 128u;
		}
	}
}

// ---------------------------------------------------------------------------
// Peer session — one WebRTC connection to a viewer
// ---------------------------------------------------------------------------

struct PeerSession {
	std::string uuid;
	std::string session;
	std::shared_ptr<rtc::PeerConnection> pc;
	std::shared_ptr<rtc::Track> videoTrack;
	std::shared_ptr<rtc::Track> alphaTrack;
	uint16_t videoSeq = 0;
	uint16_t alphaSeq = 0;
	uint32_t videoSsrc = 0xDEADBEEF;
	uint32_t alphaSsrc = 0xCAFEBABE;
	std::atomic<bool> connected{false};
};

// ---------------------------------------------------------------------------
// Publisher
// ---------------------------------------------------------------------------

struct Config {
	std::string streamId;
	std::string password;
	std::string wssHost = "wss://wss.vdo.ninja";
	int width = 320;
	int height = 240;
	int fps = 30;
};

class AlphaPublisher {
public:
	explicit AlphaPublisher(Config cfg) : cfg_(std::move(cfg)) {}

	bool start()
	{
		signaling_ = std::make_unique<vdoninja::VDONinjaSignaling>();
		signaling_->setSalt("vdo.ninja");

		signaling_->setOnConnected([this]() { LOG_INFO("Signaling connected"); });
		signaling_->setOnDisconnected([this]() { LOG_WARN("Signaling disconnected"); });
		signaling_->setOnError([](const std::string &e) { LOG_ERR("Signaling error: %s", e.c_str()); });

		signaling_->setOnOfferRequest([this](const std::string &uuid, const std::string &session) {
			LOG_INFO("Viewer %s wants to connect", uuid.c_str());
			createPeerForViewer(uuid, session);
		});

		signaling_->setOnAnswer([this](const std::string &uuid, const std::string &sdp, const std::string &) {
			LOG_INFO("Received answer from %s", uuid.c_str());
			std::lock_guard<std::mutex> lock(peersMutex_);
			auto it = peers_.find(uuid);
			if (it == peers_.end())
				return;
			try {
				it->second->pc->setRemoteDescription(rtc::Description(sdp, "answer"));
			} catch (const std::exception &e) {
				LOG_ERR("Failed to set remote description for %s: %s", uuid.c_str(), e.what());
			}
		});

		signaling_->setOnIceCandidate(
		    [this](const std::string &uuid, const std::string &candidate, const std::string &mid,
		           const std::string &) {
			    std::lock_guard<std::mutex> lock(peersMutex_);
			    auto it = peers_.find(uuid);
			    if (it == peers_.end())
				    return;
			    try {
				    it->second->pc->addRemoteCandidate(rtc::Candidate(candidate, mid));
			    } catch (...) {
			    }
		    });

		if (!signaling_->connect(cfg_.wssHost)) {
			LOG_ERR("Failed to connect to signaling server");
			return false;
		}

		if (!signaling_->publishStream(cfg_.streamId, cfg_.password)) {
			LOG_ERR("Failed to publish stream '%s'", cfg_.streamId.c_str());
			return false;
		}

		LOG_INFO("Publishing stream '%s' — waiting for viewers...", cfg_.streamId.c_str());
		return true;
	}

	void runFrameLoop()
	{
		VP9Encoder primaryEnc, alphaEnc;
		if (!primaryEnc.init(cfg_.width, cfg_.height, cfg_.fps)) {
			LOG_ERR("Primary encoder init failed");
			return;
		}
		if (!alphaEnc.init(cfg_.width, cfg_.height, cfg_.fps)) {
			LOG_ERR("Alpha encoder init failed");
			primaryEnc.close();
			return;
		}

		// Scratch buffers for the alpha-as-Y frame
		const int alphaFrameSize = av_image_get_buffer_size(AV_PIX_FMT_YUV420P, cfg_.width, cfg_.height, 32);
		std::vector<uint8_t> alphaBuf(static_cast<size_t>(alphaFrameSize));

		const auto frameDuration = std::chrono::microseconds(1000000 / cfg_.fps);
		auto nextFrameTime = std::chrono::steady_clock::now();
		int64_t pts = 0;

		// RTP timestamps advance by 90000/fps ticks each frame
		const uint32_t rtpTicksPerFrame = static_cast<uint32_t>(90000 / cfg_.fps);

		LOG_INFO("Frame loop running (%dx%d @ %d fps)", cfg_.width, cfg_.height, cfg_.fps);

		while (g_running.load()) {
			// Sleep until next frame time
			const auto now = std::chrono::steady_clock::now();
			if (nextFrameTime > now) {
				std::this_thread::sleep_until(nextFrameTime);
			}
			nextFrameTime += frameDuration;

			// Generate test pattern into primary encoder frame
			generateTestFrame(primaryEnc.frame->data[0], primaryEnc.frame->linesize[0],
			                  primaryEnc.frame->data[1], primaryEnc.frame->linesize[1],
			                  primaryEnc.frame->data[2], primaryEnc.frame->linesize[2],
			                  nullptr, // alpha not written to primary frame
			                  0, cfg_.width, cfg_.height, static_cast<int>(pts));

			// Generate alpha frame: Y = alpha values, U = V = 128
			{
				// We need a temporary alpha-A plane; reuse alphaBuf as scratch
				const int w = cfg_.width;
				const int h = cfg_.height;
				// Fill alpha Y-plane from the test pattern (circle alpha)
				const float cx = static_cast<float>(w) * 0.5f +
				                 static_cast<float>(w) * 0.3f * std::sin(static_cast<float>(pts) * 0.05f);
				const float cy = static_cast<float>(h) * 0.5f;
				const float radius = static_cast<float>(std::min(w, h)) * 0.25f;

				uint8_t *aY = alphaEnc.frame->data[0];
				uint8_t *aU = alphaEnc.frame->data[1];
				uint8_t *aV = alphaEnc.frame->data[2];
				const int aYStride = alphaEnc.frame->linesize[0];
				const int aUStride = alphaEnc.frame->linesize[1];
				const int aVStride = alphaEnc.frame->linesize[2];

				for (int y = 0; y < h; ++y) {
					for (int x = 0; x < w; ++x) {
						const float dx = static_cast<float>(x) - cx;
						const float dy = static_cast<float>(y) - cy;
						aY[y * aYStride + x] =
						    (dx * dx + dy * dy < radius * radius) ? 255u : 128u;
					}
				}
				for (int y = 0; y < h / 2; ++y) {
					for (int x = 0; x < w / 2; ++x) {
						aU[y * aUStride + x] = 128;
						aV[y * aVStride + x] = 128;
					}
				}
			}

			// Encode both frames
			auto primaryPackets = primaryEnc.encode(pts);
			auto alphaPackets = alphaEnc.encode(pts);
			++pts;

			// Compute RTP timestamp for this frame
			const uint32_t rtpTs = static_cast<uint32_t>(pts) * rtpTicksPerFrame;

			// Send to all connected viewers
			{
				std::lock_guard<std::mutex> lock(peersMutex_);
				for (auto &kv : peers_) {
					auto &peer = *kv.second;
					if (!peer.connected.load())
						continue;

					for (const auto &pkt : primaryPackets) {
						sendVP9FrameRtp(peer.videoTrack, peer.videoSeq, rtpTs, peer.videoSsrc, 96,
						               pkt);
					}
					for (const auto &pkt : alphaPackets) {
						sendVP9FrameRtp(peer.alphaTrack, peer.alphaSeq, rtpTs, peer.alphaSsrc, 97,
						               pkt);
					}
				}
			}
		}

		primaryEnc.close();
		alphaEnc.close();
		LOG_INFO("Frame loop exited");
	}

	void stop()
	{
		g_running = false;
		if (signaling_)
			signaling_->disconnect();
		std::lock_guard<std::mutex> lock(peersMutex_);
		peers_.clear();
	}

private:
	void createPeerForViewer(const std::string &uuid, const std::string &session)
	{
		rtc::Configuration rtcConfig{};
		rtcConfig.proxyServer.reset();
		rtcConfig.bindAddress.reset();
		rtcConfig.certificateType = rtc::CertificateType::Default;
		rtcConfig.iceTransportPolicy = rtc::TransportPolicy::All;
		rtcConfig.enableIceTcp = false;
		rtcConfig.enableIceUdpMux = false;
		rtcConfig.disableAutoNegotiation = false;
		rtcConfig.forceMediaTransport = false;
		rtcConfig.portRangeBegin = 1024;
		rtcConfig.portRangeEnd = 65535;
		rtcConfig.mtu.reset();
		rtcConfig.maxMessageSize.reset();
		rtcConfig.iceServers.push_back({"stun:stun.l.google.com:19302"});
		rtcConfig.iceServers.push_back({"stun:stun.cloudflare.com:3478"});

		auto peer = std::make_shared<PeerSession>();
		peer->uuid = uuid;
		peer->session = session;
		peer->pc = std::make_shared<rtc::PeerConnection>(rtcConfig);

		const auto weakPeer = std::weak_ptr<PeerSession>(peer);

		peer->pc->onStateChange([this, uuid, weakPeer](rtc::PeerConnection::State state) {
			if (state == rtc::PeerConnection::State::Connected) {
				LOG_INFO("Viewer %s connected", uuid.c_str());
				if (auto p = weakPeer.lock()) {
					p->connected = true;
				}
			} else if (state == rtc::PeerConnection::State::Disconnected ||
			           state == rtc::PeerConnection::State::Failed ||
			           state == rtc::PeerConnection::State::Closed) {
				LOG_INFO("Viewer %s disconnected (state=%d)", uuid.c_str(), static_cast<int>(state));
				if (auto p = weakPeer.lock()) {
					p->connected = false;
				}
				std::lock_guard<std::mutex> lock(peersMutex_);
				peers_.erase(uuid);
			}
		});

		peer->pc->onLocalCandidate([this, uuid, session](rtc::Candidate cand) {
			signaling_->sendIceCandidate(uuid, std::string(cand), cand.mid(), session);
		});

		// Set up local description callback BEFORE creating the offer
		peer->pc->onLocalDescription([this, uuid, session](rtc::Description sdp) {
			LOG_INFO("Sending offer to viewer %s", uuid.c_str());
			signaling_->sendOffer(uuid, std::string(sdp), session);
		});

		// Add primary VP9 video track (SendOnly)
		{
			rtc::Description::Video vid("video", rtc::Description::Direction::SendOnly);
			vid.addVP9Codec(96);
			vid.setBitrate(500);
			peer->videoTrack = peer->pc->addTrack(vid);
		}

		// Add alpha VP9 video track (SendOnly) — second video m-line
		{
			rtc::Description::Video alpha("video-alpha", rtc::Description::Direction::SendOnly);
			alpha.addVP9Codec(97);
			alpha.setBitrate(200);
			peer->alphaTrack = peer->pc->addTrack(alpha);
		}

		// Create offer (triggers onLocalDescription)
		peer->pc->setLocalDescription();

		{
			std::lock_guard<std::mutex> lock(peersMutex_);
			peers_[uuid] = peer;
		}
	}

	Config cfg_;
	std::unique_ptr<vdoninja::VDONinjaSignaling> signaling_;
	std::map<std::string, std::shared_ptr<PeerSession>> peers_;
	std::mutex peersMutex_;
};

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

static void printUsage(const char *prog)
{
	fprintf(stderr, "Usage: %s --stream-id <id> [--password <pwd>]\n", prog);
	fprintf(stderr, "              [--wss wss://wss.vdo.ninja]\n");
	fprintf(stderr, "              [--width 320] [--height 240] [--fps 30]\n");
	fprintf(stderr, "\n");
	fprintf(stderr, "Publishes a VP9 test stream (primary + alpha) to VDO.Ninja.\n");
	fprintf(stderr, "Open the OBS plugin in native-receiver mode with the same stream-id\n");
	fprintf(stderr, "to verify BGRA output with transparency.\n");
}

int main(int argc, char **argv)
{
	Config cfg;
	bool hasStreamId = false;

	for (int i = 1; i < argc; ++i) {
		const std::string arg(argv[i]);
		if ((arg == "--stream-id" || arg == "-s") && i + 1 < argc) {
			cfg.streamId = argv[++i];
			hasStreamId = true;
		} else if ((arg == "--password" || arg == "-p") && i + 1 < argc) {
			cfg.password = argv[++i];
		} else if (arg == "--wss" && i + 1 < argc) {
			cfg.wssHost = argv[++i];
		} else if (arg == "--width" && i + 1 < argc) {
			cfg.width = std::stoi(argv[++i]);
		} else if (arg == "--height" && i + 1 < argc) {
			cfg.height = std::stoi(argv[++i]);
		} else if (arg == "--fps" && i + 1 < argc) {
			cfg.fps = std::stoi(argv[++i]);
		} else if (arg == "--help" || arg == "-h") {
			printUsage(argv[0]);
			return 0;
		}
	}

	if (!hasStreamId) {
		fprintf(stderr, "Error: --stream-id is required\n\n");
		printUsage(argv[0]);
		return 1;
	}

	// Clamp to sane values
	cfg.width = std::max(64, std::min(cfg.width, 1920)) & ~1;   // must be even
	cfg.height = std::max(64, std::min(cfg.height, 1080)) & ~1; // must be even
	cfg.fps = std::max(1, std::min(cfg.fps, 60));

	std::signal(SIGINT, signalHandler);
	std::signal(SIGTERM, signalHandler);

	LOG_INFO("VP9 alpha publisher starting");
	LOG_INFO("  stream-id : %s", cfg.streamId.c_str());
	LOG_INFO("  wss       : %s", cfg.wssHost.c_str());
	LOG_INFO("  resolution: %dx%d @ %d fps", cfg.width, cfg.height, cfg.fps);

	AlphaPublisher publisher(cfg);

	if (!publisher.start()) {
		return 1;
	}

	// Run the frame encoding loop (blocks until Ctrl+C)
	publisher.runFrameLoop();

	publisher.stop();
	LOG_INFO("Exiting");
	return 0;
}
