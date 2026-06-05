"use strict";

// Concentric rings of dots whose x-angle is offset by per-ring noise, so they
// drift off their circles and orbit over time. Fixed ring count scaled to the
// canvas (constant cost at any resolution). Audio: kick swells the dots, bass
// adds orbital drift, energy brightens.

const RINGS = 540;

function setup() {
  createCanvas(windowWidth, windowHeight);
  noSmooth();
  background(0);
}

function windowResized() {
  resizeCanvas(windowWidth, windowHeight);
  background(0);
}

function draw() {
  background(0);
  const a = window.visualizaAudio || {};
  const kick = a.kick || 0, bass = a.bass || 0, energy = a.energy || 0;

  const s = min(width, height) / (2 * RINGS);
  strokeWeight(max(1.0, s * 1.4 * (1.0 + 2.0 * kick))); // dots swell on the kick
  const f = frameCount % RINGS;
  translate(width / 2, height / 2);

  for (let n = 0; n < RINGS; n++) {
    for (let i = 0; i < 360; i += 12) {
      stroke(map(i, 0, 300, 0, 255) * (1.5 + 0.8 * energy)); // energy brightens
      const x = sin(radians(i + noise(n + f) * 10 * f/100 * (1.0 + bass) + n)) * n * s; // bass = drift
      const y = cos(radians(i + n)) * n * s;
      point(x, y);
    }
  }
}
