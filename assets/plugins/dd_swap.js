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
let prevMulHigh = 1.0;
let prevMulMid  = 1.0;
let prevAtten   = 1.0;
let prevLowGate = 0.0;
let envLow = 0.0, envMid = 0.0, envHigh = 0.0; // envelopes per band
let onsetPulse = 0.0; // short transient pulse

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
  // Keep only the freshest metrics (lower latency)
  try { sub.setsockopt(zmq.RCVHWM, 1); } catch (_) {}
  try { sub.setsockopt(zmq.CONFLATE, 1); } catch (_) {}

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

  // Helpers
  const clamp01 = (v)=> Math.max(0.0, Math.min(1.0, v));
  const smoothstep = (a,b,x)=>{ let t = clamp01((x-a)/(b-a)); return t*t*(3.0-2.0*t); };
  const updateEnv = (cur, env)=>{ const a=0.35, d=0.10; return cur>env ? env+(cur-env)*a : env+(cur-env)*d; };

  const lowNow = clamp01(last.bandLow || 0.0);
  const midNow = clamp01(last.bandMid || 0.0);
  const highNow = clamp01(last.bandHigh || 0.0);
  const deltaLow = Math.max(0.0, lowNow - prevBandLow);
  prevBandLow = prevBandLow * 0.9 + lowNow * 0.1;

  // Envelopes and onset pulse
  envLow  = updateEnv(lowNow,  envLow);
  envMid  = updateEnv(midNow,  envMid);
  envHigh = updateEnv(highNow, envHigh);
  if ((last.onset || 0.0) > 0.10) onsetPulse = Math.max(onsetPulse, clamp01(last.onset));
  onsetPulse *= 0.85;

  // Eased weights per band
  const wLow  = smoothstep(0.12, 0.45, lowNow);
  const wMid  = smoothstep(0.10, 0.50, midNow);
  const wHigh = smoothstep(0.10, 0.40, highNow);
  const onset = clamp01(last.onset || 0.0);
  const activity = clamp01((last.rms||0.0)*0.9 + onset*0.8 + (last.percE||0.0)*0.6 + highNow*0.3);

  // Largest magnitude for masks
  const largest = fwd_mvs.largest_sq();
  const maxSq = (largest && largest.length >= 3) ? largest[2]|0 : 0;

  // HIGH band: scale fast vectors (mul) — softer range and higher threshold
  let thPctH = 0.995 - 0.40*envHigh; // 99.5% -> ~59.5%
  const thresholdH = Math.floor(thPctH * maxSq);
  const maskH = fwd_mvs.compare_gt(thresholdH);
  let targetMulHigh = 1.0 + 0.18 * envHigh + 0.10 * onsetPulse; // max ~1.28
  let smMulHigh = prevMulHigh * 0.85 + targetMulHigh * 0.15;
  prevMulHigh = smMulHigh;
  fwd_mvs.mul(MV(smMulHigh, smMulHigh), maskH);

  // MID band: dampen moderate vectors (breathing) — gentler
  let thPctM = 0.96 - 0.25*envMid;
  const thresholdM = Math.floor(thPctM * maxSq);
  const maskM = fwd_mvs.compare_gt(thresholdM);
  let targetMulMid = 1.0 - 0.08 * envMid * (0.4 + 0.6*(1.0-activity)); // down to ~0.92
  let smMulMid = prevMulMid * 0.85 + targetMulMid * 0.15;
  prevMulMid = smMulMid;
  fwd_mvs.mul(MV(smMulMid, smMulMid), maskM);

  // LOW band: swap HV under bass strength/rise — more conservative + smoothed
  const instantGate = smoothstep(0.32, 0.68, envLow) * (0.35 + 0.65*onset)
                    + smoothstep(0.06, 0.16, deltaLow) * 0.35
                    + 0.30 * onsetPulse;
  const lowGate = prevLowGate * 0.85 + instantGate * 0.15;
  prevLowGate = lowGate;
  if (lowGate > 0.70) {
    fwd_mvs.swap_hv();
    const targetAtten = 1.0 - 0.04 * (lowGate * (1.0 - activity*0.4)); // down to ~0.96
    const smAtten = prevAtten * 0.85 + targetAtten * 0.15;
    prevAtten = smAtten;
    fwd_mvs.mul(MV(smAtten, smAtten));
  }

}

// dd_swap.js
// swaps x and y components of mv

// (Removed duplicate legacy exports to avoid redefinition)



