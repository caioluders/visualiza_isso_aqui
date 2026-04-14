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

// User-adjustable controls. A value of 0 uses the shader default.
uniform float u_particleCountF;   // 24..180
uniform float u_particleSpeed;    // motion multiplier
uniform float u_particleSize;     // point/streak size multiplier
uniform float u_particleSpread;   // cloud radius multiplier
uniform float u_particleGain;     // brightness multiplier
uniform float u_trailLength;      // anisotropic streak length

out vec4 FragColor;

const float PI = 3.14159265359;
const float TAU = 6.28318530718;

float sat(float x){ return clamp(x, 0.0, 1.0); }

mat2 rot(float a){
    float c = cos(a), s = sin(a);
    return mat2(c, -s, s, c);
}

float hash11(float p){
    return fract(sin(p * 127.1) * 43758.5453123);
}

vec3 hash31(float p){
    return fract(sin(vec3(p * 127.1 + 17.0, p * 311.7 + 43.0, p * 74.7 + 113.0)) * 43758.5453123);
}

float bandAt(int index){
    int i = index - (index / 16) * 16;
    return u_bands[i];
}

float onsetAt(int index){
    int i = index - (index / 16) * 16;
    return u_onsets[i];
}

vec3 chromaTint(){
    vec3 c = vec3(
        0.16 + u_chroma[0] + 0.70*u_chroma[7] + 0.45*u_chroma[9],
        0.16 + u_chroma[2] + 0.75*u_chroma[4] + 0.35*u_chroma[11],
        0.16 + u_chroma[3] + 0.70*u_chroma[5] + 0.55*u_chroma[10]
    );
    return normalize(c);
}

vec3 bandColor(float bandIndex, float bandEnergy, float chromaFlux){
    float lowMask = 1.0 - smoothstep(2.5, 5.5, bandIndex);
    float highMask = smoothstep(9.0, 15.0, bandIndex);
    float midMask = sat(1.0 - lowMask - highMask);

    vec3 lowCol = vec3(0.04, 0.82, 0.74);
    vec3 midCol = vec3(1.00, 0.34, 0.20);
    vec3 highCol = vec3(0.98, 0.82, 0.22);
    vec3 c = lowCol * lowMask + midCol * midMask + highCol * highMask;
    c = mix(c, chromaTint(), 0.22 + 0.38*chromaFlux);
    c = mix(c, vec3(0.62, 0.88, 1.00), 0.16*u_rolloff + 0.16*u_percRatio);
    return c * (0.65 + 0.65*bandEnergy);
}

