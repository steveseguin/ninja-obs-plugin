# VDO.Ninja Workflow Map

Last reviewed: 2026-06-30 after parser fuzzing and JSON array-scalar
hardening, official remote rotate-command alignment,
combined close-payload cleanup priority, official cleanup spelling alignment,
structured media-control fanout, typed peer cleanup dispatch, official peer
cleanup, hangup, liveness, room-state, stats authorization, screen-share state,
director-controlled audio/video/transform state, recovery-control,
mesh-control, game-capture protocol cross-reference, unsupported control-center
rejection, remote-control auth-shape tightening, director-only control
rejection, publisher-wide recovery fan-out, signaling-shape alignment,
native-source dispatch-alignment, director-only mesh request rejection,
transform-command rejection, unsupported-control precedence, and active-control
plus telemetry-response precedence, keyframe field-fanout, unsupported
keyframe-rate/isolation-volume control passes, terminal-message keyframe fanout
gating, and active-field ordering for unsupported/recovery controls

Purpose: keep an AI-friendly, code-free map of how signaling, peer state, video,
audio, data channels, and teardown currently flow through the plugin. This is
not user documentation. It is a review aid for finding duplicated calls, missing
state gates, stale sessions, callback lifetime bugs, and bad workflow edges.

## Map Format

This document uses a compact FlowMap style:

- `Actor`: long-lived component that owns state or effects.
- `State`: durable state that changes workflow decisions.
- `Event`: incoming trigger.
- `Gate`: condition that blocks, allows, retries, or deduplicates an event.
- `Edge`: one workflow transition.
- `Invariant`: rule that should remain true across refactors.
- `Hazard`: known or suspected workflow risk.

This format is intentionally lighter than UML and more explicit than prose. It
should be updated when the source changes, especially after changes to:

- `src/vdoninja-output.*`
- `src/vdoninja-signaling.*`
- `src/vdoninja-peer-manager.*`
- `src/vdoninja-source.*`
- `src/vdoninja-data-channel.*`
- `src/vdoninja-common.h`

Options considered:

- Mermaid sequence diagrams: readable for one happy path, weak for state gates
  and retries.
- UML/statecharts: precise, but too heavy to keep current during rapid review.
- Pseudocode: easy for an agent to write, but too close to implementation and
  too likely to hide the architecture under procedural detail.
- FlowMap: chosen because it is stable, searchable, compact, and can represent
  actors, events, state, gates, invariants, and hazards without copying code.

## Source Of Truth

Recent review window:

- Base before last-month changes: `86ebcc6 Add signed macOS installer packaging`
- Reviewed through: `c02b01c Release v1.1.48`
- Main changed workflow areas:
  - deferred WebRTC peer cleanup
  - publish media send threading
  - output/source teardown hardening
  - duplicate offer request suppression
  - native receiver retry and decode queue bounds
  - browser/native child source lifecycle
  - OBS smoke checks

Read-only protocol cross-reference:

- `C:\Users\steve\Code\game-capture\docs\native-streaming-flow-map.md` was
  reviewed as an external native-client interpretation of VDO.Ninja behavior.
  It adds useful Control Center detail that this map did not previously carry:
  settings readback uses `getAudioSettings` / `getVideoSettings` with
  `audioOptions`, `videoOptions`, and `mediaDevices`; video settings mutation
  uses privileged `requestVideoHack`; data-channel callback completion uses
  `cbid`; `requestResolution` has `{w,h,s,c}` semantics; `bitrate` /
  `audioBitrate` route-disable semantics are distinct from sender target caps;
  and browser-only controls should receive VDO-shaped `rejected` responses
  rather than being silently ignored.
- The same cross-reference clarifies recovery scope: peer
  `iceRestartRequest` is unprivileged and scoped to the resolved/bound peer,
  while Control Center `refreshConnection` / `refreshAll` are privileged
  publisher-wide recovery controls that restart every current publisher peer
  connection after authorization.
- This OBS plugin currently has only an `enableRemote` flag and no stored
  VDO-compatible director identity or `remote` token value. Therefore the code
  can align message shapes, peer recovery, rejection behavior, and supported
  OBS-native side effects, but it cannot yet fully implement the official
  director-vs-remote-token authorization split without adding settings/state
  for that model.

## Actor Index

Actor: OBS output wrapper

- Source: `VDONinjaOutput`
- Owns OBS output lifecycle, settings snapshot, encoded packet callbacks,
  publisher signaling setup, publisher peer manager setup, media-send worker,
  latest keyframe cache, data-channel dispatch, and auto-inbound orchestration.

Actor: OBS VDO.Ninja service and control surfaces

- Source: `plugin-main.*`, `vdoninja-dock.*`
- Owns OBS service registration, service settings, stream-key compatibility,
  generated push/view URLs, profile encoder configuration, control-center start
  and stop actions, remote-control execution, and temporary service restore.

Actor: signaling client

- Source: `VDONinjaSignaling`
- Owns WebSocket connection thread, send queue, current room, published stream,
  viewing stream records, signaling callback dispatch, reconnect/fallback state,
  password/salt encryption context, and VDO.Ninja message normalization.

Actor: peer manager

- Source: `VDONinjaPeerManager`
- Owns libdatachannel peer connections, publisher/viewer peer records, local and
  remote SDP handling, ICE candidate routing, media track setup, media sending,
  data-channel wiring, deferred peer cleanup, and viewer slot counting.

Actor: auto-inbound scene manager

- Source: `VDOAutoSceneManager`
- Owns automatic browser-source creation/removal for inbound room members,
  own-stream filtering, target-scene placement, optional scene switching, and
  grid layout refreshes queued to the OBS UI thread.

Actor: publisher peer

- Source: `PeerInfo` with `type = Publisher`
- Means OBS is sending media to a remote VDO.Ninja viewer.
- Has send-only video/audio tracks and optional `sendChannel` data channel.

Actor: native viewer peer

- Source: `PeerInfo` with `type = Viewer`
- Means OBS is receiving media from a remote VDO.Ninja publisher.
- Has recv-only video, optional alpha video, audio tracks, and optional
  signaling/data channel paths.

Actor: VDO.Ninja source wrapper

- Source: public `VDONinjaSource`
- Default ingest path. Owns a private browser source child, or a private native
  receiver child when experimental native mode is selected.

Actor: internal native receiver source

- Source: internal `vdoninja_native_source`
- Owns native signaling, native peer manager, track callbacks, RTP parsing,
  video/audio decoding, alpha composition, OBS async video output, OBS audio
  output, and retry state.

Actor: data channel handler

- Source: `VDONinjaDataChannel`
- Parses chat, tally, mute, OBS/source state, media control, screen-share state,
  director-video state, director audio/display state, director transform state,
  recovery control, mesh control, stats, stats requests, keyframe request,
  ping/pong liveness, hangup, remote-control, custom messages, and inbound
  playback hints.

## Identity And Session Model

Identity values:

- `streamId`: user-visible push/view id.
- `hashedStreamId`: VDO.Ninja-compatible stream id after password/salt hashing.
- `roomId`: user-visible room id.
- `hashedRoomId`: VDO.Ninja-compatible room id after password/salt hashing.
- `UUID`: signaling peer identity from VDO.Ninja messages.
- `session`: per-negotiation/session token used to route SDP and ICE.
- `mid`: media section identity for audio/video/alpha matching.

Session rules:

- Signaling messages may arrive encrypted or plaintext.
- Password candidates are derived from the published stream, viewing streams,
  current room, and default password.
- Publisher peers are keyed by remote viewer `UUID`.
- Native viewer peers are keyed by remote publisher/media `UUID`.
- A publisher peer is recreated when an offer request arrives with a rotated
  non-empty session or when the old peer is already terminal.
- Duplicate same-session publisher offer requests never re-enter local-offer
  generation; they resend a cached offer when one exists and the peer is still
  negotiating.

Invariant:

- `UUID + session` identifies a negotiation path more precisely than `UUID`
  alone.
- Any answer, ICE candidate, data-channel envelope, cleanup, or retry decision
  that uses only `UUID` should be reviewed for stale-session behavior.

## State Index

Output state:

- `running`: OBS output lifecycle is started.
- `connected`: signaling is connected and publishing has been seeded; not the
  same as having at least one viewer.
- `capturing`: OBS encoded packet capture has begun.
- `mediaSendWorkerRunning`: worker accepts queued audio/video frames.
- `cachedKeyframe`: latest encoded video keyframe for fast viewer warm-up.
- `selectedAudioTrackIdx`: one OBS encoded audio track selected for publishing.

Signaling state:

- `shouldRun`: WebSocket thread should keep running.
- `connected`: current WebSocket is open.
- `needsReconnect`: current connection should end and attempt reconnect.
- `initialConnectionFinished`: `connect()` may return.
- `currentRoom`: room membership and encryption context. The member vector is
  the current known room stream list, seeded by `listing`/`transferred` and kept
  current by incremental stream add/remove events.
- `publishedStream`: active seed/publish record and encryption context.
- `viewingStreams`: active play/view records and encryption context.
- `reconnectSuppressedByServer`: server alert blocks reconnect.
- `reconnectDeferredUntilMs`: server alert delays reconnect.
- `sendQueue`: pending outbound signaling messages.
- Queue lifetime rule: stale queued messages are cleared when starting a fresh
  connection, when disconnecting, and when a WebSocket attempt ends before an
  automatic reconnect.

Peer state:

- `ConnectionState.New`
- `ConnectionState.Connecting`
- `ConnectionState.Connected`
- `ConnectionState.Disconnected`
- `ConnectionState.Failed`
- `ConnectionState.Closed`
- `terminalStateTimeMs`: when a terminal state was observed.
- `disconnectNotified`: prevents repeated disconnect callback delivery.
- `cleanupRetired`: peer was removed from active map and moved to deferred
  cleanup.
- `localOfferRequested`: publisher offer generation has already been requested
  for this peer/session.
- `lastLocalOfferSdp`: cached local offer used to resend same-session retries
  without re-entering local-offer generation.
- `negotiationMutex`: protects cached negotiation text.
- `mediaMutex`: protects per-peer media track snapshots, packet sequence state,
  keyframe gate state, and release of media resources.
- `awaitingVideoKeyframe`: publisher will drop delta frames to this viewer until
  a keyframe is sent.
- `hasDataChannel`: peer has a data channel path.

Source wrapper state:

- `active`: OBS source activation state.
- `showing`: OBS source visibility state.
- `browserSource`: private child for browser-backed ingest.
- `nativeReceiverSource`: private child for native ingest.
- `childActive` and `childShowing`: refcount mirror for private child lifecycle.
- child config caches: URL, width, height, source settings.

Native receiver state:

- `nativeRunning`: connection thread and media callbacks should process work.
- `connected`: native receiver has at least one active media peer/track.
- `videoTrack`, `alphaVideoTrack`, `audioTrack`: active libdatachannel tracks.
- `videoTrackPeerUuid`, `alphaVideoTrackPeerUuid`, `audioTrackPeerUuid`: peer
  ownership of active tracks.
- decode state: decoder contexts, RTP assembly buffers, timestamp maps,
  pending alpha frames, pending decode timestamp queues.
- retry state: view retry count, last view request time, next retry time,
  awaiting peer connection, retry suppression flag.
- output state: last packet times, keyframe request time, video output active.
- remote mute/suppression state: `remoteAudioMuted` suppresses native audio
  output and marks OBS source audio inactive. Native video suppression is split
  into `remoteMediaVideoMuted` for publisher media state
  (`info.video_muted_init`, `muteState`, `videoMuted`),
  `remoteDirectorVideoMuted` for director video-hide state
  (`info.directorVideoMuted`, `directVideoMuted`), and `remoteVirtualHangup`
  for official virtual video suppression. `remoteVideoMuted` is the derived
  effective render gate. Any active video-suppression flag clears and
  suppresses native video output until all active flags are cleared. This is
  distinct from top-level director control `remoteVideoMuted`, which asks a
  publisher to suppress outgoing video.

OBS service/control state:

- VDO.Ninja service type: `vdoninja_service`.
- VDO.Ninja output type: `vdoninja_output`.
- Publishing codec contract: H.264 video, Opus audio.
- Native receive codec contract: VP9 or H.264 video, optional dual-track VP9
  alpha, Opus audio.
- Temporary service restore: control-center publish can swap to VDO.Ninja and
  restore the previous service after streaming stops.
- Frontend events: ensure service exists on load/profile change, apply VDO.Ninja
  service before streaming starts, tear down dock on exit.

Auto-inbound state:

- `running`: room automation is active.
- `ownStreamIds`: local stream ids and hashes ignored during room listing.
- `managedStreamIds`: inbound streams currently managed as OBS sources.
- `removeOnDisconnect`: controls whether removed streams delete source or hide
  scene item.
- `layoutMode`: grid layout refresh is queued after additions/removals.

Data-channel state:

- Handler callbacks: chat, tally, mute, custom, keyframe, remote-control.
  Output-owned dispatch handles stats requests and hangup after parsing.
- Parser priority: official VDO.Ninja handles data-channel messages as
  field-based objects, while this plugin returns one primary `DataMessageType`.
  To preserve official behavior, transport/liveness/cleanup/signaling fields and
  active controls that require a reply or side effect must outrank passive state
  buckets. `UnsupportedControl`, `RecoveryControl`, `Hangup`, `StatsRequest`,
  accepted `RemoteControl` shapes, and stats response telemetry are classified
  before passive mute, OBS/source, screen-share, director-video, director-audio,
  and transform state.
- Field fanout: keyframe is also detected by field presence outside the primary
  parser type. Official VDO.Ninja checks `keyframe` after stats handling in the
  same message object, so a combined stats/keyframe payload must both send stats
  and prime the requesting peer with the cached keyframe.
- Peer tally cache: per-peer tally state used for aggregate output tally.
- Peer stats cache: output-owned last stats payload/timestamp by peer UUID.
- Stats input: official VDO.Ninja stats responses arrive as `remoteStats`;
  legacy/plugin-level `stats` payloads are accepted for compatibility.
- Stats request: official viewers/directors can ask publishers for stats with
  `requestStats` or `requestStatsContinuous`; the plugin answers with
  `remoteStats`. Rich native peer runtime snapshots require output
  `enableRemote`; otherwise the plugin returns an empty official-shaped
  `remoteStats` object and does not start a continuous stats subscription.
- Continuous stats subscribers: per-viewer UUID set owned by output. A truthy
  `requestStatsContinuous` adds the viewer only when `enableRemote` is enabled;
  false removes it, peer disconnect removes it, and output stop clears the set.
- Direct signaling input: official peers can send `description`, `candidate`,
  and `candidates` over an already-open data channel. Publisher output and
  native/source receive paths prepare missing sender `UUID` from the
  data-channel peer before routing through the same signaling parser used for
  WebSocket messages.
- Liveness input: official disconnected-peer recovery sends `ping` with a token
  over publisher and viewer/native data channels and expects `pong` with the
  same JSON token type. Numeric tokens must stay numeric because the official
  side compares them with strict equality before deciding whether to recover or
  close the peer.
- ICE restart input: official recovery can send `iceRestartRequest` over the
  data channel or WebSocket. Direct data-channel handling is presence-based and
  terminal, matching official `webrtc.js`; WebSocket signaling remains truthy
  and UUID-scoped. The plugin treats accepted requests as publisher-peer
  re-offer requests for the existing `UUID`/session when the peer is still live
  and signaling state is stable.
- Hangup input: official remote/director endpoint-stop requests normally arrive
  as `hangup:true`, but official data-channel handling checks field presence.
  This is endpoint-stop control, not peer cleanup. The native publisher output
  currently rejects it because the plugin has no director identity model.
- Screen-share state input: official peers can advertise initial screen-share
  status as `info.screenShareState`, live screen-share status as
  `screenShareState`, and screen stop state as `screenStopped`. The current
  plugin parser preserves this as known state but does not attach browser-style
  mixer/UI side effects.
- Director-video state input: official peers can advertise initial director
  video-hide status as `info.directorVideoMuted`, live director video-hide
  status as `directVideoMuted`, and director virtual video suppression as
  `virtualHangup`. A `directVideoMuted` payload can include `target:true` for
  self-targeting or a target UUID string for another peer. Official director
  publisher control can also send `remoteVideoMuted` to ask the publisher to
  suppress or resume its outgoing video. Native source receive applies
  receiver-side `info.directorVideoMuted`, self-targeted or locally matched
  `directVideoMuted`, and `virtualHangup` to its split video-suppression state.
  A string-targeted `directVideoMuted` update for another UUID is ignored by the
  single-source native receiver render gate, matching the browser behavior where
  that form updates another peer entry instead of the recipient itself. This is
  target scoping only; the native receiver still lacks the browser
  `directorList` authority check for live director updates. Native publisher
  output treats top-level `remoteVideoMuted` as director-only and rejects it
  until a director identity model exists.
- Director audio/display state input: official peers can advertise initial
  director speaker/display mute status as `info.directorSpeakerMuted` and
  `info.directorDisplayMuted`. Live director controls use `speakerMute` and
  `displayMute`; native publisher output treats those as director-only and
  rejects them without director identity.
- Director transform state input: official peers can advertise initial director
  mirror/flip/rotation status as `info.directorMirror`, `info.directorFlip`,
  and `info.rotate_video`, while live advertised rotation state can arrive as
  top-level `rotate_video`. Live director mirror control uses
  `mirrorGuestState` plus `mirrorGuestTarget`; live remote rotate control uses
  top-level `rotate`. Native publisher output rejects those live transform
  commands because it cannot authenticate director identity and does not own a
  browser video-transform surface.
- Playback hints: inbound URLs discovered from VDO.Ninja data-channel payloads.
- Transport bindings: native viewer maps a transport data channel to a target
  media peer by `UUID + session`.

## Thread And Ownership Map

Thread/context: OBS frontend/UI thread

- Owns OBS scene/source creation, removal, visibility, frontend streaming,
  profile service swaps, dock updates, and remote-control effects.
- May call output/source start, stop, update, activate, deactivate, show, hide,
  and destroy.
- Must not wait on RTC callbacks while holding OBS UI-affine resources that a
  queued callback needs.

Thread/context: OBS encoder callback

- Calls output encoded packet handler.
- Must only copy/cache packets and enqueue media work.
- Must not perform WebSocket, RTC, OBS UI, or blocking send work.

Thread/context: output start/stop worker

- Initializes signaling, peer manager, callbacks, auto-inbound, and signaling
  connection.
- Output stop serializes against start/destroy through `startStopMutex`.
- Stop owns callback clearing, publishing stop, signaling disconnect, data
  capture end, media worker stop, and keyframe/timestamp cache reset.

Thread/context: output media-send worker

- Owns draining the bounded encoded media queue.
- May call peer-manager audio/video send only when output is still marked
  connected.
- Peer-manager media send snapshots peers and packet state before calling RTC
  track sends.

Thread/context: output remote-stats worker

- Owns repeating `remoteStats` snapshots for viewers that requested
  `requestStatsContinuous:true`.
- Sleeps while there are no subscribers, wakes every 3000 ms while subscribed,
  snapshots subscriber UUIDs under its own lock, and sends after releasing that
  lock.
- Stops before peer publishing teardown so it cannot send into retired peer
  state during output stop.

Thread/context: signaling WebSocket thread and callbacks

- Owns socket open/close/error/message callbacks, send queue draining,
  reconnect/fallback loop, and signaling callback dispatch.
- Signaling state is protected by `stateMutex`; callback members by
  `callbackMutex`; queued outbound messages by `sendMutex`.
- Callback handlers must assume duplicate, stale, and session-rotated messages.

Thread/context: libdatachannel callbacks

- Own peer state changes, local descriptions, local ICE, incoming tracks, data
  channel open/message, and track media packets.
- Must not synchronously destroy PeerConnection/Track/DataChannel objects from
  their own callbacks.
- Must not call external RTC sends while holding the peer map lock.

Thread/context: native receiver connection thread

- Owns connect loop, native view retries, and retired-peer cleanup.
- Native media callbacks can arrive on RTC threads and must gate on
  `nativeRunning` plus current track ownership before parsing or decoding.

State meaning guardrail:

- `signaling.connected`: WebSocket is open.
- output `connected`: publish seed path was attempted and data capture may run.
- peer `Connected`: RTC connection reached connected state.
- media flowing: packets/decoded frames are actually observed.
- Review any user-facing status or retry decision that treats these as the same
  state.

## OBS Service And Codec Contract

Flow: service registration and setup

