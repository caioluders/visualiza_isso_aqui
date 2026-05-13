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
uniform float u_kickImpact;
uniform float u_snareImpact;
uniform float u_hatTick;
uniform float u_beatPulse;
uniform float u_subBody;
uniform float u_bassBody;
uniform float u_harmonicBody;
uniform float u_leadPresence;
uniform float u_airPresence;
uniform float u_novelty;
uniform float u_brightness;
uniform float u_percussiveFocus;
uniform float u_energyLevel;
uniform float u_tension;
uniform float u_release;
uniform float u_dropEvent;

// User-adjustable params
uniform float u_advect;      // advection scale
uniform float u_noiseFreq;   // base noise frequency
uniform float u_colorGain;   // color intensity

out vec4 FragColor;

float smooth01(float x){ return smoothstep(0.0, 1.0, clamp(x, 0.0, 1.0)); }
float softCompress(float x){ return x / (1.0 + x); }

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

// Audio-driven parameters
struct AudioParams {
    float bass;
    float mid;
    float high;
    float onsetAvg;
};

AudioParams getAudio(){
    AudioParams a;
    a.bass = smooth01(0.22 * ((u_bands[0] + u_bands[1] + u_bands[2]) / 3.0) + 0.24 * u_subBody + 0.36 * u_bassBody + 0.18 * u_energyLevel);
    a.mid = smooth01(0.16 * ((u_bands[3] + u_bands[4] + u_bands[5] + u_bands[6] + u_bands[7] + u_bands[8] + u_bands[9]) / 7.0) + 0.42 * u_harmonicBody + 0.28 * u_leadPresence + 0.14 * u_release);
    a.high = smooth01(0.14 * ((u_bands[10] + u_bands[11] + u_bands[12] + u_bands[13] + u_bands[14] + u_bands[15]) / 6.0) + 0.44 * u_airPresence + 0.24 * u_brightness + 0.12 * u_hatTick);
    float os = 0.0; for (int i=0;i<16;i++) os += u_onsets[i]; a.onsetAvg = os/16.0;
    return a;
}

void main(){
    vec2 R = u_resolution;
    vec2 uv = (gl_FragCoord.xy - 0.5*R)/min(R.x, R.y);
    AudioParams a = getAudio();

    // "Velocity field" via curl noise, animated by time and audio
    float t = u_time;
    float pulse = clamp(0.40 * max(u_beatPulse, u_kickImpact) + 0.16 * u_snareImpact + 0.10 * u_dropEvent + 0.10 * u_novelty, 0.0, 1.0);
    float macro = smooth01(0.55 * u_energyLevel + 0.30 * u_tension + 0.15 * u_release);
    float adv = (0.55 + 0.95*a.high + 0.35*macro + 0.20*pulse) * (u_advect == 0.0 ? 1.0 : u_advect);
    float freq = (1.6 + 2.4*a.mid + 0.8*u_leadPresence + 0.4*u_novelty) * (u_noiseFreq == 0.0 ? 1.0 : u_noiseFreq);
    vec2  p = uv * (2.0 + 1.5*a.bass);
    vec2  v = curl(p*freq + 0.25*t*adv);

    // Pseudo advection: backtrace position in the field (semi-Lagrangian)
    // (Single step approximation; visually compelling for this use-case)
    vec2 back = uv - 0.35*v;

    // Sample a couple layers of noise for "density"
    float dens = 0.0;
    dens += noise(back*3.0 + vec2(0.1*t, 0.07*t));
    dens += 0.5*noise(back*6.0 - vec2(0.09*t, 0.05*t));
    dens += 0.25*noise(back*12.0 + vec2(0.03*t, -0.02*t));
    dens /= 1.75;

    // Incompressibility-ish: remove some divergence with simple normalization
    float div = dot(v, normalize(vec2(1.0,1.0)));
    dens = clamp(dens - 0.25*div, 0.0, 1.0);

    // Color via bands and density
    vec3 base = vec3(0.05, 0.06, 0.08);
    vec3 pal = vec3(0.9, 0.25, 0.3)*a.bass + vec3(0.2,0.8,0.35)*a.mid + vec3(0.25,0.35,1.0)*a.high;
    vec3 col = base + pal * pow(dens, 1.15 + 0.65*a.high + 0.20*u_brightness);
    col *= (u_colorGain == 0.0 ? 1.0 : u_colorGain);

    // Add streaks along the velocity direction (anisotropic look)
    float streaks = abs(dot(normalize(v), normalize(uv)));
    col += streaks * 0.22 * vec3(0.8, 0.9, 1.2) * (0.2 + 0.55*a.high + 0.25*u_airPresence);

    // Onset flash and pulse
    float flash = clamp(0.35 * pulse + 0.15 * smooth01(a.onsetAvg) + 0.12 * u_hatTick, 0.0, 1.0);
    col += flash * 0.10 * vec3(1.0, 0.9, 0.8) + 0.10 * u_dropEvent * vec3(1.0, 0.6, 0.3);
    col *= 0.92 + 0.12 * macro;

    // Soft vignette
    float r = length(uv);
    float vig = smoothstep(0.95, 0.25, r);
    col *= mix(0.85, 1.0, vig);

    FragColor = vec4(pow(col, vec3(0.9)), 1.0);
}
