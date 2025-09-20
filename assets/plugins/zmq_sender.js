import * as std from "std";
import * as zmq from "zmq";

function main(argv) {
  let endpoint = argv.length > 0 ? argv[0] : "ipc:///tmp/ffg_metrics";
  // Fallback to TCP if IPC not available
  if (endpoint.startsWith("ipc://") && zmq.has && (zmq.has("ipc") === 0)) {
    endpoint = "tcp://127.0.0.1:5556";
  }
  const ctx = new zmq.Context();
  const pub = ctx.socket(zmq.PUB);
  try { pub.setsockopt(zmq.SNDHWM, 1); } catch (e) {}
  pub.bind(endpoint);
  // Relay stdin lines to PUB
  while (true) {
    const line = std.in.getline();
    if (line === null) break;
    try { pub.send(line); } catch (e) { /* drop */ }
  }
  pub.close();
  ctx.term();
}

try { main(scriptArgs.slice(1)); } catch (e) { /* ignore */ }


