/* Generative 3D fractal (Mandelbox-style) raymarch, purple palette
   GL 3.2 core, app-compatible uniforms (u_time, u_resolution, optional iResolution)
*/

#version 150
precision highp float;

uniform vec2  u_resolution;
uniform vec2  iResolution;  // optional; app may not set it
uniform float u_time;

out vec4 FragColor;

// Camera helpers
mat3 rotY(float a){ float c=cos(a), s=sin(a); return mat3(c,0.0,-s, 0.0,1.0,0.0, s,0.0,c); }
mat3 rotX(float a){ float c=cos(a), s=sin(a); return mat3(1.0,0.0,0.0, 0.0,c,-s, 0.0,s,c); }

// Mandelbox distance estimator
float mandelboxDE(vec3 p){
    vec3 z = p;
    float dr = 1.0;
    float scale = 2.3;
    float minR2 = 0.5;
    float fixedR2 = 1.0;
    for (int i = 0; i < 12; ++i){
        // Box fold
        z = clamp(z, -1.0, 1.0) * 2.0 - z;
        // Sphere fold
        float r2 = dot(z, z);
        if (r2 < minR2){
            float t = (fixedR2 / minR2);
            z *= t;
            dr *= t;
        } else if (r2 > fixedR2){
            float t = fixedR2 / r2;
            z *= t;
            dr *= t;
        }
        // Scale and translate
        z = z * scale + p;
        dr = dr * abs(scale) + 1.0;
    }
    float r = length(z);
    return 0.5 * log(r) * r / abs(dr + 1e-6);
}

float map(vec3 p){
    return mandelboxDE(p);
}

vec3 calcNormal(vec3 p){
    // numerical gradient
    float e = 1e-3;
    vec2 h = vec2(1.0, -1.0) * e;
    return normalize(h.xyy * map(p + h.xyy) +
                     h.yyx * map(p + h.yyx) +
                     h.yxy * map(p + h.yxy) +
                     h.xxx * map(p + h.xxx));
}

// Purple palette
vec3 palette(float t){
    vec3 a = vec3(0.10, 0.00, 0.20);
    vec3 b = vec3(0.65, 0.20, 0.90);
    vec3 c = vec3(0.20, 0.10, 0.30);
    vec3 d = vec3(0.00, 0.33, 0.67);
    return a + b * cos(6.28318 * (c * t + d));
}

void main(){
    vec2 res = (iResolution.x > 0.0) ? iResolution : u_resolution;
    vec2 uv  = (gl_FragCoord.xy - 0.5 * res) / max(res.y, 1.0);

    // Camera
    float t = u_time * 0.25;
    vec3 ro = vec3(0.0, 0.0, 4.0);
    ro = rotY(t*0.7) * rotX(sin(t*0.5)*0.3) * ro;
    vec3 ta = vec3(0.0);
    vec3 ww = normalize(ta - ro);
    vec3 uu = normalize(cross(vec3(0.0,1.0,0.0), ww));
    vec3 vv = cross(ww, uu);
    float fov = 1.6; // ~60 deg
    vec3 rd = normalize(uu * uv.x + vv * uv.y + ww * fov);

    // Raymarch
    float tacc = 0.0;
    float d;
    float total = 0.0;
    vec3 p;
    const int MAX_STEPS = 128;
    const float SURF = 0.0008;
    const float FAR = 30.0;
    int i;
    for (i = 0; i < MAX_STEPS; ++i){
        p = ro + rd * tacc;
        d = map(p);
        if (d < SURF || tacc > FAR) break;
        // step with safety factor
        tacc += d * 0.9;
        total += 1.0;
    }

    vec3 col = vec3(0.0);
    if (d < SURF){
        vec3 n = calcNormal(p);
        // Soft lighting
        vec3 lightPos = vec3(2.5 * cos(t*1.7), 1.8, 2.5 * sin(t*1.3));
        vec3 l = normalize(lightPos - p);
        float diff = clamp(dot(n, l), 0.0, 1.0);
        float spec = pow(clamp(dot(reflect(-l, n), -rd), 0.0, 1.0), 32.0);
        float ao = exp(-0.03 * total); // simple AO from steps
        vec3 base = palette(0.3 + 0.15 * total);
        col = base * (0.2 + 0.8 * diff) * ao + 0.25 * spec * vec3(1.0);
    }

    // Fog to purple space
    float fog = 1.0 - exp(-0.035 * tacc);
    vec3 fogCol = vec3(0.06, 0.0, 0.10);
    col = mix(col, fogCol, fog);

    // Subtle filmic tone
    col = col / (col + 1.0);
    col = pow(col, vec3(0.4545));

    FragColor = vec4(col, 1.0);
}


