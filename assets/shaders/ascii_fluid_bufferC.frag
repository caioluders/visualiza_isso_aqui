#version 150

uniform vec2 u_resolution;
uniform sampler2D iChannel1; // divergence B
uniform sampler2D iChannel2; // pressure C previous iteration

out vec4 FragColor;

void main() {
    vec2 uv = gl_FragCoord.xy / u_resolution.xy;
    vec2 texel = 1.0 / u_resolution.xy;

    float div = texture(iChannel1, uv).x;
    float pL = texture(iChannel2, uv - vec2(texel.x, 0.0)).x;
    float pR = texture(iChannel2, uv + vec2(texel.x, 0.0)).x;
    float pD = texture(iChannel2, uv - vec2(0.0, texel.y)).x;
    float pU = texture(iChannel2, uv + vec2(0.0, texel.y)).x;

    float pressure = (pL + pR + pD + pU - div) * 0.25;
    FragColor = vec4(pressure, 0.0, 0.0, 1.0);
}
