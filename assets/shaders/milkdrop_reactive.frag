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
uniform float u_beat;
uniform float u_beatEnv;
uniform float u_bpm;
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
uniform float u_transientDensity;
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

float clamp01(float x) {
    return clamp(x, 0.0, 1.0);
}

float soft01(float x) {
    x = clamp01(x);
    return x * x * (3.0 - 2.0 * x);
}

mat2 rot(float a) {
    float c = cos(a);
    float s = sin(a);
    return mat2(c, -s, s, c);
}

float hash(vec2 p) {
    return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453123);
}

float noise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);
    return mix(
        mix(hash(i), hash(i + vec2(1.0, 0.0)), u.x),
        mix(hash(i + vec2(0.0, 1.0)), hash(i + vec2(1.0, 1.0)), u.x),
        u.y
    );
}

float fbm(vec2 p) {
    float v = 0.0;
    float a = 0.5;
    for (int i = 0; i < 5; ++i) {
        v += a * noise(p);
        p = p * 2.03 + vec2(13.1, 7.7);
        a *= 0.5;
    }
    return v;
}

vec2 kaleido(vec2 p, float slices) {
    float r = length(p);
    float a = atan(p.y, p.x);
    float sector = 2.0 * PI / max(2.0, slices);
    a = mod(a, sector);
    a = abs(a - 0.5 * sector);
    return vec2(cos(a), sin(a)) * r;
}

float bandAt(float t) {
    t = clamp(t, 0.0, 0.9999);
    float idxf = t * 15.0;
    int i0 = int(floor(idxf));
    int i1 = min(i0 + 1, 15);
    float f = fract(idxf);
    return mix(u_bands[i0], u_bands[i1], f);
}

float onsetAt(float t) {
    t = clamp(t, 0.0, 0.9999);
    float idxf = t * 15.0;
    int i0 = int(floor(idxf));
    int i1 = min(i0 + 1, 15);
    float f = fract(idxf);
    return mix(u_onsets[i0], u_onsets[i1], f);
}

vec3 palette(float t) {
    vec3 a = vec3(0.12, 0.05, 0.18);
    vec3 b = vec3(0.55, 0.45, 0.55);
    vec3 c = vec3(1.0, 1.0, 1.0);
    vec3 d = vec3(0.00, 0.18, 0.35) + vec3(0.15, 0.08, 0.00) * u_brightness;
    return a + b * cos(6.28318 * (c * t + d));
}

float tunnelField(vec2 p, float t, float bass, float body, float air, float pulse) {
    float r = max(length(p), 0.0008);
    float a = atan(p.y, p.x);
    float rings = sin(22.0 * log(r * (1.12 + 0.35 * bass) + 0.075) - t * (2.1 + 2.4 * air + 1.8 * pulse));
    float spokes = sin(a * mix(5.0, 18.0, 0.25 + 0.75 * body) + t * (0.55 + 0.7 * u_tension));
    float swirl = sin((p.x - p.y) * (12.0 + 20.0 * body) - t * (2.3 + 1.7 * bass));
    return 0.42 * rings + 0.33 * spokes + 0.25 * swirl;
}

float spectralRibbon(vec2 p, float t, float offset, float width, float gain) {
    float x = p.x * 0.5 + 0.5;
    float band = bandAt(x);
    float on = onsetAt(x);
    float targetY = offset + gain * (0.15 + 0.5 * band + 0.22 * on) * sin(t + x * (8.0 + 4.0 * u_brightness));
    return exp(-50.0 * abs(p.y - targetY)) * (0.45 + 0.85 * band + 0.25 * on) * width;
}

float radialSpectrum(vec2 p, float pulse, float lift) {
    float a = atan(p.y, p.x) / (2.0 * PI) + 0.5;
    float band = bandAt(a);
    float on = onsetAt(a);
    float r = length(p);
    float edge = 0.20 + lift * (0.20 + 0.34 * band + 0.18 * on + 0.15 * pulse);
    return smoothstep(edge + 0.02, edge - 0.02, abs(r - edge));
}

float starBurst(vec2 p, float t, float air, float novelty) {
    vec2 gp = p * (16.0 + 10.0 * air);
    vec2 cell = floor(gp);
    vec2 local = fract(gp) - 0.5;
    float id = hash(cell);
    float blink = smoothstep(0.86, 1.0, sin(t * (1.4 + 4.0 * id) + id * 19.0) * 0.5 + 0.5);
    float star = smoothstep(0.11, 0.0, length(local));
    return star * blink * (0.15 + 0.85 * air + 0.35 * novelty);
}