1. Event: plugin loads.
2. Edge: register VDO.Ninja output.
3. Edge: register public source and internal native receiver source.
4. Edge: register VDO.Ninja service pointing at the VDO.Ninja output.
5. Edge: register control center source and dock.
6. Edge: register frontend event callback.
7. Event: OBS finishes loading or profile changes.
8. Edge: ensure RTMP catalog has a visible VDO.Ninja entry.
9. Edge: ensure a VDO.Ninja streaming service can be selected.

Flow: publish service selection

1. Event: user/config/control center applies VDO.Ninja service settings.
2. Edge: settings are normalized into stream id, room id, password, signaling
   host, salt, ICE, TURN, max viewers.
3. Gate: stream id is required before OBS can try to connect.
4. Edge: OBS service exposes VDO.Ninja output type.
5. Contract: publisher output supports H.264 video and Opus audio today.
6. Contract: native receiver VP9 support does not imply VP9 publisher output.

Flow: control center publish run

1. Event: user starts publishing from control center.
2. Gate: if current service is not VDO.Ninja, previous service is captured.
3. Edge: VDO.Ninja service settings are applied and saved.
4. Edge: profile streaming audio encoder is configured to Opus where possible.
5. Edge: OBS frontend streaming starts.
6. Event: OBS streaming stops.
7. Edge: temporary prior service is restored when the run was temporary.

Flow: remote control over data channel

1. Event: publisher receives remote-control data-channel message.
2. Gate: OBS command/action messages must include VDO's top-level `remote`
   field. Bare `obsCommand` or bare `action` is rejected as `obsCommand`.
3. Gate: output setting `enableRemote` must be true before accepted
   remote-control effects run.
4. Edge: remote action is queued onto OBS UI thread.
5. Supported effects:
   - next/previous scene.
   - set scene by name.
   - start/stop streaming.
   - start/stop recording.
   - start/stop virtual camera.
   - mute/unmute desktop audio source.

## Signaling Protocol Flow

Official VDO.Ninja signaling contract checked against
`C:\Users\steve\Code\vdoninja\webrtc.js` and
`C:\Users\steve\Code\vdoninja\hss\gemini.js`:

- Publish/seed sends `{request:"seed", streamID}`.
- View/play sends `{request:"play", streamID}`.
- Room join sends `{request:"joinroom", roomid}` plus optional `claim`.
- Publisher offer sends `{UUID, streamID, session, description}` where
  `description` is either an SDP object containing `type` and `sdp`, or an
  encrypted string plus `vector`.
- Viewer answer sends `{UUID, session, description}` with the same
  `description` object/encrypted-string rule.
- Publisher-side ICE candidates sent toward viewers use `type:"local"` and
  carry `{UUID, type, session, candidates}` or encrypted `candidates` plus
  `vector`.
- Viewer-side ICE candidates sent toward publishers use `type:"remote"` and
  carry `{UUID, type, session, candidates}` or encrypted `candidates` plus
  `vector`.
- Single-candidate `candidate` messages are accepted by official VDO.Ninja and
  the plugin parser, but official current VDO.Ninja emits bundled candidates
  under `candidates`.
- Direct data-channel messages are JSON payloads sent as-is on `sendChannel` or
  `receiveChannel`; targeted signaling-over-data-channel still needs
  `UUID + session` so the receiver can bind it to the correct media peer.
- Official direct data-channel signaling is processed before app/control
  messages. VDO.Ninja assigns the sender `UUID` to the received object before
  handling `description`, `candidate`, or `candidates`; the plugin mirrors this
  when an inbound data-channel signaling object lacks an explicit `UUID`.
- Official keyframe request data-channel payload is `{keyframe:true}`.
- Official stats responses to `requestStats` and `requestStatsContinuous` use
  the data-channel field `remoteStats`.
- Official `requestStatsContinuous:true` starts a repeating stats interval and
  sends an immediate snapshot; `requestStatsContinuous:false` clears that
  interval. The plugin mirrors this with an output-owned subscriber worker.
- Official disconnected-peer recovery sends data-channel `{ping:<token>}` and
  expects `{pong:<same token>}` before deciding that the peer needs recovery or
  close. The token is commonly a numeric `Date.now()` value and should not be
  stringified.
- Official disconnected-peer recovery can also send `iceRestartRequest` either
  over the data channel or as a WebSocket message with `UUID`. Browser
  VDO.Ninja calls `restartIce()` when available or creates a fresh offer with
  ICE restart intent.
- Official peer close uses a top-level data-channel `bye` field, normally
  `{bye:true}`, and receivers call the peer cleanup/close path immediately on
  field presence. Official `lib.js` can send close as
  `{videoMuted:true,bye:true}`, so data-channel cleanup must be classified
  before mute/video state. WebSocket signaling cleanup can arrive as truthy
  `bye` or `request:"cleanup"`. A false `bye` field is not cleanup in the
  official WebSocket receive path. Official `webrtc.js` / `lib.js` do not use
  `request:"bye"` as a cleanup spelling.
- Official remote/director hangup uses data-channel `hangup`, commonly
  `{hangup:true}`, and browser VDO.Ninja calls its local hangup/disconnect path
  when authorized. The receiver checks field presence, so this is endpoint stop
  control, not the same as `bye`. The OBS native publisher currently rejects it
  because remote-token authorization is not the same as director identity.
- Official OBS/tally-style source state is carried in `obsState` with
  `visibility`, `sourceActive`, `streaming`, `recording`, `virtualcam`, and
  `details`. Related scene-state shims can also arrive as top-level
  `sceneDisplay` and `sceneMute`. The plugin-local `tallyOn`, `tallyPreview`,
  `tallyOff`, and `audioMuted` helpers are compatibility payloads, not official
  VDO.Ninja signaling objects.
- Official viewer-driven media control uses data-channel `bitrate`,
  `audioBitrate`, `targetBitrate`, `targetAudioBitrate`,
  `optimizedBitrate`, and `requestResolution`. In browser VDO.Ninja,
  `bitrate:0` and `audioBitrate:0` make the corresponding RTCRtpSender
  inactive; negative or positive values make it active.
- Official screen-share state is split between initial peer info and live
  state updates. Browser VDO.Ninja advertises initial status through
  `info.screenShareState`, sends live status through top-level
  `screenShareState`, and sends screen-stop state through top-level
  `screenStopped`.
- Official director-video state is also split between initial peer info and
  live director control updates. Browser VDO.Ninja advertises initial director
  video-hide state through `info.directorVideoMuted`, sends live director
  video-hide updates through top-level `directVideoMuted`, and sends director
  virtual video suppression through top-level `virtualHangup`. `directVideoMuted`
  may carry `target:true` when the recipient itself is the target, or a target
  UUID string when the recipient should update another peer's state. The native
  OBS source has one render target, so it applies `target:true`, no-target
  passive/compatibility updates, and string-targeted updates only when the string
  names the local state peer. This does not prove the sender is a director; the
  native receiver currently has no official `directorList` equivalent.
- Official publisher-side remote video mute is top-level `remoteVideoMuted`.
  Browser VDO.Ninja applies it to the publishing endpoint's local
  `remoteVideoMuted` state, toggles video mute handling, and sends top-level
  `videoMuted` state back out to peers.
- Official director speaker/display mute state uses the same initial-info plus
  live-control pattern. Browser VDO.Ninja advertises initial state through
  `info.directorSpeakerMuted` and `info.directorDisplayMuted`, then applies
  director controls through top-level `speakerMute` and `displayMute`.
- Official director transform state also uses initial peer info and live
  director control updates. Browser VDO.Ninja advertises initial mirror/flip
  state through `info.directorMirror` and `info.directorFlip`, and advertises
  current rotation through `info.rotate_video`. Live mirror changes use
  top-level `mirrorGuestState` plus `mirrorGuestTarget`. Live remote rotate
  commands arrive as top-level `rotate` with `true` meaning rotate/toggle,
  `false` meaning reset, and an integer meaning an absolute requested angle.
  Live advertised rotation state arrives as top-level `rotate_video`.
  `mirrorGuestTarget:true` means the remote sender itself is the target; a
  target UUID string means another peer should update its view of that guest.
- Official recovery controls are direct data-channel fields. Browser
  VDO.Ninja sends `refreshVideo`, `refreshMicrophone`, `refreshConnection`,
  `refreshAll`, and `restartWhip` from director/remote control surfaces.
  Browser receiver handling checks field presence, not boolean truthiness:
  `{"refreshVideo":false}` is still a refresh request. Browser receivers treat
  `refreshMicrophone` and `refreshVideo` as local device refreshes,
  `refreshConnection` as ICE restart for peer connections, `refreshAll` as
  media-device refresh plus ICE restart, and `restartWhip` as WHIP publishing
  reconnect only.
- Official mesh controls are direct data-channel fields. Browser VDO.Ninja
  sends `reconnectPeer` to ask one guest to reconnect to a named peer, sends
  `getConnectionMap` to request mesh-visualization state, and returns
  `connectionMap` with `uuid`, `streamID`, `label`, `browser`, `connections`,
  and `requesterUUID`. Browser VDO.Ninja rejects `reconnectPeer` and
  `getConnectionMap` from non-director senders; the OBS native publisher has no
  director identity model, so it rejects those request fields.
- Official mute state is split: initial audio mute state is advertised through
  `info.muted`, initial video mute state through `info.video_muted_init`,
  dynamic audio mute updates use top-level `muteState`, and dynamic video mute
  state uses top-level `videoMuted`.
- Official room admission responses use `request:"listing"` for normal joins
  and `request:"transferred"` for room-transfer joins. Both carry a `list` of
  current room members/streams and should update local room membership state.
- Official room incremental join notifications use `request:"someonejoined"`
  with `UUID` and optional `streamID`. When `streamID` is present, the plugin
  treats it as a stream-added event for auto-inbound and native retry flows.
- Current official client/server teardown does not define outbound WebSocket
  `leaveroom` or `stopPlay` requests. Disconnect/cleanup flows are driven by
  socket close, server `cleanup`, and peer/data-channel `bye` handling.

Protocol guardrail:

- Do not add redundant top-level SDP fields such as `sdp` or offer/answer
  `type` beside `description`. Those fields belong inside `description`.
- Do not add generic WebSocket objects such as `{UUID,data}` unless the
  official VDO.Ninja side already consumes that shape for the intended path.
- Do not invent teardown request names. If a local method clears plugin state,
  keep it local unless the official client/server path has a matching request.
- Do not conflate `hangup` with `bye`: official `hangup` asks this endpoint to
  stop itself when the sender is authorized, while `bye`/`cleanup` removes or
  retires a peer. The OBS native publisher rejects `hangup` until director
  identity is modeled.
- ICE `type` is directional, not a constant: publisher peers send `local`;
  native viewer peers send `remote`.
- When exposing OBS state to browser viewers, prefer the official `obsState`
  shape over older plugin-local tally/mute convenience payloads. When receiving
  `obsState`, preserve it as source/OBS state; do not reinterpret
  `visibility:false` or `sourceActive:false` as media mute unless a separate
  official mute or bitrate/optimization path says to do that.
- For plugin publisher output, do not pretend full browser-style per-peer
  bitrate scaling exists unless the OBS encoder/RTP path actually supports it.
  The safe currently implemented subset is active/inactive send gating:
  `bitrate:0` suppresses video RTP to that peer, `audioBitrate:0` suppresses
  audio RTP to that peer, and nonzero values re-enable sends.
- Publisher input extracts media-control fields with the structured
  media-control helper independently from the primary parser type. This mirrors
  browser VDO.Ninja's field-by-field handling after `manageSceneState`, lets
  combined top-level state plus bitrate messages work, and avoids treating
  nested stats fields as media-control requests.
- Keep screen-share state separate from media mute and bitrate control.
  `screenShareState:false` is not equivalent to `videoMuted:true`, and
  `screenStopped:true` is not equivalent to peer `bye` unless a specific
  receiver-side screen-share peer model implements that transition.
- Keep director-video state separate from endpoint hangup and peer cleanup.
  `virtualHangup` is an official video-suppression state field, while
  data-channel `hangup` is a director endpoint-stop request and `bye:true`
  retires a peer. Native publisher output rejects director endpoint-stop
  requests until it can authenticate director identity.
- Treat publisher-side `remoteVideoMuted` as director-only until the plugin has
  a real director identity model. It changes the publisher's outgoing video
  state globally and is not the same as a receiver reporting top-level
  `videoMuted`; without director identity, reject it.
- Keep director speaker/display mute separate from publisher mute state.
  `speakerMute` / `displayMute` are director-controlled receiver/output state;
  `muteState` and `videoMuted` are publisher media mute state. Live
  `speakerMute` / `displayMute` commands are rejected by native publisher
  output because the plugin cannot prove director identity.
- Keep director mirror/flip state separate from media mute, hangup, and
  media availability. `info.directorMirror` / `info.directorFlip` and
  `rotate_video` are passive transform state, not media availability state.
  Live transform commands `mirrorGuestState` / `mirrorGuestTarget` and
  `rotate` require director/remote authorization and native implementation;
  reject them until both exist.
- Gate recovery controls behind explicit remote-control permission when they
  affect OBS publisher behavior. `refreshConnection` / `refreshAll` are repair
  requests, not peer cleanup or endpoint hangup, and official Control Center
  scope is publisher-wide rather than requester-only. `restartWhip` is not
  actionable for the native OBS publisher output unless a WHIP publisher owner
  is added, so unsupported/disabled recovery fields should produce VDO-shaped
  `rejected` responses.
- Gate mesh request controls behind director identity, not the broad
  remote-control flag. `reconnectPeer` is a repair request for a named peer,
  not endpoint hangup. `getConnectionMap` is diagnostics for director mesh
  visualization, not media state. Until the plugin models director identity,
  native publisher output rejects both request fields and only recognizes
  `connectionMap` as a response payload.

Data-channel message matrix:

- `chat` / `chatMessage`: app-level chat payload; parsed by the handler and
  forwarded to dock UI callbacks.
- `keyframe`: official keyframe request. `requestKeyframe` is accepted as
  compatibility input only.
- `remoteStats`: official stats response from a browser publisher/viewer.
- `stats`: legacy/plugin-level stats response accepted for compatibility.
- `requestStats`: official immediate stats request; plugin responds with
  `remoteStats`. The rich native peer runtime snapshot requires output
  `enableRemote`; otherwise the response is `{remoteStats:{}}`.
- `requestStatsContinuous:true`: official continuous stats request. With
  `enableRemote`, the plugin adds the viewer to the remote-stats subscriber
  set, sends an immediate `remoteStats` snapshot, then repeats snapshots every
  3000 ms. Without `enableRemote`, it sends `{remoteStats:{}}` once and does
  not subscribe the viewer.
- `requestStatsContinuous:false`: official interval stop request; plugin removes
  the viewer from the remote-stats subscriber set.
- `description` / `candidate` / `candidates`: official direct data-channel
  signaling. Publisher output and native/source receive paths inject sender
  `UUID` when absent and route the payload through
  `VDONinjaSignaling::processIncomingMessage`.
- `ping`: official peer liveness probe; plugin responds to the same peer with
  `pong` carrying the same raw JSON token.
- `pong`: official peer liveness response; plugin classifies it as known input.
  Current output-side behavior is no-op because the plugin does not currently
  initiate its own data-channel ping recovery loop.
- `iceRestartRequest`: official peer recovery request. Direct data-channel
  input is recognized by field presence and asks the peer manager to create a
  fresh publisher offer for that peer; WebSocket input dispatches through the
  dedicated signaling ICE-restart callback when the request is truthy.
- `refreshVideo`: official remote/director recovery control. OBS publisher
  output honors it only when `enableRemote` is enabled, and maps it to cached
  keyframe priming for the requesting peer because OBS already owns a native
  encoded stream rather than a browser camera device. When remote control is
  disabled, publisher output returns `{"rejected":"refreshVideo"}`. Field
  presence means requested, even when the JSON value is false.
- `refreshMicrophone`: official remote/director recovery control. The plugin
  classifies and parses it for protocol alignment, but native OBS publisher
  output rejects it because it does not own a browser microphone capture
  device. Field presence means requested, even when the JSON value is false.
- `refreshConnection`: official remote/director recovery control. OBS
  publisher output honors it only when `enableRemote` is enabled, and maps it
  to the existing peer-manager ICE restart/re-offer path for every current
  publisher peer snapshot. When remote control is disabled, publisher output
  returns `{"rejected":"refreshConnection"}`. Field presence means requested,
  even when the JSON value is false.
- `refreshAll`: official remote/director recovery control. OBS publisher
  output honors it only when `enableRemote` is enabled, and performs the
  implemented OBS-native equivalents of `refreshVideo` and
  publisher-wide `refreshConnection`. When remote control is disabled,
  publisher output returns `{"rejected":"refreshAll"}`. Field presence means
  requested, even when the JSON value is false.
- `restartWhip`: official WHIP publisher reconnect control. The plugin
  classifies and parses it for protocol alignment, but native OBS publisher
  output rejects it because it is not the browser WHIP path. Field presence
  means requested, even when the JSON value is false.
- Disabled or unavailable OBS publisher output rejects only one recovery
  control per message, using the official non-director rejection order:
  `refreshMicrophone`, `refreshVideo`, `refreshConnection`, then `refreshAll`.
  `restartWhip` is kept as an OBS-native explicit rejection after those fields
  because the plugin has no WHIP publisher connection to restart.
- `reconnectPeer`: official mesh repair control. Browser VDO.Ninja closes the
  named peer connection and relies on renegotiation, but rejects this request
  from non-directors. OBS publisher output rejects it because the plugin has no
  director identity model.
- `getConnectionMap`: official mesh diagnostics request. OBS publisher output
  rejects it because official VDO.Ninja treats it as director mesh
  visualization, not a remote-token-only control.
- `connectionMap`: official mesh diagnostics response. The plugin classifies
  and parses it as mesh control for protocol alignment; publisher output does
  not currently aggregate director-side mesh views.
- `hangup`: official remote/director endpoint-stop request; plugin classifies
  field presence as `Hangup` but publisher output rejects it because the plugin
  has no director identity model. Do not let a remote-token-only peer stop OBS
  output through this path.
- top-level data-channel `bye`, `request:"cleanup"`: official peer close or
  cleanup request; plugin classifies these as `PeerBye` before ordinary
  mute/video/source state so combined close payloads still retire the peer.
  Publisher output retires the peer and clears output-owned per-peer telemetry
  and continuous stats subscription state.
- `obsCommand` / `action`: plugin remote-control input aligned with official
  OBS-control payload shape. The message must include top-level `remote`;
  missing `remote` is classified as unsupported `obsCommand` so publisher
  output can return `{"rejected":"obsCommand"}`. Effects also require output
  `enableRemote`.
- legacy `remote` action field: plugin-local compatibility input where
  `remote` itself carries an action such as `nextScene`. This is not official
  VDO.Ninja remote-token shape and should not be expanded.
- `getAudioSettings`, `getVideoSettings`, `requestVideoHack`, `changeCamera`,
  `changeMicrophone`, `changeSpeaker`, `requestAudioHack`,
  `requestChangeEQ`, `requestChangeGating`, `requestChangeCompressor`,
  `requestChangeMicDelay`, `requestChangeSubGain`, `requestChangeLowcut`,
  `requestChangeMicPanning`, `requestVideoRecord`, `changeOrder`, `changeURL`,
  `changeLabel`, `remoteVideoMuted`, `lowerhand`, `displayMute`, `speakerMute`,
  `volume`, `micIsolated`, `micIsolate`, `lowerVolume`, `requestUpload`,
  `stopClock`, `resumeClock`, `setClock`, `hideClock`, `showClock`,
  `startClock`, `pauseClock`, `showTime`, `group`, `reload`, `scale`, `pan`,
  `tilt`, `zoom`, `focus`, `autofocus`, `exposure`, `keyframeRate`: official
  VDO Control Center controls that this
  native OBS publisher does not currently implement or cannot authorize without
  director identity. Publisher output classifies these as unsupported and
  replies to the requesting peer with `{"rejected":"<field>"}`.
- `getOBSState:true`: plugin-supported request for an immediate `obsState`
  response.
- `obsState`: official OBS/source state sent by the plugin to browser viewers
  and recognized when official clients send it back. This is source/OBS state,
  not mute state.
- `sceneDisplay` / `sceneMute`: official scene-state companion fields handled
  by VDO.Ninja `manageSceneState`; plugin classifies them with `obsState` as
  OBS/source state.
