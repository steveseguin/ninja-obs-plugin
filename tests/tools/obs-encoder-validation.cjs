async function verifyExpectedEncoderMode(client, simpleEncoder, advancedEncoder) {
  if (!simpleEncoder && !advancedEncoder) return;
  if (simpleEncoder && advancedEncoder) {
    throw new Error("Specify an expected Simple or Advanced encoder, not both");
  }
  const outputMode = await client.request("GetProfileParameter", {
    parameterCategory: "Output",
    parameterName: "Mode",
  });
  const expectedMode = advancedEncoder ? "Advanced" : "Simple";
  if (outputMode.parameterValue !== expectedMode) {
    throw new Error(
      `Expected OBS Output/Mode=${expectedMode} for the encoder check; observed ${outputMode.parameterValue}`,
    );
  }
}

module.exports = { verifyExpectedEncoderMode };
