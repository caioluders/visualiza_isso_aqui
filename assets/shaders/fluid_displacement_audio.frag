#version 150
precision highp float;

// Port of ISF "fluid_displacement_audio.fs" to our runtime
// Mappings:
//  - RENDERSIZE -> u_resolution
//  - TIME       -> u_time
//  - lowFreq/midFreq/highFreq -> u_bandLow/u_bandMid/u_bandHigh
// Controls are derived from audio (gentle defaults) to avoid extra app-side UI.

uniform vec2  u_resolution;
uniform float u_time;
uniform float u_bandLow;
uniform float u_bandMid;
uniform float u_bandHigh;
uniform float u_rms;
uniform float u_onset;

// Extended AudioAnalysis uniforms
uniform float u_bands[16];
uniform float u_onsets[16];
uniform float u_centroidNorm; // 0..1
uniform float u_flux;         // >=0, typically ~0..1
uniform float u_beat;         // impulse-ish
uniform float u_beatEnv;      // eased envelope 0..1
uniform float u_bpm;          // beats per minute
uniform float u_percE;        // percussive energy
uniform float u_harmE;        // harmonic energy
uniform float u_percRatio;    // percussive/(percussive+harmonic)
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
uniform float u_texScale;     // scales fluidTexture noise scale
uniform float u_flowGain;     // multiplies flowIntensity
uniform float u_colorGain;    // multiplies final color

out vec4 FragColor;

// -------- Hash / Noise --------
float hash(vec2 p) {
    return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453);
}

float noise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);
    return mix(
        mix(hash(i + vec2(0.0, 0.0)), hash(i + vec2(1.0, 0.0)), u.x),
        mix(hash(i + vec2(0.0, 1.0)), hash(i + vec2(1.0, 1.0)), u.x),
        u.y
    );
}

float fractalNoise(vec2 p, int octaves) {
    float value = 0.0;
    float amplitude = 0.5;
    float frequency = 1.0;
    for (int i = 0; i < 4; i++) {
        if (i >= octaves) break;
        value += amplitude * noise(p * frequency);
        amplitude *= 0.5;
        frequency *= 2.0;
    }
    return value;
}

// -------- Audio analysis helpers --------
struct FreqData { float bass; float mid; float high; float flow; };

// Soft shaping helpers to reduce jitter from spiky inputs
float smooth01(float x) { return smoothstep(0.0, 1.0, clamp(x, 0.0, 1.0)); }
float softCompress(float x) { return x / (1.0 + x); }

float onsetAverage() {
    float s = 0.0;
    for (int i = 0; i < 16; i++) s += u_onsets[i];
    return s / 16.0;
}

float bandsLowAvg() {
    float s = 0.0; for (int i = 0; i < 4; i++) s += u_bands[i];
    return s * 0.25;
}

float bandsMidAvg() {
    float s = 0.0; for (int i = 4; i < 10; i++) s += u_bands[i];
    return s / 6.0;
}

float bandsHighAvg() {
    float s = 0.0; for (int i = 10; i < 16; i++) s += u_bands[i];
    return s / 6.0;
}

vec3 chromaTint() {
    vec3 c = vec3(
        0.16 + u_chroma[0] + 0.70*u_chroma[7] + 0.45*u_chroma[9],
        0.16 + u_chroma[2] + 0.75*u_chroma[4] + 0.35*u_chroma[11],
        0.16 + u_chroma[3] + 0.70*u_chroma[5] + 0.55*u_chroma[10]
    );
    return normalize(c);
}