- `bitrate`: official viewer request for publisher-side video bitrate control.
  The plugin classifies it as media control. For publisher output, `0` disables
  video RTP sends to that viewer; nonzero values re-enable sends and request a
  keyframe boundary. Publisher output extracts this field with structured
  top-level parsing even when another field family is the primary message type.
  Full per-peer bitrate scaling is not implemented.
- `audioBitrate`: official viewer request for publisher-side audio bitrate
  control. The plugin classifies it as media control. For publisher output, `0`
  disables audio RTP sends to that viewer; nonzero values re-enable sends.
  Publisher output extracts this field with structured top-level parsing even
  when another field family is the primary message type.
- `targetBitrate`, `targetAudioBitrate`, `optimizedBitrate`,
  `requestResolution`: official media-control companion fields. The plugin
  classifies and parses these as media-control state for protocol alignment,
  but publisher output does not currently transcode, rescale, or renegotiate
  OBS media in response. Game-capture's VDO cross-reference confirms
  `requestResolution` uses `{w,h,s,c}` where dimensions can be partial, `s` is
  snap, and `c` is cover behavior. It also confirms `targetBitrate` /
  `targetAudioBitrate` are sender target-cap requests where boolean false
  unlocks the cap; zero route-disable belongs to `bitrate` / `audioBitrate`,
  not the target-cap fields.
- `info.screenShareState`: official initial screen-share state inside peer
  info. The plugin classifies and parses it as screen-share state when no
  higher-priority initial info field claims the single parser type.
- `screenShareState`: official live screen-share state; parsed as
  screen-share state.
- `screenStopped`: official screen-stop state; parsed as screen-share state.
- `info.directorVideoMuted`: official initial director video-hide state inside
  peer info. The plugin classifies and parses it as director-video state when no
  higher-priority initial info field claims the single parser type.
- `directVideoMuted`: official live director video-hide state; parsed as
  director-video state. Optional `target:true` means this recipient is the
  target; optional target string names another peer.
- `virtualHangup`: official director virtual video suppression state; parsed as
  director-video state, not as endpoint hangup.
- `remoteVideoMuted`: official director request for the publisher to suppress
  or resume its outgoing video. The parser helper can extract it, but primary
  dispatch classifies it as unsupported for native publisher output because the
  plugin cannot prove director identity.
- `info.directorSpeakerMuted`: official initial director speaker mute state
  inside peer info. The plugin classifies and parses it as director
  audio/display state when no higher-priority initial info field claims the
  single parser type.
- `info.directorDisplayMuted`: official initial director display mute state
  inside peer info; parsed with director audio/display state.
- `speakerMute`: official live director speaker mute control; parser helper can
  extract it, but primary dispatch classifies it as unsupported for native
  publisher output because the plugin cannot prove director identity.
- `displayMute`: official live director display mute control; parser helper can
  extract it, but primary dispatch classifies it as unsupported for native
  publisher output because the plugin cannot prove director identity.
- `info.directorMirror`: official initial director mirror state inside peer
  info. The plugin classifies and parses it as director transform state when no
  higher-priority initial info field claims the single parser type.
- `info.directorFlip`: official initial director flip state inside peer info;
  parsed with director transform state.
- `info.rotate_video`: official initial video rotation state inside peer info;
  parsed with director transform state.
- `mirrorGuestState`: official live director mirror command when paired with
  `mirrorGuestTarget`; native publisher output rejects it as unsupported
  without director identity.
- `mirrorGuestTarget`: official live director mirror target; `true` means the
  recipient itself, while a string identifies another peer.
- `rotate`: official live remote rotate command sent to the publishing
  endpoint; native publisher output rejects it as unsupported. The transform
  helper can still extract toggle, reset, or absolute degree intent for review.
- `rotate_video`: official live advertised video rotation state; parsed with
  director transform state as the current rotation degrees for receivers.
- `info.muted`: official initial audio mute state inside peer info.
- `info.video_muted_init`: official initial video mute state inside peer info.
- `muteState`: official dynamic audio mute state; parsed as audio mute state
  when received.
- `videoMuted`: official dynamic video mute state; parsed as mute state when
  received.
- `tallyOn`, `tallyPreview`, `tallyOff`, `audioMuted`: plugin-local
  compatibility helper fields, not official VDO.Ninja signaling.
- `UUID` + `session` wrapper: plugin-native data-channel transport envelope for
  target-routed SDP/ICE/control messages when a transport peer differs from the
  target media peer.

Signaling message matrix:

- `request:"seed"`: plugin publish request sent to the server with `streamID`.
- `request:"play"`: plugin view request sent to the server with `streamID`.
- `request:"joinroom"`: room join request sent to the server with `roomid` and
  optional director claim.
- `request:"listing"`: official room admission/listing response; updates local
  room joined state and member list.
- `request:"transferred"`: official room transfer response; handled as a
  listing because it carries the replacement room member list.
- `request:"someonejoined"` with `streamID`: official incremental room stream
  notification; handled as stream added and appended to the stored room member
  stream snapshot if absent.
- `request:"videoaddedtoroom"` / `videoAddedToRoom`: official and legacy room
  stream-added variants; handled as stream added and appended to the stored room
  member stream snapshot if absent.
- `request:"videoremovedfromroom"` / `videoRemovedFromRoom`: official and
  legacy room stream-removed variants; handled as stream removed and removed
  from the stored room member stream snapshot.
- `request:"joinpending"` and `request:"joinrequestresult"`: official room
  admission UI/status messages. The plugin currently parses them as generic
  requests and does not expose UI handling for the approval queue.
- `request:"offerSDP"`, `sendoffer`, `play`, or compatible `joinroom` with a
  stream id: offer request directed to the publisher peer manager.
- `description`: SDP offer/answer envelope.
- `candidate` / `candidates`: ICE candidate or candidate bundle.
- `request:"cleanup"` / truthy `bye`: WebSocket peer cleanup. This is not
  endpoint hangup. `bye:false` is ignored as cleanup to match official
  WebSocket handling. `request:"bye"` remains a generic request, not cleanup.
- `iceRestartRequest:true` with `UUID`: official WebSocket peer recovery
  request; plugin dispatches to the peer manager's ICE-restart/re-offer path
  instead of the ordinary offer-request duplicate suppression path. Direct
  data-channel `iceRestartRequest` is presence-based and terminal.
- `request:"alert"` / `error` / `alert`: server alert/error.

Flow: outbound signaling connection

1. Event: signaling `connect()` is called.
2. Gate: if already connected, return success.
3. Gate: if a dead WebSocket thread exists, join it first.
4. State: host list is rebuilt from configured host plus built-in fallback.
5. State: active host index resets to the first host.
6. State: reconnect suppression/defer state is cleared.
7. State: outbound send queue is cleared so old messages cannot replay.
8. Edge: WebSocket thread starts.
9. Gate: caller waits for initial connect/failover cycle or disconnect.

Flow: WebSocket open/fail/reconnect

1. Event: WebSocket opens.
2. State: `connected = true`; initial connection completes.
3. Edge: connected callback runs.
4. Event: WebSocket closes after opening.
5. State: `connected = false`; `needsReconnect = true`.
6. Event: pre-open error.
7. Edge: if another host exists, advance to fallback host before reporting
   final failure.
8. Edge: otherwise report error callback.
9. Gate: reconnect stops when auto-reconnect is disabled, max attempts reached,
   server alert suppresses reconnect, or caller disconnects.
10. Backoff: signaling reconnect delay doubles up to max; server alert can
    defer longer.

Flow: outbound signaling send queue

1. Event: caller sends signaling message.
2. Gate: message is accepted only while signaling is connected.
3. Edge: message enters send queue.
4. Edge: WebSocket thread pops and sends queued messages while connected.
5. Gate: if the socket closes before send, message is dropped, not replayed
   after a future connection.
6. Event: WebSocket attempt ends for close, reconnect, fallback, or exception.
7. State: queue is cleared before any later connection attempt.
8. Event: disconnect.
9. State: queue is cleared before WebSocket thread is joined.

Flow: inbound signaling message

1. Event: WebSocket text message or data-channel-carried signaling arrives.
2. Edge: parser normalizes `UUID`, `session`, request, stream id, description,
   candidates, listing/transfer, alert, cleanup, room video add/remove, and
   official room `someonejoined` variants.
3. Edge: encrypted description/candidate payloads are decrypted using candidate
   passwords from matching published stream, viewing streams, current room, and
   default password.
4. Dispatch:
   - listing and transferred updates room members and fires room joined
     callback.
   - offer fires offer callback.
   - answer fires answer callback.
   - candidate/bundle fires ICE callback.
   - offer request fires request callback for `offerSDP`, `sendoffer`, `play`,
     and compatible `joinroom` with stream id.
   - alert applies reconnect/retry policy and fires error callback.
   - cleanup/bye fires peer cleanup callback.
   - video added/removed and `someonejoined` with stream id update the stored
     room member stream snapshot, then fire stream callbacks.

Flow: signaling room and stream state

1. Event: join room requested.
2. State: room id, hashed room id, and room encryption password are stored
   before the outbound `joinroom` message is queued.
3. Event: room listing or transferred response received.
4. State: `currentRoom.isJoined = true` and member list is updated.
5. Event: room stream-added event received through `videoaddedtoroom`,
   `videoAddedToRoom`, or `someonejoined` with `streamID`.
6. State: stream id is appended to `currentRoom.members` if absent.
7. Event: room stream-removed event received through `videoremovedfromroom` or
   `videoRemovedFromRoom`.
8. State: stream id is removed from `currentRoom.members`.
9. Event: publish requested.
10. State: published stream id, hashed stream id, password, and publishing flag
   are stored before the outbound `seed` message is queued.
11. Event: view requested.
12. State: viewing stream record is stored before outbound `play` is queued.
13. Event: stop viewing requested.
14. State: viewing stream record is removed locally; no unsupported WebSocket
    stop request is queued.
15. Event: disconnect.
16. State: room, published stream, and viewing stream records are cleared after
    the socket thread has been stopped.
17. Event: leave room requested before disconnect.
18. State: pending or confirmed room context is cleared locally; no unsupported
    WebSocket leave request is queued.

Invariant:

- Room/publish/view state is both protocol state and decryption context. Clearing
  it too early can make late encrypted SDP/ICE undecodable; keeping it too long
  can make stale encrypted messages look valid.
- `isInRoom` currently means listing-confirmed membership, not merely that a
  `joinroom` message was queued.

## Publisher Flow

Flow: OBS starts publishing

1. Event: user starts OBS streaming through VDO.Ninja output.
2. Gate: output must not already be running.
3. Gate: stream id must be present.
4. Gate: OBS data capture must be possible.
5. Gate: output audio encoders must be Opus.
6. Gate: publisher video codec is H.264. Non-H.264 settings are overridden.
7. State: `running = true`; `connected = false`; `capturing = false`.
8. State: keyframe cache and RTP timestamp guards are reset.
9. Edge: start media-send worker.
10. Edge: start output start thread with immutable settings snapshot.

Flow: publisher signaling setup

1. Event: output start thread begins.
2. Edge: initialize peer manager with signaling client.
3. Edge: peer manager registers signaling handlers for offer, answer, offer
   request, ICE, and peer cleanup.
4. Edge: apply codec, bitrate, ICE, TURN, data channel, and salt settings.
5. Edge: configure auto-inbound scene manager if enabled.
6. Edge: install output signaling callbacks.
7. Edge: install peer manager callbacks.
8. Edge: enable signaling auto-reconnect.
9. Edge: connect to signaling WebSocket.

Flow: signaling connected for publisher

1. Event: WebSocket open.
2. Edge: optional room join for publish presence and/or auto-inbound.
3. Edge: publish stream by sending VDO.Ninja `seed` with hashed stream id.
4. Edge: start peer manager publishing with max viewer count.
5. State: `connected = true`.
6. Edge: begin OBS data capture if not already capturing.

Flow: remote viewer asks for media

1. Event: signaling request `offerSDP`, `sendoffer`, `play`, or compatible
   `joinroom` with stream id.
2. Edge: signaling dispatches offer request to peer manager.
3. Gate: peer manager must be publishing.
4. Gate: request must include `UUID`.
5. Gate: existing peer with rotated session or terminal state is retired.
6. Gate: viewer slot count must be below max.
7. Edge: create publisher peer if none is reusable.
8. State: peer has send-only video/audio tracks and optional `sendChannel`.
9. State: peer session is request session, or generated if absent.
10. State: candidate bundle session is peer session.
11. State: `awaitingVideoKeyframe = true` unless peer was already connected.
12. Gate: if same peer/session already requested a local offer, ignore the
    duplicate before re-entering local-offer generation.
13. Edge: if duplicate request arrives after local offer exists and peer is
    still negotiating, resend cached offer.
14. Edge: request local description from libdatachannel only for a fresh
    peer/session.
15. State: `localOfferRequested = true` until peer is retired or offer creation
    throws.

Flow: publisher local offer

1. Event: libdatachannel produces local offer.
2. Gate: peer must still exist, not be terminal, not be retired, and manager
   must not be shutting down.
3. State: local offer SDP is cached under peer negotiation lock.
4. Edge: signaling sends offer to remote `UUID` with peer session.
5. Edge: offer includes hashed stream id when publishing state is active.
6. Edge: offer description is encrypted when active password context exists.

Flow: publisher receives answer

1. Event: signaling receives answer.
2. Gate: peer must exist.
3. Gate: peer must be publisher type.
4. Gate: terminal peers ignore the answer.
5. Gate: non-empty session mismatch is ignored in all states.
6. Edge: remote answer is applied to peer connection only for matching or
   sessionless answers.
7. State: peer should move toward `Connecting` then `Connected` via RTC state
   callbacks.

Flow: publisher ICE

1. Event: libdatachannel emits local candidate.
2. Edge: candidate is stored in peer candidate bundle by `UUID`.
3. Edge: bundle flushes when enough candidates accumulate or gathering
   completes.
4. Edge: candidate is sent via bound signaling data channel when available,
   otherwise via WebSocket signaling.
5. Event: remote ICE arrives from signaling.
6. Gate: peer must exist and not be terminal.
7. Gate: non-empty session mismatch is ignored in all states.
8. Edge: remote candidate is applied to peer connection only for matching or
   sessionless candidates.

Flow: publisher media packets

1. Event: OBS encoded packet callback fires.
2. Gate: output must be `running` and signaling `connected`.
3. Video edge:
   - derive RTP timestamp from DTS/timebase.
   - force monotonic timestamp if encoder timestamp goes backward or stalls.
   - cache keyframe payload and timestamp.
   - enqueue video frame to media-send queue.
4. Audio edge:
   - drop packets from non-selected OBS audio tracks.
   - derive RTP timestamp from DTS/timebase.
   - force monotonic timestamp if needed.
   - enqueue audio frame to media-send queue.
5. Gate: media-send queue is bounded; oldest frames are dropped when saturated.
6. Edge: media-send worker forwards frames to peer manager off the OBS encoder
   callback thread.
7. Edge: peer manager snapshots connected publisher peers under the peer map
   lock, then releases the map lock before sending.
8. Edge: each peer send snapshots the track and packetizes under that peer's
   media lock, then releases plugin locks before calling libdatachannel send.

Flow: publisher video send to one viewer

1. Event: media-send worker asks peer manager to send video.
2. Gate: peer manager must be publishing.
3. Gate: peer must be publisher type, connected, and not retired.
4. Gate: if peer is awaiting keyframe, delta frames are dropped.
5. State: first keyframe clears `awaitingVideoKeyframe`.
6. Edge: H.264 frame is packetized into RTP with per-peer sequence and shared
   video SSRC.
7. Edge: packets are sent on the peer video track.

Flow: publisher audio send to one viewer

1. Event: media-send worker asks peer manager to send audio.
2. Gate: peer manager must be publishing.
3. Gate: peer must be publisher type, connected, and not retired.
4. Edge: Opus payload is wrapped in RTP with per-peer sequence and shared audio
   SSRC.
5. Edge: packet is sent on the peer audio track.

Flow: publisher data channel

1. Event: `sendChannel` opens.
2. Edge: output sends initial plugin/viewer info.
3. Edge: output queues OBS state collection on UI thread.
4. Event: viewer sends data-channel message.
5. Edge: data channel handler parses chat, tally, mute, stats, stats requests,
   OBS/source state, media control, screen-share state, director video state,
   director audio/display state, director transform state, direct signaling,
   keyframe request, ping/pong, ICE restart, hangup, remote control, or custom
   data.
6. Edge: direct signaling payloads are sender-UUID normalized and routed into
   signaling message processing, then return.
7. Edge: ping causes publisher to send `pong` with the same raw token to that
   peer, then return.
8. Edge: ICE restart request asks peer manager to generate a fresh publisher
   offer for that peer/session, then return.
9. Edge: keyframe request causes publisher to send cached keyframe to that peer
   if available.
10. Edge: stats update runtime telemetry.
11. Edge: remote-control messages execute only when remote control is enabled;
   `hangup` field presence is classified separately and rejected until director
   identity exists.
12. Edge: inbound playback hints can drive auto-inbound scene creation.

## Data Channel Flow

Flow: publisher inbound data message

1. Event: viewer data channel message arrives on an RTC callback thread.
2. Gate: peer manager checks peer exists, is not retired, and is not terminal.
3. Edge: peer manager copies the registered data-message callback under
   callback lock, then invokes it outside the callback lock.
4. Edge: output parses the message through the data-channel handler.
5. Dispatch:
   - chat queues dock UI display.
   - tally updates per-peer tally state.
   - mute is parsed and can notify callback users.
   - keyframe request sends cached keyframe to the requesting peer; official
     VDO.Ninja sends this as `keyframe`, while the plugin also accepts
     `requestKeyframe` for compatibility. Keyframe is a field-level fanout, not
     only a primary parsed type, so it is still honored when paired with
     `requestStats` or other independently handled fields. This fanout is
     disabled for VDO.Ninja's early-return transport/terminal messages:
     `description`, `candidate`, `candidates`, `ping`, `pong`,
     `iceRestartRequest`, `bye` / `request:"cleanup"`, and `hangup`.
   - stats updates output telemetry cache; official VDO.Ninja sends this under
     `remoteStats`, while `stats` remains accepted for compatibility. Stats
     response telemetry outranks passive mute/source state, because official
     VDO.Ninja consumes `remoteStats` through an independent field check after
     other state handling.
   - stats request sends `remoteStats` back to the requesting peer. With remote
     control enabled, the snapshot excludes the requesting peer and includes
     active peer state plus any cached stats payloads. With remote control
     disabled, the response is the empty official shape `{remoteStats:{}}` and
     no continuous subscriber is added. Continuous stop always removes the
     subscription. Stats requests are primary active controls and outrank
     passive OBS/source state so a combined payload still gets a stats response.
   - OBS/source state, screen-share state, director video state, director
     audio/display state, and director transform state are classified as known
     official protocol input. Publisher output also runs a structured
     media-control extraction pass over each message so top-level `bitrate` and
     `audioBitrate` side effects are not lost when the primary type is another
     state family. Only the implemented media-control active/inactive subset
     mutates publisher media sends today; director-only live controls such as
     `remoteVideoMuted`, `speakerMute`, and `displayMute` are rejected without
     director identity.
   - recovery control is classified as known official protocol input. When
     remote control is enabled, `refreshVideo` primes the requesting peer with
     the cached keyframe, `refreshConnection` snapshots every current publisher
     peer and asks the peer manager for ICE restart/re-offer on each one,
     `refreshAll` performs both implemented repairs, and unsupported
     `refreshMicrophone` / `restartWhip` return VDO-shaped `rejected`
     responses. When remote control is disabled, actionable recovery controls
     are rejected instead of disappearing silently. Recovery controls outrank
     passive state buckets because official VDO.Ninja keeps processing control
     fields after `manageSceneState`.
   - unsupported official Control Center controls such as settings readback,
     `requestVideoHack`, browser WebAudio mutations, browser reload/scale, PTZ
     controls, OBS command payloads missing `remote`, and director-only
     controls are classified as known unsupported protocol input and rejected
     with VDO's `rejected` family. This classification outranks passive state
     buckets such as mute and OBS/source state, so a combined payload cannot
     hide a rejected control behind `videoMuted` or `obsState`. Stats requests
     and keyframe requests outrank unsupported-control classification because
     official VDO.Ninja handles those fields before many Control Center
     rejection checks; publisher output extracts the unsupported-control name
     separately so the request can still receive `{"rejected":"<field>"}` after
     earlier active side effects run.
   - accepted OBS remote-control payloads require VDO's `remote` field. They are
     classified before passive OBS/source state so authenticated command shapes
     cannot be hidden by a simultaneous `obsState` update. The plugin still
     lacks token matching; `enableRemote` gates whether the parsed command has
     an OBS UI side effect.
   - mesh response payloads are classified as known official protocol input.
     Mesh request fields `reconnectPeer` and `getConnectionMap` are
     director-only in official VDO.Ninja, so native publisher output rejects
     them without director identity instead of honoring them under
     `enableRemote`.
   - direct signaling payloads (`description`, `candidate`, `candidates`) are
     prepared with the data-channel sender UUID when needed and then routed
     through the shared signaling parser.
   - ping sends `pong` to the same peer with the same raw JSON token; this keeps
     official disconnected-peer liveness checks from timing out.
   - pong is accepted as a known liveness response and is currently a no-op.
   - ICE restart request asks peer manager to create a fresh publisher offer
     for the same peer when the peer is live and signaling state is stable.
   - hangup is rejected as director-only because the plugin does not have
     director identity. Hangup outranks passive mute/video state because it is
     endpoint-stop control, not media availability state.
   - bye clears output-owned peer telemetry/subscription state, then retires the
     peer through the peer manager.
   - remote-control parses allowed action/value.
   - custom payload is passed to custom callback users.
