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
    float low = u_kickImpact * 1.7 + u_bassBody + u_subBody * 0.5;
    float mid = u_harmonicBody + u_leadPresence;
    float high = u_airPresence + u_percussiveFocus;
    float sum = max(0.001, low + mid + high);

    float x = (low * 0.05 + mid * 0.50 + high * 0.95) / sum;
    float y = 0.92 - sat(u_energyLevel * 0.72 + u_tension * 0.34 + high * 0.18) * 0.84;

    return vec2(sat(x), sat(y)) * size;
}

void main() {
    vec2 size = u_resolution;
    vec2 uv = gl_FragCoord.xy / size;
    float h = fract(sin(dot(floor(gl_FragCoord.xy/1.0)+floor(u_time*18.0), vec2(12.9898,78.233))) * 43758.5453);
    float sh = (h - 0.5) * step(1.82, h) * (1.0 + 1.0*u_novelty + 8.0*u_percussiveFocus);
    vec2 guv = clamp((gl_FragCoord.xy + vec2(sh, 0.0)) / size, vec2(0.0), vec2(1.0));
    vec3 prev = texture(iChannel0, guv).rgb;

    int r = byteFrom(prev.r);
    int g = byteFrom(prev.g);
    int b = byteFrom(prev.b);
    int c = (r << 16) | (g << 8) | b;

    float red = float((c << 2) & 255);
    float green = float((c << 4) & 170);
    float blue = float(c  & 255);

    vec2 mouse = audioMouse(size);
    float d = distance(mouse, gl_FragCoord.xy) * 0.4;
    d = max(0.7, d);

    float rad = 20.0-u_kickImpact;
    red += 255.0 / d - rad;
    green += 255.0 / d - rad;
    blue += 255.0 / d - rad;

    vec3 corrupted = clamp(vec3(red, green, blue) / 255.0, 0.0, 1.0);
    vec3 bg = vec3(0.11, 0.114, 0.10+u_kickImpact/10);
    vec3 next = mix(corrupted, max(corrupted, prev * 0.98 + bg), 0.35);
    FragColor = vec4(clamp(next, 0.0, 1.0), 1.0);
}
