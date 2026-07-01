const { chromium } = require("playwright");

function ensureQuery(url, key, value) {
  const u = new URL(url);
  if (!u.searchParams.has(key)) {
    u.searchParams.set(key, value);
  }
  return u.toString();
}

async function collectMediaSnapshot(page) {
  return page.evaluate(async () => {
    const videos = Array.from(document.querySelectorAll("video")).map((v, index) => {
      const stream = v.srcObject;
      const audioTracks = stream && stream.getAudioTracks ? stream.getAudioTracks().length : 0;
      const videoTracks = stream && stream.getVideoTracks ? stream.getVideoTracks().length : 0;
      return {
        index,
        readyState: v.readyState,
        paused: v.paused,
        currentTime: v.currentTime,
        videoWidth: v.videoWidth,
        videoHeight: v.videoHeight,
        audioTracks,
        videoTracks,
      };
    });

    const peerConnections = [];
    const seenPeerConnections = new Set();
    function addPeerConnection(pc, source, id) {
      if (!pc || typeof pc.getStats !== "function" || seenPeerConnections.has(pc)) {
        return;
      }
      seenPeerConnections.add(pc);
      peerConnections.push({ pc, source, id });
    }

    if (Array.isArray(window.__pcList)) {
      window.__pcList.forEach((pc, index) => addPeerConnection(pc, "constructor-patch", String(index)));
    }
    if (window.session && window.session.pcs && typeof window.session.pcs === "object") {
      Object.entries(window.session.pcs).forEach(([id, pc]) => addPeerConnection(pc, "session.pcs", id));
    }

    const pcStats = [];
    for (const entry of peerConnections) {
      const pc = entry.pc;
        try {
          const stats = await pc.getStats();
          let inboundVideoBytes = 0;
          let inboundAudioBytes = 0;
          let outboundVideoBytes = 0;
          let outboundAudioBytes = 0;
          stats.forEach((s) => {
            if (s.type === "inbound-rtp" && !s.isRemote) {
              if (s.kind === "video") inboundVideoBytes += s.bytesReceived || 0;
              if (s.kind === "audio") inboundAudioBytes += s.bytesReceived || 0;
            }
            if (s.type === "outbound-rtp" && !s.isRemote) {
              if (s.kind === "video") outboundVideoBytes += s.bytesSent || 0;
              if (s.kind === "audio") outboundAudioBytes += s.bytesSent || 0;
            }
          });
          pcStats.push({
            source: entry.source,
            id: entry.id,
            state: pc.connectionState,
            inboundVideoBytes,
            inboundAudioBytes,
            outboundVideoBytes,
            outboundAudioBytes,
          });
        } catch (error) {
          pcStats.push({ source: entry.source, id: entry.id, error: String(error) });
        }
    }

    return {
      url: location.href,
      title: document.title || "",
      textSample: (document.body ? document.body.innerText || "" : "").slice(0, 300),
      videoElements: videos,
      pcStats,
      timestamp: Date.now(),
    };
  });
}

function parseBoolean(value, fallback = false) {
  if (value === undefined || value === null || value === "") {
    return fallback;
  }
  const normalized = String(value).trim().toLowerCase();
  return normalized === "1" || normalized === "true" || normalized === "yes" || normalized === "on";
}

function parseNonNegativeInteger(value, fallback) {
  const parsed = Number(value);
  if (!Number.isFinite(parsed)) {
    return fallback;
  }
  return Math.max(0, Math.trunc(parsed));
}

function sleep(ms) {
  return new Promise((resolve) => setTimeout(resolve, Math.max(0, ms)));
}

