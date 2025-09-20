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

// UI-driven tweakables
uniform float u_camVel;       // Smoothed camera velocity (0.6..5.5 typical)
uniform float u_useCamVel;    // >0.5 to force using u_camVel
uniform float u_warpIntensity;// 0..1 domain warp weight
uniform float u_stepFactor;   // 0.05..1.0 march step factor
uniform float u_epsilon;      // hit epsilon (e.g. 0.0015)
uniform float u_maxStepsF;    // max steps as float (will be cast to int)
uniform float u_fogDensity;   // fog density (e.g. 0.015)
uniform float u_barrelK;      // barrel warp coeff (0..0.3)
uniform float u_colorGain;    // post color gain (0.5..2.0)
uniform float u_cellScale;    // repetition scale (~1.0)
uniform float u_r1Base;       // base radius sphere1
uniform float u_r2Base;       // base radius sphere2
uniform float u_flashGain;    // 0..1 flash gain

out vec4 FragColor;

// --- SDF helpers ---
float sdSphere(vec3 p, float r) { return length(p) - r; }

// Repeat space by period c
vec3 opRep(vec3 p, vec3 c) {
    return mod(p + 0.5*c, c) - 0.5*c;
}

mat2 rot(float a){ float c = cos(a), s = sin(a); return mat2(c,-s,s,c); }

// Scene distance field
float mapScene(vec3 p, out float id) {
    // Repeat a lattice of spheres, subtly warped by audio bands
    float bass = (u_bands[0] + u_bands[1] + u_bands[2]) / 3.0;
    float high = 0.0; for (int i=10;i<16;i++) high += u_bands[i]; high /= 6.0;

    // Domain warp (scaled by UI control)
    float W = u_warpIntensity;
    p.xy *= rot(W * (0.2 * sin(u_time*0.27) + 0.3 * u_bandMid));
    p.yz *= rot(W * (0.2 * cos(u_time*0.21) + 0.2 * high));
    p.xz *= rot(W * (0.15 * sin(u_time*0.17) + 0.2 * bass));

    vec3 cell = vec3(2.7 + 1.2*bass, 2.7 + 0.7*u_bandMid, 3.1 + 0.9*high) * max(0.2, u_cellScale);
    vec3 q = opRep(p, cell);

    // Multi-radius spheres give moiré-like interference
    float r1 = u_r1Base + 0.35*bass;
    float r2 = u_r2Base + 0.25*high;
    float d1 = sdSphere(q, r1);
    float d2 = sdSphere(q, r2);
    float d = min(d1, d2);
    id = (d1 < d2) ? 1.0 : 2.0;
    return d;
}

// Numerical normal from gradient
vec3 calcNormal(vec3 p){
    float id; 
    float e = max(1e-5, u_epsilon);
    vec2 h = vec2(1.0, -1.0) * e;
    return normalize(vec3(
        mapScene(p + vec3(h.x,0,0), id) - mapScene(p + vec3(h.y,0,0), id),
        mapScene(p + vec3(0,h.x,0), id) - mapScene(p + vec3(0,h.y,0), id),
        mapScene(p + vec3(0,0,h.x), id) - mapScene(p + vec3(0,0,h.y), id)
    ));
}

// Raymarch
vec3 raymarch(vec3 ro, vec3 rd){
    float t = 0.0;
    float id = 0.0;
    vec3 col = vec3(0.0);
    int maxSteps = int(clamp(u_maxStepsF, 1.0, 256.0));
    for (int i=0; i<maxSteps; ++i){
        vec3 p = ro + rd*t;
        float d = mapScene(p, id);
        if (d < 0.001){
            // Shading
            vec3 n = calcNormal(p);
            vec3 l = normalize(vec3(0.6, 0.8, 0.2));
            float diff = clamp(dot(n,l), 0.0, 1.0);
            float spec = pow(clamp(dot(reflect(-l,n), -rd), 0.0, 1.0), 16.0 + 64.0*u_bandHigh);

            // Psychedelic color from audio bands
            float bass = (u_bands[0] + u_bands[1] + u_bands[2]) / 3.0;
            float mid  = 0.0; for (int k=3;k<=9;k++) mid += u_bands[k]; mid /= 2.0;
            float high = 0.0; for (int k=10;k<16;k++) high += u_bands[k]; high /= 1.0;

            vec3 base = vec3(0.15, 0.10, 0.18);
            vec3 aCol = vec3(0.9, 0.2, 0.4) * bass + vec3(0.2, 0.8, 0.4) * mid + vec3(0.25, 0.35, 1.0) * high;
            vec3 surf = base + aCol * (0.4 + 0.6*diff) + vec3(spec);
            // Rim accent
            float rim = pow(1.0 - clamp(dot(n, -rd), 0.0, 1.0), 2.0);
            surf += rim * vec3(0.5 + 0.5*high, 0.2 + 0.6*mid, 0.5 + 0.5*bass) * 0.6;

            col = surf;
            break;
        }
        t += d * clamp(u_stepFactor, 0.05, 1.0);
        if (t > 60.0) break;
    }
    // Fog/background
    vec3 bg = vec3(0.03, 0.04, 0.06) + 0.05*vec3(0.6, 0.4, 1.0)*u_bandHigh;
    col = mix(bg, col, exp(-max(0.0, u_fogDensity)*t));
    return col;
}

void main(){
    vec2 res = u_resolution;
    float aspect = res.x / res.y;
    vec2 uv = (gl_FragCoord.xy - 0.5*res) / res.y;

    // Music velocity proxy from onsets and RMS
    float onsetSum = 0.0; for (int i=0;i<16;i++) onsetSum += u_onsets[i];
    float onsetAvg = onsetSum / 16.0;
    float velDerived = mix(0.6, 5.5, clamp(0.55*onsetAvg + 0.45*u_rms, 0.0, 1.0));
    float vel = (u_useCamVel > 0.5) ? u_camVel : velDerived;

    // Camera path with swirl; speed scales with vel
    float t = u_time * vel;
    vec3 ro = vec3(0.0, 0.0, -3.0 + t*2.5);
    float swirl = 0.6 * sin(0.3*t);
    vec3 ta = vec3(0.0);
    vec3 ww = normalize(ta - ro);
    vec3 uu = normalize(cross(vec3(0.0,1.0,0.0), ww));
    vec3 vv = cross(ww, uu);
    uu.xy *= rot(swirl);
    vv.xy *= rot(swirl);

    vec3 rd = normalize(uv.x*uu*aspect + uv.y*vv + 1.3*ww);

    // Slight barrel warp driven by highs
    rd.xy *= 1.0 + u_barrelK * dot(uv,uv);

    vec3 col = raymarch(ro, rd);

    // Global flash on onset
    float flash = clamp(u_flashGain * (u_onset + 0.6*onsetAvg), 0.0, 1.0);
    col += flash * vec3(0.15, 0.12, 0.18);

    col *= u_colorGain;
    FragColor = vec4(pow(col, vec3(0.9)), 1.0);
}


