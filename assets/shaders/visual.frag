#version 150
precision highp float;

uniform vec2 u_resolution;
uniform float u_time;
uniform float u_rms;
uniform float u_bandLow;
uniform float u_bandMid;
uniform float u_bandHigh;
uniform float u_onset;
uniform float u_kickImpact;
uniform float u_bassBody;
uniform float u_harmonicBody;
uniform float u_leadPresence;
uniform float u_airPresence;
uniform float u_energyLevel;
uniform float u_novelty;
uniform float u_brightness;
uniform float u_tension;
uniform float u_dropEvent;

// User-adjustable params (shown dynamically in UI)
uniform float u_freqScale;    // multiplies base frequency (default 1)
uniform float u_moireMix;     // 0..1 blend between product and additive look
uniform float u_visualBrightness;   // overall brightness multiplier
uniform float u_contrast;     // contrast around 0.5

out vec4 FragColor;

mat2 rot(float a){ float c = cos(a), s = sin(a); return mat2(c,-s,s,c); }

float stripe(vec2 p, vec2 dir, float freq){
	float x = dot(p, normalize(dir)) * freq;
	return 0.5 + 0.5*cos(x);
}

float ring(vec2 p, float freq){
	float r = length(p);
	return 0.5 + 0.5*cos(r * freq);
}

float linstep(float a, float b, float x){ return clamp((x - a)/(b - a), 0.0, 1.0); }
float smooth01(float x){ return smoothstep(0.0, 1.0, clamp(x, 0.0, 1.0)); }
float softCompress(float x){ return x / (1.0 + x); }

void main(){
	// Normalize coords around center, keep aspect
	vec2 res = u_resolution;
	float s = min(res.x, res.y);
	vec2 uv = (gl_FragCoord.xy - 0.5*res) / s;

	// Audio-reactive parameters
	float pulse = clamp(0.42 * u_kickImpact + 0.16 * u_dropEvent + 0.14 * u_novelty, 0.0, 1.0);
	float macro = smooth01(0.55*u_energyLevel + 0.30*u_tension + 0.15*u_dropEvent);
	float speed = mix(0.12, 1.25, clamp(0.15*u_bandHigh + 0.28*u_airPresence + 0.18*u_brightness + 0.18*u_tension + 0.12*u_novelty, 0.0, 1.0));
	float baseF = mix(18.0, 74.0, clamp(0.12*u_bandMid + 0.34*u_harmonicBody + 0.28*u_leadPresence + 0.18*u_novelty + 0.08*u_brightness, 0.0, 1.0));
	float detune = 0.02 + 0.05 * smooth01(u_rms) + 0.06 * u_novelty + 0.04 * u_airPresence;
	float wobble = 0.16 * u_bandLow + 0.32 * u_bassBody + 0.10 * pulse + 0.08 * macro;

	float t = u_time * speed;
	float a1 = 0.25 + 0.35 * sin(t*0.7 + 1.3) + 0.5*wobble;
	float a2 = a1 + (0.15 + detune) + 0.05*sin(t*0.9);

    float fscale = (u_freqScale == 0.0 ? 1.0 : u_freqScale);
    float f1 = baseF * fscale * (1.0 + 0.10*sin(t*0.8));
    float f2 = baseF * fscale * (1.0 + detune);
    float fr = baseF * fscale * 0.55 * (1.0 + 0.2*sin(t*0.3));

	// Domain warp by a slow radial field for richer moiré
	vec2 pw = uv;
	float r = length(uv);
	float warp = 0.02 * sin(6.2831*r + t*0.8) * (0.35 + 0.35*u_airPresence + 0.20*u_brightness);
	pw += warp * normalize(uv + 1e-4);

	// Three slightly detuned patterns
	float s1 = stripe(pw*rot(a1), vec2(1.0,0.0), f1);
	float s2 = stripe(pw*rot(a2), vec2(1.0,0.0), f2);
	float sr = ring(pw, fr);

	// Interference
    float m = s1 * s2;
    m = mix(m, m*sr, clamp(u_moireMix, 0.0, 1.0));

	// Sharpen with gentle contrast curve and onset flash
	m = pow(m, mix(1.2, 0.78, clamp(0.65*pulse + 0.35*macro, 0.0, 1.0)));

	// Subtle colorization: phase offset per channel
	float cR = m;
	float cG = stripe(pw*rot(a2+0.12), vec2(1.0,0.0), f2*1.005) * sr;
	float cB = stripe(pw*rot(a1-0.10), vec2(1.0,0.0), f1*0.995);

	vec3 col = vec3(cR, cG, cB);
	// Tone and audio-driven brightness
	float bias = 0.05 + 0.11*macro + 0.08*pulse + 0.05*u_brightness;
    col = mix(vec3(0.05,0.06,0.08), col, 0.85);
    col += bias;
    // Contrast and brightness controls
    float contrast = (u_contrast == 0.0 ? 1.0 : u_contrast);
    col = mix(vec3(0.5), col, contrast);
    float brightness = (u_visualBrightness == 0.0 ? 1.0 : u_visualBrightness);
    col *= brightness * (0.92 + 0.10*softCompress(u_tension + u_energyLevel));

	// Vignette for cohesion
	float vig = 1.0 - linstep(0.65, 1.05, length(uv));
	col *= mix(0.85, 1.0, vig);

	FragColor = vec4(col, 1.0);
}
