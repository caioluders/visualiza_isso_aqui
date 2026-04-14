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
uniform float u_flux;         // spectral flux (small positive)
uniform float u_beat;         // 0 or 1 when beat triggers
uniform float u_beatEnv;      // 0..1 decaying envelope after beat
uniform float u_bpm;          // BPM estimate (0 if unknown)
uniform float u_percE;        // percussive energy
uniform float u_harmE;        // harmonic energy
uniform float u_percRatio;    // percussive ratio 0..1
uniform float u_energyDelta;
uniform float u_drop;
uniform float u_breakState;
uniform float u_buildUp;
uniform float u_layerChange;
uniform float u_isolatedHit;
uniform float u_rolloff;
uniform float u_flatness;
uniform float u_crest;
uniform float u_contrastBands[6];
uniform float u_contrastMean;
uniform float u_chroma[12];
uniform float u_chromaFlux;
uniform float u_onsetDensity;
uniform float u_lowDensity;
uniform float u_highDensity;
uniform float u_beatPhase;
uniform float u_beatConfidence;
uniform float u_novelty;

out vec4 FragColor;

float sat(float x){ return clamp(x, 0.0, 1.0); }

vec3 chromaTint() {
    vec3 c = vec3(
        0.16 + u_chroma[0] + 0.70*u_chroma[7] + 0.45*u_chroma[9],
        0.16 + u_chroma[2] + 0.75*u_chroma[4] + 0.35*u_chroma[11],
        0.16 + u_chroma[3] + 0.70*u_chroma[5] + 0.55*u_chroma[10]
    );
    return normalize(c);
}

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
    float groove = mix(sin(6.2831853 * max(u_bpm, 30.0) * t / 60.0), sin(6.2831853 * u_beatPhase), u_beatConfidence);
    float texture = sat(0.35*u_rolloff + 0.30*u_contrastMean + 0.20*u_crest + 0.15*u_novelty);
    float structure = sat(0.25*u_energyDelta + 0.25*u_layerChange + 0.22*u_buildUp + 0.18*u_drop + 0.25*u_novelty);

    // Audio-reactive parameters (coarse + 16-band + extended)
    float lowZoom    = 0.13 + 0.16*(0.6*u_bandLow + 0.4*bass) + 0.15*u_rms + 0.16*u_beatEnv + 0.10*u_lowDensity + 0.08*u_drop;
    float midFreq    = mix(360.0, 3900.0, sat(0.38*(u_bandMid + midB) + 0.14*u_rms + 0.26*texture + 0.15*u_chromaFlux));
    float highShaper = 0.48 + 0.72*(0.5*u_bandHigh + 0.5*highB) + 0.20*u_percRatio + 0.18*u_highDensity + 0.16*u_contrastMean;
    float flash      = sat(0.08*u_onset + 0.34*onBass + 0.22*onHigh + 0.22*u_beatEnv + 0.25*u_drop + 0.20*u_onsetDensity);
    float wSpeed     = 0.09 + 0.18*(0.5*u_bandHigh + 0.5*highB) + 0.10*sat(u_flux) + 0.14*u_buildUp + 0.12*u_beatConfidence;

	// Base coordinate and slight domain warp from lows/highs
	vec2 uv = FC / r - 0.5;
	uv.x *= r.x / r.y ;
	uv *= lowZoom;
	float rad = length(uv);
	float ang = atan(uv.y, uv.x);
	float warp = 0.0007 * sin((6.2831*rad * (1.0 + 0.6*u_bandMid + 0.9*structure)) + t*wSpeed/100.0 + 0.08*groove);
	uv += warp * vec2(cos(ang), sin(ang));

	// Core iterative expression (keeps your structure, makes constants reactive)
	float code = length(uv);
	code = code + code*code + t/10;
    code = sin(code*2.0 + 0.25*u_bpm + 0.4*groove) + cos(wSpeed + 0.25*u_chromaFlux);
    code = sin(code * (midFreq*0.15) * pow(abs(code)+1e-4, 0.1 + 0.99*highShaper));
    code += 0.15*u_beatEnv + 0.10*u_percRatio + 0.18*u_drop - 0.10*u_breakState;
    
    float v = 0.5 + 0.5*code;
    vec3 lowCol = vec3(0.02, 0.80, 0.74);
    vec3 midCol = vec3(1.00, 0.34, 0.22);
    vec3 highCol = vec3(0.98, 0.82, 0.20);
    vec3 tint = normalize(lowCol*(0.30 + bass) + midCol*(0.25 + midB) + highCol*(0.20 + highB));
    tint = mix(tint, chromaTint(), 0.22 + 0.35*u_chromaFlux);
    tint = mix(tint, vec3(0.62, 0.88, 1.0), 0.18*u_percRatio + 0.18*u_rolloff);
    vec3 col = mix(vec3(0.015, 0.025, 0.035), tint, smoothstep(0.25, 0.95, v));
    col += flash * vec3(0.26, 0.20, 0.12);
    col *= mix(1.0, 0.58, u_breakState);
	FragColor = vec4(pow(max(col, 0.0), vec3(0.9)), 1.0);
}
