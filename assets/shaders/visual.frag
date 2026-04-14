#version 150
precision highp float;

uniform vec2 u_resolution;
uniform float u_time;
uniform float u_rms;
uniform float u_bandLow;
uniform float u_bandMid;
uniform float u_bandHigh;
uniform float u_onset;
uniform float u_bands[16];
uniform float u_onsets[16];
uniform float u_centroidNorm;
uniform float u_flux;
uniform float u_beatEnv;
uniform float u_bpm;
uniform float u_percRatio;
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

// User-adjustable params (shown dynamically in UI)
uniform float u_freqScale;    // multiplies base frequency (default 1)
uniform float u_moireMix;     // 0..1 blend between product and additive look
uniform float u_brightness;   // overall brightness multiplier
uniform float u_contrast;     // contrast around 0.5

out vec4 FragColor;

mat2 rot(float a){ float c = cos(a), s = sin(a); return mat2(c,-s,s,c); }
float sat(float x){ return clamp(x, 0.0, 1.0); }

float stripe(vec2 p, vec2 dir, float freq){
	float x = dot(p, normalize(dir)) * freq;
	return 0.5 + 0.5*cos(x);
}

float ring(vec2 p, float freq){
	float r = length(p);
	return 0.5 + 0.5*cos(r * freq);
}

float linstep(float a, float b, float x){ return clamp((x - a)/(b - a), 0.0, 1.0); }

void main(){
	// Normalize coords around center, keep aspect
	vec2 res = u_resolution;
	float s = min(res.x, res.y);
	vec2 uv = (gl_FragCoord.xy - 0.5*res) / s;

	float bass = (u_bands[0] + u_bands[1] + u_bands[2]) / 3.0;
	float mid = 0.0; for (int i=3; i<=9; ++i) mid += u_bands[i]; mid /= 7.0;
	float high = 0.0; for (int i=10; i<16; ++i) high += u_bands[i]; high /= 6.0;
	float onsetAvg = 0.0; for (int i=0; i<16; ++i) onsetAvg += u_onsets[i]; onsetAvg /= 16.0;
	float onHigh = 0.0; for (int i=10; i<16; ++i) onHigh += u_onsets[i]; onHigh /= 6.0;
	float pulse = sat(0.36*u_beatEnv + 0.20*onsetAvg + 0.18*u_onsetDensity + 0.27*u_drop + 0.16*u_isolatedHit);
	float detail = sat(0.28*u_rolloff + 0.24*u_contrastMean + 0.18*u_crest + 0.18*high + 0.12*u_novelty);
	float momentum = sat(0.34*u_energyDelta + 0.30*u_buildUp + 0.18*u_layerChange + 0.25*u_novelty);
	float bpmPhase = mix(sin(6.2831853 * max(u_bpm, 30.0) * u_time / 60.0), sin(6.2831853 * u_beatPhase), u_beatConfidence);
	vec3 chroma = normalize(vec3(
		0.16 + u_chroma[0] + 0.70*u_chroma[7] + 0.45*u_chroma[9],
		0.16 + u_chroma[2] + 0.75*u_chroma[4] + 0.35*u_chroma[11],
		0.16 + u_chroma[3] + 0.70*u_chroma[5] + 0.55*u_chroma[10]
	));

	// Audio-reactive parameters
	float speed = mix(0.08, 1.8, sat(0.55*high + 0.25*pulse + 0.20*u_buildUp));
	float baseF = mix(16.0, 104.0, sat(0.55*mid + 0.30*detail + 0.15*u_rms));
	float detune = 0.018 + 0.12 * sat(u_rms + 0.5*u_energyDelta) + 0.035*pulse + 0.06*u_chromaFlux;
	float wobble = 0.45 * bass + 0.25*u_buildUp;

	float t = u_time * speed;
	float a1 = 0.25 + 0.35 * sin(t*0.7 + 1.3) + 0.5*wobble + 0.20*u_layerChange;
	float a2 = a1 + (0.15 + detune) + 0.05*sin(t*0.9 + bpmPhase);

    float fscale = (u_freqScale == 0.0 ? 1.0 : u_freqScale);
    float f1 = baseF * fscale * (1.0 + 0.10*sin(t*0.8) + 0.04*pulse);
    float f2 = baseF * fscale * (1.0 + detune + 0.05*u_energyDelta);
    float fr = baseF * fscale * mix(0.42, 0.72, bass) * (1.0 + 0.2*sin(t*0.3));

	// Domain warp by a slow radial field for richer moiré
	vec2 pw = uv;
	float r = length(uv);
	float warp = 0.018 * sin(6.2831*r*(1.0 + 1.8*mid) + t*0.8) * (0.4 + high + pulse);
	pw += warp * normalize(uv + 1e-4);
	pw *= 1.0 + 0.035*bpmPhase*u_beatEnv + 0.06*u_drop;

	// Three slightly detuned patterns
	float s1 = stripe(pw*rot(a1), vec2(1.0,0.0), f1);
	float s2 = stripe(pw*rot(a2), vec2(1.0,0.0), f2);
	float sr = ring(pw, fr);

	// Interference
    float m = s1 * s2;
    m = mix(m, m*sr, clamp(u_moireMix, 0.0, 1.0));
	m = mix(m, max(m, sr*s2), 0.25*momentum);

	// Sharpen with gentle contrast curve and onset flash
	m = pow(m, mix(1.25, 0.55, pulse));

	// Subtle colorization: phase offset per channel
	float cR = m;
	float cG = stripe(pw*rot(a2+0.12), vec2(1.0,0.0), f2*1.005) * sr;
	float cB = stripe(pw*rot(a1-0.10), vec2(1.0,0.0), f1*0.995);

	vec3 col = vec3(cR, cG, cB);
	vec3 tint = vec3(0.06, 0.78, 0.70)*bass + vec3(0.98, 0.36, 0.20)*mid + vec3(0.98, 0.82, 0.18)*high;
	tint = mix(tint, chroma, 0.22 + 0.35*u_chromaFlux);
	tint = mix(tint, vec3(0.62, 0.88, 1.00), 0.18*u_percRatio + 0.20*onHigh + 0.12*u_rolloff);
	col *= 0.65 + 0.75*tint;
	// Tone and audio-driven brightness
	float bias = 0.05 + 0.20*u_rms + 0.22*pulse + 0.25*u_drop;
    col = mix(vec3(0.05,0.06,0.08), col, 0.85);
    col += bias;
	col *= mix(1.0, 0.60, u_breakState);
    // Contrast and brightness controls
    float contrast = (u_contrast == 0.0 ? 1.0 : u_contrast);
    col = mix(vec3(0.5), col, contrast);
    float brightness = (u_brightness == 0.0 ? 1.0 : u_brightness);
    col *= brightness;

	// Vignette for cohesion
	float vig = 1.0 - linstep(0.65, 1.05, length(uv));
	col *= mix(0.85, 1.0, vig);

	FragColor = vec4(col, 1.0);
}
