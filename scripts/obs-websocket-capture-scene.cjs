const fs = require("fs");
const path = require("path");
const crypto = require("crypto");

function sleep(ms) {
  return new Promise((resolve) => setTimeout(resolve, ms));
}

class ObsWebSocketClient {
  constructor(url) {
    this.url = url;
    this.socket = null;
    this.requestId = 0;
    this.pending = new Map();
    this.identified = false;
    this.requestTimeoutMs = Number(process.env.OBS_WEBSOCKET_REQUEST_TIMEOUT_MS || 15000);
  }

  async connect() {
    await new Promise((resolve, reject) => {
      const socket = new WebSocket(this.url);
      this.socket = socket;

      socket.addEventListener("open", () => resolve());
      socket.addEventListener("error", (error) => reject(error));
      socket.addEventListener("message", (event) => {
        try {
          const message = JSON.parse(event.data.toString());
          if (message.op === 0) {
            socket.send(
              JSON.stringify({
                op: 1,
                d: {
                  rpcVersion: 1,
                  eventSubscriptions: 0,
                },
              })
            );
            return;
          }

          if (message.op === 2) {
            this.identified = true;
            return;
          }

          if (message.op !== 7) {
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

async function main() {
  const websocketUrl = process.env.OBS_WEBSOCKET_URL || process.argv[2] || "ws://127.0.0.1:4455";
  const outputPath =
    process.env.OBS_CAPTURE_OUTPUT ||
    process.argv[3] ||
    path.resolve(process.cwd(), "artifacts", `obs-scene-capture-${Date.now()}.png`);

  const client = new ObsWebSocketClient(websocketUrl);

  try {
    await client.connect();
    const currentProgram = await client.request("GetCurrentProgramScene");
    const sceneName = currentProgram.currentProgramSceneName;

    const response = await client.request("GetSourceScreenshot", {
      sourceName: sceneName,
      imageFormat: "png",
      imageWidth: 1280,
      imageHeight: 720,
      imageCompressionQuality: 0,
    });

    const prefix = "data:image/png;base64,";
    if (!response.imageData || !response.imageData.startsWith(prefix)) {
      throw new Error("OBS did not return a PNG screenshot");
    }

    const buffer = Buffer.from(response.imageData.slice(prefix.length), "base64");
    fs.mkdirSync(path.dirname(outputPath), { recursive: true });
    fs.writeFileSync(outputPath, buffer);

    console.log(
      JSON.stringify(
        {
          ok: true,
          websocketUrl,
          sceneName,
          outputPath,
          byteLength: buffer.length,
          sha256: crypto.createHash("sha256").update(buffer).digest("hex"),
        },
        null,
        2
      )
    );
  } finally {
    await client.close();
  }
}

main().catch((error) => {
  console.error(error);
  process.exit(1);
});
