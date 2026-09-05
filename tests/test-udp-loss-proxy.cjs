const test = require("node:test");
const assert = require("node:assert/strict");
const dgram = require("node:dgram");
const path = require("node:path");
const { spawn } = require("node:child_process");
const { once } = require("node:events");

async function startProxy(port, args = []) {
  const child = spawn(process.execPath, [
    path.join(__dirname, "../scripts/udp-loss-proxy.cjs"),
    "--remote-host", "127.0.0.1",
    "--remote-port", String(port),
    "--local-port", "0",
    ...args,
  ]);
  let timer;
  try {
    const ready = await new Promise((resolve, reject) => {
      let output = "";
      timer = setTimeout(() => reject(new Error("Proxy startup timed out")), 3000);
      child.on("error", reject);
      child.once("exit", code => reject(new Error(`Proxy exited before readiness: ${code}`)));
      child.stdout.on("data", (data) => {
        output += data;
        if (output.includes("\n")) {
          resolve(JSON.parse(output.split("\n")[0]));
        }
      });
    });
    return { child, port: ready.localPort };
  } catch (error) {
    child.kill("SIGTERM");
    throw error;
  } finally {
    clearTimeout(timer);
  }
}

test("UDP proxy keeps delayed replies bound to their original clients", async () => {
  const server = dgram.createSocket("udp4");
  const first = dgram.createSocket("udp4");
  const second = dgram.createSocket("udp4");
  const packets = [];
  server.on("message", (data, remote) => {
    packets.push({ data, remote });
    if (packets.length === 2) {
      for (const packet of packets) {
        server.send(packet.data, packet.remote.port, packet.remote.address);
      }
    }
  });
  server.bind(0, "127.0.0.1");
  await once(server, "listening");
  const { child: proxy, port } = await startProxy(server.address().port);
  let timer;
  try {
    const replies = Promise.all([once(first, "message"), once(second, "message")]);
    first.send(Buffer.from("first"), port, "127.0.0.1");
    second.send(Buffer.from("second"), port, "127.0.0.1");
    const [a, b] = await Promise.race([
      replies,
      new Promise((_, reject) => {
        timer = setTimeout(() => reject(new Error("Client reply was misrouted")), 1000);
      }),
    ]);
    assert.equal(a[0].toString(), "first");
    assert.equal(b[0].toString(), "second");
    assert.notEqual(packets[0].remote.port, packets[1].remote.port);
  } finally {
    clearTimeout(timer);
    proxy.kill("SIGTERM");
    await once(proxy, "exit");
    first.close();
    second.close();
    server.close();
  }
});

test("UDP proxy still applies deterministic loss in both directions", async () => {
  const server = dgram.createSocket("udp4");
  const client = dgram.createSocket("udp4");
  const forwarded = [];
  const received = [];
  server.on("message", (data, remote) => {
    forwarded.push(data.toString());
    server.send(data, remote.port, remote.address);
  });
  client.on("message", (data) => received.push(data.toString()));
  server.bind(0, "127.0.0.1");
  await once(server, "listening");
  const { child, port } = await startProxy(server.address().port, [
    "--drop-every", "2", "--warmup-ms", "0",
  ]);
  let log = "";
  child.stderr.on("data", (data) => { log += data; });
  try {
    for (const message of ["1", "2", "3", "4"]) {
      await new Promise((resolve, reject) => {
        client.send(Buffer.from(message), port, "127.0.0.1", (error) => {
          if (error) reject(error);
          else resolve();
        });
      });
    }
    await new Promise((resolve) => setTimeout(resolve, 200));
    assert.deepEqual(forwarded, ["1", "3"]);
    assert.deepEqual(received, ["1"]);
  } finally {
    child.kill("SIGTERM");
    await once(child, "exit");
    client.close();
    server.close();
  }
  const counters = JSON.parse(log.match(/final (\{[^\n]+\})/)[1]);
  assert.equal(counters.droppedClientToRemote, 2);
  assert.equal(counters.droppedRemoteToClient, 1);
});

test("UDP proxy deterministically reorders outbound packets without losing or misrouting them", async () => {
  const server = dgram.createSocket("udp4");
  const client = dgram.createSocket("udp4");
  const forwarded = [];
  server.on("message", data => forwarded.push(data.toString()));
  server.bind(0, "127.0.0.1");
  await once(server, "listening");
  const { child, port } = await startProxy(server.address().port, [
    "--reorder-every", "2", "--reorder-delay-ms", "50", "--warmup-ms", "0",
    "--direction", "client-to-remote",
  ]);
  try {
    for (const message of ["1", "2", "3", "4"]) client.send(Buffer.from(message), port, "127.0.0.1");
    await new Promise(resolve => setTimeout(resolve, 250));
    assert.deepEqual(forwarded, ["1", "3", "2", "4"]);
  } finally {
    child.kill("SIGTERM");
    await once(child, "exit");
    client.close(); server.close();
  }
});


test("UDP proxy can impair replies without reordering client requests", async () => {
  const server = dgram.createSocket("udp4");
  const client = dgram.createSocket("udp4");
  const requests = [], replies = [];
  server.on("message", (data, remote) => {
    requests.push(data.toString());
    server.send(data, remote.port, remote.address);
  });
  client.on("message", data => replies.push(data.toString()));
  server.bind(0, "127.0.0.1");
  await once(server, "listening");
  const { child, port } = await startProxy(server.address().port, [
    "--reorder-every", "2", "--reorder-delay-ms", "50", "--warmup-ms", "0",
    "--direction", "remote-to-client",
  ]);
  try {
    for (const message of ["1", "2", "3", "4"]) client.send(Buffer.from(message), port, "127.0.0.1");
    await new Promise(resolve => setTimeout(resolve, 250));
    assert.deepEqual(requests, ["1", "2", "3", "4"]);
    assert.deepEqual(replies, ["1", "3", "2", "4"]);
  } finally {
    child.kill("SIGTERM"); await once(child, "exit");
    client.close(); server.close();
  }
});

test("UDP proxy rejects invalid impairment settings instead of silently bypassing loss", async () => {
  await assert.rejects(startProxy(3478, ["--drop-every", "invalid"]), /exited/);
  await assert.rejects(startProxy(3478, ["--direction", "invalid"]), /exited/);
  await assert.rejects(startProxy(3478, ["--reorder-delay-ms", "Infinity"]), /exited/);
});
