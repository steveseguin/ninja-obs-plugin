const childProcess = require("child_process");
const path = require("path");

function parseArgs(argv) {
  const result = {};
  for (let index = 0; index < argv.length; index += 1) {
    const token = argv[index];
    if (!token.startsWith("--")) {
      continue;
    }
    const equals = token.indexOf("=");
    if (equals >= 0) {
      result[token.slice(2, equals)] = token.slice(equals + 1);
    } else {
      result[token.slice(2)] = argv[index + 1];
      index += 1;
    }
  }
  return result;
}

function markerFilter(fps) {
  const filters = [
    "drawbox=x=192:y=920:w=1536:h=96:color=gray:t=fill",
  ];
  const sync = 0xd3;
  for (let cell = 0; cell < 32; cell += 1) {
    const x = 192 + cell * 48 + 3;
    filters.push(`drawbox=x=${x}:y=923:w=42:h=90:color=black:t=fill`);
    let enable;
    if (cell < 8) {
      if (((sync >>> (7 - cell)) & 1) === 0) {
        continue;
      }
      enable = "1";
    } else if (cell < 24) {
      const bit = 23 - cell;
      enable = `gte(mod(floor(n/pow(2\\,${bit}))\\,2)\\,1)`;
    } else {
      const bit = 31 - cell;
      enable = `lt(mod(floor(n/pow(2\\,${bit}))\\,2)\\,1)`;
    }
    filters.push(
      `drawbox=x=${x}:y=923:w=42:h=90:color=white:t=fill:enable='${enable}'`,
    );
  }
  return filters.join(",");
}

function main() {
  const args = parseArgs(process.argv.slice(2));
  const output = path.resolve(args.output || "artifacts/publish-sequence.mp4");
  const duration = Math.max(5, Number(args.duration || 60));
  const fps = Math.max(1, Number(args.fps || 60));
  const ffmpeg = args.ffmpeg || process.env.FFMPEG || "ffmpeg";
  const videoFilter = markerFilter(fps);
  const command = [
    "-y",
    "-f",
    "lavfi",
    "-i",
    `testsrc2=size=1920x1080:rate=${fps}:duration=${duration}`,
    "-f",
    "lavfi",
    "-i",
    `sine=frequency=997:sample_rate=48000:duration=${duration}`,
    "-vf",
    videoFilter,
    "-c:v",
    "libx264",
    "-preset",
    "veryfast",
    "-crf",
    "16",
    "-pix_fmt",
    "yuv420p",
    "-g",
    String(fps * 2),
    "-c:a",
    "aac",
    "-b:a",
    "160k",
    "-shortest",
    output,
  ];
  console.error(
    `[publish-sequence-generator] creating ${duration}s ${fps} FPS test video at ${output}`,
  );
  const result = childProcess.spawnSync(ffmpeg, command, {
    stdio: "inherit",
  });
  if (result.error) {
    throw result.error;
  }
  if (result.status !== 0) {
    throw new Error(`ffmpeg exited with status ${result.status}`);
  }
  console.log(output);
}

main();
