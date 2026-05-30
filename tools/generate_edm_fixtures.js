#!/usr/bin/env node

const fs = require("fs");
const path = require("path");

const SAMPLE_RATE = 48000;
const BLOCK_SIZE = 1024;
const TAU = Math.PI * 2.0;
const ROLES = ["kick", "bass", "harmonic", "lead", "air", "perc"];

function clamp01(x) {
  return Math.max(0.0, Math.min(1.0, x));
}

function followEnvelope(current, target, dtSec, attackSec, releaseSec) {
  const tau = target > current ? attackSec : releaseSec;
  const alpha = clamp01(dtSec / Math.max(1e-4, tau));
  return current + (target - current) * alpha;
}

function smoothSemanticLane(values, dtSec, attackSec, releaseSec) {
  const out = new Array(values.length).fill(0.0);
  let state = 0.0;
  for (let i = 0; i < values.length; i += 1) {
    state = followEnvelope(state, values[i], dtSec, attackSec, releaseSec);
    out[i] = clamp01(state);
  }
  return out;
}

function smoothstep(a, b, x) {
  const t = clamp01((x - a) / Math.max(1e-9, b - a));
  return t * t * (3.0 - 2.0 * t);
}

function lerp(a, b, t) {
  return a + (b - a) * t;
}

function ensureDir(dir) {
  fs.mkdirSync(dir, { recursive: true });
}

function parseArgs(argv) {
  const out = {
    outDir: path.resolve("build/generated_edm_fixtures"),
    sampleRate: SAMPLE_RATE,
    blockSize: BLOCK_SIZE,
  };
  for (let i = 2; i < argv.length; i += 1) {
    const arg = argv[i];
    if (arg === "--out-dir" && i + 1 < argv.length) {
      out.outDir = path.resolve(argv[++i]);
    } else if (arg === "--sample-rate" && i + 1 < argv.length) {
      out.sampleRate = Math.max(8000, Number(argv[++i]) | 0);
    } else if (arg === "--block-size" && i + 1 < argv.length) {
      out.blockSize = Math.max(64, Number(argv[++i]) | 0);
    } else {
      throw new Error(`Unknown argument: ${arg}`);
    }
  }
  return out;
}

function makeRng(seed) {
  let state = seed >>> 0;
  return () => {
    state = (1664525 * state + 1013904223) >>> 0;
    return state / 0x100000000;
  };
}

function panGains(pan) {
  const t = clamp01((pan + 1.0) * 0.5);
  return {
    left: Math.cos(t * Math.PI * 0.5),
    right: Math.sin(t * Math.PI * 0.5),
  };
}

function createFixtureBuffer(sampleCount, frameCount) {
  const left = new Float32Array(sampleCount);
  const right = new Float32Array(sampleCount);
  const frameEnergy = {};
  for (const role of ROLES) {
    frameEnergy[role] = new Float64Array(frameCount);
  }
  return { left, right, frameEnergy };
}

function accumulateRole(frameEnergy, role, sampleIndex, blockSize, left, right) {
  const frameIndex = Math.min(frameEnergy[role].length - 1, Math.floor(sampleIndex / blockSize));
  frameEnergy[role][frameIndex] += 0.5 * (left * left + right * right);
}

function writeSample(buffer, role, sampleIndex, blockSize, value, pan) {
  if (sampleIndex < 0 || sampleIndex >= buffer.left.length) return;
  const gains = panGains(pan);
  const left = value * gains.left;
  const right = value * gains.right;
  buffer.left[sampleIndex] += left;
  buffer.right[sampleIndex] += right;
  if (role) {
    accumulateRole(buffer.frameEnergy, role, sampleIndex, blockSize, left, right);
  }
}

function oscSaw(freq, t) {
  let sum = 0.0;
  for (let h = 1; h <= 5; h += 1) {
    sum += Math.sin(TAU * freq * h * t) / h;
  }
  return (2.0 / Math.PI) * sum;
}

function oscSquare(freq, t) {
  let sum = 0.0;
  for (let h = 1; h <= 7; h += 2) {
    sum += Math.sin(TAU * freq * h * t) / h;
  }
  return (4.0 / Math.PI) * sum;
}

function renderKick(buffer, sampleRate, blockSize, startSec, level, rng) {
  const start = Math.floor(startSec * sampleRate);
  const duration = Math.floor(sampleRate * 0.22);
  let phase = 0.0;
  for (let i = 0; i < duration; i += 1) {
    const t = i / sampleRate;
    const env = Math.exp(-t * 24.0);
    const freq = 46.0 + 132.0 * Math.exp(-t * 30.0);
    phase += TAU * freq / sampleRate;
    const click = (rng() * 2.0 - 1.0) * Math.exp(-t * 260.0) * 0.12;
    const sample = level * (Math.sin(phase) * env * 0.95 + click);
    writeSample(buffer, "kick", start + i, blockSize, sample, 0.0);
  }
}

