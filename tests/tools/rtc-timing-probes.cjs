// Passive encoded-frame timestamps and optional fixed receiver-buffer control.
async function installProbes(context, iceServers, fixedBuffer) {
  await context.addInitScript(
    ({ iceServers, fixedBuffer }) => {
      window.__pcList = [];
      window.__encodedTiming = [];
      window.__encodedOverflow = 0;
      window.__negotiation = [];
      window.__timingCapabilities = {
        encodedReceiver:
          typeof RTCRtpReceiver.prototype.createEncodedStreams === "function",
        fixedBuffers: [],
      };
      const observed = new WeakSet();
      const NativeTransformStream = window.TransformStream;
      function observe(endpoint, direction) {
        if (
          !endpoint ||
          observed.has(endpoint) ||
          typeof endpoint.createEncodedStreams !== "function"
        )
          return;
        observed.add(endpoint);
        try {
          const { readable, writable } = endpoint.createEncodedStreams();
          readable
            .pipeThrough(
              new NativeTransformStream({
                transform(frame, controller) {
                  if (window.__encodedTiming.length < 120000)
                    window.__encodedTiming.push({
                      direction,
                      now: performance.now(),
                      epoch: performance.timeOrigin + performance.now(),
                      rtpTimestamp: frame.timestamp,
                      type: frame.type,
                      bytes: frame.data.byteLength,
                      metadata:
                        typeof frame.getMetadata === "function"
                          ? frame.getMetadata()
                          : null,
                    });
                  else ++window.__encodedOverflow;
                  controller.enqueue(frame);
                },
              }),
            )
            .pipeTo(writable)
            .catch((error) => {
              window.__encodedError = String(error);
            });
        } catch (error) {
          window.__encodedError = String(error);
        }
      }
      const NativePC = window.RTCPeerConnection;
      window.RTCPeerConnection = function (config = {}, ...rest) {
        const pc = new NativePC(
          {
            ...config,
            iceServers,
            iceTransportPolicy: "relay",
            encodedInsertableStreams: true,
          },
          ...rest,
        );
        for (const method of [
          "setLocalDescription",
          "setRemoteDescription",
          "addIceCandidate",
        ]) {
          const original = pc[method].bind(pc);
          pc[method] = async (...args) => {
            try {
              const result = await original(...args);
              if (window.__negotiation.length < 2000)
                window.__negotiation.push({
                  method,
                  args,
                  now: performance.now(),
                  local: pc.localDescription?.toJSON(),
                  remote: pc.remoteDescription?.toJSON(),
                });
              return result;
            } catch (error) {
              if (window.__negotiation.length < 2000)
                window.__negotiation.push({
                  method,
                  args,
                  now: performance.now(),
                  error: String(error),
                });
              throw error;
            }
          };
        }
        const setConfiguration = pc.setConfiguration.bind(pc);
        pc.setConfiguration = (config) =>
          setConfiguration({
            ...config,
            iceServers,
            iceTransportPolicy: "relay",
          });
        const addTrack = pc.addTrack.bind(pc);
        pc.addTrack = (...args) => {
          const sender = addTrack(...args);
          observe(sender, "send");
          return sender;
        };
        const addTransceiver = pc.addTransceiver.bind(pc);
        pc.addTransceiver = (...args) => {
          const t = addTransceiver(...args);
          observe(t.sender, "send");
          return t;
        };
        pc.addEventListener("track", (event) => {
          observe(event.receiver, "receive");
          if (fixedBuffer !== null) {
            const receiver = event.receiver;
            const key =
              "jitterBufferTarget" in receiver
                ? "jitterBufferTarget"
                : "playoutDelayHint";
            const descriptor = Object.getOwnPropertyDescriptor(
              RTCRtpReceiver.prototype,
              key,
            );
            if (descriptor?.set && descriptor?.get) {
              const value =
                key === "jitterBufferTarget" ? fixedBuffer : fixedBuffer / 1000;
              descriptor.set.call(receiver, value);
              window.__timingCapabilities.fixedBuffers.push({
                kind: receiver.track.kind,
                key,
                requested: value,
                applied: descriptor.get.call(receiver),
              });
              // Isolate a fixed browser target from the application's adaptive writes.
              Object.defineProperty(receiver, key, {
                get: () => descriptor.get.call(receiver),
                set: () => {},
              });
            }
          }
        });
        if (!window.__pcList.includes(pc)) window.__pcList.push(pc);
        return pc;
      };
      window.RTCPeerConnection.prototype = NativePC.prototype;
      Object.setPrototypeOf(window.RTCPeerConnection, NativePC);
    },
    { iceServers, fixedBuffer },
  );
}
async function stats(page) {
  return page.evaluate(async () => {
    const pcs = [];
    for (const pc of window.__pcList || []) {
      const report = await pc.getStats();
      pcs.push({
        state: pc.connectionState,
        stats: Array.from(report.values()).filter((s) =>
          [
            "inbound-rtp",
            "outbound-rtp",
            "remote-outbound-rtp",
            "remote-inbound-rtp",
            "candidate-pair",
            "local-candidate",
            "remote-candidate",
            "codec",
          ].includes(s.type),
        ),
      });
    }
    return {
      now: performance.now(),
      epoch: performance.timeOrigin + performance.now(),
      pcs,
    };
  });
}

module.exports = { installProbes, stats };
