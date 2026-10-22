#pragma once
// Stone skin maps: thick rounded rock plates split by dark crevices, made on the CPU once.
// Periodic Voronoi cells (the texture tiles), a rounded slab bevel per plate, crevice
// occlusion baked into the colour, and a normal map from the plate heights. The game's own
// material program shades them with its own lights; nothing here is a shader.
#include <cmath>
#include <cstdint>
#include <vector>
namespace fp::stone {
inline constexpr unsigned size = 512, cells = 7, levels = 10; // 512 .. 1

struct Maps {
    // RGBA8 mip chains, level 0 first: diffuse, specular, normal (xyz in rgb, x again in a,
    // so a shader that reads its normal from rg, ag or rgb sees the same vector).
    std::vector<uint8_t> diffuse, specular, normal;
    size_t offset[levels]{};
};

inline uint32_t hash(uint32_t x, uint32_t y, uint32_t salt) {
    uint32_t h = x * 0x8da6b343u ^ y * 0xd8163841u ^ salt * 0xcb1ab31fu;
    h ^= h >> 13; h *= 0x5bd1e995u; h ^= h >> 15;
    return h;
}
inline float unit(uint32_t h) { return float(h & 0xffffff) / float(0x1000000); }
inline int wrap(int v, int n) { return ((v % n) + n) % n; }

// Value noise with a period of `period` lattice steps across the texture, so it tiles.
inline float noise(float x, float y, int period, uint32_t salt) {
    const int ix = int(std::floor(x)), iy = int(std::floor(y));
    const float fx = x - float(ix), fy = y - float(iy);
    const float sx = fx * fx * (3 - 2 * fx), sy = fy * fy * (3 - 2 * fy);
    auto at = [&](int a, int b) { return unit(hash(uint32_t(wrap(a, period)), uint32_t(wrap(b, period)), salt)); };
    const float top = at(ix, iy) + (at(ix + 1, iy) - at(ix, iy)) * sx;
    const float bottom = at(ix, iy + 1) + (at(ix + 1, iy + 1) - at(ix, iy + 1)) * sx;
    return top + (bottom - top) * sy;
}
// Octaves from `low` to `high` lattice steps across the texture (u, v in 0..1), 0..1.
inline float fbm(float u, float v, int low, int high, uint32_t salt, float gain = .55f) {
    float sum = 0, amp = .5f, norm = 0;
    for (int period = low; period <= high; period *= 2, amp *= gain) {
        sum += amp * noise(u * float(period), v * float(period), period, salt + uint32_t(period));
        norm += amp;
    }
    return sum / norm;
}
inline float grain(float u, float v, uint32_t salt) { return fbm(u, v, 16, 128, salt); }
// Sharp creases where the noise crosses its middle: fractures and grit, 0..1 with 1 on a crease.
inline float ridged(float u, float v, int low, int high, uint32_t salt) {
    float sum = 0, amp = .5f, norm = 0;
    for (int period = low; period <= high; period *= 2, amp *= .5f) {
        const float r = 1.f - std::fabs(2.f * noise(u * float(period), v * float(period), period, salt + uint32_t(period)) - 1.f);
        sum += amp * r * r;
        norm += amp;
    }
    return sum / norm;
}
inline float smooth(float a, float b, float x) {
    const float t = std::fmin(1.f, std::fmax(0.f, (x - a) / (b - a)));
    return t * t * (3 - 2 * t);
}

struct Cell { float x, y; uint32_t id; };
// The feature point of lattice cell (cx, cy), cx and cy taken modulo the period.
inline Cell point(int cx, int cy) {
    const uint32_t wx = uint32_t(wrap(cx, int(cells))), wy = uint32_t(wrap(cy, int(cells)));
    const uint32_t h = hash(wx, wy, 0x51);
    return {float(cx) + .08f + .84f * unit(h), float(cy) + .08f + .84f * unit(hash(wx, wy, 0x52)), wy * cells + wx};
}

struct Sample { float border, dx, dy; uint32_t id; }; // border: distance to the nearest cell edge, in cells
inline Sample voronoi(float px, float py) {
    const int cx = int(std::floor(px)), cy = int(std::floor(py));
    Cell best{}; float bestD = 1e9f;
    for (int j = -1; j <= 1; ++j) for (int i = -1; i <= 1; ++i) {
        const Cell c = point(cx + i, cy + j);
        const float d = (c.x - px) * (c.x - px) + (c.y - py) * (c.y - py);
        if (d < bestD) { bestD = d; best = c; }
    }
    // A smooth minimum over the edges rounds the plate corners (no ridge lines into them).
    float soft = 0, hard = 1e9f;
    const float k = .045f;
    const int bx = int(std::floor(best.x)), by = int(std::floor(best.y));
    for (int j = -2; j <= 2; ++j) for (int i = -2; i <= 2; ++i) {
        const Cell c = point(bx + i, by + j);
        const float ex = c.x - best.x, ey = c.y - best.y, len = std::sqrt(ex * ex + ey * ey);
        if (len < 1e-5f) continue;
        const float mx = (c.x + best.x) * .5f - px, my = (c.y + best.y) * .5f - py;
        const float d = (mx * ex + my * ey) / len;
        hard = std::fmin(hard, d);
        soft += std::exp(-d / k);
    }
    const float border = soft > 0 ? std::fmax(0.f, std::fmin(hard, -k * std::log(soft))) : hard;
    return {border, px - best.x, py - best.y, best.id};
}

inline uint8_t byte(float v) { return uint8_t(std::lround(std::fmin(1.f, std::fmax(0.f, v)) * 255.f)); }

inline void generate(Maps& out) {
    const unsigned n = size;
    std::vector<float> height(size_t(n) * n), occlusion(size_t(n) * n);
    std::vector<float> colour(size_t(n) * n * 3), gloss(size_t(n) * n);
    const float crack = .022f, bevel = .3f;
    for (unsigned y = 0; y < n; ++y) for (unsigned x = 0; x < n; ++x) {
        const float u = (float(x) + .5f) / float(n), v = (float(y) + .5f) / float(n);
        // Warped cells, so plate edges wander and chip instead of running straight.
        const float wu = u + .018f * (fbm(u, v, 6, 24, 41) - .5f) + .004f * (fbm(u, v, 48, 192, 43) - .5f);
        const float wv = v + .018f * (fbm(u, v, 6, 24, 42) - .5f) + .004f * (fbm(u, v, 48, 192, 44) - .5f);
        Sample s = voronoi(wu * float(cells), wv * float(cells));
        s.border = std::fmax(0.f, s.border + .03f * (fbm(u, v, 32, 128, 45) - .55f));
        const uint32_t h = hash(s.id, 7, 0x77);
        // A rounded slab: uneven crown, worn bevel down into the crevice, each plate a little tilted.
        const float t = std::fmin(1.f, std::fmax(0.f, (s.border - crack) / bevel));
        const float bevelled = std::sqrt(1.f - (1.f - t) * (1.f - t));
        const float tiltX = unit(h) - .5f, tiltY = unit(h >> 8) - .5f;
        const float crown = .85f + .15f * unit(h >> 16) + .18f * (tiltX * s.dx + tiltY * s.dy) - .35f * (s.dx * s.dx + s.dy * s.dy);
        // Rock surface: lumps, gritty pitting, fine fractures across the plates.
        const float lumps = fbm(u, v, 12, 48, 3) - .5f, grit = fbm(u, v, 96, 256, 5, .7f) - .5f;
        const float fracture = smooth(.86f, .97f, ridged(u, v, 20, 80, 7)) * smooth(.45f, .65f, fbm(u, v, 8, 16, 8));
        const float pits = smooth(.62f, .78f, fbm(u, v, 64, 256, 9, .6f));
        const size_t k = size_t(y) * n + x;
        height[k] = s.border < crack ? -.15f * (1.f - s.border / crack)
                  : bevelled * crown + .16f * lumps * t + .07f * grit - .14f * fracture - .06f * pits;
        occlusion[k] = (.16f + .84f * smooth(0.f, crack + bevel * .7f, s.border)) * (1.f - .45f * fracture) * (1.f - .25f * pits);
        // Weathered grey granite: each plate its own shade, some a touch warmer or cooler, mottled,
        // flecked light and dark, with faint earthy staining.
        const float shade = .78f + .3f * unit(h >> 4), warm = unit(h >> 12) - .5f; // warm: -0.5 cool .. +0.5 warm
        const float rim = .8f + .2f * smooth(crack, crack + bevel, s.border);
        const float mottle = .8f + .4f * fbm(u, v, 8, 64, 21), fleck = fbm(u, v, 128, 256, 23, .8f);
        const float speck = fleck > .68f ? 1.25f : fleck < .3f ? .72f : 1.f;
        const float stain = smooth(.55f, .8f, fbm(u, v, 6, 24, 25)) * .5f; // rusty-brown patches
        float r = .5f * shade * (1.f + .06f * warm), gr = .49f * shade, b = .46f * shade * (1.f - .07f * warm);
        r *= 1.f + .14f * stain; gr *= 1.f + .02f * stain; b *= 1.f - .12f * stain;
        const float m = rim * mottle * speck * (1.f + .5f * grit);
        r *= m; gr *= m; b *= m;
        if (s.border < crack) { r = .09f; gr = .085f; b = .08f; }
        colour[k * 3] = r * occlusion[k];
        colour[k * 3 + 1] = gr * occlusion[k];
        colour[k * 3 + 2] = b * occlusion[k];
        // Stone is barely glossy: a faint dull sheen on the worn crowns only.
        gloss[k] = s.border < crack ? 0.f : (.015f + .03f * t * fbm(u, v, 16, 64, 27)) * (1.f - pits);
    }
    // Level 0, then box-filtered levels down to 1 x 1 (normals renormalised).
    size_t total = 0;
    for (unsigned l = 0, w = n; l < levels; ++l, w = w > 1 ? w / 2 : 1) { out.offset[l] = total; total += size_t(w) * w * 4; }
    out.diffuse.assign(total, 0); out.specular.assign(total, 0); out.normal.assign(total, 0);
    const float strength = 1.5f;
    for (unsigned y = 0; y < n; ++y) for (unsigned x = 0; x < n; ++x) {
        const size_t k = size_t(y) * n + x, o = k * 4;
        auto at = [&](int a, int b) { return height[size_t(wrap(b, int(n))) * n + size_t(wrap(a, int(n)))]; };
        const float dx = (at(int(x) + 1, int(y)) - at(int(x) - 1, int(y))) * .5f * float(n) / float(cells) * bevel;
        const float dy = (at(int(x), int(y) + 1) - at(int(x), int(y) - 1)) * .5f * float(n) / float(cells) * bevel;
        float nx = -dx * strength, ny = -dy * strength, nz = 1.f;
        const float len = std::sqrt(nx * nx + ny * ny + nz * nz);
        nx /= len; ny /= len; nz /= len;
        out.diffuse[o] = byte(colour[k * 3]); out.diffuse[o + 1] = byte(colour[k * 3 + 1]);
        out.diffuse[o + 2] = byte(colour[k * 3 + 2]); out.diffuse[o + 3] = 255;
        out.specular[o] = out.specular[o + 1] = out.specular[o + 2] = byte(gloss[k] * occlusion[k]);
        out.specular[o + 3] = byte(.06f * occlusion[k]);
        out.normal[o] = byte(nx * .5f + .5f); out.normal[o + 1] = byte(ny * .5f + .5f);
        out.normal[o + 2] = byte(nz * .5f + .5f); out.normal[o + 3] = out.normal[o];
    }
    for (unsigned l = 1, w = n / 2; l < levels; ++l, w = w > 1 ? w / 2 : 1) {
        const unsigned p = w * 2;
        for (std::vector<uint8_t>* map : {&out.diffuse, &out.specular, &out.normal}) {
            uint8_t* dst = map->data() + out.offset[l];
            const uint8_t* src = map->data() + out.offset[l - 1];
            for (unsigned y = 0; y < w; ++y) for (unsigned x = 0; x < w; ++x) {
                float sum[4]{};
                for (unsigned j = 0; j < 2; ++j) for (unsigned i = 0; i < 2; ++i)
                    for (unsigned c = 0; c < 4; ++c) sum[c] += src[(size_t(y * 2 + j) * p + x * 2 + i) * 4 + c];
                uint8_t* d = dst + (size_t(y) * w + x) * 4;
                if (map == &out.normal) {
                    float v3[3];
                    for (unsigned c = 0; c < 3; ++c) v3[c] = sum[c] / (4.f * 255.f) * 2.f - 1.f;
                    const float len = std::sqrt(v3[0] * v3[0] + v3[1] * v3[1] + v3[2] * v3[2]);
                    for (unsigned c = 0; c < 3; ++c) d[c] = byte((len > 1e-5f ? v3[c] / len : (c == 2 ? 1.f : 0.f)) * .5f + .5f);
                    d[3] = d[0];
                } else {
                    for (unsigned c = 0; c < 4; ++c) d[c] = uint8_t(std::lround(sum[c] / 4.f));
                }
            }
        }
    }
}
} // namespace fp::stone
