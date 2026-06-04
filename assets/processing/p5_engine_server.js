#!/usr/bin/env node
"use strict";

const crypto = require("crypto");
const fs = require("fs");
const http = require("http");
const path = require("path");
const { URL } = require("url");

function argValue(name, fallback) {
  const index = process.argv.indexOf(name);
  if (index >= 0 && index + 1 < process.argv.length) return process.argv[index + 1];
  return fallback;
}

const port = Number(argValue("--port", "18181"));
const root = path.resolve(argValue("--root", path.dirname(__filename)));
const defaultSketch = argValue("--sketch", "audio_spirograph.js");
const frameFile = argValue("--frame-file", "");
const hotReloadEnabled = argValue("--hot-reload", "1") !== "0";
const clients = new Set();

function contentType(filePath) {
  const ext = path.extname(filePath).toLowerCase();
  if (ext === ".html") return "text/html; charset=utf-8";
  if (ext === ".js") return "text/javascript; charset=utf-8";
  if (ext === ".json") return "application/json; charset=utf-8";
  if (ext === ".css") return "text/css; charset=utf-8";
  return "application/octet-stream";
}

function sendFile(res, filePath) {
  fs.readFile(filePath, (err, data) => {
    if (err) {
      res.writeHead(404, { "Content-Type": "text/plain; charset=utf-8" });
      res.end("not found\n");
      return;
    }
    res.writeHead(200, {
      "Content-Type": contentType(filePath),
      "Cache-Control": "no-cache",
    });
    res.end(data);
  });
}

function listSketches() {
  const sketchDir = path.join(root, "sketches");
  try {
    return fs.readdirSync(sketchDir)
      .filter((name) => name.endsWith(".js"))
      .sort();
  } catch {
    return [];
  }
}

function safeFile(base, requested) {
  const resolvedBase = path.resolve(base);
  const resolved = path.resolve(resolvedBase, requested);
  if (resolved !== resolvedBase && !resolved.startsWith(resolvedBase + path.sep)) return null;
  return resolved;
}

function websocketFrame(text) {
  const payload = Buffer.from(text, "utf8");
  let header;
  if (payload.length < 126) {
    header = Buffer.from([0x81, payload.length]);
  } else if (payload.length < 65536) {
    header = Buffer.alloc(4);
    header[0] = 0x81;
    header[1] = 126;
    header.writeUInt16BE(payload.length, 2);
  } else {
    header = Buffer.alloc(10);
    header[0] = 0x81;
    header[1] = 127;
    header.writeBigUInt64BE(BigInt(payload.length), 2);
  }
  return Buffer.concat([header, payload]);
}

function broadcast(text) {
  const frame = websocketFrame(text);
  for (const socket of Array.from(clients)) {
    if (!socket.writable) {
      clients.delete(socket);
      continue;
    }
    socket.write(frame, (err) => {
      if (err) {
        clients.delete(socket);
        socket.destroy();
      }
    });
  }
}

function broadcastReload(filePath) {
  const relative = path.relative(root, filePath).replace(/\\/g, "/");
  const payload = JSON.stringify({
    __visualizaControl: "reload",
    reason: "file-change",
    file: relative || path.basename(filePath),
    time: Date.now(),
  });
  console.log(`p5 hot reload: ${relative || filePath}`);
  broadcast(payload);
}