6. Gate: recovery-control, mesh-control, and remote-control effects require
   output setting `enableRemote`; hangup remains rejected until director
   identity exists.
7. Edge: OBS-state requests queue an OBS UI task and then send state back to the
   requesting peer.
8. Edge: playback hints can ask auto-inbound manager to add a browser source.

Flow: publisher outbound data message

1. Event: output or dock wants to send info, state, tally, or custom data.
2. Gate: target peer must exist, not be terminal, and have an open data channel.
3. Edge: peer/track/data-channel handles should be snapshotted under peer map
   lock.
4. Edge: actual `DataChannel::send` should happen after plugin locks are
   released.
5. Edge: direct data-channel sends use the JSON payload as-is, matching
   official VDO.Ninja `sendMessage`, `sendRequest`, and `sendPeers` behavior.
6. Edge: official OBS/source state should use `obsState`; plugin-local
   `tallyOn`, `tallyPreview`, `tallyOff`, and `audioMuted` helper payloads are
   compatibility messages only.
7. Edge: viewer signaling fallback wraps target-routed payloads with `UUID` and
   `session` so the receiver can bind signaling-over-data-channel messages to
   the correct target media peer.
8. Hazard: external RTC send while holding `peersMutex` can deadlock or delay
   peer cleanup if the send path synchronously triggers RTC callbacks.

Flow: native viewer data channel as signaling transport

1. Event: native viewer receives data-channel message from a publisher-side
   transport peer.
2. Edge: `ping` is answered with `pong` carrying the same raw JSON token;
   inbound `pong` is accepted as a known no-op because the plugin does not
   currently initiate its own ping recovery loop.
3. Gate: after tolerant classification, malformed JSON is ignored before target
   binding or signaling forwarding. The peer-manager callback guard is a last
   resort, not normal malformed-message control flow.
4. Edge: if payload names a target `UUID` and `session`, bind that transport
   channel to the target media peer.
5. Edge: direct SDP/ICE payloads are prepared with the data-channel sender UUID
   when no explicit `UUID` exists, then fed into the signaling parser.
6. Edge: typed peer cleanup (top-level `bye` or `request:"cleanup"`) is
   handled as peer cleanup. Targeted cleanup is fed into
   the signaling parser without rewriting its shape; untargeted data-channel
   cleanup retires the sender peer.
7. Edge: when a target media peer becomes known, send viewer preferences to the
   target peer.
8. Gate: binding should prefer `UUID + session`; sessionless fallback is allowed
   only while a target session is not yet known.

Flow: data-channel callback ownership

1. Event: output start installs data-channel handler callbacks.
2. Event: output stop clears peer-manager data-channel callbacks, then clears
   data-channel handler callbacks.
3. Gate: any callback object that can be read from RTC callback threads and
   written from stop/update/destruction needs a lock, atomic handoff, or
   callback-guard discipline.
4. Edge: data-channel handler callbacks are copied under handler mutex and
   invoked after the mutex is released.
5. Invariant: data-channel callback storage should be reviewed separately from
   peer-manager callback storage; they are different owners and locks.

## Auto-Inbound Room Flow

Flow: output auto-inbound setup

1. Event: publisher output starts with auto-inbound enabled.
2. Edge: auto-inbound settings are configured from output settings.
3. Edge: own stream ids are registered as raw id, custom-password hash, and
   default-password hash.
4. Edge: auto scene manager starts and clears previous managed state.
5. Event: signaling connected.
6. Edge: output joins auto-inbound room, or publisher room if no separate
   auto-inbound room is configured.
7. Event: room listing arrives.
8. Edge: each listed stream is processed as a stream-added event.
9. Event: `someonejoined` or `videoaddedtoroom` arrives with a stream id.
10. Edge: that stream is processed as a stream-added event.

Flow: auto-inbound stream added

1. Gate: manager must be running.
2. Gate: stream id must not be empty.
3. Gate: stream id must not match local own stream ids.
4. Edge: build inbound VDO.Ninja viewer URL from stream id, room, password,
   base URL, and salt.
5. Gate: target must be convertible into a usable viewer URL.
6. State: stream id is added to managed set.
7. Edge: OBS UI task creates or updates a browser source.
8. Edge: browser source is added to target scene or current scene.
9. Edge: source is made visible.
10. Optional edge: switch OBS to target scene.
11. Optional edge: queue grid layout refresh.

Flow: auto-inbound stream removed

1. Gate: stream id must not be empty.
2. State: stream id is removed from managed set.
3. Edge: OBS UI task finds the source and target scene.
4. Gate: if `removeOnDisconnect`, source is removed.
5. Else: scene item is hidden.
6. Optional edge: queue grid layout refresh.

Invariant:

- Auto-inbound UI tasks capture values only; they must not depend on the manager
  still existing when OBS later executes the queued task.

## Browser-Backed Source Flow

Flow: wrapper source created

1. Event: OBS creates public VDO.Ninja source.
2. State: settings are loaded.
3. Gate: if not internal native source, wrapper decides child mode.
4. Edge: browser-backed mode creates private OBS `browser_source`.
5. Edge: native-wrapper mode creates private internal native receiver source.

Flow: browser child creation/update

1. Gate: wrapper must not be internal native source.
2. Gate: `useNativeReceiver` must be false.
3. Gate: stream id must be present.
4. Edge: build VDO.Ninja viewer page URL from stream id, password, room, salt,
   and signaling host.
5. Edge: create private browser source if missing.
6. Edge: update existing browser source only when URL or dimensions changed.
7. Edge: attach audio capture callback and audio active/deactive signals.
8. Edge: mirror wrapper active/showing state to child source.

Flow: browser child media

1. Video: OBS browser source renders itself.
2. Audio: child audio capture callback forwards captured audio to wrapper source.
3. Audio state: child audio activate/deactivate mirrors wrapper source audio
   active state.

Flow: wrapper child lifecycle mirroring

1. Event: wrapper activates, deactivates, shows, hides, updates, or destroys.
2. Edge: wrapper acquires the current child source under child-source lock.
3. Edge: child active/showing refs are adjusted to match wrapper active/showing
   booleans.
4. Edge: child audio capture callback and audio activate/deactivate signal
   handlers are connected when the child is created.
5. Edge: signal handlers and audio capture callback are disconnected before the
   child source is released.
6. Gate: child source replacement must detach the previous child lifecycle refs
   before releasing it.
7. Gate: callback code uses async callback guard, because OBS child callbacks can
   outlive wrapper-side source pointer changes.

Flow: wrapper mode switch

1. Event: source settings change between browser-backed and native receiver
   modes, or stream/dimensions/settings change.
2. Gate: internal native source never creates another child source.
3. Edge: browser mode releases native child and ensures browser child.
4. Edge: native-wrapper mode releases browser child and ensures internal native
   child.
5. Edge: existing child is updated only when URL, dimensions, or relevant source
   settings differ.
6. Invariant: browser child and native child are mutually exclusive for a public
   wrapper source.

Invariant:

- Wrapper property updates should not recreate the browser child unless URL or
  dimensions changed.
- Child active/showing refcounts must be detached before releasing the child.

## Native Receiver Flow

Flow: wrapper native child

1. Gate: wrapper must not be internal native source.
2. Gate: `useNativeReceiver` must be true.
3. Gate: stream id must be present.
4. Edge: create private internal native receiver source if missing.
5. Edge: update existing native child only when receiver settings or dimensions
   changed.
6. Edge: attach child audio capture callback and lifecycle mirror.

Flow: internal native source activation

1. Event: OBS activates internal native source.
2. Gate: stream id must be present.
3. Edge: join previous connection thread if still joinable.
4. Edge: reset native media/decode state and retry state.
5. State: `nativeRunning = true`; `connected = false`.
6. Edge: start native connection thread.

Flow: native signaling setup

1. Edge: initialize peer manager with signaling client.
2. Edge: apply data-channel, ICE, TURN, and salt settings.
3. Edge: install track, peer, data-channel, signaling, stream, and cleanup
   callbacks.
4. Edge: enable signaling auto-reconnect.
5. Edge: connect WebSocket.

Flow: native signaling connected

1. Event: WebSocket open.
2. Edge: optional room join.
3. Edge: request view stream by sending VDO.Ninja `play` with hashed stream id.
4. Edge: peer manager enters viewing mode.
5. State: retry state marks `awaitingPeerConnection = true`.

Flow: native view request retry state

1. Event: initial signaling connection, stream-added event, scheduled retry, or
   peer/stream recovery path requests view.
2. Gate: native receiver must be running, stream id must exist, signaling must
   be connected, and retry must not be suppressed.
3. Gate: repeated non-reset requests are throttled by minimum view-request gap.
4. State: `lastViewRequestTimeMs` is updated.
5. State: `awaitingPeerConnection = true`.
6. Edge: signaling sends `play` for the target stream.
7. Edge: peer manager starts viewing that stream.
8. Event: no peer arrives before timeout.
9. State: `nextViewRetryTimeMs` is scheduled with backoff and
   `awaitingPeerConnection = false`.
10. Event: retry due.
11. State: retry count increments and a new view request is sent.

Flow: native offer handling

1. Event: signaling receives publisher offer.
2. Gate: existing viewer peer with different session rejects the offer.
3. Edge: create viewer peer if none exists.
4. State: peer session is offer session.
5. Edge: bind pending viewer signaling data channel for `UUID + session` if one
   was discovered earlier.
6. Edge: constrain remote offer to native-supported feedback/codecs.
7. Edge: prepare recv-only tracks based on offered media sections:
   - first video: prefer VP9, fall back to H.264.
   - second VP9 video: alpha track when possible.
   - audio: Opus only.
8. Edge: apply remote offer.
9. Event: libdatachannel produces local answer.
10. Edge: answer is sent via bound data channel when available, otherwise via
    WebSocket signaling.

Flow: native data channel as signaling transport

1. Event: native receiver gets data-channel message from a publisher-side
   transport peer.
2. Edge: `ping` is answered with `pong`; `pong` is treated as known liveness
   response and otherwise ignored.
3. Edge: if message contains `UUID` and `session`, bind transport data channel
   to target media peer.
4. Edge: `muteState` updates native remote audio mute state. Muting audio
   suppresses OBS audio output and marks the OBS source audio inactive;
   unmuting marks the OBS source audio active again before decoded audio frames
   resume.
5. Edge: `videoMuted`, `info.video_muted_init`, `info.directorVideoMuted`,
   `directVideoMuted`, and `virtualHangup` update split native receiver video
   suppression flags. Decoded video is cleared and hidden while any flag is
   active, then restored only when all active suppression flags are false.
6. Edge: `screenShareState` and `screenStopped` are parsed as known official
   state by the shared handler but currently do not mutate native source output
   state. Director-only live controls such as `remoteVideoMuted`,
   `speakerMute`, and `displayMute` are recognized as unsupported-control
   payloads by the shared parser; the native source path currently ignores them
   rather than applying source-side effects.
7. Edge: direct SDP/ICE payloads are fed back into the signaling parser. Typed
   peer cleanup is handled by the same cleanup path; raw substring matches are
   not used as protocol classification.
8. Edge: targeted peer gets viewer preferences when its media peer is resolved.

Flow: native cleanup and stream removal

1. Event: signaling cleanup/bye arrives, either over WebSocket signaling or
   data-channel-carried signaling.
2. Edge: peer manager disconnects the named peer and retires RTC resources for
   deferred cleanup.
3. Edge: native receiver clears active video/alpha/audio ownership that matches
   the peer UUID.
4. Edge: matching video cleanup clears current OBS video output.
5. Edge: matching audio cleanup marks source audio inactive.
6. Gate: if no active media track remains and auto-reconnect is enabled,
   schedule recovery retry.
7. Event: stream-removed arrives for the target stream, or for a UUID that owns
   an active media track.
8. Edge: peer manager stops viewing the stream.
9. Edge: native state is fully reset and retry is scheduled when allowed.

Flow: native track attachment

1. Event: peer manager reports track.
2. Video gate:
   - track must advertise H.264 or VP9.
   - track is ignored if another peer still owns active video.
3. Video edge:
   - clear previous video callbacks.
   - set active video track and owner UUID.
   - reset assembly/decode state when replacing.
   - attach RTP/media handlers and binary message callback.
   - request bitrate and keyframe when appropriate.
4. Alpha gate:
   - track must not be the same mid as primary video unless reattached as video.
   - track is ignored if another peer still owns active alpha.
5. Alpha edge:
   - set active alpha track and owner UUID.
   - force software VP9 primary decode for compositing.
   - reset alpha assembly/decode state.
   - attach binary message callback.
6. Audio gate:
   - track must advertise Opus.
   - track is ignored if video peer ownership conflicts.
7. Audio edge:
   - set active audio track and owner UUID.
   - reset audio decoder when replacing.
   - attach binary message callback.

Flow: native video receive

1. Event: video RTP packet arrives.
2. Gate: `nativeRunning` must be true.
3. Gate: `remoteVideoMuted` suppresses decoded OBS video output while still
   allowing RTP/decode state to continue.
4. Edge: parse RTP payload and RED wrapper if present.
5. VP9 edge:
   - parse VP9 payload descriptor.
   - assemble frame between start/end flags.
   - submit completed frame to video decoder.
6. H.264 edge:
   - parse single NAL, STAP-A, or FU-A payload.
   - assemble access unit by RTP timestamp and marker bit.
   - submit completed access unit to video decoder.
7. Decode edge:
   - initialize H.264 or VP9 decoder, hardware accelerated when possible.
   - submit compressed frame.
   - request keyframe on decode submit/decode failure.
   - map RTP timestamp to monotonic OBS timestamp.
   - output decoded video frame to OBS.

Flow: native VP9 alpha receive

1. Event: alpha RTP packet arrives.
2. Gate: `nativeRunning` must be true.
3. Edge: parse VP9 payload descriptor and assemble alpha frame.
4. Edge: decode alpha as software VP9.
5. Edge: store alpha Y-plane by RTP timestamp in bounded pending-alpha state.
6. Edge: primary video output consumes matching alpha timestamp when available.
7. Gate: alpha frame dimensions must match primary video frame.
8. Edge: YUV primary + alpha Y plane are exposed as YUVA420P for BGRA output.

Flow: native audio receive

1. Event: audio RTP packet arrives.
2. Gate: `nativeRunning` must be true.
3. Gate: `remoteAudioMuted` suppresses decoded OBS audio output.
4. Edge: parse RTP payload.
5. Edge: initialize Opus decoder for negotiated sample rate and channels.
6. Edge: decode Opus frame.
7. Edge: resample to OBS planar float audio.
8. Edge: map RTP timestamp to monotonic OBS timestamp.
9. Edge: set source audio active and output audio to OBS.

Flow: native retry and stale media

1. Event: connection thread loop ticks every 100 ms.
2. Edge: service view retry.
3. Gate: retry is skipped when native not running, auto-reconnect disabled, or
   already connected.
4. Gate: server alerts can suppress or delay retry.
5. Edge: no-offer timeout schedules backoff retry.
6. Edge: peer disconnect or stream removed schedules recovery retry.
7. Event: video tick.
8. Edge: stale displayed video is cleared when no native video packets arrive
   for the stale-video timeout.
9. Edge: if connected but no recent video, request keyframe at a throttled rate.

Flow: native decode ownership reset

1. Event: source disconnect, settings update, stream removal, peer cleanup, peer
   disconnect, or track replacement.
2. Edge: track callbacks are cleared before track references are dropped.
3. Edge: RTP assembly buffers and timestamp maps are cleared for affected media.
4. Edge: decoder contexts and frames are freed for affected media.
5. Edge: pending alpha frames are cleared on full native reset.
6. Edge: OBS audio is marked inactive when audio ownership is removed.
7. Edge: OBS video output is cleared when primary video ownership is removed.

## Teardown Flow

Flow: output stop

1. Event: OBS stops VDO.Ninja output or output is destroyed.
2. State: `running = false`; `connected = false`.
3. Edge: stop remote-stats worker and clear continuous stats subscribers.
4. Edge: stop media-send worker and clear queued media frames.
5. Edge: stop auto-inbound scene manager.
6. Edge: clear signaling callbacks.
7. Edge: clear peer-manager callbacks.
8. Edge: clear data-channel handler callbacks.
9. Edge: stop peer manager publishing.
10. Edge: clear peer telemetry.
11. Edge: unpublish stream state.
12. Edge: clear local room state when present.
13. Edge: disconnect signaling.
14. Edge: join start thread.
15. Edge: end OBS data capture if active.
16. Edge: clear keyframe and timestamp caches.
17. Edge: destructor detaches async callback owner and waits for in-flight
    callbacks to drain.

Flow: remote hangup request

1. Event: publisher receives official data-channel `hangup` field, commonly
   `{hangup:true}`.
2. Gate: publisher output has no VDO-compatible director identity model.
3. Edge: output sends VDO-shaped `{"rejected":"hangup"}` to the requesting peer.
4. Edge: OBS output remains running; media, peer, signaling, telemetry, room,
   and data-channel state are not cleared by this request.
5. Invariant: peer-only cleanup still uses `bye` or `cleanup`. Do not re-enable
   endpoint stop through the broad remote-control flag.

Flow: native source disconnect

1. Event: OBS deactivates, updates, destroys, or disables internal native source.
2. State: `nativeRunning = false`; `connected = false`.
3. Edge: source audio is marked inactive.
4. Edge: retry state is reset.
5. Edge: signaling is disconnected and callbacks are cleared.
6. Edge: peer manager callbacks are cleared.
7. Edge: connection thread is joined.
8. Edge: native tracks, callbacks, decoders, assembly buffers, and OBS video
   output are cleared.
9. Edge: destructor detaches async callback owner and waits for in-flight
   callbacks to drain.

Flow: peer terminal state

1. Event: libdatachannel peer state changes to `Disconnected`, `Failed`, or
   `Closed`.
2. State: peer terminal state and terminal timestamp are recorded.
3. Gate: disconnect notification is emitted once.
4. Edge: peer is removed from active peer map.
5. Edge: candidate bundles and pending viewer signaling channels for that peer
   are removed.
6. State: `cleanupRetired = true`.
7. Edge: peer is appended to retired peer list.
8. Edge: later non-RTC service path clears callbacks and releases RTC objects.

Invariant:

- PeerConnection objects should not be destroyed synchronously from their own
  RTC callbacks.
- OBS owner objects should not be reachable from async callbacks after callback
  state is detached.
- OBS UI-affine work should be queued to the OBS UI thread.
- Callback storage is owner-specific. Clearing peer-manager callbacks does not
  make data-channel handler callback members thread-safe by itself.
- External RTC sends should not execute under plugin ownership locks.

## Current Review Fixes Applied

Fix: signaling send queue is scoped to one WebSocket attempt.

- Source anchors: `VDONinjaSignaling::connect`,
  `VDONinjaSignaling::disconnect`, `VDONinjaSignaling::wsThreadFunc`,
  `VDONinjaSignaling::clearSendQueue`.
- Workflow: outbound signaling messages are accepted only while connected, and
  unsent queued messages are dropped when the current connection attempt ends.
- Why this matters: queued offer, answer, candidate, or data messages cannot
  replay into a later room, stream, or peer session.
