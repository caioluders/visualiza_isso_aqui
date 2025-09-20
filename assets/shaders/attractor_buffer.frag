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

out vec4 FragColor;

const float PI = 3.14159265359;

vec2 Dir(float a){ return vec2(cos(a), sin(a)); }

void main(){
    vec2 res = iResolution.x > 0.0 ? iResolution : u_resolution;
    vec2 px  = gl_FragCoord.xy;
    vec2 uv  = (px - 0.5 * res) / max(res.y, 1.0);
    float t  = u_time;

    // Derive an activity factor from audio
    float activity = clamp(u_rms * 1.2 + u_onset * 0.8 + u_percE * 0.6, 0.0, 1.0);
    float hi = clamp(u_bandHigh, 0.0, 1.0);
    float lo = clamp(u_bandLow, 0.0, 1.0);
    float mid = clamp(u_bandMid, 0.0, 1.0);

    float d = 0.0;
    const float n = 10.0;
    for (float i = 0.0; i < n; i += 1.0) {
        float a = atan(uv.y, uv.x);
        // Speed and amplitude modulated by audio
        float speed = mix(0.6, 2.2, activity);
        float amp   = 0.06 + 0.14 * (hi + 0.5 * u_onset);
        vec2 dir = amp * sin(2.0*a + t * speed) * Dir(8.0*PI*uv.x)
                 + amp * cos(2.0*a + t * speed) * Dir(8.0*PI*uv.y);
        // Bass pushes flow radially
        dir += 0.02 * lo * Dir(6.0*PI*uv.y + t * (0.5 + activity));
        d += length(dir) / (i + 1.0);
        uv += dir;
    }
    // Trail decay scales with activity (more activity -> slower decay)
    float k = mix(16.0, 8.0, activity);
    vec3 col = vec3(exp(-k * d * d));
    // Simple spectral tint
    vec3 tint = vec3(0.35 + 0.65 * lo,
                     0.35 + 0.65 * mid,
                     0.35 + 0.65 * hi);
    col *= tint;
    FragColor = vec4(col, 1.0);
}