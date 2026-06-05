#version 150

uniform vec2 u_resolution;
uniform float u_time;
uniform float u_bassBody;
uniform float u_harmonicBody;
uniform float u_leadPresence;
uniform float u_airPresence;
uniform float u_brightness;
uniform float u_energyLevel;
uniform float u_tension;
uniform float u_release;
uniform float u_percussiveFocus;
uniform float u_dropEvent;

uniform sampler2D iChannel3; // projected state D: velocity + bass/body
uniform sampler2D iChannel4; // projected state E: lead/air/kick/perc

out vec4 FragColor;

const int kGlyphWidth = 8;
const int kGlyphHeight = 8;
const int kGlyphCount = 70;
const int kDisplayGlyphCount = 24;
const int kGlyphRows[kGlyphCount * kGlyphHeight] = int[](
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 16, 0, 0, 0, 16, 16, 16, 0, 0, 0, 0, 0, 16, 0, 0, 0, 0, 0, 0, 0,
    16, 24, 24, 36, 36, 0, 0, 0, 0, 0, 0, 16, 48, 0, 0, 0, 0, 16, 0, 0, 0, 0, 16, 0, 16, 0, 0, 0, 16, 48, 32, 0,
    0, 0, 58, 44, 0, 0, 0, 0, 0, 0, 56, 0, 0, 0, 0, 0, 0, 0, 0, 124, 0, 0, 0, 0, 0, 16, 16, 124, 16, 16, 0, 0,
    0, 60, 0, 60, 0, 0, 0, 0, 0, 0, 12, 48, 48, 28, 4, 0, 0, 0, 48, 12, 12, 56, 32, 0, 16, 0, 16, 16, 16, 16, 16, 16,
    16, 16, 16, 16, 16, 16, 0, 16, 16, 16, 16, 16, 16, 16, 16, 24, 28, 36, 36, 4, 12, 8, 0, 8, 8, 24, 16, 16, 16, 32, 32, 32,
    16, 16, 16, 16, 16, 16, 16, 16, 8, 24, 16, 16, 16, 16, 16, 24, 16, 24, 8, 8, 8, 8, 8, 24, 16, 16, 16, 16, 16, 16, 16, 16,
    8, 8, 8, 8, 8, 8, 8, 8, 16, 16, 16, 48, 48, 16, 16, 16, 16, 16, 16, 24, 24, 16, 16, 16, 24, 56, 8, 8, 8, 8, 8, 8,
    16, 16, 56, 16, 16, 16, 16, 24, 24, 16, 56, 16, 16, 16, 16, 16, 0, 16, 16, 16, 16, 16, 16, 16, 0, 28, 24, 16, 16, 16, 16, 0,
    0, 54, 20, 24, 24, 20, 38, 0, 0, 124, 100, 68, 68, 68, 68, 0, 0, 68, 68, 68, 68, 76, 124, 0, 0, 68, 108, 40,
    40, 56, 16, 0, 0, 56, 108, 64, 64, 108, 56, 0, 0, 60, 12, 8, 16, 48, 60, 0, 196, 72, 56, 48, 48, 56, 76, 196,
    68, 108, 40, 56, 16, 16, 16, 16, 68, 68, 68, 68, 68, 68, 108, 56, 4, 4, 4, 4, 4, 36, 36, 60, 120, 204, 132, 128,
    128, 132, 204, 120, 32, 32, 32, 32, 32, 32, 32, 62, 196, 130, 130, 130, 130, 198, 126, 2, 56, 108, 68, 68, 68, 68, 108, 56,
    124, 198, 130, 130, 130, 130, 198, 124, 124, 12, 8, 24, 48, 32, 96, 124, 0, 254, 146, 146, 146, 146, 146, 0, 0, 155, 219, 218, 126, 110,
    100, 0, 60, 108, 68, 68, 108, 60, 4, 4, 60, 54, 34, 34, 54, 60, 32, 32, 4, 4, 60, 108, 68, 68, 108, 60, 32, 32,
    60, 54, 34, 34, 54, 60, 32, 32, 36, 44, 56, 56, 44, 36, 64, 64, 124, 100, 68, 68, 68, 68, 0, 56, 72, 56, 104,
    72, 120, 0, 0, 56, 108, 68, 68, 108, 56, 0, 0, 16, 124, 56, 40, 0, 0, 0, 20, 20, 28, 60, 24, 60, 40, 56,
    198, 198, 198, 238, 170, 170, 186, 146, 153, 153, 185, 185, 175, 230, 230, 102, 56, 64, 68, 62, 100, 68, 68, 60, 120, 68, 68,
    60, 108, 68, 68, 56, 228, 172, 168, 240, 30, 42, 106, 78, 62, 34, 34, 38, 62, 34, 34, 60, 98, 223, 181, 165, 175, 254,
    70, 60, 56, 124, 84, 112, 60, 20, 84, 124, 124, 70, 66, 66, 66, 66, 70, 124, 66, 66, 66, 66, 126, 66, 66, 66
);
const int kDisplayGlyphMap[kDisplayGlyphCount] = int[](
    0, 1, 3, 6, 8, 9, 11, 12,
    15, 18, 20, 28, 32, 33, 36, 42,
    45, 47, 56, 58, 59, 60, 63, 65
);

