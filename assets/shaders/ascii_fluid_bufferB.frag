#version 150

uniform vec2 u_resolution;
uniform sampler2D iChannel0; // A current

out vec4 FragColor;

vec2 decodeVel(vec4 s) { return s.xy * 2.0 - 1.0; }

void main() {
    vec2 uv = gl_FragCoord.xy / u_resolution.xy;
    vec2 texel = 1.0 / u_resolution.xy;

    float vxR = decodeVel(texture(iChannel0, uv + vec2(texel.x, 0.0))).x;
    float vxL = decodeVel(texture(iChannel0, uv - vec2(texel.x, 0.0))).x;
    float vyU = decodeVel(texture(iChannel0, uv + vec2(0.0, texel.y))).y;
    float vyD = decodeVel(texture(iChannel0, uv - vec2(0.0, texel.y))).y;

    float div = 0.5 * ((vxR - vxL) + (vyU - vyD));
    FragColor = vec4(div, 0.0, 0.0, 1.0);
}
