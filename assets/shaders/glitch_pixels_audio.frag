#version 150
precision highp float;

uniform vec2 u_resolution;
uniform sampler2D iChannel0; // current glitch feedback buffer

out vec4 FragColor;

void main() {
    vec2 uv = gl_FragCoord.xy / u_resolution.xy;
    vec3 color = texture(iChannel0, uv).rgb;
    FragColor = vec4(color, 1.0);
}
