#version 150
precision highp float;

uniform vec2 u_resolution;
uniform float u_time;
uniform float u_beatPulse;    // 0..1 beat-synced pulse (kick + beat envelope)
uniform float u_bassBody;     // 0..1 bass presence
uniform float u_airPresence;  // 0..1 high/air content
uniform float u_energyLevel;  // 0..1 overall energy
out vec4 FragColor;

// Thickened gyroid surface: sin(x)cos(y)+sin(y)cos(z)+sin(z)cos(x) = 0.
float map(vec3 p) {
    p *= 4.0;
    float thickness = 0.08 * u_beatPulse + 0.12 * u_bassBody; // walls swell on the pulse
    return abs(dot(sin(p.yzx+thickness)+cos(p.yzx+3), cos(p.yzx))) - thickness;
}

vec3 normal(vec3 p) {
    vec2 e = vec2(0.001, 0.0);
    return normalize(vec3(
        map(p + e.xyy) - map(p - e.xyy),
        map(p + e.yxy) - map(p - e.yxy),
        map(p + e.yyx) - map(p - e.yyx)));
}

void main() {
    vec2 uv = (2.0 * gl_FragCoord.xy - u_resolution) / u_resolution.y;
    vec3 ro = vec3(0.0, 0.0, u_time * 3);       // fly forward
    vec3 rd = normalize(vec3(uv, 1.2 - 0.3));  // pulse punches the camera in

    float t = 0.0;
    for (int i = 0; i < 500; ++i) {
        float d = map(ro + rd * t);
        if (d < 0.01 || t > 30.0) break;
        t += d * 0.1 ;                              // 0.7: gyroid SDF safety factor
    }

    vec3 p = ro + rd * t;
    float diff = clamp(dot(normal(p), -rd), 0.0, 0.4);   // headlight
    // Black -> purple: deep violet in shadow, lilac toward the light.
    vec3 base = mix(vec3(1.00, 1.0, 1.0), vec3(0.70, 0.05, 0.0), 0.5 * sin(t * 0.1 + 0.10 * u_airPresence));
    vec3 col = base * diff ;             // shade (black where unlit)
    col *= 1 + 1.8 * u_energyLevel;    // energy drives brightness

    float kick = u_beatPulse * u_beatPulse;            // sharpen the pulse
    col += col * 1.0 * kick;                           // bright flash on the pulse

    FragColor = vec4(pow(col, vec3(0.2548)), 1.0);
}