void main(){
    vec2 res = u_resolution;
    float s = min(res.x, res.y);
    vec2 uv = (gl_FragCoord.xy - 0.5*res) / max(s, 1.0);
    uv.x *= res.x / max(res.y, 1.0);

    float bass = (u_bands[0] + u_bands[1] + u_bands[2]) / 3.0;
    float mid = 0.0; for (int i = 3; i <= 9; ++i) mid += u_bands[i]; mid /= 7.0;
    float high = 0.0; for (int i = 10; i < 16; ++i) high += u_bands[i]; high /= 6.0;
    float onsetAvg = 0.0; for (int i = 0; i < 16; ++i) onsetAvg += u_onsets[i]; onsetAvg /= 16.0;

    float groove = mix(
        sin(TAU * max(u_bpm, 30.0) * u_time / 60.0),
        sin(TAU * u_beatPhase),
        u_beatConfidence
    );
    float pulse = sat(0.34*u_beatEnv + 0.22*u_onset + 0.20*u_onsetDensity + 0.30*u_drop + 0.18*u_isolatedHit);
    float detail = sat(0.30*u_rolloff + 0.24*u_contrastMean + 0.18*u_crest + 0.18*high + 0.10*u_novelty);
    float density = sat(0.30*u_onsetDensity + 0.28*u_highDensity + 0.18*u_buildUp + 0.16*u_rms + 0.12*u_novelty);

    float count = u_particleCountF == 0.0 ? 132.0 : clamp(u_particleCountF, 24.0, 180.0);
    float speedCtl = u_particleSpeed == 0.0 ? 1.0 : u_particleSpeed;
    float sizeCtl = u_particleSize == 0.0 ? 1.0 : u_particleSize;
    float spreadCtl = u_particleSpread == 0.0 ? 1.0 : u_particleSpread;
    float gainCtl = u_particleGain == 0.0 ? 1.0 : u_particleGain;
    float trailCtl = u_trailLength == 0.0 ? 1.0 : u_trailLength;

    float cameraSpin = 0.12*u_time + 0.18*groove*u_beatConfidence + 0.38*u_layerChange;
    vec2 viewUv = rot(cameraSpin) * uv;

    vec3 col = mix(vec3(0.010, 0.016, 0.026), vec3(0.026, 0.014, 0.024), u_breakState);
    float nebula = exp(-dot(viewUv, viewUv) * (2.0 + 1.5*u_breakState));
    col += nebula * vec3(0.020, 0.045, 0.055) * (0.35 + 0.65*u_flatness);

    for (int i = 0; i < 180; ++i) {
        if (float(i) >= count) continue;

        float id = float(i);
        vec3 seed = hash31(id + 3.7);
        int bandIndex = i - (i / 16) * 16;
        float band = bandAt(bandIndex);
        float onset = onsetAt(bandIndex);
        float bandT = float(bandIndex) / 15.0;

        float lane = seed.x * TAU;
        float spiralSign = seed.y < 0.5 ? -1.0 : 1.0;
        float localSpeed = speedCtl * (0.20 + 0.95*bass + 0.55*band + 0.55*u_buildUp + 0.35*density);
        float zPhase = fract(seed.z + u_time * localSpeed * (0.035 + 0.045*bandT) + 0.20*u_drop + 0.06*groove*u_beatConfidence);
        float z = mix(11.5, 0.85, zPhase);

        float shell = sqrt(seed.y) * (1.0 + 0.9*spreadCtl + 0.50*bass + 0.32*u_lowDensity);
        float vortex = u_time*(0.10 + 0.18*mid + 0.10*u_buildUp) + z*(0.34 + 0.70*detail) + spiralSign*0.36*groove*u_beatConfidence;
        float burst = pulse * (0.35 + 0.65*hash11(id + floor(u_time * max(u_bpm, 60.0) / 60.0)));
        float angle = lane + vortex + 0.70*u_chromaFlux + 0.55*u_layerChange;
        float radius = shell * (0.55 + 0.60*band + 0.28*burst + 0.24*u_novelty);

        vec3 p = vec3(cos(angle), sin(angle), 0.0) * radius;
        p.xy += vec2(
            sin(z*1.7 + seed.x*TAU + u_time*(0.20 + high)),
            cos(z*1.3 + seed.y*TAU - u_time*(0.16 + mid))
        ) * (0.08 + 0.22*u_flatness + 0.12*u_energyDelta);
        p.z = z;

        float fov = 1.35 + 0.24*u_rolloff + 0.18*pulse;
        vec2 screen = p.xy / max(0.35, p.z) * fov;
        vec2 delta = viewUv - screen;

        vec2 radial = normalize(screen + vec2(1e-4));
        vec2 tangent = vec2(-radial.y, radial.x) * spiralSign;
        float stretch = (1.0 + trailCtl * (1.4*localSpeed + 1.2*u_highDensity + 1.2*u_flux)) * (1.0 + 0.8*onset);
        float along = dot(delta, tangent) / stretch;
        float across = dot(delta, radial);

        float size = sizeCtl * (0.008 + 0.020*band + 0.020*onset + 0.014*pulse + 0.010*u_crest) / max(0.55, p.z);
        size *= mix(1.25, 0.78, u_flatness);
        float core = exp(-(along*along + across*across) / max(1e-5, size*size));
        float halo = exp(-dot(delta, delta) / max(1e-5, size*size*(10.0 + 18.0*detail)));
        float depthFade = smoothstep(11.8, 1.2, p.z);

        vec3 pc = bandColor(float(bandIndex), band, u_chromaFlux);
        pc *= 0.55 + 0.55*depthFade + 0.65*onset + 0.45*burst;
        col += pc * (core * 1.25 + halo * 0.20) * depthFade;
    }

    float center = exp(-dot(viewUv, viewUv) * (8.0 - 3.0*bass));
    col += center * vec3(0.12, 0.24, 0.22) * (0.25*bass + 0.18*u_lowDensity + 0.35*u_drop);
    col += u_isolatedHit * vec3(0.25, 0.16, 0.08) * exp(-dot(viewUv, viewUv) * 3.0);

    float vignette = smoothstep(1.25, 0.20, length(uv));
    col *= mix(0.55, 1.0, vignette);
    col *= gainCtl * (0.78 + 0.55*u_rms + 0.35*density + 0.25*u_beatEnv);
    col = col / (col + vec3(1.0));
    col = pow(max(col, 0.0), vec3(0.82));

    FragColor = vec4(col, 1.0);
}