function renderBass(buffer, sampleRate, blockSize, startSec, noteHz, durationSec, level, pan = -0.35) {
  const start = Math.floor(startSec * sampleRate);
  const duration = Math.floor(durationSec * sampleRate);
  for (let i = 0; i < duration; i += 1) {
    const t = i / sampleRate;
    const attack = smoothstep(0.0, 0.015, t);
    const release = 1.0 - smoothstep(durationSec * 0.72, durationSec, t);
    const env = attack * release;
    const wave = 0.68 * Math.sin(TAU * noteHz * t) + 0.25 * Math.sin(TAU * noteHz * 2.0 * t) + 0.12 * Math.sin(TAU * noteHz * 3.0 * t);
    writeSample(buffer, "bass", start + i, blockSize, level * env * wave, pan);
  }
}

function renderChord(buffer, sampleRate, blockSize, startSec, rootHz, durationSec, level, pan = 0.22) {
  const start = Math.floor(startSec * sampleRate);
  const duration = Math.floor(durationSec * sampleRate);
  const intervals = [1.0, Math.pow(2.0, 3.0 / 12.0), Math.pow(2.0, 7.0 / 12.0)];
  for (let i = 0; i < duration; i += 1) {
    const t = i / sampleRate;
    const attack = smoothstep(0.0, 0.05, t);
    const release = 1.0 - smoothstep(durationSec * 0.55, durationSec, t);
    const env = attack * release;
    let wave = 0.0;
    for (let n = 0; n < intervals.length; n += 1) {
      wave += oscSaw(rootHz * intervals[n], t) * (0.75 - n * 0.15);
    }
    wave /= intervals.length;
    writeSample(buffer, "harmonic", start + i, blockSize, level * env * wave * 0.65, pan);
  }
}

function renderLead(buffer, sampleRate, blockSize, startSec, noteHz, durationSec, level, pan = 0.45) {
  const start = Math.floor(startSec * sampleRate);
  const duration = Math.floor(durationSec * sampleRate);
  for (let i = 0; i < duration; i += 1) {
    const t = i / sampleRate;
    const attack = smoothstep(0.0, 0.01, t);
    const release = 1.0 - smoothstep(durationSec * 0.45, durationSec, t);
    const env = attack * release;
    const vibrato = 1.0 + 0.003 * Math.sin(TAU * 5.0 * t);
    const wave = 0.56 * oscSaw(noteHz * vibrato, t) + 0.28 * oscSquare(noteHz * 2.0 * vibrato, t) + 0.16 * Math.sin(TAU * noteHz * 3.0 * t);
    writeSample(buffer, "lead", start + i, blockSize, level * env * wave * 0.52, pan);
  }
}

function renderHat(buffer, sampleRate, blockSize, startSec, durationSec, level, rng, pan = -0.55) {
  const start = Math.floor(startSec * sampleRate);
  const duration = Math.floor(durationSec * sampleRate);
  let prev = 0.0;
  for (let i = 0; i < duration; i += 1) {
    const t = i / sampleRate;
    const env = Math.exp(-t * (durationSec < 0.1 ? 85.0 : 24.0));
    const white = rng() * 2.0 - 1.0;
    const hp = white - prev * 0.96;
    prev = white;
    writeSample(buffer, "air", start + i, blockSize, level * env * hp * 0.26, pan);
  }
}

function renderClap(buffer, sampleRate, blockSize, startSec, level, rng, pan = 0.62) {
  const start = Math.floor(startSec * sampleRate);
  const duration = Math.floor(sampleRate * 0.18);
  let prev = 0.0;
  for (let i = 0; i < duration; i += 1) {
    const t = i / sampleRate;
    const burst = Math.exp(-Math.pow((t - 0.0) * 75.0, 2.0))
      + 0.8 * Math.exp(-Math.pow((t - 0.015) * 75.0, 2.0))
      + 0.6 * Math.exp(-Math.pow((t - 0.031) * 75.0, 2.0));
    const white = rng() * 2.0 - 1.0;
    const hp = white - prev * 0.92;
    prev = white;
    writeSample(buffer, "perc", start + i, blockSize, level * burst * hp * 0.24, pan);
  }
}

function renderRiser(buffer, sampleRate, blockSize, startSec, durationSec, level, rng, pan = 0.0) {
  const start = Math.floor(startSec * sampleRate);
  const duration = Math.floor(durationSec * sampleRate);
  let prev = 0.0;
  for (let i = 0; i < duration; i += 1) {
    const t = i / sampleRate;
    const build = smoothstep(0.0, durationSec, t);
    const white = rng() * 2.0 - 1.0;
    const hp = white - prev * lerp(0.85, 0.98, build);
    prev = white;
    writeSample(buffer, "air", start + i, blockSize, level * build * hp * 0.12, pan);
  }
}

