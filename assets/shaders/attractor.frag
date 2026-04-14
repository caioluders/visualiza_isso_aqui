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
uniform float     u_rms;
uniform float     u_bandLow;
uniform float     u_bandMid;
uniform float     u_bandHigh;
uniform float     u_onset;
uniform float     u_beatEnv;
uniform float     u_centroidNorm;
uniform float     u_drop;
uniform float     u_breakState;
uniform float     u_buildUp;
uniform float     u_layerChange;
uniform float     u_isolatedHit;
uniform float     u_rolloff;
uniform float     u_flatness;
uniform float     u_crest;
uniform float     u_contrastBands[6];
uniform float     u_contrastMean;
uniform float     u_chroma[12];
uniform float     u_chromaFlux;
uniform float     u_onsetDensity;
uniform float     u_lowDensity;
uniform float     u_highDensity;
uniform float     u_beatPhase;
uniform float     u_beatConfidence;
uniform float     u_novelty;

out vec4 FragColor;

#define f(x) texelFetch(iChannel0, ivec2(x), 0)

void main() {
    vec2 res = iResolution.x > 0.0 ? iResolution : u_resolution;
    vec2 p = gl_FragCoord.xy;

    vec4 c = f(p);

    p.x -= 250.0;
    float pulse = clamp(0.40*u_beatEnv + 0.22*u_onset + 0.18*u_onsetDensity + 0.28*u_drop + 0.18*u_isolatedHit, 0.0, 1.0);
    float groove = mix(sin(u_time * 0.5), sin(6.2831853 * u_beatPhase), u_beatConfidence);
    float sigma = 10.0 + 1.6*u_bandMid + 0.7*u_buildUp + 0.8*u_contrastMean;
    float rho = 28.0 + 5.4*u_bandLow + 3.6*u_drop + 1.4*u_lowDensity;
    float beta = 2.6 + 0.45*u_bandHigh + 0.35*u_rolloff + 0.18*u_crest;
    float trail = mix(420.0, 260.0, clamp(u_rms + pulse, 0.0, 1.0));
    trail = mix(trail, 180.0, 0.45*u_breakState);

    c += (p.y > 1.0) ?
         f(vec2(0.0)) / exp(2.0 + length(p - (5.0 + 1.5*pulse) * f(vec2(0.0)).yx)) - c / trail :
         vec4(
            c.z * c.y - beta * c.x,
            sigma * (c.z - c.y) + 0.1 + 0.03*sin(u_time + u_layerChange*6.0 + 0.5*groove),
            c.y * (rho - c.x) - c.z,
            0.0
         ) / 100.0;

    if (p.y > 1.0) {
        vec3 tint = normalize(vec3(0.05, 0.80, 0.72)*(0.35 + u_bandLow) +
                              vec3(1.00, 0.36, 0.22)*(0.25 + u_bandMid) +
                              vec3(0.98, 0.82, 0.20)*(0.20 + u_bandHigh));
        vec3 chroma = normalize(vec3(0.16 + u_chroma[0] + 0.70*u_chroma[7] + 0.45*u_chroma[9],
                                     0.16 + u_chroma[2] + 0.75*u_chroma[4] + 0.35*u_chroma[11],
                                     0.16 + u_chroma[3] + 0.70*u_chroma[5] + 0.55*u_chroma[10]));
        tint = mix(tint, chroma, 0.18 + 0.35*u_chromaFlux);
        c.rgb *= mix(vec3(0.96), tint*(0.85 + 0.45*pulse), 0.35 + 0.35*u_layerChange + 0.20*u_novelty);
    }

    FragColor = c;
}
