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
uniform float u_snareImpact;
uniform float u_bassBody;
uniform float u_harmonicBody;
uniform float u_leadPresence;
uniform float u_airPresence;
uniform float u_energyLevel;
uniform float u_novelty;
uniform float u_brightness;
uniform float u_tension;

// User-adjustable params (auto-exposed in UI)
uniform float u_freqScale;     // scales stripe frequency
uniform float u_angleOffset1;  // add to first layer angle
uniform float u_angleOffset2;  // add to second layer angle
uniform float u_mix;           // 0..1, product vs additive moiré
uniform float u_moireBrightness;    // overall brightness
uniform float u_contrast;      // contrast around 0.5

out vec4 FragColor;

mat2 rot(float a){ float c = cos(a), s = sin(a); return mat2(c,-s,s,c); }

float stripe(vec2 p, float freq){ return 0.5 + 0.5*cos(p.x * freq); }
float smooth01(float x){ return smoothstep(0.0, 1.0, clamp(x, 0.0, 1.0)); }
float softCompress(float x){ return x / (1.0 + x); }

void main(){
	vec2 res = u_resolution;
	float s = min(res.x, res.y);
	vec2 uv = (gl_FragCoord.xy - 0.5*res) / s;

    float pulse = clamp(0.32*u_kickImpact + 0.22*u_snareImpact + 0.12*u_novelty, 0.0, 1.0);
    float macro = smooth01(0.55*u_energyLevel + 0.30*u_tension + 0.15*u_bassBody);
    float t = u_time * (0.22 + 0.42*u_airPresence + 0.22*u_tension + 0.14*u_snareImpact + 0.12*u_novelty);
    float f = mix(30.0, 110.0, clamp(0.12*u_bandMid + 0.12*u_rms + 0.34*u_harmonicBody + 0.24*u_brightness + 0.18*u_leadPresence, 0.0, 1.0));
    float fscale = (u_freqScale == 0.0 ? 1.0 : u_freqScale);
    f *= fscale;
    float d = 0.03 + 0.05*macro + 0.07*u_novelty + 0.05*u_bassBody;

    vec2 p1 = rot(0.4 + 0.2*sin(t*0.7) + u_angleOffset1)*uv;
    vec2 p2 = rot(0.4 + d + 0.15*sin(t*0.6) + u_angleOffset2)*uv;

    float a = stripe(p1, f);
    float b = stripe(p2, f*(1.0 + d));
    float m = mix(a*b, 0.5*(a+b), clamp(u_mix, 0.0, 1.0));

    vec3 col = vec3(m);
    col = mix(vec3(0.05,0.06,0.08), col, 0.9);
    col += vec3(0.10, 0.08, 0.05) * pulse + vec3(0.04, 0.04, 0.06) * macro;
    float contrast = (u_contrast == 0.0 ? 1.0 : u_contrast);
    col = mix(vec3(0.5), col, contrast);
    float brightness = (u_moireBrightness == 0.0 ? 1.0 : u_moireBrightness);
    col *= brightness * (0.92 + 0.10*softCompress(u_tension + u_energyLevel));
	FragColor = vec4(col, 1.0);
}
