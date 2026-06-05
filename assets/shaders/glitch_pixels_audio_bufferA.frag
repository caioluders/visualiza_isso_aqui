#version 150
precision highp float;

uniform vec2 u_resolution;
uniform sampler2D iChannel0; // previous A

uniform float u_kickImpact;
uniform float u_subBody;
uniform float u_bassBody;
uniform float u_harmonicBody;
uniform float u_leadPresence;
uniform float u_airPresence;
uniform float u_percussiveFocus;
uniform float u_energyLevel;
uniform float u_tension;
uniform float u_time;
uniform float u_novelty;

out vec4 FragColor;

float sat(float x) { return clamp(x, 0.0, 1.0); }

int byteFrom(float x) {
    return int(clamp(floor(x * 255.0 + 0.5), 0.0, 255.0));
}

vec2 audioMouse(vec2 size) {
    // Normalize each spectral group to a comparable 0..1 range so the
    // naturally louder low end does not pin the position to one side.
    float low  = sat((u_kickImpact * 1.7 + u_bassBody + u_subBody * 0.5) / 3.2);
    float mid  = sat((u_harmonicBody + u_leadPresence) / 2.0);
    float high = sat((u_airPresence + u_percussiveFocus) / 2.0);
    float sum = max(0.001, low + mid + high);

    // Spectral centroid across the full width, then expand around the center
    // so the focal point actually sweeps the whole horizontal range.
    float x = (low * 0.08 + mid * 0.50 + high * 0.92) / sum;
    x = 0.5 + (x - 0.5) * 2.0;

    // Vertical position driven by overall drive, with extra sway from the
    // low/high spectral tilt for more movement.
    float drive = sat(u_energyLevel * 0.7 + u_tension * 0.3 + high * 0.2);
    float y = 0.85 - drive * 0.7 + (high - low) * 0.18;

    return clamp(vec2(x, y), vec2(0.02), vec2(0.98)) * size;
}

void main() {
    vec2 size = u_resolution;
    vec2 uv = gl_FragCoord.xy / size;
    float h = fract(sin(dot(floor(gl_FragCoord.xy/1.0)+floor(u_time*1.0), vec2(10.9898,78.233))) * 43758.5453);
    float sh = (h - 0.5) * step(10.82, h) * (1.0 + 1.0*u_novelty + 8.0*u_percussiveFocus);
    vec2 guv = clamp((gl_FragCoord.xy + vec2(sh, 0.0)) / size, vec2(0.0), vec2(1.0));
    vec3 prev = texture(iChannel0, guv).rgb;

    int r = byteFrom(prev.r);
    int g = byteFrom(prev.g);
    int b = byteFrom(prev.b);
    int c = (r << 16) | (g << 7) | b;

    float red = float((c << 4) & 255);
    float green = float((c>> 10) & 170);
    float blue = float(c & 255);

    vec2 mouse = audioMouse(size);
    float d = distance(mouse, gl_FragCoord.xy) * 0.4;
    d = max(0.7, d);

    float rad = 15.0-u_kickImpact;
    float glow = 255.0 / d - rad;
    // The kick drives the color: neutral when quiet, punching warm/red on
    // impact so each kick visibly shifts the palette.
    vec3 kickTint = mix(vec3(1.0), vec3(2.0, 0.7, 0.3), sat(u_kickImpact));
    red   += glow * kickTint.r;
    green += glow * kickTint.g;
    blue  += glow * kickTint.b;

    vec3 corrupted = clamp(vec3(red, green, blue) / 255.0, 0.0, 1.0);
    vec3 bg = vec3(0.11, 0.114, 0.10+u_kickImpact/10);
    vec3 next = mix(corrupted, max(corrupted, prev * 0.99 + bg), 0.30);
    // Flush the whole frame toward a warm tint with the kick.
    next = mix(next, next * vec3(1.6, 0.85, 0.6), sat(u_kickImpact) * 0.6);
    FragColor = vec4(clamp(next, 0.0, 1.0), 1.0);
}