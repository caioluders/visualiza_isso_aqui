#version 150
precision highp float;

uniform vec2 u_resolution;
uniform float u_time;
uniform float u_rms;
uniform float u_bandLow;
uniform float u_bandMid;
uniform float u_bandHigh;
uniform float u_onset;
uniform float u_kickImpact;
uniform float u_bassBody;
uniform float u_harmonicBody;
uniform float u_leadPresence;
uniform float u_airPresence;
uniform float u_energyLevel;
uniform float u_brightness;
uniform float u_dropEvent;
uniform float u_beatPulse;
uniform float u_release;
uniform float u_tension;

out vec4 FragColor;

float smooth01(float x){ return smoothstep(0.0, 1.0, clamp(x, 0.0, 1.0)); }

void main(){
	vec2 uv = gl_FragCoord.xy / u_resolution.xy;
	float pulse = clamp(0.45 * max(u_beatPulse, u_kickImpact) + 0.20 * u_dropEvent, 0.0, 1.0);
	float bass = smooth01(0.75 * u_bassBody + 0.25 * u_release);
	float body = smooth01(0.70 * u_harmonicBody + 0.30 * u_leadPresence);
	float air = smooth01(0.70 * u_airPresence + 0.30 * u_brightness);
	float macro = smooth01(0.55 * u_energyLevel + 0.30 * u_tension + 0.15 * u_release);
	vec3 a = vec3(0.1, 0.12, 0.15) + 0.06 * vec3(bass, body, air);
	vec3 b = vec3(0.64 + 0.22*macro + 0.12*pulse,
	              0.55 + 0.24*body + 0.18*u_leadPresence,
	              0.68 + 0.22*air + 0.14*u_brightness);
	vec3 col = mix(a, b, smoothstep(0.0,1.0,uv.y));
	col += vec3(0.08, 0.07, 0.05) * pulse;
	FragColor = vec4(col, 1.0);
}
