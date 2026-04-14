#version 150
precision highp float;

// "Stable Fluids"-inspired psychedelic fluid field
// Note: This is a single-pass, procedural approximation that mimics advection
// and incompressible flow aesthetics for visuals. For a full simulation, a
// multi-pass ping-pong pipeline (advection, diffusion, pressure solve,
// projection) should be used as in Jos Stam's paper. This shader produces a
// similar look, driven by audio.

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
uniform float u_advect;      // advection scale
uniform float u_noiseFreq;   // base noise frequency
uniform float u_colorGain;   // color intensity

out vec4 FragColor;

// Hash/Noise helpers
float hash(vec2 p){
    p = fract(p*vec2(123.34, 345.45));
    p += dot(p, p+34.345);
    return fract(p.x*p.y);
}

float noise(vec2 p){
    vec2 i = floor(p), f = fract(p);
    f = f*f*(3.0-2.0*f);
    float a = hash(i+vec2(0,0));
    float b = hash(i+vec2(1,0));
    float c = hash(i+vec2(0,1));
    float d = hash(i+vec2(1,1));
    return mix(mix(a,b,f.x), mix(c,d,f.x), f.y);
}

// Curl noise
vec2 curl(vec2 p){
    float e = 0.0015;
    float n1 = noise(p + vec2(0.0, e));
    float n2 = noise(p - vec2(0.0, e));
    float n3 = noise(p + vec2(e, 0.0));
    float n4 = noise(p - vec2(e, 0.0));
    return vec2(n1 - n2, n4 - n3) / (2.0*e);
}

vec3 chromaTint() {
    vec3 c = vec3(
        0.16 + u_chroma[0] + 0.70*u_chroma[7] + 0.45*u_chroma[9],
        0.16 + u_chroma[2] + 0.75*u_chroma[4] + 0.35*u_chroma[11],
        0.16 + u_chroma[3] + 0.70*u_chroma[5] + 0.55*u_chroma[10]
    );
    return normalize(c);
}

// Audio-driven parameters
struct AudioParams {
    float bass;
    float mid;
    float high;
    float onsetAvg;
    float onHigh;
};

AudioParams getAudio(){
    AudioParams a;
    a.bass = (u_bands[0] + u_bands[1] + u_bands[2]) / 3.0;
    a.mid = 0.0; for (int i=3;i<=9;i++) a.mid += u_bands[i]; a.mid /= 7.0;
    a.high = 0.0; for (int i=10;i<16;i++) a.high += u_bands[i]; a.high /= 6.0;
    float os = 0.0; for (int i=0;i<16;i++) os += u_onsets[i]; a.onsetAvg = os/16.0;
    a.onHigh = 0.0; for (int i=10;i<16;i++) a.onHigh += u_onsets[i]; a.onHigh /= 6.0;
    return a;
}

void main(){
    vec2 R = u_resolution;
    vec2 uv = (gl_FragCoord.xy - 0.5*R)/min(R.x, R.y);
    AudioParams a = getAudio();
    float pulse = clamp(0.42*u_beatEnv + 0.20*a.onsetAvg + 0.18*u_onsetDensity + 0.22*u_drop + 0.15*u_isolatedHit, 0.0, 1.0);
    float detail = clamp(0.32*u_rolloff + 0.24*u_contrastMean + 0.20*u_crest + 0.14*a.high + 0.10*u_novelty, 0.0, 1.0);
    float bps = max(u_bpm, 30.0) / 60.0;
    float bpmPhase = mix(sin(6.2831853 * bps * u_time), sin(6.2831853 * u_beatPhase), u_beatConfidence);

    // "Velocity field" via curl noise, animated by time and audio
    float t = u_time;
    float adv = (0.55 + 1.20*a.high + 0.55*u_rms + 0.80*u_buildUp + 0.50*pulse + 0.40*u_highDensity) * mix(1.08, 0.82, u_flatness) * (u_advect == 0.0 ? 1.0 : u_advect);
    float freq = (1.7 + 2.7*a.mid + 2.4*detail + 0.55*u_energyDelta + 0.35*u_chromaFlux) * (u_noiseFreq == 0.0 ? 1.0 : u_noiseFreq);
    vec2  p = uv * (1.8 + 1.6*a.bass + 0.6*u_breakState);
    p += 0.06 * vec2(cos(t*0.2 + bpmPhase), sin(t*0.17)) * (u_beatEnv + u_layerChange);
    vec2  v = curl(p*freq + 0.25*t*adv);

    // Pseudo advection: backtrace position in the field (semi-Lagrangian)
    // (Single step approximation; visually compelling for this use-case)
    vec2 back = uv - (0.30 + 0.16*pulse + 0.08*u_energyDelta + 0.10*u_novelty)*v;

    // Sample a couple layers of noise for "density"
    float dens = 0.0;
    dens += noise(back*(2.5 + 2.0*a.bass) + vec2(0.1*t, 0.07*t));
    dens += 0.5*noise(back*(5.0 + 5.0*a.mid) - vec2(0.09*t, 0.05*t));
    dens += 0.25*noise(back*(10.0 + 16.0*detail) + vec2(0.03*t, -0.02*t));
    dens /= 1.75;

    // Incompressibility-ish: remove some divergence with simple normalization
    float div = dot(v, normalize(vec2(1.0,1.0)));
    dens = clamp(dens - 0.25*div, 0.0, 1.0);

    // Color via bands and density
    vec3 base = mix(vec3(0.018, 0.028, 0.038), vec3(0.035, 0.018, 0.030), u_breakState);
    vec3 pal = vec3(0.9, 0.25, 0.3)*a.bass + vec3(0.2,0.8,0.35)*a.mid + vec3(0.25,0.35,1.0)*a.high;
    pal = mix(pal, chromaTint(), 0.20 + 0.35*u_chromaFlux);
    pal = mix(pal, vec3(0.62, 0.88, 1.00), 0.20*u_percRatio + 0.16*a.onHigh + 0.12*u_rolloff);
    vec3 col = base + pal * pow(dens, 1.2 + 0.8*a.high);
    col *= (u_colorGain == 0.0 ? 1.0 : u_colorGain);

    // Add streaks along the velocity direction (anisotropic look)
    float streaks = abs(dot(normalize(v), normalize(uv)));
    col += streaks * 0.25 * vec3(0.8, 0.9, 1.2) * (0.2 + 0.8*a.high + 0.5*u_layerChange);

    // Onset flash and pulse
    float flash = clamp(u_onset + a.onsetAvg + u_drop, 0.0, 1.0);
    col += flash * 0.15 * vec3(1.0, 0.9, 0.8);

    // Soft vignette
    float r = length(uv);
    float vig = smoothstep(0.95, 0.25, r);
    col *= mix(0.85, 1.0, vig);

    FragColor = vec4(pow(col, vec3(0.9)), 1.0);
}