function startHotReload() {
  if (!hotReloadEnabled) {
    console.log("p5 hot reload disabled");
    return;
  }

  const watchers = [];
  let reloadTimer = null;
  let pendingFile = "";

  function schedule(filePath) {
    pendingFile = filePath;
    if (reloadTimer) clearTimeout(reloadTimer);
    reloadTimer = setTimeout(() => {
      reloadTimer = null;
      broadcastReload(pendingFile);
    }, 120);
  }

  function watchDir(dir, label, shouldReload) {
    try {
      if (!fs.existsSync(dir)) return;
      const watcher = fs.watch(dir, { persistent: true }, (_eventType, filename) => {
        const name = filename ? filename.toString() : "";
        if (!shouldReload(name)) return;
        schedule(name ? path.join(dir, name) : dir);
      });
      watcher.on("error", (err) => {
        console.warn(`p5 hot reload watcher failed for ${label}: ${err.message}`);
      });
      watchers.push(watcher);
      console.log(`p5 hot reload watching ${label}: ${dir}`);
    } catch (err) {
      console.warn(`p5 hot reload unavailable for ${label}: ${err.message}`);
    }
  }

  const selectedSketch = path.basename(defaultSketch);
  watchDir(path.join(root, "sketches"), `sketch ${selectedSketch}`, (name) => !name || name === selectedSketch);
  watchDir(root, "engine files", (name) => !name || name === "engine.js" || name === "p5_engine.html");

  function closeWatchers() {
    for (const watcher of watchers) {
      try { watcher.close(); } catch {}
    }
  }
  process.once("exit", closeWatchers);
}

function parseClientFrames(socket, onMessage) {
  let buffer = Buffer.alloc(0);
  let fragmentedOpcode = 0;
  let fragments = [];
  socket.on("data", (chunk) => {
    buffer = Buffer.concat([buffer, chunk]);
    for (;;) {
      if (buffer.length < 2) return;
      const fin = (buffer[0] & 0x80) !== 0;
      const opcode = buffer[0] & 0x0f;
      const masked = (buffer[1] & 0x80) !== 0;
      let length = buffer[1] & 0x7f;
      let offset = 2;
      if (length === 126) {
        if (buffer.length < offset + 2) return;
        length = buffer.readUInt16BE(offset);
        offset += 2;
      } else if (length === 127) {
        if (buffer.length < offset + 8) return;
        const bigLength = buffer.readBigUInt64BE(offset);
        if (bigLength > BigInt(64 * 1024 * 1024)) {
          socket.destroy();
          return;
        }
        length = Number(bigLength);
        offset += 8;
      }
      if (!masked) {
        socket.destroy();
        return;
      }
      if (buffer.length < offset + 4 + length) return;
      const mask = buffer.subarray(offset, offset + 4);
      offset += 4;
      const payload = Buffer.from(buffer.subarray(offset, offset + length));
      buffer = buffer.subarray(offset + length);
      for (let i = 0; i < payload.length; i++) payload[i] ^= mask[i & 3];
      if (opcode === 0x8) {
        socket.end();
        return;
      }
      if (opcode === 0x0 && fragmentedOpcode) {
        fragments.push(payload);
        if (fin) {
          onMessage(Buffer.concat(fragments), fragmentedOpcode);
          fragments = [];
          fragmentedOpcode = 0;
        }
      } else if (opcode === 0x1 || opcode === 0x2) {
        if (fin) {
          onMessage(payload, opcode);
        } else {
          fragmentedOpcode = opcode;
          fragments = [payload];
        }
      }
    }
  });
}

function writeFrameFile(payload) {
  if (!frameFile || payload.length < 8) return;
  const width = payload.readUInt32LE(0);
  const height = payload.readUInt32LE(4);
  const bytes = width * height * 4;
  if (width < 1 || height < 1 || width > 4096 || height > 4096 || payload.length < 8 + bytes) {
    console.warn(`discarded invalid frame ${width}x${height} payload=${payload.length}`);
    return;
  }
  writeFrameFile.seq = (writeFrameFile.seq || 0) + 1;
  const header = Buffer.alloc(24);
  header.write("VZP5FRM1", 0, "ascii");
  header.writeUInt32LE(width, 8);
  header.writeUInt32LE(height, 12);
  header.writeUInt32LE(writeFrameFile.seq >>> 0, 16);
  header.writeUInt32LE(bytes, 20);
  const tmp = frameFile + ".tmp";
  fs.writeFile(tmp, Buffer.concat([header, payload.subarray(8, 8 + bytes)]), (err) => {
    if (err) {
      console.warn(`failed writing frame tmp ${tmp}: ${err.message}`);
      return;
    }
    fs.rename(tmp, frameFile, (renameErr) => {
      if (renameErr) {
        console.warn(`failed publishing frame ${frameFile}: ${renameErr.message}`);
      } else if (!writeFrameFile.loggedFirstFrame) {
        writeFrameFile.loggedFirstFrame = true;
        console.log(`p5 first frame written ${width}x${height} to ${frameFile}`);
      }
    });
  });
}

