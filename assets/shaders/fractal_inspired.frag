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
uniform float u_sectionChange;

out vec4 FragColor;

#define N normalize
float smooth01(float x){ return smoothstep(0.0, 1.0, clamp(x, 0.0, 1.0)); }
float softCompress(float x){ return x / (1.0 + x); }

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
    p.y -= 2.75;
    p *= vec3(0.6, 0.5, 0.4);
    p += cos(p.yzx * 2.0 + p.xzy * 3.0 + p.zxy * 4.0) * 0.15;
    for (s = 0.0, w = 0.5; s++ < 8.0; p *= l, w *= l){
        p = abs(sin(p)) - 1.0;
        l = 1.15 / dot(p, p);
    }
    return length(p) / w;
}

vec3 look(vec3 p, float zoffs){
    float t = u_time * 0.5;
    return p - vec3(
        tanh(cos(t * 0.4) * 0.5) * 6.0 ,
        tanh(cos(t * 0.6) * 2.0 ) * 5.0 - 5.0,
        zoffs + (u_time * 0.3) * 5.0 + tanh(cos(t * 0.4) * 4.0) * 4.0 
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

    vec3 e = vec3(0.01, 0.0, 0.0);
    vec3 ro = vec3(0.0);
    ro = vec3(0.0, 0.0, (u_time * 0.3) * 5.0);
    vec3 p = ro;
    vec3 Z = N(ro - look(p, 8.0) - p);
    vec3 X = N(vec3(Z.z, 0.0, -Z.x));
    vec3 D = N(vec3(u, 1.0) * mat3(-X, cross(X, Z), Z));

    vec4 o = vec4(0.0);
    float d = 0.0, s = 0.0;
    for (float i = 0.0; i < 128.0; i += 1.0){
        float pulse = clamp(0.38 * max(u_beatEnv, u_beatPulse) + 0.16 * u_kickImpact + 0.10 * u_dropEvent, 0.0, 1.0);
        p = ro + D * d + pulse * 0.28 * (sin(u_time) * 0.5 + 0.5) + 0.16 * softCompress(u_tension + u_energyLevel);
        s = map(p) + (0.25 * u_bands[0] + 0.45 * u_bassBody + 0.20 * u_subBody)/1000.0;
        d += s;
        o += 0.1 * (10.0 * vec4(9,2,1,0) + 0.1 * vec4(1,2,6,0) / (0.001 + abs(s)));
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
    o.rgb *= tx;
    o *= AO(p, r);

    // tone and palette shift to purples
    vec3 col = o.rgb;
    col = pow(col, vec3(1.2));
    col *= vec3(1.55 + 0.20*u_leadPresence, 1.20 + 0.15*u_harmonicBody, 1.70 + 0.25*u_airPresence);
    col = tanh(sqrt(d * col / 1e8 * exp(d / 7.0)));
    col = col / (col + 1.0);
    col = pow(col, vec3(0.4545));
    col *= 0.90 + 0.14*softCompress(u_energyLevel + u_release) + 0.10*u_dropEvent + 0.06*u_airPresence;

    FragColor = vec4(col, 1.0);
}
