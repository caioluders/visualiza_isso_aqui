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
uniform float u_beat;
uniform float u_beatEnv;
uniform float u_bpm;
uniform float u_percRatio;
uniform float u_kickImpact;
uniform float u_snareImpact;
uniform float u_hatTick;
uniform float u_beatPulse;
uniform float u_subBody;
uniform float u_bassBody;
uniform float u_harmonicBody;
uniform float u_leadPresence;
uniform float u_airPresence;
uniform float u_novelty;
uniform float u_brightness;
uniform float u_percussiveFocus;
uniform float u_energyLevel;
uniform float u_tension;
uniform float u_release;
uniform float u_dropEvent;
uniform float u_sectionChange;

out vec4 FragColor;

mat2 rot(float a) {
	float c = cos(a);
	float s = sin(a);
	return mat2(c, -s, s, c);
}

float hash(vec2 p) {
	return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453);
}

float bandBars(vec2 uv) {
	float x = clamp(uv.x, 0.0, 0.999);
	float idx = floor(x * 16.0);
	float local = fract(x * 16.0);
	float band = 0.0;
	for (int i = 0; i < 16; ++i) {
		if (float(i) == idx) band = clamp(u_bands[i] + 0.55 * u_onsets[i], 0.0, 1.0);
	}
	float h = smoothstep(0.0, 1.0, band);
	float bar = step(abs(uv.y), h);
	float edge = smoothstep(0.48, 0.38, abs(local - 0.5));
	return bar * edge;
}

float tunnel(vec2 p, float t) {
	float r = max(length(p), 0.001);
	float a = atan(p.y, p.x);
	float beat = 1.0 + 0.22 * max(u_beatEnv, u_beatPulse);
	float rings = sin(22.0 * log(r * beat + 0.08) - t * (2.0 + u_airPresence * 2.6 + u_tension * 1.2));
	float spokes = sin(a * mix(6.0, 18.0, clamp(0.5 * u_centroidNorm + 0.5 * u_brightness, 0.0, 1.0)) + t * 0.8);
	return 0.5 + 0.5 * (0.75 * rings + 0.25 * spokes);
}

void main() {
	vec2 res = u_resolution.xy;
	float side = min(res.x, res.y);
	vec2 uv = (gl_FragCoord.xy - 0.5 * res) / side;
	vec2 screenUv = gl_FragCoord.xy / res;

	float low = clamp(0.40 * u_bandLow + 0.25 * u_subBody + 0.35 * u_bassBody, 0.0, 1.0);
	float mid = clamp(0.25 * u_bandMid + 0.45 * u_harmonicBody + 0.30 * u_leadPresence, 0.0, 1.0);
	float high = clamp(0.30 * u_bandHigh + 0.45 * u_airPresence + 0.25 * u_brightness, 0.0, 1.0);
	float transient = clamp(0.38 * u_kickImpact + 0.22 * u_snareImpact + 0.14 * u_hatTick + 0.14 * u_dropEvent + 0.12 * sqrt(clamp(u_flux + u_novelty, 0.0, 1.0)), 0.0, 1.0);
	float beatPulse = smoothstep(0.0, 1.0, max(u_beatEnv, u_beatPulse));
	float perc = clamp(0.55 * u_percRatio + 0.45 * u_percussiveFocus, 0.0, 1.0);

	float bpmRate = clamp(u_bpm / 160.0, 0.5, 1.8);
	float t = u_time * (0.55 + 0.45 * bpmRate);
	vec2 p = uv;
	p *= rot(0.18 * sin(t * 0.3) + 0.65 * (mid - high));
	p += 0.035 * vec2(
		sin(uv.y * 8.0 + t * 2.1),
		cos(uv.x * 7.0 - t * 1.7)
	) * (0.4 + high + transient);

	float core = tunnel(p * (0.90 + 0.20 * low - 0.06 * beatPulse + 0.05 * u_tension), t);
	float bassWave = sin((length(p) * 24.0 - t * 5.0) * (1.0 + low));
	float midGrid = sin((p.x + p.y) * (18.0 + mid * 38.0) + t * 1.4);
	float air = hash(floor((screenUv + t * 0.005) * res / 3.0));
	float bars = bandBars(vec2(screenUv.x, screenUv.y * 2.0 - 1.0));

	vec3 bassColor = vec3(0.02, 0.07, 0.16);
	vec3 midColor = vec3(0.0, 0.78, 0.62);
	vec3 highColor = vec3(1.0, 0.12, 0.58);
	vec3 flashColor = vec3(1.0, 0.92, 0.45);

	vec3 col = bassColor;
	col += midColor * smoothstep(0.18, 0.82, core) * (0.35 + 0.95 * mid);
	col += highColor * smoothstep(0.65, 1.0, bassWave * 0.5 + 0.5) * (0.08 + 0.5 * high);
	col += vec3(0.12, 0.22, 0.45) * smoothstep(0.55, 1.0, midGrid * 0.5 + 0.5) * (0.2 + low);
	col += flashColor * (0.22 * transient + 0.12 * u_beat + 0.18 * beatPulse + 0.24 * u_dropEvent);
	col += mix(midColor, highColor, clamp(0.5 * u_centroidNorm + 0.5 * u_brightness, 0.0, 1.0)) * bars * (0.24 + 0.65 * perc + 0.20 * u_sectionChange);
	col += vec3(air) * high * 0.09;
	col *= 0.88 + 0.22 * u_energyLevel + 0.10 * u_release;

	float vignette = smoothstep(0.95, 0.25, length(uv));
	col *= mix(0.68, 1.08, vignette);
	col *= 0.85 + 0.95 * clamp(u_rms * 2.0, 0.0, 1.0);

	FragColor = vec4(pow(max(col, vec3(0.0)), vec3(0.92)), 1.0);
}
