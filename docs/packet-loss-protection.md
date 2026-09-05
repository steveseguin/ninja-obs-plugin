# Packet-Loss Protection Reference

This page describes the native Ninja OBS Plugin publisher in v1.1.60 and later and its experimental loss-protection
settings. These settings affect each direct plugin-to-viewer WebRTC connection independently. They are not VDO.Ninja
URL parameters.

## Quick choice

Start with **Off**. First make sure the configured bitrate fits the sustained route and leaves upload headroom.

| Setting | What it protects | Approximate extra video traffic | When to consider it |
| --- | --- | --- | --- |
| `Off` | Automatic NACK repair only | None while the path is clean | Default and best compatibility; use this first |
| `Low` | A delayed copy of keyframe RTP packets | Up to 20% best effort | Isolated loss damages keyframes, but upload headroom is limited |
| `Medium` | Keyframes plus one quarter of delta packets | Up to 50% best effort | Measured random loss remains after lowering bitrate |
| `High` | Every video RTP packet can receive one delayed copy | Up to 100% best effort | Controlled testing on a lossy path with enough capacity to nearly double video traffic |

`High` is the closest mode to “send every video packet twice.” It still sends only one optional copy, and a copy can
expire rather than delay current media.

Packet duplication can make congestion worse. With direct peer-to-peer fan-out, apply the overhead to every viewer:

```text
approximate upload = (video bitrate + protection traffic + audio/overhead) x viewer count
```

## Recovery that is already automatic

Packet Duplication does not enable NACK. Video NACK repair is active independently of the selected duplication mode.

The publisher automatically provides:

- a sent-video history bounded by two seconds, 2,048 RTP packets, and 4 MiB;
- retransmission of the original RTP packet when a viewer NACK identifies a missing sequence number;
- paced repair traffic with a separate allowance and a 500 ms repair deadline;
- frame-aware pacing so a large keyframe does not monopolize the audio/video transport;
- whole-frame queue admission and decoder-safe gating after a confirmed local frame failure;
- a keyframe interval of no more than two seconds unless OBS is configured to ignore service recommendations;
- PLI/FIR, NACK, cache, repair, receiver-loss, jitter, RTT, REMB, queue, and keyframe diagnostics in the rolling
  `Publish:` log summary.

The publisher resends the original media RTP packet. It does not negotiate a separate RTX payload type or RTX SSRC.
NACK is reactive, so the request and repair must complete before the receiver's playout deadline. More receiver buffer
can give a repair time to arrive, at the cost of latency.

OBS does not expose a reliable on-demand keyframe request to this output. A PLI therefore waits for the next scheduled
live IDR instead of forcing an immediate encoder keyframe. The service's two-second keyframe clamp bounds the normal
wait, and the publisher does not replay a stale cached keyframe into an established stream.

## How packet duplication works

Packet duplication is proactive: it does not wait for loss feedback.

- The copy keeps the same RTP payload, timestamp, SSRC, and sequence number as the primary packet.
- It is delayed by 15 ms so the two transmissions are less likely to share the same short loss burst.
- It is lower priority than live media and NACK repair.
- It is sent only while live media is not backlogged.
- It expires 250 ms after the primary was sent.
- A two-second token budget limits sustained protection traffic.
- `Low` copies keyframe packets, `Medium` also selects every fourth delta packet, and `High` selects every packet.

The `High` scheduler includes a small internal packetization/keyframe-burst allowance so selected copies can reach the
wire before their deadline. This does not create more than one copy of a packet.

Because the duplicate uses the original RTP sequence number, this is not negotiated RTP RED, ULPFEC, FlexFEC, or RTX.
Compatible receivers treat the later packet as a duplicate if the first copy arrived and as the missing original if it
did not.

## Audio RED

**Audio RED (Experimental)** is separate from video packet duplication.

When enabled, the plugin offers RFC 2198 audio RED in addition to Opus. A viewer that selects the RED mapping receives
the current Opus frame and, when available, one previous Opus frame inside the current RED RTP payload. A viewer that
does not accept the mapping receives ordinary Opus. Negotiation and fallback happen per viewer.

Audio RED is default-off and adds audio bandwidth. It is not the same as Opus in-band FEC: Opus FEC is generated inside
the encoder, while RFC 2198 RED wraps encoded payload generations outside the encoder. The plugin does not configure
OBS's Opus packet-loss percentage or guarantee that OBS enabled in-band Opus FEC.

## Why video RED/ULPFEC is not offered

The plugin does not contain a native ULPFEC or FlexFEC packet generator, and libdatachannel does not provide one in the
media-handler path used by this publisher. Merely adding `red` and `ulpfec` payload types to SDP would advertise a
feature without producing valid repair packets.

