#version 150
precision highp float;

uniform vec2 u_resolution;
uniform float u_time;
uniform float u_rms;
uniform float u_bandLow;
uniform float u_bandMid;
uniform float u_bandHigh;
uniform float u_onset;

// User-adjustable params (auto-exposed in UI)
uniform float u_freqScale;     // scales stripe frequency
uniform float u_angleOffset1;  // add to first layer angle
uniform float u_angleOffset2;  // add to second layer angle
uniform float u_mix;           // 0..1, product vs additive moiré
uniform float u_brightness;    // overall brightness
uniform float u_contrast;      // contrast around 0.5

out vec4 FragColor;

mat2 rot(float a){ float c = cos(a), s = sin(a); return mat2(c,-s,s,c); }

float stripe(vec2 p, float freq){ return 0.5 + 0.5*cos(p.x * freq); }

void main(){
	vec2 res = u_resolution;
	float s = min(res.x, res.y);
	vec2 uv = (gl_FragCoord.xy - 0.5*res) / s;

    float t = u_time * (0.3 + 1.2*u_bandHigh + 0.6*u_onset);
    float f = mix(30.0, 120.0, clamp(u_bandMid + 0.2*u_rms, 0.0, 1.0));
    float fscale = (u_freqScale == 0.0 ? 1.0 : u_freqScale);
    f *= fscale;
	float d = 0.03 + 0.2*u_rms;

    vec2 p1 = rot(0.4 + 0.2*sin(t*0.7) + u_angleOffset1)*uv;
    vec2 p2 = rot(0.4 + d + 0.15*sin(t*0.6) + u_angleOffset2)*uv;

    float a = stripe(p1, f);
    float b = stripe(p2, f*(1.0 + d));
    float m = mix(a*b, 0.5*(a+b), clamp(u_mix, 0.0, 1.0));

    vec3 col = vec3(m);
    col = mix(vec3(0.05,0.06,0.08), col, 0.9);
    col += 0.25*u_onset + 0.15*u_rms;
    float contrast = (u_contrast == 0.0 ? 1.0 : u_contrast);
    col = mix(vec3(0.5), col, contrast);
    float brightness = (u_brightness == 0.0 ? 1.0 : u_brightness);
    col *= brightness;
	FragColor = vec4(col, 1.0);
}


