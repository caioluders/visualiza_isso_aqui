#version 150
precision highp float;

uniform vec2 u_resolution;
uniform float u_time;
uniform float u_rms;
uniform float u_onset;
uniform float u_bandLow;
uniform float u_bandMid;
uniform float u_bandHigh;
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

// helpers
float h1(float x){return fract(sin(x*12.9898)*43758.5453);} // 1D hash noise
mat2 rot(float a){float c=cos(a),s=sin(a);return mat2(c,-s,s,c);} 
float sat(float x){ return clamp(x, 0.0, 1.0); }

void main(){
	vec2 res=u_resolution;
	float s=min(res.x,res.y);
	vec2 uv=(gl_FragCoord.xy-0.5*res)/s;
	float px=1.0/s; // pixel size in uv units

	float bass = (u_bands[0] + u_bands[1] + u_bands[2]) / 3.0;
	float mid = 0.0; for (int i=3; i<=9; ++i) mid += u_bands[i]; mid /= 7.0;
	float high = 0.0; for (int i=10; i<16; ++i) high += u_bands[i]; high /= 6.0;
	float onsetAvg = 0.0; for (int i=0; i<16; ++i) onsetAvg += u_onsets[i]; onsetAvg /= 16.0;
	float pulse = sat(0.40*u_beatEnv + 0.20*onsetAvg + 0.18*u_onsetDensity + 0.24*u_drop + 0.18*u_isolatedHit);
	float detail = sat(0.30*u_rolloff + 0.24*u_contrastMean + 0.18*u_crest + 0.18*high + 0.10*u_novelty);
	float groove = mix(sin(6.2831853 * max(30.0, u_bpm) * u_time / 60.0), sin(6.2831853 * u_beatPhase), u_beatConfidence);
	vec3 chroma = normalize(vec3(
		0.16 + u_chroma[0] + 0.70*u_chroma[7] + 0.45*u_chroma[9],
		0.16 + u_chroma[2] + 0.75*u_chroma[4] + 0.35*u_chroma[11],
		0.16 + u_chroma[3] + 0.70*u_chroma[5] + 0.55*u_chroma[10]
	));

	// mild spin
	float spin=0.06 + 0.16*bass + 0.12*high + 0.22*u_buildUp + 0.18*pulse;
	uv=rot(u_time*spin + 0.16*groove*u_beatConfidence + 0.25*u_layerChange)*uv;
	uv *= 1.0 - 0.08*pulse + 0.06*u_breakState;

	float r=length(uv)+1e-6;
	float th=atan(uv.y,uv.x);

	// emulate Processing loop by checking nearby rings/angles only
	float star=0.0;
	float f = float(int(mod(u_time*(36.0 + 70.0*detail + 35.0*u_buildUp + 22.0*u_highDensity), s))); // approx frameCount%width
	float n0 = floor(r*s+0.5);
	float deg = degrees(th);
	float stepDeg = mix(18.0, 7.0, detail);
	float i0 = floor(deg/stepDeg+0.5)*stepDeg;

	for (int dk=-2; dk<=2; ++dk){
		float n = max(1.0, n0 + float(dk));
		float rad = n/s; // ring radius in uv units
		float noi = h1(n+f + 20.0*u_energyDelta); // noise(n+f)
		for (int dj=-1; dj<=1; ++dj){
			float i = i0 + float(dj)*stepDeg;
			float aX = radians(i + noi*(80.0 + 80.0*high) + n + 25.0*pulse);
			float aY = radians(i + n + 18.0*u_layerChange);
			vec2 p = vec2(sin(aX), cos(aY)) * rad; // point position
			float d = length(uv - p);
			float size = mix(0.55, 1.8, sat(pulse + high*0.4 + u_isolatedHit*0.5));
			star = max(star, 1.0 - smoothstep(0.0, size*px, d));
		}
	}

	// brightness like stroke(map(i,...)) using local angle
	float bright = clamp(i0/300.0, 0.2, 1.0);
	float spiral = smoothstep(0.04, 0.0, abs(sin(3.0*th + 9.0*r - u_time*(0.4 + bass))));
	float dust = spiral * exp(-r*2.2) * (0.10 + 0.45*mid) * (1.0 - 0.55*u_breakState);
	vec3 cool = vec3(0.35, 0.85, 1.00);
	vec3 warm = vec3(1.00, 0.45, 0.22);
	vec3 gold = vec3(0.98, 0.80, 0.25);
	vec3 tint = normalize(cool*(0.35 + high) + warm*(0.25 + bass) + gold*(0.20 + mid));
	tint = mix(tint, chroma, 0.18 + 0.35*u_chromaFlux);
	tint = mix(tint, vec3(0.72, 0.92, 1.0), 0.20*u_percRatio + 0.16*u_rolloff);
	vec3 col = tint * star * bright * (0.8 + 1.4*pulse + 0.7*u_rms);
	col += dust * mix(warm, cool, high);
	col += u_drop * exp(-r*2.8) * vec3(0.40, 0.30, 0.18);
	FragColor = vec4(col,1.0);
}
