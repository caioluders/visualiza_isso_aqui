// dd_swap.js with ZMQ-reactive control
// - Swaps x/y components of motion vectors
// - Reacts to live audio metrics received via ZMQ (from host app)

import * as zmq from "zmq";

let ctx = null;
let sub = null;
let poller = null;
let last = {
  rms: 0.0,
  bandLow: 0.0,
  bandMid: 0.0,
  bandHigh: 0.0,
  onset: 0.0,
  bpm: 0.0,
  beatEnv: 0.0,
  percE: 0.0,
  harmE: 0.0,
  percRatio: 0.0,
};
let prevBandLow = 0.0;

export function setup(args)
{
  // We need MV access
  args.features = [ "mv" ];

  // Create SUB socket and connect to host metrics
  ctx = new zmq.Context();
  sub = ctx.socket(zmq.SUB);
  // Prefer IPC on same machine; fallback to TCP if unavailable
  let endpoint = "ipc:///tmp/ffg_metrics";
  try {
    if (zmq.has && zmq.has("ipc") === 0) endpoint = "tcp://127.0.0.1:5556";
  } catch (_) { endpoint = "tcp://127.0.0.1:5556"; }
  sub.connect(endpoint);
  sub.setsockopt(zmq.SUBSCRIBE, "");

  // Poller for non-blocking receive
  poller = new zmq.Poller();
  poller.add(sub, zmq.POLLIN);
}

function pumpMetricsNonBlocking()
{
  if (!poller) return;
  try {
    // Non-blocking poll
    const ev = poller.wait(0);
    if (!ev) return;
    // Drain all pending messages to keep most recent
    while (true) {
      const s = sub.recv_str();
      if (s === undefined || s === null) break;
      try {
        const data = JSON.parse(s);
        if (typeof data === 'object' && data) {
          last.rms = +data.rms || 0.0;
          last.bandLow = +data.bandLow || 0.0;
          last.bandMid = +data.bandMid || 0.0;
          last.bandHigh = +data.bandHigh || 0.0;
          last.onset = +data.onset || 0.0;
          last.bpm = +data.bpm || 0.0;
          last.beatEnv = +data.beatEnv || 0.0;
          last.percE = +data.percE || 0.0;
          last.harmE = +data.harmE || 0.0;
          last.percRatio = +data.percRatio || 0.0;
          console.log("Metrics updated: bandHigh", data.bandHigh);
          console.log("Metrics updated: rms", data.rms);
          console.log("Metrics updated: onset", data.onset);
          console.log("Metrics updated: bpm", data.bpm);
          console.log("Metrics updated: beatEnv", data.beatEnv);
          console.log("Metrics updated: percE", data.percE);
          console.log("Metrics updated: harmE", data.harmE);
          console.log("Metrics updated: percRatio", data.percRatio);
        }
      } catch (_) { /* ignore parse errors */ }
      // Attempt to get more without blocking; break if none
      const s2 = sub.recv_str();
      if (s2 === undefined || s2 === null) break; // put back? we consumed one extra
      try {
        const data2 = JSON.parse(s2);
        if (typeof data2 === 'object' && data2) {
          last = Object.assign(last, data2);
        }
      } catch (_) {}
      break;
    }
  } catch (_) {
    // ignore
  }
}

export function glitch_frame(frame)
{
  // Update metrics
  pumpMetricsNonBlocking();

  // Use forward MVs if present
  const fwd_mvs = frame.mv?.forward;
  if (!fwd_mvs) return;

  // Truncate overflow like ffedit recommends
  frame.mv.overflow = "truncate";

  // Mild baseline; accelerate only when bass rises, and only on strong MVs
  const lowNow = Math.max(0.0, Math.min(1.0, last.bandLow || 0.0));
  const deltaLow = Math.max(0.0, lowNow - prevBandLow);
  prevBandLow = prevBandLow * 0.9 + lowNow * 0.1;

  const musicLevel = Math.max(
    (last.rms || 0.0) * 1.2,
    (last.onset || 0.0) * 2.0,
    (last.percE || 0.0),
    lowNow * 1.5
  );
  let musicActive = (musicLevel - 0.05) / 0.15;

  // Compute threshold over largest motion (squared magnitude)
  // Make threshold primarily responsive to bass level/rise
  const largest = fwd_mvs.largest_sq(); // [x, y, sq]
  const maxSq = (largest && largest.length >= 3) ? largest[2]|0 : 0;
  const bass = lowNow;
  const bassRise = deltaLow;
  let thresholdPct = 0.98 - 0.70 * bass - 0.50 * bassRise - 0.30 * musicActive;
  const threshold = Math.floor(thresholdPct * maxSq);
  const mask = fwd_mvs.compare_gt(threshold);

  // Scale only strong vectors; multiplier driven by bass increase and activity
  let multiple = 1.005 + (deltaLow * 3.0) * musicActive;
  
  fwd_mvs.mul(MV(multiple, multiple), mask);

}

// dd_swap.js
// swaps x and y components of mv

// (Removed duplicate legacy exports to avoid redefinition)


