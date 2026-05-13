#version 150
precision highp float;

uniform vec2 u_resolution;
uniform float u_time;

uniform float u_rms;
uniform float u_bandLow;
uniform float u_bandMid;
uniform float u_bandHigh;
uniform float u_onset;

// Advanced analysis arrays (first N bands used; app currently sends 16)
uniform float u_bands[16];
uniform float u_onsets[16];

// Extended analysis uniforms
uniform float u_centroidNorm; // 0..1 (normalized spectral centroid)
uniform float u_flux;         // spectral flux normalized to ~0..1
uniform float u_beat;         // 0 or 1 when beat triggers
uniform float u_beatEnv;      // 0..1 decaying envelope after beat
uniform float u_bpm;          // BPM estimate (0 if unknown)
uniform float u_percE;        // percussive energy
uniform float u_harmE;        // harmonic energy
uniform float u_percRatio;    // percussive ratio 0..1
uniform float u_kickImpact;
uniform float u_snareImpact;
uniform float u_hatTick;
uniform float u_beatPulse;
uniform float u_subBody;
uniform float u_bassBody;
uniform float u_harmonicBody;
uniform float u_leadPresence;
uniform float u_airPresence;
uniform float u_novelty;
uniform float u_brightness;
uniform float u_percussiveFocus;
uniform float u_energyLevel;
uniform float u_tension;
uniform float u_dropEvent;

out vec4 FragColor;

void main() {
	vec2 FC = gl_FragCoord.xy;
	vec2 r = u_resolution;
	float t = u_time;

    // Aggregate bands from the 16-band analysis to refine reactivity
    float bass = u_bands[0];
    float midB = 0.0; for (int i = 3; i <= 9; ++i) midB += u_bands[i]; midB /= 7.0;
    float highB = 0.0; for (int i = 10; i < 16; ++i) highB += u_bands[i]; highB /= 6.0;
    float onBass = (u_onsets[0] + u_onsets[1] + u_onsets[2]) / 3.0;
    float onHigh = 0.0; for (int i = 10; i < 16; ++i) onHigh += u_onsets[i]; onHigh /= 6.0;

    // Audio-reactive parameters (coarse + 16-band + extended)
    float lowZoom    = 0.15 + 0.10*(0.25*u_bandLow + 0.25*bass + 0.20*u_subBody + 0.30*u_bassBody) + 0.16*u_energyLevel + 0.14*u_beatPulse;
    float midFreq    = mix(380.0, 3200.0, clamp(0.18*(u_bandMid + midB) + 0.26*u_harmonicBody + 0.30*u_leadPresence + 0.12*u_brightness, 0.0, 1.0));
    float highShaper = 0.55 + 0.45*(0.20*u_bandHigh + 0.20*highB + 0.35*u_airPresence + 0.25*u_brightness) + 0.10*u_percussiveFocus;
    float flash      = clamp(0.22*u_kickImpact + 0.20*u_snareImpact + 0.10*u_hatTick + 0.15*u_dropEvent + 0.10*u_novelty + 0.08*u_onset + 0.15*u_beatPulse, 0.0, 1.0);
    float wSpeed     = 0.10 + 0.12*u_airPresence + 0.12*sqrt(clamp(u_flux + u_novelty, 0.0, 1.0)) + 0.10*u_beatPulse + 0.08*u_tension;

	// Base coordinate and slight domain warp from lows/highs
	vec2 uv = FC / r - 0.5;
	uv.x *= r.x / r.y ;
	uv *= lowZoom;
	float rad = length(uv);
	float ang = atan(uv.y, uv.x);
	float warp = 0.0005 * sin( (6.2831*rad * (1.0 + 0.6*u_bandMid)) + t*wSpeed/100 );
	uv += warp * vec2(cos(ang), sin(ang));

	// Core iterative expression (keeps your structure, makes constants reactive)
	float code = length(uv);
	code = code + code*code + t/10;
    code = sin(code*2.0 + 0.3*u_bpm) + cos(wSpeed);
    code = sin(code * (midFreq*0.15) * pow(abs(code)+1e-4, 0.1 + 0.99*highShaper));
    code += 0.10*u_beatPulse + 0.08*u_percRatio + 0.10*u_percussiveFocus + 0.18*flash;

    vec3 col = vec3(code);
    col *= 0.88 + 0.20*flash + 0.16*u_energyLevel;
    col.r += 0.04*u_percRatio + 0.08*u_leadPresence;
    col.g += 0.05*highShaper + 0.06*u_harmonicBody;
    col.b += 0.05*(0.4*u_bandHigh + 0.6*highB) + 0.10*u_airPresence + 0.05*u_tension;
	FragColor = vec4(col, 1.0);
}