function renderFixtureAudio(fixture, sampleRate, blockSize) {
  const beatSec = 60.0 / fixture.bpm;
  const barSec = beatSec * 4.0;
  const durationSec = fixture.bars * barSec;
  const sampleCount = Math.ceil(durationSec * sampleRate);
  const frameCount = Math.ceil(sampleCount / blockSize);
  const buffer = createFixtureBuffer(sampleCount, frameCount);
  const rng = makeRng(fixture.seed);

  const bassNotes = fixture.bassNotes;
  const chordRoots = fixture.chordRoots;
  const leadScale = fixture.leadScale;
  const chordBeats = fixture.chordBeats || [0, 2];
  const chordDurationMul = fixture.chordDurationMul || 1.65;
  const leadStepStride = fixture.leadStepStride || 1;
  const leadDurationMul = fixture.leadDurationMul || 0.28;
  const bassEveryStep = fixture.bassEveryStep || false;
  const bassDurationMul = fixture.bassDurationMul || 0.34;
  const kickBeats = fixture.kickBeats || [0, 1, 2, 3];
  const clapBeats = fixture.clapBeats || [1, 3];
  const bassStepPattern = fixture.bassStepPattern || null;
  const leadStepPattern = fixture.leadStepPattern || null;
  const swing = fixture.swing || 0.0;
  const hatShortDuration = fixture.hatShortDuration || 0.06;
  const hatLongDuration = fixture.hatLongDuration || 0.16;

  for (let bar = 0; bar < fixture.bars; bar += 1) {
    const section = fixture.sectionAtBar(bar, fixture.bars);
    const barStart = bar * barSec;
    const bassHz = bassNotes[bar % bassNotes.length];
    const chordHz = chordRoots[bar % chordRoots.length];

    for (let beat = 0; beat < 4; beat += 1) {
      const beatStart = barStart + beat * beatSec;
      if (section.kick > 0.0 && kickBeats.includes(beat)) {
        renderKick(buffer, sampleRate, blockSize, beatStart, 0.95 * section.kick, rng);
      }

      if (section.perc > 0.0 && clapBeats.includes(beat)) {
        renderClap(buffer, sampleRate, blockSize, beatStart + beatSec * 0.02, 0.88 * section.perc, rng);
      }

      if (section.harmonic > 0.0 && chordBeats.includes(beat)) {
        renderChord(buffer, sampleRate, blockSize, beatStart, chordHz, beatSec * chordDurationMul, 0.62 * section.harmonic, beat === 0 ? -0.18 : 0.18);
      }
    }

    for (let step = 0; step < 8; step += 1) {
      const stepStart = barStart + step * beatSec * 0.5 + ((step % 2 === 1) ? beatSec * 0.5 * swing : 0.0);
      if (section.air > 0.0) {
        const hatLevel = step % 4 === 3 ? 0.24 : 0.17;
        renderHat(buffer, sampleRate, blockSize, stepStart, step % 4 === 3 ? hatLongDuration : hatShortDuration, hatLevel * section.air, rng, step % 2 === 0 ? -0.42 : 0.42);
      }
      const bassStepEnabled = bassStepPattern ? bassStepPattern.includes(step) : (bassEveryStep || step % 2 === 1);
      if (section.bass > 0.0 && bassStepEnabled) {
        renderBass(buffer, sampleRate, blockSize, stepStart + beatSec * 0.02, bassHz, beatSec * bassDurationMul, 0.72 * section.bass);
      }
      const leadStepEnabled = leadStepPattern ? leadStepPattern.includes(step) : (step % leadStepStride === 0);
      if (section.lead > 0.0 && leadStepEnabled) {
        const leadHz = leadScale[(bar * 8 + step) % leadScale.length];
        renderLead(buffer, sampleRate, blockSize, stepStart, leadHz, beatSec * leadDurationMul, 0.58 * section.lead, step % 2 === 0 ? 0.35 : -0.35);
      }
    }

    if (section.airRiser > 0.0) {
      renderRiser(buffer, sampleRate, blockSize, barStart, barSec, 0.7 * section.airRiser, rng);
    }
  }

  return {
    buffer,
    durationSec,
    beatSec,
    barSec,
    frameCount,
  };
}

