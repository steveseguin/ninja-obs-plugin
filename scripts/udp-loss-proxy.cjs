const dgram = require("dgram");
const dns = require("dns").promises;

function parseArgs(argv) {
  const result = {};
  for (let index = 0; index < argv.length; index += 1) {
    if (!argv[index].startsWith("--")) {
      continue;
    }
    const name = argv[index].slice(2);
    result[name] = argv[index + 1];
    index += 1;
  }
  return result;
}

async function main() {
  const args = parseArgs(process.argv.slice(2));
  const remoteHost = args["remote-host"];
  const remotePort = Number(args["remote-port"] || 3478);
  const localPort = Number(args["local-port"] || 0);
  const bindAddress = args["bind-address"] || "127.0.0.1";
  const dropEvery = Math.max(0, Number(args["drop-every"] || 0));
  const warmupMs = Math.max(0, Number(args["warmup-ms"] || 5000));
  if (!remoteHost) {
    throw new Error(
      "Usage: node scripts/udp-loss-proxy.cjs --remote-host HOST [--remote-port 3478] [--drop-every 50]",
    );
  }
  const addresses = await dns.lookup(remoteHost, { family: 4, all: true });
  if (addresses.length === 0) {
    throw new Error(`No IPv4 address resolved for ${remoteHost}`);
  }
  const remoteAddress = addresses[0].address;
  const socket = dgram.createSocket("udp4");
  let client = null;
  let firstPacketAt = 0;
  const counters = {
    clientToRemote: 0,
    remoteToClient: 0,
    droppedClientToRemote: 0,
    droppedRemoteToClient: 0,
  };

  function shouldDrop(direction) {
    counters[direction] += 1;
    if (
      dropEvery <= 0 ||
      Date.now() - firstPacketAt < warmupMs ||
      counters[direction] % dropEvery !== 0
    ) {
      return false;
    }
    const dropKey =
      direction === "clientToRemote"
        ? "droppedClientToRemote"
        : "droppedRemoteToClient";
    counters[dropKey] += 1;
    return true;
  }

  socket.on("message", (message, rinfo) => {
    if (!firstPacketAt) {
      firstPacketAt = Date.now();
    }
    const fromRemote =
      rinfo.address === remoteAddress && rinfo.port === remotePort;
    if (fromRemote) {
      if (!client || shouldDrop("remoteToClient")) {
        return;
      }
      socket.send(message, client.port, client.address);
      return;
    }
    client = { address: rinfo.address, port: rinfo.port };
    if (shouldDrop("clientToRemote")) {
      return;
    }
    socket.send(message, remotePort, remoteAddress);
  });
  socket.on("error", (error) => {
    console.error(`[udp-loss-proxy] ${error.stack || error}`);
    process.exitCode = 1;
  });
  socket.bind(localPort, bindAddress, () => {
    const address = socket.address();
    console.log(
      JSON.stringify({
        ready: true,
        localAddress: address.address,
        localPort: address.port,
        remoteHost,
        remoteAddress,
        remotePort,
        dropEvery,
        warmupMs,
      }),
    );
  });

  const reportTimer = setInterval(() => {
    console.error(`[udp-loss-proxy] ${JSON.stringify(counters)}`);
  }, 5000);
  function stop() {
    clearInterval(reportTimer);
    console.error(`[udp-loss-proxy] final ${JSON.stringify(counters)}`);
    socket.close(() => process.exit(process.exitCode || 0));
  }
  process.on("SIGINT", stop);
  process.on("SIGTERM", stop);
}

main().catch((error) => {
  console.error(error);
  process.exitCode = 1;
});
