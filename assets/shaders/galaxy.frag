#version 150
precision highp float;

uniform vec2 u_resolution;
uniform float u_time;
uniform float u_rms;
uniform float u_onset;

out vec4 FragColor;

// helpers
float h1(float x){return fract(sin(x*12.9898)*43758.5453);} // 1D hash noise
mat2 rot(float a){float c=cos(a),s=sin(a);return mat2(c,-s,s,c);} 

void main(){
	vec2 res=u_resolution;
	float s=min(res.x,res.y);
	vec2 uv=(gl_FragCoord.xy-0.5*res)/s;
	float px=1.0/s; // pixel size in uv units

	// mild spin
	float spin=0.12 + 0.10*u_rms + 0.25*u_onset;
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
	float bright = clamp(i0/300.0, 0.2, 1.0);
	vec3 col = vec3(star*bright);
	FragColor = vec4(col,1.0);
}


