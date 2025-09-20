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

// Audio-driven parameters
struct AudioParams {
    float bass;
    float mid;
    float high;
    float onsetAvg;
};

AudioParams getAudio(){
    AudioParams a;
    a.bass = (u_bands[0] + u_bands[1] + u_bands[2]) / 3.0;
    a.mid = 0.0; for (int i=3;i<=9;i++) a.mid += u_bands[i]; a.mid /= 7.0;
    a.high = 0.0; for (int i=10;i<16;i++) a.high += u_bands[i]; a.high /= 6.0;
    float os = 0.0; for (int i=0;i<16;i++) os += u_onsets[i]; a.onsetAvg = os/16.0;
    return a;
}

void main(){
    vec2 R = u_resolution;
    vec2 uv = (gl_FragCoord.xy - 0.5*R)/min(R.x, R.y);
    AudioParams a = getAudio();

    // "Velocity field" via curl noise, animated by time and audio
    float t = u_time;
    float adv = (0.6 + 1.8*a.high + 0.8*u_rms) * (u_advect == 0.0 ? 1.0 : u_advect);
    float freq = (1.8 + 3.5*a.mid) * (u_noiseFreq == 0.0 ? 1.0 : u_noiseFreq);
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
    vec3 col = base + pal * pow(dens, 1.2 + 0.8*a.high);
    col *= (u_colorGain == 0.0 ? 1.0 : u_colorGain);

    // Add streaks along the velocity direction (anisotropic look)
    float streaks = abs(dot(normalize(v), normalize(uv)));
    col += streaks * 0.25 * vec3(0.8, 0.9, 1.2) * (0.2 + 0.8*a.high);

    // Onset flash and pulse
    float flash = clamp(u_onset + a.onsetAvg, 0.0, 1.0);
    col += flash * 0.15 * vec3(1.0, 0.9, 0.8);

    // Soft vignette
    float r = length(uv);
    float vig = smoothstep(0.95, 0.25, r);
    col *= mix(0.85, 1.0, vig);

    FragColor = vec4(pow(col, vec3(0.9)), 1.0);
}


