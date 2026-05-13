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
uniform float u_kickImpact;
uniform float u_snareImpact;
uniform float u_hatTick;
uniform float u_beatPulse;
uniform float u_subBody;
uniform float u_bassBody;
uniform float u_harmonicBody;
uniform float u_leadPresence;
uniform float u_airPresence;
uniform float u_transientDensity;
uniform float u_novelty;
uniform float u_brightness;
uniform float u_percussiveFocus;
uniform float u_energyLevel;
uniform float u_tension;
uniform float u_release;
uniform float u_dropEvent;
uniform float u_sectionChange;

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

FreqData analyzeFrequencies(float lowF, float midF, float highF, float t) {
    // Smooth inputs a little with tiny animated noise
    float smoothLow  = mix(lowF,  lowF  + 0.3 * noise(vec2(t * 0.20, 1.0)), 0.25);
    float smoothMid  = mix(midF,  midF  + 0.3 * noise(vec2(t * 0.15, 2.0)), 0.25);
    float smoothHigh = mix(highF, highF + 0.3 * noise(vec2(t * 0.10, 3.0)), 0.25);

    // Velocities integrated over time (soft)
    float bassV = sin(t * 0.3) * smoothLow * 0.45;
    float midV  = sin(t * 0.7) * smoothMid * 0.65;
    float highV = sin(t * 1.2) * smoothHigh * 0.25;
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
    float smoothFlux = softCompress(max(u_flux + 0.6 * u_novelty, 0.0));
    float smoothRms  = smooth01(max(u_rms, u_energyLevel));
    float beatPulse = clamp(0.55 * max(u_beatEnv, u_beatPulse) + 0.18 * smoothOn + 0.22 * u_kickImpact, 0.0, 1.5);
    float velocityScale = 0.80 + 0.55 * smoothRms + 0.20 * smoothFlux + 0.18 * u_tension;
    float flowIntensity = (0.8 + 0.6 * beatPulse) * (u_flowGain == 0.0 ? 1.0 : u_flowGain);
    return (bassFlow + midFlow + highFlow) * velocityScale * flowIntensity;
}

vec2 flowingDisplacement(vec2 pos, float t, FreqData fd) {
    float baseFreq = 3.0 + fd.flow * 0.45;
    float baseAmp  = (0.55 + 0.35*u_energyLevel + 0.25*u_bassBody) * (0.5 + fd.flow * 0.3);
    baseAmp *= mix(0.9, 1.3, clamp(0.55 * u_percRatio + 0.45 * u_percussiveFocus, 0.0, 1.0));
    vec2 displaced = pos;
    vec2 vel = flowVelocity(pos, t, fd);
    float bps = max(u_bpm, 1.0) / 60.0; // correct BPM -> beats/sec
    float beatPhase = sin(6.2831853 * bps * t);
    for (float i = 0.0; i < 4.0; i += 1.0) {
        float lt = t * (0.4 + i * 0.18);
        float lf = baseFreq * pow(1.6, i);
        float la = baseAmp / pow(1.8, i);
        float angle = lt * 0.10 + i * 0.5 + 0.05 * beatPhase;
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
    float ns = 20.0 * nsUser * (0.7 + 0.4 * clamp(u_centroidNorm + 0.5 * u_brightness, 0.0, 1.0));
    vec2 nuv = uv * ns;
    // very slow drift to hide minor discontinuities without jitter
    nuv += 0.05 * vec2(noise(vec2(t * 0.05, 0.0)), noise(vec2(0.0, t * 0.05)));
    float bassTex = fractalNoise(nuv + t * 0.2 + fd.bass * 0.5, 3);
    float midTex  = fractalNoise(nuv * 1.5 + t * 0.4 + fd.mid  * 0.3, 3);
    float highTex = fractalNoise(nuv * 22.2 + t * 0.6 + fd.high * 0.8, 2);
    float combined = bassTex * (0.4 + abs(fd.bass) * 0.2)
                   + midTex  * (0.3 + abs(fd.mid)  * 0.1)
                   + highTex * (0.3 + abs(fd.high) * 0.6);
    combined += fd.flow * 0.1;
    combined += 0.08 * softCompress(max(u_flux, 0.0)) + 0.10 * u_novelty + 0.08 * u_hatTick;
    return combined * 0.33;
}

void main() {
    vec2 uv = gl_FragCoord.xy / u_resolution.xy;
    vec2 c  = uv - 0.5;
    float t = u_time;

    // Audio mapping
    float audioLow  = clamp(u_bandLow,  0.0, 1.2);
    float audioMid  = clamp(u_bandMid,  0.0, 1.0);
    float audioHigh = clamp(u_bandHigh, 0.0, 1.5);

    FreqData fd = analyzeFrequencies(audioLow, audioMid, audioHigh, t);
    vec2 disp = flowingDisplacement(c, t, fd);
    float tex = fluidTexture(disp, t, fd);

    // Color phases
    float ph1 = length(disp) * 6.0 - t * 1.3 + fd.bass * 2.0;
    float ph2 = atan(disp.y, disp.x) * 2.5 + t * 1.1 + fd.mid  * 1.5;
    float ph3 = (disp.x + disp.y) * 4.0 + t * 0.9 + fd.high * 3.0;

    vec3 col;
    col.r = 0.5 + 0.5 * sin(ph1 + tex * 3.14159) * (0.8 + abs(fd.bass) * 0.4);
    col.g = 0.5 + 0.5 * sin(ph2 + tex * 3.14159 + 2.094) * (0.8 + abs(fd.mid)  * 0.6);
    col.b = 0.5 + 0.5 * sin(ph3 + tex * 3.14159 + 4.188) * (0.8 + abs(fd.high) * 0.8);

    col *= (0.8 + 0.2 * tex);
    col *= (1.0 + fd.flow * 0.2);
    // Bias with HPSS and beat/onset features
    float percBoost = 0.08 * clamp(u_percRatio, 0.0, 1.0) + 0.10 * u_dropEvent;
    float harmBoost = 0.10 * clamp(u_harmE, 0.0, 1.0);
    col.b *= (1.0 + percBoost);
    col.rg *= (1.0 + 0.5 * harmBoost);
    float brightness = 1.0 + 0.10 * max(u_beatEnv, u_beatPulse) + 0.08 * smooth01(onsetAverage()) + 0.10 * u_brightness + 0.06 * u_release;
    // Subtle vignette further stabilizes edges
    float r = length(c);
    brightness *= 1.0 - 0.05 * smoothstep(0.7, 1.0, r);
    col *= brightness;
    col = clamp(col, 0.0, 1.0);

    FragColor = vec4(col * (u_colorGain == 0.0 ? 1.0 : u_colorGain), 1.0);
}

