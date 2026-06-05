#version 150
precision highp float;

uniform vec2 u_resolution;
uniform float u_time;
uniform float u_energyLevel;  // 0..1 overall energy
uniform float u_beatPulse;    // 0..1 beat-synced pulse
out vec4 FragColor;

const float TWO_PI = 6.28318530718;
const float ANG_CELLS = 30.0;        // 30 dots per ring (the i += 12 loop)

// 1D value noise (stand-in for Processing's noise()).
float hash(float x) { return fract(sin(x * 127.1) * 43758.5453123); }
float vnoise(float x) {
    float i = floor(x), f = fract(x);
    return mix(hash(i), hash(i + 1.0), f * f * (3.0 - 2.0 * f));
}

// Reinterpretation of the Processing point-scatter as an orbiting dot field:
//   point( sin(i + noise(n+f)*100 + n)*n , cos(i + n)*n )
// Dots sit on concentric rings (radius n); "+ n" winds them into a slow spiral,
// and the per-ring noise knocks each dot off its circle -> asteroid-field feel.
// We gather per pixel: check the nearest few ring/angle cells for a dot.
void main() {
    vec2 q = (2.0 * gl_FragCoord.xy - u_resolution) / u_resolution.y;
    float r = length(q);
    float a = atan(q.y, q.x);
    if (a < 0.0) a += TWO_PI;

    const float dr = 0.045;          // ring spacing
    const float wind = 7.0;          // how fast rings rotate with radius ("+ n")
    float ringF = r / dr;

    vec3 col = vec3(0.0);
    for (int ri = -1; ri <= 1; ri++) {
        float ring = floor(ringF) + float(ri);
        if (ring < 1.0) continue;
        float rd = ring * dr;                                  // this ring's radius

        // Per-ring time-scrolling noise: angular wobble (the noise(n+f)*100 term),
        // which makes the dots drift/orbit instead of sitting on clean circles.
        float wob = (vnoise(ring * 0.6 + u_time * 0.4) - 0.5) * 1.4;
        float rot = rd * wind + wob + u_time * 0.15;           // ring rotation

        // Nearest angular cell, then scan its neighbours for the closest dot.
        float ac = floor((a - rot) / (TWO_PI / ANG_CELLS) + 0.5);
        for (int ai = -1; ai <= 1; ai++) {
            float k = ac + float(ai);
            float theta = rot + k * (TWO_PI / ANG_CELLS);
            vec2 dotPos = vec2(cos(theta), sin(theta)) * rd;
            float dd = length(q - dotPos);
            float dot = smoothstep(0.010, 0.0, dd);            // round dot

            float bright = clamp(mod(k, ANG_CELLS) / 25.0, 0.05, 1.0); // map(i,0,300,..)
            col += vec3(bright) * dot;
        }
    }

    // Fade the singular center and the field edge.
    col *= smoothstep(1.2, 0.15, r);
    col *= 0.85 + 0.5 * u_energyLevel;                          // gentle audio lift
    col += col * 2.0 * u_beatPulse * u_beatPulse;               // beat flash

    FragColor = vec4(col, 1.0);
}
