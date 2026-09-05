const fs = require("fs");
const path = require("path");
const crypto = require("crypto");
const zlib = require("zlib");
const http = require("http");
const https = require("https");
const { spawn } = require("child_process");

function sleep(ms) {
  return new Promise((resolve) => setTimeout(resolve, ms));
}

function requestJson(urlText, token, options = {}) {
  const url = new URL(urlText);
  const transport = url.protocol === "https:" ? https : http;
  const method = options.method || "GET";
  const body = options.body === undefined ? null : JSON.stringify(options.body);
  const timeoutMs = Math.max(250, Number(options.timeoutMs || 2000));
  return new Promise((resolve, reject) => {
    const request = transport.request(url, {
      method,
      headers: {
        Accept: "application/json",
        Authorization: `Bearer ${token}`,
        ...(body
          ? {
              "Content-Type": "application/json",
              "Content-Length": Buffer.byteLength(body),
            }
          : {}),
      },
    }, (response) => {
      const chunks = [];
      response.on("data", (chunk) => chunks.push(Buffer.from(chunk)));
      response.on("end", () => {
        const text = Buffer.concat(chunks).toString("utf8");
        if ((response.statusCode || 500) < 200 || (response.statusCode || 500) >= 300) {
          reject(new Error(`HTTP ${response.statusCode} from ${url.pathname}: ${text.slice(0, 500)}`));
          return;
        }
        try {
          resolve(JSON.parse(text));
        } catch (error) {
          reject(new Error(`Invalid JSON from ${url.pathname}: ${error.message}`));
        }
      });
    });
    request.setTimeout(timeoutMs, () => request.destroy(new Error(`HTTP timeout after ${timeoutMs}ms`)));
    request.on("error", reject);
    if (body) {
      request.write(body);
    }
    request.end();
  });
}

function normalizedDiagnosticsPeer(peer) {
  if (!peer || typeof peer !== "object") {
    return null;
  }
  const signaling = peer.signaling && typeof peer.signaling === "object" ? peer.signaling : {};
  const transport = peer.transport && typeof peer.transport === "object" ? peer.transport : {};
  const clientTransportGeneration = Number(signaling.client_transport_generation);
  const activeTransportGeneration = Number(signaling.active_transport_generation);
  const activeOfferGeneration = Number(signaling.active_offer_generation);
  const uuid = String(peer.uuid || "");
  const session = String(peer.session || "");
  if (!uuid || !session || !Number.isFinite(clientTransportGeneration) ||
      !Number.isFinite(activeTransportGeneration)) {
    return null;
  }
  const state = String(peer.last_connection_state || "unknown").toLowerCase();
  const dataChannelOpen = transport.data_channel_open === true;
  const transportRetired = transport.transport_retired === true;
  const normalized = {
    uuid,
    session,
    streamId: String(peer.stream_id || ""),
    state,
    connected: state === "connected" && dataChannelOpen && !transportRetired,
    transportRetired,
    dataChannelOpen,
    clientTransportGeneration,
    activeTransportGeneration,
    activeOfferGeneration: Number.isFinite(activeOfferGeneration) ? activeOfferGeneration : null,
    createdSteadyMs: Number(peer.created_steady_ms) || 0,
    systemPlatform: String(peer.system && peer.system.platform || ""),
    systemBrowser: String(peer.system && peer.system.browser || ""),
  };
  normalized.logicalKey = `${uuid}|${session}`;
  normalized.transportKey = `${normalized.logicalKey}|${clientTransportGeneration}|${activeTransportGeneration}`;
  return normalized;
}

function isNativeObsDiagnosticsPeer(peer, streamId) {
  if (!peer || (streamId && peer.streamId !== streamId)) {
    return false;
  }
  return /obs/i.test(peer.systemPlatform) && /native receiver/i.test(peer.systemBrowser);
}

function sameTransportIdentity(left, right) {
  return !!left && !!right && left.transportKey === right.transportKey;
}

function sameLogicalPeer(left, right) {
  return !!left && !!right && left.logicalKey === right.logicalKey;
}

function snapshotContainsActiveTransport(snapshot, identity) {
  return !!snapshot && Array.isArray(snapshot.peers) && snapshot.peers.some(
    (peer) => peer.connected && sameTransportIdentity(peer, identity)
  );
}

function snapshotShowsTransportRetiredOrAbsent(snapshot, identity) {
  if (!snapshot || !Array.isArray(snapshot.peers) || !identity) {
    return false;
  }
  const matching = snapshot.peers.find((peer) => sameTransportIdentity(peer, identity));
  return !matching || matching.transportRetired === true;
}

function evaluateObservedTransition(kind, before, retired, after) {
  const failureReasons = [];
  const beforePeer = before && before.peer;
  const afterPeer = after && after.peer;
  if (!before || before.source !== "game-capture-diagnostics" || !beforePeer || !beforePeer.connected) {
    failureReasons.push("missing connected before-peer diagnostics evidence");
  }
  if (!retired || retired.source !== "game-capture-diagnostics") {
    failureReasons.push("missing diagnostics evidence for the transition boundary");
  }
  if (!after || after.source !== "game-capture-diagnostics" || !afterPeer || !afterPeer.connected) {
    failureReasons.push("missing connected after-peer diagnostics evidence");
  }
  if (beforePeer && retired && !snapshotShowsTransportRetiredOrAbsent(retired, beforePeer)) {
    failureReasons.push("the old transport had neither disappeared nor been marked retired at the claimed boundary");
  }

  if (kind === "same-peer-ice-rebuild") {
    if (beforePeer && afterPeer && !sameLogicalPeer(beforePeer, afterPeer)) {
      failureReasons.push("same-peer rebuild changed uuid/session");
    }
    if (beforePeer && afterPeer &&
        afterPeer.clientTransportGeneration <= beforePeer.clientTransportGeneration) {
      failureReasons.push("client transport generation did not increase");
    }
    if (beforePeer && afterPeer &&
        afterPeer.activeTransportGeneration <= beforePeer.activeTransportGeneration) {
      failureReasons.push("active transport generation did not increase");
    }
    if (before && after && before.publisherPid !== after.publisherPid) {
      failureReasons.push("same-peer rebuild unexpectedly changed publisher PID");
    }
  } else if (kind === "publisher-restart") {
    if (before && after && before.publisherPid === after.publisherPid) {
      failureReasons.push("publisher restart did not change publisher PID");
    }
    if (before && after && before.control && after.control &&
        before.control.discoverySha256 === after.control.discoverySha256) {
      failureReasons.push("publisher restart reused the old control discovery identity");
    }
    if (beforePeer && afterPeer && before && after &&
        `${before.publisherPid}|${beforePeer.transportKey}` ===
          `${after.publisherPid}|${afterPeer.transportKey}`) {
      failureReasons.push("publisher restart reused the old publisher/transport identity");
    }
  } else {
    if (before && after && before.publisherPid !== after.publisherPid) {
      failureReasons.push("source lifecycle transition unexpectedly changed publisher PID");
    }
    if (beforePeer && afterPeer && sameTransportIdentity(beforePeer, afterPeer)) {
      failureReasons.push("source lifecycle transition reused the old peer transport identity");
    }
    if (beforePeer && afterPeer && sameLogicalPeer(beforePeer, afterPeer)) {
      failureReasons.push("source lifecycle transition did not create a distinct uuid/session peer");
    }
  }
  return {
    ok: failureReasons.length === 0,
    kind,
    before,
    retired,
    after,
    failureReasons,
  };
}

function classifyObservedConnection(snapshot, transitionEvidence, initialPeer) {
  const peer = snapshot && snapshot.peer;
  const beforePeer = transitionEvidence && transitionEvidence.before && transitionEvidence.before.peer || initialPeer;
  const afterPeer = transitionEvidence && transitionEvidence.after && transitionEvidence.after.peer;
  if (peer && afterPeer && sameTransportIdentity(peer, afterPeer)) {
    return "post";
  }
  if (peer && beforePeer && sameTransportIdentity(peer, beforePeer)) {
    return "pre";
  }
  return "unknown";
}

class GameCaptureDiagnosticsObserver {
  constructor(discoveryPath, streamId, options = {}) {
    this.discoveryPath = discoveryPath;
    this.streamId = streamId;
    this.pollIntervalMs = Math.max(25, Number(options.pollIntervalMs || 50));
    this.requestTimeoutMs = Math.max(250, Number(options.requestTimeoutMs || 1500));
    this.snapshots = [];
    this.errors = [];
    this.running = false;
    this.loopPromise = null;
  }

  readDiscovery() {
    if (!this.discoveryPath || !fs.existsSync(this.discoveryPath)) {
      throw new Error(`Game Capture control discovery is unavailable: ${this.discoveryPath || "missing path"}`);
    }
    const discoveryText = fs.readFileSync(this.discoveryPath, "utf8");
    const discovery = JSON.parse(discoveryText);
    if (!discovery.base_url || !discovery.token || !Number.isFinite(Number(discovery.pid))) {
      throw new Error("Game Capture control discovery is incomplete");
    }
    return {
      pid: Number(discovery.pid),
      baseUrl: String(discovery.base_url).replace(/\/$/, ""),
      token: String(discovery.token),
      version: String(discovery.version || ""),
      createdUtc: String(discovery.created_utc || ""),
      discoverySha256: crypto.createHash("sha256").update(discoveryText).digest("hex"),
      tokenSha256: crypto.createHash("sha256").update(String(discovery.token)).digest("hex"),
    };
  }

  async observe(label = "poll") {
    const control = this.readDiscovery();
    const diagnostics = await requestJson(`${control.baseUrl}/diagnostics`, control.token, {
      timeoutMs: this.requestTimeoutMs,
    });
    const peers = (Array.isArray(diagnostics.peers) ? diagnostics.peers : [])
      .map(normalizedDiagnosticsPeer)
      .filter((peer) => isNativeObsDiagnosticsPeer(peer, this.streamId))
      .sort((left, right) => right.createdSteadyMs - left.createdSteadyMs);
    const peer = peers.find((candidate) => candidate.connected) || peers[0] || null;
    const snapshot = {
      source: "game-capture-diagnostics",
      label,
      observedAtMs: Date.now(),
      observedAt: new Date().toISOString(),
      publisherPid: control.pid,
      control: {
        baseUrl: control.baseUrl,
        version: control.version,
        createdUtc: control.createdUtc,
        discoverySha256: control.discoverySha256,
        tokenSha256: control.tokenSha256,
      },
      diagnosticsSha256: crypto.createHash("sha256").update(JSON.stringify(diagnostics)).digest("hex"),
      generatedSteadyMs: Number(diagnostics.generated_steady_ms) || null,
      peer,
      peers,
    };
    this.snapshots.push(snapshot);
    if (this.snapshots.length > 2000) {
      this.snapshots.splice(0, this.snapshots.length - 2000);
    }
    return snapshot;
  }

  start() {
    if (this.running) {
      return;
    }
    this.running = true;
    this.loopPromise = (async () => {
      while (this.running) {
        try {
          await this.observe("background-poll");
        } catch (error) {
          this.errors.push({
            observedAtMs: Date.now(),
            message: String(error && error.message ? error.message : error),
          });
          if (this.errors.length > 200) {
            this.errors.shift();
          }
        }
        await sleep(this.pollIntervalMs);
      }
    })();
  }

  async stop() {
    this.running = false;
    if (this.loopPromise) {
      await this.loopPromise;
    }
  }

  latest() {
    return this.snapshots[this.snapshots.length - 1] || null;
  }

