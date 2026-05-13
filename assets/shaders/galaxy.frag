#version 150
precision highp float;

uniform vec2 u_resolution;
uniform float u_time;
uniform float u_rms;
uniform float u_onset;
uniform float u_kickImpact;
uniform float u_bassBody;
uniform float u_leadPresence;
uniform float u_airPresence;
uniform float u_energyLevel;
uniform float u_novelty;
uniform float u_brightness;
uniform float u_beatPulse;
uniform float u_release;
uniform float u_dropEvent;
uniform float u_tension;

out vec4 FragColor;

// helpers
float h1(float x){return fract(sin(x*12.9898)*43758.5453);} // 1D hash noise
mat2 rot(float a){float c=cos(a),s=sin(a);return mat2(c,-s,s,c);} 
float smooth01(float x){ return smoothstep(0.0, 1.0, clamp(x, 0.0, 1.0)); }

void main(){
	vec2 res=u_resolution;
	float s=min(res.x,res.y);
	vec2 uv=(gl_FragCoord.xy-0.5*res)/s;
	float px=1.0/s; // pixel size in uv units

	// mild spin
	float pulse = clamp(0.42 * max(u_beatPulse, u_kickImpact) + 0.14 * u_dropEvent + 0.10 * u_novelty, 0.0, 1.0);
	float macro = smooth01(0.55 * u_energyLevel + 0.25 * u_tension + 0.20 * u_release);
	float spin=0.08 + 0.03*u_rms + 0.07*macro + 0.08*u_airPresence + 0.06*pulse;
	uv=rot(u_time*spin)*uv;

	float r=length(uv)+1e-6;
	float th=atan(uv.y,uv.x);

	// emulate Processing loop by checking nearby rings/angles only
	float star=0.0;
	float f = float(int(mod(u_time*60.0, s))); // approx frameCount%width
	float n0 = floor(r*s+0.5);
	float deg = degrees(th);
	float i0 = floor(deg/12.0+0.5)*12.0;

	for (int dk=-2; dk<=2; ++dk){
		float n = max(1.0, n0 + float(dk));
		float rad = n/s; // ring radius in uv units
		float noi = h1(n+f); // noise(n+f)
		for (int dj=-1; dj<=1; ++dj){
			float i = i0 + float(dj)*12.0; // nearest 12° steps
			float aX = radians(i + noi*100.0 + n);
			float aY = radians(i + n);
			vec2 p = vec2(sin(aX), cos(aY)) * rad; // point position
			float d = length(uv - p);
			star = max(star, 1.0 - smoothstep(0.0, 0.65*px, d)); // ~1px point
		}
	}

	// brightness like stroke(map(i,...)) using local angle
	float bright = clamp(i0/300.0, 0.2, 1.0) * (0.72 + 0.18*u_brightness + 0.12*u_leadPresence + 0.08*macro);
	vec3 col = vec3(star*bright);
	col += vec3(0.10*u_novelty + 0.08*pulse, 0.07*u_bassBody + 0.04*u_release, 0.16*u_airPresence + 0.05*macro) * star;
	FragColor = vec4(col,1.0);
}