const server = http.createServer((req, res) => {
  const requestUrl = new URL(req.url, "http://127.0.0.1");
  const pathname = decodeURIComponent(requestUrl.pathname);

  if (pathname === "/") {
    if (!requestUrl.searchParams.has("sketch")) {
      res.writeHead(302, { Location: "/?sketch=" + encodeURIComponent(defaultSketch) });
      res.end();
      return;
    }
    sendFile(res, path.join(root, "p5_engine.html"));
    return;
  }

  if (pathname === "/engine.js") {
    sendFile(res, path.join(root, "engine.js"));
    return;
  }

  if (pathname === "/sketch-list.json") {
    res.writeHead(200, {
      "Content-Type": "application/json; charset=utf-8",
      "Cache-Control": "no-cache",
    });
    res.end(JSON.stringify({ sketches: listSketches(), defaultSketch }) + "\n");
    return;
  }

  if (pathname.startsWith("/sketches/")) {
    const filePath = safeFile(path.join(root, "sketches"), pathname.slice("/sketches/".length));
    if (!filePath) {
      res.writeHead(403);
      res.end("forbidden\n");
      return;
    }
    sendFile(res, filePath);
    return;
  }

  if (pathname.startsWith("/vendor/")) {
    const filePath = safeFile(path.join(root, "vendor"), pathname.slice("/vendor/".length));
    if (!filePath) {
      res.writeHead(403);
      res.end("forbidden\n");
      return;
    }
    sendFile(res, filePath);
    return;
  }

  res.writeHead(404, { "Content-Type": "text/plain; charset=utf-8" });
  res.end("not found\n");
});

server.on("upgrade", (req, socket) => {
  const requestUrl = new URL(req.url, "http://127.0.0.1");
  if (requestUrl.pathname !== "/metrics" && requestUrl.pathname !== "/frames") {
    socket.destroy();
    return;
  }
  const key = req.headers["sec-websocket-key"];
  if (!key) {
    socket.destroy();
    return;
  }
  const accept = crypto
    .createHash("sha1")
    .update(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11")
    .digest("base64");
  socket.write([
    "HTTP/1.1 101 Switching Protocols",
    "Upgrade: websocket",
    "Connection: Upgrade",
    "Sec-WebSocket-Accept: " + accept,
    "",
    "",
  ].join("\r\n"));
  if (requestUrl.pathname === "/metrics") {
    clients.add(socket);
    parseClientFrames(socket, () => {});
    socket.on("close", () => clients.delete(socket));
    socket.on("error", () => clients.delete(socket));
  } else {
    console.log("p5 frame client connected");
    parseClientFrames(socket, (payload, opcode) => {
      if (opcode === 0x2) writeFrameFile(payload);
    });
    socket.on("error", () => {});
  }
});

let stdinBuffer = "";
process.stdin.setEncoding("utf8");
process.stdin.on("data", (chunk) => {
  stdinBuffer += chunk;
  for (;;) {
    const newline = stdinBuffer.indexOf("\n");
    if (newline < 0) break;
    const line = stdinBuffer.slice(0, newline + 1);
    stdinBuffer = stdinBuffer.slice(newline + 1);
    broadcast(line);
  }
});
process.stdin.on("end", () => process.exit(0));

server.listen(port, "127.0.0.1", () => {
  console.log(`visualiza p5 engine listening on http://127.0.0.1:${port}/?sketch=${encodeURIComponent(defaultSketch)}`);
  console.log(`root=${root}`);
  if (frameFile) console.log(`frameFile=${frameFile}`);
  startHotReload();
});