  async waitFor(predicate, timeoutMs, label, sinceMs = 0) {
    const deadline = Date.now() + timeoutMs;
    let cursor = Math.max(
      0,
      this.snapshots.findIndex((snapshot) => snapshot.observedAtMs >= sinceMs)
    );
    if (this.snapshots.length && !this.snapshots.some((snapshot) => snapshot.observedAtMs >= sinceMs)) {
      cursor = this.snapshots.length;
    }
    while (Date.now() < deadline) {
      const available = this.snapshots.slice(cursor);
      const match = available.find(predicate);
      if (match) {
        return { ...match, boundaryLabel: label };
      }
      cursor = this.snapshots.length;
      try {
        const immediate = await this.observe(`boundary:${label}`);
        if (predicate(immediate)) {
          return { ...immediate, boundaryLabel: label };
        }
      } catch (error) {
        this.errors.push({ observedAtMs: Date.now(), message: String(error.message || error) });
      }
      await sleep(25);
    }
    throw new Error(`Timed out waiting for observed diagnostics boundary '${label}'`);
  }

  async postControlCommand(body, label) {
    const control = this.readDiscovery();
    const requestedAtMs = Date.now();
    const response = await requestJson(`${control.baseUrl}/commands`, control.token, {
      method: "POST",
      body,
      timeoutMs: 5000,
    });
    return {
      label,
      requestedAtMs,
      completedAtMs: Date.now(),
      publisherPid: control.pid,
      baseUrl: control.baseUrl,
      tokenSha256: control.tokenSha256,
      request: body,
      response,
    };
  }
}

class ObsWebSocketClient {
  constructor(url, options = {}) {
    this.url = url;
    this.eventSubscriptions = options.eventSubscriptions || 0;
    this.onEvent = typeof options.onEvent === "function" ? options.onEvent : null;
    this.socket = null;
    this.requestId = 0;
    this.pending = new Map();
    this.identified = false;
    this.requestTimeoutMs = Number(process.env.OBS_WEBSOCKET_REQUEST_TIMEOUT_MS || 15000);
  }

  async connect() {
    await new Promise((resolve, reject) => {
      const socket = new WebSocket(this.url, "obswebsocket.json");
      this.socket = socket;

      socket.addEventListener("open", () => resolve());
      socket.addEventListener("error", (error) => reject(error));
      socket.addEventListener("message", async (event) => {
        try {
          const message = JSON.parse(event.data.toString());
          if (message.op === 0) {
            const identify = { rpcVersion: 1, eventSubscriptions: this.eventSubscriptions };
            if (message.d && message.d.authentication) {
              const password = process.env.OBS_WEBSOCKET_PASSWORD;
              if (!password) throw new Error("Set OBS_WEBSOCKET_PASSWORD for this OBS server");
              const secret = crypto.createHash("sha256")
                .update(password + message.d.authentication.salt, "utf8").digest("base64");
              identify.authentication = crypto.createHash("sha256")
                .update(secret + message.d.authentication.challenge, "utf8").digest("base64");
            }
            socket.send(
              JSON.stringify({
                op: 1,
                d: identify,
              })
            );
            return;
          }

          if (message.op === 2) {
            this.identified = true;
            return;
          }

          if (message.op !== 7) {
            if (message.op === 5 && this.onEvent) {
              this.onEvent(message.d || {});
            }
            return;
          }

          const requestId = message.d && message.d.requestId;
          const pending = requestId ? this.pending.get(requestId) : null;
          if (!pending) {
            return;
          }

          this.pending.delete(requestId);
          if (message.d.requestStatus && message.d.requestStatus.result) {
            pending.resolve(message.d.responseData || {});
            return;
          }

          const comment =
            (message.d.requestStatus && message.d.requestStatus.comment) || "OBS request failed";
          pending.reject(new Error(`${message.d.requestType}: ${comment}`));
        } catch (error) {
          reject(error);
        }
      });
    });

    for (let i = 0; i < 50 && !this.identified; i += 1) {
      await sleep(100);
    }

    if (!this.identified) {
      throw new Error("Timed out waiting for obs-websocket identify handshake");
    }
  }

  async request(requestType, requestData = {}) {
    if (!this.socket || this.socket.readyState !== WebSocket.OPEN) {
      throw new Error("obs-websocket is not connected");
    }

    const requestId = `req-${++this.requestId}`;
    const payload = {
      op: 6,
      d: {
        requestType,
        requestId,
        requestData,
      },
    };

    const response = new Promise((resolve, reject) => {
      const timeout = setTimeout(() => {
        this.pending.delete(requestId);
        reject(new Error(`${requestType}: Timed out after ${this.requestTimeoutMs}ms`));
      }, this.requestTimeoutMs);

      this.pending.set(requestId, {
        resolve: (value) => {
          clearTimeout(timeout);
          resolve(value);
        },
        reject: (error) => {
          clearTimeout(timeout);
          reject(error);
        },
      });
    });

    this.socket.send(JSON.stringify(payload));
    return response;
  }

  async close() {
    if (!this.socket) {
      return;
    }

    for (const pending of this.pending.values()) {
      pending.reject(new Error("obs-websocket connection closed"));
    }
    this.pending.clear();

    this.socket.close();
    this.socket = null;
  }
}

const EVENT_SUBSCRIPTION_INPUT_VOLUME_METERS = 1 << 16;

function parseBoolean(value, fallback = false) {
  if (value === undefined) {
    return fallback;
  }
  const normalized = String(value).trim().toLowerCase();
  return normalized === "1" || normalized === "true" || normalized === "yes" || normalized === "on";
}

function parseNumber(value, fallback) {
  if (value === undefined || value === null || value === "") {
    return fallback;
  }
  const parsed = Number(value);
  return Number.isFinite(parsed) ? parsed : fallback;
}

function clampInt(value, fallback, minValue, maxValue) {
  const parsed = Math.trunc(parseNumber(value, fallback));
  return Math.max(minValue, Math.min(maxValue, parsed));
}

function logStep(message) {
  console.error(`[obs-source-check] ${message}`);
}

function selectColorSourceKind(inputKinds) {
  for (const candidate of ["color_source_v3", "color_source"]) {
    if (inputKinds.includes(candidate)) {
      return candidate;
    }
  }
  return null;
}

function paethPredictor(left, up, upLeft) {
  const p = left + up - upLeft;
  const pa = Math.abs(p - left);
  const pb = Math.abs(p - up);
  const pc = Math.abs(p - upLeft);
  if (pa <= pb && pa <= pc) {
    return left;
  }
  return pb <= pc ? up : upLeft;
}

function decodePng(buffer) {
  const signature = Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]);
  if (buffer.length < signature.length || !buffer.subarray(0, signature.length).equals(signature)) {
    throw new Error("PNG signature not found");
  }

  let offset = signature.length;
  let width = 0;
  let height = 0;
  let bitDepth = 0;
  let colorType = 0;
  const idatChunks = [];

  while (offset + 12 <= buffer.length) {
    const length = buffer.readUInt32BE(offset);
    const type = buffer.toString("ascii", offset + 4, offset + 8);
    const dataStart = offset + 8;
    const dataEnd = dataStart + length;
    if (dataEnd + 4 > buffer.length) {
      throw new Error(`PNG chunk ${type} exceeds buffer length`);
    }
    const data = buffer.subarray(dataStart, dataEnd);
    if (type === "IHDR") {
      width = data.readUInt32BE(0);
      height = data.readUInt32BE(4);
      bitDepth = data[8];
      colorType = data[9];
    } else if (type === "IDAT") {
      idatChunks.push(data);
    } else if (type === "IEND") {
      break;
    }
    offset = dataEnd + 4;
  }

  if (!width || !height) {
    throw new Error("PNG IHDR was missing dimensions");
  }
  if (bitDepth !== 8 || (colorType !== 2 && colorType !== 6)) {
    throw new Error(`Unsupported PNG format bitDepth=${bitDepth} colorType=${colorType}`);
  }

  const channels = colorType === 6 ? 4 : 3;
  const raw = zlib.inflateSync(Buffer.concat(idatChunks));
  const rowBytes = width * channels;
  const expectedRawBytes = (rowBytes + 1) * height;
  if (raw.length < expectedRawBytes) {
    throw new Error(`PNG data was shorter than expected (${raw.length}/${expectedRawBytes})`);
  }

  const filtered = Buffer.alloc(rowBytes * height);
  let rawOffset = 0;
  for (let y = 0; y < height; y += 1) {
    const filter = raw[rawOffset++];
    const rowStart = y * rowBytes;
    const prevRowStart = rowStart - rowBytes;
    for (let x = 0; x < rowBytes; x += 1) {
      const current = raw[rawOffset++];
      const left = x >= channels ? filtered[rowStart + x - channels] : 0;
      const up = y > 0 ? filtered[prevRowStart + x] : 0;
      const upLeft = y > 0 && x >= channels ? filtered[prevRowStart + x - channels] : 0;
      let value = current;
      if (filter === 1) {
        value = current + left;
      } else if (filter === 2) {
        value = current + up;
      } else if (filter === 3) {
        value = current + Math.floor((left + up) / 2);
      } else if (filter === 4) {
        value = current + paethPredictor(left, up, upLeft);
      } else if (filter !== 0) {
        throw new Error(`Unsupported PNG filter ${filter}`);
      }
      filtered[rowStart + x] = value & 0xff;
    }
  }

  const rgba = Buffer.alloc(width * height * 4);
  for (let source = 0, dest = 0; source < filtered.length; source += channels, dest += 4) {
    rgba[dest] = filtered[source];
    rgba[dest + 1] = filtered[source + 1];
    rgba[dest + 2] = filtered[source + 2];
    rgba[dest + 3] = channels === 4 ? filtered[source + 3] : 255;
  }

  return { width, height, rgba };
}

function pixelAt(image, x, y) {
  const clampedX = Math.max(0, Math.min(image.width - 1, Math.round(x)));
  const clampedY = Math.max(0, Math.min(image.height - 1, Math.round(y)));
  const index = (clampedY * image.width + clampedX) * 4;
  return {
    r: image.rgba[index],
    g: image.rgba[index + 1],
    b: image.rgba[index + 2],
    a: image.rgba[index + 3],
  };
}

function colorDistance(a, b) {
  return Math.abs(a.r - b.r) + Math.abs(a.g - b.g) + Math.abs(a.b - b.b);
}

function normalizeAlphaPattern(value) {
  return String(value || "generic").trim().toLowerCase();
}

function blendColor(foreground, background, alpha) {
  const inverse = 255 - alpha;
  return {
    r: Math.round((foreground.r * alpha + background.r * inverse) / 255),
    g: Math.round((foreground.g * alpha + background.g * inverse) / 255),
    b: Math.round((foreground.b * alpha + background.b * inverse) / 255),
    a: 255,
  };
}

function fixtureColorForEpoch(epoch) {
  return String(epoch || "pre").toLowerCase() === "post"
    ? { r: 32, g: 255, b: 255, a: 255 }
    : { r: 32, g: 96, b: 255, a: 255 };
}

function median(values) {
  if (!values.length) {
    return null;
  }
  const sorted = [...values].sort((a, b) => a - b);
  const middle = Math.floor(sorted.length / 2);
  return sorted.length % 2 === 0
    ? (sorted[middle - 1] + sorted[middle]) / 2
    : sorted[middle];
}

