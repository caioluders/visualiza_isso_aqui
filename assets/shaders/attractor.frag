/* [SH17A] Lorenz attractor (single-pass with feedback), by mattz
   Licence: https://creativecommons.org/licenses/by-nc-sa/3.0/
   Expects previous frame bound as iChannel0
*/

#version 150
precision highp float;

uniform sampler2D iChannel0; // previous frame (feedback)
uniform vec2      iResolution; // optional (fallback from u_resolution)
uniform vec2      u_resolution; // app-specific

out vec4 FragColor;

#define f(x) texelFetch(iChannel0, ivec2(x), 0)

void main() {
    vec2 res = iResolution.x > 0.0 ? iResolution : u_resolution;
    vec2 p = gl_FragCoord.xy;

    vec4 c = f(p);

    p.x -= 250.0;

    c += (p.y > 1.0) ?
         f(vec2(0.0)) / exp(2.0 + length(p - 5.0 * f(vec2(0.0)).yx)) - c / 400.0 :
         vec4(
            c.z * c.y - 2.6 * c.x,
            10.0 * (c.z - c.y) + 0.1,
            c.y * (28.0 - c.x) - c.z,
            0.0
         ) / 100.0;

    FragColor = c;
}