- Review rule: do not add reliable replay to the raw send queue. If signaling
  replay is needed later, model it as explicit per-room/per-session state.

Fix: publisher answer and ICE session ownership is strict.

- Source anchors: `VDONinjaPeerManager::onSignalingAnswer`,
  `VDONinjaPeerManager::onSignalingIceCandidate`.
- Workflow: non-empty session mismatch is ignored in all publisher peer states.
- Why this matters: peer record session, candidate bundle session, and
  data-channel routing no longer drift apart during negotiation.
- Review rule: any future relaxed session acceptance must update all affected
  peer, bundle, and transport state atomically.

Fix: same-session offer retries have a resend path.

- Source anchors: `VDONinjaPeerManager::onSignalingOfferRequest`,
  `VDONinjaPeerManager::installLocalDescriptionCallback`, `PeerInfo`.
- Workflow: duplicate same-session requests do not re-enter local-offer
  generation. Once libdatachannel has produced an offer, the cached SDP can be
  resent while the peer is still negotiating.
- Why this matters: the crash-prevention latch no longer turns every duplicate
  viewer retry into a silent negotiation stall.
- Review rule: keep `localOfferRequested` as the re-entrancy gate and
  `lastLocalOfferSdp` as retry output, or replace both with a more explicit
  offer state machine.

Fix: peer media sends no longer hold the global peer map lock.

- Source anchors: `VDONinjaPeerManager::sendAudioFrame`,
  `VDONinjaPeerManager::sendVideoFrame`,
  `VDONinjaPeerManager::sendAudioFrameToPeer`,
  `VDONinjaPeerManager::sendVideoFrameToPeerHandle`, `PeerInfo`.
- Workflow: connected publisher peers are snapshotted under `peersMutex`; each
  peer snapshots track state and packetizes under `mediaMutex`; libdatachannel
  send is called after plugin locks are released.
- Why this matters: libdatachannel send behavior cannot block peer-map or
  per-peer media operations such as signaling callbacks, cleanup, connect, or
  disconnect.
- Review rule: never call libdatachannel `send` while holding `peersMutex` or
  `mediaMutex`.

Fix: peer resource release follows the same lock/external-callback boundary.

- Source anchors: `VDONinjaPeerManager::releasePeerResources`,
  `VDONinjaPeerManager::clearPeerCallbacks`, `clearPeerConnectionCallbacks`,
  `clearTrackCallbacks`, `clearDataChannelCallbacks`.
- Official comparison: browser VDO.Ninja's `closePC` / `closeRPC` paths treat
  `bye` and cleanup as peer teardown, then centralize closing the WebRTC
  objects. In the native plugin, libdatachannel callbacks can run on RTC-owned
  threads, so teardown must not destroy or mutate RTC callback state while a
  plugin media lock is held.
- Workflow: release now marks the peer retired before cleanup, snapshots
  peer-owned RTC handles under `mediaMutex`, clears PeerConnection, Track, and
  owned DataChannel callbacks after releasing that lock, then resets the
  peer-owned handles while local shared pointers keep final destruction outside
  the lock.
- Why this matters: a peer removed by data-channel `bye`, signaling cleanup,
  OBS stop, or terminal RTC state cannot continue media sends, and callback
  removal/destruction cannot re-enter plugin state while `mediaMutex` is held.
- Review rule: never clear libdatachannel callbacks, close/destroy
  PeerConnection/Track/DataChannel objects, or invoke their methods while
  holding `peersMutex` or `mediaMutex`; snapshot handles first, then call out.

Fix: advertised publisher codec support matches implemented publisher output.

- Source anchors: `vdoninja_service` metadata in `plugin-main.*`,
  `VDONinjaOutput`, `VDONinjaPeerManager::setupPublisherTracks`,
  `VDONinjaPeerManager::setVideoCodec`.
- Workflow: OBS service metadata advertises H.264 video and Opus audio for
  publishing. Publisher peer SDP is also clamped to H.264. Native receive still
  supports VP9 or H.264 video plus Opus audio.
- Why this matters: native VP9 receive support no longer implies an unsupported
  VP9 publisher path through OBS service selection.
- Review rule: expose VP9 publisher support only after OBS encode, RTP
  packetization, SDP, service metadata, and validation all support it end to
  end.

Fix: data-channel sends and callbacks follow the lock/send boundary.

- Source anchors: `VDONinjaPeerManager::sendDataToAll`,
  `VDONinjaPeerManager::sendDataToPeer`, `VDONinjaDataChannel`.
- Workflow: outbound data-channel targets and payloads are snapshotted under
  plugin locks; `DataChannel::send` runs after those locks are released.
  Handler callbacks are copied under the data-channel mutex and invoked after
  the mutex is released.
- Protocol check: official VDO.Ninja direct data-channel helpers send JSON
  payloads directly on `sendChannel` or `receiveChannel`; targeted fallback
  routing still needs `UUID`/`session` wrapping for signaling-over-data-channel.
  Official keyframe requests use `keyframe`, with `requestKeyframe` accepted by
  the plugin as compatibility input only.
- Why this matters: a browser-side response or RTC callback triggered by a data
  send cannot re-enter plugin peer state while `peersMutex` is held, and output
  stop no longer races unsynchronized data-channel callback fields.

Fix: outbound signaling objects match official VDO.Ninja shapes more closely.

- Source anchors: `VDONinjaSignaling::sendOffer`,
  `VDONinjaSignaling::sendAnswer`,
  `VDONinjaSignaling::sendAnswerViaDataChannel`,
  `VDONinjaSignaling::sendIceCandidate`,
  `VDONinjaSignaling::sendIceCandidateViaDataChannel`,
  `VDONinjaPeerManager::bundleAndSendCandidates`.
- Official comparison: VDO.Ninja puts SDP only in `description`, uses
  `type:"local"` for publisher-side candidates, and uses `type:"remote"` for
  viewer-side candidates.
- Workflow: offer/answer messages no longer duplicate top-level `sdp` or
  offer/answer `type` beside `description`. Candidate senders now choose
  `local` for publisher peers and `remote` for native viewer peers.
- Why this matters: native viewer ICE now routes like official VDO.Ninja
  `rpcs` candidate traffic, and outbound SDP messages avoid accidental custom
  protocol shape.
- Review rule: before adding an outbound signaling key, confirm the same key is
  consumed on the official path or document it as a plugin-only data-channel
  extension, not a VDO.Ninja signaling object.

Fix: unsupported teardown signaling requests were removed.

- Source anchors: `VDONinjaSignaling::leaveRoom`,
  `VDONinjaSignaling::stopViewing`.
- Official comparison: current official `webrtc.js` and `hss/gemini.js` do not
  define outbound `leaveroom` or `stopPlay` WebSocket request handling.
- Workflow: these methods now clear plugin room/view state only. Server-visible
  teardown continues through WebSocket disconnect, server cleanup, and peer/data
  channel `bye` messages.
- Why this matters: plugin teardown no longer sends protocol objects that the
  official app/server path does not consume, reducing confusing no-op signaling
  and future stale-queue risks.
- Review rule: C++ lifecycle method names are not protocol proof. Confirm the
  actual official request string before sending it over WebSocket.

Fix: unused generic WebSocket data object was removed.

- Source anchor: removed `VDONinjaSignaling::sendDataMessage`.
- Workflow: direct peer data travels over data channels. WebSocket signaling
  remains limited to room, seed/play, SDP, ICE, cleanup, alert, and listing
  style messages.
- Why this matters: the plugin no longer exposes a helper for inventing
  `{UUID,data}` WebSocket messages that official VDO.Ninja does not use as a
  general signaling contract.
- Review rule: data-channel payloads can be plugin/app-level JSON; WebSocket
  signaling payloads should stay aligned with the official server/client
  contract.

Fix: official data-channel stats request/response fields are handled.

- Source anchors: `VDONinjaDataChannel::parseMessage`,
  `VDONinjaOutput::buildRemoteStatsMessage`,
  `VDONinjaOutput::remoteStatsThread`, publisher data-channel callback.
- Official comparison: VDO.Ninja responds to `requestStats` and
  `requestStatsContinuous` with `remoteStats`; browser viewers consume
  `remoteStats`, not a generic top-level `stats` field. Official
  `requestStatsContinuous:true` starts a 3000 ms interval, while false clears
  it.
- Workflow: the plugin now classifies both official `remoteStats` and legacy
  `stats` payloads as stats replies, preserving the raw nested object for
  telemetry consumers. It also classifies `requestStats` and
  `requestStatsContinuous` as known stats-request messages with explicit
  immediate, continuous-start, and continuous-stop modes. Rich native
  `remoteStats` snapshots are sent only when output `enableRemote` is enabled.
  Immediate unauthenticated requests get `{remoteStats:{}}`; continuous-start
  unauthenticated requests also get `{remoteStats:{}}` but are not subscribed.
  Continuous-stop and peer disconnect remove the subscription; output stop
  clears subscribers and joins the worker before peer teardown.
- Authorization comparison: official VDO.Ninja exposes broad peer stats to directors
  and authorized remote clients. Unauthenticated requesters get a filtered
  stats subset that excludes guest peers and room-internal state. The plugin's
  native snapshot contains internal viewer role/state/data-channel fields, so
  it is treated as remote-control diagnostics rather than the unauthenticated
  filtered stats surface.
- Validation: focused data-channel tests verify all three request modes:
  immediate, continuous start, and continuous stop. The OBS plugin build
  validates the output-owned worker and lifecycle wiring.
- Why this matters: stats replies from official browser publishers are no
  longer dropped into the generic/custom path, and official viewers/directors
  asking the plugin publisher for continuous stats no longer get only the first
  snapshot.
- Review rule: if a data-channel helper emits plugin-local fields such as
  `tallyOn`, `tallyPreview`, `tallyOff`, or `audioMuted`, document that as
  compatibility. Official OBS/source state should remain `obsState`.

Fix: official OBS/source state is classified separately from mute state.

- Source anchors: `VDONinjaDataChannel::parseMessage`,
  `tests/test-data-channel.cpp`, official VDO.Ninja `manageSceneState` in
  `C:\Users\steve\Code\vdoninja\lib.js`.
- Official comparison: browser VDO.Ninja handles `obsState.visibility`,
  `obsState.sourceActive`, `obsState.streaming`, `obsState.recording`,
  `obsState.virtualcam`, `obsState.details`, and top-level `sceneDisplay` /
  `sceneMute` through `manageSceneState`. `obsState.visibility` is used by
  official bitrate optimization before later `bitrate` handling, while mute
  state is still carried by `info.muted`, `info.video_muted_init`,
  `muteState`, and `videoMuted`.
- Workflow: the plugin now classifies `obsState`, `sceneDisplay`, and
  `sceneMute` as `ObsState` data-channel messages and preserves the raw
  payload. The parser does not treat these fields as mute state and does not
  invent a media-suppression side effect.
- Validation: focused data-channel tests cover official `obsState`,
  `sceneDisplay`, and `sceneMute` payloads. The tests failed before the
  `ObsState` parser type existed and pass after the fix.
- Why this matters: official OBS/source state messages no longer fall through
  the unknown-message path, and future receiver/publisher logic can key off a
  protocol-correct state classification.
- Review rule: source visibility/OBS activity and media mute are separate
  protocol concepts. Keep `obsState` in the source-state bucket unless a
  distinct official optimization path is implemented.

Fix: official bitrate media-control requests gate publisher sends.

- Source anchors: `VDONinjaDataChannel::parseMediaControl`,
  `VDONinjaOutput` publisher data-channel callback,
  `VDONinjaPeerManager::setPeerMediaSendEnabled`,
  `VDONinjaPeerManager::sendVideoFrameToPeerHandle`,
  `VDONinjaPeerManager::sendAudioFrameToPeer`, official VDO.Ninja
  `requestRateLimit`, `requestAudioRateLimit`, `limitBitrate`, and
  `limitAudioBitrate` in `C:\Users\steve\Code\vdoninja\webrtc.js`.
- Official comparison: browser VDO.Ninja sends `bitrate` and `audioBitrate`
  over the data channel when a viewer wants the publisher to rate-limit.
  Browser publisher handling applies those values to RTCRtpSender encodings.
  A value of `0` sets the sender inactive; negative or positive values keep it
  active. Official code also recognizes `targetBitrate`,
  `targetAudioBitrate`, `optimizedBitrate`, and `requestResolution`.
- Workflow: the plugin now classifies pure official media-control payloads as
  `MediaControl` and extracts field presence/value with top-level structured
  parsing. Publisher output runs that extraction as a side pass because
  official VDO.Ninja handles message fields independently after scene-state
  handling. This keeps combined messages such as `obsState` plus `bitrate`
  actionable without raw string scans, while nested fields inside `remoteStats`
  are ignored. Publisher output applies the safe sender-active subset:
  `bitrate:0` stops video RTP sends to that peer, `audioBitrate:0` stops audio
  RTP sends to that peer, and nonzero values re-enable sends. Re-enabling video
  marks the peer as awaiting a keyframe and primes the viewer with the cached
  keyframe when available. The plugin does not claim browser-equivalent
  per-peer bitrate scaling, resolution scaling, or target-bitrate control for
  already-encoded OBS media.
- Validation: focused data-channel tests cover `bitrate`, `audioBitrate`,
  `requestResolution`, bitrate extraction, structured extraction from combined
  top-level messages, and ignoring nested stats bitrate fields. The original
  parser tests failed before the `MediaControl` parser type/helper existed and
  pass after the fix; the combined/nested tests guard the publisher fanout rule.
  The OBS DLL build validates the publisher callback and peer-manager
  send-gating code.
- Why this matters: official viewers/directors that hide, optimize, or
  unhide a publisher now get the same active/inactive media effect for plugin
  publisher output instead of having those messages fall through as unknown.
- Review rule: separate active/inactive gating from bitrate quality control.
  Do not implement full bitrate/resolution behavior without a concrete encoder
  or RTP-path mechanism and validation.

Fix: official screen-share state is classified as known data-channel state.

- Source anchors: `VDONinjaDataChannel::parseMessage`,
  `VDONinjaDataChannel::parseScreenShareState`, `tests/test-data-channel.cpp`,
  official VDO.Ninja `webrtc.js` initial `msg.info.screenShareState` send path
  and receiver handling for `info.screenShareState`, `screenShareState`, and
  `screenStopped`.
- Official comparison: browser VDO.Ninja advertises initial screen-share state
  inside the peer `info` object, updates live screen-share state with
  top-level `screenShareState`, and marks screen stop with top-level
  `screenStopped`. The browser receiver uses those fields to update RPC
  screen-share state and mixer/UI behavior.
- Workflow: the plugin now classifies official screen-share state payloads as
  `ScreenShareState` and extracts field presence/value for
  `info.screenShareState`, `screenShareState`, and `screenStopped`. This is a
  parser/state alignment fix only; the plugin does not currently own a
  separate browser-style screen-share mixer model to mutate from these fields.
- Validation: focused data-channel tests cover official initial screen-share
  state, live screen-share state, screen-stop state, and field extraction. They
  failed before `ScreenShareState` and `parseScreenShareState` existed and pass
  after the fix.
- Why this matters: official browser screen-share state no longer falls through
  the unknown-message path, and future screen-share-specific plugin behavior
  has a protocol-correct state bucket.
- Review rule: keep screen-share state separate from mute, OBS/source
  visibility, bitrate optimization, peer cleanup, and endpoint hangup.

Fix: official director-video state is classified as known data-channel state.

- Source anchors: `VDONinjaDataChannel::parseMessage`,
  `VDONinjaDataChannel::parseDirectorVideoState`,
  `tests/test-data-channel.cpp`, official VDO.Ninja `webrtc.js` initial
  `msg.info.directorVideoMuted` send path and receiver handling for
  `info.directorVideoMuted`, `directVideoMuted`, `virtualHangup`, and
  `remoteVideoMuted`.
- Official comparison: browser VDO.Ninja advertises initial director video-hide
  state inside the peer `info` object. Director hide/show updates use
  `directVideoMuted` and can include `target:true` for the recipient itself or
  a target UUID for another peer. Director virtual video suppression uses
  `virtualHangup`. Director remote video mute uses `remoteVideoMuted`, which
  browser publishers apply to local remote-video-mute state and then report to
  peers as `videoMuted`.
- Workflow: the plugin classifies passive `info.directorVideoMuted`,
  `directVideoMuted`, and `virtualHangup` payloads as `DirectorVideoState` and
  extracts field presence/value for review/state alignment. Native source
  receive also extracts receiver-side video suppression into split state:
  media video mute, director video mute, and virtual hangup. The effective
  native render gate is true while any of those flags is true, so clearing
  ordinary `videoMuted` cannot unhide video while `virtualHangup` or director
  video-hide remains active. Director video-hide target scope is preserved:
  `target:true` applies to this native source, a matching target UUID applies to
  the local peer state, and a different target UUID is ignored instead of hiding
  the current OBS source. This is not full director authorization; the native
  receiver path has no `directorList` model today. Top-level `remoteVideoMuted`
  is classified as `UnsupportedControl` because official VDO rejects it for
  non-directors and this plugin cannot prove director identity. Director lists
  and mixer UI side effects are not implemented by this parser bucket.
- Validation: focused data-channel tests cover initial director video-hide
  state, live director video-hide state, self-targeted `target:true`, target
  UUID extraction, virtual hangup state, unsupported top-level
  `remoteVideoMuted`, and mixed initial `info` extraction.
  They failed before `DirectorVideoState` and `parseDirectorVideoState` existed
  and pass after the fix. The OBS plugin build validates the unsupported-control
  rejection wiring for top-level `remoteVideoMuted`.
- Validation: `DataChannelTest.ExtractsReceiverVideoSuppressionState` failed
  before receiver-side extraction included `directVideoMuted`,
  `info.directorVideoMuted`, and `virtualHangup`, and passes after
  `parseReceiverVideoSuppression` and native source split-state handling were
  added.
- Validation: `DataChannelTest.ReceiverDirectorVideoSuppressionUsesOfficialTargetScope`
  failed while string-targeted `directVideoMuted` updates applied to any native
  source. It passes after receiver-side suppression preserved the official
  target fields and native source state checks the target before updating
  `remoteDirectorVideoMuted`.
- Why this matters: official director video-hide and virtual-video suppression
  messages no longer fall through the unknown-message path or get conflated
  with endpoint `hangup` / peer `bye:true`.
- Review rule: do not map `virtualHangup` to OBS output stop. Treat it as
  director-controlled video state unless a specific, authorized runtime owner
  implements the official side effects.
- Review rule: do not re-enable publisher-side `remoteVideoMuted` effects under
  the broad remote-control switch. Add a real director identity model first.

Fix: official director speaker/display mute state is classified separately from
publisher mute state.

- Source anchors: `VDONinjaDataChannel::parseMessage`,
  `VDONinjaDataChannel::parseDirectorAudioState`,
  `tests/test-data-channel.cpp`, official VDO.Ninja `webrtc.js` initial
  `msg.info.directorSpeakerMuted` / `msg.info.directorDisplayMuted` send path
  and receiver handling for `speakerMute` / `displayMute`.
- Official comparison: browser VDO.Ninja advertises initial director
  speaker/display mute state inside peer `info`. Live director controls use
  top-level `speakerMute` and `displayMute`. These are not the same protocol
  concept as publisher `muteState` or `videoMuted`.
- Workflow: the plugin classifies passive `info.directorSpeakerMuted` and
  `info.directorDisplayMuted` payloads as `DirectorAudioState` and extracts
  live `speakerMute` / `displayMute` only through the helper for review/state
  alignment. Primary dispatch classifies live `speakerMute` / `displayMute` as
  unsupported because official VDO rejects those controls for non-directors and
  this plugin cannot prove director identity.
- Validation: focused data-channel tests cover initial director speaker/display
  state, unsupported live `speakerMute`, unsupported live `displayMute`, helper
  extraction, and mixed initial `info` extraction. They failed before
  `DirectorAudioState` and
  `parseDirectorAudioState` existed and pass after the fix.
- Why this matters: official director audio/display state no longer falls
  through the unknown-message path or gets conflated with publisher media mute
  state.
- Review rule: keep director-controlled receiver/output state separate from
  publisher-owned media state. Do not route `speakerMute` / `displayMute`
  through ordinary `OnMuteChange` without preserving that distinction.

Fix: official live transform commands are rejected while passive transform
state remains parsed.

