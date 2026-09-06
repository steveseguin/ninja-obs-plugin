const test = require("node:test");
const assert = require("node:assert/strict");
const vm = require("node:vm");
const { installProbes } = require("./tools/rtc-timing-probes.cjs");

async function fixture(fixedBuffer = null) {
  class Receiver {
    constructor() {
      this.track = { kind: "video" };
      this.target = 0;
    }
    get jitterBufferTarget() {
      return this.target;
    }
    set jitterBufferTarget(value) {
      this.target = value;
    }
  }
  class PC {
    constructor(config) {
      this.config = config;
      this.handlers = {};
    }
    setConfiguration(config) {
      this.config = config;
    }
    addEventListener(name, handler) {
      this.handlers[name] = handler;
    }
    addTrack(sender) {
      return sender;
    }
    addTransceiver(sender) {
      return { sender };
    }
    async setLocalDescription() {}
    async setRemoteDescription() {}
    async addIceCandidate() {}
  }
  const sandbox = {
    RTCPeerConnection: PC,
    RTCRtpReceiver: Receiver,
    TransformStream,
    performance,
    console,
  };
  sandbox.window = sandbox;
  vm.createContext(sandbox);
  await installProbes(
    {
      addInitScript: async (fn, args) => {
        sandbox.args = args;
        vm.runInContext(`(${fn.toString()})(args)`, sandbox);
      },
    },
    [{ urls: "turn:127.0.0.1:3478" }],
    fixedBuffer,
  );
  return sandbox;
}

test("encoded probe survives application TransformStream replacement without changing frames", async () => {
  const page = await fixture();
  page.TransformStream = class {
    constructor() {
      throw new Error("Application polyfill");
    }
  };
  const frame = {
    timestamp: 9000,
    type: "key",
    data: new Uint8Array([1, 2, 3]).buffer,
    getMetadata: () => ({ mimeType: "video/H264" }),
  };
  let resolve;
  const received = new Promise((done) => {
    resolve = done;
  });
  const sender = {
    createEncodedStreams: () => ({
      readable: new ReadableStream({
        start(controller) {
          controller.enqueue(frame);
          controller.close();
        },
      }),
      writable: new WritableStream({
        write(value) {
          resolve(value);
        },
      }),
    }),
  };
  const pc = new page.RTCPeerConnection();
  pc.addTrack(sender);
  assert.equal(await received, frame);
  assert.equal(page.__encodedTiming[0].rtpTimestamp, 9000);
  assert.equal(page.__encodedTiming[0].bytes, 3);
  assert.equal(page.__encodedError, undefined);
});

test("fixed buffer preserves requested value across application writes", async () => {
  const page = await fixture(300);
  const pc = new page.RTCPeerConnection();
  const receiver = new page.RTCRtpReceiver();
  pc.handlers.track({ receiver });
  receiver.jitterBufferTarget = 0;
  assert.equal(receiver.jitterBufferTarget, 300);
  assert.equal(page.__timingCapabilities.fixedBuffers[0].applied, 300);
});

test("unsupported encoded probe stays explicit and relay isolation survives reconfiguration", async () => {
  const page = await fixture();
  const pc = new page.RTCPeerConnection({ iceTransportPolicy: "all" });
  pc.setConfiguration({ iceServers: [{ urls: "stun:example.invalid" }] });
  assert.equal(pc.config.iceTransportPolicy, "relay");
  assert.equal(pc.config.iceServers[0].urls, "turn:127.0.0.1:3478");
  assert.equal(page.__timingCapabilities.encodedReceiver, false);
  assert.equal(page.__pcList.length, 1);
});