vec2 decodeVel(vec4 s) { return s.xy * 2.0 - 1.0; }

int glyphRowMask(int glyph, int row) {
    glyph = clamp(glyph, 0, kGlyphCount - 1);
    row = clamp(row, 0, kGlyphHeight - 1);
    return kGlyphRows[glyph * kGlyphHeight + row];
}

float asciiRampGlyph(float level, vec2 local) {
    vec2 padded = (local - vec2(0.06, 0.06)) / vec2(0.88, 0.88);
    if (padded.x < 0.0 || padded.x >= 1.0 || padded.y < 0.0 || padded.y >= 1.0) return 0.0;

    int levelIndex = int(clamp(floor(level * float(kDisplayGlyphCount)), 0.0, float(kDisplayGlyphCount - 1)));
    int glyph = kDisplayGlyphMap[levelIndex];
    ivec2 cell = ivec2(floor(vec2(padded.x, 1.0 - padded.y) * vec2(float(kGlyphWidth), float(kGlyphHeight))));
    cell.x = clamp(cell.x, 0, kGlyphWidth - 1);
    cell.y = clamp(cell.y, 0, kGlyphHeight - 1);

    int mask = glyphRowMask(glyph, cell.y);
    int bit = 1 << ((kGlyphWidth - 1) - cell.x);
    return (mask & bit) != 0 ? 1.0 : 0.0;
}