- Source anchors: `VDONinjaDataChannel::parseMessage`,
  `VDONinjaDataChannel::parseDirectorTransformState`,
  `tests/test-data-channel.cpp`, official VDO.Ninja `webrtc.js` initial
  `msg.info.directorMirror` / `msg.info.directorFlip` send path and receiver
  handling for `mirrorGuestState` / `mirrorGuestTarget`, official remote
  `rotate` command handling, and official `rotate_video` send/receive paths.
- Official comparison: browser VDO.Ninja advertises initial director mirror and
  flip state inside peer `info`, and uses `info.rotate_video` / top-level
  `rotate_video` for current video rotation. Live director mirror controls use
  top-level `mirrorGuestState` plus `mirrorGuestTarget`; live remote rotate
  commands use top-level `rotate`. Official receiver code rejects live
  `rotate` and `mirrorGuestState` controls from unauthorized senders.
- Workflow: the plugin now classifies passive transform state
  (`info.directorMirror`, `info.directorFlip`, `info.rotate_video`, and
  top-level `rotate_video`) as `DirectorTransformState`. It classifies live
  transform commands (`rotate`, `mirrorGuestState` plus `mirrorGuestTarget`) as
  `UnsupportedControl` so publisher output returns VDO-shaped rejections
  instead of silently ignoring commands it cannot apply. The transform helper
  still extracts `mirrorGuestState`, `mirrorGuestTarget`, and `rotate` command
  intent for review/state mapping.
- Validation: focused data-channel tests cover passive initial/live transform
  state, helper extraction for mirror/rotate command payloads, and unsupported
  primary dispatch for live mirror/rotate commands.
  `DataChannelTest.ParsesOfficialRemoteRotateCommandMessage` and
  `DataChannelTest.ParsesOfficialMirrorGuestStateMessage` failed after the
  rejection expectations were added because both commands were still classified
  as `DirectorTransformState`; they pass after moving those command keys to the
  unsupported-control path.
- Why this matters: official director mirror/flip/rotation state remains
  recognized, while unsupported live transform commands no longer disappear as
  no-ops or get conflated with media mute, endpoint hangup, or peer cleanup.
- Review rule: keep transform state separate from media availability. Applying
  mirror/flip to OBS output should happen only in a receiver owner that can map
  target UUIDs to the correct native source or browser element; applying
  rotation requires an explicit native/source transform owner.

Fix: official publisher mute state affects native receiver output.

- Source anchors: `VDONinjaDataChannel::parseMuteState`,
  native/source data-channel callback in `VDONinjaSource`,
  `VDONinjaSource::handleRemoteMuteState`,
  `VDONinjaSource::outputDecodedVideoFrame`, and
  `VDONinjaSource::outputDecodedAudioFrame`.
- Official comparison: browser VDO.Ninja advertises initial audio mute state in
  `info.muted`, initial video mute state in `info.video_muted_init`, sends
  dynamic audio mute changes as top-level `muteState`, and sends dynamic video
  mute changes as top-level `videoMuted`. Browser viewers hide video on
  `info.video_muted_init`/`videoMuted:true` and update audio mute UI/state on
  `info.muted`/`muteState`.
- Workflow: the plugin preserves which mute fields were present, so a
  video-only `videoMuted` update does not accidentally rewrite audio state.
  Native/source receive paths now apply `info.muted`/`muteState` to remote
  audio output and `info.video_muted_init`/`videoMuted` to remote video output.
  Audio mute marks OBS source audio inactive and suppresses decoded audio
  output. Video mute clears the current OBS frame and suppresses decoded video
  output until the publisher sends an explicit video unmute update.
- Validation: `DataChannelTest.ExtractsOfficialMuteStateAsAudioOnly`,
  `DataChannelTest.ExtractsOfficialVideoMutedAsVideoOnly`, and
  `DataChannelTest.ExtractsPluginAudioVideoMuteState` verify the dynamic
  field-presence contract. `DataChannelTest.ParsesOfficialInitialInfoMuteStateMessage`
  and `DataChannelTest.ExtractsOfficialInitialInfoMuteState` failed before
  nested `info` support and pass after the fix. The OBS plugin build validates
  native/source callback and output wiring.
- Why this matters: native OBS sources no longer keep showing a stale frame or
  playing audio after an official browser publisher reports mute state over the
  data channel.
- Review rule: official media-state fields are independent. Preserve presence
  before mutating receiver state; do not treat an absent field as an explicit
  unmute.

Fix: official room transfer/join notifications map to local room events and
state.

- Source anchors: `parseSignalingMessage`,
  `VDONinjaSignaling::processMessage`, `tests/test-signaling-state.cpp`.
- Official comparison: `hss/gemini.js` sends `request:"transferred"` with a
  `list` after room transfer/admission and sends `request:"someonejoined"` with
  `UUID` and optional `streamID` to existing room members.
- Workflow: `transferred` is handled as a listing-style room state response.
  `someonejoined` with `streamID` is handled as a stream-added event. Incremental
  room stream add/remove events now also update `currentRoom.members`, so local
  room state remains aligned with the callbacks fired for auto-inbound and
  native retry flows.
- Validation: `SignalingStateTest.IncrementalRoomStreamEventsUpdateMemberSnapshot`
  failed before the state update because the member snapshot stayed at
  `{ "cam_1" }` after `someonejoined` added `cam_2`; it passes after the fix.
- Why this matters: auto-inbound and native room retry flows no longer miss
  streams announced through the current official room-join notification path,
  and code that consults local room state no longer reads a stale listing after
  incremental room events.
- Review rule: room events are not limited to names containing
  `videoaddedtoroom`; compare against the server's actual admission broadcasts
  before treating a request as generic. Any callback-visible room event should
  also be checked against the stored room-state model.

Fix: official data-channel ping/pong liveness is handled without changing token
type.

- Source anchors: `VDONinjaDataChannel::parseMessage`,
  `VDONinjaDataChannel::createPongMessage`, publisher data-channel callback in
  `VDONinjaOutput`, native/source data-channel callback in `VDONinjaSource`.
- Official comparison: `webrtc.js` responds to inbound `ping` by sending
  `pong` with the same value on both `pcs` and `rpcs` paths, records inbound
  `pong` as `lastPongToken`, and compares it against the original ping token
  before recovering or closing a disconnected peer.
- Workflow: the plugin now classifies `ping` and `pong` as known data-channel
  messages. Publisher output and native/source receive paths answer `ping` by
  sending `pong` to the same peer and preserving the raw JSON token, so numeric
  official `Date.now()` tokens stay numeric.
- Validation: `DataChannelTest.ParsesOfficialPingMessage` and
  `DataChannelTest.ParsesOfficialPongMessage` failed before parser support
  because both payloads were `Unknown`; they pass after the fix.
  `DataChannelTest.CreatesOfficialPongMessageWithoutStringifyingToken` verifies
  the `pong` token is not stringified.
- Why this matters: official browser peers no longer mistake this plugin for an
  unresponsive peer during disconnected-state liveness checks, regardless of
  whether OBS is publishing or natively viewing.
- Review rule: any message value used as an official token, session id, or
  comparison operand should preserve its expected JSON type unless the official
  code explicitly accepts string coercion.

Fix: official direct data-channel SDP/ICE signaling reaches shared signaling
processing on publisher and native/source receive paths.

- Source anchors: `VDONinjaDataChannel::parseMessage`,
  `VDONinjaDataChannel::prepareSignalingMessage`, publisher data-channel
  callback in `VDONinjaOutput`, native/source data-channel callback in
  `VDONinjaSource`.
- Official comparison: `webrtc.js` assigns the sender `UUID` to each incoming
  data-channel object and processes `description`, `candidate`, and
  `candidates` before app/control messages.
- Workflow: the plugin now classifies those direct data-channel SDP/ICE payloads
  as `Signaling`. Publisher output and native/source receive paths inject the
  data-channel sender UUID when the payload lacks `UUID`/`uuid`/`from`, then
  route it through `VDONinjaSignaling::processIncomingMessage` so the existing
  answer and ICE handlers apply. Signaling fields take precedence over app
  fields, matching the official handler order.
- Validation: `DataChannelTest.ParsesOfficialDataChannelDescriptionAsSignaling`
  and `DataChannelTest.ParsesOfficialDataChannelCandidateBundleAsSignaling`
  failed before parser support because both payloads were `Unknown`; they pass
  after the fix. `DataChannelTest.GivesOfficialDataChannelSignalingPrecedenceOverAppFields`
  failed while chat parsing ran before signaling parsing and passes after the
  order was corrected. `DataChannelTest.PreparesOfficialDataChannelSignalingWithSenderUuid`,
  `DataChannelTest.PreparesOfficialDataChannelCandidateWithSenderUuid`, and
  `DataChannelTest.PreparesOfficialDataChannelCandidateBundleWithSenderUuid`
  verify UUID injection for direct SDP/ICE shapes, while
  `DataChannelTest.KeepsExistingUuidWhenPreparingOfficialDataChannelSignaling`
  verifies explicit UUIDs are preserved.
- Why this matters: recovery or renegotiation traffic sent over an established
  data channel no longer disappears because the shared signaling parser has the
  same sender identity official VDO.Ninja supplies before handling SDP/ICE.
- Review rule: when direct data-channel signaling is accepted, preserve the
  official sender identity behavior before feeding the shared signaling parser.

Fix: official ICE-restart recovery requests reach publisher re-offer handling.

- Source anchors: `VDONinjaDataChannel::parseMessage`,
  `parseSignalingMessage`, `VDONinjaSignaling::handleRequest`,
  `VDONinjaPeerManager::requestIceRestart`, publisher data-channel callback in
  `VDONinjaOutput`.
- Official comparison: `webrtc.js` handles `iceRestartRequest` both from
  WebSocket messages and direct data-channel messages. The data-channel handler
  checks field presence and returns before later fields; WebSocket handling
  checks truthiness plus `UUID`. Browser VDO.Ninja calls `restartIce()` when
  available or creates an offer with ICE restart intent.
- Workflow: the plugin now classifies data-channel `iceRestartRequest` by
  presence and routes it directly to the peer manager. Presence also suppresses
  independent keyframe fanout because official VDO returns from this terminal
  branch. WebSocket `iceRestartRequest` is normalized into a signaling request
  and dispatched through a dedicated ICE-restart callback, not the ordinary
  offer-request path that suppresses duplicate connected-session offers. The
  peer manager creates a fresh publisher offer for a live publisher peer only
  when the libdatachannel signaling state is stable.
- Validation: `DataChannelTest.ParsesOfficialIceRestartRequest`,
  `SignalingProtocolTest.ParsesOfficialIceRestartRequest`, and
  `SignalingStateTest.OfficialIceRestartRequestDispatchesIceRestartRequest`
  failed before parser/callback support and pass after the fix. The OBS plugin
  build validates the peer-manager/output wiring.
- Validation: `DataChannelTest.ParsesOfficialIceRestartRequestByPresence` and
  `DataChannelTest.IceRestartRequestPreventsIndependentKeyframeFanoutByPresence`
  failed before data-channel ICE restart changed from truthy-only to
  presence-based.
- Why this matters: official browser recovery no longer sends an ICE restart
  request that the plugin drops as unknown or routes into duplicate-offer
  suppression.
- Review rule: recovery requests are not normal first-offer requests. Keep them
  distinct from room admission and duplicate offer suppression so connected
  peers can still ask for a repair offer.

Fix: official remote/director recovery controls are classified and mapped to
OBS-native repair actions.

- Source anchors: `VDONinjaDataChannel::parseMessage`,
  `VDONinjaDataChannel::parseRecoveryControl`, publisher data-channel callback
  in `VDONinjaOutput`, official VDO.Ninja `directRefreshVideo`,
  `directRefreshConnection`, `meshRefreshVideo`, `meshRefreshConnection`,
  `meshRefreshAll`, and `meshRestartWhip` in
  `C:\Users\steve\Code\vdoninja\lib.js`, plus receiver handling in
  `C:\Users\steve\Code\vdoninja\webrtc.js`.
- Official comparison: browser VDO.Ninja sends `refreshVideo`,
  `refreshMicrophone`, `refreshConnection`, `refreshAll`, and `restartWhip` as
  direct data-channel control fields. Browser receiver handling is
  presence-based, so a false JSON value still triggers the request path. It
  refreshes the local microphone device for `refreshMicrophone`, refreshes the
  local video device for `refreshVideo`, restarts ICE on peer connections for
  `refreshConnection`, does media-device refresh plus ICE restart for
  `refreshAll`, and restarts a WHIP publisher connection for `restartWhip`.
  Non-director rejection handling is an ordered `else if` chain, so mixed
  recovery-control messages reject only the first recognized field in the
  official order: `refreshMicrophone`, `refreshVideo`, `refreshConnection`,
  then `refreshAll`.
  The game-capture native map adds an important scope rule: Control Center
  `refreshConnection` / `refreshAll` are publisher-wide recovery controls,
  while data-channel/WebSocket `iceRestartRequest` remains peer-scoped.
- Workflow: the plugin now classifies those fields as `RecoveryControl` and
  treats field presence as the request. Publisher output honors actionable
  recovery requests only when `enableRemote` is enabled. `refreshVideo` primes the
  requesting viewer with the cached keyframe, `refreshConnection` snapshots all
  current publisher peers and runs the existing peer-manager ICE
  restart/re-offer path for each eligible peer, and `refreshAll` performs both
  implemented repairs. Unsupported or disabled recovery fields return
  VDO-shaped `rejected` responses because OBS native publisher output is not
  browser microphone-device or WHIP output. Disabled publisher output uses
  `VDONinjaDataChannel::recoveryControlRejectionName` so combined recovery
  fields match the official non-director rejection order instead of preferring
  `refreshAll`.
- Validation: focused data-channel tests for `refreshVideo`,
  `refreshMicrophone`, `refreshConnection`, `refreshAll`, `restartWhip`, and
  `parseRecoveryControl` failed before the `RecoveryControl` parser type/helper
  existed and pass after the fix with
  `build\Debug\vdoninja-tests.exe --gtest_filter=DataChannelTest.ParsesOfficialRefreshVideoControlMessage:DataChannelTest.ParsesOfficialRefreshMicrophoneControlMessage:DataChannelTest.ParsesOfficialRefreshConnectionControlMessage:DataChannelTest.ParsesOfficialRefreshAllControlMessage:DataChannelTest.ParsesOfficialRestartWhipControlMessage:DataChannelTest.ExtractsOfficialRecoveryControl`.
- Validation: `DataChannelTest.RecoveryControlsUseOfficialPresenceSemantics`
  failed before `parseRecoveryControl` changed from boolean-value parsing to
  official presence semantics, and passes after the fix.
- Validation: `DataChannelTest.SelectsOfficialRecoveryRejectionOrder` failed
  while the plugin selected `refreshAll` before earlier official recovery
  fields in mixed disabled-output payloads, and passes after the selector was
  changed to official rejection order.
- Why this matters: official director/remote recovery controls no longer fall
  through as unknown/custom messages, and the plugin publisher now performs the
  OBS-native repair actions it can safely own.
- Review rule: do not map `refreshConnection` / `refreshAll` to peer cleanup or
  endpoint stop. They are publisher-wide Control Center repair requests. Do not
  implement `restartWhip` side effects in native publisher output without a real
  WHIP owner; reject it through the official response family.

Fix: unsupported official Control Center fields are classified and rejected.

- Source anchors: `VDONinjaDataChannel::parseMessage`,
  `VDONinjaOutput::sendRejectedControlToPeer`, publisher data-channel callback
  in `VDONinjaOutput`, and game-capture's VDO.Ninja flow map notes for
  settings readback, `requestVideoHack`, advanced audio controls, browser
  reload/scale, PTZ, director-only controls, and volume.
- Official comparison: VDO.Ninja has recognized Control Center fields that are
  meaningful for browser endpoints or director-authenticated browser paths but
  not for this native OBS publisher. The receiver should fail those known
  controls with `rejected` rather than letting callers wait on a silent no-op.
  Official browser handling checks `requestStats` and then `keyframe` before
  many later Control Center-only paths, so combined payloads can still produce
  stats/keyframe side effects before the unsupported field is rejected.
- Workflow: the plugin now classifies unsupported official Control Center
  fields as `UnsupportedControl` and replies to the requesting peer with
  `{"rejected":"<field>"}`. This currently covers settings readback,
  `requestVideoHack`, device switches, WebAudio mutations, browser reload and
  sender scale, PTZ controls, and director-only controls such as
  `remoteVideoMuted`, `speakerMute`, `displayMute`, `hangup`, `volume`,
  `keyframeRate`, `micIsolate`, and `lowerVolume`. Unsupported-control
  classification runs before passive mute and OBS/source state buckets so
  combined payloads still produce the official rejection. Stats requests and
  keyframe requests run before unsupported-control classification, matching the
  official publisher handler's order; publisher output uses
  `VDONinjaDataChannel::unsupportedControlName` to reject the unsupported field
  after those earlier side effects are processed.
- Validation: focused data-channel tests cover `getAudioSettings`,
  `requestVideoHack`, `requestAudioHack`, `zoom`, `keyframeRate`,
  `micIsolate`, and `lowerVolume` classification as unsupported controls.
  `DataChannelTest.ParsesOfficialKeyframeRateAsUnsupportedControl`,
  `DataChannelTest.ParsesOfficialMicIsolateAsUnsupportedControl`, and
  `DataChannelTest.ParsesOfficialLowerVolumeAsUnsupportedControl` failed
  before these official fields were added to the unsupported bucket. The
  current director-only pass also covers unsupported `remoteVideoMuted`,
  `speakerMute`, and `displayMute`, including combined `videoMuted` and
  `obsState` payloads that failed before the precedence fix.
- Validation: `DataChannelTest.StatsRequestTakesPrecedenceOverUnsupportedControl`
  and `DataChannelTest.KeyframeRequestTakesPrecedenceOverUnsupportedControl`
  failed before stats/keyframe moved ahead of unsupported-control
  classification. `DataChannelTest.ExtractsUnsupportedControlNameFromCombinedActivePayload`
  guards the separate rejection extraction used by publisher output.
- Review rule: do not add partial browser-only side effects unless the native
  endpoint owns the matching resource and can produce the official response
  fields. Unsupported official control messages should be explicit failures.

Fix: OBS remote-control commands require VDO's `remote` field.

- Source anchors: `VDONinjaDataChannel::parseMessage`,
  `VDONinjaDataChannel::parseRemoteControlMessage`, publisher data-channel
  callback in `VDONinjaOutput`, and official VDO.Ninja `processOBSCommand` in
  `C:\Users\steve\Code\vdoninja\lib.js`.
- Official comparison: official OBS-control senders add `remote` to
  `obsCommand` messages. Official receivers reject `obsCommand` when `remote`
  is missing or does not match the endpoint's remote setting.
- Workflow: the plugin now classifies bare `obsCommand` and bare top-level
  `action` as unsupported `obsCommand`, causing publisher output to return
  `{"rejected":"obsCommand"}`. Messages with top-level `remote` still parse as
  plugin remote-control input and still require output `enableRemote` before
  effects run.
- Validation: `DataChannelTest.RejectsObsCommandWithoutRemoteField`,
  `DataChannelTest.RejectsLegacyActionWithoutRemoteField`, and
  `DataChannelCallbackTest.DoesNotTriggerRemoteControlFromObsCommandWithoutRemote`
  failed before this parser change and pass after it.
- Review rule: do not use `enableRemote` as a substitute for the VDO message
  shape. This plugin still lacks token matching, but accepted OBS-control
  payloads must at least carry the official `remote` field.

Fix: official mesh reconnect/map request fields are rejected without director identity.

- Source anchors: `VDONinjaDataChannel::parseMessage`,
  `VDONinjaDataChannel::parseMeshControl`,
  `VDONinjaOutput::buildConnectionMapMessage`, publisher data-channel callback
  in `VDONinjaOutput`, official VDO.Ninja `directReconnectPeer`,
  mesh-data request/response handling in `C:\Users\steve\Code\vdoninja\lib.js`,
  and receiver handling in `C:\Users\steve\Code\vdoninja\webrtc.js`.
- Official comparison: browser VDO.Ninja sends `reconnectPeer` to tell one
  guest to reconnect to a named peer. Browser handling closes the named `pcs`
  or `rpcs` peer and expects normal renegotiation to recover. Browser VDO.Ninja
  also sends `getConnectionMap` for mesh visualization and expects a
  `connectionMap` response containing local identity, label/browser metadata,
  connection rows, and the requester's UUID. Official receiver code rejects
  `reconnectPeer` and `getConnectionMap` from non-directors.
