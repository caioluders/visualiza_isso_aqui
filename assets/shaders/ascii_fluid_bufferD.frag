#version 150

uniform vec2 u_resolution;
uniform sampler2D iChannel0; // A current
uniform sampler2D iChannel2; // C pressure

out vec4 FragColor;

vec2 decodeVel(vec4 s) { return s.xy * 2.0 - 1.0; }
vec4 encodeState(vec2 v, float bass, float body) { return vec4(clamp(v * 0.5 + 0.5, 0.0, 1.0), clamp(bass, 0.0, 1.0), clamp(body, 0.0, 1.0)); }

void main() {
    vec2 uv = gl_FragCoord.xy / u_resolution.xy;
    vec2 texel = 1.0 / u_resolution.xy;

    vec4 state = texture(iChannel0, uv);
    vec2 vel = decodeVel(state);
    float bass = state.z;
    float body = state.w;

    float pR = texture(iChannel2, uv + vec2(texel.x, 0.0)).x;
    float pL = texture(iChannel2, uv - vec2(texel.x, 0.0)).x;
    float pU = texture(iChannel2, uv + vec2(0.0, texel.y)).x;
    float pD = texture(iChannel2, uv - vec2(0.0, texel.y)).x;

    vec2 grad = vec2(pR - pL, pU - pD) * 0.5;
    vel -= grad;

    FragColor = encodeState(vel, bass, body);
}
