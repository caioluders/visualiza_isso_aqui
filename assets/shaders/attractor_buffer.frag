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

const float PI = 3.14159265359;

vec2 Dir(float a){ return vec2(cos(a), sin(a)); }
float smooth01(float x){ return smoothstep(0.0, 1.0, clamp(x, 0.0, 1.0)); }
float softCompress(float x){ return x / (1.0 + x); }

void main(){
    vec2 res = iResolution.x > 0.0 ? iResolution : u_resolution;
    vec2 px  = gl_FragCoord.xy;
    vec2 uv  = (px - 0.5 * res) / max(res.y, 1.0);
    float t  = u_time;

    // Derive an activity factor from audio
    float pulse = clamp(0.42 * max(u_beatEnv, u_beatPulse) + 0.18 * u_kickImpact + 0.12 * u_snareImpact + 0.10 * u_dropEvent, 0.0, 1.0);
    float activity = smooth01(0.32 * u_energyLevel + 0.22 * pulse + 0.10 * u_hatTick + 0.18 * u_novelty + 0.18 * u_tension);
    float hi = smooth01(0.20 * u_bandHigh + 0.45 * u_airPresence + 0.20 * u_brightness + 0.15 * u_hatTick);
    float lo = smooth01(0.18 * u_bandLow + 0.32 * u_subBody + 0.38 * u_bassBody + 0.12 * u_release);
    float mid = smooth01(0.12 * u_bandMid + 0.48 * u_harmonicBody + 0.28 * u_leadPresence + 0.12 * u_release);
    float flowLift = 0.85 + 0.22 * softCompress(u_tension + u_energyLevel);

    float d = 0.0;
    const float n = 10.0;
    for (float i = 0.0; i < n; i += 1.0) {
        float a = atan(uv.y, uv.x);
        // Speed and amplitude modulated by audio
        float speed = mix(0.6, 2.5, clamp(activity + 0.18 * u_tension, 0.0, 1.0));
        float amp   = (0.05 + 0.06 * hi + 0.04 * pulse + 0.03 * u_hatTick + 0.03 * u_percussiveFocus) * flowLift;
        vec2 dir = amp * sin(2.0*a + t * speed) * Dir(8.0*PI*uv.x)
                 + amp * cos(2.0*a + t * speed) * Dir(8.0*PI*uv.y);
        // Bass pushes flow radially
        dir += 0.018 * (0.65 * lo + 0.35 * pulse) * Dir(6.0*PI*uv.y + t * (0.5 + activity));
        d += length(dir) / (i + 1.0);
        uv += dir;
    }
    // Trail decay scales with activity (more activity -> slower decay)
    float k = mix(16.0, 8.0, activity);
    vec3 col = vec3(exp(-k * d * d));
    // Simple spectral tint
    vec3 tint = vec3(0.35 + 0.58 * lo + 0.08 * u_harmE + 0.06 * pulse + 0.08 * u_dropEvent,
                     0.35 + 0.60 * mid + 0.08 * u_release + 0.05 * u_leadPresence,
                     0.35 + 0.58 * hi + 0.08 * u_percRatio + 0.06 * u_sectionChange + 0.05 * u_novelty);
    col *= tint;
    FragColor = vec4(col, 1.0);
}
