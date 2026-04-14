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

vec3 chromaTint() {
    vec3 c = vec3(
        0.16 + u_chroma[0] + 0.70*u_chroma[7] + 0.45*u_chroma[9],
        0.16 + u_chroma[2] + 0.75*u_chroma[4] + 0.35*u_chroma[11],
        0.16 + u_chroma[3] + 0.70*u_chroma[5] + 0.55*u_chroma[10]
    );
    return normalize(c);
}

// Scene distance field
float mapScene(vec3 p, out float id) {
    // Repeat a lattice of spheres, subtly warped by audio bands
    float bass = (u_bands[0] + u_bands[1] + u_bands[2]) / 3.0;
    float mid = 0.0; for (int i=3;i<=9;i++) mid += u_bands[i]; mid /= 7.0;
    float high = 0.0; for (int i=10;i<16;i++) high += u_bands[i]; high /= 6.0;
    float pulse = clamp(0.45*u_beatEnv + 0.22*u_drop + 0.18*u_isolatedHit + 0.18*u_lowDensity, 0.0, 1.0);
    float detail = clamp(0.30*u_rolloff + 0.25*u_contrastMean + 0.20*u_crest + 0.15*high + 0.10*u_novelty, 0.0, 1.0);

    // Domain warp (scaled by UI control)
    float W = u_warpIntensity * (1.0 + 0.30*u_buildUp + 0.20*u_energyDelta + 0.28*u_novelty);
    p.xy *= rot(W * (0.2 * sin(u_time*0.27) + 0.3 * mid + 0.18*u_layerChange));
    p.yz *= rot(W * (0.2 * cos(u_time*0.21) + 0.2 * high + 0.10*detail));
    p.xz *= rot(W * (0.15 * sin(u_time*0.17) + 0.2 * bass + 0.12*pulse));

    vec3 cell = vec3(2.7 + 1.2*bass + 0.35*u_drop,
                     2.7 + 0.7*mid + 0.25*u_buildUp,
                     3.1 + 0.9*detail + 0.25*u_highDensity) * max(0.2, u_cellScale);
    vec3 q = opRep(p, cell);

    // Multi-radius spheres give moiré-like interference
    float r1 = u_r1Base + 0.32*bass + 0.12*pulse;
    float r2 = u_r2Base + 0.22*detail + 0.08*u_isolatedHit + 0.10*u_contrastBands[4];
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
            float mid  = 0.0; for (int k=3;k<=9;k++) mid += u_bands[k]; mid /= 7.0;
            float high = 0.0; for (int k=10;k<16;k++) high += u_bands[k]; high /= 6.0;
            float detail = clamp(0.30*u_rolloff + 0.25*u_contrastMean + 0.20*u_crest + 0.15*high + 0.10*u_novelty, 0.0, 1.0);
            float pulse = clamp(0.45*u_beatEnv + 0.22*u_drop + 0.18*u_isolatedHit + 0.18*u_lowDensity, 0.0, 1.0);

            vec3 base = mix(vec3(0.035, 0.040, 0.050), vec3(0.040, 0.025, 0.035), u_breakState);
            vec3 aCol = vec3(0.05, 0.78, 0.72) * bass + vec3(0.98, 0.34, 0.22) * mid + vec3(0.96, 0.78, 0.20) * detail;
            aCol = mix(aCol, chromaTint(), 0.20 + 0.30*u_chromaFlux);
            aCol = mix(aCol, vec3(0.62, 0.88, 1.0), 0.20*u_percRatio + 0.15*u_rolloff);
            vec3 surf = base + aCol * (0.35 + 0.65*diff) + vec3(spec) * (0.7 + 0.6*pulse);
            // Rim accent
            float rim = pow(1.0 - clamp(dot(n, -rd), 0.0, 1.0), 2.0);
            surf += rim * vec3(0.5 + 0.5*detail, 0.2 + 0.6*mid, 0.5 + 0.5*bass) * (0.45 + 0.45*u_layerChange);

            col = surf;
            break;
        }
        t += d * clamp(u_stepFactor, 0.05, 1.0);
        if (t > 60.0) break;
    }
    // Fog/background
    vec3 bg = mix(vec3(0.020, 0.028, 0.038), vec3(0.040, 0.020, 0.030), u_breakState)
            + 0.04*chromaTint()*(0.4 + 0.6*u_rolloff);
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
    float velDerived = mix(0.6, 5.8, clamp(0.25*onsetAvg + 0.25*u_onsetDensity + 0.25*u_rms + 0.15*u_buildUp + 0.10*u_novelty, 0.0, 1.0));
    float vel = (u_useCamVel > 0.5) ? u_camVel : velDerived;
    vel *= 1.0 + 0.12*u_beatEnv + 0.10*u_drop;

    // Camera path with swirl; speed scales with vel
    float bpmPhase = mix(sin(6.2831853 * max(u_bpm, 30.0) * u_time / 60.0), sin(6.2831853 * u_beatPhase), u_beatConfidence);
    float t = u_time * vel;
    vec3 ro = vec3(0.0, 0.0, -3.0 + t*2.5);
    float swirl = 0.6 * sin(0.3*t + 0.2*bpmPhase) + 0.25*u_layerChange;
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
    float flash = clamp(u_flashGain * (u_onset + 0.5*onsetAvg + 0.35*u_onsetDensity + 0.8*u_drop + 0.4*u_isolatedHit), 0.0, 1.0);
    col += flash * vec3(0.18, 0.14, 0.10);

    col *= u_colorGain;
    FragColor = vec4(pow(col, vec3(0.9)), 1.0);
}