function analyzeAlphaCompositeImages(background, final, options = {}) {
  if (background.width !== final.width || background.height !== final.height) {
    throw new Error(
      `Screenshot dimensions changed between background and final captures: ` +
        `${background.width}x${background.height} -> ${final.width}x${final.height}`
    );
  }

  const expected = pixelAt(background, background.width / 2, background.height / 2);
  const pattern = normalizeAlphaPattern(options.pattern);
  const expectedVisualEpoch = String(options.expectedVisualEpoch || "pre").toLowerCase();
  const fixtureColor = fixtureColorForEpoch(expectedVisualEpoch);
  const alternateFixtureColor = fixtureColorForEpoch(expectedVisualEpoch === "post" ? "pre" : "post");
  const halfCompositeColor = blendColor(fixtureColor, expected, 128);
  const alternateHalfCompositeColor = blendColor(alternateFixtureColor, expected, 128);
  const tolerance = parseNumber(options.tolerance, 36);
  const fixtureColorTolerance = parseNumber(options.fixtureColorTolerance, 36);
  const halfCompositeTolerance = parseNumber(options.halfCompositeTolerance, 36);
  const step = Math.max(1, Math.trunc(parseNumber(options.sampleStep, 2)));
  let total = 0;
  let backgroundLike = 0;
  let foregroundLike = 0;
  let darkFill = 0;
  let greenFill = 0;
  let fixtureColorLike = 0;
  let alternateFixtureColorLike = 0;
  let halfCompositeLike = 0;
  let alternateHalfCompositeLike = 0;
  let markerMinX = final.width;
  let markerMinY = final.height;
  let markerMaxX = -1;
  let markerMaxY = -1;

  for (let y = 0; y < final.height; y += step) {
    for (let x = 0; x < final.width; x += step) {
      const px = pixelAt(final, x, y);
      total += 1;
      if (colorDistance(px, expected) <= tolerance) {
        backgroundLike += 1;
      } else {
        foregroundLike += 1;
      }
      if (px.r < 12 && px.g < 12 && px.b < 12) {
        darkFill += 1;
      }
      if (px.g > 150 && px.g > px.r + 40 && px.g > px.b + 40) {
        greenFill += 1;
      }
      if (colorDistance(px, fixtureColor) <= fixtureColorTolerance) {
        fixtureColorLike += 1;
        markerMinX = Math.min(markerMinX, x);
        markerMinY = Math.min(markerMinY, y);
        markerMaxX = Math.max(markerMaxX, x);
        markerMaxY = Math.max(markerMaxY, y);
      }
      if (colorDistance(px, alternateFixtureColor) <= fixtureColorTolerance) {
        alternateFixtureColorLike += 1;
      }
      if (colorDistance(px, halfCompositeColor) <= halfCompositeTolerance) {
        halfCompositeLike += 1;
      }
      if (colorDistance(px, alternateHalfCompositeColor) <= halfCompositeTolerance) {
        alternateHalfCompositeLike += 1;
      }
    }
  }

  const result = {
    ok: false,
    backgroundPngPath: options.backgroundPngPath || null,
    finalPngPath: options.finalPngPath || null,
    width: final.width,
    height: final.height,
    expectedBackground: expected,
    backgroundPixelSha256: crypto.createHash("sha256").update(background.rgba).digest("hex"),
    compositePixelSha256: crypto.createHash("sha256").update(final.rgba).digest("hex"),
    pattern,
    expectedVisualEpoch,
    fixtureColor,
    alternateFixtureColor,
    halfCompositeColor,
    alternateHalfCompositeColor,
    tolerance,
    fixtureColorTolerance,
    halfCompositeTolerance,
    sampleStep: step,
    totalSamples: total,
    backgroundLikeSamples: backgroundLike,
    foregroundLikeSamples: foregroundLike,
    darkFillSamples: darkFill,
    greenFillSamples: greenFill,
    backgroundLikeRatio: total > 0 ? backgroundLike / total : 0,
    foregroundLikeRatio: total > 0 ? foregroundLike / total : 0,
    darkFillRatio: total > 0 ? darkFill / total : 0,
    greenFillRatio: total > 0 ? greenFill / total : 0,
    fixtureColorLikeRatio: total > 0 ? fixtureColorLike / total : 0,
    alternateFixtureColorLikeRatio: total > 0 ? alternateFixtureColorLike / total : 0,
    halfCompositeLikeRatio: total > 0 ? halfCompositeLike / total : 0,
    alternateHalfCompositeLikeRatio: total > 0 ? alternateHalfCompositeLike / total : 0,
    markerBoundingBox:
      markerMaxX >= markerMinX && markerMaxY >= markerMinY
        ? {
            minX: markerMinX,
            minY: markerMinY,
            maxX: markerMaxX,
            maxY: markerMaxY,
            width: markerMaxX - markerMinX + step,
            height: markerMaxY - markerMinY + step,
            centerX: (markerMinX + markerMaxX) / 2,
            centerY: (markerMinY + markerMaxY) / 2,
          }
        : null,
  };

  const minBackgroundRatio = parseNumber(options.minBackgroundRatio, 0.03);
  const minForegroundRatio = parseNumber(options.minForegroundRatio, 0.01);
  const maxDarkFillRatio = parseNumber(options.maxDarkFillRatio, 0.002);
  const maxGreenFillRatio = parseNumber(options.maxGreenFillRatio, 1);
  const minFixtureColorRatio = parseNumber(options.minFixtureColorRatio, 0.065);
  const maxFixtureColorRatio = parseNumber(options.maxFixtureColorRatio, 0.105);
  const minSolidColorRatio = parseNumber(options.minSolidColorRatio, 0.92);
  const minHalfCompositeRatio = parseNumber(options.minHalfCompositeRatio, 0.92);
  const maxSolidBackgroundLeakRatio = parseNumber(options.maxSolidBackgroundLeakRatio, 0.02);
  const maxUnblendedFixtureRatio = parseNumber(options.maxUnblendedFixtureRatio, 0.1);
  const maxAlternateEpochRatio = parseNumber(options.maxAlternateEpochRatio, 0.002);
  const minCheckerRatio = parseNumber(options.minCheckerRatio, 0.42);
  const maxCheckerRatio = parseNumber(options.maxCheckerRatio, 0.58);
  const minMovingWidthRatio = parseNumber(options.minMovingWidthRatio, 0.22);
  const maxMovingWidthRatio = parseNumber(options.maxMovingWidthRatio, 0.28);
  const minMovingHeightRatio = parseNumber(options.minMovingHeightRatio, 0.30);
  const maxMovingHeightRatio = parseNumber(options.maxMovingHeightRatio, 0.36);
  const waitingBackgroundRatio = parseNumber(options.waitingBackgroundRatio, 0.98);
  result.minBackgroundRatio = minBackgroundRatio;
  result.minForegroundRatio = minForegroundRatio;
  result.maxDarkFillRatio = maxDarkFillRatio;
  result.maxGreenFillRatio = maxGreenFillRatio;
  result.minFixtureColorRatio = minFixtureColorRatio;
  result.maxFixtureColorRatio = maxFixtureColorRatio;
  result.minSolidColorRatio = minSolidColorRatio;
  result.minHalfCompositeRatio = minHalfCompositeRatio;
  result.maxSolidBackgroundLeakRatio = maxSolidBackgroundLeakRatio;
  result.maxUnblendedFixtureRatio = maxUnblendedFixtureRatio;
  result.maxAlternateEpochRatio = maxAlternateEpochRatio;
  result.minCheckerRatio = minCheckerRatio;
  result.maxCheckerRatio = maxCheckerRatio;
  result.waitingBackgroundRatio = waitingBackgroundRatio;
  result.markerWidthRatio = result.markerBoundingBox
    ? result.markerBoundingBox.width / result.width
    : 0;
  result.markerHeightRatio = result.markerBoundingBox
    ? result.markerBoundingBox.height / result.height
    : 0;
  result.classification = result.backgroundLikeRatio >= waitingBackgroundRatio
    ? "waiting-background"
    : "invalid-composite";
  const expectedEpochEvidence = pattern === "alpha-half"
    ? result.halfCompositeLikeRatio
    : result.fixtureColorLikeRatio;
  const alternateEpochEvidence = pattern === "alpha-half"
    ? result.alternateHalfCompositeLikeRatio
    : result.alternateFixtureColorLikeRatio;
  result.detectedVisualEpoch = expectedEpochEvidence > alternateEpochEvidence
    ? expectedVisualEpoch
    : alternateEpochEvidence > expectedEpochEvidence
      ? (expectedVisualEpoch === "post" ? "pre" : "post")
      : "unknown";

  if (pattern === "alpha-opaque") {
    result.ok =
      result.fixtureColorLikeRatio >= minSolidColorRatio &&
      result.backgroundLikeRatio <= maxSolidBackgroundLeakRatio &&
      result.alternateFixtureColorLikeRatio <= maxAlternateEpochRatio &&
      result.darkFillRatio <= maxDarkFillRatio &&
      result.greenFillRatio <= maxGreenFillRatio;
  } else if (pattern === "alpha-half") {
    result.ok =
      result.halfCompositeLikeRatio >= minHalfCompositeRatio &&
      result.backgroundLikeRatio <= maxSolidBackgroundLeakRatio &&
      result.fixtureColorLikeRatio <= maxUnblendedFixtureRatio &&
      result.alternateHalfCompositeLikeRatio <= maxAlternateEpochRatio &&
      result.darkFillRatio <= maxDarkFillRatio &&
      result.greenFillRatio <= maxGreenFillRatio;
  } else if (pattern === "alpha-checker") {
    result.ok =
      result.backgroundLikeRatio >= minCheckerRatio &&
      result.backgroundLikeRatio <= maxCheckerRatio &&
      result.fixtureColorLikeRatio >= minCheckerRatio &&
      result.fixtureColorLikeRatio <= maxCheckerRatio &&
      result.alternateFixtureColorLikeRatio <= maxAlternateEpochRatio &&
      result.darkFillRatio <= maxDarkFillRatio &&
      result.greenFillRatio <= maxGreenFillRatio;
  } else if (pattern === "alpha-moving-edge") {
    result.ok =
      result.fixtureColorLikeRatio >= minFixtureColorRatio &&
      result.fixtureColorLikeRatio <= maxFixtureColorRatio &&
      result.alternateFixtureColorLikeRatio <= maxAlternateEpochRatio &&
      !!result.markerBoundingBox &&
      result.markerWidthRatio >= minMovingWidthRatio &&
      result.markerWidthRatio <= maxMovingWidthRatio &&
      result.markerHeightRatio >= minMovingHeightRatio &&
      result.markerHeightRatio <= maxMovingHeightRatio &&
      result.darkFillRatio <= maxDarkFillRatio &&
      result.greenFillRatio <= maxGreenFillRatio;
  } else {
    result.ok =
      result.backgroundLikeRatio >= minBackgroundRatio &&
      result.foregroundLikeRatio >= minForegroundRatio &&
      result.darkFillRatio <= maxDarkFillRatio &&
      result.greenFillRatio <= maxGreenFillRatio;
  }
  if (result.ok) {
    result.classification = "valid-composite";
  }
  if (!result.ok) {
    let reason = "";
    if (pattern === "alpha-opaque" && result.fixtureColorLikeRatio < minSolidColorRatio) {
      reason = `opaque fixture color coverage too small (${result.fixtureColorLikeRatio.toFixed(4)} < ${minSolidColorRatio})`;
    } else if (pattern === "alpha-half" && result.halfCompositeLikeRatio < minHalfCompositeRatio) {
      reason = `50% alpha composite coverage too small (${result.halfCompositeLikeRatio.toFixed(4)} < ${minHalfCompositeRatio})`;
    } else if (
      (pattern === "alpha-opaque" || pattern === "alpha-half") &&
      result.backgroundLikeRatio > maxSolidBackgroundLeakRatio
    ) {
      reason = `solid fixture leaked background (${result.backgroundLikeRatio.toFixed(4)} > ${maxSolidBackgroundLeakRatio})`;
    } else if (pattern === "alpha-half" && result.fixtureColorLikeRatio > maxUnblendedFixtureRatio) {
      reason = `50% alpha fixture remained unblended (${result.fixtureColorLikeRatio.toFixed(4)} > ${maxUnblendedFixtureRatio})`;
    } else if (result.classification === "waiting-background") {
      reason = `background-only frame is still waiting for decoded media (${result.backgroundLikeRatio.toFixed(4)} >= ${waitingBackgroundRatio})`;
    } else if (alternateEpochEvidence > maxAlternateEpochRatio) {
      reason = `stale ${result.detectedVisualEpoch} visual epoch was observed (${alternateEpochEvidence.toFixed(4)} > ${maxAlternateEpochRatio})`;
    } else if (pattern === "alpha-moving-edge" && result.fixtureColorLikeRatio < minFixtureColorRatio) {
      reason = `moving marker color coverage too small (${result.fixtureColorLikeRatio.toFixed(4)} < ${minFixtureColorRatio})`;
    } else if (pattern === "alpha-moving-edge" && result.fixtureColorLikeRatio > maxFixtureColorRatio) {
      reason = `moving marker color coverage too large (${result.fixtureColorLikeRatio.toFixed(4)} > ${maxFixtureColorRatio})`;
    } else if (pattern === "alpha-moving-edge" &&
      (result.markerWidthRatio < minMovingWidthRatio || result.markerWidthRatio > maxMovingWidthRatio ||
       result.markerHeightRatio < minMovingHeightRatio || result.markerHeightRatio > maxMovingHeightRatio)) {
      reason = `moving marker geometry was ${result.markerWidthRatio.toFixed(4)}x${result.markerHeightRatio.toFixed(4)}; expected approximately 0.25x0.33`;
    } else if (pattern === "alpha-checker" &&
      (result.backgroundLikeRatio < minCheckerRatio || result.backgroundLikeRatio > maxCheckerRatio ||
       result.fixtureColorLikeRatio < minCheckerRatio || result.fixtureColorLikeRatio > maxCheckerRatio)) {
      reason = `checker coverage was background=${result.backgroundLikeRatio.toFixed(4)} foreground=${result.fixtureColorLikeRatio.toFixed(4)}; expected near 50/50`;
    } else if (result.backgroundLikeRatio < minBackgroundRatio && pattern !== "alpha-opaque" && pattern !== "alpha-half") {
      reason = `transparent/background area too small (${result.backgroundLikeRatio.toFixed(4)} < ${minBackgroundRatio})`;
    } else if (result.foregroundLikeRatio < minForegroundRatio && pattern !== "alpha-opaque" && pattern !== "alpha-half") {
      reason = `foreground area too small (${result.foregroundLikeRatio.toFixed(4)} < ${minForegroundRatio})`;
    } else if (result.darkFillRatio > maxDarkFillRatio) {
      reason = `dark fill too large (${result.darkFillRatio.toFixed(4)} > ${maxDarkFillRatio})`;
    } else {
      reason = `green fill too large (${result.greenFillRatio.toFixed(4)} > ${maxGreenFillRatio})`;
    }
    result.failureReason = reason;
    if (options.throwOnFailure !== false) {
      throw new Error(`Alpha composite pixel check failed: ${reason}`);
    }
  }
  return result;
}

