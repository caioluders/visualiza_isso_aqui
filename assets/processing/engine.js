"use strict";

(function () {
  const defaults = {
    rms: 0, bandLow: 0, bandMid: 0, bandHigh: 0, onset: 0, flux: 0,
    bpm: 0, beatEnv: 0, beat: 0, percE: 0, harmE: 0, percRatio: 0,
    kick: 0, snare: 0, hat: 0, beatPulse: 0,
    sub: 0, bass: 0, body: 0, harmonic: 0, lead: 0, air: 0,
    transient: 0, novelty: 0, brightness: 0, percussive: 0,
    energy: 0, tension: 0, release: 0, drop: 0, section: 0,
    roleKickCenterNorm: 0, roleBassCenterNorm: 0, roleLeadCenterNorm: 0, roleAirCenterNorm: 0,
    roleKickConfidence: 0, roleBassConfidence: 0, roleLeadConfidence: 0, roleAirConfidence: 0,
    bands: new Array(16).fill(0),
    onsets: new Array(16).fill(0),
  };

  const state = Object.assign({}, defaults);
  state.bands = defaults.bands.slice();
  state.onsets = defaults.onsets.slice();
  state.connected = false;
  state.lastMessageTime = 0;
  const params = new URLSearchParams(location.search);
  state.sketch = params.get("sketch") || "audio_spirograph.js";
  const captureEnabled = params.get("capture") === "1";
  const captureFps = Math.max(1, Math.min(60, Number(params.get("captureFps") || 60)));
  const captureWidth = Math.max(128, Math.min(4096, Number(params.get("captureWidth") || window.innerWidth || 1280)));
  const captureHeight = Math.max(128, Math.min(4096, Number(params.get("captureHeight") || window.innerHeight || 720)));

  window.visualizaAudio = state;
  window.visualizaSignal = function visualizaSignal(name, fallback) {
    const value = state[name];
    return Number.isFinite(value) ? value : (fallback || 0);
  };
  window.visualizaBand = function visualizaBand(index) {
    const i = Math.max(0, Math.min(15, Math.floor(index)));
    return state.bands[i] || 0;
  };
  window.visualizaOnset = function visualizaOnset(index) {
    const i = Math.max(0, Math.min(15, Math.floor(index)));
    return state.onsets[i] || 0;
  };

  function setStatus(text) {
    const el = document.getElementById("status");
    if (el) el.textContent = text;
  }

  function connect() {
    const proto = location.protocol === "https:" ? "wss:" : "ws:";
    const ws = new WebSocket(proto + "//" + location.host + "/metrics");
    ws.onopen = function () {
      state.connected = true;
      setStatus("connected: " + state.sketch);
    };
    ws.onmessage = function (event) {
      try {
        const next = JSON.parse(event.data);
        for (const key of Object.keys(next)) state[key] = next[key];
        if (!Array.isArray(state.bands)) state.bands = defaults.bands.slice();
        if (!Array.isArray(state.onsets)) state.onsets = defaults.onsets.slice();
        state.connected = true;
        state.lastMessageTime = performance.now();
      } catch (err) {
        console.warn("bad metrics frame", err);
      }
    };
    ws.onclose = function () {
      state.connected = false;
      setStatus("disconnected; reconnecting...");
      setTimeout(connect, 500);
    };
    ws.onerror = function () {
      ws.close();
    };
  }

  function startFrameCapture() {
    if (!captureEnabled) return;
    const proto = location.protocol === "https:" ? "wss:" : "ws:";
    let ws = null;
    let lastCapture = 0;
    const captureCanvas = document.createElement("canvas");
    const captureCtx = captureCanvas.getContext("2d", { willReadFrequently: true });

    function connectFrames() {
      ws = new WebSocket(proto + "//" + location.host + "/frames");
      ws.binaryType = "arraybuffer";
      ws.onclose = function () { setTimeout(connectFrames, 500); };
      ws.onerror = function () { ws.close(); };
    }

    function capture(now) {
      requestAnimationFrame(capture);
      if (!ws || ws.readyState !== WebSocket.OPEN) return;
      if (now - lastCapture < 1000 / captureFps) return;
      const source = document.querySelector("canvas");
      if (!source || !captureCtx) return;
      const width = captureWidth;
      const height = captureHeight;
      if (captureCanvas.width !== width || captureCanvas.height !== height) {
        captureCanvas.width = width;
        captureCanvas.height = height;
      }
      try {
        captureCtx.drawImage(source, 0, 0, width, height);
        const image = captureCtx.getImageData(0, 0, width, height);
        const packet = new ArrayBuffer(8 + image.data.byteLength);
        const view = new DataView(packet);
        view.setUint32(0, width, true);
        view.setUint32(4, height, true);
        new Uint8Array(packet, 8).set(image.data);
        ws.send(packet);
        lastCapture = now;
      } catch (err) {
        console.warn("p5 frame capture failed", err);
      }
    }

    connectFrames();
    requestAnimationFrame(capture);
  }

  function loadSketch() {
    const script = document.createElement("script");
    script.src = "/sketches/" + encodeURIComponent(state.sketch);
    script.onerror = function () {
      setStatus("failed to load sketch: " + state.sketch);
    };
    document.body.appendChild(script);
  }

  connect();
  startFrameCapture();
  loadSketch();
})();
