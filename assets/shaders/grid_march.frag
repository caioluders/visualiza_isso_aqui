#version 150
precision highp float;

// Flying through an infinite folded grid: march a ray forward and fold space
// into unit cells with mod(); the color comes from where you land in the cell.
// Ported from a compact Shadertoy-style sketch (r -> u_resolution, t -> u_time).
//
// ===== Available inputs (all audio 0..1 unless noted). Use any of them. =====
uniform vec2  u_resolution;   // viewport size, px
uniform float u_time;         // seconds

// Beat / rhythm
uniform float u_kickImpact;
uniform float u_snareImpact;
uniform float u_hatTick;
uniform float u_beat;         // 0 or 1 (fires on the beat)
uniform float u_beatEnv;
uniform float u_beatPulse;
uniform float u_bpm;          // NOT normalized (~60-180)

// Spectral roles
uniform float u_subBody;
uniform float u_bassBody;
uniform float u_harmonicBody;
uniform float u_leadPresence;
uniform float u_airPresence;
uniform float u_percussiveFocus;

// Energy / structure
uniform float u_energyLevel;
uniform float u_tension;
uniform float u_release;
uniform float u_dropEvent;
uniform float u_sectionChange;
uniform float u_novelty;
uniform float u_brightness;
uniform float u_transientDensity;

// Low-level
uniform float u_rms;
uniform float u_onset;
uniform float u_flux;
uniform float u_bandLow;
uniform float u_bandMid;
uniform float u_bandHigh;
uniform float u_percE;
uniform float u_harmE;
uniform float u_percRatio;
uniform float u_centroidNorm;

// Per-band (low -> high), each bin 0..1
uniform float u_bands[16];
uniform float u_onsets[16];

// Role positions / confidence (0..1)
uniform float u_roleKickCenterNorm;
uniform float u_roleBassCenterNorm;
uniform float u_roleLeadCenterNorm;
uniform float u_roleAirCenterNorm;
uniform float u_roleKickConfidence;
uniform float u_roleBassConfidence;
uniform float u_roleLeadConfidence;
uniform float u_roleAirConfidence;

out vec4 FragColor;

void main() {
    vec2 u = gl_FragCoord.xy / u_resolution.y;
    vec3 p = vec3(u, 1.0);
    vec3 d = normalize(vec3(u, 2.0));
    vec3 c = vec3(1.0);

    float speed = 30 - u_rms  ;   // kick punches the camera forward
    for (float i = 0.0; i < 20.0; i++) {
        p += d  * speed + u_time / 2.0 ;
        c = abs(mod(p, 1.0) - u_kickImpact);           // fold space into unit cells
        if (dot(c, c) < 0.1) break;           // stop near a cell corner
    }

    vec3 col = vec3(c.r * 1.2, c.g * 0.3, c.b * 1.4);
    col *= 1.6 + 1.1 * u_energyLevel;         // energy brightens
    FragColor = vec4(col, 1.0);
}