function analyzeAlphaCompositeSequence(samples, options = {}) {
  const pattern = normalizeAlphaPattern(options.pattern);
  const expectedVisualEpoch = String(options.expectedVisualEpoch || "pre").toLowerCase();
  const requiredSampleCount = Math.max(1, Math.trunc(parseNumber(
    options.requiredUsefulSampleCount === undefined
      ? options.requiredSampleCount
      : options.requiredUsefulSampleCount,
    1
  )));
  const waitingSamples = samples.filter((sample) => sample && sample.classification === "waiting-background");
  const usefulSamples = samples.filter((sample) => sample && sample.classification !== "waiting-background");
  const screenshotHashes = usefulSamples
    .map((sample) => sample && sample.screenshot && sample.screenshot.sha256)
    .filter((value) => typeof value === "string" && value.length > 0);
  const compositePixelHashes = usefulSamples
    .map((sample) => sample && sample.compositePixelSha256)
    .filter((value) => typeof value === "string" && value.length > 0);
  const uniqueScreenshotHashes = [...new Set(screenshotHashes)];
  const uniqueCompositePixelHashes = [...new Set(compositePixelHashes)];
  const screenshotEvidencePaths = usefulSamples
    .map((sample) => sample && sample.screenshot && sample.screenshot.outputPath)
    .filter((value) => typeof value === "string" && value.length > 0);
  const markerBoxes = usefulSamples.map((sample) => sample && sample.markerBoundingBox).filter(Boolean);
  const result = {
    ok: false,
    pattern,
    expectedVisualEpoch,
    requiredSampleCount,
    observedSampleCount: samples.length,
    usefulSampleCount: usefulSamples.length,
    waitingSampleCount: waitingSamples.length,
    uniqueScreenshotHashes,
    uniqueScreenshotCount: uniqueScreenshotHashes.length,
    uniqueCompositePixelHashes,
    uniqueCompositePixelCount: uniqueCompositePixelHashes.length,
    screenshotEvidencePaths,
    markerBoxes,
    failureReasons: [],
  };

  if (usefulSamples.length < requiredSampleCount) {
    result.failureReasons.push(`useful sample count ${usefulSamples.length} is below required ${requiredSampleCount}`);
  }
  if (usefulSamples.some((sample) => !sample.ok)) {
    result.failureReasons.push("one or more non-background pixel samples failed exact composite validation");
  }
  if (screenshotHashes.length !== usefulSamples.length) {
    result.failureReasons.push(
      `PNG evidence hashes were present for only ${screenshotHashes.length}/${usefulSamples.length} useful samples`
    );
  }
  if (screenshotEvidencePaths.length !== usefulSamples.length) {
    result.failureReasons.push(
      `screenshot evidence paths were present for only ${screenshotEvidencePaths.length}/${usefulSamples.length} useful samples`
    );
  }
  if (options.requireEvidenceFiles === true) {
    const evidenceFailures = usefulSamples.flatMap((sample) => {
      const evidencePath = sample && sample.screenshot && sample.screenshot.outputPath;
      const declaredHash = sample && sample.screenshot && sample.screenshot.sha256;
      if (!evidencePath || !fs.existsSync(evidencePath)) {
        return [`sample ${sample && sample.sample} screenshot evidence file is missing`];
      }
      const actualHash = crypto.createHash("sha256").update(fs.readFileSync(evidencePath)).digest("hex");
      return actualHash === declaredHash
        ? []
        : [`sample ${sample && sample.sample} screenshot evidence hash does not match its file`];
    });
    result.evidenceFilesVerified = evidenceFailures.length === 0;
    result.failureReasons.push(...evidenceFailures);
  }
  if (compositePixelHashes.length !== usefulSamples.length) {
    result.failureReasons.push(
      `decoded composite pixel hashes were present for only ${compositePixelHashes.length}/${usefulSamples.length} useful samples`
    );
  }
  const firstUsefulSample = usefulSamples[0] || null;
  result.firstUsefulSampleIndex = firstUsefulSample ? firstUsefulSample.sample : null;
  result.firstUsefulSampleValid = !!firstUsefulSample && firstUsefulSample.ok === true;
  if (!firstUsefulSample) {
    result.failureReasons.push("no non-background frame was observed");
  } else if (!firstUsefulSample.ok) {
    result.failureReasons.push("the first non-background frame failed exact composite validation");
  }
  if (usefulSamples.some((sample) => sample.detectedVisualEpoch !== expectedVisualEpoch)) {
    result.failureReasons.push(`one or more useful frames did not carry the expected '${expectedVisualEpoch}' visual epoch`);
  }
  if (usefulSamples.some((sample) => sample.connectionEpoch !== expectedVisualEpoch)) {
    result.failureReasons.push(
      `one or more useful frames were not captured on the expected '${expectedVisualEpoch}' connection epoch`
    );
  }

  if (pattern === "alpha-moving-edge") {
    const minimumUnique = requiredSampleCount;
    result.minimumUniqueScreenshotCount = minimumUnique;
    if (uniqueCompositePixelHashes.length < minimumUnique) {
      result.failureReasons.push(
        `moving fixture produced only ${uniqueCompositePixelHashes.length} unique decoded frames; need ${minimumUnique}`
      );
    }
    if (uniqueScreenshotHashes.length < minimumUnique) {
      result.failureReasons.push(
        `moving fixture produced only ${uniqueScreenshotHashes.length} unique PNG evidence hashes; need ${minimumUnique}`
      );
    }
    if (markerBoxes.length !== usefulSamples.length) {
      result.failureReasons.push(`moving marker was detected in only ${markerBoxes.length}/${usefulSamples.length} useful samples`);
    }
    if (markerBoxes.length > 1) {
      const centers = markerBoxes.map((box) => box.centerX);
      const widths = markerBoxes.map((box) => box.width);
      const heights = markerBoxes.map((box) => box.height);
      const centerSpan = Math.max(...centers) - Math.min(...centers);
      const medianWidth = median(widths);
      const medianHeight = median(heights);
      const maxWidthDeviation = Math.max(...widths.map((value) => Math.abs(value - medianWidth))) /
        Math.max(1, medianWidth);
      const maxHeightDeviation = Math.max(...heights.map((value) => Math.abs(value - medianHeight))) /
        Math.max(1, medianHeight);
      result.markerCenterSpan = centerSpan;
      result.minimumMarkerCenterSpan = Math.max(24, usefulSamples[0].width * 0.12);
      result.medianMarkerWidth = medianWidth;
      result.medianMarkerHeight = medianHeight;
      result.maxMarkerWidthDeviation = maxWidthDeviation;
      result.maxMarkerHeightDeviation = maxHeightDeviation;
      if (centerSpan < result.minimumMarkerCenterSpan) {
        result.failureReasons.push(
          `moving marker center span ${centerSpan.toFixed(1)} is below ${result.minimumMarkerCenterSpan.toFixed(1)}`
        );
      }
      if (maxWidthDeviation > 0.35 || maxHeightDeviation > 0.35) {
        result.failureReasons.push(
          `moving marker dimensions were unstable (width=${maxWidthDeviation.toFixed(3)}, height=${maxHeightDeviation.toFixed(3)})`
        );
      }
    }
  } else if (["alpha-checker", "alpha-opaque", "alpha-half"].includes(pattern)) {
    if (uniqueCompositePixelHashes.length !== 1) {
      result.failureReasons.push(
        `static fixture produced ${uniqueCompositePixelHashes.length} unique decoded frames; expected exactly 1`
      );
    }
  }

  result.ok = result.failureReasons.length === 0;
  return result;
}

function analyzeAlphaTransition(preSamples, postSamples, options = {}) {
  const pattern = normalizeAlphaPattern(options.pattern);
  const requiredUsefulSampleCount = Math.max(
    1,
    Math.trunc(parseNumber(options.requiredUsefulSampleCount, pattern === "alpha-moving-edge" ? 10 : 4))
  );
  const pre = analyzeAlphaCompositeSequence(preSamples, {
    ...options,
    pattern,
    expectedVisualEpoch: "pre",
    requiredUsefulSampleCount,
  });
  const post = analyzeAlphaCompositeSequence(postSamples, {
    ...options,
    pattern,
    expectedVisualEpoch: "post",
    requiredUsefulSampleCount,
  });
  const preHashes = new Set(pre.uniqueCompositePixelHashes);
  const postUseful = postSamples.filter(
    (sample) => sample && sample.classification !== "waiting-background"
  );
  const firstPost = postUseful[0] || null;
  const firstPostHash = firstPost && firstPost.compositePixelSha256;
  const failureReasons = [];
  if (!pre.ok) {
    failureReasons.push(`pre-transition sequence failed: ${pre.failureReasons.join("; ")}`);
  }
  if (!post.ok) {
    failureReasons.push(`post-transition sequence failed: ${post.failureReasons.join("; ")}`);
  }
  if (!firstPostHash) {
    failureReasons.push("first useful post-transition frame has no decoded pixel hash");
  } else if (preHashes.has(firstPostHash)) {
    failureReasons.push("first useful post-transition frame reused a pre-transition decoded pixel hash");
  }
  if (firstPost && firstPost.detectedVisualEpoch !== "post") {
    failureReasons.push("first useful post-transition frame carried a stale pre-transition visual epoch");
  }
  if (firstPost && firstPost.connectionEpoch !== "post") {
    failureReasons.push("first useful post-transition frame was not captured on the new connection epoch");
  }
  return {
    ok: failureReasons.length === 0,
    pattern,
    requiredUsefulSampleCount,
    pre,
    post,
    firstPostSampleIndex: firstPost ? firstPost.sample : null,
    firstPostDecodedPixelSha256: firstPostHash || null,
    firstPostConnectionEpoch: firstPost ? firstPost.connectionEpoch || null : null,
    firstPostHashWasSeenPreTransition: !!firstPostHash && preHashes.has(firstPostHash),
    failureReasons,
  };
}