function buildTruthFrames(fixture, render, sampleRate, blockSize) {
  const frames = [];
  const roleValues = {};
  const roleRms = {};
  let globalRolePeak = 1e-9;
  for (const role of ROLES) {
    const rms = Array.from(render.buffer.frameEnergy[role], (sumSq) => Math.sqrt(sumSq / blockSize));
    roleRms[role] = rms;
    for (const v of rms) {
      globalRolePeak = Math.max(globalRolePeak, v);
    }
  }
  for (const role of ROLES) {
    roleValues[role] = Array.from(roleRms[role], (v) => clamp01(v / globalRolePeak));
  }
  const dtSec = blockSize / sampleRate;
  const kickSemanticInput = roleValues.kick.map((v, i) => clamp01(Math.max(v, 0.08 * roleValues.perc[i])));
  const bassSemanticInput = roleValues.bass.map((v, i) => clamp01(Math.max(v, 0.18 * roleValues.kick[i], 0.12 * roleValues.harmonic[i])));
  const harmonicSemanticInput = roleValues.harmonic.map((v, i) => clamp01(Math.max(v, 0.40 * roleValues.lead[i], 0.08 * roleValues.bass[i])));
  const leadSemanticInput = roleValues.lead.map((v, i) => clamp01(Math.max(v, 0.28 * roleValues.harmonic[i])));
  const airSemanticInput = roleValues.air.map((v, i) => clamp01(Math.max(v, 0.30 * roleValues.perc[i], 0.18 * roleValues.lead[i], 0.06 * roleValues.harmonic[i])));
  const percSemanticInput = roleValues.perc.map((v, i) => clamp01(Math.max(v, 0.12 * roleValues.air[i], 0.06 * roleValues.kick[i])));
  const semanticRoleValues = {
    kick: smoothSemanticLane(kickSemanticInput, dtSec, 0.005, 0.070),
    bass: smoothSemanticLane(bassSemanticInput, dtSec, 0.050, 0.260),
    harmonic: smoothSemanticLane(harmonicSemanticInput, dtSec, 0.070, 0.320),
    lead: smoothSemanticLane(leadSemanticInput, dtSec, 0.050, 0.260),
    air: smoothSemanticLane(airSemanticInput, dtSec, 0.030, 0.220),
    perc: smoothSemanticLane(percSemanticInput, dtSec, 0.010, 0.100),
  };

  const energyRaw = new Array(render.frameCount).fill(0.0);
  const tensionRaw = new Array(render.frameCount).fill(0.0);
  const releaseRaw = new Array(render.frameCount).fill(0.0);

  for (let frameIndex = 0; frameIndex < render.frameCount; frameIndex += 1) {
    const timestampSec = frameIndex * blockSize / sampleRate;
    const barPos = timestampSec / render.barSec;
    const section = fixture.sectionAtTime(timestampSec, render.durationSec);
    const kick = semanticRoleValues.kick[frameIndex];
    const bass = semanticRoleValues.bass[frameIndex];
    const harmonic = semanticRoleValues.harmonic[frameIndex];
    const lead = semanticRoleValues.lead[frameIndex];
    const air = semanticRoleValues.air[frameIndex];
    const perc = semanticRoleValues.perc[frameIndex];

    energyRaw[frameIndex] = clamp01(0.22 * kick + 0.22 * bass + 0.20 * harmonic + 0.15 * lead + 0.11 * air + 0.10 * perc);
    tensionRaw[frameIndex] = clamp01(0.70 * section.tension + 0.10 * lead + 0.12 * air + 0.08 * perc);
    releaseRaw[frameIndex] = clamp01(0.80 * section.release + 0.20 * (1.0 - energyRaw[frameIndex]));

    frames.push({
      block_index: frameIndex,
      timestamp_sec: Number(timestampSec.toFixed(8)),
      bar_position: Number(barPos.toFixed(4)),
      kick,
      bass,
      harmonic,
      lead,
      air,
      perc,
    });
  }

  const energyValues = smoothSemanticLane(energyRaw, dtSec, 0.10, 0.30);
  const tensionValues = smoothSemanticLane(tensionRaw, dtSec, 0.35, 0.80);
  const releaseValues = smoothSemanticLane(releaseRaw, dtSec, 0.20, 0.60);
  for (let i = 0; i < frames.length; i += 1) {
    frames[i].energy = clamp01(energyValues[i]);
    frames[i].tension = clamp01(tensionValues[i]);
    frames[i].release = clamp01(releaseValues[i]);
  }
  return frames;
}

function peakAbs(array) {
  let peak = 1e-9;
  for (let i = 0; i < array.length; i += 1) {
    peak = Math.max(peak, Math.abs(array[i]));
  }
  return peak;
}

