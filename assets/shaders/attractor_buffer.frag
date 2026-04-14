/* Attractor buffer (standalone, GL 3.2, app-compatible uniforms)
   Ported from a ShaderToy-style snippet to our app's shader system.
   Uses u_time / iResolution (fallback from u_resolution) and writes a trail field.
*/

#version 150
precision highp float;

uniform vec2  iResolution;   // optional
uniform vec2  u_resolution;  // app-provided
uniform float u_time;        // app-provided time

// Audio analysis (app-provided). Absent uniforms default to 0.
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
uniform float u_bands[16];
uniform float u_onsets[16];
uniform float u_centroidNorm;
uniform float u_flux;
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

const float PI = 3.14159265359;

vec2 Dir(float a){ return vec2(cos(a), sin(a)); }

void main(){
    vec2 res = iResolution.x > 0.0 ? iResolution : u_resolution;
    vec2 px  = gl_FragCoord.xy;
    vec2 uv  = (px - 0.5 * res) / max(res.y, 1.0);
    float t  = u_time;

    // Derive an activity factor from audio
    float bass = (u_bands[0] + u_bands[1] + u_bands[2]) / 3.0;
    float midB = 0.0; for (int bi=3; bi<=9; ++bi) midB += u_bands[bi]; midB /= 7.0;
    float hiB = 0.0; for (int bi=10; bi<16; ++bi) hiB += u_bands[bi]; hiB /= 6.0;
    float onsetAvg = 0.0; for (int bi=0; bi<16; ++bi) onsetAvg += u_onsets[bi]; onsetAvg /= 16.0;
    float pulse = clamp(0.40*u_beatEnv + 0.20*onsetAvg + 0.18*u_onsetDensity + 0.24*u_drop + 0.18*u_isolatedHit, 0.0, 1.0);
    float activity = clamp(u_rms * 0.7 + pulse * 0.75 + u_percE * 0.18 + u_buildUp * 0.42 + u_novelty * 0.35, 0.0, 1.0);
    float hi = clamp(0.45*u_bandHigh + 0.35*hiB + 0.20*u_rolloff + 0.20*u_contrastMean, 0.0, 1.0);
    float lo = clamp(0.50*u_bandLow + 0.50*bass, 0.0, 1.0);
    float mid = clamp(0.50*u_bandMid + 0.50*midB + 0.20*u_energyDelta, 0.0, 1.0);

    float d = 0.0;
    const float n = 10.0;
    for (float i = 0.0; i < n; i += 1.0) {
        float a = atan(uv.y, uv.x);
        // Speed and amplitude modulated by audio
        float speed = mix(0.35, 2.8, activity) + 0.30*u_buildUp + 0.22*u_beatConfidence;
        float amp   = 0.04 + 0.12 * (hi + 0.6 * pulse + 0.20*u_flux + 0.18*u_highDensity);
        vec2 dir = amp * sin(2.0*a + t * speed) * Dir(8.0*PI*uv.x)
                 + amp * cos(2.0*a + t * speed) * Dir(8.0*PI*uv.y);
        // Bass pushes flow radially
        dir += 0.018 * lo * Dir(6.0*PI*uv.y + t * (0.5 + activity));
        dir += 0.012 * u_layerChange * Dir(10.0*PI*uv.x + t);
        d += length(dir) / (i + 1.0);
        uv += dir;
    }
    // Trail decay scales with activity (more activity -> slower decay)
    float k = mix(17.0, 7.0, activity);
    k = mix(k, 22.0, 0.45*u_breakState);
    vec3 col = vec3(exp(-k * d * d));
    // Simple spectral tint
    vec3 tint = vec3(0.35 + 0.65 * lo,
                     0.35 + 0.65 * mid,
                     0.35 + 0.65 * hi);
    vec3 chroma = normalize(vec3(
        0.16 + u_chroma[0] + 0.70*u_chroma[7] + 0.45*u_chroma[9],
        0.16 + u_chroma[2] + 0.75*u_chroma[4] + 0.35*u_chroma[11],
        0.16 + u_chroma[3] + 0.70*u_chroma[5] + 0.55*u_chroma[10]
    ));
    tint = mix(tint, chroma, 0.18 + 0.35*u_chromaFlux);
    tint = mix(tint, vec3(1.0, 0.78, 0.52), 0.18*u_percRatio + 0.26*u_drop);
    col *= tint;
    FragColor = vec4(col, 1.0);
}