function analyzeAlphaCaptureCadence(samples, options = {}) {
  const inputCreatedAtMs = Number(options.inputCreatedAtMs);
  const requiredMaximumMs = Math.max(1, Math.trunc(parseNumber(options.requiredMaximumMs, 100)));
  const absoluteMaximumMs = Math.max(
    requiredMaximumMs,
    Math.trunc(parseNumber(options.absoluteMaximumMs, requiredMaximumMs + 50))
  );
  const captureStartTimes = samples
    .map((sample) => sample && sample.screenshot && sample.screenshot.captureStartedAtMs)
    .filter((value) => Number.isFinite(value));
  const captureStartGapsMs = captureStartTimes.slice(1).map(
    (value, index) => value - captureStartTimes[index]
  );
  const firstCaptureLatencyMs = captureStartTimes.length && Number.isFinite(inputCreatedAtMs)
    ? captureStartTimes[0] - inputCreatedAtMs
    : null;
  const maxCaptureStartGapMs = captureStartGapsMs.length ? Math.max(...captureStartGapsMs) : null;
  const gapsOverRequiredMaximum = captureStartGapsMs.filter((gap) => gap > requiredMaximumMs);
  const allowedJitterGapCount = captureStartGapsMs.length
    ? Math.max(1, Math.floor(captureStartGapsMs.length * 0.05))
    : 0;
  const failureReasons = [];
  if (captureStartTimes.length !== samples.length) {
    failureReasons.push(`capture-start timestamps were present for only ${captureStartTimes.length}/${samples.length} samples`);
  }
  if (firstCaptureLatencyMs === null || firstCaptureLatencyMs < 0 || firstCaptureLatencyMs > requiredMaximumMs) {
    failureReasons.push(
      `first capture latency ${firstCaptureLatencyMs}ms was outside 0..${requiredMaximumMs}ms`
    );
  }
  if (captureStartGapsMs.some((gap) => gap <= 0)) {
    failureReasons.push("capture-start timestamps were not strictly increasing");
  }
  if (maxCaptureStartGapMs !== null && maxCaptureStartGapMs > absoluteMaximumMs) {
    failureReasons.push(
      `maximum capture-start gap ${maxCaptureStartGapMs}ms exceeded absolute ${absoluteMaximumMs}ms limit`
    );
  }
  if (gapsOverRequiredMaximum.length > allowedJitterGapCount) {
    failureReasons.push(
      `${gapsOverRequiredMaximum.length} capture-start gaps exceeded ${requiredMaximumMs}ms; ` +
        `allowed ${allowedJitterGapCount} scheduling-jitter outlier(s)`
    );
  }
  return {
    ok: failureReasons.length === 0,
    requiredMaximumMs,
    absoluteMaximumMs,
    allowedJitterGapCount,
    gapsOverRequiredMaximumMs: gapsOverRequiredMaximum,
    inputCreatedAtMs: Number.isFinite(inputCreatedAtMs) ? inputCreatedAtMs : null,
    firstCaptureStartedAtMs: captureStartTimes[0] || null,
    firstCaptureLatencyMs,
    maxCaptureStartGapMs,
    captureStartTimes,
    captureStartGapsMs,
    failureReasons,
  };
}

function analyzeAlphaComposite(backgroundPngPath, finalPngPath, options = {}) {
  const background = decodePng(fs.readFileSync(backgroundPngPath));
  const final = decodePng(fs.readFileSync(finalPngPath));
  return analyzeAlphaCompositeImages(background, final, {
    ...options,
    backgroundPngPath,
    finalPngPath,
  });
}

function startPerturbCommand(command) {
  if (!command) {
    return null;
  }
  logStep(`starting perturb command: ${command}`);
  const child = spawn(command, {
    cwd: process.cwd(),
    shell: true,
    windowsHide: true,
  });
  const chunks = { stdout: [], stderr: [] };
  child.stdout.on("data", (chunk) => chunks.stdout.push(Buffer.from(chunk)));
  child.stderr.on("data", (chunk) => chunks.stderr.push(Buffer.from(chunk)));
  const exitPromise = new Promise((resolve) => {
    child.on("exit", (code) => resolve(code));
  });
  return { child, command, chunks, startedAt: new Date().toISOString(), exitPromise };
}

async function waitForPerturbCommand(handle, timeoutMs) {
  if (!handle) {
    return null;
  }
  const { child, chunks } = handle;
  const result = {
    command: handle.command,
    startedAt: handle.startedAt,
    finishedAt: null,
    exitCode: null,
    timedOut: false,
    stdout: "",
    stderr: "",
  };

  await new Promise((resolve) => {
    const timeout = setTimeout(() => {
      result.timedOut = true;
      child.kill();
    }, timeoutMs);
    handle.exitPromise.then((code) => {
      clearTimeout(timeout);
      result.exitCode = code;
      result.finishedAt = new Date().toISOString();
      resolve();
    });
  });

  result.stdout = Buffer.concat(chunks.stdout).toString("utf8");
  result.stderr = Buffer.concat(chunks.stderr).toString("utf8");
  return result;
}

function createAudioMeterCollector(inputName) {
  const summary = {
    inputName,
    sampleCount: 0,
    nonSilentSampleCount: 0,
    highLevelSampleCount: 0,
    maxMagnitude: 0,
    maxLevel: 0,
    firstSeenAt: null,
    lastSeenAt: null,
  };

  function observeLevel(value) {
    if (!Number.isFinite(value)) {
      return;
    }

    const abs = Math.abs(value);
    if (abs > summary.maxMagnitude) {
      summary.maxMagnitude = abs;
    }
    if (abs > summary.maxLevel) {
      summary.maxLevel = abs;
    }
    if (abs > 0.0001) {
      summary.nonSilentSampleCount += 1;
    }
    if (abs >= 0.999) {
      summary.highLevelSampleCount += 1;
    }
  }

  return {
    summary,
    onEvent(event) {
      if (!event || event.eventType !== "InputVolumeMeters") {
        return;
      }

      const inputs = event.eventData && Array.isArray(event.eventData.inputs) ? event.eventData.inputs : [];
      const target = inputs.find((input) => input && input.inputName === inputName);
      if (!target || !Array.isArray(target.inputLevelsMul)) {
        return;
      }

      summary.sampleCount += 1;
      const now = new Date().toISOString();
      if (!summary.firstSeenAt) {
        summary.firstSeenAt = now;
      }
      summary.lastSeenAt = now;

      for (const channel of target.inputLevelsMul) {
        if (!Array.isArray(channel)) {
          continue;
        }
        for (const level of channel) {
          observeLevel(Number(level));
        }
      }
    },
  };
}

async function captureSourceScreenshot(client, sourceName, outputPath, options = {}) {
  const minScreenshotBytes = Number(
    options.minScreenshotBytes || process.env.VDONINJA_MIN_SCREENSHOT_BYTES || 10000
  );
  const imageWidth = clampInt(options.imageWidth, 1280, 2, 7680);
  const imageHeight = clampInt(options.imageHeight, 720, 2, 4320);
  const response = await client.request("GetSourceScreenshot", {
    sourceName,
    imageFormat: "png",
    imageWidth,
    imageHeight,
    imageCompressionQuality: 0,
  });

  const imageData = response.imageData || "";
  const prefix = "data:image/png;base64,";
  if (!imageData.startsWith(prefix)) {
    throw new Error("OBS did not return a PNG screenshot");
  }

  const buffer = Buffer.from(imageData.slice(prefix.length), "base64");
  if (buffer.length < minScreenshotBytes) {
    throw new Error(`Screenshot was unexpectedly small (${buffer.length} bytes)`);
  }

  fs.mkdirSync(path.dirname(outputPath), { recursive: true });
  fs.writeFileSync(outputPath, buffer);

  return {
    outputPath,
    byteLength: buffer.length,
    sha256: crypto.createHash("sha256").update(buffer).digest("hex"),
  };
}

async function stretchSceneItemToCanvas(client, sceneName, sceneItemId, label) {
  if (sceneItemId === undefined || sceneItemId === null) {
    return;
  }

  const canvasWidth = clampInt(process.env.VDONINJA_CANVAS_WIDTH, 1280, 2, 7680);
  const canvasHeight = clampInt(process.env.VDONINJA_CANVAS_HEIGHT, 720, 2, 4320);
  logStep(`stretching ${label} to ${canvasWidth}x${canvasHeight} canvas`);
  await client.request("SetSceneItemTransform", {
    sceneName,
    sceneItemId,
    sceneItemTransform: {
      positionX: 0,
      positionY: 0,
      rotation: 0,
      scaleX: 1,
      scaleY: 1,
      alignment: 5,
      boundsType: "OBS_BOUNDS_STRETCH",
      boundsAlignment: 5,
      boundsWidth: canvasWidth,
      boundsHeight: canvasHeight,
      cropLeft: 0,
      cropRight: 0,
      cropTop: 0,
      cropBottom: 0,
    },
  });
}