function writeWav(filePath, left, right, sampleRate) {
  const sampleCount = left.length;
  const channels = 2;
  const bytesPerSample = 2;
  const dataSize = sampleCount * channels * bytesPerSample;
  const buffer = Buffer.alloc(44 + dataSize);
  buffer.write("RIFF", 0);
  buffer.writeUInt32LE(36 + dataSize, 4);
  buffer.write("WAVE", 8);
  buffer.write("fmt ", 12);
  buffer.writeUInt32LE(16, 16);
  buffer.writeUInt16LE(1, 20);
  buffer.writeUInt16LE(channels, 22);
  buffer.writeUInt32LE(sampleRate, 24);
  buffer.writeUInt32LE(sampleRate * channels * bytesPerSample, 28);
  buffer.writeUInt16LE(channels * bytesPerSample, 32);
  buffer.writeUInt16LE(16, 34);
  buffer.write("data", 36);
  buffer.writeUInt32LE(dataSize, 40);

  let offset = 44;
  for (let i = 0; i < sampleCount; i += 1) {
    const l = Math.max(-1.0, Math.min(1.0, left[i]));
    const r = Math.max(-1.0, Math.min(1.0, right[i]));
    buffer.writeInt16LE(Math.round(l * 32767.0), offset);
    buffer.writeInt16LE(Math.round(r * 32767.0), offset + 2);
    offset += 4;
  }
  fs.writeFileSync(filePath, buffer);
}

