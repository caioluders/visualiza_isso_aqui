#version 150
precision highp float;

uniform vec2 u_resolution;
uniform float u_time;
uniform float u_rms;
uniform float u_bandLow;
uniform float u_bandMid;
uniform float u_bandHigh;
uniform float u_onset;
uniform float u_bands[16];
uniform float u_onsets[16];
uniform float u_centroidNorm;
uniform float u_flux;
uniform float u_beatEnv;
uniform float u_bpm;
uniform float u_percRatio;
uniform float u_energyDelta;
uniform float u_drop;
uniform float u_breakState;
uniform float u_buildUp;
uniform float u_layerChange;
uniform float u_isolatedHit;
uniform float u_rolloff;
uniform float u_flatness;
uniform float u_crest;
uniform float u_contrastBands[6];
uniform float u_contrastMean;
uniform float u_chroma[12];
uniform float u_chromaFlux;
uniform float u_onsetDensity;
uniform float u_lowDensity;
uniform float u_highDensity;
uniform float u_beatPhase;
uniform float u_beatConfidence;
uniform float u_novelty;

out vec4 FragColor;

float sat(float x){ return clamp(x, 0.0, 1.0); }
mat2 rot(float a){ float c = cos(a), s = sin(a); return mat2(c,-s,s,c); }

void main(){
	vec2 uv = gl_FragCoord.xy / u_resolution.xy;
	vec2 p = uv * 2.0 - 1.0;
	p.x *= u_resolution.x / max(u_resolution.y, 1.0);

	float bass = (u_bands[0] + u_bands[1] + u_bands[2]) / 3.0;
	float mid = 0.0; for (int i = 3; i <= 9; ++i) mid += u_bands[i]; mid /= 7.0;
	float high = 0.0; for (int i = 10; i < 16; ++i) high += u_bands[i]; high /= 6.0;
	float onLow = (u_onsets[0] + u_onsets[1] + u_onsets[2]) / 3.0;
	float onHigh = 0.0; for (int i = 10; i < 16; ++i) onHigh += u_onsets[i]; onHigh /= 6.0;

	float pulse = sat(0.40*u_beatEnv + 0.20*u_onset + 0.18*u_onsetDensity + 0.18*onLow + 0.30*u_drop);
	float detail = sat(0.28*u_rolloff + 0.24*u_contrastMean + 0.18*u_crest + 0.18*high + 0.12*u_novelty);
	float structure = sat(0.28*u_energyDelta + 0.28*u_layerChange + 0.26*u_buildUp + 0.24*u_novelty);
	float groove = mix(sin(6.2831853 * max(30.0, u_bpm) * u_time / 60.0), sin(6.2831853 * u_beatPhase), u_beatConfidence);
	vec3 chroma = normalize(vec3(
		0.16 + u_chroma[0] + 0.70*u_chroma[7] + 0.45*u_chroma[9],
		0.16 + u_chroma[2] + 0.75*u_chroma[4] + 0.35*u_chroma[11],
		0.16 + u_chroma[3] + 0.70*u_chroma[5] + 0.55*u_chroma[10]
	));

	p *= rot(0.10*sin(u_time*0.15) + 0.10*groove*u_beatConfidence + 0.18*u_layerChange);
	float r = length(p);
	float a = atan(p.y, p.x);
	float breathe = 1.0 + 0.10*bass + 0.06*pulse - 0.05*u_breakState;
	float rings = 0.5 + 0.5*sin(r * mix(7.0, 28.0, detail) * breathe - u_time*(0.45 + 1.2*bass));
	float spokes = 0.5 + 0.5*cos(a * mix(3.0, 11.0, mid) + u_time*(0.25 + high));
	float veil = smoothstep(0.15, 0.95, uv.y + 0.06*sin(p.x*3.0 + u_time*0.4));
	float field = mix(veil, rings*spokes, 0.35 + 0.45*structure);

	vec3 shadow = mix(vec3(0.02, 0.035, 0.05), vec3(0.04, 0.02, 0.03), u_breakState);
	vec3 lowCol = vec3(0.05, 0.78, 0.72);
	vec3 midCol = vec3(0.98, 0.33, 0.22);
	vec3 highCol = vec3(0.96, 0.82, 0.22);
	vec3 pal = lowCol*bass + midCol*mid + highCol*high;
	pal = mix(pal, chroma, 0.18 + 0.35*u_chromaFlux);
	pal = mix(pal, vec3(0.68, 0.84, 1.00), 0.20*u_percRatio + 0.20*u_isolatedHit + 0.12*u_rolloff);

	vec3 col = mix(shadow, pal + vec3(0.18, 0.12, 0.10), field);
	col += pulse * vec3(0.30, 0.24, 0.18);
	col += onHigh * vec3(0.10, 0.18, 0.26);
	col *= 0.72 + 0.45*u_rms + 0.25*(1.0 - u_breakState);
	FragColor = vec4(pow(max(col, 0.0), vec3(0.9)), 1.0);
}
