#pragma once

// Shared procedural wound GLSL helpers (valid in both GLSL 1.20 screen-space woundShader and
// GLSL 1.30 UV-space woundSkin* bake shaders). Deduplicates hash, value noise, bruise mottling,
// cut lens/lip/smear geometry, and scar healing tint.
#define FP_WOUND_GLSL_HELPERS R"glsl(
float woundHash(float x) {
    return fract(sin(x * 12.9898) * 43758.5453);
}

float woundValueNoise(vec2 p) {
    vec2 i = floor(p), f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    float a = woundHash(i.x + i.y * 57.0), b = woundHash(i.x + 1.0 + i.y * 57.0);
    float c = woundHash(i.x + (i.y + 1.0) * 57.0), d = woundHash(i.x + 1.0 + (i.y + 1.0) * 57.0);
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}

vec3 woundBruiseTint(vec2 q, float radius, float severity, float aux, float seed, float fracture, float debugGlow,
                     out float weight) {
    float mottle = 0.78 + 0.22 * sin(q.x * 0.9 + seed * 31.0) * sin(q.y * 1.3 - seed * 17.0);
    float r = length(q) / (radius * mottle);
    weight = min((1.0 - smoothstep(0.3, 1.0, r)) * (0.5 + 0.4 * severity) * (1.0 + 0.35 * fracture), 1.0);
    vec3 outer = mix(mix(vec3(0.85, 0.42, 0.4), vec3(0.62, 0.45, 0.62), aux), vec3(0.5, 0.33, 0.5), fracture);
    vec3 inner = mix(mix(vec3(0.6, 0.18, 0.16), vec3(0.42, 0.17, 0.2), aux), vec3(0.26, 0.08, 0.13), fracture);
    vec3 bruise = mix(outer, inner, 1.0 - smoothstep(0.0, 0.7, r));
    if (debugGlow > 0.5) bruise = vec3(1.0, 0.05, 0.9) * 2.2;
    float graze = severity > 0.6
                      ? step(0.8, woundHash(floor(q.x * 2.0) + floor(q.y * 2.0) * 57.0 + seed * 101.0)) *
                            (1.0 - smoothstep(0.0, 0.45, r))
                      : 0.0;
    return mix(bruise, vec3(0.35, 0.05, 0.04), graze);
}

void woundCutShape(vec2 q, float halfLen, float halfWid, float seed, out float a, out float taper, out float wobble,
                   out float x, out float inside, out float lip) {
    a = q.x / halfLen;
    taper = sqrt(max(1.0 - a * a, 0.0));
    wobble = 0.22 * halfWid * sin(a * 5.0 + seed * 40.0);
    x = abs(q.y - wobble) / max(halfWid * (0.25 + 0.75 * taper), 0.05);
    inside = taper > 0.0 ? 1.0 - smoothstep(0.6, 1.0, x) : 0.0;
    lip = taper * smoothstep(0.7, 1.1, x) * (1.0 - smoothstep(1.3, 2.4, x));
}

vec3 woundCutSkinTint(vec2 q, float a, float x, float inside, float lip, float severity, float aux, float seed,
                      float facing, out float smear) {
    smear = (1.0 - smoothstep(1.2, 4.0, x)) * (1.0 - smoothstep(0.8, 1.25, abs(a))) *
            (0.6 + 0.4 * woundHash(floor(q.x * 1.5 + seed * 13.0) + floor(q.y * 1.5) * 31.0));
    vec3 cut = mix(vec3(1.0), mix(vec3(0.55, 0.3, 0.24), vec3(0.7, 0.22, 0.2), aux), smear * (0.55 + 0.35 * severity));
    cut = mix(cut, vec3(1.0, 0.5, 0.45) * (1.0 + 0.3 * facing), lip * 0.8);
    cut = mix(cut, mix(vec3(0.5, 0.035, 0.03), vec3(0.07, 0.003, 0.003), 1.0 - smoothstep(0.0, 0.8, x)), inside);
    return cut;
}

vec3 woundScarApply(vec3 cut, float taper, float x, float scar) {
    float scarLine = taper * (1.0 - smoothstep(0.25, 0.6, x));
    float scarHalo = taper * (1.0 - smoothstep(0.6, 2.0, x));
    return mix(cut, mix(mix(vec3(1.0), vec3(1.05, 0.9, 0.88), scarHalo * 0.45), vec3(1.14, 1.02, 0.97), scarLine * 0.85), scar);
}
)glsl"
