/* Generative 3D fractal (Mandelbox-style) raymarch, purple palette
   GL 3.2 core, app-compatible uniforms (u_time, u_resolution, optional iResolution)
*/

#version 150
precision highp float;

uniform vec2  u_resolution;
uniform vec2  iResolution;  // optional; app may not set it
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

out vec4 FragColor;

// Camera helpers
mat3 rotY(float a){ float c=cos(a), s=sin(a); return mat3(c,0.0,-s, 0.0,1.0,0.0, s,0.0,c); }
mat3 rotX(float a){ float c=cos(a), s=sin(a); return mat3(1.0,0.0,0.0, 0.0,c,-s, 0.0,s,c); }
float sat(float x){ return clamp(x, 0.0, 1.0); }

vec3 chromaTint() {
    vec3 c = vec3(
        0.16 + u_chroma[0] + 0.70*u_chroma[7] + 0.45*u_chroma[9],
        0.16 + u_chroma[2] + 0.75*u_chroma[4] + 0.35*u_chroma[11],
        0.16 + u_chroma[3] + 0.70*u_chroma[5] + 0.55*u_chroma[10]
    );
    return normalize(c);
}

// Mandelbox distance estimator
float mandelboxDE(vec3 p){
    vec3 z = p;
    float dr = 1.0;
    float bass = (u_bands[0] + u_bands[1] + u_bands[2]) / 3.0;
    float mid = 0.0; for (int i=3; i<=9; ++i) mid += u_bands[i]; mid /= 7.0;
    float scale = 2.15 + 0.25*mid + 0.16*u_buildUp + 0.10*u_beatEnv + 0.14*u_contrastMean;
    float minR2 = 0.42 + 0.14*bass + 0.04*u_drop + 0.06*u_lowDensity;
    float fixedR2 = 1.0;
    for (int i = 0; i < 12; ++i){
        // Box fold
        z = clamp(z, -1.0, 1.0) * 2.0 - z;
        // Sphere fold
        float r2 = dot(z, z);
        if (r2 < minR2){
            float t = (fixedR2 / minR2);
            z *= t;
            dr *= t;
        } else if (r2 > fixedR2){
            float t = fixedR2 / r2;
            z *= t;
            dr *= t;
        }
        // Scale and translate
        z = z * scale + p * (1.0 + 0.04*u_energyDelta);
        dr = dr * abs(scale) + 1.0;
    }
    float r = length(z);
    return 0.5 * log(r) * r / abs(dr + 1e-6);
}

float map(vec3 p){
    return mandelboxDE(p);
}

vec3 calcNormal(vec3 p){
    // numerical gradient
    float e = 1e-3;
    vec2 h = vec2(1.0, -1.0) * e;
    return normalize(h.xyy * map(p + h.xyy) +
                     h.yyx * map(p + h.yyx) +
                     h.yxy * map(p + h.yxy) +
                     h.xxx * map(p + h.xxx));
}

vec3 palette(float t){
    float bass = (u_bands[0] + u_bands[1] + u_bands[2]) / 3.0;
    float mid = 0.0; for (int i=3; i<=9; ++i) mid += u_bands[i]; mid /= 7.0;
    float high = 0.0; for (int i=10; i<16; ++i) high += u_bands[i]; high /= 6.0;
    vec3 teal = vec3(0.02, 0.78, 0.72);
    vec3 coral = vec3(0.98, 0.32, 0.22);
    vec3 gold = vec3(0.96, 0.78, 0.20);
    vec3 ice = vec3(0.62, 0.90, 1.00);
    vec3 audio = normalize(teal*(0.30 + bass) + coral*(0.25 + mid) + gold*(0.20 + high));
    audio = mix(audio, chromaTint(), 0.18 + 0.35*u_chromaFlux);
    audio = mix(audio, ice, 0.18*u_percRatio + 0.16*u_rolloff);
    vec3 wave = 0.5 + 0.5*cos(6.28318 * (vec3(0.65, 0.43, 0.31) * t + vec3(0.00, 0.22, 0.58)));
    return audio * (0.35 + 0.85*wave);
}

