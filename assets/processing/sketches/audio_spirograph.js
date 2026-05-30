"use strict";

let particles = [];
let smooth = {};
let phase = 0;

const roles = [
  { key: "kick", hue: 18, x: 0.18, y: 0.78, radius: 1.6, rate: 34, speed: 9.0, weight: 5.0 },
  { key: "bass", hue: 195, x: 0.25, y: 0.68, radius: 1.2, rate: 26, speed: 4.8, weight: 3.2 },
  { key: "harmonic", hue: 128, x: 0.62, y: 0.36, radius: 1.0, rate: 18, speed: 3.4, weight: 2.6 },
  { key: "lead", hue: 318, x: 0.76, y: 0.28, radius: 0.8, rate: 25, speed: 5.4, weight: 2.8 },
  { key: "air", hue: 52, x: 0.18, y: 0.22, radius: 0.65, rate: 20, speed: 7.2, weight: 1.8 },
  { key: "percussive", hue: 278, x: 0.82, y: 0.70, radius: 0.75, rate: 30, speed: 8.2, weight: 2.1 },
];

function setup() {
  createCanvas(windowWidth, windowHeight);
  pixelDensity(1);
  colorMode(HSB, 360, 100, 100, 1);
  background(4, 16, 4);
  noStroke();
}

function windowResized() {
  resizeCanvas(windowWidth, windowHeight);
  background(4, 16, 4);
}

function audioValue(key) {
  const a = window.visualizaAudio || {};
  return constrain(Number(a[key] || 0), 0, 1);
}

function smoothSignal(key, attack, release) {
  const target = audioValue(key);
  const current = smooth[key] || 0;
  const k = target > current ? attack : release;
  smooth[key] = lerp(current, target, k);
  return smooth[key];
}

function emitRole(role, strength) {
  const n = Math.floor(strength * role.rate + random(strength * role.rate));
  const cx = role.x * width;
  const cy = role.y * height;
  const scale = min(width, height);
  for (let i = 0; i < n; i++) {
    const angle = random(TWO_PI);
    const spread = random(0.006, 0.04) * scale * role.radius;
    const px = cx + cos(angle) * spread;
    const py = cy + sin(angle) * spread;
    const flow = angle + phase * 0.35 + sin(role.hue + frameCount * 0.015) * 0.55;
    const velocity = role.speed * (0.45 + strength * 1.4);
    particles.push({
      x: px,
      y: py,
      vx: cos(flow) * velocity,
      vy: sin(flow) * velocity - strength * 2.8,
      life: 1,
      decay: random(0.010, 0.030) * (1.15 - strength * 0.45),
      hue: role.hue + random(-12, 12),
      sat: 78 + strength * 20,
      bri: 38 + strength * 62,
      size: role.weight * (0.5 + strength * 2.4) * random(0.65, 1.4),
    });
  }
}

function drawField() {
  const a = window.visualizaAudio || {};
  const tension = smoothSignal("tension", 0.035, 0.018);
  const energy = smoothSignal("energy", 0.12, 0.04);
  const drop = smoothSignal("drop", 0.65, 0.12);
  const release = smoothSignal("release", 0.08, 0.025);
  phase += 0.009 + energy * 0.026 + tension * 0.018;

  fill(220, 45, 3, 0.11 + release * 0.08);
  rect(0, 0, width, height);

  blendMode(ADD);
  noFill();
  strokeWeight(1.0 + energy * 2.0);
  const centerX = width * (0.5 + sin(phase * 0.7) * 0.05);
  const centerY = height * (0.5 + cos(phase * 0.9) * 0.05);
  for (let i = 0; i < 9; i++) {
    const band = Array.isArray(a.bands) ? (a.bands[i % a.bands.length] || 0) : 0;
    const radius = min(width, height) * (0.08 + i * 0.045 + band * 0.09 + tension * 0.05);
    const hue = (190 + i * 18 + tension * 90 + drop * 80) % 360;
    stroke(hue, 78, 38 + band * 50 + energy * 30, 0.10 + band * 0.20);
    beginShape();
    for (let t = 0; t <= 96; t++) {
      const p = t / 96;
      const angle = p * TWO_PI + phase * (0.5 + i * 0.04);
      const warp = sin(angle * 3.0 + phase * 2.0 + i) * (8 + band * 36);
      vertex(centerX + cos(angle) * (radius + warp), centerY + sin(angle) * (radius - warp));
    }
    endShape(CLOSE);
  }
  blendMode(BLEND);
}

function drawParticles() {
  blendMode(ADD);
  for (let i = particles.length - 1; i >= 0; i--) {
    const p = particles[i];
    const curl = noise(p.x * 0.0028, p.y * 0.0028, frameCount * 0.009) * TWO_PI * 2.0;
    const pullX = (width * 0.5 - p.x) * 0.0009;
    const pullY = (height * 0.48 - p.y) * 0.0007;
    p.vx += cos(curl) * 0.42 + pullX;
    p.vy += sin(curl) * 0.42 + pullY;
    p.vx *= 0.982;
    p.vy *= 0.982;
    p.x += p.vx;
    p.y += p.vy;
    p.life -= p.decay;

    const alpha = constrain(p.life, 0, 1) * 0.42;
    fill((p.hue + phase * 25) % 360, p.sat, p.bri, alpha);
    circle(p.x, p.y, p.size * (0.6 + p.life * 1.5));

    if (p.life <= 0 || p.x < -80 || p.x > width + 80 || p.y < -80 || p.y > height + 80) {
      particles.splice(i, 1);
    }
  }
  blendMode(BLEND);
}

function drawMeters() {
  const x = 18;
  const y = 18;
  const w = 120;
  const h = 5;
  noStroke();
  for (let i = 0; i < roles.length; i++) {
    const role = roles[i];
    const v = smooth[role.key] || 0;
    fill(role.hue, 80, 80, 0.28);
    rect(x, y + i * 11, w, h);
    fill(role.hue, 90, 100, 0.85);
    rect(x, y + i * 11, w * v, h);
  }
}

function draw() {
  drawField();
  for (const role of roles) {
    const strength = smoothSignal(role.key, 0.38, 0.08);
    emitRole(role, strength);
  }

  const kick = smooth.kick || 0;
  if (kick > 0.18) {
    blendMode(ADD);
    noFill();
    stroke(18, 95, 100, 0.13 + kick * 0.24);
    strokeWeight(1.5 + kick * 5.0);
    circle(width * 0.18, height * 0.78, min(width, height) * (0.08 + kick * 0.34));
    blendMode(BLEND);
  }

  while (particles.length > 900) particles.shift();
  drawParticles();
  drawMeters();
}