void main() {
    vec2 res = u_resolution;
    float side = min(res.x, res.y);
    vec2 uv = (gl_FragCoord.xy - 0.5 * res) / side;
    vec2 screenUv = gl_FragCoord.xy / res;

    float pulse = clamp01(max(max(u_kickImpact, u_beatPulse), 0.65 * u_snareImpact + 0.35 * u_dropEvent));
    float bass = clamp01(0.55 * u_subBody + 0.45 * u_bassBody);
    float body = clamp01(0.62 * u_harmonicBody + 0.38 * u_leadPresence);
    float lead = clamp01(u_leadPresence);
    float air = clamp01(0.65 * u_airPresence + 0.35 * u_brightness);
    float perc = clamp01(0.6 * u_percussiveFocus + 0.4 * u_hatTick);
    float energy = clamp01(u_energyLevel);
    float tension = clamp01(u_tension);
    float novelty = clamp01(0.65 * u_novelty + 0.35 * sqrt(max(u_flux, 0.0)));
    float release = clamp01(u_release);
    float beat = soft01(max(u_beatEnv, u_beatPulse));

    float bpmScale = clamp(u_bpm / 140.0, 0.6, 1.5);
    float t = u_time * (0.65 + 0.35 * bpmScale);

    vec2 p = uv;
    p *= rot(0.12 * sin(t * 0.35) + 0.20 * tension + 0.10 * pulse);
    p += 0.03 * vec2(
        sin(p.y * 10.0 + t * 2.2),
        cos(p.x * 11.0 - t * 1.8)
    ) * (0.35 + 0.55 * air + 0.25 * novelty);
    p = kaleido(p, mix(5.0, 14.0, clamp01(0.45 * body + 0.35 * lead + 0.20 * tension)));

    float zoom = 1.0 + 0.22 * bass - 0.16 * beat + 0.08 * release;
    vec2 q = p * zoom;

    float tunnel = tunnelField(q, t, bass, body, air, pulse);
    float tunnelMask = smoothstep(-0.7, 0.9, tunnel);

    float ring = radialSpectrum(q, pulse, 0.50 + 0.22 * body + 0.08 * tension);
    float ribbonA = spectralRibbon(q * rot(0.35 + 0.2 * beat), t * 1.15, -0.22 + 0.12 * sin(t * 0.5), 1.0, 0.42 + 0.25 * lead);
    float ribbonB = spectralRibbon(q.yx * vec2(1.0, -1.0) * rot(-0.25), -t * 0.95, 0.18 + 0.10 * cos(t * 0.37), 1.0, 0.32 + 0.20 * air);
    float stars = starBurst(q, t, air, novelty);

    float plasma = fbm(q * (4.0 + 5.0 * body) + vec2(t * 0.22, -t * 0.18));
    plasma += 0.6 * fbm(q * rot(0.45) * (7.0 + 6.0 * air) - vec2(t * 0.14, t * 0.19));
    plasma *= 0.625;

    vec3 base = palette(
        0.18 * t +
        0.18 * plasma +
        0.12 * body +
        0.10 * lead +
        0.08 * air
    );

    vec3 tunnelColor = mix(
        vec3(0.02, 0.03, 0.06),
        base,
        tunnelMask
    );

    vec3 bassWash = vec3(0.08, 0.20, 0.75) * bass * (0.4 + 0.6 * smoothstep(0.1, 1.0, 1.0 - length(q)));
    vec3 bodyWash = vec3(0.00, 0.95, 0.72) * body * (0.25 + 0.75 * tunnelMask);
    vec3 leadWash = vec3(1.00, 0.32, 0.78) * lead * (0.25 + 1.15 * ring);
    vec3 airWash = vec3(0.95, 0.92, 1.00) * air * (0.14 + 0.75 * stars);
    vec3 percWash = vec3(1.00, 0.75, 0.15) * perc * (0.20 + 0.90 * (ribbonA + ribbonB));

    vec3 col = tunnelColor;
    col += bassWash;
    col += bodyWash;
    col += leadWash;
    col += airWash;
    col += percWash;

    float flash = 0.22 * pulse + 0.15 * u_dropEvent + 0.10 * u_sectionChange + 0.08 * u_onset;
    col += vec3(1.00, 0.97, 0.80) * flash;

    float centerGlow = exp(-3.6 * length(q)) * (0.22 + 0.55 * energy + 0.35 * beat);
    col += mix(vec3(0.0, 0.5, 1.0), vec3(1.0, 0.0, 0.75), 0.45 + 0.35 * lead) * centerGlow;

    float vignette = smoothstep(1.22, 0.18, length(uv));
    col *= mix(0.58, 1.15, vignette);
    col *= 0.82 + 0.38 * energy + 0.16 * clamp01(u_rms * 2.0);

    float scan = 0.96 + 0.04 * sin(screenUv.y * res.y * 0.55 + t * 22.0);
    col *= scan;

    col = pow(max(col, vec3(0.0)), vec3(0.92));
    FragColor = vec4(col, 1.0);
}