FreqData analyzeFrequencies(float lowF, float midF, float highF, float t) {
    // Smooth inputs a little with tiny animated noise
    float smoothLow  = mix(lowF,  lowF  + 0.3 * noise(vec2(t * 0.20, 1.0)), 0.18);
    float smoothMid  = mix(midF,  midF  + 0.3 * noise(vec2(t * 0.15, 2.0)), 0.18);
    float smoothHigh = mix(highF, highF + 0.3 * noise(vec2(t * 0.10, 3.0)), 0.18);

    // Velocities integrated over time (soft)
    float pulse = clamp(0.50*u_beatEnv + 0.25*u_drop + 0.18*u_isolatedHit + 0.20*u_lowDensity, 0.0, 1.0);
    float texture = clamp(0.35*u_contrastMean + 0.25*u_crest + 0.25*u_rolloff + 0.15*u_novelty, 0.0, 1.0);
    float bassV = sin(t * (0.24 + 0.18*u_buildUp + 0.08*u_beatConfidence)) * smoothLow * (0.42 + 0.22*pulse);
    float midV  = sin(t * (0.60 + 0.25*u_energyDelta + 0.12*u_chromaFlux)) * smoothMid * (0.58 + 0.16*u_layerChange);
    float highV = sin(t * (1.05 + 0.55*u_rolloff + 0.20*u_highDensity)) * smoothHigh * (0.22 + 0.18*pulse + 0.12*texture);
    float combined = (bassV + midV + highV) * 0.33;

    FreqData fd; fd.bass = bassV; fd.mid = midV; fd.high = highV; fd.flow = combined; return fd;
}

vec2 flowVelocity(vec2 pos, float t, FreqData f) {
    vec2 bassFlow = vec2(
        sin(pos.y * 2.0 + t * 0.45) * f.bass,
        cos(pos.x * 1.8 + t * 0.28) * f.bass
    );
    vec2 midFlow = vec2(
        sin(pos.x * 3.0 + pos.y * 2.0 + t * 0.7) * f.mid,
        cos(pos.y * 2.5 + pos.x * 1.5 + t * 0.55) * f.mid
    );
    vec2 highFlow = vec2(
        sin(pos.x * 6.0 + t * 1.2) * f.high,
        cos(pos.y * 5.0 + t * 1.4) * f.high
    ) * 0.35;
    // Velocity scale & flow intensity driven by extended audio features
    float oa = onsetAverage();
    float smoothOn = smooth01(oa);
    float smoothFlux = softCompress(max(u_flux, 0.0));
    float smoothRms  = smooth01(u_rms);
    float beatPulse = clamp(0.85 * u_beatEnv + 0.22 * smoothOn + 0.20*u_onsetDensity, 0.0, 1.5);
    float velocityScale = 0.80 + 0.75 * smoothRms + 0.18 * smoothFlux + 0.36*u_buildUp + 0.22*u_novelty + 0.18*u_contrastMean;
    float flowIntensity = (0.8 + 0.6 * beatPulse) * mix(1.10, 0.82, u_flatness) * (u_flowGain == 0.0 ? 1.0 : u_flowGain);
    return (bassFlow + midFlow + highFlow) * velocityScale * flowIntensity;
}

vec2 flowingDisplacement(vec2 pos, float t, FreqData fd) {
    float baseFreq = 2.8 + fd.flow * 0.45 + 0.9*u_energyDelta + 0.7*u_buildUp + 0.8*u_contrastMean;
    float baseAmp  = (0.6 + 0.6*u_rms) * (0.5 + fd.flow * 0.3);
    baseAmp *= mix(0.85, 1.35, clamp(u_percRatio, 0.0, 1.0));
    baseAmp *= 1.0 + 0.22*u_drop + 0.16*u_isolatedHit;
    vec2 displaced = pos;
    vec2 vel = flowVelocity(pos, t, fd);
    float bps = max(u_bpm, 1.0) / 60.0; // correct BPM -> beats/sec
    float beatPhase = mix(sin(6.2831853 * bps * t), sin(6.2831853 * u_beatPhase), u_beatConfidence);
    for (float i = 0.0; i < 4.0; i += 1.0) {
        float lt = t * (0.4 + i * 0.18);
        float lf = baseFreq * pow(1.6, i);
        float la = baseAmp / pow(1.8, i);
        float angle = lt * 0.10 + i * 0.5 + 0.05 * beatPhase + 0.16*u_layerChange;
        mat2 R = mat2(cos(angle), -sin(angle), sin(angle), cos(angle));
        vec2 lp = displaced * R;
        vec2 lvel = vec2(
            sin(lp.y * lf + lt) * la,
            cos(lp.x * lf + lt) * la
        );
        displaced += lvel + vel * (0.06 / (i + 1.0));
    }
    return displaced;
}

