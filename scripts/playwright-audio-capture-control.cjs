const fs = require("fs");
const path = require("path");
const { chromium } = require("playwright");
const {
  analyzePcm16Le,
  createPcm16Wav,
} = require("../tests/tools/audio-continuity-analysis.cjs");

async function main() {
  const durationMs = Math.max(
    1000,
    Number(process.env.VDONINJA_AUDIO_CONTROL_MS || 10000),
  );
  const browser = await chromium.launch({
    headless: process.env.HEADLESS === "0" ? false : true,
    args: ["--autoplay-policy=no-user-gesture-required"],
  });

  try {
    const page = await browser.newPage();
    const capture = await page.evaluate(async (captureDurationMs) => {
      const SourceAudioContext =
        window.AudioContext || window.webkitAudioContext;
      const sourceContext = new SourceAudioContext({ sampleRate: 48000 });
      const captureContext = new SourceAudioContext({ sampleRate: 48000 });
      const oscillator = sourceContext.createOscillator();
      const gain = sourceContext.createGain();
      const mediaDestination = sourceContext.createMediaStreamDestination();
      oscillator.type = "sine";
      oscillator.frequency.value = 997;
      gain.gain.value = 0.08;
      oscillator.connect(gain).connect(mediaDestination);

      const source = captureContext.createMediaStreamSource(
        mediaDestination.stream,
      );
      const processor = captureContext.createScriptProcessor(4096, 1, 1);
      const silentOutput = captureContext.createGain();
      silentOutput.gain.value = 0;
      const chunks = [];
      let sampleCount = 0;
      const clonedTrack = mediaDestination.stream.getAudioTracks()[0].clone();
      const trackProcessor = new MediaStreamTrackProcessor({
        track: clonedTrack,
      });
      const rawReader = trackProcessor.readable.getReader();
      const rawChunks = [];
      let rawSampleCount = 0;
      let rawSampleRate = 0;
      let rawCaptureActive = true;
      const rawCaptureLoop = (async () => {
        while (rawCaptureActive) {
          const { value, done } = await rawReader.read();
          if (done || !value) {
            break;
          }
          try {
            const samples = new Float32Array(value.numberOfFrames);
            value.copyTo(samples, { planeIndex: 0, format: "f32-planar" });
            rawChunks.push(samples);
            rawSampleCount += samples.length;
            rawSampleRate = value.sampleRate;
          } finally {
            value.close();
          }
        }
      })();
      processor.onaudioprocess = (event) => {
        const samples = event.inputBuffer.getChannelData(0);
        chunks.push(new Float32Array(samples));
        sampleCount += samples.length;
      };
      source.connect(processor);
      processor.connect(silentOutput).connect(captureContext.destination);
      oscillator.start();
      await sourceContext.resume();
      await captureContext.resume();
      await new Promise((resolve) => setTimeout(resolve, captureDurationMs));

      processor.onaudioprocess = null;
      rawCaptureActive = false;
      await rawReader.cancel();
      await rawCaptureLoop;
      clonedTrack.stop();

      function encodeChunks(capturedChunks, capturedSampleCount) {
        const pcm = new Int16Array(capturedSampleCount);
        let destinationOffset = 0;
        for (const chunk of capturedChunks) {
          for (let index = 0; index < chunk.length; index += 1) {
            const sample = Math.max(-1, Math.min(1, chunk[index]));
            pcm[destinationOffset + index] = Math.round(sample * 32767);
          }
          destinationOffset += chunk.length;
        }

        const bytes = new Uint8Array(pcm.buffer);
        let binary = "";
        const blockSize = 32768;
        for (let offset = 0; offset < bytes.length; offset += blockSize) {
          binary += String.fromCharCode(
            ...bytes.subarray(
              offset,
              Math.min(offset + blockSize, bytes.length),
            ),
          );
        }
        return btoa(binary);
      }

      await sourceContext.close();
      await captureContext.close();
      return {
        sampleRate: captureContext.sampleRate,
        sampleCount,
        pcmBase64: encodeChunks(chunks, sampleCount),
        rawSampleRate,
        rawSampleCount,
        rawPcmBase64: encodeChunks(rawChunks, rawSampleCount),
      };
    }, durationMs);

    const pcm = Buffer.from(capture.pcmBase64, "base64");
    const playoutAnalysis = analyzePcm16Le(pcm, {
      sampleRate: capture.sampleRate,
      toneHz: 997,
    });
    const rawPcm = Buffer.from(capture.rawPcmBase64, "base64");
    const rawTrackAnalysis = analyzePcm16Le(rawPcm, {
      sampleRate: capture.rawSampleRate,
      toneHz: 997,
    });
    const outputDir = path.resolve(process.cwd(), "artifacts");
    fs.mkdirSync(outputDir, { recursive: true });
    const wavPath = path.join(
      outputDir,
      `browser-audio-control-${Date.now()}.wav`,
    );
    const rawWavPath = path.join(
      outputDir,
      `browser-audio-control-raw-${Date.now()}.wav`,
    );
    fs.writeFileSync(wavPath, createPcm16Wav(pcm, capture.sampleRate));
    fs.writeFileSync(rawWavPath, createPcm16Wav(rawPcm, capture.rawSampleRate));
    console.log(
      JSON.stringify(
        {
          ok: rawTrackAnalysis.ok,
          durationMs,
          sampleCount: capture.sampleCount,
          wavPath,
          playoutAnalysis,
          rawSampleCount: capture.rawSampleCount,
          rawWavPath,
          rawTrackAnalysis,
        },
        null,
        2,
      ),
    );
    if (!rawTrackAnalysis.ok) {
      process.exitCode = 1;
    }
  } finally {
    await browser.close();
  }
}

main().catch((error) => {
  console.error(error.stack || String(error));
  process.exit(1);
});