- Workflow: the plugin now classifies `reconnectPeer` and `getConnectionMap`
  as `UnsupportedControl` so publisher output replies with VDO-shaped
  rejections. It still classifies `connectionMap` as `MeshControl` and the
  helper still extracts all three fields for protocol review. Do not route
  `reconnectPeer` through ICE restart/re-offer or answer `getConnectionMap`
  through the broad `enableRemote` flag; both require a real director identity
  model.
- Validation: focused data-channel tests for `reconnectPeer`,
  `getConnectionMap`, `connectionMap`, and `parseMeshControl` pass with
  `build\Debug\vdoninja-tests.exe --gtest_filter=DataChannelTest.ParsesOfficialReconnectPeerMeshControlMessage:DataChannelTest.ParsesOfficialGetConnectionMapMeshControlMessage:DataChannelTest.ParsesOfficialConnectionMapResponseMessage:DataChannelTest.ExtractsOfficialMeshControl`.
- Validation: after changing expectations to the official director-only
  behavior, the first two focused tests failed because `reconnectPeer` and
  `getConnectionMap` were still classified as `MeshControl`; they pass after
  moving those request fields to the unsupported-control path.
- Validation: `PeerManagerSnapshotTest.ExposesPerPeerMediaSendState` failed
  before `PeerSnapshot` exposed media-send gates and passes after the snapshot
  state was added. That state remains useful for future director-authenticated
  connection-map work, but publisher output no longer serves connection maps to
  remote-token-only requesters.
- Why this matters: official director mesh tools no longer see the plugin
  publisher as a peer that silently drops mesh requests, and a remote reconnect
  request can use the plugin's existing session-aware repair path. Mesh
  visualization also no longer claims a disabled per-peer audio/video sender is
  still active.
- Review rule: keep mesh diagnostics separate from media state. Do not make
  candidate type, bandwidth, or browser metadata look more precise than the OBS
  plugin can prove from its native peer snapshots.

Fix: official peer cleanup reaches publisher peers over signaling and data
channels.

- Source anchors: `VDONinjaDataChannel::parseMessage`,
  `VDONinjaPeerManager::initialize`, publisher data-channel callback in
  `VDONinjaOutput`.
- Official comparison: `lib.js` can send `{videoMuted:true,bye:true}` while
  closing peers so the remote video goes black, and `webrtc.js` treats incoming
  data-channel `bye` field presence as an immediate `closePC(UUID)` request.
  Its WebSocket receive path checks `msg.bye || msg.request == "cleanup"`, so
  false `bye` is not signaling cleanup. Official `webrtc.js` / `lib.js` do not
  use `request:"bye"` as a peer cleanup spelling.
- Workflow: data-channel top-level `bye` field presence and
  `request:"cleanup"` are classified as `PeerBye` before mute/video/source
  state. Publisher output clears telemetry and continuous stats subscription
  state for that peer, then retires the peer through the peer manager. Native
  source receive handles typed direct SDP/ICE signaling and typed peer cleanup
  explicitly instead of scanning raw message text for `"request"` or `"bye"`.
  The signaling parser only treats WebSocket `bye` as cleanup when it is
  truthy, and treats `request:"cleanup"` as cleanup. Data-channel
  `request:"bye"` remains `Unknown`; WebSocket `request:"bye"` remains a
  generic `Request`. The peer manager also consumes signaling peer-cleanup
  callbacks from `VDONinjaSignaling`, so WebSocket or data-channel-carried
  cleanup reaches publisher peers.
- Validation: `DataChannelTest.ParsesOfficialByeAsPeerCleanup`,
  `DataChannelTest.ParsesOfficialCloseMessageAsPeerCleanupBeforeMute`,
  `DataChannelTest.ParsesOfficialByePresenceAsPeerCleanup`,
  `DataChannelTest.ParsesOfficialCleanupRequestAsPeerCleanup`,
  `DataChannelTest.IgnoresUnofficialByeRequestAsPeerCleanup`,
  `SignalingProtocolTest.ParsesCleanupRequestMessage`,
  `SignalingProtocolTest.ParsesByeMessageAsPeerCleanup`,
  `SignalingProtocolTest.IgnoresUnofficialByeRequestAsPeerCleanup`, and
  `SignalingProtocolTest.IgnoresFalseByeMessageAsPeerCleanup` cover the
  official peer cleanup spellings, combined close-payload priority,
  data-channel bye presence behavior, and the false-WebSocket-`bye` guard. The
  OBS plugin build validates the output, source, and peer-manager callback
  wiring.
- Why this matters: closed browser viewers no longer rely only on eventual RTC
  state changes for cleanup, and server/signaling cleanup messages now reach the
  publisher peer map.
- Review rule: keep `bye`/`cleanup` peer-scoped. They should retire a peer and
  clear per-peer state, not stop the whole OBS output.

Fix: official hangup data-channel requests are recognized but rejected without
director identity.

- Source anchors: `VDONinjaDataChannel::parseMessage`, publisher data-channel
  callback in `VDONinjaOutput`.
- Official comparison: `lib.js` direct hangup builds `hangup:true` for the
  target peer, and `webrtc.js` handles incoming `hangup` by field presence when
  deciding whether to call the local session hangup path or reject a
  non-director request.
- Workflow: the plugin classifies `hangup` field presence as a dedicated
  data-channel `Hangup` message. Publisher output now replies with
  `{"rejected":"hangup"}` because the plugin has no director identity model and
  official VDO rejects non-director hangup requests.
- Validation: `DataChannelTest.ParsesOfficialHangupRequest` failed before the
  parser change because `hangup:true` was `Unknown` with no retained payload; it
  passes after the fix.
- Validation: `DataChannelTest.ParsesOfficialHangupRequestByPresence` and
  `DataChannelTest.HangupPreventsIndependentKeyframeFanoutByPresence` failed
  before hangup changed from truthy-only to presence-based terminal handling.
- Why this matters: director-only disconnect requests from non-director peers no
  longer disappear as unknown payloads or stop OBS through the broad remote
  switch.
- Review rule: keep `hangup` and `bye` separate. `hangup` is endpoint stop
  control; `bye`/`cleanup` is peer cleanup.

Fix: active data-channel controls and telemetry responses outrank passive state
buckets.

- Source anchors: `VDONinjaDataChannel::parseMessage`, publisher data-channel
  callback in `VDONinjaOutput`, official VDO.Ninja `manageSceneState` and
  `processOBSCommand` in `C:\Users\steve\Code\vdoninja\lib.js`, plus official
  data-channel field handling in `C:\Users\steve\Code\vdoninja\webrtc.js`.
- Official comparison: browser VDO.Ninja calls `manageSceneState(msg, UUID)` for
  `obsState`, `sceneDisplay`, and `sceneMute`, then continues checking the same
  message object for fields such as `requestStats`, `hangup`, recovery controls,
  rejected director-only controls, and remote/OBS commands. The native receiver
  side also consumes `remoteStats` through an independent field check. State
  handling does not consume the message. This plugin's parser returns one
  primary type, so active controls and telemetry responses need explicit
  priority over passive state buckets.
- Workflow: parser priority now keeps direct signaling, ping/pong, peer cleanup,
  ICE restart, and `hangup` at the front, then classifies stats requests,
  accepted remote-control shapes, chat/tally, keyframe, recovery controls, known
  unsupported Control Center fields, and stats response telemetry
  (`remoteStats` / legacy `stats`) before passive media/source state. Publisher
  output already runs structured media-control extraction separately; it also
  parses recovery fields independently after stats/keyframe handling, and
  extracts unsupported-control names after earlier active side effects so a
  combined active-plus-recovery/unsupported payload still receives the matching
  VDO-shaped behavior.
- Validation: `DataChannelTest.RecoveryControlTakesPrecedenceOverObsState` and
  `DataChannelTest.HangupTakesPrecedenceOverMuteState` failed before recovery
  and hangup were moved ahead of passive state. `DataChannelTest.StatsRequestTakesPrecedenceOverObsState`
  and `DataChannelTest.RemoteControlTakesPrecedenceOverObsState` failed before
  stats requests and accepted remote-control shapes were moved ahead of
  `obsState`. `DataChannelTest.RemoteStatsTakesPrecedenceOverObsState` failed
  before stats response telemetry moved ahead of passive source state.
  `DataChannelTest.StatsRequestTakesPrecedenceOverRecoveryControl` and
  `DataChannelTest.KeyframeRequestTakesPrecedenceOverRecoveryControl` failed
  before recovery moved behind those earlier official checks. The focused stats
  response/request regression now passes 7/7.
- Why this matters: official control messages can arrive with state snapshots or
  state updates in the same JSON object, and stats responses can arrive with
  unrelated state. The native publisher should reject, answer, act on, or cache
  the active/telemetry field instead of silently treating the whole payload as
  passive mute/source state.
- Review rule: when adding a new data-channel field, decide whether it is active
  control, passive state, transport/signaling, or telemetry. Active controls and
  telemetry responses that require a response, side effect, or cache update
  should not sit behind passive state in the single-primary parser unless the
  owner also has an explicit fanout path.

Fix: keyframe requests fan out independently from the primary data-message type.

- Source anchors: `VDONinjaDataChannel::hasKeyframeRequest`,
  `VDONinjaDataChannel::handleMessage`, publisher data-channel callback in
  `VDONinjaOutput`, official VDO.Ninja stats and keyframe checks in
  `C:\Users\steve\Code\vdoninja\webrtc.js`.
- Official comparison: browser VDO.Ninja checks `requestStats` and
  `requestStatsContinuous`, then later checks `keyframe` in the same message
  handler without consuming the object. A combined payload can therefore ask for
  stats and a keyframe in one data-channel message.
- Workflow: keyframe requests are now detected from raw field presence. The
  generic data-channel callback fires for `keyframe` / `requestKeyframe` even
  when the primary parser type is `StatsRequest`, and publisher output primes
  the requesting peer from the same field-level helper. The helper and generic
  callback suppress fanout for official early-return terminal transport
  messages, so combined `bye`, `ping`, `pong`, direct data-channel signaling,
  ICE restart, or hangup payloads do not also request a keyframe.
- Validation: `DataChannelCallbackTest.TriggersOnKeyframeRequestWhenCombinedWithStatsRequest`
  failed before the fix because the primary type was `StatsRequest`; after the
  helper/fanout patch, the focused keyframe/stats regression passes 6/6.
- Validation: `DataChannelTest.KeyframeRequestFanoutIgnoresTerminalTransportMessages`,
  `DataChannelCallbackTest.DoesNotTriggerKeyframeRequestWhenCombinedWithPeerBye`,
  `DataChannelCallbackTest.DoesNotTriggerKeyframeRequestWhenCombinedWithPing`,
  and `DataChannelCallbackTest.DoesNotTriggerKeyframeRequestWhenCombinedWithDirectSignaling`
  failed before terminal-message gating and pass after the gate.
- Why this matters: a stats request should not suppress an independent
  keyframe/PLI request in the same official VDO.Ninja message object.
- Review rule: when a VDO field is handled by official code as an independent
  `if ("field" in msg)` side effect, prefer a field-level helper/fanout in the
  owner instead of relying only on parser priority.

Fix: native receive codec helper matches current native decoder scope.

- Source anchor: `isSupportedNativeVideoCodecName`.
- Workflow: native receive helper accepts H.264 and VP9, while rejecting VP8 and
  AV1.
- Why this matters: retry/decoder policy now matches the actual native receiver
  workflow documented above.

Fix: JSON array parsing advances over scalar entries.

- Source anchor: `JsonParser::getArray` in `src/vdoninja-utils.cpp`.
- Discovery: deterministic fuzz-style signaling tests hung on payloads with
  scalar array members such as `null`, `false`, `true`, or numbers. The parser
  previously advanced only for string and object entries, so scalar entries left
  `pos` unchanged and the array loop could run forever.
- Workflow: array parsing now handles nested arrays and scalar values, advancing
  to the next comma or closing bracket. This protects signaling normalization
  paths that consume official list/candidate-style arrays from malformed or
  mixed-type input.
- Validation: `JsonParserTest.ParsesArrayWithScalarValues`,
  `DataChannelEdgeCaseTest.FuzzesOfficialMessageParserAndExtractors`, and
  `SignalingProtocolTest.FuzzesSignalingNormalizationWithoutCrashing` pass after
  the fix. The signaling fuzz test timed out before this parser hardening.
- Review rule: any parser loop over ad hoc JSON must advance on every branch,
  including unsupported scalar values.

## Validation Snapshot

Last validation for this map revision:

- Current active-control and telemetry-response precedence regression:
  `build\Debug\vdoninja-tests.exe --gtest_filter=DataChannelTest.RecoveryControlTakesPrecedenceOverObsState:DataChannelTest.HangupTakesPrecedenceOverMuteState:DataChannelTest.StatsRequestTakesPrecedenceOverObsState:DataChannelTest.RemoteControlTakesPrecedenceOverObsState:DataChannelTest.RemoteStatsTakesPrecedenceOverObsState:DataChannelTest.ParsesOfficialRequestStatsMessage:DataChannelTest.ParsesTruthyOfficialContinuousStatsRequest:DataChannelTest.ParsesFalseOfficialContinuousStatsRequest:DataChannelTest.ParsesRemoteControlObsCommandMessage:DataChannelTest.RejectsObsCommandWithoutRemoteField:DataChannelTest.RejectsLegacyActionWithoutRemoteField --gtest_brief=1`
  passed 11/11. The stats-request and remote-control precedence tests failed
  before the parser moved those active controls ahead of passive state buckets;
  the recovery and hangup precedence tests failed before the same ordering fix
  for those controls. `DataChannelTest.RemoteStatsTakesPrecedenceOverObsState`
  failed before stats response telemetry moved ahead of passive state buckets.
- Current focused stats response/request regression:
  `build\Debug\vdoninja-tests.exe --gtest_filter=DataChannelTest.RemoteStatsTakesPrecedenceOverObsState:DataChannelTest.ParsesOfficialRemoteStatsMessage:DataChannelTest.ParsesStatsMessage:DataChannelTest.StatsRequestTakesPrecedenceOverObsState:DataChannelTest.ParsesOfficialRequestStatsMessage:DataChannelTest.ParsesTruthyOfficialContinuousStatsRequest:DataChannelTest.ParsesFalseOfficialContinuousStatsRequest --gtest_brief=1`
  passed 7/7.
- Current focused keyframe/stats fanout regression:
  `build\Debug\vdoninja-tests.exe --gtest_filter=DataChannelCallbackTest.TriggersOnKeyframeRequestWhenCombinedWithStatsRequest:DataChannelCallbackTest.TriggersOnKeyframeRequest:DataChannelTest.StatsRequestTakesPrecedenceOverObsState:DataChannelTest.ParsesOfficialRequestStatsMessage:DataChannelTest.ParsesTruthyOfficialContinuousStatsRequest:DataChannelTest.ParsesFalseOfficialContinuousStatsRequest --gtest_brief=1`
  passed 6/6. `DataChannelCallbackTest.TriggersOnKeyframeRequestWhenCombinedWithStatsRequest`
  failed before the keyframe request moved to field-level fanout because the
  primary parser type was `StatsRequest`.
- Current focused unsupported-control classification regression:
  `build\Debug\vdoninja-tests.exe --gtest_filter=DataChannelTest.ParsesOfficialSettingsReadbackAsUnsupportedControl:DataChannelTest.ParsesOfficialVideoHackAsUnsupportedControl:DataChannelTest.ParsesOfficialBrowserOnlyControlAsUnsupportedControl:DataChannelTest.ParsesOfficialPtzControlAsUnsupportedControl:DataChannelTest.ParsesOfficialKeyframeRateAsUnsupportedControl:DataChannelTest.ParsesOfficialMicIsolateAsUnsupportedControl:DataChannelTest.ParsesOfficialLowerVolumeAsUnsupportedControl --gtest_brief=1`
  passed 7/7.
- Current focused terminal keyframe fanout regression:
  `build\Debug\vdoninja-tests.exe --gtest_filter=DataChannelTest.KeyframeRequestFanoutIgnoresTerminalTransportMessages:DataChannelCallbackTest.DoesNotTriggerKeyframeRequestWhenCombinedWithPeerBye:DataChannelCallbackTest.DoesNotTriggerKeyframeRequestWhenCombinedWithPing:DataChannelCallbackTest.DoesNotTriggerKeyframeRequestWhenCombinedWithDirectSignaling:DataChannelCallbackTest.TriggersOnKeyframeRequestWhenCombinedWithStatsRequest --gtest_brief=1`
  passed 5/5.
- Current focused recovery active-field ordering regression:
  `build\Debug\vdoninja-tests.exe --gtest_filter=DataChannelTest.StatsRequestTakesPrecedenceOverRecoveryControl:DataChannelTest.KeyframeRequestTakesPrecedenceOverRecoveryControl:DataChannelTest.StatsRequestTakesPrecedenceOverUnsupportedControl:DataChannelTest.KeyframeRequestTakesPrecedenceOverUnsupportedControl:DataChannelTest.ExtractsUnsupportedControlNameFromCombinedActivePayload --gtest_brief=1`
  passed 5/5. The recovery precedence cases failed before recovery moved behind
  stats/keyframe, matching official VDO.Ninja's field order.
- Current focused recovery presence/order regression:
  `build\Debug\vdoninja-tests.exe --gtest_filter=DataChannelTest.SelectsOfficialRecoveryRejectionOrder:DataChannelTest.ExtractsOfficialRecoveryControl:DataChannelTest.RecoveryControlsUseOfficialPresenceSemantics:DataChannelTest.ParsesOfficialRefreshVideoControlMessage:DataChannelTest.ParsesOfficialRefreshMicrophoneControlMessage:DataChannelTest.ParsesOfficialRefreshConnectionControlMessage:DataChannelTest.ParsesOfficialRefreshAllControlMessage:DataChannelTest.ParsesOfficialRestartWhipControlMessage:DataChannelTest.StatsRequestTakesPrecedenceOverRecoveryControl:DataChannelTest.KeyframeRequestTakesPrecedenceOverRecoveryControl --gtest_brief=1`
  passed 10/10. `DataChannelTest.RecoveryControlsUseOfficialPresenceSemantics`
  failed before `parseRecoveryControl` changed from boolean-value parsing to
  official presence semantics for `refreshVideo`, `refreshMicrophone`,
  `refreshConnection`, `refreshAll`, and `restartWhip`.
  `DataChannelTest.SelectsOfficialRecoveryRejectionOrder` failed while the
  disabled-output selector preferred `refreshAll` before earlier official
  rejection fields.
- Current focused receiver video-suppression regression:
  `build\Debug\vdoninja-tests.exe --gtest_filter=DataChannelTest.ReceiverDirectorVideoSuppressionUsesOfficialTargetScope:DataChannelTest.ExtractsReceiverVideoSuppressionState:DataChannelTest.ExtractsOfficialDirectorVideoState:DataChannelTest.ExtractsOfficialSelfTargetedDirectorVideoState:DataChannelTest.ExtractsOfficialInitialDirectorVideoMutedState:DataChannelTest.ExtractsOfficialMixedInitialDirectorVideoMutedState:DataChannelTest.ParsesOfficialDirectVideoMutedMessage:DataChannelTest.ParsesOfficialVirtualHangupStateMessage --gtest_brief=1`
  passed 8/8. `DataChannelTest.ExtractsReceiverVideoSuppressionState` failed
  before receiver-side extraction included `directVideoMuted`,
  `info.directorVideoMuted`, and `virtualHangup`.
  `DataChannelTest.ReceiverDirectorVideoSuppressionUsesOfficialTargetScope`
  failed before string-targeted `directVideoMuted` updates were scoped to the
  local target peer.
- Current focused fuzz/parser hardening regression:
  `build\Debug\vdoninja-tests.exe --gtest_filter=JsonParserTest.ParsesArrayWithScalarValues:DataChannelEdgeCaseTest.FuzzesOfficialMessageParserAndExtractors:SignalingProtocolTest.FuzzesSignalingNormalizationWithoutCrashing --gtest_brief=1 --gtest_repeat=50 --gtest_shuffle`
  passed 3/3 for 50 shuffled repeats. The signaling fuzz test timed out
  before `JsonParser::getArray` advanced over scalar array entries.