function fixtureDefinitions() {
  const bassNotes = [55.0, 49.0, 61.74, 46.25];
  const chordRoots = [220.0, 196.0, 246.94, 185.0];
  const leadScale = [440.0, 493.88, 554.37, 659.25, 554.37, 493.88, 440.0, 369.99];

  return [
    {
      id: "semantic_groove_short",
      title: "Deterministic semantic groove",
      split: "dev",
      style: "edm_groove",
      focus_roles: ["kick", "bass", "harmonic", "lead", "air", "perc", "energy"],
      seed: 0x12a4beef,
      bpm: 132,
      bars: 8,
      bassNotes,
      chordRoots,
      leadScale,
      sectionAtBar() {
        return { kick: 1.0, bass: 0.85, harmonic: 0.62, lead: 0.30, air: 0.52, perc: 0.54, airRiser: 0.0, tension: 0.28, release: 0.08 };
      },
      sectionAtTime() {
        return { tension: 0.28, release: 0.08 };
      },
    },
    {
      id: "semantic_build_drop",
      title: "Deterministic build and drop",
      split: "dev",
      style: "edm_build_drop",
      focus_roles: ["kick", "bass", "air", "perc", "energy", "tension", "release"],
      seed: 0x52f1c0de,
      bpm: 132,
      bars: 16,
      bassNotes,
      chordRoots,
      leadScale,
      sectionAtBar(bar) {
        if (bar < 4) return { kick: 0.85, bass: 0.48, harmonic: 0.36, lead: 0.10, air: 0.30, perc: 0.28, airRiser: 0.0, tension: 0.18, release: 0.10 };
        if (bar < 8) {
          const t = (bar - 4) / 4.0;
          return { kick: 0.82, bass: lerp(0.40, 0.18, t), harmonic: lerp(0.42, 0.28, t), lead: lerp(0.18, 0.40, t), air: lerp(0.48, 0.92, t), perc: lerp(0.34, 0.70, t), airRiser: lerp(0.20, 0.85, t), tension: lerp(0.35, 1.0, t), release: 0.0 };
        }
        if (bar < 12) return { kick: 1.0, bass: 1.0, harmonic: 0.82, lead: 0.58, air: 0.64, perc: 0.74, airRiser: 0.0, tension: 0.40, release: 0.08 };
        const t = (bar - 12) / 4.0;
        return { kick: lerp(0.72, 0.28, t), bass: lerp(0.66, 0.18, t), harmonic: lerp(0.58, 0.32, t), lead: lerp(0.34, 0.12, t), air: lerp(0.44, 0.24, t), perc: lerp(0.46, 0.16, t), airRiser: 0.0, tension: lerp(0.18, 0.05, t), release: lerp(0.35, 1.0, t) };
      },
      sectionAtTime(timeSec, durationSec) {
        const t = timeSec / durationSec;
        if (t < 0.25) return { tension: 0.18, release: 0.10 };
        if (t < 0.50) return { tension: smoothstep(0.25, 0.50, t), release: 0.02 };
        if (t < 0.75) return { tension: 0.42, release: 0.08 };
        return { tension: lerp(0.20, 0.04, smoothstep(0.75, 1.0, t)), release: lerp(0.42, 1.0, smoothstep(0.75, 1.0, t)) };
      },
    },
    {
      id: "semantic_lead_air_showcase",
      title: "Deterministic lead and air focus",
      split: "dev",
      style: "lead_air_showcase",
      focus_roles: ["lead", "air", "release"],
      seed: 0x0badcafe,
      bpm: 136,
      bars: 10,
      bassNotes: [61.74, 55.0, 65.41, 58.27],
      chordRoots: [246.94, 220.0, 261.63, 233.08],
      leadScale: [659.25, 739.99, 830.61, 987.77, 830.61, 739.99, 659.25, 554.37],
      sectionAtBar(bar) {
        const pulse = bar % 2 === 0 ? 1.0 : 0.85;
        return { kick: 0.72 * pulse, bass: 0.42, harmonic: 0.46, lead: 0.78, air: 0.86, perc: 0.34, airRiser: bar >= 6 ? 0.28 : 0.0, tension: bar >= 6 ? 0.60 : 0.32, release: bar >= 8 ? 0.24 : 0.04 };
      },
      sectionAtTime(timeSec, durationSec) {
        const t = timeSec / durationSec;
        return {
          tension: t < 0.55 ? 0.34 : lerp(0.42, 0.72, smoothstep(0.55, 0.85, t)),
          release: t < 0.80 ? 0.05 : lerp(0.14, 0.52, smoothstep(0.80, 1.0, t)),
        };
      },
    },
    {
      id: "semantic_bass_focus",
      title: "Deterministic bass focus",
      split: "dev",
      style: "bass_focus",
      focus_roles: ["bass", "energy"],
      seed: 0x51a551ed,
      bpm: 130,
      bars: 8,
      bassNotes: [43.65, 49.0, 55.0, 61.74],
      chordRoots: [174.61, 196.0, 220.0, 246.94],
      leadScale,
      bassEveryStep: true,
      bassDurationMul: 0.90,
      sectionAtBar(bar) {
        const pulse = bar % 2 === 0 ? 1.0 : 0.82;
        return { kick: 0.0, bass: 1.0 * pulse, harmonic: 0.0, lead: 0.0, air: 0.0, perc: 0.0, airRiser: 0.0, tension: 0.10, release: 0.08 };
      },
      sectionAtTime(timeSec, durationSec) {
        const t = timeSec / durationSec;
        return { tension: 0.10, release: lerp(0.05, 0.18, smoothstep(0.75, 1.0, t)) };
      },
    },
    {
      id: "semantic_harmonic_focus",
      title: "Deterministic harmonic stabs",
      split: "dev",
      style: "harmonic_focus",
      focus_roles: ["harmonic", "energy"],
      seed: 0x6badf00d,
      bpm: 124,
      bars: 8,
      bassNotes,
      chordRoots: [440.0, 523.25, 392.0, 493.88],
      leadScale,
      chordBeats: [0],
      chordDurationMul: 3.6,
      sectionAtBar() {
        return { kick: 0.0, bass: 0.0, harmonic: 1.0, lead: 0.0, air: 0.0, perc: 0.0, airRiser: 0.0, tension: 0.18, release: 0.20 };
      },
      sectionAtTime(timeSec, durationSec) {
        const beatSec = 60.0 / 124.0;
        const local = timeSec % (beatSec * 2.0);
        const gate = local < beatSec * 0.95 ? 0.2 : 0.65;
        return { tension: 0.16, release: gate };
      },
    },
    {
      id: "semantic_lead_focus",
      title: "Deterministic lead focus",
      split: "dev",
      style: "lead_focus",
      focus_roles: ["lead", "release"],
      seed: 0x1eed5eed,
      bpm: 138,
      bars: 8,
      bassNotes,
      chordRoots,
      leadScale: [659.25, 830.61, 739.99, 987.77, 830.61, 739.99, 659.25, 554.37],
      leadStepStride: 4,
      leadDurationMul: 1.8,
      sectionAtBar() {
        return { kick: 0.0, bass: 0.0, harmonic: 0.0, lead: 1.0, air: 0.0, perc: 0.0, airRiser: 0.0, tension: 0.34, release: 0.12 };
      },
      sectionAtTime(timeSec, durationSec) {
        const t = timeSec / durationSec;
        return { tension: lerp(0.24, 0.42, smoothstep(0.2, 0.8, t)), release: t < 0.82 ? 0.08 : lerp(0.15, 0.62, smoothstep(0.82, 1.0, t)) };
      },
    },
    {
      id: "semantic_air_focus",
      title: "Deterministic air focus",
      split: "dev",
      style: "air_focus",
      focus_roles: ["air", "perc"],
      seed: 0x0ff1ce42,
      bpm: 140,
      bars: 8,
      bassNotes,
      chordRoots,
      leadScale,
      hatShortDuration: 0.11,
      hatLongDuration: 0.24,
      sectionAtBar(bar) {
        const build = bar >= 4 ? 0.9 : 0.55;
        return { kick: 0.0, bass: 0.0, harmonic: 0.0, lead: 0.0, air: build, perc: 0.36, airRiser: bar >= 4 ? 0.55 : 0.0, tension: bar >= 4 ? 0.78 : 0.34, release: 0.10 };
      },
      sectionAtTime(timeSec, durationSec) {
        const t = timeSec / durationSec;
        return { tension: t < 0.5 ? 0.34 : lerp(0.45, 0.92, smoothstep(0.5, 1.0, t)), release: 0.08 };
      },
    },
    {
      id: "semantic_release_focus",
      title: "Deterministic release focus",
      split: "dev",
      style: "release_focus",
      focus_roles: ["release", "energy", "tension"],
      seed: 0x1234abcd,
      bpm: 132,
      bars: 8,
      bassNotes,
      chordRoots,
      leadScale,
      sectionAtBar(bar) {
        if (bar < 4) return { kick: 0.9, bass: 0.75, harmonic: 0.58, lead: 0.22, air: 0.30, perc: 0.44, airRiser: 0.0, tension: 0.42, release: 0.05 };
        return { kick: 0.0, bass: 0.0, harmonic: 0.0, lead: 0.0, air: 0.0, perc: 0.0, airRiser: 0.0, tension: 0.04, release: 1.0 };
      },
      sectionAtTime(timeSec, durationSec) {
        const half = durationSec * 0.5;
        if (timeSec < half) return { tension: 0.42, release: 0.05 };
        const t = smoothstep(half, durationSec, timeSec);
        return { tension: lerp(0.20, 0.0, t), release: lerp(0.45, 1.0, t) };
      },
    },
    {
      id: "holdout_house_shuffle",
      title: "Holdout house shuffle",
      split: "holdout",
      style: "house_shuffle",
      focus_roles: ["kick", "bass", "harmonic", "perc", "energy"],
      seed: 0x71abc0de,
      bpm: 124,
      bars: 10,
      bassNotes: [55.0, 61.74, 49.0, 58.27],
      chordRoots: [220.0, 246.94, 196.0, 233.08],
      leadScale,
      swing: 0.18,
      bassStepPattern: [1, 3, 5, 7],
      sectionAtBar(bar) {
        const lift = bar >= 6 ? 0.08 : 0.0;
        return { kick: 1.0, bass: 0.82, harmonic: 0.58, lead: 0.12, air: 0.44, perc: 0.64, airRiser: 0.0, tension: 0.22 + lift, release: 0.08 };
      },
      sectionAtTime(timeSec, durationSec) {
        const t = timeSec / durationSec;
        return { tension: lerp(0.20, 0.34, smoothstep(0.55, 0.95, t)), release: t < 0.85 ? 0.08 : lerp(0.12, 0.38, smoothstep(0.85, 1.0, t)) };
      },
    },
    {
      id: "holdout_techno_rumble",
      title: "Holdout techno rumble",
      split: "holdout",
      style: "techno_rumble",
      focus_roles: ["kick", "bass", "energy"],
      seed: 0x44fe1201,
      bpm: 138,
      bars: 10,
      bassNotes: [43.65, 41.2, 46.25, 49.0],
      chordRoots: [174.61, 164.81, 185.0, 196.0],
      leadScale,
      bassEveryStep: true,
      bassDurationMul: 1.35,
      chordBeats: [0],
      chordDurationMul: 2.8,
      sectionAtBar(bar) {
        const density = bar >= 6 ? 1.0 : 0.84;
        return { kick: 1.0, bass: 0.94 * density, harmonic: 0.22, lead: 0.0, air: 0.20, perc: 0.18, airRiser: 0.0, tension: bar >= 7 ? 0.38 : 0.22, release: 0.05 };
      },
      sectionAtTime(timeSec, durationSec) {
        const t = timeSec / durationSec;
        return { tension: lerp(0.20, 0.40, smoothstep(0.45, 0.95, t)), release: t < 0.92 ? 0.04 : lerp(0.08, 0.22, smoothstep(0.92, 1.0, t)) };
      },
    },
    {
      id: "holdout_trance_supersaw",
      title: "Holdout trance supersaw",
      split: "holdout",
      style: "trance_supersaw",
      focus_roles: ["harmonic", "lead", "air", "energy", "tension"],
      seed: 0x1aceb00c,
      bpm: 140,
      bars: 12,
      bassNotes: [46.25, 55.0, 49.0, 61.74],
      chordRoots: [185.0, 220.0, 196.0, 246.94],
      leadScale: [659.25, 739.99, 830.61, 987.77, 1108.73, 987.77, 830.61, 739.99],
      chordBeats: [0],
      chordDurationMul: 3.9,
      leadStepStride: 2,
      leadDurationMul: 1.1,
      hatShortDuration: 0.08,
      hatLongDuration: 0.20,
      sectionAtBar(bar) {
        if (bar < 4) return { kick: 0.88, bass: 0.52, harmonic: 0.78, lead: 0.34, air: 0.56, perc: 0.22, airRiser: 0.0, tension: 0.28, release: 0.06 };
        if (bar < 8) return { kick: 0.86, bass: 0.44, harmonic: 0.92, lead: 0.62, air: 0.82, perc: 0.26, airRiser: 0.52, tension: 0.70, release: 0.02 };
        return { kick: 1.0, bass: 0.76, harmonic: 1.0, lead: 0.84, air: 0.88, perc: 0.28, airRiser: 0.0, tension: 0.40, release: 0.08 };
      },
      sectionAtTime(timeSec, durationSec) {
        const t = timeSec / durationSec;
        if (t < 0.33) return { tension: 0.28, release: 0.05 };
        if (t < 0.66) return { tension: lerp(0.38, 0.92, smoothstep(0.33, 0.66, t)), release: 0.02 };
        return { tension: lerp(0.52, 0.22, smoothstep(0.66, 1.0, t)), release: lerp(0.08, 0.32, smoothstep(0.80, 1.0, t)) };
      },
    },
    {
      id: "holdout_breakdown_sparse",
      title: "Holdout breakdown sparse",
      split: "holdout",
      style: "breakdown_sparse",
      focus_roles: ["harmonic", "air", "release"],
      seed: 0x5eed0a11,
      bpm: 128,
      bars: 8,
      bassNotes: [65.41, 58.27, 61.74, 55.0],
      chordRoots: [261.63, 233.08, 246.94, 220.0],
      leadScale: [523.25, 659.25, 587.33, 698.46, 659.25, 587.33, 523.25, 493.88],
      kickBeats: [0],
      clapBeats: [],
      chordBeats: [0],
      chordDurationMul: 4.2,
      leadStepPattern: [2, 6],
      leadDurationMul: 1.4,
      hatShortDuration: 0.10,
      hatLongDuration: 0.24,
      sectionAtBar(bar) {
        const finalLift = bar >= 6 ? 0.18 : 0.0;
        return { kick: 0.22, bass: 0.10, harmonic: 0.82, lead: 0.28, air: 0.54 + finalLift, perc: 0.06, airRiser: bar >= 6 ? 0.18 : 0.0, tension: 0.12 + 0.10 * finalLift, release: 0.48 };
      },
      sectionAtTime(timeSec, durationSec) {
        const t = timeSec / durationSec;
        return { tension: lerp(0.12, 0.22, smoothstep(0.70, 1.0, t)), release: lerp(0.40, 0.74, smoothstep(0.0, 1.0, t)) };
      },
    },
  ];
}