float fluidTexture(vec2 uv, float t, FreqData fd) {
    float nsUser = (u_texScale == 0.0 ? 1.0 : u_texScale);
    float ns = 20.0 * nsUser * (0.70 + 0.55 * clamp(u_rolloff, 0.0, 1.0) + 0.30*u_contrastMean + 0.20*u_novelty);
    vec2 nuv = uv * ns;
    // very slow drift to hide minor discontinuities without jitter
    nuv += 0.05 * vec2(noise(vec2(t * 0.05, 0.0)), noise(vec2(0.0, t * 0.05)));
    float bassTex = fractalNoise(nuv + t * 0.2 + fd.bass * 0.5, 3);
    float midTex  = fractalNoise(nuv * 1.5 + t * 0.4 + fd.mid  * 0.3, 3);
    float highTex = fractalNoise(nuv * (16.0 + 18.0*u_crest) + t * 0.6 + fd.high * 0.8, 2);
    float combined = bassTex * (0.4 + abs(fd.bass) * 0.2)
                   + midTex  * (0.3 + abs(fd.mid)  * 0.1)
                   + highTex * (0.3 + abs(fd.high) * 0.6);
    combined += fd.flow * 0.1;
    combined += 0.10 * softCompress(max(u_flux, 0.0)) + 0.08*u_layerChange + 0.10*u_chromaFlux;
    return combined * 0.33;
}

void main() {
    vec2 uv = gl_FragCoord.xy / u_resolution.xy;
    vec2 c  = uv - 0.5;
    float t = u_time;

    // Audio mapping
    float audioLow  = clamp(0.55*u_bandLow  + 0.45*bandsLowAvg(),  0.0, 1.2);
    float audioMid  = clamp(0.55*u_bandMid  + 0.45*bandsMidAvg(),  0.0, 1.0);
    float audioHigh = clamp(0.55*u_bandHigh + 0.45*bandsHighAvg(), 0.0, 1.5);

    FreqData fd = analyzeFrequencies(audioLow, audioMid, audioHigh, t);
    vec2 disp = flowingDisplacement(c, t, fd);
    float tex = fluidTexture(disp, t, fd);

    // Color phases
    float ph1 = length(disp) * (5.5 + 2.0*u_buildUp) - t * 1.2 + fd.bass * 2.0 + 0.8*u_drop;
    float ph2 = atan(disp.y, disp.x) * (2.3 + 1.2*u_energyDelta) + t * 1.0 + fd.mid  * 1.5;
    float ph3 = (disp.x + disp.y) * (3.8 + 3.5*u_centroidNorm) + t * 0.85 + fd.high * 3.0 + u_layerChange;

    vec3 col;
    col.r = 0.5 + 0.5 * sin(ph1 + tex * 3.14159) * (0.8 + abs(fd.bass) * 0.4);
    col.g = 0.5 + 0.5 * sin(ph2 + tex * 3.14159 + 2.094) * (0.8 + abs(fd.mid)  * 0.6);
    col.b = 0.5 + 0.5 * sin(ph3 + tex * 3.14159 + 4.188) * (0.8 + abs(fd.high) * 0.8);

    col = mix(col, col * chromaTint() * 1.45, 0.28 + 0.30*u_chromaFlux);
    col *= (0.8 + 0.2 * tex);
    col *= (1.0 + fd.flow * 0.2);
    // Bias with HPSS and beat/onset features
    float percBoost = 0.15 * clamp(u_percRatio, 0.0, 1.0);
    float harmBoost = 0.10 * clamp(u_harmE, 0.0, 1.0);
    col.b *= (1.0 + percBoost);
    col.rg *= (1.0 + 0.5 * harmBoost);
    float brightness = 1.0 + 0.10 * u_beatEnv + 0.08 * smooth01(onsetAverage()) + 0.14*u_drop + 0.10*u_isolatedHit + 0.12*u_onsetDensity + 0.10*u_novelty;
    // Subtle vignette further stabilizes edges
    float r = length(c);
    brightness *= 1.0 - 0.05 * smoothstep(0.7, 1.0, r);
    col *= brightness;
    col *= mix(1.0, 0.66, u_breakState);
    col = clamp(col, 0.0, 1.0);

    FragColor = vec4(col * (u_colorGain == 0.0 ? 1.0 : u_colorGain), 1.0);
}
