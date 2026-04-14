/* Ray-marched generative fractal inspired by snippet (with AO, tex3D),
   adapted to GL 3.2 and this app's uniforms. Optional iChannel0 for texturing.
*/

#version 150
precision highp float;

uniform vec2  u_resolution;
uniform vec2  iResolution; // optional fallback
uniform float u_time;
uniform sampler2D iChannel0; // optional (can be feedback/noise)

// audio analysis (app-provided)
uniform float u_rms;
uniform float u_bandLow;
uniform float u_bandMid;
uniform float u_bandHigh;
uniform float u_onset;
uniform float u_bpm;
uniform float u_beatEnv;
uniform float u_percE;
uniform float u_harmE;
uniform float u_percRatio;
uniform float u_flux;
uniform float u_centroidNorm;
uniform float u_bands[16];
uniform float u_onsets[16];
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

#define N normalize

vec3 chromaTint() {
    vec3 c = vec3(
        0.16 + u_chroma[0] + 0.70*u_chroma[7] + 0.45*u_chroma[9],
        0.16 + u_chroma[2] + 0.75*u_chroma[4] + 0.35*u_chroma[11],
        0.16 + u_chroma[3] + 0.70*u_chroma[5] + 0.55*u_chroma[10]
    );
    return normalize(c);
}

// tri-planar texture blend (@Shane style)
vec3 tex3D(sampler2D tex, in vec3 p, in vec3 n){
    n = max((abs(n) - 0.2) * 7.0, 0.001);
    n /= (n.x + n.y + n.z);
    vec2 a = fract(p.yz * 0.5 + 0.5);
    vec2 b = fract(p.zx * 0.5 + 0.5);
    vec2 c = fract(p.xy * 0.5 + 0.5);
    return texture(tex, a).xyz * n.x + texture(tex, b).xyz * n.y + texture(tex, c).xyz * n.z;
}

// smooth min (@Shane/@Alex Evans talk)
float smin(float a, float b, float k){
   float f = max(0.0, 1.0 - abs(b - a) / k);
   return min(a, b) - k * 0.25 * f * f;
}

float fractal(vec3 p){
    float s, w, l = 1.0;
    float bass = (u_bands[0] + u_bands[1] + u_bands[2]) / 3.0;
    float mid = 0.0; for (int i=3; i<=9; ++i) mid += u_bands[i]; mid /= 7.0;
    float high = 0.0; for (int i=10; i<16; ++i) high += u_bands[i]; high /= 6.0;
    p.y -= 2.75 + 0.25*u_buildUp - 0.18*u_drop;
    p *= vec3(0.58 + 0.12*bass, 0.48 + 0.10*mid, 0.38 + 0.12*high);
    p += cos(p.yzx * (1.8 + 0.8*mid) + p.xzy * 3.0 + p.zxy * (3.5 + 1.2*u_rolloff + 0.6*u_contrastMean)) * (0.12 + 0.08*u_energyDelta + 0.08*u_novelty);
    for (s = 0.0, w = 0.5; s++ < 8.0; p *= l, w *= l){
        p = abs(sin(p)) - 1.0;
        l = (1.10 + 0.12*u_beatEnv + 0.08*u_buildUp) / dot(p, p);
    }
    return length(p) / w;
}

vec3 look(vec3 p, float zoffs){
    float t = u_time * (0.35 + 0.35*u_buildUp + 0.20*u_bandHigh);
    return p - vec3(
        tanh(cos(t * 0.4 + u_layerChange) * 0.5) * 6.0 ,
        tanh(cos(t * 0.6) * 2.0 ) * 5.0 - 5.0 + 0.7*u_drop,
        zoffs + (u_time * (0.22 + 0.18*u_bandLow)) * 5.0 + tanh(cos(t * 0.4) * 4.0) * 4.0
    );
}

float map(vec3 p){
    float s = fractal(p);
    s = smin(s, 2.0 - p.y, length(p.xy));
    return s;
}

// iq-style AO
float AO(in vec3 pos, in vec3 nor){
    float sca = 2.2, occ = 0.0;
    for(int i = 0; i < 5; i++){
        float hr = 0.01 + float(i) * 0.5 / 4.0;
        float dd = map(nor * hr + pos);
        occ += (hr - dd) * sca;
        sca *= 0.7;
    }
    return clamp(1.0 - occ, 0.0, 1.0);
}