async function performAlphaTransition(options) {
  const {
    client,
    diagnosticsObserver,
    initialPeer,
    mode,
    label,
    command,
    timeoutMs,
    holdMs,
    epochFile,
    sceneName,
    sceneItemId,
    inputName,
    inputSettings,
  } = options;
  const result = {
    ok: false,
    mode,
    label,
    command: command || null,
    startedAt: new Date().toISOString(),
    finishedAt: null,
    epochFile: epochFile || null,
    epochWrittenAt: null,
    epochWrittenAtMs: null,
    sceneItemDisabledAt: null,
    sceneItemDisabledAtMs: null,
    sceneItemEnabledAt: null,
    sceneItemEnabledAtMs: null,
    sourceRemovedAt: null,
    sourceRemovedAtMs: null,
    sourceRecreatedAt: null,
    sourceRecreatedAtMs: null,
    commandResult: null,
    controlRequest: null,
    observedTransition: null,
    error: null,
  };

  try {
    if (!diagnosticsObserver) {
      throw new Error("Alpha transitions require observed Game Capture /diagnostics evidence");
    }
    const writePostEpoch = () => {
      if (!epochFile) {
        return;
      }
      fs.mkdirSync(path.dirname(path.resolve(epochFile)), { recursive: true });
      fs.writeFileSync(epochFile, "post\n");
      result.epochWrittenAtMs = Date.now();
      result.epochWrittenAt = new Date().toISOString();
    };

    const before = await diagnosticsObserver.waitFor(
      (snapshot) => snapshot.peer && snapshot.peer.connected &&
        (!initialPeer || sameTransportIdentity(snapshot.peer, initialPeer)),
      timeoutMs,
      "connected-before-transition"
    );
    result.beforeObservedAtMs = before.observedAtMs;
    const waitForOldTransportRetirement = (sinceMs) => diagnosticsObserver.waitFor(
      (snapshot) => snapshot.observedAtMs >= sinceMs &&
        snapshotShowsTransportRetiredOrAbsent(snapshot, before.peer),
      timeoutMs,
      "old-transport-retired",
      sinceMs
    );
    let retired;
    let after;

    if (mode === "source-toggle") {
      await client.request("SetSceneItemEnabled", {
        sceneName,
        sceneItemId,
        sceneItemEnabled: false,
      });
      result.sceneItemDisabledAtMs = Date.now();
      result.sceneItemDisabledAt = new Date().toISOString();
      retired = await waitForOldTransportRetirement(result.sceneItemDisabledAtMs);
      writePostEpoch();
      await sleep(holdMs);
      await client.request("SetSceneItemEnabled", {
        sceneName,
        sceneItemId,
        sceneItemEnabled: true,
      });
      result.sceneItemEnabledAtMs = Date.now();
      result.sceneItemEnabledAt = new Date().toISOString();
      after = await diagnosticsObserver.waitFor(
        (snapshot) => snapshot.observedAtMs >= result.sceneItemEnabledAtMs &&
          snapshot.publisherPid === before.publisherPid &&
          snapshot.peer && snapshot.peer.connected &&
          !sameLogicalPeer(snapshot.peer, before.peer),
        timeoutMs,
        "distinct-peer-connected-after-source-toggle",
        result.sceneItemEnabledAtMs
      );
    } else if (mode === "source-recreate") {
      await client.request("RemoveInput", { inputName });
      result.sourceRemovedAtMs = Date.now();
      result.sourceRemovedAt = new Date().toISOString();
      retired = await waitForOldTransportRetirement(result.sourceRemovedAtMs);
      writePostEpoch();
      await sleep(holdMs);
      const recreated = await client.request("CreateInput", {
        sceneName,
        inputName,
        inputKind: "vdoninja_source",
        inputSettings,
        sceneItemEnabled: true,
      });
      await stretchSceneItemToCanvas(client, sceneName, recreated.sceneItemId, inputName);
      result.sceneItemId = recreated.sceneItemId;
      result.sourceRecreatedAtMs = Date.now();
      result.sourceRecreatedAt = new Date().toISOString();
      after = await diagnosticsObserver.waitFor(
        (snapshot) => snapshot.observedAtMs >= result.sourceRecreatedAtMs &&
          snapshot.publisherPid === before.publisherPid &&
          snapshot.peer && snapshot.peer.connected &&
          !sameLogicalPeer(snapshot.peer, before.peer),
        timeoutMs,
        "distinct-peer-connected-after-source-recreate",
        result.sourceRecreatedAtMs
      );
    } else if (mode === "same-peer-ice-rebuild") {
      result.controlRequest = await diagnosticsObserver.postControlCommand(
        { command: "refresh_peer_transports" },
        "same-peer-ice-rebuild"
      );
      if (!result.controlRequest.response || result.controlRequest.response.ok !== true ||
          Number(result.controlRequest.response.accepted_peer_count) < 1) {
        throw new Error("Authenticated refresh_peer_transports command did not accept an active peer");
      }
      retired = await waitForOldTransportRetirement(result.controlRequest.requestedAtMs);
      writePostEpoch();
      await sleep(holdMs);
      after = await diagnosticsObserver.waitFor(
        (snapshot) => snapshot.observedAtMs >= retired.observedAtMs &&
          snapshot.publisherPid === before.publisherPid &&
          snapshot.peer && snapshot.peer.connected &&
          sameLogicalPeer(snapshot.peer, before.peer) &&
          snapshot.peer.clientTransportGeneration > before.peer.clientTransportGeneration &&
          snapshot.peer.activeTransportGeneration > before.peer.activeTransportGeneration,
        timeoutMs,
        "same-logical-peer-connected-on-higher-transport-generation",
        retired.observedAtMs
      );
    } else if (["command", "publisher-restart"].includes(mode)) {
      if (!command) {
        throw new Error(`Alpha transition mode '${mode}' requires VDONINJA_ALPHA_TRANSITION_COMMAND`);
      }
      const handle = startPerturbCommand(command);
      const commandStartedAtMs = Date.parse(handle.startedAt);
      result.commandResult = await waitForPerturbCommand(handle, timeoutMs);
      if (!result.commandResult || result.commandResult.timedOut || result.commandResult.exitCode !== 0) {
        throw new Error(
          `transition command failed exit=${result.commandResult && result.commandResult.exitCode} ` +
          `timedOut=${result.commandResult && result.commandResult.timedOut}`
        );
      }
      retired = await waitForOldTransportRetirement(commandStartedAtMs);
      writePostEpoch();
      after = await diagnosticsObserver.waitFor(
        (snapshot) => snapshot.observedAtMs >= retired.observedAtMs &&
          snapshot.peer && snapshot.peer.connected &&
          (mode === "publisher-restart"
            ? snapshot.publisherPid !== before.publisherPid
            : !sameLogicalPeer(snapshot.peer, before.peer)),
        timeoutMs,
        mode === "publisher-restart"
          ? "new-publisher-peer-connected"
          : "distinct-peer-connected-after-command",
        retired.observedAtMs
      );
    } else {
      throw new Error(`Unsupported alpha transition mode '${mode}'`);
    }
    const evaluatedMode = mode === "command" ? "source-recreate" : mode;
    result.observedTransition = evaluateObservedTransition(evaluatedMode, before, retired, after);
    result.ok = result.observedTransition.ok;
    if (!result.ok) {
      throw new Error(`Observed diagnostics transition was invalid: ${result.observedTransition.failureReasons.join("; ")}`);
    }
  } catch (error) {
    result.error = String(error && error.message ? error.message : error);
  }
  result.finishedAt = new Date().toISOString();
  return result;
}

