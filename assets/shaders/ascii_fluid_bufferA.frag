#version 150

uniform vec2 u_resolution;
uniform float u_time;
uniform float u_kickImpact;
uniform float u_snareImpact;
uniform float u_hatTick;
uniform float u_subBody;
uniform float u_bassBody;
uniform float u_harmonicBody;
uniform float u_leadPresence;
uniform float u_airPresence;
uniform float u_transientDensity;
uniform float u_novelty;
uniform float u_energyLevel;
uniform float u_tension;
uniform float u_dropEvent;
uniform float u_release;
uniform float u_bands[16];
uniform float u_onsets[16];
uniform float u_injectionRow;         // @ui min=0.05 max=0.95 default=0.25 label=Injection_Row group=Fluid
uniform float u_injectionGain;        // @ui min=0.00 max=3.00 default=1.00 label=Injection_Gain group=Fluid
uniform float u_upwardForce;          // @ui min=0.00 max=3.00 default=0.45 label=Upward_Force group=Fluid
uniform float u_densityDecay;         // @ui min=0.85 max=0.999 default=0.996 label=Density_Decay group=Decay
uniform float u_velocityDecay;        // @ui min=0.85 max=0.999 default=0.997 label=Velocity_Decay group=Decay
uniform float u_silenceDensityDecay;  // @ui min=0.00 max=0.95 default=0.40 label=Silence_Density_Decay group=Decay
uniform float u_silenceVelocityDecay; // @ui min=0.00 max=0.95 default=0.25 label=Silence_Velocity_Decay group=Decay

uniform sampler2D iChannel0; // previous A
uniform sampler2D iChannel3; // previous/current D projected state

out vec4 FragColor;

vec2 decodeVel(vec4 s) { return s.xy * 2.0 - 1.0; }
vec4 encodeState(vec2 v, float bass, float body) { return vec4(clamp(v * 0.5 + 0.5, 0.0, 1.0), clamp(bass, 0.0, 1.0), clamp(body, 0.0, 1.0)); }

