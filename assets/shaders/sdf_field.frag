#version 150
precision highp float;

// True SDF raymarcher: an infinite field of primitives that MORPH between
// shapes with the music, with twist warp, lighting, fog and edge glow.

uniform vec2  u_resolution;
uniform float u_time;
uniform float u_energyLevel;
uniform float u_kickImpact;
uniform float u_bassBody;
uniform float u_leadPresence;
uniform float u_airPresence;
uniform float u_tension;
uniform float u_beatPulse;
out vec4 FragColor;

mat2 rot(float a){ float c=cos(a), s=sin(a); return mat2(c,-s,s,c); }

// --- SDF primitives ---
float sdSphere(vec3 p, float r){ return length(p) - r; }
float sdBox(vec3 p, vec3 b){ vec3 q=abs(p)-b; return length(max(q,0.0)) + min(max(q.x,max(q.y,q.z)),0.0); }
float sdOcta(vec3 p, float s){ p=abs(p); return (p.x+p.y+p.z-s) * 0.57735; }
float sdTorus(vec3 p, vec2 t){ vec2 q=vec2(length(p.xz)-t.x, p.y); return length(q)-t.y; }
float smin(float a, float b, float k){ float h=clamp(0.5+0.5*(b-a)/k,0.0,1.0); return mix(b,a,h)-k*h*(1.0-h); }

// Scene: a repeated cell whose shape morphs with energy/tension, twisted by bass.
float map(vec3 p){
    vec3 q = p;
    q.xy *= rot(q.z * 0.18 + u_bassBody * 1.5);        // twist the tunnel
    vec3 cell = mod(q, 3.0) - 2.0;                       // infinite repetition

    float r = 0.55 + 0.30 * u_kickImpact;               // kick swells the shapes
    float s = sdSphere(cell, r);
    float b = sdBox(cell, vec3(r * 1.8));
    float o = sdOcta(cell, r * 1.0);
    float t = sdTorus(cell, vec2(r, r * 0.35));

    float shape = mix(s, b, smoothstep(0.0, 1.0, u_energyLevel)); // sphere -> box with energy
    shape = mix(shape, o, u_tension);                            // -> octahedron on build-ups
    shape = smin(shape, t, 0.35);                               // fuse a torus in (blobby)
    return shape;
}

vec3 calcNormal(vec3 p){
    vec2 e = vec2(0.0015, 0.0);
    return normalize(vec3(
        map(p+e.xyy)-map(p-e.xyy),
        map(p+e.yxy)-map(p-e.yxy),
        map(p+e.yyx)-map(p-e.yyx)));
}

void main(){
    vec2 uv = (2.0*gl_FragCoord.xy - u_resolution) / u_resolution.y;

    // Camera flies forward with a sway; lead rolls the view.
    vec3 ro = vec3(0.6*sin(u_time*0.2), 1.5*cos(u_time*0.15), u_time*5.4);
    vec3 rd = normalize(vec3(uv, 1.3));
    rd.xy *= rot(u_time*0.04 + u_leadPresence*0.6);

    float t = 0.0, glow = 0.0;
    bool hit = false;
    vec3 p;
    for (int i = 0; i < 90; i++) {
        p = ro + rd * t;
        float d = map(p);
        glow += 0.018 / (0.1 + d*d);          // edge / volumetric glow
        if (d < .01) { hit = true; break; }
        t += d * 1.55;                          // 0.55: warped SDF safety factor
        if (t > 40.0) break;
    }

    vec3 col = vec3(0.015, 0.015, 0.03);        // background
    if (hit) {
        vec3 n = calcNormal(p);
        vec3 l = normalize(vec3(0.6, 0.8, 0.4));
        float diff = clamp(dot(n, l), 0.0, 1.0);
        float fres = pow(1.0 - clamp(dot(n, -rd), 0.0, 1.0), 3.0);
        vec3 pal = 0.5 + 0.5 * cos(t*0.2 + 3.0*u_airPresence + vec3(0.0, 2.0, 4.0));
        col = pal * (0.2 + diff) + fres * vec3(0.4, 0.6, 1.0);
        col *= exp(-0.03 * t);                  // distance fog
    }
    col += glow * 0.00 * vec3(0.3, 0.6, 1.0);   // glow even on misses
    // col *= 0.7 + 0.8 * u_energyLevel;           // energy brightness
    // col += u_beatPulse * u_beatPulse * 0.3;     // beat flash

    FragColor = vec4(pow(col, vec3(0.4545)), 1.0);
}
