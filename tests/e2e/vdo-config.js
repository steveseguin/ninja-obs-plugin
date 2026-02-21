function hasEnv(name) {
  return Object.prototype.hasOwnProperty.call(process.env, name);
}

function getEnvOrDefault(name, fallback) {
  if (!hasEnv(name)) {
    return fallback;
  }
  return process.env[name] ?? fallback;
}

function getOptionalEnv(name) {
  if (!hasEnv(name)) {
    return "";
  }
  return process.env[name] ?? "";
}

function buildScenarioUrls() {
  const streamId = getEnvOrDefault("VDO_STREAM_ID", "Alsosuitbc");
  const noPassword = getOptionalEnv("VDO_NO_PASSWORD") === "1";
  const password = noPassword ? "" : hasEnv("VDO_PASSWORD") ? process.env.VDO_PASSWORD ?? "" : "somepassword";
  const roomId = getOptionalEnv("VDO_ROOM_ID");
  const bitrate = getOptionalEnv("VDO_BITRATE");
  const includeRoomInView = roomId ? getOptionalEnv("VDO_VIEW_INCLUDE_ROOM") !== "0" : false;
  const includeSceneInView = roomId ? getOptionalEnv("VDO_VIEW_INCLUDE_SCENE") !== "0" : false;
  const cleanoutput = getEnvOrDefault("VDO_CLEANOUTPUT", "1");

  const viewParams = new URLSearchParams();
  if (cleanoutput) {
    viewParams.set("cleanoutput", cleanoutput);
  }
  if (password) {
    viewParams.set("password", password);
  }

  const pushParams = new URLSearchParams(viewParams);
  pushParams.set("autostart", "1");
  pushParams.set("webcam", "1");
  if (roomId) {
    pushParams.set("room", roomId);
    if (includeRoomInView) {
      viewParams.set("room", roomId);
      if (includeSceneInView) {
        viewParams.set("scene", "1");
      }
    }
  }
  if (bitrate) {
    pushParams.set("bitrate", bitrate);
  }

  const pushUrl =
    process.env.VDO_PUSH_URL ||
    `https://vdo.ninja/?push=${encodeURIComponent(streamId)}&${pushParams.toString()}`;
  const viewUrl =
    process.env.VDO_VIEW_URL ||
    `https://vdo.ninja/?view=${encodeURIComponent(streamId)}&${viewParams.toString()}`;

  return {
    streamId,
    noPassword,
    password,
    roomId,
    bitrate,
    includeRoomInView,
    includeSceneInView,
    pushUrl,
    viewUrl,
  };
}

module.exports = {
  buildScenarioUrls,
};
