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
uniform float u_airPresence;
uniform float u_energyLevel;
uniform float u_leadPresence;
uniform float u_dropEvent;
uniform float u_beatPulse;
uniform float u_brightness;
uniform float u_tension;
uniform float u_release;

// User-adjustable params
uniform float u_waveScale;   // scales spatial frequency
uniform float u_waveMix;     // mix between bands influence 0..1
uniform float u_waveGain;    // overall brightness

out vec4 FragColor;

float smooth01(float x){ return smoothstep(0.0, 1.0, clamp(x, 0.0, 1.0)); }

void main(){
	vec2 uv = gl_FragCoord.xy / u_resolution.xy;
	uv = uv*2.0 - 1.0;
	uv.x *= u_resolution.x/u_resolution.y;

    float pulse = clamp(0.45 * max(u_beatPulse, u_kickImpact) + 0.18 * u_dropEvent, 0.0, 1.0);
    float macro = smooth01(0.55 * u_energyLevel + 0.30 * u_tension + 0.15 * u_release);
    float t = u_time * (0.32 + 0.26*u_bassBody + 0.18*pulse + 0.12*macro);
    float s = (u_waveScale == 0.0 ? 1.0 : u_waveScale);
    float w1 = sin(uv.x*(7.0*s) + t*1.2);
    float w2 = sin(uv.x*(12.0*s) + t*1.8);
    float w3 = sin(uv.x*(20.0*s) + t*2.4);
	float y = (w1*0.6 + w2*0.3 + w3*0.2);
	float band = mix(0.18*u_bandMid + 0.58*u_harmonicBody + 0.10*u_release,
                     0.22*u_harmonicBody + 0.25*u_leadPresence + 0.20*u_airPresence + 0.18*u_brightness + 0.15*macro,
                     clamp(u_waveMix, 0.0, 1.0));
	float v = smoothstep(y-0.05-0.15*band, y+0.05+0.15*band, uv.y);
	vec3 col = mix(vec3(0.03,0.04,0.06), vec3(0.18 + 0.10*u_bassBody, 0.28 + 0.14*u_harmonicBody, 0.72 + 0.20*u_airPresence), v);
    col += vec3(0.08, 0.06, 0.04) * pulse * (u_waveGain == 0.0 ? 1.0 : u_waveGain);
    col *= 0.92 + 0.12 * macro;
	FragColor = vec4(col,1.0);
}