float sat(float x) { return clamp(x, 0.0, 1.0); }
float bandAt(float x) {
    float fi = clamp(x * 15.0, 0.0, 15.0);
    int i0 = int(floor(fi));
    int i1 = min(i0 + 1, 15);
    float t = fract(fi);
    return mix(u_bands[i0], u_bands[i1], t);
}
float onsetAt(float x) {
    float fi = clamp(x * 15.0, 0.0, 15.0);
    int i0 = int(floor(fi));
    int i1 = min(i0 + 1, 15);
    float t = fract(fi);
    return mix(u_onsets[i0], u_onsets[i1], t);
}
void main() {
    vec2 uv = gl_FragCoord.xy / u_resolution.xy;
    vec2 texel = 1.0 / u_resolution.xy;

    vec4 projPrev = texture(iChannel3, uv);
    vec2 velPrev = decodeVel(projPrev);
    float bassPrev = projPrev.z;
    float bodyPrev = projPrev.w;

    vec2 advUv = clamp(uv - velPrev * 0.018, texel * 0.5, 1.0 - texel * 0.5);
    vec4 adv = texture(iChannel3, advUv);
    vec2 vel = decodeVel(adv);
    float bass = adv.z;
    float body = adv.w;

    vec2 lapVel =
        decodeVel(texture(iChannel3, uv + vec2(texel.x, 0.0))) +
        decodeVel(texture(iChannel3, uv - vec2(texel.x, 0.0))) +
        decodeVel(texture(iChannel3, uv + vec2(0.0, texel.y))) +
        decodeVel(texture(iChannel3, uv - vec2(0.0, texel.y))) - 4.0 * vel;
    float lapBass =
        texture(iChannel3, uv + vec2(texel.x, 0.0)).z +
        texture(iChannel3, uv - vec2(texel.x, 0.0)).z +
        texture(iChannel3, uv + vec2(0.0, texel.y)).z +
        texture(iChannel3, uv - vec2(0.0, texel.y)).z - 4.0 * bass;
    float lapBody =
        texture(iChannel3, uv + vec2(texel.x, 0.0)).w +
        texture(iChannel3, uv - vec2(texel.x, 0.0)).w +
        texture(iChannel3, uv + vec2(0.0, texel.y)).w +
        texture(iChannel3, uv - vec2(0.0, texel.y)).w - 4.0 * body;

    vel += lapVel * 0.018;
    bass += lapBass * 0.010;
    body += lapBody * 0.012;

    float audioEnergy = sat(0.60 * u_energyLevel + 0.25 * u_subBody + 0.15 * u_transientDensity);
    float injectGain = max(0.0, u_injectionGain);
    float upwardForce = max(0.0, u_upwardForce);
    float styleGroove = sat(0.42 * u_bassBody + 0.28 * u_kickImpact + 0.16 * u_harmonicBody + 0.14 * (1.0 - u_tension));
    float styleRumble = sat(0.48 * u_subBody + 0.24 * u_bassBody + 0.18 * u_kickImpact + 0.10 * u_energyLevel);
    float styleLift = sat(0.28 * u_harmonicBody + 0.24 * u_leadPresence + 0.24 * u_airPresence + 0.24 * u_tension);
    float styleSparse = sat(0.36 * u_release + 0.22 * (1.0 - u_kickImpact) + 0.20 * (1.0 - u_bassBody) + 0.22 * (1.0 - u_transientDensity));
    float bassRow = mix(0.46, 0.38, styleRumble);
    float bodyRow = mix(0.82, 0.90, styleLift);
    float airRow = mix(0.82, 0.92, styleLift);
    float leadRow = mix(0.62, 0.74, styleLift);
    float kickRow = mix(0.12, 0.08, styleGroove);
    float percRow = mix(0.24, 0.18, sat(0.6 * styleGroove + 0.4 * styleLift));
    float bassXSharp = mix(3200.0, 2400.0, styleRumble);
    float topXSharp = mix(3000.0, 2200.0, styleLift);
    float ySharp = mix(1500.0, 900.0, styleLift + 0.3 * styleSparse);
    float groovePush = 0.016 + 0.018 * styleGroove;
    float rumblePush = 0.018 + 0.026 * styleRumble;
    float liftPush = 0.018 + 0.028 * styleLift;

    for (int i = 0; i < 128; ++i) {
        float xNorm = (float(i) + 0.5) / 128.0;
        float bassX = mix(0.08, 0.32, xNorm);
        float bodyX = mix(0.76, 0.96, xNorm);
        float airX = mix(0.04, 0.24, xNorm);
        float leadX = mix(0.56, 0.76, xNorm);
        float kickX = mix(0.06, 0.20, xNorm);
        float percX = mix(0.84, 0.96, xNorm);
        float band = sat(bandAt(xNorm));
        float onset = sat(onsetAt(xNorm));
        float intensity = max(band, onset * 0.9);
        if (intensity > 0.08) {
            float bassDx = uv.x - bassX;
            float bassDy = uv.y - bassRow;
            float bodyDx = uv.x - bodyX;
            float bodyDy = uv.y - bodyRow;
            float airDx = uv.x - airX;
            float airDy = uv.y - airRow;
            float leadDx = uv.x - leadX;
            float leadDy = uv.y - leadRow;
            float kickDx = uv.x - kickX;
            float kickDy = uv.y - kickRow;
            float percDx = uv.x - percX;
            float percDy = uv.y - percRow;
            float bassSplat = exp(-(bassDx * bassDx * bassXSharp + bassDy * bassDy * ySharp));
            float bodySplat = exp(-(bodyDx * bodyDx * topXSharp + bodyDy * bodyDy * ySharp));
            float airSplat = exp(-(airDx * airDx * topXSharp + airDy * airDy * ySharp));
            float leadSplat = exp(-(leadDx * leadDx * topXSharp + leadDy * leadDy * ySharp));
            float kickSplat = exp(-(kickDx * kickDx * bassXSharp + kickDy * kickDy * ySharp));
            float percSplat = exp(-(percDx * percDx * bassXSharp + percDy * percDy * ySharp));
            bass += bassSplat * intensity * (0.12 + 0.11 * styleRumble + 0.04 * styleGroove) * injectGain;
            body += bodySplat * intensity * (0.11 + 0.10 * styleLift + 0.03 * styleSparse) * injectGain;
            vel += normalize(vec2(0.96, -0.05)) * bassSplat * (0.004 + intensity * rumblePush) * upwardForce;
            vel += normalize(vec2(-0.50, -0.62)) * bodySplat * (0.004 + intensity * liftPush) * upwardForce;
            vel += normalize(vec2(0.62, -0.54)) * airSplat * (0.003 + intensity * (0.014 + 0.016 * styleLift + u_airPresence * 0.010)) * upwardForce;
            vel += normalize(vec2(-0.56, -0.44)) * leadSplat * (0.003 + intensity * (0.014 + 0.016 * styleLift + u_leadPresence * 0.010)) * upwardForce;
            vel += normalize(vec2(0.82, 0.12)) * kickSplat * (0.003 + max(onset, u_kickImpact * 0.7) * (0.015 + groovePush)) * upwardForce;
            vel += normalize(vec2(-0.82, 0.05)) * percSplat * (0.003 + max(onset, u_hatTick * 0.6 + u_snareImpact * 0.4) * (0.014 + groovePush * 0.8)) * upwardForce;
            vel += vec2(-bassDx, -0.05 - 0.08 * styleRumble) * bassSplat * onset * (0.038 + 0.016 * styleGroove);
            vel += vec2(-bodyDx, -0.04 - 0.08 * styleLift) * bodySplat * onset * (0.036 + 0.018 * styleLift);
        }
    }

    float silence = 1.0 - smoothstep(0.006, 0.040, audioEnergy);
    float bassDecay = mix(clamp(u_densityDecay + 0.001 + 0.001 * styleRumble - 0.002 * styleSparse, 0.0, 0.999), clamp(u_silenceDensityDecay + 0.02 * styleSparse, 0.0, 0.999), silence);
    float bodyDecay = mix(clamp(u_densityDecay - 0.001 + 0.002 * styleLift - 0.001 * styleGroove, 0.0, 0.999), clamp(u_silenceDensityDecay + 0.03 + 0.02 * styleSparse, 0.0, 0.999), silence);
    bass *= bassDecay;
    body *= bodyDecay;
    vel *= mix(clamp(u_velocityDecay + 0.001 * styleRumble - 0.002 * styleLift, 0.0, 0.999), clamp(u_silenceVelocityDecay + 0.06 * styleSparse, 0.0, 0.999), silence);
    if (silence > 0.98) {
        bass *= 0.75;
        body *= 0.75;
        vel *= 0.15;
    }

    FragColor = encodeState(vel, bass, body);
}
