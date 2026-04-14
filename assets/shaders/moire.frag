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

// User-adjustable params (auto-exposed in UI)
uniform float u_freqScale;     // scales stripe frequency
uniform float u_angleOffset1;  // add to first layer angle
uniform float u_angleOffset2;  // add to second layer angle
uniform float u_mix;           // 0..1, product vs additive moiré
uniform float u_brightness;    // overall brightness
uniform float u_contrast;      // contrast around 0.5

out vec4 FragColor;

mat2 rot(float a){ float c = cos(a), s = sin(a); return mat2(c,-s,s,c); }
float sat(float x){ return clamp(x, 0.0, 1.0); }

float stripe(vec2 p, float freq){ return 0.5 + 0.5*cos(p.x * freq); }

void main(){
	vec2 res = u_resolution;
	float s = min(res.x, res.y);
	vec2 uv = (gl_FragCoord.xy - 0.5*res) / s;

    float bass = (u_bands[0] + u_bands[1] + u_bands[2]) / 3.0;
    float mid = 0.0; for (int i=3; i<=9; ++i) mid += u_bands[i]; mid /= 7.0;
    float high = 0.0; for (int i=10; i<16; ++i) high += u_bands[i]; high /= 6.0;
    float onsetAvg = 0.0; for (int i=0; i<16; ++i) onsetAvg += u_onsets[i]; onsetAvg /= 16.0;
    float onHigh = 0.0; for (int i=10; i<16; ++i) onHigh += u_onsets[i]; onHigh /= 6.0;
    float pulse = sat(0.36*u_beatEnv + 0.20*onsetAvg + 0.18*u_onsetDensity + 0.24*u_drop + 0.18*u_isolatedHit);
    float detail = sat(0.30*u_rolloff + 0.26*u_contrastMean + 0.20*u_crest + 0.14*high + 0.10*u_novelty);
    float bpmPhase = mix(sin(6.2831853 * max(u_bpm, 30.0) * u_time / 60.0), sin(6.2831853 * u_beatPhase), u_beatConfidence);
    vec3 chroma = normalize(vec3(
        0.16 + u_chroma[0] + 0.70*u_chroma[7] + 0.45*u_chroma[9],
        0.16 + u_chroma[2] + 0.75*u_chroma[4] + 0.35*u_chroma[11],
        0.16 + u_chroma[3] + 0.70*u_chroma[5] + 0.55*u_chroma[10]
    ));

    float t = u_time * (0.18 + 0.85*high + 0.45*u_buildUp + 0.35*pulse);
    float f = mix(28.0, 150.0, sat(0.55*mid + 0.35*detail + 0.10*u_rms));
    float fscale = (u_freqScale == 0.0 ? 1.0 : u_freqScale);
    f *= fscale;
    float d = 0.025 + 0.14*u_rms + 0.07*bass + 0.04*u_energyDelta + 0.05*u_chromaFlux;

    vec2 warped = uv;
    float r = length(warped);
    warped += normalize(warped + 1e-4) * 0.025 * sin(r*10.0 + t*1.3) * (bass + pulse);

    vec2 p1 = rot(0.4 + 0.22*sin(t*0.7) + 0.20*u_layerChange + u_angleOffset1)*warped;
    vec2 p2 = rot(0.4 + d + 0.17*sin(t*0.6 + bpmPhase) + 0.16*u_energyDelta + u_angleOffset2)*warped;

    float a = stripe(p1, f);
    float b = stripe(p2, f*(1.0 + d + 0.08*detail));
    float c = stripe(rot(1.2 + 0.25*bpmPhase)*warped, f*(0.45 + 0.45*high));
    float m = mix(a*b, 0.5*(a+b), clamp(u_mix, 0.0, 1.0));
    m = mix(m, m*c, 0.25 + 0.45*detail);
    m = pow(max(m, 0.0), mix(1.45, 0.62, pulse));

    vec3 lowCol = vec3(0.00, 0.78, 0.72);
    vec3 midCol = vec3(0.98, 0.30, 0.22);
    vec3 highCol = vec3(0.98, 0.82, 0.18);
    vec3 tint = normalize(lowCol*(0.25 + bass) + midCol*(0.25 + mid) + highCol*(0.20 + high));
    tint = mix(tint, chroma, 0.20 + 0.35*u_chromaFlux);
    tint = mix(tint, vec3(0.70, 0.90, 1.00), 0.20*u_percRatio + 0.18*onHigh + 0.12*u_rolloff);

    vec3 col = tint * m;
    col = mix(vec3(0.025,0.030,0.040), col, 0.92);
    col += pulse * vec3(0.25, 0.19, 0.12) + u_drop * vec3(0.30, 0.22, 0.15);
    col *= mix(1.0, 0.62, u_breakState);
    float contrast = (u_contrast == 0.0 ? 1.0 : u_contrast);
    col = mix(vec3(0.5), col, contrast);
    float brightness = (u_brightness == 0.0 ? 1.0 : u_brightness);
    col *= brightness;
	FragColor = vec4(col, 1.0);
}
