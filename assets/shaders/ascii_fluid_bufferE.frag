#version 150

uniform vec2 u_resolution;
uniform float u_time;
uniform float u_snareImpact;
uniform float u_hatTick;
uniform float u_kickImpact;
uniform float u_dropEvent;
uniform float u_harmonicBody;
uniform float u_leadPresence;
uniform float u_airPresence;
uniform float u_transientDensity;
uniform float u_energyLevel;
uniform float u_tension;
uniform float u_bands[16];
uniform float u_onsets[16];
uniform float u_injectionRow;
uniform float u_injectionGain;
uniform float u_densityDecay;
uniform float u_silenceDensityDecay;

uniform sampler2D iChannel3; // projected state D
uniform sampler2D iChannel4; // previous E

out vec4 FragColor;

float sat(float x) { return clamp(x, 0.0, 1.0); }
vec2 decodeVel(vec4 s) { return s.xy * 2.0 - 1.0; }
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

    vec2 vel = decodeVel(texture(iChannel3, uv));
    vec4 adv = texture(iChannel4, clamp(uv - vel * 0.018, texel * 0.5, 1.0 - texel * 0.5));
    float lead = adv.x;
    float air = adv.y;
    float kick = adv.z;
    float perc = adv.w;

    float lapLead =
        texture(iChannel4, uv + vec2(texel.x, 0.0)).x +
        texture(iChannel4, uv - vec2(texel.x, 0.0)).x +
        texture(iChannel4, uv + vec2(0.0, texel.y)).x +
        texture(iChannel4, uv - vec2(0.0, texel.y)).x - 4.0 * lead;
    float lapAir =
        texture(iChannel4, uv + vec2(texel.x, 0.0)).y +
        texture(iChannel4, uv - vec2(texel.x, 0.0)).y +
        texture(iChannel4, uv + vec2(0.0, texel.y)).y +
        texture(iChannel4, uv - vec2(0.0, texel.y)).y - 4.0 * air;
    float lapKick =
        texture(iChannel4, uv + vec2(texel.x, 0.0)).z +
        texture(iChannel4, uv - vec2(texel.x, 0.0)).z +
        texture(iChannel4, uv + vec2(0.0, texel.y)).z +
        texture(iChannel4, uv - vec2(0.0, texel.y)).z - 4.0 * kick;
    float lapPerc =
        texture(iChannel4, uv + vec2(texel.x, 0.0)).w +
        texture(iChannel4, uv - vec2(texel.x, 0.0)).w +
        texture(iChannel4, uv + vec2(0.0, texel.y)).w +
        texture(iChannel4, uv - vec2(0.0, texel.y)).w - 4.0 * perc;
    lead += lapLead * 0.012;
    air += lapAir * 0.009;
    kick += lapKick * 0.010;
    perc += lapPerc * 0.011;

    float audioEnergy = sat(0.45 * u_energyLevel + 0.20 * u_airPresence + 0.15 * u_transientDensity + 0.20 * u_kickImpact);
    float injectGain = max(0.0, u_injectionGain);
    float styleGroove = sat(0.36 * u_kickImpact + 0.28 * u_hatTick + 0.20 * u_snareImpact + 0.16 * (1.0 - u_tension));
    float styleRumble = sat(0.42 * u_kickImpact + 0.22 * u_energyLevel + 0.18 * u_transientDensity + 0.18 * (1.0 - u_airPresence));
    float styleLift = sat(0.28 * u_harmonicBody + 0.24 * u_leadPresence + 0.24 * u_airPresence + 0.24 * u_tension);
    float styleSparse = sat(0.34 * (1.0 - u_kickImpact) + 0.28 * (1.0 - u_transientDensity) + 0.20 * u_airPresence + 0.18 * (1.0 - u_energyLevel));
    float airRow = mix(0.82, 0.92, styleLift);
    float leadRow = mix(0.62, 0.76, styleLift);
    float kickRow = mix(0.12, 0.08, sat(styleGroove + 0.4 * styleRumble));
    float percRow = mix(0.24, 0.16, styleGroove);
    float topXSharp = mix(3000.0, 2200.0, styleLift + 0.2 * styleSparse);
    float bottomXSharp = mix(3200.0, 2500.0, styleGroove);
    float ySharp = mix(1500.0, 880.0, styleLift + 0.25 * styleSparse);

    for (int i = 0; i < 128; ++i) {
        float xNorm = (float(i) + 0.5) / 128.0;
        float airX = mix(0.04, 0.24, xNorm);
        float leadX = mix(0.56, 0.76, xNorm);
        float kickX = mix(0.06, 0.20, xNorm);
        float percX = mix(0.84, 0.96, xNorm);
        float band = sat(bandAt(xNorm));
        float onset = sat(onsetAt(xNorm));
        float intensity = max(band, onset * 0.9);
        if (intensity > 0.08) {
            float airDx = uv.x - airX;
            float airDy = uv.y - airRow;
            float leadDx = uv.x - leadX;
            float leadDy = uv.y - leadRow;
            float kickDx = uv.x - kickX;
            float kickDy = uv.y - kickRow;
            float percDx = uv.x - percX;
            float percDy = uv.y - percRow;
            float airSplat = exp(-(airDx * airDx * topXSharp + airDy * airDy * ySharp));
            float leadSplat = exp(-(leadDx * leadDx * topXSharp + leadDy * leadDy * ySharp));
            float kickSplat = exp(-(kickDx * kickDx * bottomXSharp + kickDy * kickDy * ySharp));
            float percSplat = exp(-(percDx * percDx * bottomXSharp + percDy * percDy * ySharp));
            lead += leadSplat * intensity * (0.14 + 0.10 * styleLift + 0.03 * styleSparse) * injectGain;
            air += airSplat * intensity * (0.15 + 0.11 * styleLift + 0.03 * styleSparse) * injectGain;
            kick += kickSplat * max(onset, u_kickImpact * 0.7) * (0.18 + 0.10 * styleRumble + 0.04 * styleGroove) * injectGain;
            perc += percSplat * max(onset, u_hatTick * 0.6 + u_snareImpact * 0.4) * (0.16 + 0.08 * styleGroove + 0.03 * styleLift) * injectGain;
        }
    }

    float silence = 1.0 - smoothstep(0.006, 0.040, audioEnergy);
    float leadDecay = mix(clamp(u_densityDecay - 0.002 + 0.003 * styleLift - 0.001 * styleGroove, 0.0, 0.999), clamp(u_silenceDensityDecay + 0.03 * styleSparse, 0.0, 0.999), silence);
    float airDecay = mix(clamp(u_densityDecay - 0.001 + 0.003 * styleLift, 0.0, 0.999), clamp(u_silenceDensityDecay + 0.02 * styleSparse, 0.0, 0.999), silence);
    float kickDecay = mix(clamp(u_densityDecay - 0.010 + 0.003 * styleRumble, 0.0, 0.999), clamp(u_silenceDensityDecay + 0.05, 0.0, 0.999), silence);
    float percDecay = mix(clamp(u_densityDecay - 0.008 + 0.002 * styleGroove, 0.0, 0.999), clamp(u_silenceDensityDecay + 0.03, 0.0, 0.999), silence);
    lead *= leadDecay;
    air *= airDecay;
    kick *= kickDecay;
    perc *= percDecay;
    if (silence > 0.98) {
        lead *= 0.72;
        air *= 0.72;
        kick *= 0.58;
        perc *= 0.60;
    }

    FragColor = vec4(
        clamp(lead, 0.0, 1.0),
        clamp(air, 0.0, 1.0),
        clamp(kick, 0.0, 1.0),
        clamp(perc, 0.0, 1.0)
    );
}