function generateFixture(def, options) {
  const render = renderFixtureAudio(def, options.sampleRate, options.blockSize);
  const frames = buildTruthFrames(def, render, options.sampleRate, options.blockSize);

  const peak = Math.max(peakAbs(render.buffer.left), peakAbs(render.buffer.right), 1e-9);
  const gain = 0.9 / peak;
  for (let i = 0; i < render.buffer.left.length; i += 1) {
    render.buffer.left[i] *= gain;
    render.buffer.right[i] *= gain;
  }

  const wavPath = path.join(options.outDir, `${def.id}.wav`);
  const truthPath = path.join(options.outDir, `${def.id}.truth.json`);
  writeWav(wavPath, render.buffer.left, render.buffer.right, options.sampleRate);

  const truth = {
    id: def.id,
    title: def.title,
    split: def.split || "dev",
    style: def.style || "generic",
    focus_roles: def.focus_roles || [],
    sample_rate: options.sampleRate,
    block_size: options.blockSize,
    bpm: def.bpm,
    bars: def.bars,
    duration_sec: Number(render.durationSec.toFixed(6)),
    wav: wavPath,
    frames,
  };
  fs.writeFileSync(truthPath, JSON.stringify(truth, null, 2) + "\n");
  return {
    id: def.id,
    title: def.title,
    split: def.split || "dev",
    style: def.style || "generic",
    focus_roles: def.focus_roles || [],
    wav: wavPath,
    truth: truthPath,
    sample_rate: options.sampleRate,
    block_size: options.blockSize,
    bpm: def.bpm,
    bars: def.bars,
    duration_sec: truth.duration_sec,
  };
}

function main() {
  const options = parseArgs(process.argv);
  ensureDir(options.outDir);
  const manifest = {
    generated_at: new Date().toISOString(),
    sample_rate: options.sampleRate,
    block_size: options.blockSize,
    fixtures: [],
  };

  for (const fixture of fixtureDefinitions()) {
    manifest.fixtures.push(generateFixture(fixture, options));
  }

  const manifestPath = path.join(options.outDir, "generated_fixtures_manifest.json");
  fs.writeFileSync(manifestPath, JSON.stringify(manifest, null, 2) + "\n");
  console.log(JSON.stringify({ out_dir: options.outDir, manifest: manifestPath, fixtures: manifest.fixtures }, null, 2));
}

try {
  main();
} catch (error) {
  console.error(`error: ${error.message}`);
  process.exit(1);
}
