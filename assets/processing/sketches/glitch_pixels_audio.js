"use strict";

let d;
let red;
let green;
let blue;
let rad = 0.1;
let audioX = 200;
let audioY = 200;

function setup() {
  createCanvas(400, 400);
  pixelDensity(1);
  background(0);
}

function value(name) {
  const a = window.visualizaAudio || {};
  const v = Number(a[name] || 0);
  return constrain(Number.isFinite(v) ? v : 0, 0, 1);
}

function moveAudioMouse() {
  const low = value("kick") * 0.7 + value("bass") + value("sub") * 0.5;
  const mid = value("harmonic") + value("lead");
  const high = value("air") + value("percussive");
  const sum = max(0.001, low + mid + high);

  const x = ((low * 0.18) + (mid * 0.52) + (high * 0.86)) / sum;
  const y = 0.82 - constrain(value("energy") * 0.55 + value("tension") * 0.25, 0, 1) * 0.64;

  audioX = lerp(audioX, x * width, 0.16);
  audioY = lerp(audioY, y * height, 0.16 + value("kick") * 0.18);
}

function draw() {
  moveAudioMouse();
  loadPixels();

  for (let i = 0; i < height; i++) {
    for (let j = 0; j < width; j++) {
      const index = (i * width + j) * 4;
      const c = ((pixels[index] << 16) | (pixels[index + 1] << 8) | pixels[index + 2]) >>> 0;

      red = (c << 7) & 0xff;
      green = (c << 4) & 0xaa;
      blue = c & 0xff;

      d = dist(audioX, audioY, j, i) * 1.4;
      d = max(0.7, d);

      red += 50 / d - rad;
      green += 50 / d - rad;
      blue += 155 / d - rad;

      pixels[index] = red;
      pixels[index + 1] = green;
      pixels[index + 2] = blue;
      pixels[index + 3] = 255;
    }
  }

  updatePixels();
}