void main(){
    vec2 res = (iResolution.x > 0.0) ? iResolution : u_resolution;
    vec2 uv  = (gl_FragCoord.xy - 0.5 * res) / max(res.y, 1.0);

    // Camera
    float bass = (u_bands[0] + u_bands[1] + u_bands[2]) / 3.0;
    float high = 0.0; for (int bi=10; bi<16; ++bi) high += u_bands[bi]; high /= 6.0;
    float onsetAvg = 0.0; for (int bi=0; bi<16; ++bi) onsetAvg += u_onsets[bi]; onsetAvg /= 16.0;
    float pulse = sat(0.40*u_beatEnv + 0.20*onsetAvg + 0.18*u_onsetDensity + 0.24*u_drop + 0.18*u_isolatedHit);
    float groove = mix(sin(6.2831853 * max(30.0, u_bpm) * u_time / 60.0), sin(6.2831853 * u_beatPhase), u_beatConfidence);
    float t = u_time * (0.16 + 0.14*high + 0.20*u_buildUp + 0.10*u_highDensity);
    vec3 ro = vec3(0.0, 0.0, 4.0 - 0.45*bass - 0.35*pulse + 0.25*u_breakState);
    ro = rotY(t*0.7 + 0.12*groove*u_beatConfidence + 0.35*u_layerChange) * rotX(sin(t*0.5)*0.3 + 0.10*u_energyDelta + 0.08*u_novelty) * ro;
    vec3 ta = vec3(0.0);
    vec3 ww = normalize(ta - ro);
    vec3 uu = normalize(cross(vec3(0.0,1.0,0.0), ww));
    vec3 vv = cross(ww, uu);
    float fov = 1.45 + 0.22*u_rolloff + 0.12*u_contrastMean + 0.18*pulse;
    vec3 rd = normalize(uu * uv.x + vv * uv.y + ww * fov);

    // Raymarch
    float tacc = 0.0;
    float d;
    float total = 0.0;
    vec3 p;
    const int MAX_STEPS = 128;
    const float SURF = 0.0008;
    const float FAR = 30.0;
    int i;
    for (i = 0; i < MAX_STEPS; ++i){
        p = ro + rd * tacc;
        d = map(p);
        if (d < SURF || tacc > FAR) break;
        // step with safety factor
        tacc += d * 0.9;
        total += 1.0;
    }

    vec3 col = vec3(0.0);
    if (d < SURF){
        vec3 n = calcNormal(p);
        // Soft lighting
        vec3 lightPos = vec3(2.5 * cos(t*1.7), 1.8, 2.5 * sin(t*1.3));
        vec3 l = normalize(lightPos - p);
        float diff = clamp(dot(n, l), 0.0, 1.0);
        float spec = pow(clamp(dot(reflect(-l, n), -rd), 0.0, 1.0), 32.0);
        float ao = exp(-(0.025 + 0.020*u_breakState) * total); // simple AO from steps
        vec3 base = palette(0.25 + 0.12 * total + 0.20*u_energyDelta);
        col = base * (0.18 + 0.82 * diff) * ao + (0.18 + 0.35*pulse) * spec * vec3(1.0, 0.86, 0.68);
        col += pow(1.0 - clamp(dot(n, -rd), 0.0, 1.0), 2.5) * vec3(0.25, 0.65, 0.75) * (0.2 + 0.8*high);
    }

    float fog = 1.0 - exp(-(0.025 + 0.030*u_breakState) * tacc);
    vec3 fogCol = mix(vec3(0.015, 0.025, 0.035), vec3(0.035, 0.015, 0.025), u_breakState);
    col = mix(col, fogCol, fog);
    col += u_drop * vec3(0.18, 0.12, 0.08);

    // Subtle filmic tone
    col = col / (col + 1.0);
    col = pow(col, vec3(0.4545));

    FragColor = vec4(col, 1.0);
}