function appendQueryFragment(url, fragment) {
  const trimmed = String(fragment || "").trim();
  if (!trimmed) {
    return url;
  }

  const u = new URL(url);
  const query = trimmed.replace(/^[?#&]+/, "");
  for (const part of query.split("&")) {
    const item = part.trim();
    if (!item) {
      continue;
    }
    const separator = item.indexOf("=");
    if (separator === -1) {
      u.searchParams.set(item, "");
    } else {
      u.searchParams.set(item.slice(0, separator), item.slice(separator + 1));
    }
  }
  return u.toString();
}

function parsePublisherVariants(value) {
  return String(value || "")
    .split(";")
    .map((entry) => entry.trim())
    .filter(Boolean);
}

function normalizeChoice(value, allowed, fallback) {
  const normalized = String(value || "").trim().toLowerCase();
  return allowed.includes(normalized) ? normalized : fallback;
}

function buildPublisherUrl(baseUrl, index, variants) {
  const variant = variants.length ? variants[index % variants.length] : "";
  return ensureQuery(ensureQuery(appendQueryFragment(baseUrl, variant), "autostart", "1"), "webcam", "1");
}

async function nudgePublisherPage(page) {
  const cameraButtonByRole = page.getByRole("button", { name: /share your camera/i });
  const cameraButtonByText = page.getByText(/share your camera/i).first();
  const canClickRole = await cameraButtonByRole.isVisible().catch(() => false);
  const canClickText = await cameraButtonByText.isVisible().catch(() => false);
  if (canClickRole || canClickText) {
    const target = canClickRole ? cameraButtonByRole : cameraButtonByText;
    await target.click({ timeout: 3000 }).catch(() => {});
    await page.waitForTimeout(5000);
  }

  const vp = page.viewportSize() || { width: 1280, height: 720 };
  await page.mouse.click(Math.floor(vp.width / 2), Math.floor(vp.height / 2));
  await page.waitForTimeout(4000);
}

async function preparePublisherPage(page, url, startupWaitMs) {
  await page.goto(url, { waitUntil: "domcontentloaded", timeout: 60000 });
  await page.waitForTimeout(startupWaitMs);
  await nudgePublisherPage(page);
}

async function startPublisherChurn(page, config) {
  if (!config || config.profile === "off") {
    return { started: false, profile: "off" };
  }

  return page.evaluate((churnConfig) => {
    if (window.__publisherChurn && window.__publisherChurn.timer) {
      clearInterval(window.__publisherChurn.timer);
    }

    const state = {
      profile: churnConfig.profile,
      intervalMs: churnConfig.intervalMs,
      maxIterations: churnConfig.iterations,
      extraAudio: !!churnConfig.extraAudio,
      dataFuzzProfile: churnConfig.dataFuzzProfile || "off",
      startedAt: Date.now(),
      stoppedAt: null,
      iteration: 0,
      emptyTicks: 0,
      replacements: 0,
      enabledToggles: 0,
      nullReplacements: 0,
      extraAudioTracks: 0,
      dataMessages: 0,
      malformedDataMessages: 0,
      binaryDataMessages: 0,
      errors: [],
      events: [],
      timer: null,
    };
    window.__publisherChurn = state;

    function remember(kind, fields = {}) {
      state.events.push({ at: Date.now(), kind, ...fields });
      if (state.events.length > 120) {
        state.events.shift();
      }
    }

    function rememberError(error, context) {
      state.errors.push({ at: Date.now(), context, message: String(error && error.message ? error.message : error) });
      if (state.errors.length > 80) {
        state.errors.shift();
      }
    }

    function allPeerConnections() {
      const pcs = [];
      const seen = new Set();
      const add = (pc) => {
        if (!pc || seen.has(pc)) {
          return;
        }
        seen.add(pc);
        pcs.push(pc);
      };
      if (Array.isArray(window.__pcList)) {
        window.__pcList.forEach(add);
      }
      if (window.session && window.session.pcs && typeof window.session.pcs === "object") {
        Object.values(window.session.pcs).forEach(add);
      }
      return pcs;
    }

    function livePeerConnections() {
      return allPeerConnections().filter((pc) => {
        return pc && pc.signalingState !== "closed" && pc.connectionState !== "closed";
      });
    }

    function makeVideoTrack(iteration) {
      const sizes = [
        [320, 180, 15],
        [640, 360, 30],
        [1280, 720, 30],
        [1920, 1080, 30],
        [3840, 2160, 15],
        [320, 720, 24],
      ];
      const [width, height, fps] = sizes[iteration % sizes.length];
      const canvas = document.createElement("canvas");
      canvas.width = width;
      canvas.height = height;
      const context = canvas.getContext("2d", { alpha: false });
      let frame = 0;
      const draw = () => {
        const hue = (iteration * 37 + frame * 11) % 360;
        context.fillStyle = `hsl(${hue}, 80%, 42%)`;
        context.fillRect(0, 0, width, height);
        context.fillStyle = `hsl(${(hue + 160) % 360}, 90%, 65%)`;
        context.fillRect((frame * 17) % Math.max(1, width - 40), (frame * 9) % Math.max(1, height - 40), 40, 40);
        context.fillStyle = "white";
        context.font = `${Math.max(18, Math.floor(height / 14))}px sans-serif`;
        context.fillText(`codex churn ${iteration}`, 16, Math.max(32, Math.floor(height / 8)));
        frame += 1;
      };
      draw();
      const timer = setInterval(draw, Math.max(16, Math.floor(1000 / fps)));
      const stream = canvas.captureStream(fps);
      const track = stream.getVideoTracks()[0];
      track.__codexChurnTrack = true;
      track.__codexChurnTimer = timer;
      track.__codexChurnStream = stream;
      track.__codexChurnStop = () => clearInterval(timer);
      return track;
    }

    function makeAudioTrack(iteration, channels) {
      const AudioContextClass = window.AudioContext || window.webkitAudioContext;
      if (!AudioContextClass) {
        throw new Error("AudioContext is unavailable");
      }
      const channelCount = Math.max(1, Math.min(6, Math.trunc(channels) || 1));
      const audioContext = new AudioContextClass({ sampleRate: 48000 });
      const destination = audioContext.createMediaStreamDestination();
      const gain = audioContext.createGain();
      gain.gain.value = 0.04;

      const oscillators = [];
      const oscillatorTypes = ["sine", "triangle", "square", "sawtooth"];
      for (let channelIndex = 0; channelIndex < channelCount; channelIndex += 1) {
        const oscillator = audioContext.createOscillator();
        oscillator.type = oscillatorTypes[channelIndex % oscillatorTypes.length];
        oscillator.frequency.value = 220 + ((iteration + channelIndex) % 8) * 55;
        oscillators.push(oscillator);
      }

      if (channelCount >= 2) {
        const merger = audioContext.createChannelMerger(channelCount);
        oscillators.forEach((oscillator, channelIndex) => {
          oscillator.connect(merger, 0, channelIndex);
        });
        merger.connect(gain).connect(destination);
      } else {
        oscillators[0].connect(gain).connect(destination);
      }

      oscillators.forEach((oscillator) => oscillator.start());
      audioContext.resume().catch(() => {});
      const track = destination.stream.getAudioTracks()[0];
      track.__codexChurnTrack = true;
      track.__codexChurnAudioContext = audioContext;
      track.__codexChurnOscillators = oscillators;
      track.__codexChurnStop = () => {
        for (const oscillator of track.__codexChurnOscillators || []) {
          try {
            oscillator.stop();
          } catch (_) {}
        }
        audioContext.close().catch(() => {});
      };
      return track;
    }

    function stopChurnTrack(track) {
      if (!track || !track.__codexChurnTrack) {
        return;
      }
      try {
        if (typeof track.__codexChurnStop === "function") {
          track.__codexChurnStop();
        }
      } catch (error) {
        rememberError(error, "stop-churn-track");
      }
      try {
        track.stop();
      } catch (_) {}
    }

    async function replaceSenderTrack(sender, nextTrack, context) {
      const previousTrack = sender.track;
      await sender.replaceTrack(nextTrack);
      state.replacements += 1;
      remember("replace-track", {
        context,
        trackKind: nextTrack ? nextTrack.kind : null,
        previousKind: previousTrack ? previousTrack.kind : null,
      });
      if (previousTrack !== nextTrack) {
        stopChurnTrack(previousTrack);
      }
    }

    function officialStateMessages(iteration) {
      const resolutionCases = [
        { w: 320, h: 180, s: false, c: false },
        { w: 640, h: 360, s: 100, c: true },
        { w: 1280, h: 720, s: false, c: null },
        { w: 1920, h: 1080, s: 100, c: false },
        { w: 3840, h: 2160, s: false, c: true },
      ];
      const bitrateCases = [0, 80, 250, 1200, 6000];
      const audioBitrateCases = [-1, 0, 16, 48, 128];
      const rotationCases = [0, 90, 180, 270];
      const peerLabel = `codex-fuzz-peer-${iteration % 3}`;

      return [
        { ping: { source: "codex-publisher-churn", iteration, at: Date.now() } },
        { requestStats: true },
        { requestStatsContinuous: iteration % 2 === 0 },
        {
          remoteStats: {
            [peerLabel]: {
              video_bitrate_kbps: bitrateCases[iteration % bitrateCases.length],
              audio_bitrate_kbps: audioBitrateCases[(iteration + 1) % audioBitrateCases.length],
              info: {
                muted: iteration % 2 === 0,
                video_muted_init: iteration % 3 === 0,
              },
            },
          },
        },
        { obsState: { visibility: iteration % 2 === 0, muted: iteration % 3 === 0 } },
        { sceneDisplay: iteration % 2 === 0 },
        { sceneMute: iteration % 3 === 0 },
        {
          bitrate: bitrateCases[iteration % bitrateCases.length],
          audioBitrate: audioBitrateCases[iteration % audioBitrateCases.length],
        },
        {
          targetBitrate: bitrateCases[(iteration + 2) % bitrateCases.length],
          targetAudioBitrate: audioBitrateCases[(iteration + 2) % audioBitrateCases.length],
          optimizedBitrate: bitrateCases[(iteration + 3) % bitrateCases.length],
        },
        { requestResolution: resolutionCases[iteration % resolutionCases.length] },
        { screenShareState: iteration % 4 === 0 },
        { screenStopped: iteration % 5 === 0 },
        { directVideoMuted: iteration % 6 === 0, target: true },
        { virtualHangup: iteration % 9 === 0 },
        { rotate_video: rotationCases[iteration % rotationCases.length] },
        { mirrorGuestState: iteration % 2 === 0, mirrorGuestTarget: true },
        {
          info: {
            muted: iteration % 2 === 0,
            video_muted_init: iteration % 3 === 0,
            screenShareState: iteration % 4 === 0,
            directorVideoMuted: iteration % 6 === 0,
            directorSpeakerMuted: iteration % 2 === 1,
            directorDisplayMuted: iteration % 3 === 1,
            directorMirror: iteration % 2 === 0,
            directorFlip: iteration % 2 === 1,
            rotate_video: rotationCases[(iteration + 1) % rotationCases.length],
          },
        },
        { getConnectionMap: true },
        { connectionMap: { nodes: [{ id: peerLabel }], edges: [], iteration } },
      ];
    }

    function aggressiveControlMessages(iteration) {
      const uuid = `codex-fuzz-peer-${iteration % 3}`;
      return [
        { refreshVideo: true, UUID: uuid, remote: "codex-fuzz" },
        { refreshMicrophone: true, UUID: uuid, remote: "codex-fuzz" },
        { refreshConnection: true, UUID: uuid, remote: "codex-fuzz" },
        { refreshAll: true, UUID: uuid, remote: "codex-fuzz" },
        { restartWhip: true, UUID: uuid, remote: "codex-fuzz" },
        { remoteVideoMuted: iteration % 2 === 0 },
        { speakerMute: iteration % 2 === 0 },
        { displayMute: iteration % 3 === 0 },
        { rotate: iteration % 3 === 0 ? "toggle" : iteration % 3 === 1 ? false : 180, remote: "codex-fuzz" },
        { obsCommand: "SetCurrentProgramScene" },
        { action: "bitrate", value: iteration * 100 },
        { requestVideoHack: { deviceId: "default", width: 1280, height: 720 }, UUID: uuid },
        { changeCamera: "default", UUID: uuid },
        { requestAudioHack: { deviceId: "default", echoCancellation: false }, UUID: uuid },
        { group: iteration % 2 === 0 ? "codex-a,codex-b" : false },
      ];
    }

    function terminalControlMessages(iteration) {
      if (iteration % 13 !== 7) {
        return [];
      }
      return [
        { iceRestartRequest: false },
        { hangup: false },
        { bye: false },
        { request: "cleanup" },
      ];
    }

    function malformedDataStrings(iteration) {
      if (iteration % 5 !== 0) {
        return [];
      }
      const largePayload = JSON.stringify({
        chat: "codex-large-data-message",
        body: "x".repeat(16 * 1024),
        iteration,
      });
      return ["not-json", "{", "[]", "null", largePayload];
    }

    function sendDataMessages(iteration) {
      const messages = [
        { chat: `codex publisher churn ${iteration}`, timestamp: Date.now() },
        { audioMuted: iteration % 2 === 0, videoMuted: iteration % 3 === 0 },
        iteration % 2 === 0 ? { tallyOn: true } : { tallyPreview: true },
        { keyframe: true },
        { requestResolution: { w: iteration % 2 === 0 ? 640 : 1920, h: iteration % 2 === 0 ? 360 : 1080, s: false, c: null } },
        { screenShareState: iteration % 4 === 0 },
        { getConnectionMap: true },
      ];
      const dataFuzzProfile = state.dataFuzzProfile || "off";
      if (dataFuzzProfile !== "off") {
        messages.push(...officialStateMessages(iteration));
      }
      if (dataFuzzProfile === "aggressive" || dataFuzzProfile === "terminal") {
        messages.push(...aggressiveControlMessages(iteration));
      }
      if (dataFuzzProfile === "terminal") {
        messages.push(...terminalControlMessages(iteration));
      }
      const malformedMessages =
        dataFuzzProfile === "aggressive" || dataFuzzProfile === "terminal" ? malformedDataStrings(iteration) : [];

      const channels = [];
      for (const pc of livePeerConnections()) {
        if (pc.sendChannel) {
          channels.push(pc.sendChannel);
        }
        if (pc.receiveChannel) {
          channels.push(pc.receiveChannel);
        }
      }
      if (Array.isArray(window.__dcList)) {
        channels.push(...window.__dcList);
      }
      if (window.session && window.session.pcs && typeof window.session.pcs === "object") {
        for (const pc of Object.values(window.session.pcs)) {
          if (pc && pc.sendChannel) {
            channels.push(pc.sendChannel);
          }
          if (pc && pc.receiveChannel) {
            channels.push(pc.receiveChannel);
          }
        }
      }

      const uniqueChannels = Array.from(new Set(channels)).filter((channel) => {
        return channel && channel.readyState === "open" && typeof channel.send === "function";
      });

      for (const channel of uniqueChannels) {
        for (const message of messages) {
          try {
            channel.send(JSON.stringify(message));
            state.dataMessages += 1;
          } catch (error) {
            rememberError(error, "data-channel-send");
          }
        }
        for (const rawMessage of malformedMessages) {
          try {
            channel.send(rawMessage);
            state.dataMessages += 1;
            state.malformedDataMessages += 1;
          } catch (error) {
            rememberError(error, "data-channel-send-malformed");
          }
        }
        if ((dataFuzzProfile === "aggressive" || dataFuzzProfile === "terminal") && iteration % 7 === 0) {
          try {
            channel.send(new Uint8Array([0, 1, 2, 3, iteration % 256]).buffer);
            state.binaryDataMessages += 1;
          } catch (error) {
            rememberError(error, "data-channel-send-binary");
          }
        }
      }
    }

    async function tick() {
      const aggressive = state.profile === "aggressive";
      const peerConnections = livePeerConnections();
      if (peerConnections.length === 0) {
        state.emptyTicks += 1;
        remember("tick-empty", { emptyTicks: state.emptyTicks });
        return;
      }

      state.iteration += 1;
      const iteration = state.iteration;
      remember("tick", { iteration, peerConnections: peerConnections.length });

      for (const pc of peerConnections) {
        const senders = typeof pc.getSenders === "function" ? pc.getSenders() : [];
        for (const sender of senders) {
          const track = sender && sender.track;
          if (!track || typeof sender.replaceTrack !== "function") {
            continue;
          }

          if (iteration % 2 === 0) {
            track.enabled = !track.enabled;
            state.enabledToggles += 1;
            remember("toggle-enabled", { trackKind: track.kind, enabled: track.enabled });
          }

          try {
            if (track.kind === "video" && iteration % 3 === 0) {
              await replaceSenderTrack(sender, makeVideoTrack(iteration), "video-generated");
            } else if (track.kind === "audio" && iteration % 4 === 0) {
              const channels = iteration % 12 === 0 ? 6 : iteration % 8 === 0 ? 1 : 2;
              await replaceSenderTrack(sender, makeAudioTrack(iteration, channels), `audio-generated-${channels}ch`);
            }

            if (aggressive && iteration % 11 === 5) {
              const kind = sender.track ? sender.track.kind : track.kind;
              await replaceSenderTrack(sender, null, `${kind}-null`);
              state.nullReplacements += 1;
              setTimeout(() => {
                const replacement = kind === "audio" ? makeAudioTrack(iteration + 1, 2) : makeVideoTrack(iteration + 1);
                replaceSenderTrack(sender, replacement, `${kind}-restore`).catch((error) => {
                  rememberError(error, `${kind}-restore`);
                });
              }, 300);
            }
          } catch (error) {
            rememberError(error, `sender-${track.kind}`);
          }
        }

        if (state.extraAudio && !pc.__codexChurnExtraAudioAdded && typeof pc.addTrack === "function") {
          try {
            const extraTrack = makeAudioTrack(iteration + 100, iteration % 5 === 0 ? 6 : 2);
            const extraStream = new MediaStream([extraTrack]);
            pc.addTrack(extraTrack, extraStream);
            pc.__codexChurnExtraAudioAdded = true;
            state.extraAudioTracks += 1;
            remember("extra-audio-track", { iteration });
          } catch (error) {
            rememberError(error, "extra-audio-track");
          }
        }
      }

      sendDataMessages(iteration);

      if (state.maxIterations > 0 && iteration >= state.maxIterations) {
        clearInterval(state.timer);
        state.timer = null;
        state.stoppedAt = Date.now();
        remember("stopped", { iteration });
      }
    }

    state.timer = setInterval(() => {
      tick().catch((error) => rememberError(error, "tick"));
    }, Math.max(100, churnConfig.intervalMs));
    tick().catch((error) => rememberError(error, "initial-tick"));

    return {
      started: true,
      profile: state.profile,
      intervalMs: state.intervalMs,
      maxIterations: state.maxIterations,
      extraAudio: state.extraAudio,
    };
  }, config);
}

function startPublisherReloadLoop(entries, config) {
  const state = {
    started: false,
    requestedReloads: config.reloads,
    intervalMs: config.intervalMs,
    startupWaitMs: config.startupWaitMs,
    completedReloads: 0,
    errors: [],
    recentEvents: [],
    cancelled: false,
    promise: null,
  };

  const remember = (kind, fields = {}) => {
    state.recentEvents.push({ at: Date.now(), kind, ...fields });
    if (state.recentEvents.length > 80) {
      state.recentEvents.shift();
    }
  };
  const rememberError = (error, context) => {
    state.errors.push({ at: Date.now(), context, message: String(error && error.message ? error.message : error) });
    if (state.errors.length > 40) {
      state.errors.shift();
    }
  };

  if (!entries.length || config.reloads <= 0) {
    return state;
  }

  state.started = true;
  state.promise = (async () => {
    for (let iteration = 0; iteration < config.reloads && !state.cancelled; iteration += 1) {
      await sleep(config.intervalMs);
      if (state.cancelled) {
        break;
      }

      const entry = entries[iteration % entries.length];
      remember("reload-start", { iteration: iteration + 1, publisherIndex: entry.index, url: entry.url });
      try {
        await preparePublisherPage(entry.page, entry.url, config.startupWaitMs);
        entry.reloads += 1;
        state.completedReloads += 1;
        remember("reload-complete", { iteration: iteration + 1, publisherIndex: entry.index });
        entry.lastChurnStart = await startPublisherChurn(entry.page, config.churn);
      } catch (error) {
        rememberError(error, `publisher-${entry.index}-reload`);
      }
    }
  })();

  return state;
}

function serializeReloadState(state) {
  if (!state) {
    return null;
  }
  return {
    started: state.started,
    requestedReloads: state.requestedReloads,
    intervalMs: state.intervalMs,
    startupWaitMs: state.startupWaitMs,
    completedReloads: state.completedReloads,
    errors: state.errors,
    recentEvents: state.recentEvents,
  };
}

async function collectPublisherChurn(page) {
  return page.evaluate(() => {
    const state = window.__publisherChurn;
    if (!state) {
      return null;
    }
    return {
      profile: state.profile,
      intervalMs: state.intervalMs,
      maxIterations: state.maxIterations,
      extraAudio: state.extraAudio,
      dataFuzzProfile: state.dataFuzzProfile,
      startedAt: state.startedAt,
      stoppedAt: state.stoppedAt,
      iteration: state.iteration,
      emptyTicks: state.emptyTicks,
      replacements: state.replacements,
      enabledToggles: state.enabledToggles,
      nullReplacements: state.nullReplacements,
      extraAudioTracks: state.extraAudioTracks,
      dataMessages: state.dataMessages,
      malformedDataMessages: state.malformedDataMessages,
      binaryDataMessages: state.binaryDataMessages,
      errors: state.errors,
      recentEvents: state.events,
    };
  });
}

async function main() {
  const pushUrlInput = process.argv[2] || "https://vdo.ninja/?push=Alsosuitbc&password=somepassword";
  const viewUrlInput = process.argv[3] || "https://vdo.ninja/?view=Alsosuitbc&password=somepassword";
  const durationMs = Number(process.env.PUBLISH_DURATION_MS || 15 * 60 * 1000);
  const startupWaitMs = Number(process.env.PUBLISH_STARTUP_WAIT_MS || 20000);
  const viewProbeWaitMs = Number(process.env.VIEW_PROBE_WAIT_MS || 25000);
  const fakeAudioCaptureFile = process.env.FAKE_AUDIO_CAPTURE_FILE || "";
  const churnProfile = (process.env.PUBLISH_CHURN_PROFILE || "off").trim().toLowerCase();
  const churnIterations = parseNonNegativeInteger(
    process.env.PUBLISH_CHURN_ITERATIONS || (churnProfile === "aggressive" ? 120 : 45)
  );
  const churnIntervalMs = parseNonNegativeInteger(
    process.env.PUBLISH_CHURN_INTERVAL_MS || (churnProfile === "aggressive" ? 750 : 1500),
    churnProfile === "aggressive" ? 750 : 1500
  );
  const churnExtraAudio = parseBoolean(process.env.PUBLISH_CHURN_EXTRA_AUDIO, churnProfile === "aggressive");
  const dataFuzzProfile = normalizeChoice(
    process.env.PUBLISH_DATA_FUZZ_PROFILE || "off",
    ["off", "official", "aggressive", "terminal"],
    "off"
  );
  const publisherCount = Math.max(1, parseNonNegativeInteger(process.env.PUBLISHER_COUNT, 1));
  const publisherStaggerMs = parseNonNegativeInteger(process.env.PUBLISHER_STAGGER_MS, 1500);
  const publisherVariants = parsePublisherVariants(process.env.PUBLISHER_URL_VARIANTS || "");
  const publisherReloads = parseNonNegativeInteger(process.env.PUBLISHER_RELOADS, 0);
  const publisherReloadIntervalMs = parseNonNegativeInteger(process.env.PUBLISHER_RELOAD_INTERVAL_MS, 20000);
  const publisherReloadStartupWaitMs = parseNonNegativeInteger(
    process.env.PUBLISHER_RELOAD_STARTUP_WAIT_MS,
    Math.min(startupWaitMs, 8000)
  );

  const viewUrl = ensureQuery(viewUrlInput, "cleanoutput", "1");

  const launchArgs = [
    "--autoplay-policy=no-user-gesture-required",
    "--use-fake-ui-for-media-stream",
    "--use-fake-device-for-media-stream",
    "--allow-http-screen-capture",
  ];
  if (fakeAudioCaptureFile) {
    launchArgs.push(`--use-file-for-fake-audio-capture=${fakeAudioCaptureFile}`);
  }

  const browser = await chromium.launch({
    headless: process.env.HEADLESS === "0" ? false : true,
    args: launchArgs,
  });

  const context = await browser.newContext({
    permissions: ["camera", "microphone"],
  });

  await context.addInitScript(() => {
    window.__pcList = [];
    window.__dcList = [];
    const NativePC = window.RTCPeerConnection;
    if (NativePC) {
      const nativeCreateDataChannel = NativePC.prototype.createDataChannel;
      if (nativeCreateDataChannel && !NativePC.prototype.__codexDataChannelPatched) {
        NativePC.prototype.createDataChannel = function (...dcArgs) {
          const dc = nativeCreateDataChannel.apply(this, dcArgs);
          window.__dcList.push(dc);
          return dc;
        };
        NativePC.prototype.__codexDataChannelPatched = true;
      }
      window.RTCPeerConnection = function (...args) {
        const pc = new NativePC(...args);
        window.__pcList.push(pc);
        pc.addEventListener("datachannel", (event) => {
          if (event && event.channel) {
            window.__dcList.push(event.channel);
          }
        });
        return pc;
      };
      window.RTCPeerConnection.prototype = NativePC.prototype;
    }
  });

  const publisherEntries = [];
  for (let index = 0; index < publisherCount; index += 1) {
    const page = await context.newPage();
    const url = buildPublisherUrl(pushUrlInput, index, publisherVariants);
    await preparePublisherPage(page, url, startupWaitMs);
    publisherEntries.push({ index, page, url, reloads: 0, lastChurnStart: null });
    if (index + 1 < publisherCount && publisherStaggerMs > 0) {
      await sleep(publisherStaggerMs);
    }
  }

  const churnConfig = {
    profile: churnProfile,
    iterations: churnIterations,
    intervalMs: Math.max(100, churnIntervalMs),
    extraAudio: churnExtraAudio,
    dataFuzzProfile,
  };

  const publisherChurnStart = [];
  for (const entry of publisherEntries) {
    entry.lastChurnStart = await startPublisherChurn(entry.page, churnConfig);
    publisherChurnStart.push({ publisherIndex: entry.index, url: entry.url, ...entry.lastChurnStart });
  }

  const publisherReloadState = startPublisherReloadLoop(publisherEntries, {
    reloads: publisherReloads,
    intervalMs: Math.max(1000, publisherReloadIntervalMs),
    startupWaitMs: publisherReloadStartupWaitMs,
    churn: churnConfig,
  });

  const publisherSnapshots = [];
  for (const entry of publisherEntries) {
    publisherSnapshots.push({ publisherIndex: entry.index, url: entry.url, snapshot: await collectMediaSnapshot(entry.page) });
  }

  const viewerPage = await context.newPage();
  await viewerPage.goto(viewUrl, { waitUntil: "domcontentloaded", timeout: 60000 });
  await viewerPage.waitForTimeout(viewProbeWaitMs);
  const viewerSample1 = await collectMediaSnapshot(viewerPage);
  await viewerPage.waitForTimeout(7000);
  const viewerSample2 = await collectMediaSnapshot(viewerPage);

  const playbackAdvanced = viewerSample2.videoElements.some((v2) => {
    const v1 = viewerSample1.videoElements.find((x) => x.index === v2.index);
    return v1 && v2.currentTime > v1.currentTime + 0.4;
  });

  const hasInboundBytes = viewerSample2.pcStats.some(
    (s) => (s.inboundVideoBytes || 0) > 0 || (s.inboundAudioBytes || 0) > 0
  );
  const hasTracks = viewerSample2.videoElements.some((v) => v.videoTracks > 0 || v.audioTracks > 0);

  const report = {
    startedAt: new Date().toISOString(),
    pushUrl: publisherEntries[0] ? publisherEntries[0].url : buildPublisherUrl(pushUrlInput, 0, publisherVariants),
    publisherCount,
    publisherVariants,
    publisherUrls: publisherEntries.map((entry) => entry.url),
    publisherReloads: serializeReloadState(publisherReloadState),
    publisherDataFuzzProfile: dataFuzzProfile,
    viewUrl,
    publisherChurnStart,
    publisherChurn: publisherEntries[0] ? await collectPublisherChurn(publisherEntries[0].page) : null,
    publisherChurns: await Promise.all(
      publisherEntries.map(async (entry) => ({
        publisherIndex: entry.index,
        url: entry.url,
        reloads: entry.reloads,
        churn: await collectPublisherChurn(entry.page),
      }))
    ),
    publisherSnapshot: publisherSnapshots[0] ? publisherSnapshots[0].snapshot : null,
    publisherSnapshots,
    viewerSample1,
    viewerSample2,
    playbackAdvanced,
    hasInboundBytes,
    hasTracks,
    likelyConnected: playbackAdvanced || (hasInboundBytes && hasTracks),
    keepAliveMs: durationMs,
  };

  console.log(JSON.stringify(report, null, 2));

  // Keep publisher alive so the user can check live playout.
  await viewerPage.close();
  await sleep(durationMs);
  publisherReloadState.cancelled = true;
  if (publisherReloadState.promise) {
    await Promise.race([publisherReloadState.promise, sleep(1000)]).catch(() => {});
  }

  await context.close();
  await browser.close();
}

main().catch((error) => {
  console.error(error);
  process.exit(1);
});