void main(){
    vec2 R = (iResolution.x > 0.0) ? iResolution : u_resolution;
    vec2 u = (gl_FragCoord.xy - R * 0.5) / max(R.y, 1.0);
    float bass = (u_bands[0] + u_bands[1] + u_bands[2]) / 3.0;
    float mid = 0.0; for (int bi=3; bi<=9; ++bi) mid += u_bands[bi]; mid /= 7.0;
    float high = 0.0; for (int bi=10; bi<16; ++bi) high += u_bands[bi]; high /= 6.0;
    float onsetAvg = 0.0; for (int bi=0; bi<16; ++bi) onsetAvg += u_onsets[bi]; onsetAvg /= 16.0;
    float pulse = clamp(0.40*u_beatEnv + 0.20*onsetAvg + 0.18*u_onsetDensity + 0.24*u_drop + 0.18*u_isolatedHit, 0.0, 1.0);
    float groove = mix(sin(6.2831853 * max(u_bpm, 30.0) * u_time / 60.0), sin(6.2831853 * u_beatPhase), u_beatConfidence);

    vec3 e = vec3(0.01, 0.0, 0.0);
    vec3 ro = vec3(0.0);
    ro = vec3(0.0, 0.0, (u_time * (0.22 + 0.18*bass + 0.15*u_buildUp)) * 5.0);
    vec3 p = ro;
    vec3 Z = N(ro - look(p, 8.0) - p);
    vec3 X = N(vec3(Z.z, 0.0, -Z.x));
    vec3 D = N(vec3(u, 1.0) * mat3(-X, cross(X, Z), Z));

    vec4 o = vec4(0.0);
    float d = 0.0, s = 0.0;
    for (float i = 0.0; i < 128.0; i += 1.0){
        p = ro + D * d + (u_beatEnv * 0.42 + u_drop * 0.32 + u_lowDensity * 0.16) * (0.5 + 0.5*groove + 0.08*sin(u_time + i*0.03));
        s = map(p) + 0.012*bass + 0.006*u_energyDelta;
        d += s;
        vec4 hot = vec4(8.0 + 3.0*bass, 2.5 + 4.0*mid, 1.5 + 5.0*high + 2.5*u_contrastMean, 0.0);
        vec4 cold = vec4(1.0 + 2.0*u_percRatio, 2.0 + 2.0*u_rolloff, 6.0 + 2.0*u_flatness, 0.0);
        o += 0.1 * (8.0 * hot + 0.12 * cold / (0.001 + abs(s)));
        if (d > 40.0) break;
    }

    // normal via finite differences
    vec3 r = N(vec3(
        map(p) - map(p - e.xyy),
        map(p) - map(p - e.yxy) ,
        map(p) - map(p - e.yyx)
    ));

    // texture modulation (if iChannel0 bound)
    vec3 tx = tex3D(iChannel0, p * 0.5, r);
    float txUse = smoothstep(0.02, 0.18, dot(tx, vec3(0.333)));
    o.rgb *= mix(vec3(1.0), tx, txUse);
    o *= AO(p, r);

    // tone and audio palette shift
    vec3 col = o.rgb;
    col = pow(col, vec3(1.2));
    vec3 tint = vec3(0.05, 0.80, 0.74)*bass + vec3(1.00, 0.36, 0.22)*mid + vec3(0.98, 0.82, 0.20)*high;
    tint = mix(tint, chromaTint(), 0.18 + 0.35*u_chromaFlux);
    tint = mix(tint, vec3(0.62, 0.88, 1.0), 0.20*u_percRatio + 0.15*u_rolloff);
    col *= 1.15 + 1.10*tint;
    col = tanh(sqrt(d * col / 1e8 * exp(d / 7.0)));
    col += (0.14*pulse + 0.18*u_drop) * vec3(1.0, 0.82, 0.55);
    col *= mix(1.0, 0.62, u_breakState);
    col = col / (col + 1.0);
    col = pow(col, vec3(0.4545));

    FragColor = vec4(col, 1.0);
}
