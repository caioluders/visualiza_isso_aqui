#version 150
precision highp float;

uniform vec2 u_resolution;
uniform float u_time;
uniform float u_rms;
uniform float u_bandLow;
uniform float u_bandMid;
uniform float u_bandHigh;
uniform float u_onset;

// User-adjustable params
uniform float u_waveScale;   // scales spatial frequency
uniform float u_waveMix;     // mix between bands influence 0..1
uniform float u_waveGain;    // overall brightness

out vec4 FragColor;

void main(){
	vec2 uv = gl_FragCoord.xy / u_resolution.xy;
	uv = uv*2.0 - 1.0;
	uv.x *= u_resolution.x/u_resolution.y;

    float t = u_time * (0.4 + 0.8*u_bandLow);
    float s = (u_waveScale == 0.0 ? 1.0 : u_waveScale);
    float w1 = sin(uv.x*(7.0*s) + t*1.2);
    float w2 = sin(uv.x*(12.0*s) + t*1.8);
    float w3 = sin(uv.x*(20.0*s) + t*2.4);
	float y = (w1*0.6 + w2*0.3 + w3*0.2);
    float band = mix(u_bandMid, (u_bandMid*0.6 + u_bandHigh*0.4), clamp(u_waveMix, 0.0, 1.0));
	float v = smoothstep(y-0.05-0.15*band, y+0.05+0.15*band, uv.y);
	vec3 col = mix(vec3(0.03,0.04,0.06), vec3(0.2,0.35,0.8), v);
    col += (0.2*u_onset + 0.15*u_rms) * (u_waveGain == 0.0 ? 1.0 : u_waveGain);
	FragColor = vec4(col,1.0);
}


