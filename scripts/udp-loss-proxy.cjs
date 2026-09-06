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
  const reorderEvery = Number(args["reorder-every"] || 0);
  const reorderDelayMs = Number(args["reorder-delay-ms"] || 0);
  const delayMs = Number(args["delay-ms"] || 0);
  const direction = args.direction || "both";
  if (!["both", "client-to-remote", "remote-to-client"].includes(direction) ||
      ![reorderEvery, reorderDelayMs, delayMs].every(value => Number.isFinite(value) && value >= 0) ||
      !Number.isInteger(reorderEvery)) throw new Error("Invalid impairment options");
  const warmupMs = Math.max(0, Number(args["warmup-ms"] || 5000));
  if (!Number.isInteger(dropEvery) || !Number.isFinite(warmupMs) ||
      delayMs > 60000 || reorderDelayMs > 60000) throw new Error("Invalid loss/delay options");
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
  const clients = new Map();
  let firstPacketAt = 0;
  const pending = new Set();
  const counters = {
    clientToRemote: 0,
    remoteToClient: 0,
    droppedClientToRemote: 0,
    droppedRemoteToClient: 0,
    rejectedClients: 0,
    delayedClientToRemote: 0,
    delayedRemoteToClient: 0,
    rejectedDelayedPackets: 0,
  };

  function appliesTo(packetDirection) {
    return direction === "both" || direction === (packetDirection === "clientToRemote" ? "client-to-remote" : "remote-to-client");
  }

  function forward(packetDirection, send) {
    const active = appliesTo(packetDirection) && Date.now() - firstPacketAt >= warmupMs;
    const extra = active && reorderEvery > 0 && counters[packetDirection] % reorderEvery === 0 ? reorderDelayMs : 0;
    const wait = active ? delayMs + extra : 0;
    if (!wait) return send();
    counters[packetDirection === "clientToRemote" ? "delayedClientToRemote" : "delayedRemoteToClient"]++;
    if (pending.size >= 8192) { counters.rejectedDelayedPackets++; return; }
    const timer = setTimeout(() => { pending.delete(timer); send(); }, wait);
    pending.add(timer);
  }

  function shouldDrop(direction) {
    counters[direction] += 1;
    if (
      !appliesTo(direction) ||
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
    if (shouldDrop("clientToRemote")) {
      return;
    }
    const key = `${rinfo.address}:${rinfo.port}`;
    let upstream = clients.get(key);
    if (!upstream) {
      // ICE can use several local sockets. Sharing one upstream socket loses
      // the originating client and can deliver TURN authentication/media to
      // a different socket. Keep each association stable until proxy shutdown.
      if (clients.size >= 256) {
        counters.rejectedClients += 1;
        return;
      }
      upstream = dgram.createSocket("udp4");
      clients.set(key, upstream);
      upstream.on("message", (reply, remote) => {
        if (
          remote.address !== remoteAddress ||
          remote.port !== remotePort ||
          shouldDrop("remoteToClient")
        ) {
          return;
        }
        forward("remoteToClient", () => socket.send(reply, rinfo.port, rinfo.address));
      });
      upstream.on("error", (error) => {
        console.error(`[udp-loss-proxy] ${error.stack || error}`);
        process.exitCode = 1;
      });
    }
    forward("clientToRemote", () => upstream.send(message, remotePort, remoteAddress));
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
        warmupMs, delayMs, reorderEvery, reorderDelayMs, direction,
      }),
    );
  });

  const reportTimer = setInterval(() => {
    console.error(`[udp-loss-proxy] ${JSON.stringify(counters)}`);
  }, 5000);
  function stop() {
    clearInterval(reportTimer);
    for (const timer of pending) clearTimeout(timer);
    pending.clear();
    console.error(`[udp-loss-proxy] final ${JSON.stringify(counters)}`);
    for (const upstream of clients.values()) {
      upstream.close();
    }
    socket.close(() => process.exit(process.exitCode || 0));
  }
  process.on("SIGINT", stop);
  process.on("SIGTERM", stop);
}

main().catch((error) => {
  console.error(error);
  process.exitCode = 1;
});