There is also an H.264 interoperability concern. Current libwebrtc sender logic disables RED/ULPFEC when H.264-style
payloads are used with NACK because those payloads do not provide the picture-ID behavior libwebrtc uses to avoid
retransmitting FEC packets. In that combination, ULPFEC and its repair packets may themselves require retransmission,
which defeats much of the bandwidth benefit.

Browser capability or SDP output is therefore not proof of recovery. Shipping video FEC would require all of the
following:

1. a native RED/ULPFEC or FlexFEC generator with correct payload types, SSRCs, parity grouping, RTP sequencing, pacing,
   and RTCP accounting;
2. per-viewer SDP negotiation that falls back safely when the receiver does not accept it;
3. controlled induced-loss tests showing actual decoded-frame recovery in supported Chromium, Firefox, and WebKit
   receivers;
4. proof that the added repair traffic does not starve live media or worsen a congested route.

FlexFEC is the more promising future H.264 candidate because libwebrtc's H.264-with-NACK ULPFEC restriction does not
apply to FlexFEC. Until a native implementation passes recovery tests, the plugin accurately labels its video option as
**Packet Duplication**, not FEC.

VDO.Ninja browser URL options such as `&vred` and `&pvred` only influence browser SDP preference. They do not enable the
plugin's packet-duplication modes, and they do not make the plugin generate ULPFEC.

## Adaptive Bitrate from REMB

The RTP pacer retains a 4 Mbps minimum drain rate per viewer, even when the encoder adapts below that rate. This
lets large keyframes finish without repeatedly exhausting the playback buffer. It does not raise the configured
encoder bitrate, but short packet trains can exceed that average rate. Packets remain spaced and all viewers share
the existing 4 KB burst limit. A route still needs enough capacity for the actual encoded media and protection traffic.

**Adaptive Bitrate from REMB (Experimental)** is congestion avoidance, not packet repair. When the active OBS encoder
supports live bitrate changes, the plugin uses the lowest fresh REMB estimate across connected viewers, decreases in
stages, increases conservatively, respects the configured minimum, and restores the original encoder bitrate when the
stream stops.

Use adaptive bitrate when a fixed media rate exceeds a viewer's sustainable route. Use duplication or audio RED only
when the route has spare capacity and the remaining problem is isolated packet loss.

## Receiver-path limitations

The normal `VDO.Ninja Source` mode embeds the browser receiver and inherits the browser's jitter buffer, NACK/RTX, PLI,
and codec behavior.

The optional native receiver is narrower:

- it sends PLI for startup and decoder recovery but does not generate video NACK;
- it can normalize an incoming RTX packet when one is offered, but it does not request that packet itself;
- it extracts the primary payload from video RED but does not use redundant RED blocks or ULPFEC for repair;
- it has no plugin-level RTP jitter/reorder buffer.

Use the browser-backed receiver unless native VP9 alpha or another native-only feature is required.

## Reading the log

Use the rolling `Publish:` summary to distinguish the failure type:

| Observation | Likely meaning |
| --- | --- |
| Local pacer/media drops increase | The publisher is overloaded or its output queue cannot keep up |
| NACK and cache hits increase, with repairs sent | Network loss occurred and reactive repair was available |
| NACK cache misses increase | The request was outside the two-second/packet/byte history or referenced an unavailable packet |
| Repair expiry increases | The repair could not be sent inside its 500 ms deadline |
| PLI/FIR increases after NACK | Repair was absent, late, or insufficient to preserve decoder state |
| REMB stays below the configured bitrate | Lower bitrate, reduce fan-out, or test adaptive bitrate |
| Duplicate expiry or queue pressure increases | Reduce or disable packet duplication |

Test one setting at a time on the real application/browser runtime and route. A clean local recording plus damaged
remote playback points toward transport recovery; damage in the local recording points toward capture or encoding.

## Protocol references

- [RFC 4585: RTP feedback, NACK, and PLI](https://www.rfc-editor.org/rfc/rfc4585)
- [RFC 4588: RTP retransmission payload format](https://www.rfc-editor.org/rfc/rfc4588)
- [RFC 2198: RTP redundant payloads](https://www.rfc-editor.org/rfc/rfc2198)
- [RFC 5109: Generic RTP forward error correction](https://www.rfc-editor.org/rfc/rfc5109)
- [libwebrtc video sender RED/ULPFEC and FlexFEC selection](https://chromium.googlesource.com/external/webrtc/+/master/call/rtp_video_sender.cc)
- [libwebrtc video receiver RED/ULPFEC path](https://chromium.googlesource.com/external/webrtc/+/master/video/rtp_video_stream_receiver.cc)
