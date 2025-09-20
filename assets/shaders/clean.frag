#version 150
precision highp float;

uniform vec2 u_resolution;
uniform float u_time;
uniform float u_rms;
uniform float u_bandLow;
uniform float u_bandMid;
uniform float u_bandHigh;
uniform float u_onset;

out vec4 FragColor;

void main(){
	vec2 uv = gl_FragCoord.xy / u_resolution.xy;
	vec3 a = vec3(0.1, 0.12, 0.15);
	vec3 b = vec3(0.8 + 0.3*u_rms, 0.7 + 0.2*u_bandMid, 0.9 + 0.2*u_bandHigh);
	vec3 col = mix(a, b, smoothstep(0.0,1.0,uv.y));
	col += 0.2*u_onset;
	FragColor = vec4(col, 1.0);
}