async function main() {
  const mode = (process.env.VDONINJA_SOURCE_MODE || process.argv[2] || "browser").trim().toLowerCase();
  const streamId = process.env.VDONINJA_STREAM_ID || process.argv[3];
  const password = process.env.VDONINJA_PASSWORD || process.argv[4] || "";
  const roomId = process.env.VDONINJA_ROOM_ID || process.argv[5] || "";
  const waitMs = Number(process.env.VDONINJA_WAIT_MS || 30000);
  const websocketUrl = process.env.OBS_WEBSOCKET_URL || "ws://127.0.0.1:4455";
  const useNativeReceiver = mode === "native";
  const keepScene = parseBoolean(process.env.VDONINJA_KEEP_SCENE, false);
  const skipCapture = parseBoolean(process.env.VDONINJA_SKIP_CAPTURE, mode === "browser");
  const alphaPixelCheckEnabled = parseBoolean(process.env.VDONINJA_ALPHA_PIXEL_CHECK, false);
  const alphaPattern = normalizeAlphaPattern(process.env.VDONINJA_ALPHA_PATTERN);
  const requiredAlphaSampleCount = alphaPixelCheckEnabled
    ? alphaPattern === "alpha-moving-edge"
      ? 10
      : ["alpha-checker", "alpha-opaque", "alpha-half"].includes(alphaPattern)
        ? 4
        : 3
    : 1;
  const requestedAlphaSampleCount = clampInt(
    process.env.VDONINJA_ALPHA_SAMPLE_COUNT,
    requiredAlphaSampleCount,
    1,
    100
  );
  const alphaSampleCount = Math.max(requiredAlphaSampleCount, requestedAlphaSampleCount);
  const requiredAlphaSampleIntervalMs = alphaPixelCheckEnabled ? 100 : 1000;
  const requestedAlphaSampleIntervalMs = clampInt(
    process.env.VDONINJA_ALPHA_SAMPLE_INTERVAL_MS,
    alphaPixelCheckEnabled ? 75 : 1000,
    25,
    5000
  );
  const alphaSampleIntervalMs = alphaPixelCheckEnabled
    ? Math.min(requiredAlphaSampleIntervalMs, requestedAlphaSampleIntervalMs)
    : requestedAlphaSampleIntervalMs;
  const alphaTransitionCommand = process.env.VDONINJA_ALPHA_TRANSITION_COMMAND || "";
  const alphaTransitionMode = String(
    process.env.VDONINJA_ALPHA_TRANSITION_MODE || (alphaPixelCheckEnabled ? "source-toggle" : "none")
  ).trim().toLowerCase();
  const alphaTransitionLabel =
    String(process.env.VDONINJA_ALPHA_TRANSITION_LABEL || "transition").trim() || "transition";
  const alphaTransitionEnabled = alphaPixelCheckEnabled && alphaTransitionMode !== "none";
  const alphaTransitionAfterUsefulSamples = alphaTransitionEnabled
    ? Math.max(
        requiredAlphaSampleCount,
        clampInt(process.env.VDONINJA_ALPHA_TRANSITION_AFTER_SAMPLE, alphaSampleCount, 1, 100)
      )
    : null;
  const alphaTransitionHoldMs = clampInt(
    process.env.VDONINJA_ALPHA_TRANSITION_HOLD_MS,
    350,
    100,
    5000
  );
  const alphaTransitionTimeoutMs = clampInt(
    process.env.VDONINJA_ALPHA_TRANSITION_TIMEOUT_MS,
    30000,
    1000,
    120000
  );
  const alphaBackgroundColor = Number(process.env.VDONINJA_ALPHA_BACKGROUND_COLOR || 0xffff00ff);
  const alphaEpochFile = String(process.env.VDONINJA_ALPHA_EPOCH_FILE || "").trim();
  const gameCaptureControlDiscovery = String(
    process.env.VDONINJA_GAME_CAPTURE_CONTROL_DISCOVERY || ""
  ).trim();
  const alphaMaxCaptureAttempts = clampInt(
    process.env.VDONINJA_ALPHA_MAX_CAPTURE_ATTEMPTS,
    600,
    20,
    2000
  );
  const resultJsonPath = process.env.VDONINJA_SOURCE_CHECK_RESULT_JSON || "";
  const perturbCommand = process.env.VDONINJA_DURING_WAIT_COMMAND || "";
  const requirePerturbCommand = parseBoolean(process.env.VDONINJA_REQUIRE_PERTURB_COMMAND, false);
  const perturbTimeoutMs = Number(process.env.VDONINJA_PERTURB_TIMEOUT_MS || Math.max(waitMs + 15000, 30000));
  const checkAudioMeter = parseBoolean(process.env.VDONINJA_CHECK_AUDIO_METER, false);
  const failOnAudioClip = parseBoolean(process.env.VDONINJA_FAIL_ON_AUDIO_CLIP, false);
  const minAudioMeterSamples = Number(process.env.VDONINJA_MIN_AUDIO_METER_SAMPLES || 3);
  const sourceWidth = clampInt(process.env.VDONINJA_SOURCE_WIDTH, 1280, 320, 4096);
  const sourceHeight = clampInt(process.env.VDONINJA_SOURCE_HEIGHT, 720, 240, 2160);
  const canvasWidth = clampInt(process.env.VDONINJA_CANVAS_WIDTH, 1280, 2, 7680);
  const canvasHeight = clampInt(process.env.VDONINJA_CANVAS_HEIGHT, 720, 2, 4320);
  const alphaCaptureWidth = clampInt(
    process.env.VDONINJA_ALPHA_CAPTURE_WIDTH,
    Math.min(canvasWidth, 640),
    160,
    canvasWidth
  );
  const alphaCaptureHeight = clampInt(
    process.env.VDONINJA_ALPHA_CAPTURE_HEIGHT,
    Math.min(canvasHeight, 360),
    90,
    canvasHeight
  );

  if (!streamId) {
    throw new Error("Usage: node scripts/obs-websocket-vdoninja-source-check.cjs <browser|native> <streamId> [password] [roomId]");
  }
  if (!["none", "source-toggle", "source-recreate", "command", "same-peer-ice-rebuild", "publisher-restart"].includes(alphaTransitionMode)) {
    throw new Error(`Unsupported VDONINJA_ALPHA_TRANSITION_MODE '${alphaTransitionMode}'`);
  }
  if (alphaPixelCheckEnabled && !gameCaptureControlDiscovery) {
    throw new Error(
      "Alpha signal-chain validation requires VDONINJA_GAME_CAPTURE_CONTROL_DISCOVERY for observed /diagnostics evidence"
    );
  }

  const stamp = Date.now();
  const sceneName = `Codex Source Smoke ${stamp}`;
  const inputName = `Codex VDO Source ${mode} ${stamp}`;
  const audioMeterCollector = checkAudioMeter ? createAudioMeterCollector(inputName) : null;
  const client = new ObsWebSocketClient(websocketUrl, {
    eventSubscriptions: checkAudioMeter ? EVENT_SUBSCRIPTION_INPUT_VOLUME_METERS : 0,
    onEvent: audioMeterCollector ? audioMeterCollector.onEvent : null,
  });
  const diagnosticsObserver = alphaPixelCheckEnabled
    ? new GameCaptureDiagnosticsObserver(gameCaptureControlDiscovery, streamId)
    : null;
  const screenshotPath = path.resolve(
    process.cwd(),
    "artifacts",
    `obs-source-${mode}-${stamp}.png`
  );

  let previousSceneName = null;
  let targetSceneName = null;
  let createdScene = false;

  try {
    if (diagnosticsObserver) {
      diagnosticsObserver.start();
    }
    logStep(`connecting to ${websocketUrl}`);
    await client.connect();
    logStep("requesting OBS version");
    const version = await client.request("GetVersion");
    logStep("requesting input kinds");
    const kinds = await client.request("GetInputKindList", { unversioned: false });
    if (!Array.isArray(kinds.inputKinds) || !kinds.inputKinds.includes("vdoninja_source")) {
      throw new Error("OBS does not have the vdoninja_source input kind registered");
    }
    const colorSourceKind = selectColorSourceKind(kinds.inputKinds);
    if (alphaPixelCheckEnabled && !colorSourceKind) {
      throw new Error("OBS does not expose a color source input kind for alpha pixel checks");
    }

    logStep("reading current program scene");
    const currentProgram = await client.request("GetCurrentProgramScene").catch(() => ({}));
    previousSceneName = currentProgram.currentProgramSceneName || null;

    targetSceneName = sceneName;
    createdScene = true;
    logStep(`creating scene ${sceneName}`);
    await client.request("CreateScene", { sceneName });
    logStep(`switching to scene ${sceneName}`);
    await client.request("SetCurrentProgramScene", { sceneName });

    let backgroundInputName = null;
    let backgroundScreenshot = null;
    if (alphaPixelCheckEnabled) {
      backgroundInputName = `Codex Alpha Background ${stamp}`;
      logStep(`creating alpha-check background ${backgroundInputName}`);
      const backgroundInput = await client.request("CreateInput", {
        sceneName: targetSceneName,
        inputName: backgroundInputName,
        inputKind: colorSourceKind,
        inputSettings: {
          width: canvasWidth,
          height: canvasHeight,
          color: alphaBackgroundColor,
        },
        sceneItemEnabled: true,
      });
      await stretchSceneItemToCanvas(
        client,
        targetSceneName,
        backgroundInput.sceneItemId,
        backgroundInputName
      );
      backgroundScreenshot = await captureSourceScreenshot(
        client,
        targetSceneName,
        path.resolve(process.cwd(), "artifacts", `obs-source-${mode}-background-${stamp}.png`),
        {
          minScreenshotBytes: 1000,
          imageWidth: alphaCaptureWidth,
          imageHeight: alphaCaptureHeight,
        }
      );
    }

    const nativeInputSettings = {
      stream_id: streamId,
      password,
      room_id: roomId,
      use_native_receiver: useNativeReceiver,
      enable_data_channel: true,
      auto_reconnect: true,
      width: sourceWidth,
      height: sourceHeight,
    };
    if (alphaPixelCheckEnabled && alphaTransitionEnabled && !alphaEpochFile) {
      throw new Error(
        "A transition-safe alpha run requires VDONINJA_ALPHA_EPOCH_FILE so stale pre-transition video cannot pass"
      );
    }
    if (alphaEpochFile) {
      fs.mkdirSync(path.dirname(path.resolve(alphaEpochFile)), { recursive: true });
      fs.writeFileSync(alphaEpochFile, "pre\n");
    }

    logStep(`creating input ${inputName} (${sourceWidth}x${sourceHeight})`);
    const inputCreateRequestedAtMs = Date.now();
    let sourceInput = await client.request("CreateInput", {
      sceneName: targetSceneName,
      inputName,
      inputKind: "vdoninja_source",
      inputSettings: nativeInputSettings,
      sceneItemEnabled: true,
    });
    const inputCreatedAtMs = Date.now();
    // Source and canvas dimensions match in the packaged workflow. Dispatch
    // the transform without waiting so capture starts immediately and any
    // startup/stale frame is retained as evidence instead of hidden.
    let initialTransformError = null;
    const initialTransformPromise = stretchSceneItemToCanvas(
      client,
      targetSceneName,
      sourceInput.sceneItemId,
      inputName
    ).catch((error) => {
      initialTransformError = error;
    });

    const perturb = startPerturbCommand(perturbCommand);
    let screenshot = null;
    const screenshots = [];
    const alphaPixelSamples = [];
    let initialObservedPeer = null;
    let alphaTransitionResult = null;
    let alphaTransitionFinished = false;
    let alphaTransitionPromise = null;
    let perturbResult = null;
    if (!skipCapture) {
      if (!alphaPixelCheckEnabled) {
        logStep(`waiting ${waitMs}ms for source to render`);
        await sleep(waitMs);
        const captured = await captureSourceScreenshot(client, targetSceneName, screenshotPath, {
          imageWidth: canvasWidth,
          imageHeight: canvasHeight,
        });
        captured.sampleIndex = 1;
        captured.captureStartedAtMs = Date.now();
        captured.capturedAt = new Date().toISOString();
        captured.checkpoint = "steady";
        screenshots.push(captured);
      } else {
        const captureDeadlineMs = Date.now() + Math.max(
          60000,
          waitMs + alphaTransitionTimeoutMs + 30000
        );
        let checkpointPhase = "pre";
        let previousCaptureStartedAtMs = null;

        logStep(
          `starting immediate alpha capture at ${alphaSampleIntervalMs}ms cadence; ` +
          `need ${alphaSampleCount} useful samples per transition epoch`
        );
        for (let attempt = 1; attempt <= alphaMaxCaptureAttempts && Date.now() < captureDeadlineMs; attempt += 1) {
          if (alphaTransitionFinished && checkpointPhase === "transition") {
            checkpointPhase = "post";
            if (alphaTransitionResult && alphaTransitionResult.sceneItemId !== undefined) {
              sourceInput = { ...sourceInput, sceneItemId: alphaTransitionResult.sceneItemId };
            }
          }

          if (previousCaptureStartedAtMs !== null) {
            const dueAt = previousCaptureStartedAtMs + alphaSampleIntervalMs;
            if (Date.now() < dueAt) {
              await sleep(dueAt - Date.now());
            }
          }
          const captureStartedAtMs = Date.now();
          const diagnosticsAtCaptureStart = diagnosticsObserver
            ? diagnosticsObserver.latest()
            : null;
          if (!initialObservedPeer && diagnosticsAtCaptureStart &&
              diagnosticsAtCaptureStart.peer && diagnosticsAtCaptureStart.peer.connected) {
            initialObservedPeer = diagnosticsAtCaptureStart.peer;
          }
          const checkpoint = alphaTransitionEnabled
            ? `${checkpointPhase}:${alphaTransitionLabel}`
            : "steady";
          const expectedVisualEpoch = checkpointPhase === "pre" ? "pre" : "post";
          const connectionEpoch = classifyObservedConnection(
            diagnosticsAtCaptureStart,
            alphaTransitionResult && alphaTransitionResult.observedTransition,
            initialObservedPeer
          );
          const samplePath = attempt === 1
            ? screenshotPath
            : path.resolve(
                process.cwd(),
                "artifacts",
                `obs-source-${mode}-${stamp}-alpha-sample-${attempt}.png`
              );
          logStep(`capturing alpha screenshot attempt ${attempt} checkpoint=${checkpoint}`);

          let captured;
          let analysis;
          try {
            captured = await captureSourceScreenshot(client, targetSceneName, samplePath, {
              minScreenshotBytes: 1000,
              imageWidth: alphaCaptureWidth,
              imageHeight: alphaCaptureHeight,
            });
            captured.captureStartedAtMs = captureStartedAtMs;
            captured.captureCompletedAtMs = Date.now();
            captured.sampleIndex = attempt;
            captured.capturedAt = new Date().toISOString();
            captured.checkpoint = checkpoint;
            captured.expectedVisualEpoch = expectedVisualEpoch;
            captured.connectionEpoch = connectionEpoch;
            captured.diagnosticsAtCaptureStart = diagnosticsAtCaptureStart;
            analysis = analyzeAlphaComposite(backgroundScreenshot.outputPath, captured.outputPath, {
              pattern: alphaPattern,
              expectedVisualEpoch,
              tolerance: process.env.VDONINJA_ALPHA_TOLERANCE,
              fixtureColorTolerance: process.env.VDONINJA_ALPHA_FIXTURE_COLOR_TOLERANCE,
              halfCompositeTolerance: process.env.VDONINJA_ALPHA_HALF_COLOR_TOLERANCE,
              minBackgroundRatio: process.env.VDONINJA_ALPHA_MIN_BACKGROUND_RATIO,
              minForegroundRatio: process.env.VDONINJA_ALPHA_MIN_FOREGROUND_RATIO,
              maxDarkFillRatio: process.env.VDONINJA_ALPHA_MAX_DARK_RATIO,
              maxGreenFillRatio: process.env.VDONINJA_ALPHA_MAX_GREEN_RATIO,
              sampleStep: process.env.VDONINJA_ALPHA_SAMPLE_STEP,
              throwOnFailure: false,
            });
          } catch (error) {
            captured = {
              outputPath: samplePath,
              sha256: null,
              captureStartedAtMs,
              captureCompletedAtMs: Date.now(),
              sampleIndex: attempt,
              capturedAt: new Date().toISOString(),
              checkpoint,
              expectedVisualEpoch,
              connectionEpoch,
              diagnosticsAtCaptureStart,
              error: String(error && error.message ? error.message : error),
            };
            analysis = {
              ok: false,
              classification: "capture-error",
              expectedVisualEpoch,
              detectedVisualEpoch: "unknown",
              compositePixelSha256: null,
              failureReason: captured.error,
            };
          }
          screenshots.push(captured);
          alphaPixelSamples.push({
            sample: attempt,
            checkpoint,
            connectionEpoch,
            screenshot: captured,
            ...analysis,
          });
          previousCaptureStartedAtMs = captureStartedAtMs;

          const preUseful = alphaPixelSamples.filter(
            (sample) => sample.checkpoint === `pre:${alphaTransitionLabel}` &&
              sample.classification !== "waiting-background"
          ).length;
          const postUseful = alphaPixelSamples.filter(
            (sample) => sample.checkpoint === `post:${alphaTransitionLabel}` &&
              sample.classification !== "waiting-background"
          ).length;
          const steadyUseful = alphaPixelSamples.filter(
            (sample) => sample.checkpoint === "steady" && sample.classification !== "waiting-background"
          ).length;

          if (alphaTransitionEnabled && checkpointPhase === "pre" &&
              preUseful >= alphaTransitionAfterUsefulSamples) {
            checkpointPhase = "transition";
            logStep(
              `starting real alpha transition '${alphaTransitionLabel}' mode=${alphaTransitionMode}; capture continues without settling`
            );
            alphaTransitionPromise = performAlphaTransition({
              client,
              diagnosticsObserver,
              initialPeer: initialObservedPeer,
              mode: alphaTransitionMode,
              label: alphaTransitionLabel,
              command: alphaTransitionCommand,
              timeoutMs: alphaTransitionTimeoutMs,
              holdMs: alphaTransitionHoldMs,
              epochFile: alphaEpochFile,
              sceneName: targetSceneName,
              sceneItemId: sourceInput.sceneItemId,
              inputName,
              inputSettings: nativeInputSettings,
            }).then((transitionResult) => {
              alphaTransitionResult = transitionResult;
              alphaTransitionFinished = true;
              return transitionResult;
            });
          }

          if ((!alphaTransitionEnabled && steadyUseful >= alphaSampleCount) ||
              (alphaTransitionEnabled && alphaTransitionFinished && postUseful >= alphaSampleCount)) {
            break;
          }
        }
        if (alphaTransitionPromise) {
          alphaTransitionResult = await alphaTransitionPromise;
          alphaTransitionFinished = true;
        }
      }
      screenshot = screenshots[0] || null;
    } else if (checkAudioMeter) {
      logStep(`waiting ${waitMs}ms for source audio`);
      await sleep(waitMs);
    }
    perturbResult = await waitForPerturbCommand(perturb, perturbTimeoutMs);
    if (perturbResult && requirePerturbCommand &&
        (perturbResult.timedOut || perturbResult.exitCode !== 0)) {
      throw new Error(
        `Perturb command failed exit=${perturbResult.exitCode} timedOut=${perturbResult.timedOut}: ` +
          `${perturbResult.stderr || perturbResult.stdout}`
      );
    }

    await initialTransformPromise;
    if (initialTransformError) {
      throw initialTransformError;
    }

    logStep(`reading input settings for ${inputName}`);
    const inputSettings = await client.request("GetInputSettings", { inputName }).catch(() => null);
    const preSamples = alphaPixelSamples.filter(
      (sample) => sample.checkpoint === `pre:${alphaTransitionLabel}`
    );
    const transitionSamples = alphaPixelSamples.filter(
      (sample) => sample.checkpoint === `transition:${alphaTransitionLabel}`
    );
    const postSamples = alphaPixelSamples.filter(
      (sample) => sample.checkpoint === `post:${alphaTransitionLabel}`
    );
    const steadySamples = alphaPixelSamples.filter((sample) => sample.checkpoint === "steady");
    const alphaSequenceCheck = alphaPixelSamples.length
      ? alphaTransitionEnabled
        ? analyzeAlphaTransition(preSamples, postSamples, {
            pattern: alphaPattern,
            requiredUsefulSampleCount: alphaSampleCount,
            requireEvidenceFiles: true,
          })
        : analyzeAlphaCompositeSequence(steadySamples, {
            pattern: alphaPattern,
            expectedVisualEpoch: "pre",
            requiredUsefulSampleCount: alphaSampleCount,
            requireEvidenceFiles: true,
          })
      : null;
    const cadenceCheck = alphaPixelSamples.length
      ? {
          ...analyzeAlphaCaptureCadence(alphaPixelSamples, {
            inputCreatedAtMs,
            requiredMaximumMs: 100,
          }),
          requestedIntervalMs: requestedAlphaSampleIntervalMs,
          effectiveIntervalMs: alphaSampleIntervalMs,
          inputCreateRequestedAtMs,
        }
      : null;
    const transitionInvalidUsefulSamples = transitionSamples.filter(
      (sample) => sample.classification !== "waiting-background" &&
        (!sample.ok ||
          (sample.detectedVisualEpoch === "post" && sample.connectionEpoch !== "post"))
    );
    const observedTransition = alphaTransitionResult && alphaTransitionResult.observedTransition;
    const observedBoundaryOrderingOk = !!observedTransition && observedTransition.ok === true &&
      Number.isFinite(observedTransition.before && observedTransition.before.observedAtMs) &&
      Number.isFinite(observedTransition.retired && observedTransition.retired.observedAtMs) &&
      Number.isFinite(observedTransition.after && observedTransition.after.observedAtMs) &&
      Number.isFinite(alphaTransitionResult.epochWrittenAtMs) &&
      observedTransition.before.observedAtMs <= observedTransition.retired.observedAtMs &&
      observedTransition.retired.observedAtMs <= alphaTransitionResult.epochWrittenAtMs &&
      observedTransition.retired.observedAtMs <= observedTransition.after.observedAtMs;
    const transitionBoundaryOrderingOk = !alphaTransitionEnabled
      ? true
      : alphaTransitionMode === "source-toggle"
        ? observedBoundaryOrderingOk &&
          Number.isFinite(alphaTransitionResult.sceneItemDisabledAtMs) &&
          Number.isFinite(alphaTransitionResult.sceneItemEnabledAtMs) &&
          alphaTransitionResult.sceneItemDisabledAtMs <= alphaTransitionResult.epochWrittenAtMs &&
          alphaTransitionResult.epochWrittenAtMs <= alphaTransitionResult.sceneItemEnabledAtMs
        : alphaTransitionMode === "source-recreate"
          ? observedBoundaryOrderingOk &&
            Number.isFinite(alphaTransitionResult.sourceRemovedAtMs) &&
            Number.isFinite(alphaTransitionResult.sourceRecreatedAtMs) &&
            alphaTransitionResult.sourceRemovedAtMs <= alphaTransitionResult.epochWrittenAtMs &&
            alphaTransitionResult.epochWrittenAtMs <= alphaTransitionResult.sourceRecreatedAtMs
          : observedBoundaryOrderingOk;
    const alphaTransitionCheck = alphaTransitionEnabled
      ? {
          ok:
            !!alphaTransitionResult &&
            alphaTransitionResult.ok === true &&
            !!alphaTransitionResult.observedTransition &&
            alphaTransitionResult.observedTransition.ok === true &&
            !!alphaSequenceCheck &&
            alphaSequenceCheck.ok === true &&
            transitionSamples.length > 0 &&
            transitionBoundaryOrderingOk &&
            transitionInvalidUsefulSamples.length === 0,
          mode: alphaTransitionMode,
          label: alphaTransitionLabel,
          command: alphaTransitionCommand || null,
          afterUsefulSamples: alphaTransitionAfterUsefulSamples,
          settleMs: 0,
          holdMs: alphaTransitionHoldMs,
          timeoutMs: alphaTransitionTimeoutMs,
          epochFile: alphaEpochFile || null,
          preSampleCount: preSamples.length,
          transitionSampleCount: transitionSamples.length,
          postSampleCount: postSamples.length,
          transitionInvalidUsefulSampleIndexes: transitionInvalidUsefulSamples.map((sample) => sample.sample),
          boundaryOrderingOk: transitionBoundaryOrderingOk,
          result: alphaTransitionResult,
        }
      : null;
    const alphaPixelCheck = alphaPixelSamples.length
      ? {
          ok:
            alphaSequenceCheck.ok &&
            cadenceCheck.ok &&
            (!alphaTransitionCheck || alphaTransitionCheck.ok),
          pattern: alphaPattern,
          requestedSampleCount: requestedAlphaSampleCount,
          requiredSampleCount: requiredAlphaSampleCount,
          sampleCount: alphaPixelSamples.length,
          requestedSampleIntervalMs: requestedAlphaSampleIntervalMs,
          requiredSampleIntervalMs: requiredAlphaSampleIntervalMs,
          sampleIntervalMs: alphaSampleIntervalMs,
          maxObservedDarkFillRatio: Math.max(
            0,
            ...alphaPixelSamples.map((sample) => Number(sample.darkFillRatio) || 0)
          ),
          cadence: cadenceCheck,
          sequence: alphaSequenceCheck,
          transition: alphaTransitionCheck,
          samples: alphaPixelSamples,
        }
      : null;
    if (alphaPixelCheck && alphaPixelCheck.ok) {
      logStep("alpha composite pixel check passed");
    } else if (alphaPixelCheck) {
      logStep("alpha composite pixel check failed");
    }

    const result = {
      ok: !alphaPixelCheck || alphaPixelCheck.ok,
      mode,
      waitMs,
      websocketUrl,
      obsVersion: version.obsVersion,
      obsWebSocketVersion: version.obsWebSocketVersion,
      inputKindRegistered: true,
      screenshot,
      inputName,
      backgroundInputName,
      sceneName: targetSceneName,
      requestedSourceWidth: sourceWidth,
      requestedSourceHeight: sourceHeight,
      canvasWidth,
      canvasHeight,
      sourceSettings: inputSettings && inputSettings.inputSettings ? inputSettings.inputSettings : null,
      useNativeReceiver:
        inputSettings && inputSettings.inputSettings
          ? parseBoolean(inputSettings.inputSettings.use_native_receiver, useNativeReceiver)
          : useNativeReceiver,
      audioMeter: audioMeterCollector ? audioMeterCollector.summary : null,
      alphaPattern,
      alphaPixelCheck,
      diagnosticsEvidence: diagnosticsObserver
        ? {
            discoveryPath: path.resolve(gameCaptureControlDiscovery),
            snapshotCount: diagnosticsObserver.snapshots.length,
            errorCount: diagnosticsObserver.errors.length,
            snapshots: diagnosticsObserver.snapshots,
            errors: diagnosticsObserver.errors,
          }
        : null,
      perturbCommand: perturbResult,
    };

    if (checkAudioMeter) {
      const audioMeter = audioMeterCollector.summary;
      if (audioMeter.sampleCount < minAudioMeterSamples) {
        throw new Error(
          `OBS mixer did not report enough audio meter samples for ${inputName}: ` +
            `${audioMeter.sampleCount}/${minAudioMeterSamples}`
        );
      }
      if (audioMeter.nonSilentSampleCount === 0 || audioMeter.maxMagnitude <= 0.0001) {
        throw new Error(`OBS mixer audio for ${inputName} stayed silent`);
      }
      if (failOnAudioClip && (audioMeter.highLevelSampleCount > 0 || audioMeter.maxLevel >= 0.999)) {
        throw new Error(
          `OBS mixer audio for ${inputName} exceeded the configured high-level threshold: ` +
            `maxLevel=${audioMeter.maxLevel}, highLevelSamples=${audioMeter.highLevelSampleCount}`
        );
      }
    }

    const resultJson = JSON.stringify(result, null, 2);
    if (resultJsonPath) {
      fs.mkdirSync(path.dirname(path.resolve(resultJsonPath)), { recursive: true });
      fs.writeFileSync(resultJsonPath, `${resultJson}\n`);
    }
    logStep("source check result ready");
    console.log(resultJson);
    if (!result.ok) {
      const failedSamples = alphaPixelCheck.samples
        .filter((sample) => !sample.ok)
        .map((sample) => `${sample.sample}:${sample.failureReason || 'unknown'}`)
        .join(', ');
      const sequenceFailures = alphaPixelCheck.sequence.failureReasons.join(', ');
      const transitionFailure =
        alphaPixelCheck.transition && !alphaPixelCheck.transition.ok
          ? `transition '${alphaPixelCheck.transition.label}' lacked valid pre/post checkpoints`
          : "";
      throw new Error(
        `Alpha composite pixel check failed: samples=[${failedSamples}] sequence=[${sequenceFailures}] ` +
          `transition=[${transitionFailure}]`
      );
    }
  } finally {
    if (diagnosticsObserver) {
      await diagnosticsObserver.stop();
    }
    if (client.socket && client.socket.readyState === WebSocket.OPEN) {
      try {
        if (!keepScene) {
          logStep(`removing input ${inputName}`);
          await client.request("RemoveInput", { inputName }).catch(() => {});
          if (createdScene && previousSceneName) {
            logStep(`restoring scene ${previousSceneName}`);
            await client.request("SetCurrentProgramScene", { sceneName: previousSceneName }).catch(() => {});
          }
          if (createdScene) {
            logStep(`removing scene ${sceneName}`);
            await client.request("RemoveScene", { sceneName }).catch(() => {});
          }
        }
      } finally {
        logStep("closing websocket");
        await client.close();
      }
    }
  }
}

if (require.main === module) {
  main().catch((error) => {
    console.error(error.stack || String(error));
    process.exit(1);
  });
}

module.exports = {
  ObsWebSocketClient,
  analyzeAlphaComposite,
  analyzeAlphaCompositeImages,
  analyzeAlphaCompositeSequence,
  analyzeAlphaTransition,
  analyzeAlphaCaptureCadence,
  fixtureColorForEpoch,
  normalizedDiagnosticsPeer,
  evaluateObservedTransition,
  classifyObservedConnection,
  sameLogicalPeer,
  sameTransportIdentity,
};
