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

// User-adjustable params
uniform float u_waveScale;   // scales spatial frequency
uniform float u_waveMix;     // mix between bands influence 0..1
uniform float u_waveGain;    // overall brightness

out vec4 FragColor;

float sat(float x){ return clamp(x, 0.0, 1.0); }

void main(){
	vec2 uv = gl_FragCoord.xy / u_resolution.xy;
	uv = uv*2.0 - 1.0;
	uv.x *= u_resolution.x/u_resolution.y;

    float bass = (u_bands[0] + u_bands[1] + u_bands[2]) / 3.0;
    float lowMid = (u_bands[3] + u_bands[4] + u_bands[5] + u_bands[6]) / 4.0;
    float mid = 0.0; for (int i=3; i<=9; ++i) mid += u_bands[i]; mid /= 7.0;
    float high = 0.0; for (int i=10; i<16; ++i) high += u_bands[i]; high /= 6.0;
    float onLow = (u_onsets[0] + u_onsets[1] + u_onsets[2]) / 3.0;
    float onMid = 0.0; for (int i=3; i<=9; ++i) onMid += u_onsets[i]; onMid /= 7.0;
    float onHigh = 0.0; for (int i=10; i<16; ++i) onHigh += u_onsets[i]; onHigh /= 6.0;
    float pulse = sat(0.40*u_beatEnv + 0.22*onLow + 0.18*u_lowDensity + 0.22*u_drop + 0.15*u_isolatedHit);
    float detail = sat(0.30*u_rolloff + 0.24*u_contrastMean + 0.20*u_crest + 0.16*high + 0.10*u_novelty);
    float bps = max(u_bpm, 30.0) / 60.0;
    vec3 chroma = normalize(vec3(
        0.16 + u_chroma[0] + 0.70*u_chroma[7] + 0.45*u_chroma[9],
        0.16 + u_chroma[2] + 0.75*u_chroma[4] + 0.35*u_chroma[11],
        0.16 + u_chroma[3] + 0.70*u_chroma[5] + 0.55*u_chroma[10]
    ));

    float t = u_time * (0.30 + 0.55*bass + 0.45*u_buildUp + 0.35*high);
    float s = (u_waveScale == 0.0 ? 1.0 : u_waveScale);
    float bpmPhase = mix(sin(6.2831853 * bps * u_time), sin(6.2831853 * u_beatPhase), u_beatConfidence);
    float bend = 0.25*sin(uv.y*(3.0 + 5.0*mid) + t + u_layerChange*2.0);
    float x = uv.x + bend * (0.2 + 0.8*lowMid);
    float w1 = sin(x*(6.0 + 5.0*bass)*s + t*1.1 + 0.30*bpmPhase);
    float w2 = sin(x*(11.0 + 12.0*mid)*s + t*1.7 + uv.y*mid*4.0);
    float w3 = sin(x*(20.0 + 36.0*detail)*s - t*2.6 + onHigh*2.0);
	float y = (w1*(0.45 + 0.30*bass) + w2*(0.25 + 0.35*mid) + w3*(0.10 + 0.25*high));
    y *= 0.30 + 0.18*pulse + 0.12*u_buildUp;
    float band = mix(mid, (mid*0.55 + high*0.45), clamp(u_waveMix, 0.0, 1.0));
	float thickness = 0.025 + 0.14*band + 0.08*pulse + 0.05*onMid + 0.05*u_flatness;
	float crest = 1.0 - smoothstep(0.0, thickness, abs(uv.y - y));
	float echo = 1.0 - smoothstep(0.0, thickness*2.8, abs(uv.y + y*0.55 + 0.08*sin(t)));
	float v = sat(crest + 0.35*echo);

	vec3 base = mix(vec3(0.015,0.025,0.035), vec3(0.035,0.020,0.045), u_breakState);
	vec3 colA = mix(vec3(0.00,0.70,0.72), vec3(0.98,0.38,0.24), mid);
	vec3 colB = mix(vec3(0.95,0.82,0.18), vec3(0.52,0.86,1.00), u_percRatio);
	vec3 col = base + v * mix(mix(colA, chroma, 0.20 + 0.35*u_chromaFlux), colB, 0.30 + 0.40*high + 0.10*u_rolloff);
	col += (pulse*0.20 + onHigh*0.12 + u_drop*0.28) * vec3(1.0, 0.82, 0.58);
	col += sat(u_energyDelta + u_layerChange) * 0.10 * vec3(0.55, 0.95, 1.0);
    col *= (u_waveGain == 0.0 ? 1.0 : u_waveGain);
	FragColor = vec4(pow(max(col, 0.0), vec3(0.88)),1.0);
}