void main() {
    vec2 uv = gl_FragCoord.xy / u_resolution.xy;

    float cellSize = 1;
    vec2 cellCoord = gl_FragCoord.xy / cellSize;
    vec2 cell = floor(cellCoord);
    vec2 local = fract(cellCoord);
    vec2 samplePx = (cell + 0.5) * cellSize;
    vec2 sampleUv = clamp(samplePx / u_resolution.xy, vec2(0.0), vec2(1.0));

    vec4 lowState = texture(iChannel3, sampleUv);
    vec4 highState = texture(iChannel4, sampleUv);
    vec2 vel = decodeVel(lowState);

    float bass = clamp(lowState.z, 0.0, 1.0);
    float body = clamp(lowState.w, 0.0, 1.0);
    float lead = clamp(highState.x, 0.0, 1.0);
    float air = clamp(highState.y, 0.0, 1.0);
    float kick = clamp(highState.z, 0.0, 1.0);
    float perc = clamp(highState.w, 0.0, 1.0);
    float speed = length(vel);
    float styleGroove = clamp(0.42 * u_bassBody + 0.24 * perc + 0.18 * kick + 0.16 * (1.0 - u_tension), 0.0, 1.0);
    float styleRumble = clamp(0.50 * u_bassBody + 0.20 * kick + 0.15 * u_energyLevel + 0.15 * (1.0 - u_airPresence), 0.0, 1.0);
    float styleLift = clamp(0.26 * u_harmonicBody + 0.26 * u_leadPresence + 0.24 * u_airPresence + 0.24 * u_tension, 0.0, 1.0);
    float styleSparse = clamp(0.34 * u_release + 0.22 * (1.0 - u_energyLevel) + 0.22 * u_airPresence + 0.22 * (1.0 - u_percussiveFocus), 0.0, 1.0);

    vec4 fluidsA = vec4(kick, bass, body, lead);
    vec2 fluidsB = vec2(air, perc);
    float density = kick + bass + body + lead + air + perc;
    float weightedDensity = kick * 1.20 + bass * 1.08 + body * 1.00 + lead * 1.02 + air * 0.96 + perc * 1.05;
    float level = clamp(
        weightedDensity * mix(0.86, 1.02, styleRumble + 0.3 * styleGroove)
        + speed * (0.06 + 0.04 * styleLift)
        + density * 0.05 * styleSparse,
        0.0,
        0.999
    );
    float glyph = asciiRampGlyph(level, local);

    vec3 palette[6];
    // Acid theme, spread across brightness/saturation so the six roles stay
    // visually distinct (not one green->yellow ramp).
    palette[0] = vec3(1.00, 1.00, 0.10); // kick - bright neon yellow
    palette[1] = vec3(0.08, 0.40, 0.00); // bass - dark forest/acid
    palette[2] = vec3(0.35, 0.95, 0.00); // body - mid acid green
    palette[3] = vec3(0.65, 1.00, 0.00); // lead - lime
    palette[4] = vec3(1.00, 1.00, 0.55); // air  - pale yellow-white
    palette[5] = vec3(0.00, 1.00, 0.35); // perc - toxic green-cyan

    float fluidVals[6];
    fluidVals[0] = kick;
    fluidVals[1] = bass;
    fluidVals[2] = body;
    fluidVals[3] = lead;
    fluidVals[4] = air;
    fluidVals[5] = perc;

    // Blend ALL six role colors weighted by their amounts, so every active
    // fluid tints its region instead of only the single dominant role showing.
    // Squaring the weights keeps the strongest role punchy while letting the
    // others bleed through.
    vec3 fluidColor = vec3(0.0);
    float wsum = 1e-4;
    for (int i = 0; i < 6; ++i) {
        float w = fluidVals[i] * fluidVals[i];
        fluidColor += palette[i] * w;
        wsum += w;
    }
    fluidColor /= wsum;
    fluidColor *= 1.92 + 0.20 * min(1.0, density);

    float macroLight = clamp(u_brightness * 0.16 + u_energyLevel * 0.08 + u_tension * 0.05 + 0.06 * styleLift, 0.0, 0.28);
    fluidColor = mix(fluidColor, vec3(1.0, 1.0, 0.1), macroLight); // neon-yellow highlight
    fluidColor = mix(fluidColor, fluidColor.zyx, 0.6 * styleSparse);

    vec3 color = fluidColor * glyph;
    float halo = smoothstep(0.10, 1.0, density) * (0.07 + 1.05 * styleLift + 0.03 * styleSparse);
    color += fluidColor * halo * glyph;
    color += vec3(0.00, 1.00, 0.30) * glyph * u_dropEvent * (0.05 + 0.04 * styleGroove);

    color += vec3(0.30, 1.00, 0.10) * bass * 0.12 * styleRumble;
    color += vec3(0.00, 1.00, 0.10) * lead * 0.10 * styleLift;
    color += vec3(0.10, 0.00, 0.10) * air * 0.10 * styleSparse;

    FragColor = vec4(color, 1.0);
}