- Current broader parser/signaling/data-channel repeat:
  `build\Debug\vdoninja-tests.exe --gtest_filter=DataChannel*:*Signaling*:JsonParser* --gtest_brief=1 --gtest_repeat=7 --gtest_shuffle`
  passed 227 tests per repeat for 7 shuffled repeats.
- Current focused unsupported active-field ordering regression:
  `build\Debug\vdoninja-tests.exe --gtest_filter=DataChannelTest.StatsRequestTakesPrecedenceOverUnsupportedControl:DataChannelTest.KeyframeRequestTakesPrecedenceOverUnsupportedControl:DataChannelTest.ExtractsUnsupportedControlNameFromCombinedActivePayload:DataChannelTest.UnsupportedControlTakesPrecedenceOverObsState:DataChannelTest.ParsesOfficialVideoHackAsUnsupportedControl --gtest_brief=1`
  passed 5/5. The stats and keyframe precedence cases failed before
  unsupported-control classification moved behind those active fields; the
  extraction test guards the publisher output path that still replies with
  `{"rejected":"<field>"}` after earlier side effects.
- Current focused ICE-restart presence regression:
  `build\Debug\vdoninja-tests.exe --gtest_filter=DataChannelTest.ParsesOfficialIceRestartRequestByPresence:DataChannelTest.IceRestartRequestPreventsIndependentKeyframeFanoutByPresence --gtest_brief=1`
  failed before data-channel `iceRestartRequest` changed from truthy-only to
  presence-based terminal handling; after the fix, the broader focused
  regression with the existing true-value and terminal keyframe checks passed
  5/5.
- Current focused hangup presence regression:
  `build\Debug\vdoninja-tests.exe --gtest_filter=DataChannelTest.ParsesOfficialHangupRequestByPresence:DataChannelTest.HangupPreventsIndependentKeyframeFanoutByPresence --gtest_brief=1`
  failed before data-channel `hangup` changed from truthy-only to
  presence-based terminal handling; after the fix, the broader focused hangup
  regression with the existing true-value, mute-precedence, and terminal
  keyframe checks passed 5/5.
- Current full direct unit run:
  `build\Debug\vdoninja-tests.exe --gtest_brief=1` passed 428/428.
- Current CTest run: `ctest --test-dir build --output-on-failure`
  passed 776/776.
- Current structural/build checks: `git diff --check` passed; `cmake --build
  build-obs32 --config Release --target obs-vdoninja` passed and produced
  `build-obs32\Release\obs-vdoninja.dll`; `cmake --install build-obs32
  --config Release --prefix install-obs32` passed.
- Current Playwright e2e checks: `npm test` passed 5/5, including multi-viewer,
  publish/view/reload, special-character password, standard view, and VP9 native
  receiver cases. `npm run test:e2e:firefox-view` passed with active audio/video
  tracks, metadata, inbound bytes, and advancing playback.
- Current portable OBS functional smokes:
  `scripts\run-vdoninja-publish-smoke.ps1` passed against the installed
  OBS 32 build; OBS loaded the repo DLL from `_obs-portable`, the viewer reached
  `inboundBytes: 6411121`, and playback reached `currentTime: 8.511`.
  After the source data-channel malformed-message guard was added,
  `scripts\run-vdoninja-source-smoke.ps1 -Mode native` passed with OBS 32.1.0,
  obs-websocket 5.7.1, `inputKindRegistered: true`,
  `useNativeReceiver: true`, and a non-empty source screenshot
  (`byteLength: 13385`) after the native remote-audio unmute path was made
  explicit.
- `clang-format` / `clang-format-14` were not found in this shell.
- Built `vdoninja-tests` from `build` Debug for the current pass. Earlier
  focused regressions below remain recorded from the prior `build-codex-tests`
  validation pass unless their command uses `build\Debug\vdoninja-tests.exe`.
- Ran focused current remote-auth/director-only regression
  `build\Debug\vdoninja-tests.exe --gtest_filter=DataChannelTest.RejectsObsCommandWithoutRemoteField:DataChannelTest.RejectsLegacyActionWithoutRemoteField:DataChannelCallbackTest.DoesNotTriggerRemoteControlFromObsCommandWithoutRemote:DataChannelTest.ParsesOfficialRemoteVideoMutedRequestMessage:DataChannelTest.UnsupportedDirectorControlTakesPrecedenceOverPassiveInfo:DataChannelTest.ParsesOfficialSpeakerMuteMessage:DataChannelTest.ParsesOfficialDisplayMuteMessage:DataChannelTest.ParsesOfficialHangupRequest --gtest_brief=1`:
  8/8 tests passed.
- Ran focused unsupported-control precedence regression
  `build\Debug\vdoninja-tests.exe --gtest_filter=DataChannelTest.UnsupportedDirectorControlTakesPrecedenceOverPassiveInfo:DataChannelTest.UnsupportedDirectorControlTakesPrecedenceOverMuteState:DataChannelTest.UnsupportedControlTakesPrecedenceOverObsState --gtest_brief=1`:
  3/3 tests passed. The mute-state and OBS-state cases failed before the
  unsupported-control check moved ahead of passive state buckets.
- Ran focused current peer-cleanup/parser regression
  `build\Debug\vdoninja-tests.exe --gtest_filter=DataChannelTest.ParsesOfficialCloseMessageAsPeerCleanupBeforeMute:DataChannelTest.ParsesOfficialByeAsPeerCleanup:DataChannelTest.ParsesOfficialByePresenceAsPeerCleanup:DataChannelTest.ParsesOfficialCleanupRequestAsPeerCleanup:DataChannelTest.IgnoresUnofficialByeRequestAsPeerCleanup:DataChannelTest.ParsesOfficialPingMessage:DataChannelTest.ParsesOfficialPongMessage:DataChannelTest.ParsesOfficialIceRestartRequest --gtest_brief=1`:
  8/8 tests passed. `DataChannelTest.ParsesOfficialCloseMessageAsPeerCleanupBeforeMute`
  failed before the parser classified data-channel cleanup ahead of mute state.
- Ran official cleanup spelling comparison against `C:\Users\steve\Code\vdoninja`
  without modifying it: `webrtc.js` has WebSocket
  `msg.bye || msg.request == "cleanup"` and data-channel `"bye" in msg`
  handling; `lib.js` close sends `msg.videoMuted = true` and `msg.bye = true`;
  no official `request:"bye"` cleanup spelling was found.
- Ran focused current media-control fanout regression
  `build\Debug\vdoninja-tests.exe --gtest_filter=DataChannelTest.ParsesOfficialVideoBitrateControlMessage:DataChannelTest.ParsesOfficialAudioBitrateControlMessage:DataChannelTest.ParsesOfficialResolutionControlMessage:DataChannelTest.ExtractsOfficialBitrateMediaControl:DataChannelTest.ExtractsOfficialNegativeAudioBitrateAsEnable:DataChannelTest.ExtractsMediaControlFromCombinedTopLevelMessage:DataChannelTest.MediaControlIgnoresNestedStatsBitrate`:
  7/7 tests passed.
- Ran current publisher media-control structural check against
  `VDONinjaOutput` data-channel dispatch: raw `"bitrate"` /
  `"audioBitrate"` `message.find` scans are absent and the structured
  `parseMediaControl(message)` fanout is present.
- Ran current source data-channel dispatch structural check: the source callback
  no longer scans raw message text for `"request"` or `"bye"`; it handles
  `DataMessageType::Signaling` and `DataMessageType::PeerBye` explicitly.
- Ran focused current stats-request parser regression
  `build\Debug\vdoninja-tests.exe --gtest_filter=DataChannelTest.ParsesOfficialRequestStatsMessage:DataChannelTest.ParsesTruthyOfficialContinuousStatsRequest:DataChannelTest.ParsesFalseOfficialContinuousStatsRequest --gtest_brief=1`:
  3/3 tests passed.
- Ran current stats-request authorization structural check against
  `VDONinjaOutput` data-channel dispatch: stats request handling contains
  `settingsSnap.enableRemote`, sends `{remoteStats:{}}` on the disabled path,
  and does not call `addRemoteStatsSubscriber` on that disabled path. This
  check failed before the stats authorization fix because the block had no
  `enableRemote` gate.
- Ran focused recovery-control regression
  `build\Debug\vdoninja-tests.exe --gtest_filter=DataChannelTest.ParsesOfficialRefreshVideoControlMessage:DataChannelTest.ParsesOfficialRefreshMicrophoneControlMessage:DataChannelTest.ParsesOfficialRefreshConnectionControlMessage:DataChannelTest.ParsesOfficialRefreshAllControlMessage:DataChannelTest.ParsesOfficialRestartWhipControlMessage:DataChannelTest.ExtractsOfficialRecoveryControl`:
  6/6 tests passed.
- Ran focused mesh-control regression
  `build\Debug\vdoninja-tests.exe --gtest_filter=DataChannelTest.ParsesOfficialReconnectPeerMeshControlMessage:DataChannelTest.ParsesOfficialGetConnectionMapMeshControlMessage:DataChannelTest.ParsesOfficialConnectionMapResponseMessage:DataChannelTest.ExtractsOfficialMeshControl`:
  4/4 tests passed.
- Ran focused peer snapshot media-state regression
  `build\Debug\vdoninja-tests.exe --gtest_filter=PeerManagerSnapshotTest.ExposesPerPeerMediaSendState`:
  1/1 test passed.
- Ran peer cleanup lock-boundary structural check against
  `VDONinjaPeerManager::releasePeerResources`: `cleanupRetired.store(true)` is
  present; PeerConnection callbacks are cleared after handle snapshotting and
  before the peer-owned handle reset lock; `peer->pc.reset()` remains after
  callback clearing.
- Ran full Debug CTest suite
  `ctest --test-dir build -C Debug --output-on-failure`: 621/621 discovered
  CTest tests passed.
- Ran full direct GoogleTest executable
  `build\Debug\vdoninja-tests.exe --gtest_brief=1`: 389/389 tests passed.
- Built OBS plugin target
  `cmake --build build-obs32 --config Release --target obs-vdoninja`: passed
  with existing OBS SDK C4201 nameless-struct/union warnings and the existing
  unreferenced-helper C4505 warning.
- Ran `git diff --check`: clean.
- Formatter note: `clang-format` and `clang-format-14` were not available in
  this shell.
- Ran focused direct data-channel signaling regression
  `ctest --test-dir build-codex-tests -C Debug -R "DataChannelTest.ParsesOfficialDataChannelDescriptionAsSignaling|DataChannelTest.GivesOfficialDataChannelSignalingPrecedenceOverAppFields|DataChannelTest.ParsesOfficialDataChannelCandidateBundleAsSignaling|DataChannelTest.PreparesOfficialDataChannelSignalingWithSenderUuid|DataChannelTest.PreparesOfficialDataChannelCandidateWithSenderUuid|DataChannelTest.PreparesOfficialDataChannelCandidateBundleWithSenderUuid|DataChannelTest.KeepsExistingUuidWhenPreparingOfficialDataChannelSignaling" --output-on-failure`:
  7/7 tests passed.
- Ran focused ping/pong liveness regression
  `ctest --test-dir build-codex-tests -C Debug -R "DataChannelTest.ParsesOfficialPingMessage|DataChannelTest.ParsesOfficialPongMessage|DataChannelTest.CreatesOfficialPongMessageWithoutStringifyingToken|DataChannelTest.PreservesQuotedOfficialPingTokenForPong" --output-on-failure`:
  8/8 tests passed.
- Ran focused ICE-restart recovery regression
  `ctest --test-dir build-codex-tests -C Debug -R "DataChannelTest.ParsesOfficialIceRestartRequest|SignalingProtocolTest.ParsesOfficialIceRestartRequest|SignalingStateTest.OfficialIceRestartRequestDispatchesIceRestartRequest" --output-on-failure`:
  4/4 tests passed.
- Ran focused regression
  `ctest --test-dir build-codex-tests -C Debug -R "DataChannelTest.ParsesOfficialHangupRequest" --output-on-failure`:
  1/1 test passed.
- Ran focused peer-cleanup regression
  `ctest --test-dir build-codex-tests -C Debug -R "DataChannelTest.ParsesOfficialByeAsPeerCleanup" --output-on-failure`:
  2/2 tests passed.
- Ran focused stats-request mode regression
  `ctest --test-dir build-codex-tests -C Debug -R "DataChannelTest.ParsesOfficialRequestStatsMessage|DataChannelTest.ParsesTruthyOfficialContinuousStatsRequest|DataChannelTest.ParsesFalseOfficialContinuousStatsRequest" --output-on-failure`:
  3/3 tests passed.
- Ran focused official mute-state regression
  `ctest --test-dir build-codex-tests -C Debug -R "DataChannelTest.ExtractsOfficialMuteStateAsAudioOnly|DataChannelTest.ExtractsOfficialVideoMutedAsVideoOnly|DataChannelTest.ExtractsPluginAudioVideoMuteState|DataChannelTest.ParsesOfficialMuteStateMessage|DataChannelCallbackTest.TriggersOnOfficialMuteStateChange|DataChannelTest.ParsesOfficialInitialInfoMuteStateMessage|DataChannelTest.ExtractsOfficialInitialInfoMuteState" --output-on-failure`:
  7/7 tests passed; the top-level `muteState` tests and nested initial `info`
  mute-state tests failed before parser support and passed after the fixes.
- Ran focused official OBS/source-state regression
  `build\Debug\vdoninja-tests.exe --gtest_filter=DataChannelTest.ParsesOfficialObsStateMessage:DataChannelTest.ParsesOfficialSceneDisplayStateMessage:DataChannelTest.ParsesOfficialSceneMuteStateMessage`:
  3/3 tests passed; these tests failed before the `ObsState` parser type was
  added.
- Ran focused official media-control regression
  `build\Debug\vdoninja-tests.exe --gtest_filter=DataChannelTest.ParsesOfficialVideoBitrateControlMessage:DataChannelTest.ParsesOfficialAudioBitrateControlMessage:DataChannelTest.ParsesOfficialResolutionControlMessage:DataChannelTest.ExtractsOfficialBitrateMediaControl:DataChannelTest.ExtractsOfficialNegativeAudioBitrateAsEnable`:
  5/5 tests passed; these tests failed before the `MediaControl` parser
  type/helper was added.
- Ran focused official screen-share-state regression
  `build\Debug\vdoninja-tests.exe --gtest_filter=DataChannelTest.ParsesOfficialScreenShareStateMessage:DataChannelTest.ParsesOfficialScreenStoppedMessage:DataChannelTest.ParsesOfficialInitialInfoScreenShareStateMessage:DataChannelTest.ExtractsOfficialScreenShareState:DataChannelTest.ExtractsOfficialInitialInfoScreenShareState:DataChannelTest.ExtractsOfficialMixedInitialInfoScreenShareState`:
  6/6 tests passed; these tests failed before the `ScreenShareState` parser
  type/helper was added.
- Ran focused official director-video-state regression
  `build\Debug\vdoninja-tests.exe --gtest_filter=DataChannelTest.ParsesOfficialInitialDirectorVideoMutedMessage:DataChannelTest.ParsesOfficialDirectVideoMutedMessage:DataChannelTest.ParsesOfficialVirtualHangupStateMessage:DataChannelTest.ParsesOfficialRemoteVideoMutedRequestMessage:DataChannelTest.ExtractsOfficialDirectorVideoState:DataChannelTest.ExtractsOfficialSelfTargetedDirectorVideoState:DataChannelTest.ExtractsOfficialInitialDirectorVideoMutedState:DataChannelTest.ExtractsOfficialMixedInitialDirectorVideoMutedState:DataChannelTest.ExtractsOfficialRemoteVideoMutedRequest`:
  9/9 tests passed; these tests failed before the `DirectorVideoState` parser
  type/helper was added and before `remoteVideoMuted` field extraction existed.
- Ran focused official director speaker/display mute-state regression
  `build\Debug\vdoninja-tests.exe --gtest_filter=DataChannelTest.ParsesOfficialInitialDirectorAudioDisplayMutedMessage:DataChannelTest.ParsesOfficialSpeakerMuteMessage:DataChannelTest.ParsesOfficialDisplayMuteMessage:DataChannelTest.ExtractsOfficialDirectorAudioState:DataChannelTest.ExtractsOfficialInitialDirectorAudioState:DataChannelTest.ExtractsOfficialMixedInitialDirectorAudioState`:
  6/6 tests passed; these tests failed before the `DirectorAudioState` parser
  type/helper was added.
- Ran focused official director transform-state regression
  `build\Debug\vdoninja-tests.exe --gtest_filter=DataChannelTest.ParsesOfficialMirrorGuestStateMessage:DataChannelTest.ParsesOfficialRemoteRotateCommandMessage:DataChannelTest.ParsesOfficialRotateVideoStateMessage:DataChannelTest.ParsesOfficialInitialDirectorTransformStateMessage:DataChannelTest.ExtractsOfficialRemoteRotateToggleCommand:DataChannelTest.ExtractsOfficialDirectorTransformState --gtest_brief=1`:
  6/6 tests passed. The first two tests failed before the parser classified
  live transform commands as unsupported.

## Workflow Hazards To Recheck Each Review

Hazard: combined `info` payloads contain multiple state families

- Official VDO.Ninja can send one `info` object that contains mute,
  screen-share, director video, director speaker/display, and director
  mirror/flip fields together.
- Current parser helpers can extract each family when called directly, but
  `parseMessage` still returns one primary `DataMessageType`. Higher-priority
  fields can therefore claim the classification before later state families are
  surfaced.
- Today this is acceptable only because most non-mute director/source state
  buckets are parser alignment/no-op buckets. If a runtime owner starts applying
  screen-share, director video, audio/display, or mirror/flip side effects, add
  an explicit fanout path for combined `info` rather than depending on the
  single primary type.

Hazard: duplicate external events

- Check every request, answer, candidate, cleanup, data-channel `bye`, stream
  added/removed event, and OBS lifecycle callback for duplicate delivery.
- Every duplicate should have a gate: session match, owner match, latch, terminal
  state, or idempotent no-op.

Hazard: stale session

- Check every use of peer `UUID` without session.
- Check every path that accepts mismatched session while negotiating.
- Check pending viewer signaling data-channel bindings by `UUID + session`.

Hazard: callback lifetime

- Check every lambda that captures `this`.
- It should use `AsyncCallbackGuard`, weak peer, weak track, or be explicitly
  bound to an object with a longer lifetime.
- Teardown should clear callbacks before releasing objects when possible.

Hazard: RTC callback re-entrancy

- State-change callbacks may fire during close or destruction.
- Do not destroy PeerConnection, Track, or DataChannel objects from their own
  callbacks.
- Retire first, release later from non-RTC paths.

Hazard: OBS thread affinity

- OBS frontend/UI operations should run on OBS UI task queue.
- OBS encoder callbacks should not perform network send or blocking work.
- Source child active/showing refcount changes must be balanced.

Hazard: unbounded queues

- Media send queue is bounded.
- Decode timestamp queues are bounded.
- Pending viewer signaling channels are bounded.
- Recheck signaling send queue growth and stale queue semantics.

Hazard: media readiness

- Publisher video should gate delta frames until keyframe sync.
- Native receiver should request keyframes on decode stalls/failures.
- Native source should clear stale video output when packets stop.
- Audio should only publish the selected OBS track.

## Suggested Review Checklist

For any future workflow change, answer these before trusting it:

1. What actor owns the state?
2. Which thread or callback can mutate the state?
3. Can the event arrive twice?
4. Can the event arrive after stop/destroy?
5. Can the event refer to an old session?
6. What happens if signaling disconnects between queue and send?
7. What happens if peer state changes while media is being sent?
8. Is there a bounded queue or explicit drop policy?
9. Is media gated until the first keyframe/valid decoder state?
10. Are OBS child source refs, active refs, and showing refs balanced?
11. Are callbacks cleared before objects are released?
12. Does the user-facing state mean signaling connected, peer connected, or
    media flowing?

## Terms To Keep Stable

Use these names consistently in future docs and reviews:

- `publisher output`: OBS sending media to VDO.Ninja viewers.
- `browser-backed source`: OBS source wrapper using private browser source.
- `native receiver`: internal source using native WebRTC receive and FFmpeg
  decode.
- `publisher peer`: `PeerInfo` used to send media to a viewer.
- `viewer peer`: `PeerInfo` used to receive media from a publisher.
- `transport data channel`: data channel reused to carry targeted signaling.
- `media peer`: the peer that owns actual audio/video tracks.
- `retired peer`: peer removed from active map, waiting for deferred resource
  release.
- `terminal peer`: peer in disconnected, failed, or closed state.
