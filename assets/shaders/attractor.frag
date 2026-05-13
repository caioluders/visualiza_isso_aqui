/* [SH17A] Lorenz attractor (single-pass with feedback), by mattz
   Licence: https://creativecommons.org/licenses/by-nc-sa/3.0/
   Expects previous frame bound as iChannel0
*/

#version 150
precision highp float;

uniform sampler2D iChannel0; // previous frame (feedback)
uniform vec2      iResolution; // optional (fallback from u_resolution)
uniform vec2      u_resolution; // app-specific
uniform float     u_time;
uniform float     u_kickImpact;
uniform float     u_bassBody;
uniform float     u_harmonicBody;
uniform float     u_airPresence;
uniform float     u_tension;
uniform float     u_dropEvent;
uniform float     u_beatPulse;
uniform float     u_energyLevel;
uniform float     u_release;
uniform float     u_novelty;

out vec4 FragColor;

#define f(x) texelFetch(iChannel0, ivec2(x), 0)

float smooth01(float x){ return smoothstep(0.0, 1.0, clamp(x, 0.0, 1.0)); }
float softCompress(float x){ return x / (1.0 + x); }

void main() {
    vec2 res = iResolution.x > 0.0 ? iResolution : u_resolution;
    vec2 p = gl_FragCoord.xy;

    vec4 c = f(p);

    p.x -= 250.0;

    float pulse = clamp(0.45 * max(u_beatPulse, u_kickImpact) + 0.20 * u_dropEvent + 0.12 * u_novelty, 0.0, 1.0);
    float bass = smooth01(0.75 * u_bassBody + 0.25 * u_energyLevel);
    float body = smooth01(0.70 * u_harmonicBody + 0.30 * u_release);
    float air  = smooth01(0.75 * u_airPresence + 0.25 * u_tension);
    float macro = smooth01(0.55 * u_energyLevel + 0.30 * u_tension + 0.15 * u_release);
    float pull = 5.0 + 2.2 * bass + 1.8 * macro;
    c += (p.y > 1.0) ?
         f(vec2(0.0)) / exp(2.0 + length(p - pull * f(vec2(0.0)).yx)) - c / (420.0 - 100.0 * pulse) :
         vec4(
            c.z * c.y - (2.1 + 0.45 * bass + 0.25 * pulse) * c.x,
            (8.8 + 1.8 * body + 0.8 * macro) * (c.z - c.y) + 0.1 + 0.08 * air,
            c.y * (28.0 + 3.5 * softCompress(u_dropEvent + pulse) - c.x) - c.z,
            0.0
         ) / 100.0;

    FragColor = c;
}
