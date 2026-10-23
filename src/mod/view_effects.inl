// Optional screen-space view effects: ambient occlusion, reflections, depth of
// field, edge smoothing, and the helmet, ghost and hurt/winded finishing pass.
// They run inside the native renderer, straight after the main view's world
// pass and before the game's bloom and UI (see renderSampledViews below), on
// the completed colour and depth of the default framebuffer. No game light,
// material, camera or depth state is owned or written. Part of
// first_person.cpp's single translation unit; included after overlay.inl,
// whose exact-context GL owner and retirement path it reuses.
// Each effect's strength, 0..1; 0 skips its passes. The look the settings used to call Subtle
// (and an on/off effect's on) is 0.5, Strong is 1 (see effectStrength, settings.inl).
static std::atomic<float> ambientOcclusion{0.f}, depthOfField{0.f}, screenSpaceReflections{0.f};
static std::atomic<float> edgeSmoothing{0.f}, helmetVisor{0.f}, playerGhost{0.f}, conditionEffects{0.f};
static std::atomic<float> heldDetail{0.5f}; // procedural surface detail on held items; Subtle by default
static std::atomic<float> armourDetail{0.35f}; // the same on worn armour and clothing; 35% by default (frame cost)
static std::atomic<float> objectDetail{0.35f}; // the same on loose items and placed objects near the player; 35%
static std::atomic<bool> lightSmoothing{true}; // tricubic reads of the game's GI voxel volume (shader rewrite)
static std::atomic<float> levelDetail{0.3f};   // walls, floors and ceilings within about 3 m; 30% by default
static std::atomic<float> levelDetailScale{1.f}; // 1 down to 0 as the frame slows (see levelDetailFor)

// A setting's stored value as a strength, and back. The old choices 0, 1, 2 (Off, Subtle,
// Strong) stay as they are, so saved files and the current choice rows keep working; any other
// strength is stored as percent, 3..99 (a slider's 1 and 2 percent round to 3).
static GLint defaultDepthFormat(); // the window depth buffer's own format, for depth copies
inline float effectStrength(int stored) {
    return stored <= 0 ? 0.f : stored <= 2 ? 0.5f * float(stored) : float((std::min)(stored, 100)) / 100.f;
}
inline int effectSetting(float strength) {
    strength = std::clamp(strength, 0.f, 1.f);
    if (strength <= 0.f) return 0;
    if (std::abs(strength - 0.5f) < 0.005f) return 1;
    if (strength >= 0.995f) return 2;
    return std::clamp(int(std::lround(strength * 100.f)), 3, 99);
}
// Between an effect's old Subtle (0.5) and Strong (1) look; below Subtle it fades to nothing at 0.
inline float effectScale(float strength, float subtle, float strong) {
    return strength < 0.5f ? subtle * strength * 2.f : subtle + (strong - subtle) * (strength - 0.5f) * 2.f;
}

namespace glc {
constexpr GLenum fragmentShader = 0x8B30, vertexShader = 0x8B31, compileStatus = 0x8B81, linkStatus = 0x8B82;
constexpr GLenum texture0 = 0x84C0, texture1 = 0x84C1, texture2 = 0x84C2, texture3 = 0x84C3;
constexpr GLenum activeTexture = 0x84E0, textureBinding2D = 0x8069;
constexpr GLenum samplerBinding = 0x8919, currentProgram = 0x8B8D, vertexArrayBinding = 0x85B5;
constexpr GLenum arrayBuffer = 0x8892, arrayBufferBinding = 0x8894, staticDraw = 0x88E4, streamRead = 0x88E1;
constexpr GLenum elementArrayBuffer = 0x8893, dynamicDraw = 0x88E8; // the VR hand stream
constexpr GLenum pixelPackBuffer = 0x88EB, pixelPackBufferBinding = 0x88ED;
constexpr GLenum packRowLength = 0x0D02, packSkipRows = 0x0D03, packSkipPixels = 0x0D04;
constexpr GLenum drawFramebufferBinding = 0x8CA6, readFramebufferBinding = 0x8CAA, readBuffer = 0x0C02, back = 0x0405;
constexpr GLenum sampleBuffers = 0x80A8, maxTextureImageUnits = 0x8872, depthRange = 0x0B70;
constexpr GLenum polygonMode = 0x0B40, frontAndBack = 0x0408, front = 0x0404, backFace = 0x0405, fill = 0x1B02;
constexpr GLenum framebufferSrgb = 0x8DB9, rasterizerDiscard = 0x8C89, alphaTest = 0x0BC0, colorLogicOp = 0x0BF2;
constexpr GLenum rgba8 = 0x8058, depthComponent24 = 0x81A6, depthComponent = 0x1902;
constexpr GLenum framebuffer = 0x8D40, backLeft = 0x0402, colorEncoding = 0x8210, srgb = 0x8C40;
constexpr GLenum colorAttachment0 = 0x8CE0, framebufferComplete = 0x8CD5, r8 = 0x8229, red = 0x1903;
constexpr GLenum r32f = 0x822E;
constexpr GLenum r16f = 0x822D, rgba16f = 0x881A, rgba32f = 0x8814, readFramebuffer = 0x8CA8, drawFramebuffer = 0x8CA9;
constexpr GLenum depthAttachment = 0x8D00, depthFunc = 0x0B74, depthWritemask = 0x0B72, colorClearValue = 0x0C22;
constexpr GLenum blendSrcRgb = 0x80C9, blendDstRgb = 0x80C8, blendSrcAlpha = 0x80CB, blendDstAlpha = 0x80CA;
constexpr GLenum blendEquationRgb = 0x8009, blendEquationAlpha = 0x883D, funcAdd = 0x8006;
constexpr GLenum minFilter = 0x2801, magFilter = 0x2800, wrapS = 0x2802, wrapT = 0x2803;
constexpr GLenum linear = 0x2601, nearest = 0x2600, clampToEdge = 0x812F;
constexpr GLenum syncGpuCommandsComplete = 0x9117, alreadySignaled = 0x911A, timeoutExpired = 0x911B,
                 conditionSatisfied = 0x911C;
} // namespace glc

// Shaders are GLSL 1.20 so they compile on the game's compatibility context.
// Every fragment program is viewPrelude followed by its own body.
static const char* const viewVertexShader = R"glsl(#version 120
attribute vec2 position;
varying vec2 uv;
void main() {
    uv = position * 0.5 + 0.5;
    gl_Position = vec4(position, 0.0, 1.0);
}
)glsl";

static const char* const viewPrelude = R"glsl(#version 120
uniform sampler2D sceneTex;
uniform sampler2D depthTex;
uniform vec2 texel;        // 1 / viewport size
uniform vec4 projection;   // p0, p5, p8, p9 of the native perspective matrix
uniform vec4 depthLinear;  // view distance = z / (window depth * x + y); w unused
uniform float pixelScale;  // viewport height / 1080, so pixel limits look the same at any resolution
uniform float encoded;     // 1: the stored image is sRGB-encoded (the game renders with sRGB writes)
uniform float frameOffset; // turns the noise each frame where the result is blended over frames, else 0
varying vec2 uv;

const float skyDepth = 0.99999;

float viewDepth(float raw) {
    float d = raw * depthLinear.x + depthLinear.y;
    return abs(d) < 1e-6 ? 1e6 : abs(depthLinear.z / d);
}

vec3 viewPosition(vec2 p, float raw) {
    float d = viewDepth(raw);
    return vec3(d * (p * 2.0 - 1.0 + projection.zw) / projection.xy, -d);
}

// View distance at a texel, with clear depth (sky) treated as very far.
float distanceAt(vec2 p) {
    float raw = texture2D(depthTex, p).r;
    return raw >= skyDepth ? 1e5 : viewDepth(raw);
}

// The occlusion and reflection taps read precomputed view distance from distanceShader
// instead of converting the full-resolution depth for every tap.
uniform sampler2D distanceHalf;
uniform float distanceHalfOn;
float tapDistance(vec2 p) {
    return distanceHalfOn > 0.5 ? texture2D(distanceHalf, p).r : distanceAt(p);
}
vec3 positionFrom(vec2 p, float d) {
    return vec3(d * (p * 2.0 - 1.0 + projection.zw) / projection.xy, -d);
}

vec3 positionAt(vec2 p) {
    return viewPosition(p, texture2D(depthTex, p).r);
}

vec2 project(vec3 p) {
    return (projection.xy * p.xy / -p.z - projection.zw) * 0.5 + 0.5;
}

// Of the two neighbours on each axis, the one nearer in depth lies on the same
// surface, so silhouettes do not bend the normal. Faces the camera.
vec3 viewNormal(vec2 p, vec3 center) {
    vec2 dx = vec2(texel.x, 0.0), dy = vec2(0.0, texel.y);
    vec3 l = positionAt(p - dx), r = positionAt(p + dx);
    vec3 d = positionAt(p - dy), u = positionAt(p + dy);
    vec3 h = abs(l.z - center.z) < abs(r.z - center.z) ? center - l : r - center;
    vec3 v = abs(d.z - center.z) < abs(u.z - center.z) ? center - d : u - center;
    vec3 n = cross(h, v);
    float lengthSq = dot(n, n);
    if (lengthSq < 1e-12) return vec3(0.0, 0.0, 1.0);
    n *= inversesqrt(lengthSq);
    return dot(n, center) > 0.0 ? -n : n;
}

// Light adds only in linear values. With sRGB writes the game stores its image
// encoded, and averaging encoded values dims blurred torches and highlights.
vec3 toLight(vec3 c) {
    return encoded > 0.5 ? pow(c, vec3(2.2)) : c;
}

vec3 toStored(vec3 c) {
    return encoded > 0.5 ? pow(c, vec3(1.0 / 2.2)) : c;
}

// Interleaved gradient noise (Jimenez 2014): a per-pixel rotation that turns
// sample banding into fine, even grain without a noise texture.
float noise() {
    return fract(52.9829189 * fract(dot(gl_FragCoord.xy + frameOffset, vec2(0.06711056, 0.00583715))));
}

// Successive samples on a golden-angle spiral cover a disc evenly at any count.
const mat2 goldenTurn = mat2(-0.7373688, 0.6754903, -0.6754903, -0.7373688);

// A value with every NaN or infinite component replaced by 0: written to a half-float
// history, one bad texel would stay there and spread, a block on screen once upsampled.
float finite(float x) { return x >= -1e20 && x <= 1e20 ? x : 0.0; }
vec4 finite(vec4 v) { return vec4(finite(v.x), finite(v.y), finite(v.z), finite(v.w)); }
)glsl";

// Half resolution: each texel's view distance (sky 1e5), for the occlusion and reflection taps.
static const char* const distanceShader = R"glsl(
void main() {
    gl_FragColor = vec4(distanceAt(uv));
}
)glsl";

// Writes the occlusion in r and, at Strong, the light the
// occluding surfaces bounce onto this one in gba (see compositeShader).
static const char* const aoShader = R"glsl(
uniform int taps;
uniform float bounceOn; // 1: gather one bounce of the surrounding surfaces' light
uniform vec2 halfSize;  // this pass's (and the distance image's) size in texels
const float radius = 50.0; // cm: wide enough to reach into corners and along wall bases

// The texel of the distance image a point reads, at its centre. A tap's distance and the screen point it is
// placed at must be the same texel's: the distance of the nearest texel put at the tap's own point lifts it
// off a flat surface by up to half a texel's depth change, which on floors and walls seen at an angle, and
// more with distance, read as occlusion: a dark, noisy patch over the middle distance of the view.
vec2 distanceCell(vec2 p) {
    return distanceHalfOn > 0.5 ? (floor(p * halfSize) + 0.5) / halfSize : p;
}

// A nearby surface facing this one both blocks ambient light and passes on the
// light that falls on it (a torch-lit wall tints the floor beside it).
float occlusion(vec3 origin, vec3 normal, vec2 p, bool gather, inout vec3 bounce) {
    if (any(lessThan(p, vec2(0.0))) || any(greaterThan(p, vec2(1.0)))) return 0.0; // past the screen edge
    p = distanceCell(p);
    float d = tapDistance(p);
    if (d >= 5e4) return 0.0; // sky
    vec3 delta = positionFrom(p, d) - origin;
    float distanceSq = dot(delta, delta);
    if (distanceSq >= radius * radius || distanceSq < 1e-4) return 0.0;
    // The bias keeps flat, tessellated floors from shading themselves.
    float facing = max(dot(normal, delta) * inversesqrt(distanceSq) - 0.1, 0.0);
    float weight = facing * (1.0 - distanceSq / (radius * radius));
    if (gather) bounce += weight * toLight(texture2D(sceneTex, p).rgb);
    return weight;
}

void main() {
    gl_FragColor = vec4(0.0);
    float raw = texture2D(depthTex, uv).r;
    if (raw >= skyDepth) return;
    // The origin from the same distance image as the taps (this pixel is one of its texel centres), so a
    // flat surface's taps and origin lie on one plane.
    float own = tapDistance(uv);
    vec3 p = distanceHalfOn > 0.5 && own < 5e4 ? positionFrom(uv, own) : viewPosition(uv, raw);
    vec3 n = viewNormal(uv, p);
    float footprint = min(max(radius * projection.y * 0.5 / (texel.y * -p.z), 2.0), max(2.0, 32.0 * pixelScale));
    float jitter = noise();
    float turn = jitter * 6.2831853;
    vec2 direction = vec2(cos(turn), sin(turn));
    float sum = 0.0;
    vec3 bounce = vec3(0.0);
    for (int i = 0; i < 16; ++i) {
        if (i >= taps) break;
        float reach = sqrt((float(i) + fract(jitter * 7.0)) / float(taps)) * footprint;
        // Bounce from the three nearest taps only: scattered colour reads cost the most.
        sum += occlusion(p, n, uv + direction * reach * texel, bounceOn > 0.5 && i < 3, bounce);
        direction = goldenTurn * direction;
    }
    // The bounce is stored square-rooted: 8 bits keep the dim light near torches apart.
    sum *= 1.0 - smoothstep(1500.0, 3000.0, -p.z);
    gl_FragColor = vec4(clamp(2.0 * sum / float(taps), 0.0, 1.0), sqrt(clamp(2.0 * bounce / min(float(taps), 3.0), 0.0, 1.0)));
}
)glsl";

// Writes premultiplied reflection colour and its weight.
static const char* const ssrShader = R"glsl(
uniform float strength; // largest reflection weight, 0..1
uniform int taps;       // ray steps
uniform vec4 worldUp;   // world +Y in view space; w unused
const float reach = 350.0; // cm

bool sceneGap(vec3 p, out vec2 at, out float gap) {
    if (p.z > -1.0) return false;
    at = project(p);
    if (any(lessThan(at, vec2(0.0))) || any(greaterThan(at, vec2(1.0)))) return false;
    float d = tapDistance(at);
    gap = d >= 5e4 ? 1e6 : d + p.z; // > 0: the ray is in front of the scene
    return true;
}

void main() {
    gl_FragColor = vec4(0.0);
    float raw = texture2D(depthTex, uv).r;
    if (raw >= skyDepth) return;
    vec3 p = viewPosition(uv, raw);
    vec3 n = viewNormal(uv, p);
    // Floors, steps and tabletops only. A slope test, not a material mask.
    float surface = smoothstep(0.7, 0.92, dot(n, worldUp.xyz));
    if (surface <= 0.0) return;
    vec3 toEye = normalize(-p);
    vec3 ray = reflect(-toEye, n);
    vec3 origin = p + n;
    float jitter = noise();
    float previousT = 0.0, previousGap = 1.0;
    vec2 hitUv = vec2(0.0);
    float hitT = -1.0;
    for (int i = 0; i < 32; ++i) {
        if (i >= taps) break;
        float u = (float(i) + jitter) / float(taps);
        float t = reach * u * u; // dense near the surface, where contact reflections are
        vec2 at;
        float gap;
        if (!sceneGap(origin + ray * t, at, gap)) break;
        if (gap < 0.0 && previousGap >= 0.0) {
            float a = previousT, b = t;
            for (int j = 0; j < 3; ++j) { // an eighth of the step: about a reflection texel
                float m = 0.5 * (a + b);
                vec2 mAt;
                float mGap;
                if (!sceneGap(origin + ray * m, mAt, mGap)) break;
                if (mGap < 0.0) b = m;
                else a = m;
            }
            float bGap;
            sceneGap(origin + ray * b, at, bGap);
            // A thin crossing is a hit; a deep one passed behind a foreground
            // object, so keep marching beyond it.
            if (-bGap < max(3.0, 0.02 * -(origin.z + ray.z * b))) {
                hitUv = at;
                hitT = b;
                break;
            }
        }
        previousT = t;
        previousGap = gap;
    }
    if (hitT < 0.0) return;
    vec2 edge = min(hitUv, 1.0 - hitUv);
    float fade = smoothstep(0.0, 0.1, min(edge.x, edge.y)) * (1.0 - smoothstep(0.6 * reach, reach, hitT));
    float f = 1.0 - clamp(dot(n, toEye), 0.0, 1.0);
    float fresnel = mix(0.35, 1.0, f * f * f * f * f);
    vec3 hit = texture2D(sceneTex, hitUv).rgb;
    hit *= min(1.0, 0.55 / max(dot(hit, vec3(0.2126, 0.7152, 0.0722)), 1e-3));
    // Light sources and lit walls read as reflections; dark surroundings would only
    // dim the floor, so they reflect weakly.
    float bright = smoothstep(0.08, 0.5, dot(hit, vec3(0.2126, 0.7152, 0.0722)));
    float weight = strength * surface * fade * fresnel * mix(0.3, 1.0, bright);
    gl_FragColor = clamp(finite(vec4(hit * weight, weight)), 0.0, 1.0);
}
)glsl";

// Half resolution, after the occlusion or reflection pass: blends this frame's
// result with the earlier frames' at the same surface, found through the
// previous frame's camera. The noise turns every frame, so the blend averages
// away the grain of a few taps.
static const char* const resolveShader = R"glsl(
uniform sampler2D current;       // this frame's effect result
uniform sampler2D history;       // the blend up to the previous frame, filtered
uniform mat4 toPrevious;         // this frame's view space to the previous frame's
uniform vec4 previousProjection; // p0, p5, p8, p9 of the previous frame
uniform vec2 halfSize;           // effect target size in texels
uniform float historyWeight;     // share of the earlier frames; 0 without a usable history
uniform sampler2D previousDepth; // the previous frame's depth, where its history was shaded

void main() {
    vec4 now = finite(texture2D(current, uv));
    // A texel outside the range of all four edge neighbours is a stray hit on something
    // thinner than a texel (the bars of a grate): it is held to their range, or upsampled it
    // shows as a solid square. Lines (a wall base, a crease) keep their neighbours along them.
    vec4 low = vec4(1e20), high = vec4(-1e20);
    for (int i = 0; i < 4; ++i) {
        vec2 away = vec2(i < 2 ? (i == 0 ? 1.0 : -1.0) : 0.0, i < 2 ? 0.0 : (i == 2 ? 1.0 : -1.0));
        vec4 s = finite(texture2D(current, uv + away / halfSize));
        low = min(low, s);
        high = max(high, s);
    }
    now = clamp(now, low, high);
    gl_FragColor = now;
    float raw = texture2D(depthTex, uv).r;
    if (raw >= skyDepth || historyWeight <= 0.0) return;
    vec4 q = toPrevious * vec4(viewPosition(uv, raw), 1.0);
    if (q.z > -1.0) return;
    vec2 before = (previousProjection.xy * q.xy / -q.z - previousProjection.zw) * 0.5 + 0.5;
    if (any(lessThan(before, vec2(0.0))) || any(greaterThan(before, vec2(1.0)))) return;
    // The camera moving explains where this surface was; something that moved
    // itself (a walking figure, a swung door) was elsewhere, so the previous
    // frame shows another distance there and its history would trail.
    float previous = viewDepth(texture2D(previousDepth, before).r);
    if (abs(previous + q.z) > 0.03 * -q.z + 3.0) return;
    // The earlier frames can hold what this one no longer shows (a wall the
    // camera moved past, something that moved): keep them within the range of
    // this frame's neighbourhood (above), so they smooth the grain and nothing else.
    // The four edge neighbours: as tight as the full 3 x 3 for this grain, at half the reads.
    gl_FragColor = clamp(mix(now, clamp(finite(texture2D(history, before)), low, high), historyWeight), 0.0, 1.0);
}
)glsl";

// Applies the occlusion and reflections. Shared by
// the composite and, when nothing between them reads neighbouring pixels, the finish pass.
static const char* const surfaceShading = R"glsl(
uniform sampler2D aoHalf;   // half resolution: occlusion in r, square-rooted bounced light in gba
uniform sampler2D ssrHalf;  // half resolution: reflection colour (premultiplied) and weight
uniform vec2 halfSize;      // effect target size in texels
uniform vec2 ssrSize;       // the reflection target's size in texels (half the above)
uniform float aoStrength;   // largest darkening, 0 when ambient occlusion is off
uniform float ssrOn;        // 1 when reflections ran, else 0
uniform float bounceStrength; // share of the bounced light added, 0 when it was not gathered

vec4 shadeSurfaces(vec4 c) {
    float d = distanceAt(uv);
    // Edge-aware reconstruction: of the four nearest effect texels, those at this
    // pixel's depth count; across a silhouette the other side is ignored.
    vec2 at = uv * halfSize - 0.5;
    vec2 cell = floor(at), f = at - cell;
    float occlusion = 0.0, weights = 0.0;
    vec3 bounce = vec3(0.0);
    vec4 reflection = vec4(0.0);
    for (int i = 0; i < 4; ++i) {
        vec2 corner = vec2(mod(float(i), 2.0), floor(0.5 * float(i)));
        vec2 p = (cell + corner + 0.5) / halfSize;
        vec2 bilinear = mix(1.0 - f, f, corner);
        // Depth tolerance: 2 % of the distance plus 1 cm.
        float w = bilinear.x * bilinear.y * exp(-abs(tapDistance(p) - d) / (0.02 * d + 1.0)) + 1e-5;
        vec4 ao = texture2D(aoHalf, p);
        occlusion += ao.r * w;
        bounce += ao.gba * ao.gba * w;
        weights += w;
    }
    occlusion = finite(occlusion / weights);
    bounce /= weights;
    // Reflections are shaded on a grid half as fine again (ssrSize), upsampled the same way.
    if (ssrOn > 0.0) {
        at = uv * ssrSize - 0.5;
        cell = floor(at);
        f = at - cell;
        weights = 0.0;
        for (int i = 0; i < 4; ++i) {
            vec2 corner = vec2(mod(float(i), 2.0), floor(0.5 * float(i)));
            vec2 p = (cell + corner + 0.5) / ssrSize;
            vec2 bilinear = mix(1.0 - f, f, corner);
            float w = bilinear.x * bilinear.y * exp(-abs(tapDistance(p) - d) / (0.02 * d + 1.0)) + 1e-5;
            reflection += texture2D(ssrHalf, p) * w;
            weights += w;
        }
        reflection = clamp(finite(reflection / weights), 0.0, 1.0);
    }
    if (bounceStrength > 0.0) {
        // The bounced light lands on this surface's own colour: its hue at a mid
        // brightness, as the colour of an unlit corner says little about how bright it is.
        vec3 light = toLight(c.rgb);
        vec3 albedo = clamp(0.5 * light / max(dot(light, vec3(0.2126, 0.7152, 0.0722)), 0.02), 0.0, 1.0);
        c.rgb = toStored(light + bounceStrength * bounce * albedo);
    }
    if (aoStrength > 0.0) {
        // Ambient occlusion removes indirect light only; bright pixels are mostly
        // direct light (torches, highlights), so they keep their brightness.
        float luma = dot(c.rgb, vec3(0.2126, 0.7152, 0.0722));
        c.rgb *= 1.0 - aoStrength * occlusion * (1.0 - smoothstep(0.3, 0.7, luma));
    }
    if (ssrOn > 0.0) {
        float keep = 1.0 - smoothstep(0.35, 0.75, dot(c.rgb, vec3(0.2126, 0.7152, 0.0722)));
        c.rgb = c.rgb * (1.0 - reflection.a * keep) + reflection.rgb * keep;
    }
    return c;
}
)glsl";

static const char* const compositeShader = R"glsl(
void main() {
    gl_FragColor = shadeSurfaces(texture2D(sceneTex, uv));
}
)glsl";

// Depth of field: the lens both of its passes share.
static const char* const dofLens = R"glsl(
uniform float focus;   // in-focus view distance, cm
uniform float maxBlur; // circle of confusion at its largest, full-resolution pixels
uniform float lens;    // view distance, cm, where the blur begins; everything nearer stays sharp

// Only real distance softens: nothing short of lens blurs, so the focused subject,
// the hands and anything in front of them stay sharp. Past it the circle grows
// gently and reaches maxBlur only far away.
float blurAt(float d) {
    float away = max(d - lens, 0.0) / max(d, 1.0);
    return maxBlur * away * away;
}
)glsl";

// Gathers the input image (see dofCompositeShader).
static const char* const dofShader = R"glsl(
uniform int taps;

// Writes the blurred colour and, in alpha, how much of it the composite takes.
void main() {
    vec4 center = texture2D(sceneTex, uv);
    float d = distanceAt(uv);
    float blur = blurAt(d);
    float turn = noise() * 6.2831853;
    vec2 direction = vec2(cos(turn), sin(turn));
    // Blur only grows with distance, so nothing in front of a sharp pixel is blurred
    // over it: a sharp pixel stays the image.
    float radius = blur;
    if (blur < 0.5) {
        gl_FragColor = vec4(center.rgb, 0.0);
        return;
    }
    // Gather in one pass (Gustafsson 2018): each sample counts when its own
    // blur circle covers this pixel, and a sharper surface behind this pixel
    // cannot bleed forward over it.
    vec3 sum = toLight(center.rgb);
    float count = 1.0, front = 0.0;
    // Keep sample density per blurred area: a small circle needs few samples.
    int samples = int(clamp(radius * radius * 0.5, 12.0, float(taps)));
    for (int i = 1; i < 32; ++i) {
        if (i >= samples) break;
        direction = goldenTurn * direction;
        float r = sqrt(float(i) / float(samples)) * radius;
        vec2 p = uv + direction * r * texel;
        float sampleDistance = distanceAt(p);
        float sampleBlur = blurAt(sampleDistance);
        if (sampleDistance > d) sampleBlur = min(sampleBlur, blur * 2.0);
        float weight = smoothstep(r - 0.5, r + 0.5, sampleBlur);
        if (sampleDistance < d) front += weight;
        sum += mix(sum / count, toLight(texture2D(sceneTex, p).rgb), weight);
        count += 1.0;
    }
    float cover = max(smoothstep(0.5, 1.5, blur), clamp(2.0 * front / (count - 1.0), 0.0, 1.0));
    cover *= 1.0 - smoothstep(0.6, 0.9, dot(center.rgb, vec3(0.2126, 0.7152, 0.0722)));
    gl_FragColor = clamp(finite(vec4(toStored(sum / count), cover)), 0.0, 1.0);
}
)glsl";

// Full resolution: what is in focus stays the sharp image; the rest, and the soft
// edge of a blurred object in front of it, takes the filtered blur result.
static const char* const dofCompositeShader = R"glsl(
uniform sampler2D dofHalf; // half resolution, filtered

void main() {
    vec4 c = texture2D(sceneTex, uv);
    vec4 blurred = texture2D(dofHalf, uv); // alpha: how much of the blur this pixel takes
    gl_FragColor = blurred.a <= 0.0 ? c : vec4(mix(c.rgb, blurred.rgb, blurred.a), c.a);
}
)glsl";

// Eye adaptation meter: 32 x 18 cells, each the mean log brightness of 16
// samples across its part of the native image (see collectMeter).
static const char* const meterShader = R"glsl(
uniform vec2 cells; // meter size in cells

void main() {
    float sum = 0.0;
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x) {
            vec3 c = toLight(texture2D(sceneTex, uv + (vec2(float(x), float(y)) - 1.5) / (4.0 * cells)).rgb);
            sum += log2(clamp(dot(c, vec3(0.2126, 0.7152, 0.0722)), 1e-4, 0.5));
        }
    gl_FragColor = vec4(sum / 16.0);
}
)glsl";

// The last pass: eye adaptation, edge smoothing, the third-person ghost and
// the helmet rim, each switched by its own uniform.
static const char* const finishShader = R"glsl(
uniform float smoothEdges; // 0..1: how far to smooth jagged edges
uniform float sharpen;     // 0..1: restore the detail Supersample's averaging softens
uniform float surfacesOn;  // 1: the composite folded in here (shadeSurfaces) instead of its own pass
uniform float dofOn;       // 1: depth of field mixed in here (dofHalf alpha: how much blur) instead of its own pass
uniform sampler2D dofHalf;
uniform float visor;       // darkening at the helmet rim, 0..1; below 0, the helmet itself is hidden
uniform vec4 ghostA;       // view-space capsule end at the eye; w: radius, 0 when off
uniform vec4 ghostB;       // view-space capsule end at the feet; w: the ghost's strength, 1 for the old on
uniform sampler2D silhouette; // 0 where the player's body lies behind something, else 1
uniform float silhouetteOn;   // 1: the mask holds this frame's body; it replaces the capsule shape
uniform vec2 condition;    // x: how hurt, y: how winded right now (breath already applied), 0..1
uniform float exposure;    // eye adaptation: 1 as the game drew it, below 1 just after darkness falls
uniform vec4 heldDetail;   // x: strength, 0 when off
// Per held item or body part (a row each, 40 rows of 16 texels): texels 0-2 the rows taking view
// space to its own space (cm); 3 the view-space direction to the game lights near it, w how sure;
// 4 their colour (rgb, brightest 1) and the item's strength (a, 0 off); 5-8 blood spots in its
// own space, w the amount (fraction) plus wetness in tenths; 9 its wear and dirt (0..1), how many
// blood spots it carries (z), and whether wear and dirt were read (w); 10 what it is (x: 0 held,
// 1 worn, 2 a loose item or placed object).
uniform sampler2D heldData;
// Level surfaces near the camera. levelDetail: x strength (0 off), y and z the distance (cm) over
// which it fades out, w 1 when the mask holds this frame's held items and gear (else it is ignored). levelRow0..2: view space to world space. levelPeople: up to six people near the
// camera, view-space centre (xyz) and 1 in w, whose space (a 50 cm capsule 110 cm up and down) is
// left alone.
uniform vec4 levelDetail;
uniform vec4 levelRow0;
uniform vec4 levelRow1;
uniform vec4 levelRow2;
uniform vec4 levelPeople[6];
// The two strongest game lights near the player: view-space position (xyz), weight 0..1 (w, 0 none);
// and their colour (rgb, brightest 1).
uniform vec4 levelLight[2];
uniform vec4 levelLightColour[2];
uniform sampler3D heldNoise; // tiling smooth noise, 16 units a tile (held-item surfaces)
uniform vec4 worldUp;      // world +Y in view space; w unused

float lumaAt(vec2 p) {
    return dot(texture2D(sceneTex, p).rgb, vec3(0.299, 0.587, 0.114));
}

// Luma contrast finds an edge; a short walk along it finds where the stair
// step ends, and the pixel blends toward its neighbour across the edge by how
// far it sits along that step. The scene copy is linearly filtered, so a
// fractional offset blends.
vec3 smoothed(vec3 c) {
    float m = dot(c, vec3(0.299, 0.587, 0.114));
    float n = lumaAt(uv + vec2(0.0, texel.y)), s = lumaAt(uv - vec2(0.0, texel.y));
    float e = lumaAt(uv + vec2(texel.x, 0.0)), w = lumaAt(uv - vec2(texel.x, 0.0));
    float lo = min(m, min(min(n, s), min(e, w))), hi = max(m, max(max(n, s), max(e, w)));
    float range = hi - lo;
    // Dark dungeons need a low floor; relative contrast keeps bright areas calm.
    if (range < max(0.03, hi * 0.125)) return c;
    float ne = lumaAt(uv + texel), sw = lumaAt(uv - texel);
    float nw = lumaAt(uv + vec2(-texel.x, texel.y)), se = lumaAt(uv + vec2(texel.x, -texel.y));
    float vertical = abs(n + s - 2.0 * m) * 2.0 + abs(ne + se - 2.0 * e) + abs(nw + sw - 2.0 * w);
    float horizontal = abs(e + w - 2.0 * m) * 2.0 + abs(ne + nw - 2.0 * n) + abs(se + sw - 2.0 * s);
    bool edgeIsHorizontal = vertical >= horizontal;
    float side1 = edgeIsHorizontal ? s : w, side2 = edgeIsHorizontal ? n : e;
    float gradient1 = abs(side1 - m), gradient2 = abs(side2 - m);
    vec2 across = edgeIsHorizontal ? vec2(0.0, texel.y) : vec2(texel.x, 0.0);
    vec2 along = edgeIsHorizontal ? vec2(texel.x, 0.0) : vec2(0.0, texel.y);
    float other = gradient1 >= gradient2 ? side1 : side2;
    if (gradient1 >= gradient2) across = -across;
    float edgeLuma = 0.5 * (m + other);
    float scale = 0.25 * max(gradient1, gradient2);
    vec2 start = uv + 0.5 * across;
    vec2 p1 = start - along, p2 = start + along;
    float d1 = 0.0, d2 = 0.0;
    bool done1 = false, done2 = false;
    const float steps[8] = float[8](1.0, 1.0, 1.0, 1.5, 2.0, 2.0, 4.0, 8.0);
    for (int i = 0; i < 8; ++i) {
        if (!done1) d1 = lumaAt(p1) - edgeLuma;
        if (!done2) d2 = lumaAt(p2) - edgeLuma;
        done1 = done1 || abs(d1) >= scale;
        done2 = done2 || abs(d2) >= scale;
        if (done1 && done2) break;
        if (!done1) p1 -= along * steps[i];
        if (!done2) p2 += along * steps[i];
    }
    float to1 = edgeIsHorizontal ? uv.x - p1.x : uv.y - p1.y;
    float to2 = edgeIsHorizontal ? p2.x - uv.x : p2.y - uv.y;
    bool nearer1 = to1 < to2;
    // Only the end whose luma moves the same way as this pixel bounds its step.
    bool correct = ((nearer1 ? d1 : d2) < 0.0) != (m < edgeLuma);
    float offset = correct ? 0.5 - min(to1, to2) / (to1 + to2) : 0.0;
    // Sub-pixel aliasing (thin lines, specks) by the local average contrast.
    float average = (2.0 * (n + s + e + w) + ne + nw + se + sw) / 12.0;
    float sub = clamp(abs(average - m) / range, 0.0, 1.0);
    sub = (-2.0 * sub + 3.0) * sub * sub;
    offset = max(offset, sub * sub * 0.75);
    return texture2D(sceneTex, uv + offset * across).rgb;
}

// Contrast-adaptive sharpening (after AMD's FidelityFX CAS): each pixel moves
// away from its four neighbours by less where they already span a wide range,
// so edges do not ring and flat areas gain no noise.
vec3 sharpened(vec3 c) {
    vec3 n = texture2D(sceneTex, uv + vec2(0.0, texel.y)).rgb, s = texture2D(sceneTex, uv - vec2(0.0, texel.y)).rgb;
    vec3 e = texture2D(sceneTex, uv + vec2(texel.x, 0.0)).rgb, w = texture2D(sceneTex, uv - vec2(texel.x, 0.0)).rgb;
    vec3 lo = min(c, min(min(n, s), min(e, w))), hi = max(c, max(max(n, s), max(e, w)));
    vec3 amount = sqrt(clamp(min(lo, 1.0 - hi) / max(hi, vec3(1e-4)), 0.0, 1.0));
    vec3 weight = amount * (-1.0 / mix(8.0, 5.0, sharpen));
    return clamp((c + (n + s + e + w) * weight) / (1.0 + 4.0 * weight), 0.0, 1.0);
}

// Distance along a camera ray (from the view origin) to a capsule, or -1.
float capsuleHit(vec3 ray, vec3 a, vec3 b, float r) {
    vec3 ba = b - a, oa = -a;
    float baba = dot(ba, ba), bard = dot(ba, ray), baoa = dot(ba, oa);
    float rdoa = dot(ray, oa), oaoa = dot(oa, oa);
    float qa = baba - bard * bard, qb = baba * rdoa - baoa * bard, qc = baba * oaoa - baoa * baoa - r * r * baba;
    float h = qb * qb - qa * qc;
    if (h < 0.0) return -1.0;
    float t = (-qb - sqrt(h)) / qa;
    float y = baoa + t * bard;
    if (y > 0.0 && y < baba) return t;
    vec3 oc = y <= 0.0 ? oa : -b;
    qb = dot(ray, oc);
    qc = dot(oc, oc) - r * r;
    h = qb * qb - qc;
    return h > 0.0 ? -qb - sqrt(h) : -1.0;
}

)glsl" R"glsl(// Held items, re-surfaced by material. The mask replay tags each held part with
// the material its own game texture shows (steel, wood, leather, other); the
// texture itself is smoothed down to a tint and the light on it, and the surface
// is painted again from noise in the item's own space (z along its length), so
// the pattern stays on the item. It only ever scales the light already there:
// an unlit blade stays dark.
// Smooth noise with one feature per unit, from a tiling 3D texture (16 units a tile)
// baked once: one filtered fetch instead of eight hashes, and its mip levels fade
// detail finer than a pixel to its average, so nothing shimmers at a distance.
float solidNoise(vec3 p) {
    return texture3D(heldNoise, p * 0.0625).r;
}

// A drawn (not noise) feature of this wavelength (cm) shows once a pixel is well
// below it, so distant items do not shimmer.
float resolved(float wavelength, float footprint) {
    return clamp(wavelength / footprint * 0.5 - 0.5, 0.0, 1.0);
}

vec4 heldDatum(float slot, float i) {
    return texture2D(heldData, vec2((i + 0.5) / 16.0, (slot + 0.5) / 40.0));
}

// The held item or body part at p, from the mask's G (1 + slot * 6 + material, of 255); -1 for none.
float heldSlotAt(vec2 p) {
    float code = floor(texture2D(silhouette, p).g * 255.0 + 0.5) - 1.0;
    return code < 0.0 || code >= 240.0 ? -1.0 : floor(code / 6.0);
}

float sameItem(vec2 p, float slot) {
    return abs(heldSlotAt(p) - slot) < 0.5 ? 1.0 : 0.0;
}

float saturationOf(vec3 c) {
    float hi = max(c.r, max(c.g, c.b));
    return hi > 1e-4 ? (hi - min(c.r, min(c.g, c.b))) / hi : 0.0;
}

// The item's smoothed colour around uv, from same-item taps only: the texture's
// tint and the light on it, without its blurry detail. Linear values.
vec3 itemAverage(vec2 radius, float slot, float taps) {
    vec3 sum = toLight(texture2D(sceneTex, uv).rgb);
    float weight = 1.0;
    vec2 direction = vec2(0.0, 1.0);
    for (int i = 0; i < 6; ++i) {
        if (float(i) >= taps) break;
        vec2 p = uv + direction * radius * sqrt((float(i) + 0.5) / taps);
        float w = sameItem(p, slot);
        sum += w * toLight(texture2D(sceneTex, p).rgb);
        weight += w;
        direction = goldenTurn * direction;
    }
    return sum / weight;
}

// Steel: brushed along the length, with a few deeper scratches and pits.
// Further out (not closeUp) the brushing and scratches are below a pixel: only the pits remain.
float steelHeight(vec3 q, float footprint, float pitNoise, bool closeUp, out float brush) {
    float pit = smoothstep(0.76, 0.88, pitNoise);
    brush = 0.0;
    if (!closeUp) return -0.7 * pit;
    brush = (solidNoise(vec3(q.x * 9.0, q.y * 9.0, q.z * 0.4)) - 0.5) +
            (solidNoise(vec3(q.x * 3.5, q.y * 3.5, q.z * 0.25) + 7.0) - 0.5);
    float scratch = smoothstep(0.82, 0.93, solidNoise(vec3((q.x + q.z * 0.15) * 5.0, q.y * 5.0, q.z * 0.2) + 19.0));
    return 0.4 * brush - 0.8 * scratch - 0.7 * pit;
}

// Wood: growth rings round the length, fibres along it.
float woodHeight(vec3 q, float footprint, out float grain) {
    float warp = solidNoise(vec3(q.x * 0.8, q.y * 0.8, q.z * 0.12)) * 2.5;
    float rings = 0.5 + 0.5 * sin((length(q.xy) * 9.0 + warp) * 3.1415927);
    float fibre = solidNoise(vec3(q.x * 12.0, q.y * 12.0, q.z * 0.5));
    grain = mix(0.5, rings, resolved(0.35, footprint)) * 0.7 + fibre * 0.3;
    return grain - 0.5;
}

// Leather: a wrap in spiral bands with grooves between, a pebbled grain on top.
float leatherHeight(vec3 q, float footprint, out float groove) {
    float turn = atan(q.y, q.x) / 6.2831853 + q.z / 1.4;
    groove = smoothstep(0.42, 0.5, abs(fract(turn) - 0.5)) * resolved(0.5, footprint);
    float pebble = solidNoise(q * 6.0);
    return 0.5 * pebble - groove;
}

// Cloth or anything else: a fine weave, quilted in 6 cm diamonds (padded armour's
// stitching; on a small item the seams rarely show).
float clothHeight(vec3 q, float footprint, out float seam) {
    vec2 d = vec2(q.x + q.z, q.x - q.z) / 6.0;
    vec2 cell = abs(fract(d) - 0.5);
    seam = (1.0 - smoothstep(0.0, 0.06, min(cell.x, cell.y))) * resolved(1.0, footprint);
    float puff = (1.0 - 4.0 * cell.x * cell.x) * (1.0 - 4.0 * cell.y * cell.y);
    return 0.5 * sin(q.x * 25.0) * sin(q.z * 25.0) * resolved(0.25, footprint) +
           (solidNoise(q * 4.0) - 0.5) * 0.4 + 0.6 * puff * resolved(1.5, footprint) - seam;
}

// Mail: rows of rings 9 mm apart, each row offset by half a ring, laid on the face
// the surface turns toward most (in its own space); gaps between rings are dark.
float mailHeight(vec3 q, vec3 facing, float footprint, out float ring) {
    vec3 a = abs(facing);
    vec2 s = a.x > a.y && a.x > a.z ? q.yz : a.y > a.z ? q.xz : q.xy;
    s /= 0.9;
    s.x += 0.5 * mod(floor(s.y), 2.0);
    float r = length(fract(s) - 0.5);
    ring = mix(0.55, 1.0 - smoothstep(0.07, 0.13, abs(r - 0.33)), resolved(0.9, footprint));
    return ring - 0.5;
}

)glsl" R"glsl(// code: the part's material from its texture, 0 unknown, 1 steel, 2 wood, 3 leather, 4 other, 5 mail.
// rowX..rowZ: the rows taking view space to the item's own space.
vec3 heldSurface(vec3 color, vec4 rowX, vec4 rowY, vec4 rowZ, float slot, float code, float strength, vec4 lightInfo, vec3 lightTint,
                 float maskEdge) {
    float raw = texture2D(depthTex, uv).r;
    if (raw >= skyDepth) return color;
    vec3 p = viewPosition(uv, raw);
    float range = -p.z;
    float footprint = 2.0 * range * texel.y / projection.y; // cm a pixel spans here
    // Distance tiers, by how much surface one pixel covers: the detail fades out between 0.6 and
    // 1 cm a pixel (about 4 to 7 m at 1080p), where its finest features were already gone.
    float reach = 1.0 - smoothstep(0.6, 1.0, footprint);
    if (reach <= 0.0) return color;
    strength *= reach;
    bool closeUp = footprint < 0.25; // within about 2 m: the full treatment
    // Up close the normal from four depth taps (no bending at outlines); further out the
    // quad's own derivatives, which cost nothing.
    vec3 n = closeUp ? viewNormal(uv, p) : normalize(cross(dFdx(p), dFdy(p)));
    if (dot(n, p) > 0.0) n = -n;
    vec3 q = vec3(dot(rowX, vec4(p, 1.0)), dot(rowY, vec4(p, 1.0)), dot(rowZ, vec4(p, 1.0)));
    float pixelsPerCm = 1.0 / max(footprint, 1e-3);
    // One fetch, four independent noises at a 6 mm grain, shared by the pits, wear, dirt, paint
    // chips, grind lines and blood rims below instead of a fetch each.
    vec4 shared = texture3D(heldNoise, (q * 1.6 + 31.0) * 0.0625);
    float slotKind = heldDatum(slot, 10.0).x;
    // The tint and light: 0.6 cm around, at least 2 pixels; the wider average
    // tells this spot's colour from the item's as a whole (up close only).
    vec3 base = itemAverage(texel * clamp(0.6 * pixelsPerCm, 2.0, 10.0), slot, closeUp ? 4.0 : 3.0);
    vec3 wide = closeUp ? itemAverage(texel * clamp(2.5 * pixelsPerCm, 6.0, 40.0), slot, 3.0) : base;
    vec3 light = toLight(color);
    // kind: 0 steel, 1 wood, 2 leather, 3 other, 4 mail. Unknown (the texture could not be read)
    // falls back to this spot's own colour, which torchlight can tint.
    bool warm = base.r > base.b * 1.15 && base.g > base.r * 0.45; // brown, not a strong red
    float kind = code - 1.0;
    if (kind < -0.5)
        kind = saturationOf(base) < 0.25 ? 0.0 : !warm ? 3.0 :
               dot(base, vec3(0.2126, 0.7152, 0.0722)) < 0.04 ? 2.0 : 1.0;
    // A texture mostly steel can still have a wooden grip and the other way round:
    // a spot clearly warmer than the item's average is wood or leather, clearly
    // greyer is steel.
    float warmer = saturationOf(base) - saturationOf(wide);
    if (kind < 0.5 && warmer > 0.12 && warm) kind = dot(base, vec3(0.2126, 0.7152, 0.0722)) <
                                                     dot(wide, vec3(0.2126, 0.7152, 0.0722)) ? 2.0 : 1.0;
    else if (kind > 0.5 && kind < 2.5 && warmer < -0.12) kind = 0.0;
    float albedo = 1.0, h = 0.0, shine = 0.0, sharpness = 8.0, metal = 0.0, painted = 0.0, paintGrain = 0.5;
    if (kind < 0.5) {
        float brush;
        h = steelHeight(q, footprint, shared.r, closeUp, brush);
        albedo = 0.92 + 0.35 * brush;
        shine = 1.6;
        sharpness = 60.0;
        metal = 1.0;
    } else if (kind < 1.5) {
        float grain;
        vec3 wq = q;
        if (slotKind > 1.5) {
            // A door's, chest's or table's planks: the grain runs along whichever of its own axes
            // lies in the surface and points most nearly up, else across its width.
            vec3 nl = normalize(vec3(dot(rowX.xyz, n), dot(rowY.xyz, n), dot(rowZ.xyz, n)));
            vec3 ul = normalize(vec3(dot(rowX.xyz, worldUp.xyz), dot(rowY.xyz, worldUp.xyz), dot(rowZ.xyz, worldUp.xyz)));
            vec3 score = abs(ul) * (1.0 - abs(nl)) + vec3(0.01, 0.0, 0.0);
            wq = score.x > score.y && score.x > score.z ? q.yzx : score.y > score.z ? q.zxy : q;
        }
        h = woodHeight(wq, footprint, grain);
        albedo = 0.7 + 0.6 * grain;
        shine = 0.25;
        sharpness = 12.0;
    } else if (kind < 2.5) {
        float groove;
        h = leatherHeight(q, footprint, groove);
        albedo = (0.95 + 0.25 * h) * (1.0 - 0.45 * groove);
        shine = 0.35;
        sharpness = 16.0;
    } else if (kind > 3.5) {
        float ring;
        h = mailHeight(q, vec3(dot(rowX.xyz, n), dot(rowY.xyz, n), dot(rowZ.xyz, n)), footprint, ring);
        albedo = 0.35 + 0.75 * ring;
        shine = 1.2;
        sharpness = 30.0;
        metal = 1.0;
    } else if (saturationOf(base) > 0.35) {
        // Strongly coloured: paint (a shield's face), brushed on over wood.
        float strokes = solidNoise(vec3(q.x * 1.5, q.y * 6.0, q.z * 1.5) + 53.0);
        h = 0.3 * woodHeight(q, footprint, paintGrain) + 0.2 * (strokes - 0.5);
        albedo = 0.92 + 0.16 * strokes;
        shine = 0.3;
        sharpness = 20.0;
        painted = 1.0;
    } else {
        float seam;
        h = clothHeight(q, footprint, seam);
        albedo = (0.9 + 0.2 * h) * (1.0 - 0.35 * seam);
        shine = 0.1;
    }
    // The height's slope on the surface, from screen derivatives (bump without a normal map).
    h *= 0.25;
    vec3 dpx = dFdx(p), dpy = dFdy(p);
    vec3 r1 = cross(dpy, n), r2 = cross(n, dpx);
    float det = dot(dpx, r1);
    vec3 slope = sign(det) * (dFdx(h) * r1 + dFdy(h) * r2);
    vec3 bumped = abs(det) > 1e-8 ? normalize(abs(det) * n - slope) : n;
    if (dot(bumped, n) < 0.3) bumped = n; // a quad straddling the item's outline
    // The game's own lights closeUp the item when found, else from above and from the
    // viewer's side, the way torches and lanterns sit.
    vec3 lightDir = normalize(mix(normalize(worldUp.xyz * 0.8 + vec3(0.0, 0.0, 0.6)), lightInfo.xyz, lightInfo.w));
    vec3 view = normalize(-p);
    vec3 halfway = normalize(lightDir + view);
    float before = 0.4 + 0.6 * max(dot(n, lightDir), 0.0);
    float after = 0.4 + 0.6 * max(dot(bumped, lightDir), 0.0);
    float shade = clamp(after / before, 0.55, 1.6);
    // Where the normal turns within a pixel a tight highlight would flicker between
    // pixels; widen it there (the variance the pixel's footprint covers).
    vec3 turnRate = fwidth(bumped);
    sharpness /= 1.0 + sharpness * 0.5 * dot(turnRate, turnRate);
    float spec;
    if (metal > 0.5) {
        // Brushed steel: the highlight runs across the brushing, along the item's length.
        vec3 along = normalize(rowZ.xyz);
        float th = dot(along, halfway);
        spec = 0.6 * pow(sqrt(max(1.0 - th * th, 0.0)), sharpness) * max(dot(bumped, lightDir), 0.0) +
               0.5 * pow(max(dot(bumped, halfway), 0.0), sharpness * 3.0);
    } else {
        spec = pow(max(dot(bumped, halfway), 0.0), sharpness);
    }
    // Steel mirrors its surroundings: brighter facing up to the light, darker facing the floor.
    float mirror = metal > 0.5 ? mix(0.7, 1.3, smoothstep(-0.4, 0.7, dot(reflect(-view, bumped), worldUp.xyz))) : 1.0;
    // Worn bevels: the normal turns fast on a convex edge inside the item.
    // Inside the item (its pixel quad all on it, from the mask's own derivative) and convex
    // (the normal opens out along the surface): no extra taps.
    float convex = dot(dFdx(n), dFdx(p)) + dot(dFdy(n), dFdy(p)) > 0.0 ? 1.0 : 0.0;
    float edge = closeUp && maskEdge < 0.5 ? smoothstep(0.12, 0.5, length(fwidth(n))) * convex : 0.0;
    // The texture's own darker blotches stay as grime and wear, faintly.
    float grime = clamp(1.0 + 0.6 * (dot(light, vec3(0.2126, 0.7152, 0.0722)) / max(dot(base, vec3(0.2126, 0.7152, 0.0722)), 1e-4) - 1.0), 0.6, 1.2);
    float luma = dot(base, vec3(0.2126, 0.7152, 0.0722));
    vec3 tint = metal > 0.5 ? mix(vec3(luma), base, 0.6) : base;
    if (painted > 0.5) {
        // Chipped paint: bare wood shows in patches and along worn edges.
        float chip = clamp(smoothstep(0.66, 0.78, shared.r) + 0.8 * edge, 0.0, 1.0);
        tint = mix(tint, luma * vec3(1.3, 0.85, 0.5) * (0.7 + 0.6 * paintGrain), chip);
        edge *= 0.3; // the wood under worn paint does not shine
    }
)glsl" R"glsl(    // Condition: a worn item is scratched, pitted and dull (steel rusts in its pits, leather
    // and wood scuff pale); a dirty one is darker in its hollows and duller. Unread: a little of both.
    vec4 condition = heldDatum(slot, 9.0);
    // Unread: a little of both, more for world objects (old doors and chests, iron fittings rusting).
    float wear = condition.w > 0.5 ? condition.x : slotKind > 1.5 ? 0.55 : 0.2;
    float dirt = condition.w > 0.5 ? condition.y : slotKind > 1.5 ? 0.4 : 0.2;
    float blotch = shared.g;
    if (metal > 0.5) {
        float rust = smoothstep(0.75 - 0.35 * wear, 0.95 - 0.3 * wear, blotch) * smoothstep(0.35, 0.8, wear);
        tint = mix(tint, luma * vec3(1.35, 0.7, 0.4), rust * 0.75);
        albedo *= 1.0 - 0.25 * wear * smoothstep(0.55, 0.8, shared.a);
        shine *= 1.0 - 0.55 * wear - 0.4 * rust;
        mirror = mix(mirror, 1.0, 0.5 * wear);
        edge *= 1.0 + wear;
    } else {
        float scuff = smoothstep(0.7 - 0.3 * wear, 0.9, blotch) * wear;
        tint = mix(tint, mix(tint, vec3(luma) * 1.4, 0.6), scuff);
    }
    float hollow = 1.0 - smoothstep(0.35, 0.65, shared.b);
    tint *= 1.0 - 0.4 * dirt * hollow;
    shine *= 1.0 - 0.5 * dirt;
    float honed = 0.0;
    if (metal > 0.5 && closeUp && slotKind < 0.5) { // held blades only: worn plate and fittings have no cutting edge
        // A sharpened edge: a thin strip along the item's outline, 3.5 mm (at least a pixel
        // and a half) wide, ground bright, with fine grind lines running across it.
        float reach = clamp(0.35 * pixelsPerCm, 1.5, 6.0);
        float outside = 0.0;
        vec2 direction = vec2(1.0, 0.0);
        for (int i = 0; i < 6; ++i) {
            outside += 1.0 - sameItem(uv + direction * texel * reach, slot);
            direction = goldenTurn * direction;
        }
        honed = smoothstep(0.5, 2.0, outside) * resolved(0.3, footprint);
        honed *= 1.0 - wear * smoothstep(0.6, 0.75, shared.g);
        float grind = 0.5 + 0.5 * sin(q.z * 55.0 + q.x * 18.0 + shared.a * 4.0);
        tint = mix(tint, tint * (1.2 + 0.15 * mix(1.0, grind, resolved(0.12, footprint))), honed);
    }
    // Blood from its hits: smeared along the length, ragged at the rim, dark red,
    // glossy while wet and dull brown once dry.
    float blood = 0.0, wet = 0.0;
    for (int k = 0; k < 4; ++k) {
        if (float(k) >= condition.z) break; // condition.z: how many spots this item carries
        vec4 spot = heldDatum(slot, 5.0 + float(k));
        if (spot.w <= 0.0) continue;
        float amount = fract(spot.w), wetness = floor(spot.w) * 0.1;
        vec3 d = q - spot.xyz;
        float spread = length(vec3(d.x, d.y, d.z * 0.35)) + (shared.b - 0.5) * 1.6;
        float radius = 1.2 + 3.0 * amount;
        float here = smoothstep(radius, radius * 0.45, spread) * (0.55 + 0.45 * amount);
        blood = max(blood, here);
        wet = max(wet, here * wetness);
    }
    if (blood > 0.0) {
        tint = mix(tint, luma * mix(vec3(0.75, 0.22, 0.12), vec3(1.7, 0.1, 0.06), wet), blood * 0.9);
        mirror = mix(mirror, 1.0, blood);
        honed *= 1.0 - blood;
    }
    if (slotKind > 1.5 && metal < 0.5) {
        // Where hands go: an object's edges and tops are worn smooth, a little lighter, softly polished.
        float handled = max(edge, smoothstep(0.75, 0.95, dot(n, worldUp.xyz)) * 0.5);
        tint = mix(tint, tint * 1.18, handled * 0.6);
        shine += 0.5 * handled;
    }
    vec3 surface = tint * albedo * shade * mirror * grime;
    // Highlights take the lights' colour (a torch's warmth); bevel wear takes the surface's.
    vec3 sheen = mix(vec3(luma), base, 0.5 * metal);
    surface += sheen * mix(vec3(1.0), lightTint, lightInfo.w) * shine * spec + sheen * (metal > 0.5 ? 1.2 : 0.4) * edge;
    // Wet blood shines in a broad, soft highlight.
    surface += luma * vec3(1.0, 0.85, 0.8) * mix(vec3(1.0), lightTint, lightInfo.w) * 0.8 * wet *
               pow(max(dot(bumped, halfway), 0.0), 30.0);
    // The honed strip catches the light in a thin, sharp line.
    surface += sheen * mix(vec3(1.0), lightTint, lightInfo.w) * 1.5 * honed * pow(max(dot(bumped, halfway), 0.0), 120.0);
    return toStored(clamp(mix(light, surface, strength), 0.0, 1.0));
}

// Walls, floors and ceilings, in world space (they do not move), each only reshaping the light the
// game put there. Up close (to levelDetail.y..z, about 3 m) by material, told from the surface's
// own colour and which way it faces:
// - wood (warm, saturated): fibres;
// - plaster (pale, plain): soft mottling and hairline cracks;
// - metal (dark, cool grey; grates and bars): fine pitting, rust speckle where it is warm;
// - packed dirt and gravel (brownish floors): pebbles and grit;
// - stone (everything else): grain and pits, with the texture's own darker joints read as mortar
//   (sandy and recessed).
// Further out, to about 8 m, one fetch of broad variation (damp, weathering) only, so the near
// detail fades into it instead of popping. Bodies near the camera are skipped by their capsules;
// the mask keeps held items, worn gear and world objects to their own surface.
vec3 levelSurface(vec3 color) {
    float raw = texture2D(depthTex, uv).r;
    if (raw >= skyDepth) return color;
    vec3 p = viewPosition(uv, raw);
    float range = -p.z;
    float far = 1.0 - smoothstep(600.0, 800.0, range);
    if (far <= 0.0 || range < 20.0) return color;
    float near = 1.0 - smoothstep(levelDetail.y, levelDetail.z, range);
    for (int i = 0; i < 6; ++i) {
        vec4 person = levelPeople[i];
        if (person.w < 0.5) continue;
        vec3 a = person.xyz - worldUp.xyz * 110.0, axis = worldUp.xyz * 220.0;
        vec3 nearest = a + axis * clamp(dot(p - a, axis) / dot(axis, axis), 0.0, 1.0);
        if (length(p - nearest) < 50.0) return color;
    }
    vec3 q = vec3(dot(levelRow0, vec4(p, 1.0)), dot(levelRow1, vec4(p, 1.0)), dot(levelRow2, vec4(p, 1.0)));
    vec3 light = toLight(color);
    float luma = dot(light, vec3(0.2126, 0.7152, 0.0722));
    vec4 broad = texture3D(heldNoise, q * (1.0 / 16.0) * 0.08); // 12 cm patches
    float damp = smoothstep(0.58, 0.82, broad.a);
    float weather = 0.94 + 0.12 * broad.r;                      // large, slow variation
    if (near <= 0.0) {
        // The far tier: broad variation only.
        vec3 broadLook = light * weather * (1.0 - 0.18 * damp);
        return toStored(clamp(mix(light, broadLook, levelDetail.x * far * 0.6), 0.0, 1.0));
    }
    float footprint = 2.0 * range * texel.y / projection.y;
    vec3 n = normalize(cross(dFdx(p), dFdy(p)));
    if (dot(n, p) > 0.0) n = -n;
    float up = dot(n, worldUp.xyz);
    float hi = max(light.r, max(light.g, light.b));
    float saturation = hi > 1e-4 ? (hi - min(light.r, min(light.g, light.b))) / hi : 0.0;
    float stored = dot(color, vec3(0.2126, 0.7152, 0.0722));   // as stored (lighting-independent cut-offs below are rough)
    bool warm = light.r > light.b * 1.2 && light.g > light.r * 0.45;
    bool wood = warm && saturation > 0.3 && abs(up) < 0.95;
    bool dirt = !wood && up > 0.7 && warm && saturation > 0.15;
    bool metal = !wood && !dirt && saturation < 0.12 && light.b >= light.r * 0.95 && stored < 0.3;
    bool plaster = !wood && !dirt && !metal && saturation < 0.12 && stored > 0.45;
    vec4 grain = texture3D(heldNoise, q * (1.0 / 16.0) * 2.5);  // 4 mm features
    float h, albedo = 1.0, shine = 0.0;
    vec3 tint = light;
    if (wood) {
        h = (grain.r - 0.5) * 0.6 + (grain.g - 0.5) * 0.4;
        albedo = 0.9 + 0.22 * grain.r;
    } else if (dirt) {
        // Pebbles: the brightest grain points stand proud, grit between them.
        float pebble = smoothstep(0.62, 0.8, grain.b);
        h = 0.8 * pebble + 0.3 * (grain.r - 0.5);
        albedo = (0.85 + 0.25 * pebble) * (0.92 + 0.16 * grain.g);
    } else if (metal) {
        float pit = smoothstep(0.75, 0.88, grain.b);
        float rust = smoothstep(0.6, 0.85, broad.g) * (0.4 + 0.6 * grain.a);
        h = (grain.r - 0.5) * 0.3 - 0.6 * pit;
        albedo = 1.0 - 0.3 * pit;
        tint = mix(light, luma * vec3(1.4, 0.7, 0.4), rust * 0.6);
        shine = 0.6 * (1.0 - rust);
    } else if (plaster) {
        // Mottled, with hairline cracks where the grain runs along a ridge.
        float crack = 1.0 - smoothstep(0.0, 0.03, abs(grain.g - 0.5)) * 1.0;
        crack *= smoothstep(0.55, 0.7, broad.b) * clamp(0.3 / max(footprint, 1e-3), 0.0, 1.0);
        h = (grain.r - 0.5) * 0.25 - 0.5 * crack;
        albedo = (0.95 + 0.08 * grain.r) * (1.0 - 0.35 * crack);
    } else {
        // Stone. A spot darker than its surroundings in the game's own texture is a joint:
        // mortar, sandy and set back.
        vec3 around = 0.5 * (toLight(texture2D(sceneTex, uv + vec2(4.0 * texel.x, 0.0)).rgb) +
                             toLight(texture2D(sceneTex, uv + vec2(0.0, 4.0 * texel.y)).rgb));
        float joint = smoothstep(0.82, 0.6, luma / max(dot(around, vec3(0.2126, 0.7152, 0.0722)), 1e-4));
        float pit = smoothstep(0.78, 0.9, grain.b);
        h = (grain.r - 0.5) * 0.5 - 0.7 * pit * (1.0 - joint) - 0.8 * joint;
        albedo = mix((0.9 + 0.2 * grain.g) * (1.0 - 0.25 * pit), 0.85 + 0.3 * grain.a, joint);
    }
    // The 4 mm grain only where a pixel is finer than it (within a metre or two): coarser pixels each
    // take an unrelated grain value, which read as a darker, noisy oval around the view on the floor
    // in front. Its colour and bump both fade to the plain surface there; the broad look stays.
    float fine = clamp(0.2 / max(footprint, 1e-3) - 0.5, 0.0, 1.0);
    albedo = mix(1.0, albedo, fine);
    albedo *= weather * (1.0 - 0.22 * damp * (wood ? 0.4 : 1.0));
    h *= 0.15 * fine;
    vec3 dpx = dFdx(p), dpy = dFdy(p);
    vec3 r1 = cross(dpy, n), r2 = cross(n, dpx);
    float det = dot(dpx, r1);
    vec3 bumped = abs(det) > 1e-8 ? normalize(abs(det) * n - sign(det) * (dFdx(h) * r1 + dFdy(h) * r2)) : n;
    if (dot(bumped, n) < 0.3) bumped = n;
)glsl" R"glsl(    // Light from the nearest real lights, each from its own position, weighted by how near and
    // strong it is; the fixed top/viewer light fills in where none is found.
    vec3 lightDir = normalize(worldUp.xyz * 0.8 + vec3(0.0, 0.0, 0.6));
    vec3 lightTint = vec3(1.0);
    float found = 0.0;
    vec3 sum = vec3(0.0), tintSum = vec3(0.0);
    for (int i = 0; i < 2; ++i) {
        vec4 lamp = levelLight[i];
        if (lamp.w <= 0.0) continue;
        vec3 toLamp = lamp.xyz - p;
        float d = length(toLamp);
        float w = lamp.w / (1.0 + d * d / 90000.0); // fades over a few metres
        sum += w * toLamp / max(d, 1.0);
        tintSum += w * levelLightColour[i].rgb;
        found += w;
    }
    if (found > 0.0 && dot(sum, sum) > 1e-6) {
        lightDir = normalize(mix(lightDir, normalize(sum), clamp(found * 2.0, 0.0, 1.0)));
        lightTint = mix(vec3(1.0), tintSum / found, clamp(found * 2.0, 0.0, 1.0));
    }
    float shade = clamp((0.4 + 0.6 * max(dot(bumped, lightDir), 0.0)) / (0.4 + 0.6 * max(dot(n, lightDir), 0.0)), 0.6, 1.5);
    vec3 halfway = normalize(lightDir + normalize(-p));
    float sheen = (damp * 0.35 + shine) * pow(max(dot(bumped, halfway), 0.0), metal ? 60.0 : 40.0);
    vec3 detailed = tint * albedo * shade + luma * sheen * lightTint;
    // Near detail fades into the far tier's broad look rather than to nothing.
    vec3 broadLook = light * weather * (1.0 - 0.18 * damp);
    vec3 result = mix(mix(light, broadLook, 0.6), detailed, near);
    return toStored(clamp(mix(light, result, levelDetail.x * far), 0.0, 1.0));
}

void main() {
    vec4 c = texture2D(sceneTex, uv);
    vec3 color = smoothEdges > 0.0 ? mix(c.rgb, smoothed(c.rgb), smoothEdges) : sharpen > 0.0 ? sharpened(c.rgb) : c.rgb;
    // Held items first, re-surfaced from the image as the game drew it: wound marks,
    // depth of field, occlusion and reflections below then fall on the new surface.
    // The mask's code here and whether it changes within this pixel quad, taken where every pixel
    // runs (a derivative inside a branch is undefined).
    float maskCode = floor(texture2D(silhouette, uv).g * 255.0 + 0.5);
    float maskEdge = fwidth(maskCode);
    if (heldDetail.x > 0.0) {
        // G: 1 + slot * 6 + the part's material (0 unknown, 1 steel, 2 wood, 3 leather, 4 other, 5 mail), of 255.
        float code = floor(texture2D(silhouette, uv).g * 255.0 + 0.5) - 1.0;
        if (code >= 0.0 && code < 240.0) {
            float slot = floor(code / 6.0);
            vec4 tint = heldDatum(slot, 4.0);
            if (tint.a > 0.0)
                color = heldSurface(color, heldDatum(slot, 0.0), heldDatum(slot, 1.0), heldDatum(slot, 2.0), slot,
                                    code - 6.0 * slot, tint.a, heldDatum(slot, 3.0), tint.rgb, maskEdge);
        }
    }
    // Level surfaces: pixels no held item, worn gear or world object claims, near the camera.
    if (levelDetail.x > 0.0 && (levelDetail.w < 0.5 || maskCode > 254.5)) color = levelSurface(color);
    // Occlusion and reflections are low-frequency: applied after the edge passes they look
    // the same as before them, and this pass then needs no neighbouring shaded pixels.
    // Sharpened or smoothed first, then blurred where out of focus: blurred areas need neither.
    if (dofOn > 0.5) {
        vec4 blurred = texture2D(dofHalf, uv);
        if (blurred.a > 0.0) color = mix(color, blurred.rgb, blurred.a);
    }
    if (surfacesOn > 0.5) color = shadeSurfaces(vec4(color, c.a)).rgb;
    if (exposure != 1.0) color = toStored(min(toLight(color) * exposure, vec3(1.0)));
    vec4 marks = texture2D(silhouette, uv);
    if (silhouetteOn > 0.5 && marks.r < 0.5 && marks.g > 0.995) {
        // The body's own limbs also hide each other; count only what stands
        // outside the body's capsule, a wall or a pillar, and not a held item
        // showing at this pixel (G, B, A: where each held item shows).
        vec3 scene = viewPosition(uv, min(texture2D(depthTex, uv).r, skyDepth));
        vec3 axis = ghostB.xyz - ghostA.xyz;
        vec3 nearest = ghostA.xyz + axis * clamp(dot(scene - ghostA.xyz, axis) / dot(axis, axis), 0.0, 1.0);
        if (length(scene - nearest) > ghostA.w + 10.0) {
            // A faint body, brighter at its outline so the shape reads against any wall.
            vec2 away = 2.0 * texel;
            float edge = max(max(texture2D(silhouette, uv + vec2(away.x, 0.0)).r, texture2D(silhouette, uv - vec2(away.x, 0.0)).r),
                             max(texture2D(silhouette, uv + vec2(0.0, away.y)).r, texture2D(silhouette, uv - vec2(0.0, away.y)).r));
            color = mix(color, vec3(0.62, 0.72, 0.9), min((0.12 + 0.38 * edge) * ghostB.w, 0.9));
        }
    } else if (silhouetteOn < 0.5 && ghostA.w > 0.0) {
        float raw = texture2D(depthTex, uv).r;
        vec3 scene = viewPosition(uv, min(raw, skyDepth));
        vec3 ray = normalize(scene);
        float t = capsuleHit(ray, ghostA.xyz, ghostB.xyz, ghostA.w);
        // Only where something stands in front of the player: a faint body,
        // brighter at its outline so the shape reads against any wall.
        if (t > 0.0 && length(scene) < t - 15.0) {
            vec3 hit = ray * t;
            vec3 axis = ghostB.xyz - ghostA.xyz;
            vec3 nearest = ghostA.xyz + axis * clamp(dot(hit - ghostA.xyz, axis) / dot(axis, axis), 0.0, 1.0);
            float rim = 1.0 - abs(dot(normalize(hit - nearest), ray));
            color = mix(color, vec3(0.62, 0.72, 0.9), min((0.12 + 0.38 * rim * rim) * ghostB.w, 0.9));
        }
    }
    if (condition.x > 0.0 || condition.y > 0.0) {
        // Wounds drain colour and close in the edges; breathlessness darkens
        // the edges in time with each breath.
        float grey = dot(color, vec3(0.299, 0.587, 0.114));
        color = mix(color, vec3(grey), 0.6 * condition.x);
        vec2 q = (uv - 0.5) * vec2(texel.y / texel.x, 1.0);
        float edge = smoothstep(0.25, 0.9, length(q));
        color *= 1.0 - edge * (0.35 * condition.x + 0.45 * condition.y);
    }
    if (visor < 0.0) {
        // The helmet is hidden, so there is no rim to take the shape of: a soft opening instead.
        vec2 q = (uv - 0.5) * vec2(texel.y / texel.x, 1.0);
        color *= 1.0 + visor * smoothstep(0.42, 0.75, length(q * vec2(0.8, 1.15)));
    }
    if (visor > 0.0) {
        // The helmet the game draws around the eyes shades the view next to it:
        // darken by how much of a disc around this pixel is helmet (anything
        // within helmetReach of the eye), so the shadow takes the real helmet's
        // shape: an open face, a nasal guard, a great helm's slit.
        const float helmetReach = 18.0; // cm
        const float radius = 0.09; // of the view height
        vec2 direction = vec2(0.0, 1.0), aspect = vec2(texel.x / texel.y, 1.0);
        float helmet = 0.0;
        for (int i = 0; i < 8; ++i) { // the shade is soft; eight taps resolve its shape
            vec2 p = uv + direction * sqrt((float(i) + 0.5) / 8.0) * radius * aspect;
            helmet += distanceAt(p) < helmetReach ? 1.0 : 0.0;
            direction = goldenTurn * direction;
        }
        float inside = distanceAt(uv) < helmetReach ? 1.0 : 0.0;
        color *= 1.0 - visor * max(smoothstep(0.0, 0.6, helmet / 8.0), 0.6 * inside);
    }
    gl_FragColor = vec4(color, c.a);
}
)glsl";

// Wound marks (wounds.inl), their own pass before the finishing pass: drawn only over the wounds'
// screen box onto the image the finishing pass will read, each pixel reading and rewriting itself
// (a texture barrier on either side). Kept out of the finishing program, which every frame runs.
static const char* const woundShader = R"glsl(
uniform vec4 worldUp;      // world +Y in view space; w unused
uniform vec4 woundA[12];   // a wound's view-space centre; w: half length, cm (a bruise's radius)
uniform vec4 woundB[12];   // the blade's path, view space; w: kind (1 cut, 2 crush, 3 thrust) + 0.98 x a cut's
                           // wetness or a bruise's age, 0..1
uniform vec4 woundC[12];   // severity 0..1, seed (negative: the skin part is baked into the skin texture), half width (cm),
                           // length of the blood runs (cm)
uniform float woundCount;
uniform vec4 woundRect;    // screen box (uv min xy, max zw) that holds every wound's reach; outside it nothing to do
uniform vec4 dripA[24];    // a drop of blood: view-space point it fell from; w: radius (cm) + 10 x dryness step (0..9)
uniform float dripCount;
uniform vec4 capA[8];      // a flesh cap on a severed limb: view-space centre; w: radius (cm)
uniform vec4 capB[8];      // its facing (view space); w: 0 the stump, 1 the severed end
uniform float capCount;

float woundHash(float x) {
    return fract(sin(x * 12.9898) * 43758.5453);
}

// Wounds (wounds.inl) on the surface where they landed. They multiply the light already there, so a
// wound in shadow stays in shadow; only wet blood adds a little shine, scaled by that same light.
// A severed limb's cap where the view ray meets it in front of the scene: raw flesh around a pale bone
// with its marrow, a rim of skin and fat, lit from above and scaled by the skin's light around it.
// Opaque; 0 coverage elsewhere.
vec4 capAt(vec3 p) {
    vec3 ray = normalize(p);
    for (int k = 0; k < 8; ++k) {
        if (float(k) >= capCount) break;
        vec4 A = capA[k], B = capB[k];
        float facing = dot(ray, B.xyz);
        if (abs(facing) < 1e-3) continue;
        float t = dot(A.xyz, B.xyz) / facing;
        if (t <= 0.0 || t > length(p) + 2.0) continue; // behind the camera, or the scene stands in front
        vec3 hit = ray * t - A.xyz;
        float r = length(hit) / A.w;
        if (r > 1.0) continue;
        vec3 n = facing < 0.0 ? B.xyz : -B.xyz;
        vec3 flesh = mix(vec3(0.45, 0.06, 0.05), vec3(0.32, 0.03, 0.03), smoothstep(0.3, 0.8, sin(hit.x * 1.7) * sin(hit.y * 2.3) * 0.5 + 0.5));
        vec3 c = mix(flesh, vec3(0.78, 0.6, 0.45), smoothstep(0.82, 0.95, r));         // skin and fat at the rim
        c = mix(c, vec3(0.85, 0.8, 0.68), 1.0 - smoothstep(0.24, 0.3, r));             // the bone
        c = mix(c, vec3(0.35, 0.08, 0.06), 1.0 - smoothstep(0.09, 0.13, r));           // its marrow
        vec3 side = normalize(cross(B.xyz, abs(B.y) < 0.9 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0)));
        float around = dot(toLight(texture2D(sceneTex, project(A.xyz + side * A.w * 1.4)).rgb), vec3(0.2126, 0.7152, 0.0722));
        vec3 lightDir = normalize(worldUp.xyz * 0.8 + vec3(0.0, 0.0, 0.6));
        float shade = 0.35 + 0.65 * max(dot(n, lightDir), 0.0);
        return vec4(toStored(clamp(c * shade * min(around * 2.5 + 0.02, 1.2), 0.0, 1.0)), 1.0);
    }
    return vec4(0.0);
}

vec3 woundMarks(vec3 color) {
    if (uv.x < woundRect.x || uv.y < woundRect.y || uv.x > woundRect.z || uv.y > woundRect.w) return color;
    float raw = texture2D(depthTex, uv).r;
    if (capCount > 0.0) {
        vec4 cap = capAt(viewPosition(uv, min(raw, skyDepth)));
        if (cap.a > 0.0) return cap.rgb;
    }
    if (raw >= skyDepth) return color;
    vec3 p = viewPosition(uv, raw);
    vec3 n = vec3(0.0, 0.0, 1.0);
    bool near = false;
    vec3 tint = vec3(1.0);
    float wet = 0.0;
    float grey = 0.0;
    for (int i = 0; i < 12; ++i) {
        if (float(i) >= woundCount) break;
        vec4 A = woundA[i], B = woundB[i], C = woundC[i];
        vec3 d = p - A.xyz;
        float reach = A.w + C.w + 6.0;
        if (dot(d, d) > reach * reach) continue;
        if (!near) {
            n = viewNormal(uv, p);
            near = true;
            // A grey, unsaturated surface is taken for metal: it takes blood smears and runs but no tear or soak.
            vec3 base = toLight(color);
            float brightest = max(base.r, max(base.g, base.b)), darkest = min(base.r, min(base.g, base.b));
            grey = 1.0 - smoothstep(0.08, 0.2, (brightest - darkest) / max(brightest, 1e-4));
        }
        // The hit point lies on the body, so a surface well in front of it along its normal is an outer
        // layer: cloth, padding or armour (inferred from geometry; a loose collision shape can blur it).
        float off = dot(d, n);
        float layer = smoothstep(0.6, 1.6, off);
        float metal = layer * grey, cloth = layer * (1.0 - grey);
        // Only the surface at the wound: something a few cm off it (another limb, a wall behind) is left alone.
        float stick = 1.0 - smoothstep(2.0 + 2.5 * layer, 4.5 + 2.5 * layer, abs(off));
        if (stick <= 0.0) continue;
        vec3 t = B.xyz - n * dot(B.xyz, n);
        if (dot(t, t) < 1e-4) t = cross(n, abs(n.y) < 0.9 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0));
        t = normalize(t);
        vec3 s = cross(n, t);
        vec2 q = vec2(dot(d, t), dot(d, s)); // cm along the blade's path and across it, on the surface
        float baked = C.y < 0.0 ? 1.0 : 0.0;
        float fracture = step(1.25, C.x);
        float severity = C.x - 1.5 * fracture, seed = abs(C.y);
        float kind = floor(B.w + 0.01), aux = clamp((B.w - kind) / 0.98, 0.0, 1.0), scar = 0.0;
        if (kind > 4.0) { kind -= 4.0; scar = aux; aux = 0.0; } // a cut closing into a scar
        if (kind == 2.0) {
            // A bruise: mottled purple, red-brown where the blow landed hardest, torn there on a heavy one.
            float mottle = 0.78 + 0.22 * sin(q.x * 0.9 + seed * 31.0) * sin(q.y * 1.3 - seed * 17.0);
            float r = length(q) / (A.w * mottle);
            float w = min((1.0 - smoothstep(0.3, 1.0, r)) * (0.5 + 0.4 * severity) * (1.0 + 0.35 * fracture), 1.0) *
                      stick * (1.0 - layer) * (1.0 - baked); // skin only
            // Fresh it is an angry red swelling; over the next minute and a half it darkens to purple. Over a
            // broken bone it is wider and goes deep purple-black at its centre.
            vec3 outer = mix(mix(vec3(0.85, 0.42, 0.4), vec3(0.62, 0.45, 0.62), aux), vec3(0.5, 0.33, 0.5), fracture);
            vec3 inner = mix(mix(vec3(0.6, 0.18, 0.16), vec3(0.42, 0.17, 0.2), aux), vec3(0.26, 0.08, 0.13), fracture);
            vec3 bruise = mix(outer, inner, 1.0 - smoothstep(0.0, 0.7, r));
            float graze = severity > 0.6 ? step(0.8, woundHash(floor(q.x * 2.0) + floor(q.y * 2.0) * 57.0 + seed * 101.0)) *
                                               (1.0 - smoothstep(0.0, 0.45, r)) : 0.0;
            tint *= mix(vec3(1.0), mix(bruise, vec3(0.35, 0.05, 0.04), graze), w);
            continue;
        }
        // A cut, or a thrust's short deep hole: a lens-shaped gash darkest at its depth, swollen lips,
        // blood smeared around it, and fresh blood running down from it.
        float a = q.x / A.w;
        float taper = sqrt(max(1.0 - a * a, 0.0));
        float wobble = 0.22 * C.z * sin(a * 5.0 + seed * 40.0);
        float x = abs(q.y - wobble) / max(C.z * (0.25 + 0.75 * taper), 0.05);
        float inside = taper > 0.0 ? 1.0 - smoothstep(0.6, 1.0, x) : 0.0;
        float lip = taper * smoothstep(0.7, 1.1, x) * (1.0 - smoothstep(1.3, 2.4, x));
        float smear = (1.0 - smoothstep(1.2, 4.0, x)) * (1.0 - smoothstep(0.8, 1.25, abs(a))) *
                      (0.6 + 0.4 * woundHash(floor(q.x * 1.5 + seed * 13.0) + floor(q.y * 1.5) * 31.0));
        float facing = dot(worldUp.xyz, s) * sign(q.y - wobble); // the lip facing up catches the light
        // Wet blood is bright red; dried blood goes dark brown.
        vec3 blood = mix(vec3(0.3, 0.08, 0.05), vec3(0.32, 0.02, 0.018), aux);
        vec3 cut = mix(vec3(1.0), mix(vec3(0.55, 0.3, 0.24), vec3(0.7, 0.22, 0.2), aux), smear * (0.55 + 0.35 * severity));
        cut = mix(cut, vec3(1.0, 0.5, 0.45) * (1.0 + 0.3 * facing), lip * 0.8);
        cut = mix(cut, mix(vec3(0.5, 0.035, 0.03), vec3(0.07, 0.003, 0.003), 1.0 - smoothstep(0.0, 0.8, x)), inside);
        // Through cloth or padding: a ragged slit with frayed threads at its edge and the dark wound behind,
        // and blood soaking outward through the weave, wider the longer the wound bleeds, with a dried rim.
        float rag = 0.35 * (woundHash(floor(q.x * 3.0) + seed * 17.0) - 0.5);
        float xs = abs(q.y - wobble) / max(C.z * 0.7 * taper, 0.05) + rag;
        float slit = taper > 0.0 ? 1.0 - smoothstep(0.5, 0.9, xs) : 0.0;
        float fray = taper * smoothstep(0.8, 1.0, xs) * (1.0 - smoothstep(1.0, 1.9, xs)) *
                     step(0.45, fract(q.x * 2.5 + woundHash(floor(q.x * 2.5) + seed)));
        float spread = A.w * 0.5 + C.z * 2.0 + C.w * 0.45;
        float e = length(vec2(q.x / (A.w * 1.1 + C.w * 0.2), q.y / spread)) *
                  (0.8 + 0.4 * woundHash(floor(q.x * 0.8) + floor(q.y * 0.8) * 13.0 + seed * 7.0));
        float soak = (1.0 - smoothstep(0.75, 1.0, e)) * (0.5 + 0.5 * severity);
        float tide = smoothstep(0.7, 0.9, e) * (1.0 - smoothstep(0.9, 1.0, e));
        vec3 stain = mix(vec3(0.42, 0.16, 0.12), vec3(0.5, 0.07, 0.06), aux);
        vec3 fabric = mix(vec3(1.0), stain, soak * 0.85);
        fabric = mix(fabric, stain * 0.7, tide * 0.5);
        fabric = mix(fabric, fabric * 0.65, fray);
        fabric = mix(fabric, vec3(0.1, 0.02, 0.02), slit);
        vec3 plate = mix(vec3(1.0), blood * 1.6, smear * 0.6);
        // Healing into a scar: a pale, thin line with a faint pink halo.
        float scarLine = taper * (1.0 - smoothstep(0.25, 0.6, x)), scarHalo = taper * (1.0 - smoothstep(0.6, 2.0, x));
        cut = mix(cut, mix(mix(vec3(1.0), vec3(1.05, 0.9, 0.88), scarHalo * 0.45), vec3(1.14, 1.02, 0.97), scarLine * 0.85), scar);
        inside *= 1.0 - scar;
        cut = mix(cut, vec3(1.0), baked * (1.0 - layer)); // the skin already carries it
        cut = mix(cut, fabric, cloth);
        cut = mix(cut, plate, metal);
        inside = mix(inside, slit, cloth) * (1.0 - metal);
        float run = 0.0;
        vec3 down = n * dot(worldUp.xyz, n) - worldUp.xyz; // straight down, along the surface
        if (C.w > 0.0 && dot(down, down) > 0.04) {
            down = normalize(down);
            vec3 side = cross(n, down);
            float below = dot(d, down), across = dot(d, side);
            float ts = dot(t, side), td = dot(t, down);
            float thick = 0.2 + 0.25 * severity;
            for (int k = 0; k < 3; ++k) {
                float fk = float(k);
                float at = (woundHash(seed * 7.0 + fk) * 2.0 - 1.0) * (abs(ts) * A.w * 0.8 + C.z);
                // Each run leaves the cut where the cut crosses its line; a cut running downhill bleeds from its low end.
                float start = abs(ts) > 0.2 ? clamp(at / ts, -A.w, A.w) * td : A.w * abs(td);
                if (abs(ts) <= 0.2) at = 0.4 * (fk - 1.0) * C.z;
                float len = C.w * (0.45 + 0.55 * woundHash(seed * 3.0 + fk + 5.0));
                float along = below - start;
                float bead = 1.0 + 0.8 * smoothstep(len * 0.7, len, along);
                float line = 1.0 - smoothstep(thick * bead * 0.5, thick * bead, abs(across - at - 0.3 * sin(below * 0.4 + fk)));
                run = max(run, line * step(0.0, along) * (1.0 - smoothstep(len * 0.85, len, along)));
            }
            cut = mix(cut, blood, run * 0.9 * (1.0 - 0.7 * cloth)); // cloth drinks most of it
        }
        tint *= mix(vec3(1.0), cut, stick);
        wet += (inside * (0.5 + 0.5 * severity) + 0.6 * run) * stick * (0.15 + 0.85 * aux) * (1.0 - 0.6 * cloth);
    }
    // Drops of blood on the first upward surface 20..200 cm below where they fell from: a round drop with a
    // softly uneven rim, bright and wet at first, dark brown once dry. Smooth everywhere: no hard thresholds,
    // which flicker as the floor's depth wobbles from frame to frame.
    for (int j = 0; j < 24; ++j) {
        if (float(j) >= dripCount) break;
        vec4 D = dripA[j];
        float dryStep = floor(D.w / 10.0), radius = D.w - 10.0 * dryStep, dry = dryStep / 9.0;
        vec3 d = p - D.xyz;
        float h = dot(d, worldUp.xyz);
        if (h > -20.0 || h < -200.0) continue;
        vec3 spread = d - worldUp.xyz * h;
        float r = length(spread);
        if (r > radius * 1.3) continue;
        if (!near) {
            n = viewNormal(uv, p);
            near = true;
            vec3 base = toLight(color);
            float brightest = max(base.r, max(base.g, base.b)), darkest = min(base.r, min(base.g, base.b));
            grey = 1.0 - smoothstep(0.08, 0.2, (brightest - darkest) / max(brightest, 1e-4));
        }
        if (dot(n, worldUp.xyz) < 0.6) continue;
        float angle = atan(spread.z, spread.x);
        float rim = 1.0 + 0.1 * sin(angle * 5.0 + D.x * 0.37) + 0.05 * sin(angle * 11.0 - D.y * 0.53);
        float m = 1.0 - smoothstep(0.7, 1.0, r / (radius * rim));
        tint *= mix(vec3(1.0), mix(vec3(0.33, 0.025, 0.02), vec3(0.28, 0.1, 0.06), dry), m);
        wet += m * (1.0 - dry) * 0.35; // a little shine: the floor's normal, read from depth, is noisy
    }
    if (!near) return color;
    vec3 lit = toLight(color);
    float level = dot(lit, vec3(0.2126, 0.7152, 0.0722));
    lit *= tint;
    if (wet > 0.0) {
        vec3 lightDir = normalize(worldUp.xyz * 0.8 + vec3(0.0, 0.0, 0.6));
        float spec = pow(max(dot(n, normalize(lightDir + normalize(-p))), 0.0), 24.0);
        lit += vec3(0.9, 0.55, 0.5) * spec * min(wet, 1.0) * level * (0.4 + 2.5 * level);
    }
    return toStored(clamp(lit, 0.0, 1.0));
}

void main() {
    vec4 c = texture2D(sceneTex, uv);
    gl_FragColor = vec4(woundMarks(c.rgb), c.a);
}
)glsl";

// Fire and smoke (fire.inl), their own pass before the finishing pass: drawn only inside the box that
// holds every volume. Each volume is marched front to back between the camera ray's entry into its
// bounding sphere and the scene's depth, so flames end where they meet a wall, the floor or a body.
// Flames emit (a black-body ramp from deep red at the edge to near white at the hot core) and smoke
// absorbs and scatters a little of the flames' light. The noise is a frame fixed to the world, rising
// over time, so the fire does not swim as the camera turns.
static const char* const fireShader = R"glsl(
uniform vec4 fireA[12];   // base centre, view space (cm); w: radius at the base (cm)
uniform vec4 fireB[12];   // base to the flame's top (cm); w: kind (0 fire, 1 a burning body, 2 a blade)
uniform vec4 fireC[12];   // intensity 0..1, seed, smoke 0..1, unused
uniform float fireCount;
uniform vec4 fireRect;    // screen box (uv min xy, max zw); outside it nothing to do
uniform vec4 fireLook;    // x seconds, y march steps, z 1 with smoke, w unused
uniform vec4 fireRight;   // world +x in view space (xyz)
uniform sampler3D fireNoise; // tiling smooth noise, 16 units a tile, four channels

float fireFbm(vec3 p) {
    vec4 a = texture3D(fireNoise, p * 0.0625);
    vec4 b = texture3D(fireNoise, p * 0.137 + 0.31);
    return a.r * 0.5 + b.g * 0.3 + a.b * 0.2;
}

// Deep red, orange, yellow, then white-hot: linear light, brighter than 1 at the core for the bloom.
vec3 fireRamp(float t) {
    t = clamp(t, 0.0, 1.4);
    return vec3(1.6, 0.42, 0.08) * smoothstep(0.02, 0.4, t) + vec3(0.0, 0.62, 0.16) * smoothstep(0.35, 0.8, t) +
           vec3(0.0, 0.18, 0.55) * smoothstep(0.8, 1.3, t);
}

void main() {
    vec4 scene = texture2D(sceneTex, uv);
    if (uv.x < fireRect.x || uv.y < fireRect.y || uv.x > fireRect.z || uv.y > fireRect.w) {
        gl_FragColor = scene;
        return;
    }
    float raw = texture2D(depthTex, uv).r;
    vec3 surface = viewPosition(uv, raw);
    vec3 ray = normalize(vec3((uv * 2.0 - 1.0 + projection.zw) / projection.xy, -1.0));
    float sceneT = raw >= skyDepth ? 1e6 : length(surface);
    vec3 light = vec3(0.0);
    float through = 1.0;   // how much of what lies behind still shows
    float jitter = noise();
    float time = fireLook.x;
    for (int i = 0; i < 12; ++i) {
        if (float(i) >= fireCount || through < 0.02) break;
        vec4 A = fireA[i], B = fireB[i], C = fireC[i];
        float height = length(B.xyz);
        if (height < 1.0) continue;
        vec3 axis = B.xyz / height;
        float kind = B.w, radius = A.w;
        float plume = kind < 0.5 && fireLook.z > 0.5 ? 3.2 : 1.0 + 0.6 * C.z * fireLook.z; // smoke rises above the flames
        float top = height * plume;
        // Bounding sphere of flames and plume.
        vec3 centre = A.xyz + axis * top * 0.5;
        float bound = top * 0.5 + radius * (1.0 + 0.9 * (plume - 1.0));
        float b = dot(ray, centre);
        float disc = b * b - dot(centre, centre) + bound * bound;
        if (disc <= 0.0) continue;
        float s = sqrt(disc);
        float t0 = max(b - s, 0.0), t1 = min(b + s, sceneT);
        if (t1 <= t0) continue;
        // Fewer steps for a far, small volume; the step count never drops below 4.
        float steps = clamp(fireLook.y * clamp(bound * 4.0 / max(t0, 1.0), 0.35, 1.0), 4.0, fireLook.y);
        float dt = (t1 - t0) / steps;
        vec3 side = normalize(fireRight.xyz - axis * dot(fireRight.xyz, axis));
        vec3 depthAxis = cross(axis, side);
        float scale = 1.0 / max(radius * 0.55, 2.0);   // noise features about half the base radius across
        for (int k = 0; k < 24; ++k) {
            if (float(k) >= steps || through < 0.02) break;
            vec3 p = ray * (t0 + (float(k) + jitter) * dt) - A.xyz;
            float h = dot(p, axis) / height;          // 0 at the base, 1 at the flame's top
            vec3 across = p - axis * (h * height);
            float r = length(across) / radius;          // 0 on the axis, 1 at the base's edge
            vec3 q = vec3(dot(across, side), h * height, dot(across, depthAxis)) * scale + C.y;
            // Flames: a teardrop narrowing to the top, licked by noise rising faster than the smoke.
            float lick = fireFbm(q - vec3(0.0, time * 2.6, 0.0));
            float width = pow(clamp(1.0 - h, 0.0, 1.0), kind > 1.5 ? 0.25 : 0.6) * (0.75 + 0.5 * lick);
            float flame = h > -0.05 && h < 1.1 ? smoothstep(0.0, 0.35, width - r) * (1.0 - smoothstep(0.7, 1.08, h + 0.3 * lick)) : 0.0;
            float density = flame * (0.6 + 0.8 * lick);
            if (density > 0.001) {
                float heat = density * (1.15 - h * 0.6) * (1.0 - r * 0.45);
                light += through * fireRamp(heat * 1.25) * density * C.x * dt * 0.045;
                through *= exp(-density * dt * 0.012);
            }
            // Smoke: above the flames, widening as it rises, drifting and thinning out.
            if (fireLook.z > 0.5 && C.z > 0.0 && h > 0.55) {
                float rise = h - 0.55;
                float wide = r / (0.55 + 0.45 * rise);
                float puff = fireFbm(q * 0.55 - vec3(0.4 * sin(time * 0.3 + C.y), time * 1.1, 0.0));
                float smoke = smoothstep(0.35, 0.75, puff + 0.45 - wide) * smoothstep(0.0, 0.5, rise) *
                              (1.0 - smoothstep(plume * 0.55, plume * 0.95, h)) * C.z;
                if (smoke > 0.001) {
                    // Lit from below by the flames, dark grey higher up.
                    vec3 tint = mix(vec3(0.5, 0.2, 0.06) * C.x, vec3(0.05, 0.048, 0.045), smoothstep(0.0, 0.8, rise));
                    float absorb = smoke * dt * 0.012;
                    light += through * tint * absorb;
                    through *= exp(-absorb * 1.6);
                }
            }
        }
    }
    vec3 base = encoded > 0.5 ? pow(scene.rgb, vec3(2.2)) : scene.rgb;
    vec3 c = base * through + light;
    gl_FragColor = vec4(encoded > 0.5 ? pow(c, vec3(1.0 / 2.2)) : c, scene.a);
}
)glsl";

// Uniform names shared by all programs; a program that does not use one
// gets location -1, which glUniform ignores. Never treat -1 as a failure:
// drivers strip uniforms a shader does not read.
enum ViewUniform : unsigned {
    uScene, uDepth, uTexel, uProjection, uDepthLinear, uPixelScale, uEncoded,
    uStrength, uTaps, uWorldUp, uFocus, uMaxBlur, uLens, uSmoothEdges, uVisor, uGhostA, uGhostB, uCondition,
    uAoHalf, uSsrHalf, uHalfSize, uAoStrength, uSsrOn, uBounceOn, uBounceStrength, uSharpen, uPreviousDepth, uSurfacesOn, uSsrSize, uDofOn, uDistanceHalf, uDistanceHalfOn,
    uFrameOffset, uCurrent, uHistory, uToPrevious, uPreviousProjection, uHistoryWeight, uDofHalf, uCells, uExposure,
    uSilhouette, uSilhouetteOn, uHeldDetail, uHeldNoise, uHeldData, uLevelDetail, uLevelRow0, uLevelRow1, uLevelRow2, uLevelPeople, uLevelLight, uLevelLightColour,
    uWoundA, uWoundB, uWoundC, uWoundCount, uWoundRect, uDripA, uDripCount,
    uFireA, uFireB, uFireC, uFireCount, uFireRect, uFireLook, uFireRight, uFireNoise, uCapA, uCapB, uCapCount, viewUniformCount
};
static const char* const viewUniformNames[viewUniformCount] = {
    "sceneTex", "depthTex", "texel", "projection", "depthLinear", "pixelScale", "encoded",
    "strength", "taps", "worldUp", "focus", "maxBlur", "lens", "smoothEdges", "visor", "ghostA", "ghostB", "condition",
    "aoHalf", "ssrHalf", "halfSize", "aoStrength", "ssrOn", "bounceOn", "bounceStrength", "sharpen", "previousDepth", "surfacesOn", "ssrSize", "dofOn", "distanceHalf", "distanceHalfOn",
    "frameOffset", "current", "history", "toPrevious", "previousProjection", "historyWeight", "dofHalf", "cells", "exposure",
    "silhouette", "silhouetteOn", "heldDetail", "heldNoise", "heldData", "levelDetail", "levelRow0", "levelRow1", "levelRow2", "levelPeople", "levelLight", "levelLightColour",
    "woundA", "woundB", "woundC", "woundCount", "woundRect", "dripA", "dripCount",
    "fireA", "fireB", "fireC", "fireCount", "fireRect", "fireLook", "fireRight", "fireNoise", "capA", "capB", "capCount"};
struct ViewProgramUniforms { GLint at[viewUniformCount]; };
enum ViewProgram : unsigned {
    programAo, programSsr, programDof, programFinish, programComposite, programResolve, programDofComposite,
    programMeter, programDistance, programWounds, programFire, programCount
};
static ViewProgramUniforms viewUniforms[8][programCount]; // [cue context][ViewProgram]; cueLock

// Why the effects are off, for first-person-status.json. Render thread writes
// under cueLock; the status writer reads after the flag publishes the text.
enum ViewEffectUnavailable : unsigned {
    viewHookMissing = 1, viewApiMissing = 2, viewTargetsFailed = 4, viewAoFailed = 8, viewSsrFailed = 16,
    viewDofFailed = 32, viewFaulted = 64, viewFinishFailed = 128, viewCompositeFailed = 256, viewResolveFailed = 512,
    viewDofCompositeFailed = 1024, viewMeterFailed = 2048,
    viewDistanceFailed = 4096, viewWoundsFailed = 8192, viewFireFailed = 16384
};
static std::atomic<unsigned> viewEffectsUnavailable{0};
static char viewEffectsReason[256];
static std::atomic<bool> viewEffectsReasonSet{false};
static std::atomic<float> viewEffectsFocus{0.f};
static std::atomic<unsigned> viewEffectsDraws{0};

// Frames between full state audits of the view effects (about two seconds).
constexpr unsigned viewAuditFrames = 120;

// Why the last main-view frame drew no effects ("" when it drew), for first-person-status.json.
static std::atomic<const char*> viewEffectsSkip{""};
static void publishViewTargetSizes(int width, int height, int aoWidth, int aoHeight, int ssrWidth, int ssrHeight,
                                   int dofWidth, int dofHeight, bool low);
static void skipViewEffects(const char* why) {
    viewEffectsSkip.store(why, std::memory_order_relaxed);
    if (why && *why)
        publishViewTargetSizes(0, 0, 0, 0, 0, 0, 0, 0, false);
}

static void noteViewEffectFailure(unsigned bit, const char* what, const char* detail = "") {
    viewEffectsUnavailable.fetch_or(bit, std::memory_order_relaxed);
    if (viewEffectsReasonSet.load(std::memory_order_acquire)) return;
    snprintf(viewEffectsReason, sizeof viewEffectsReason, "%s%s%s", what, *detail ? ": " : "", detail);
    for (char* c = viewEffectsReason; *c; ++c)
        if (*c == '\n' || *c == '\r') *c = ' ';
    viewEffectsReasonSet.store(true, std::memory_order_release);
}

static bool loadViewEffectsApi(gl::Api& g) {
    if (!loadGl(g.uniform1i, "glUniform1i") || !loadGl(g.uniform1f, "glUniform1f") ||
        !loadGl(g.uniform2f, "glUniform2f") || !loadGl(g.uniformMatrix4fv, "glUniformMatrix4fv") ||
        !loadGl(g.activeTexture, "glActiveTexture") ||
        !loadGl(g.getShaderInfoLog, "glGetShaderInfoLog") || !loadGl(g.getProgramInfoLog, "glGetProgramInfoLog") ||
        !loadGl(g.getBufferSubData, "glGetBufferSubData") || !loadGl(g.fenceSync, "glFenceSync") ||
        !loadGl(g.clientWaitSync, "glClientWaitSync") || !loadGl(g.deleteSync, "glDeleteSync") ||
        !loadGl(g.genFramebuffers, "glGenFramebuffers") || !loadGl(g.deleteFramebuffers, "glDeleteFramebuffers") ||
        !loadGl(g.bindFramebuffer, "glBindFramebuffer") ||
        !loadGl(g.framebufferTexture2D, "glFramebufferTexture2D") ||
        !loadGl(g.checkFramebufferStatus, "glCheckFramebufferStatus") ||
        !loadGl(g.blitFramebuffer, "glBlitFramebuffer"))
        return false;
    if (!loadGl(g.bindSampler, "glBindSampler")) g.bindSampler = nullptr;
    if (!loadGl(g.bindRenderbuffer, "glBindRenderbuffer") ||
        !loadGl(g.getRenderbufferParameteriv, "glGetRenderbufferParameteriv"))
        g.bindRenderbuffer = nullptr; // offscreen sampling stays off; the rest is unaffected
    if (!loadGl(g.framebufferRenderbuffer, "glFramebufferRenderbuffer"))
        g.framebufferRenderbuffer = nullptr; // the silhouette copies the sampling target's depth instead
    if (!loadGl(g.genQueries, "glGenQueries") || !loadGl(g.deleteQueries, "glDeleteQueries") ||
        !loadGl(g.queryCounter, "glQueryCounter") || !loadGl(g.getQueryObjectiv, "glGetQueryObjectiv") ||
        !loadGl(g.getQueryObjectui64v, "glGetQueryObjectui64v"))
        g.queryCounter = nullptr; // timings stay off; nothing else depends on them
    if (!loadGl(g.getInteger64v, "glGetInteger64v")) g.getInteger64v = nullptr; // no GPU lag reading
    if (!loadGl(g.getAttachmentParameter, "glGetFramebufferAttachmentParameteriv")) g.getAttachmentParameter = nullptr;
    if (!loadGl(g.uniform4fv, "glUniform4fv")) g.uniform4fv = nullptr; // the wounds stay undrawn
    // OpenGL 1.1 exports on Windows; wglGetProcAddress is not their loader on every ICD.
    g.genTextures = glGenTextures;
    g.deleteTextures = glDeleteTextures;
    g.bindTexture = glBindTexture;
    g.texParameteri = glTexParameteri;
    g.copyTexImage2D = glCopyTexImage2D;
    g.copyTexSubImage2D = glCopyTexSubImage2D;
    return true;
}

static GLuint compileViewShader(gl::Api& g, GLenum type, const char* const* parts, GLsizei count,
                                unsigned failure, const char* name) {
    const GLuint shader = g.createShader(type);
    if (!shader) {
        noteViewEffectFailure(failure, name, "glCreateShader failed");
        return 0;
    }
    g.shaderSource(shader, count, parts, nullptr);
    g.compileShader(shader);
    GLint ok = 0;
    g.getShaderiv(shader, glc::compileStatus, &ok);
    if (!ok) {
        char log[200]{};
        g.getShaderInfoLog(shader, sizeof log, nullptr, log);
        noteViewEffectFailure(failure, name, log);
        g.deleteShader(shader);
        return 0;
    }
    return shader;
}

// shared: an optional part between the prelude and the body (dofLens).
static GLuint buildViewProgram(gl::Api& g, const char* shared, const char* body, unsigned failure, const char* name,
                               ViewProgramUniforms& uniforms) {
    const char* const fragmentParts[] = {viewPrelude, shared ? shared : "", body};
    const GLuint vertex = compileViewShader(g, glc::vertexShader, &viewVertexShader, 1, failure, name);
    const GLuint fragment = vertex ? compileViewShader(g, glc::fragmentShader, fragmentParts, 3, failure, name) : 0;
    GLuint program = fragment ? g.createProgram() : 0;
    if (program) {
        g.attachShader(program, vertex);
        g.attachShader(program, fragment);
        g.bindAttribLocation(program, 0, "position");
        g.linkProgram(program);
        GLint ok = 0;
        g.getProgramiv(program, glc::linkStatus, &ok);
        g.detachShader(program, vertex);
        g.detachShader(program, fragment);
        if (!ok) {
            char log[200]{};
            g.getProgramInfoLog(program, sizeof log, nullptr, log);
            noteViewEffectFailure(failure, name, log);
            g.deleteProgram(program);
            program = 0;
        }
    }
    if (vertex) g.deleteShader(vertex);
    if (fragment) g.deleteShader(fragment);
    if (program)
        for (unsigned i = 0; i < viewUniformCount; ++i)
            uniforms.at[i] = g.getUniformLocation(program, viewUniformNames[i]);
    return program;
}

// Fullscreen quad plus the colour/depth copies, created once per context.
static bool prepareViewTargets(gl::Api& g, fp::CueResources& r) {
    if (r.viewVao && r.viewVbo && r.viewColor && r.viewDepth && r.viewDepthPrevious) return true;
    r.retireView(g);
    drainGl();
    g.genVertexArrays(1, &r.viewVao);
    g.genBuffers(1, &r.viewVbo);
    g.genTextures(1, &r.viewColor);
    g.genTextures(1, &r.viewDepth);
    g.genTextures(1, &r.viewDepthPrevious);
    GLint savedActive = 0, savedTexture = 0, savedVao = 0, savedBuffer = 0;
    g.getIntegerv(glc::activeTexture, &savedActive);
    g.getIntegerv(glc::textureBinding2D, &savedTexture);
    g.getIntegerv(glc::vertexArrayBinding, &savedVao);
    g.getIntegerv(glc::arrayBufferBinding, &savedBuffer);
    if (!r.viewVao || !r.viewVbo || !r.viewColor || !r.viewDepth || !r.viewDepthPrevious ||
        g.getError() != GL_NO_ERROR) {
        r.retireView(g);
        drainGl();
        return false;
    }
    // Sampler state belongs to the texture object; set it once here.
    g.bindTexture(GL_TEXTURE_2D, r.viewColor);
    g.texParameteri(GL_TEXTURE_2D, glc::minFilter, glc::linear);
    g.texParameteri(GL_TEXTURE_2D, glc::magFilter, glc::linear);
    g.texParameteri(GL_TEXTURE_2D, glc::wrapS, glc::clampToEdge);
    g.texParameteri(GL_TEXTURE_2D, glc::wrapT, glc::clampToEdge);
    for (GLuint depth : {r.viewDepth, r.viewDepthPrevious}) {
        g.bindTexture(GL_TEXTURE_2D, depth);
        g.texParameteri(GL_TEXTURE_2D, glc::minFilter, glc::nearest); // depth must not blend across edges
        g.texParameteri(GL_TEXTURE_2D, glc::magFilter, glc::nearest);
        g.texParameteri(GL_TEXTURE_2D, glc::wrapS, glc::clampToEdge);
        g.texParameteri(GL_TEXTURE_2D, glc::wrapT, glc::clampToEdge);
    }
    g.bindTexture(GL_TEXTURE_2D, GLuint(savedTexture));
    g.activeTexture(GLenum(savedActive));
    g.bindVertexArray(r.viewVao);
    g.bindBuffer(glc::arrayBuffer, r.viewVbo);
    const GLfloat quad[8] = {-1.f, -1.f, 1.f, -1.f, -1.f, 1.f, 1.f, 1.f};
    g.bufferData(glc::arrayBuffer, sizeof quad, quad, glc::staticDraw);
    g.vertexAttribPointer(0, 2, GL_FLOAT, 0, 0, nullptr);
    g.enableVertexAttribArray(0);
    g.bindVertexArray(GLuint(savedVao));
    g.bindBuffer(glc::arrayBuffer, GLuint(savedBuffer));
    if (g.getError() != GL_NO_ERROR) {
        r.retireView(g);
        drainGl();
        return false;
    }
    return true;
}

// Native-resolution occlusion, reflection and depth-of-field targets, plus half-float
// occlusion/reflection histories (8 bits would stall a slow blend on a step). Binds on
// unit 2 and leaves the entry framebuffer bound (0 for desktop, a VR eye target otherwise).
static bool prepareHalfTargets(gl::Api& g, fp::CueResources& r, int width, int height, GLuint entryFramebuffer) {
    constexpr bool low = false;
    const int w = width, h = height;
    if (r.halfFbo[0] && r.halfWidth == w && r.halfHeight == h) {
        publishViewTargetSizes(width, height, w, h, r.ssrWidth, r.ssrHeight, r.dofWidth, r.dofHeight, low);
        return true;
    }
    g.activeTexture(glc::texture2);
    r.retireHalf(g);
    g.genTextures(2, r.halfTex);
    g.genFramebuffers(2, r.halfFbo);
    for (unsigned i = 0; i < 2; ++i) {
        g.genTextures(2, r.historyTex[i]);
        g.genFramebuffers(2, r.historyFbo[i]);
    }
    g.genTextures(1, &r.dofTex);
    g.genFramebuffers(1, &r.dofFbo);
    g.genTextures(1, &r.distanceTex);
    g.genFramebuffers(1, &r.distanceFbo);
    const int rw = w, rh = h, dw = w, dh = h;
    enum Size { occlusionSize, reflectionSize, blurSize };
    struct Target { unsigned fbo, texture; GLenum format, layout, type, filter; Size size; };
    const Target targets[8] = {
        // The composite filters the current results by depth itself; the resolve
        // reads a history between texels, where the camera saw it last frame. AO
        // uses only red (bounced light is disabled), so keep its targets single-channel.
        {r.halfFbo[0], r.halfTex[0], glc::r8, glc::red, GL_UNSIGNED_BYTE, glc::nearest, occlusionSize},
        {r.halfFbo[1], r.halfTex[1], glc::rgba8, GL_RGBA, GL_UNSIGNED_BYTE, glc::nearest, reflectionSize},
        {r.historyFbo[0][0], r.historyTex[0][0], glc::r16f, glc::red, GL_FLOAT, glc::linear, occlusionSize},
        {r.historyFbo[0][1], r.historyTex[0][1], glc::r16f, glc::red, GL_FLOAT, glc::linear, occlusionSize},
        {r.historyFbo[1][0], r.historyTex[1][0], glc::rgba16f, GL_RGBA, GL_FLOAT, glc::linear, reflectionSize},
        {r.historyFbo[1][1], r.historyTex[1][1], glc::rgba16f, GL_RGBA, GL_FLOAT, glc::linear, reflectionSize},
        {r.dofFbo, r.dofTex, glc::rgba8, GL_RGBA, GL_UNSIGNED_BYTE, glc::linear, blurSize},
        {r.distanceFbo, r.distanceTex, glc::r32f, glc::red, GL_FLOAT, glc::nearest, occlusionSize}};
    for (const Target& t : targets) {
        g.bindTexture(GL_TEXTURE_2D, t.texture);
        const int tw = t.size == reflectionSize ? rw : t.size == blurSize ? dw : w;
        const int th = t.size == reflectionSize ? rh : t.size == blurSize ? dh : h;
        glTexImage2D(GL_TEXTURE_2D, 0, GLint(t.format), tw, th, 0, t.layout, t.type, nullptr);
        g.texParameteri(GL_TEXTURE_2D, glc::minFilter, GLint(t.filter));
        g.texParameteri(GL_TEXTURE_2D, glc::magFilter, GLint(t.filter));
        g.texParameteri(GL_TEXTURE_2D, glc::wrapS, glc::clampToEdge);
        g.texParameteri(GL_TEXTURE_2D, glc::wrapT, glc::clampToEdge);
        g.bindFramebuffer(glc::framebuffer, t.fbo);
        g.framebufferTexture2D(glc::framebuffer, glc::colorAttachment0, GL_TEXTURE_2D, t.texture, 0);
        if (!t.fbo || !t.texture || g.checkFramebufferStatus(glc::framebuffer) != glc::framebufferComplete) {
            g.bindFramebuffer(glc::framebuffer, entryFramebuffer);
            r.retireHalf(g);
            return false;
        }
    }
    g.bindFramebuffer(glc::framebuffer, entryFramebuffer);
    r.halfWidth = w;
    r.halfHeight = h;
    r.ssrWidth = rw;
    r.ssrHeight = rh;
    r.dofWidth = dw;
    r.dofHeight = dh;
    publishViewTargetSizes(width, height, w, h, rw, rh, dw, dh, low);
    return true;
}

constexpr unsigned viewTextureUnits = 9; // scene, depth, masks, occlusion, reflections, blur, distances, held noise
constexpr unsigned heldNoiseUnit = 7;    // a 3D texture: its binding is saved and restored on its own
constexpr unsigned heldDataUnit = 8;     // the held items' and body parts' per-slot data

// The held-item noise: four independent channels of smooth value noise, 16 lattice
// cells a tile over 32 texels, with every mip level so the GPU fades detail finer
// than a pixel. Made once per context; false when 3D textures are unavailable.
constexpr GLenum texture3D = 0x806F, textureBinding3D = 0x806A, wrapR = 0x8072, linearMipmapLinear = 0x2703,
                 repeatWrap = 0x2901;
using TexImage3D = void(APIENTRY*)(GLenum, GLint, GLint, GLsizei, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*);
static TexImage3D heldTexImage3D = nullptr;
static bool prepareHeldNoise(gl::Api& g, fp::CueResources& r) {
    if (r.heldNoise) return true;
    if (!heldTexImage3D) heldTexImage3D = (TexImage3D)wglGetProcAddress("glTexImage3D");
    if (!heldTexImage3D) return false;
    constexpr int cells = 16, size = 32;
    static unsigned char lattice[4][cells * cells * cells];
    static unsigned char texels[size * size * size * 4];
    unsigned seed = 0x9E3779B9u;
    for (auto& channel : lattice)
        for (unsigned char& v : channel) {
            seed = seed * 1664525u + 1013904223u;
            v = (unsigned char)(seed >> 24);
        }
    auto smooth = [](float t) { return t * t * (3.f - 2.f * t); };
    for (int z = 0; z < size; ++z)
        for (int y = 0; y < size; ++y)
            for (int x = 0; x < size; ++x) {
                const float p[3] = {(x + 0.5f) * cells / size, (y + 0.5f) * cells / size, (z + 0.5f) * cells / size};
                int i[3];
                float f[3];
                for (int a = 0; a < 3; ++a) {
                    i[a] = int(std::floor(p[a]));
                    f[a] = smooth(p[a] - float(i[a]));
                }
                for (int c = 0; c < 4; ++c) {
                    float v = 0.f;
                    for (int corner = 0; corner < 8; ++corner) {
                        float w = 1.f;
                        int at[3];
                        for (int a = 0; a < 3; ++a) {
                            const int bit = (corner >> a) & 1;
                            w *= bit ? f[a] : 1.f - f[a];
                            at[a] = (i[a] + bit) & (cells - 1);
                        }
                        v += w * lattice[c][(at[2] * cells + at[1]) * cells + at[0]];
                    }
                    texels[((z * size + y) * size + x) * 4 + c] = (unsigned char)std::lround(v);
                }
            }
    GLint previous = 0;
    glGetIntegerv(textureBinding3D, &previous);
    g.genTextures(1, &r.heldNoise);
    if (!r.heldNoise) return false;
    g.bindTexture(texture3D, r.heldNoise);
    // Each level a 2 x 2 x 2 average of the one above, made in place.
    for (int level = 0, s = size;; ++level, s /= 2) {
        heldTexImage3D(texture3D, level, GLint(glc::rgba8), s, s, s, 0, GL_RGBA, GL_UNSIGNED_BYTE, texels);
        if (s == 1) break;
        const int h = s / 2;
        for (int z = 0; z < h; ++z)
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < h; ++x)
                    for (int c = 0; c < 4; ++c) {
                        int sum = 0;
                        for (int corner = 0; corner < 8; ++corner)
                            sum += texels[(((2 * z + (corner >> 2 & 1)) * s + 2 * y + (corner >> 1 & 1)) * s + 2 * x + (corner & 1)) * 4 + c];
                        texels[((z * h + y) * h + x) * 4 + c] = (unsigned char)((sum + 4) / 8);
                    }
    }
    g.texParameteri(texture3D, glc::minFilter, linearMipmapLinear);
    g.texParameteri(texture3D, glc::magFilter, glc::linear);
    g.texParameteri(texture3D, glc::wrapS, repeatWrap);
    g.texParameteri(texture3D, glc::wrapT, repeatWrap);
    g.texParameteri(texture3D, wrapR, repeatWrap);
    g.bindTexture(texture3D, GLuint(previous));
    if (glGetError() != GL_NO_ERROR) {
        g.deleteTextures(1, &r.heldNoise);
        r.heldNoise = 0;
        return false;
    }
    return true;
}

static void setViewport(gl::Api& g, int x, int y, int width, int height) {
    if (g.viewportArrays) g.viewportIndexed(0, float(x), float(y), float(width), float(height));
    else glViewport(x, y, width, height);
}

// Depth-of-field focus. A small patch at the screen centre is read back
// through a pixel buffer and consumed a frame later, so the GPU never stalls.
constexpr int focusPatch = 24;
constexpr float focusSelfCm = 20.f, focusHandsCm = 45.f, focusEaseSeconds = 0.6f, focusHoldLog = 0.25f;
static float focusLinear[8][2][3]; // depth constants of the frame each buffer was read in; cueLock

// The player's helmet sits inside focusSelfCm and is never a focus target. Arms
// and weapon fill part of the patch at hand distance; when the world shows
// around them, focus on the world.
static float focusFromPatch(const float* raw, const float* linear) {
    float close[focusPatch * focusPatch], distant[focusPatch * focusPatch];
    unsigned closeCount = 0, distantCount = 0;
    for (int i = 0; i < focusPatch * focusPatch; ++i) {
        if (!(raw[i] < 0.99999f)) continue;
        const float denominator = raw[i] * linear[0] + linear[1];
        const float d = std::abs(denominator) < 1e-6f ? 0.f : std::abs(linear[2] / denominator);
        if (!std::isfinite(d) || d < focusSelfCm) continue;
        if (d < focusHandsCm) close[closeCount++] = d;
        else distant[distantCount++] = d;
    }
    float* pick = distantCount * 10 >= closeCount + distantCount || !closeCount ? distant : close;
    const unsigned count = pick == distant ? distantCount : closeCount;
    if (!count) return 0.f;
    std::nth_element(pick, pick + count / 2, pick + count);
    return pick[count / 2];
}

static void requestFocus(gl::Api& g, CueContext& owner, unsigned contextIndex, const GLint* viewport,
                         const float* linear) {
    auto& r = owner.resources;
    if (!r.focusBuffers[0]) {
        g.genBuffers(2, r.focusBuffers);
        for (unsigned i = 0; i < 2; ++i) {
            g.bindBuffer(glc::pixelPackBuffer, r.focusBuffers[i]);
            g.bufferData(glc::pixelPackBuffer, sizeof(float) * focusPatch * focusPatch, nullptr, glc::streamRead);
        }
    }
    const unsigned slot = owner.focusSlot;
    if (r.focusFences[slot]) return; // still in flight; keep the earlier request
    g.bindBuffer(glc::pixelPackBuffer, r.focusBuffers[slot]);
    glReadPixels(viewport[0] + viewport[2] / 2 - focusPatch / 2, viewport[1] + viewport[3] / 2 - focusPatch / 2,
                 focusPatch, focusPatch, glc::depthComponent, GL_FLOAT, nullptr);
    r.focusFences[slot] = g.fenceSync(glc::syncGpuCommandsComplete, 0);
    memcpy(focusLinear[contextIndex][slot], linear, sizeof focusLinear[contextIndex][slot]);
    owner.focusIssued[slot] = GetTickCount64();
    owner.focusSlot = slot ^ 1;
}

// Depth of field at a strength (0.5 Subtle, 1 Strong): the blur circle far away,
// in 1080p pixels, and the view distance where it begins, past the focus (later when weaker).
static float dofMaxBlur(float strength) { return (std::max)(effectScale(strength, 5.f, 10.f), 0.5f); }
static float dofStart(float strength, float focus) {
    return focus + (std::max)(focus, 400.f) * (1.4f - 0.8f * std::clamp(strength, 0.f, 1.f));
}

// Returns the eased focus distance, or 0 while no readback has completed yet.
// Only the newest completed readback counts; one left over from before depth
// of field was switched off is dropped.
static float collectFocus(gl::Api& g, CueContext& owner, unsigned contextIndex) {
    auto& r = owner.resources;
    const ULONGLONG now = GetTickCount64();
    // After a pause (depth of field off, cursor released, third person) the old
    // focus belongs to another view; wait for a fresh readback instead.
    if (owner.focus > 0.f && now - owner.focusTick > 300) owner.focus = 0.f;
    int newest = -1;
    for (unsigned slot = 0; slot < 2; ++slot) {
        if (!r.focusFences[slot]) continue;
        const GLenum state = g.clientWaitSync(r.focusFences[slot], 0, 0);
        if (state == glc::timeoutExpired) continue;
        g.deleteSync(r.focusFences[slot]); // signalled, or failed and never will be
        r.focusFences[slot] = nullptr;
        if ((state != glc::alreadySignaled && state != glc::conditionSatisfied) ||
            now - owner.focusIssued[slot] > 1000)
            continue;
        if (newest < 0 || owner.focusIssued[slot] > owner.focusIssued[newest]) newest = int(slot);
    }
    if (newest >= 0) {
        float raw[focusPatch * focusPatch];
        g.bindBuffer(glc::pixelPackBuffer, r.focusBuffers[newest]);
        g.getBufferSubData(glc::pixelPackBuffer, 0, sizeof raw, raw);
        const float target = focusFromPatch(raw, focusLinear[contextIndex][newest]);
        if (target > 0.f) {
            if (!(owner.focus > 0.f)) owner.focus = target;
            else {
                // Ease in log distance: a jump from 1 m to 10 m takes as long as 10 m to 100 m.
                // Changes inside focusHoldLog (about 30 %) are ignored, so the focus does not hunt.
                const float seconds = float(now - owner.focusTick) * 0.001f;
                const float follow = 1.f - std::exp(-(std::min)(seconds, 1.f) / focusEaseSeconds);
                const float change = std::log(target) - std::log(owner.focus);
                const float beyond = change > focusHoldLog ? change - focusHoldLog
                                   : change < -focusHoldLog ? change + focusHoldLog : 0.f;
                owner.focus = std::exp(std::log(owner.focus) + beyond * follow);
            }
            owner.focusTick = now;
        }
    }
    viewEffectsFocus.store(owner.focus, std::memory_order_relaxed);
    return owner.focus;
}

// Eye adaptation. The meter is read back through pixel buffers a frame later,
// like the focus. The eye follows the scene slowly into darkness and quickly
// into light; the view shows the difference, so a scene the eye has adapted
// to looks exactly as the game drew it.
constexpr int meterWidth = 32, meterHeight = 18;
constexpr float darkAdaptSeconds = 3.f, lightAdaptSeconds = 0.5f;

// The meter reads a small filtered copy of the image, so a frame that needs no
// other effect copies 128 x 72 pixels instead of the whole view. The meter program
// is current; leaves its source bound on unit 0 and the meter framebuffer bound.
constexpr int meterSourceWidth = meterWidth * 4, meterSourceHeight = meterHeight * 4;
// The oval check: the mean brightness (stops) of the view's middle less its rim, from the meter's 32 x 18
// log-luminance cells, for the native image and for the finished one. Their difference is what the
// effects themselves do to the middle against the edges: a dark middle the game draws shows in the first,
// one the effects add shows in the difference. Eased; in first-person-status.json.
static std::atomic<float> ovalNativeStops{0.f}, ovalFinishedStops{0.f};
static void noteMeterOval(const float* cells, bool finished) {
    float centre = 0.f, rim = 0.f;
    int centres = 0, rims = 0;
    for (int y = 0; y < meterHeight; ++y)
        for (int x = 0; x < meterWidth; ++x) {
            const float c = cells[y * meterWidth + x];
            if (!std::isfinite(c)) continue;
            const float u = (float(x) + .5f) / meterWidth * 2.f - 1.f, v = (float(y) + .5f) / meterHeight * 2.f - 1.f;
            const float r2 = u * u + v * v;
            const float stops = std::clamp(c, -14.f, 1.f);
            if (r2 < 0.16f) { centre += stops; ++centres; }
            else if (r2 > 0.64f) { rim += stops; ++rims; }
        }
    if (!centres || !rims) return;
    std::atomic<float>& eased = finished ? ovalFinishedStops : ovalNativeStops;
    const float now = centre / float(centres) - rim / float(rims), was = eased.load(std::memory_order_relaxed);
    eased.store(was + (now - was) * 0.2f, std::memory_order_relaxed);
}
static bool requestMeter(gl::Api& g, fp::CueResources& r, CueContext& owner, GLuint entryFramebuffer,
                         const GLint* viewport, bool finished = false) {
    if (!r.meterFbo) {
        g.genTextures(1, &r.meterTex);
        g.genFramebuffers(1, &r.meterFbo);
        g.genTextures(1, &r.meterSourceTex);
        g.genFramebuffers(1, &r.meterSourceFbo);
        g.genBuffers(2, r.meterBuffers);
        g.activeTexture(glc::texture2);
        g.bindTexture(GL_TEXTURE_2D, r.meterTex);
        glTexImage2D(GL_TEXTURE_2D, 0, GLint(glc::r16f), meterWidth, meterHeight, 0, glc::red, GL_FLOAT, nullptr);
        g.texParameteri(GL_TEXTURE_2D, glc::minFilter, glc::nearest);
        g.texParameteri(GL_TEXTURE_2D, glc::magFilter, glc::nearest);
        g.bindFramebuffer(glc::framebuffer, r.meterFbo);
        g.framebufferTexture2D(glc::framebuffer, glc::colorAttachment0, GL_TEXTURE_2D, r.meterTex, 0);
        bool complete = g.checkFramebufferStatus(glc::framebuffer) == glc::framebufferComplete;
        g.bindTexture(GL_TEXTURE_2D, r.meterSourceTex);
        glTexImage2D(GL_TEXTURE_2D, 0, GLint(glc::rgba8), meterSourceWidth, meterSourceHeight, 0, GL_RGBA,
                     GL_UNSIGNED_BYTE, nullptr);
        g.texParameteri(GL_TEXTURE_2D, glc::minFilter, glc::linear);
        g.texParameteri(GL_TEXTURE_2D, glc::magFilter, glc::linear);
        g.texParameteri(GL_TEXTURE_2D, glc::wrapS, glc::clampToEdge);
        g.texParameteri(GL_TEXTURE_2D, glc::wrapT, glc::clampToEdge);
        g.bindFramebuffer(glc::framebuffer, r.meterSourceFbo);
        g.framebufferTexture2D(glc::framebuffer, glc::colorAttachment0, GL_TEXTURE_2D, r.meterSourceTex, 0);
        complete = complete && g.checkFramebufferStatus(glc::framebuffer) == glc::framebufferComplete;
        for (unsigned i = 0; i < 2; ++i) {
            g.bindBuffer(glc::pixelPackBuffer, r.meterBuffers[i]);
            g.bufferData(glc::pixelPackBuffer, sizeof(float) * meterWidth * meterHeight, nullptr, glc::streamRead);
        }
        if (!r.meterTex || !r.meterSourceTex || !r.meterBuffers[1] || !complete) {
            r.retireMeter(g);
            return false;
        }
    }
    const unsigned slot = owner.meterSlot;
    if (r.meterFences[slot]) return true; // still in flight; keep the earlier reading
    g.bindFramebuffer(glc::readFramebuffer, entryFramebuffer);
    g.bindFramebuffer(glc::drawFramebuffer, r.meterSourceFbo);
    g.blitFramebuffer(viewport[0], viewport[1], viewport[0] + viewport[2], viewport[1] + viewport[3], 0, 0,
                      meterSourceWidth, meterSourceHeight, GL_COLOR_BUFFER_BIT, glc::linear);
    g.activeTexture(glc::texture0);
    g.bindTexture(GL_TEXTURE_2D, r.meterSourceTex);
    g.bindFramebuffer(glc::framebuffer, r.meterFbo);
    setViewport(g, 0, 0, meterWidth, meterHeight);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    g.bindBuffer(glc::pixelPackBuffer, r.meterBuffers[slot]);
    glReadPixels(0, 0, meterWidth, meterHeight, glc::red, GL_FLOAT, nullptr);
    r.meterFences[slot] = g.fenceSync(glc::syncGpuCommandsComplete, 0);
    owner.meterIssued[slot] = GetTickCount64();
    owner.meterFinal[slot] = finished;
    owner.meterSlot = slot ^ 1;
    return true;
}

// Returns the exposure for this frame; 1 until a reading has arrived.
static float collectMeter(gl::Api& g, fp::CueResources& r, CueContext& owner) {
    const ULONGLONG now = GetTickCount64();
    // After a pause (adaptation off, a menu) the eye starts adapted to whatever shows next.
    if (owner.adapted > 0.f && now - owner.meterTick > 1000) owner.adapted = 0.f;
    int newest = -1;
    for (unsigned slot = 0; slot < 2; ++slot) {
        if (!r.meterFences[slot]) continue;
        const GLenum state = g.clientWaitSync(r.meterFences[slot], 0, 0);
        if (state == glc::timeoutExpired) continue;
        g.deleteSync(r.meterFences[slot]);
        r.meterFences[slot] = nullptr;
        if ((state != glc::alreadySignaled && state != glc::conditionSatisfied) || now - owner.meterIssued[slot] > 1000)
            continue;
        if (newest < 0 || owner.meterIssued[slot] > owner.meterIssued[newest]) newest = int(slot);
    }
    if (newest >= 0) {
        float cells[meterWidth * meterHeight];
        g.bindBuffer(glc::pixelPackBuffer, r.meterBuffers[newest]);
        g.getBufferSubData(glc::pixelPackBuffer, 0, sizeof cells, cells);
        noteMeterOval(cells, owner.meterFinal[newest]);
        if (owner.meterFinal[newest]) return owner.adapted > 0.f ? owner.exposure : 1.f; // the oval check only
        float sum = 0.f;
        for (float c : cells) sum += std::isfinite(c) ? std::clamp(c, -14.f, 1.f) : -14.f;
        const float scene = std::exp2(sum / float(meterWidth * meterHeight));
        if (!(owner.adapted > 0.f)) owner.adapted = scene;
        else {
            const float seconds = (std::min)(float(now - owner.meterTick) * 0.001f, 1.f);
            const float follow = 1.f - std::exp(-seconds / (scene < owner.adapted ? darkAdaptSeconds : lightAdaptSeconds));
            owner.adapted = std::exp2(std::log2(owner.adapted) + (std::log2(scene) - std::log2(owner.adapted)) * follow);
        }
        owner.meterTick = now;
        // Half the difference in stops: a torch going out reads as darkness, not as black.
        owner.exposure = std::clamp(std::sqrt(scene / owner.adapted), 0.6f, 1.6f);
    }
    if (!(owner.adapted > 0.f)) owner.exposure = 1.f;
    return owner.exposure;
}

// GPU time of each view-effect stage, eased over about ten readings, for
// first-person-status.json. Timestamps are read back frames later, never waited for.
enum ViewTimer : unsigned { timerCopies, timerSurfaces, timerComposite, timerDof, timerFinish, viewTimerCount };
static std::atomic<float> viewEffectsGpuMs[viewTimerCount];
// The same stages on the CPU: how long issuing them took. A GPU stage time is the passes' own
// work only while the GPU was already busy when they began (viewGpuLagMs well above 0).
static std::atomic<float> viewEffectsCpuMs[viewTimerCount];
// How far the GPU ran behind the CPU when the effects began: near 0, the GPU had run out of
// work and was waiting on the CPU (its stage times then include that waiting); tens of ms,
// it was busy with queued work. And the whole-frame period as the CPU and the GPU each see it.
static std::atomic<float> viewGpuLagMs{0.f}, viewGpuFrameMs{0.f}, viewCpuFrameMs{0.f};
static std::atomic<int> viewEffectsInputWidth{0}, viewEffectsInputHeight{0};
static std::atomic<int> viewEffectsAoWidth{0}, viewEffectsAoHeight{0};
static std::atomic<int> viewEffectsSsrWidth{0}, viewEffectsSsrHeight{0};
static std::atomic<int> viewEffectsDofWidth{0}, viewEffectsDofHeight{0};
static std::atomic<bool> viewEffectsLowTier{false};
static bool effectsLowTier() { return viewEffectsLowTier.load(std::memory_order_relaxed); }
static void publishViewTargetSizes(int width, int height, int aoWidth, int aoHeight, int ssrWidth, int ssrHeight,
                                   int dofWidth, int dofHeight, bool low) {
    viewEffectsInputWidth.store(width, std::memory_order_relaxed);
    viewEffectsInputHeight.store(height, std::memory_order_relaxed);
    viewEffectsAoWidth.store(aoWidth, std::memory_order_relaxed);
    viewEffectsAoHeight.store(aoHeight, std::memory_order_relaxed);
    viewEffectsSsrWidth.store(ssrWidth, std::memory_order_relaxed);
    viewEffectsSsrHeight.store(ssrHeight, std::memory_order_relaxed);
    viewEffectsDofWidth.store(dofWidth, std::memory_order_relaxed);
    viewEffectsDofHeight.store(dofHeight, std::memory_order_relaxed);
    viewEffectsLowTier.store(low, std::memory_order_relaxed);
}

static unsigned long long cpuNowNs() {
    static LARGE_INTEGER ticksPerSecond{};
    if (!ticksPerSecond.QuadPart) QueryPerformanceFrequency(&ticksPerSecond);
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    const unsigned long long f = static_cast<unsigned long long>(ticksPerSecond.QuadPart);
    const unsigned long long c = static_cast<unsigned long long>(now.QuadPart);
    return f ? c / f * 1000000000ull + c % f * 1000000000ull / f : 0;
}

static void easeMs(std::atomic<float>& value, float ms) {
    const float eased = value.load(std::memory_order_relaxed);
    value.store(eased + (ms - eased) * 0.1f, std::memory_order_relaxed);
}

static void collectViewTimings(gl::Api& g, fp::CueResources& r) {
    for (unsigned slot = 0; slot < 4; ++slot) {
        if (!r.timerIssued[slot]) continue;
        GLint ready = 0;
        g.getQueryObjectiv(r.timerQueries[slot][5], 0x8867 /*QUERY_RESULT_AVAILABLE*/, &ready);
        if (!ready) continue;
        unsigned long long stamps[6]{};
        for (unsigned k = 0; k < 6; ++k) g.getQueryObjectui64v(r.timerQueries[slot][k], 0x8866 /*QUERY_RESULT*/, &stamps[k]);
        r.timerIssued[slot] = false;
        const unsigned long long* cpu = r.timerCpu[slot];
        for (unsigned k = 0; k < viewTimerCount; ++k) {
            easeMs(viewEffectsGpuMs[k], stamps[k + 1] >= stamps[k] ? float(stamps[k + 1] - stamps[k]) * 1e-6f : 0.f);
            easeMs(viewEffectsCpuMs[k], cpu[k + 1] >= cpu[k] ? float(cpu[k + 1] - cpu[k]) * 1e-6f : 0.f);
        }
        if (r.gpuClockKnown) {
            const long long lag = static_cast<long long>(stamps[0]) - (static_cast<long long>(cpu[0]) + r.gpuMinusCpuNs);
            easeMs(viewGpuLagMs, float(double(lag) * 1e-6));
        }
        const unsigned calls = r.timerCall[slot] - r.lastGpuCall;
        if (r.lastGpuStamp && stamps[0] > r.lastGpuStamp && calls > 0 && calls < 64)
            easeMs(viewGpuFrameMs, float(double(stamps[0] - r.lastGpuStamp) * 1e-6 / calls));
        r.lastGpuStamp = stamps[0];
        r.lastGpuCall = r.timerCall[slot];
    }
}

constexpr unsigned viewRetryLimit = 3;
constexpr ULONGLONG viewRetryDelayMs = 5000;
static void failViewTargets(CueContext& owner, const char* why) {
    owner.viewTargetsFailed = true;
    owner.viewRetryAt = GetTickCount64() + viewRetryDelayMs;
    ++owner.viewRetries;
    noteViewEffectFailure(viewTargetsFailed, why);
}

struct ViewFrame {
    GLint viewport[4];
    float texel[2], projection[4], depthLinear[3], pixelScale, worldUp[3];
    bool encoded;
};

using WoundBarrier = void(APIENTRY*)();
static WoundBarrier woundBarrier = nullptr;
static void woundTextureBarrier() {
    if (woundBarrier) woundBarrier();
}
// The wound pass's framebuffer and quad, and the barrier that lets a pixel read and rewrite itself
// (GL 4.5 or NV_texture_barrier); without it the pass stays off.
static bool woundPassReady(gl::Api& g, fp::CueResources& r) {
    if (!woundBarrier && !loadGl(woundBarrier, "glTextureBarrier") && !loadGl(woundBarrier, "glTextureBarrierNV")) {
        woundBarrier = nullptr;
        return false;
    }
    if (!r.woundFbo && g.genFramebuffers) g.genFramebuffers(1, &r.woundFbo);
    if (!r.woundQuadVao) g.genVertexArrays(1, &r.woundQuadVao);
    if (!r.woundQuadVbo) g.genBuffers(1, &r.woundQuadVbo);
    return r.woundFbo && r.woundQuadVao && r.woundQuadVbo;
}

static void setViewUniforms(gl::Api& g, const ViewProgramUniforms& u, const ViewFrame& f) {
    g.uniform1i(u.at[uScene], 0);
    g.uniform1i(u.at[uDepth], 1);
    g.uniform2f(u.at[uTexel], f.texel[0], f.texel[1]);
    g.uniform4f(u.at[uProjection], f.projection[0], f.projection[1], f.projection[2], f.projection[3]);
    g.uniform4f(u.at[uDepthLinear], f.depthLinear[0], f.depthLinear[1], f.depthLinear[2], 0.f);
    g.uniform1f(u.at[uPixelScale], f.pixelScale);
    g.uniform1f(u.at[uEncoded], f.encoded ? 1.f : 0.f);
}

// The second colour target and the two framebuffers that hand the image between
// passes; viewColor already holds this frame's copy at the view's size. Binds on
// the active unit and leaves the entry framebuffer bound. False: fall back to copies.
static bool prepareChainTargets(gl::Api& g, fp::CueResources& r, int width, int height, GLuint entryFramebuffer) {
    if (r.chainFbo[0] && r.chainWidth == width && r.chainHeight == height) return true;
    r.retireChain(g);
    g.genTextures(1, &r.chainTex);
    g.genFramebuffers(2, r.chainFbo);
    if (!r.chainTex || !r.chainFbo[0] || !r.chainFbo[1]) {
        r.retireChain(g);
        return false;
    }
    g.bindTexture(GL_TEXTURE_2D, r.chainTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GLint(glc::rgba8), width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    g.texParameteri(GL_TEXTURE_2D, glc::minFilter, glc::linear);
    g.texParameteri(GL_TEXTURE_2D, glc::magFilter, glc::linear);
    g.texParameteri(GL_TEXTURE_2D, glc::wrapS, glc::clampToEdge);
    g.texParameteri(GL_TEXTURE_2D, glc::wrapT, glc::clampToEdge);
    const GLuint textures[2] = {r.viewColor, r.chainTex};
    bool complete = true;
    for (unsigned i = 0; i < 2; ++i) {
        g.bindFramebuffer(glc::framebuffer, r.chainFbo[i]);
        g.framebufferTexture2D(glc::framebuffer, glc::colorAttachment0, GL_TEXTURE_2D, textures[i], 0);
        complete = complete && g.checkFramebufferStatus(glc::framebuffer) == glc::framebufferComplete;
    }
    g.bindFramebuffer(glc::framebuffer, entryFramebuffer);
    if (!complete || glGetError() != GL_NO_ERROR) {
        r.retireChain(g);
        drainGl();
        return false;
    }
    r.chainWidth = width;
    r.chainHeight = height;
    return true;
}

static void copyViewColor(gl::Api& g, fp::CueResources& r, const GLint* viewport) {
    g.activeTexture(glc::texture0);
    g.bindTexture(GL_TEXTURE_2D, r.viewColor);
    g.copyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, viewport[0], viewport[1], viewport[2], viewport[3]);
}

// The matrices the world pass just drew with. Both world passes leave them in
// the view (+60, +E0); the supersampled pass leaves its last sample's, which
// matches the depth it left in the framebuffer. Native row-major: elements
// 4..6 of the view matrix are world +Y in view space.
struct ViewEffectProjectionSnapshot {
    float p0 = 0.f, p5 = 0.f, p8 = 0.f, p9 = 0.f, p10 = 0.f, p14 = 0.f;
    float worldUpX = 0.f, worldUpY = 1.f, worldUpZ = 0.f;
    float view[16]{}; // places world points (the third-person ghost)
    bool worldUpValid = false;
};

static bool readViewMatrices(uintptr_t view, ViewEffectProjectionSnapshot& out) {
    float m[16], p[16];
    __try {
        memcpy(m, (const void*)(view + field::view::renderView), sizeof m);
        memcpy(p, (const void*)(view + field::view::renderProjection), sizeof p);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    for (float v : p)
        if (!std::isfinite(v)) return false;
    // A standard perspective: anything else (an orthographic or broken view) is not reconstructed.
    if (std::abs(p[0]) < 1e-6f || std::abs(p[5]) < 1e-6f || !(p[10] < 0.f) || !(p[14] < 0.f) ||
        std::abs(p[11] + 1.f) > 0.01f || std::abs(p[15]) > 0.01f)
        return false;
    memcpy(out.view, m, sizeof out.view);
    out.p0 = p[0]; out.p5 = p[5]; out.p8 = p[8]; out.p9 = p[9]; out.p10 = p[10]; out.p14 = p[14];
    const float upLengthSq = m[4] * m[4] + m[5] * m[5] + m[6] * m[6];
    out.worldUpValid = std::isfinite(upLengthSq) && upLengthSq >= 0.96f && upLengthSq <= 1.04f;
    if (out.worldUpValid) {
        const float inverse = 1.f / std::sqrt(upLengthSq);
        out.worldUpX = m[4] * inverse; out.worldUpY = m[5] * inverse; out.worldUpZ = m[6] * inverse;
    }
    return true;
}

// The player as a capsule from just below the eye to above the feet. Without this
// frame's silhouette mask the ghost is this soft body-sized form; with it, the
// capsule only tells the body's own limbs apart from walls.
constexpr float playerRadiusCm = 25.f, playerTopBelowEyeCm = 13.f, playerBottomBelowEyeCm = 135.f;

// A held item as the mask replay saw it: its mesh node's sampled pose (three
// axes then the origin, sector-local) and that sector's origin.
// Slots in the mask's G channel: held items first (the player's, then others' nearest), then body
// parts carrying armour or clothing, as the world pass draws them.
constexpr unsigned heldSlotLimit = 40, heldItemLimit = 8;
// light: world direction from the item to the game lights near it (unit) and how sure
// that is (w, 0 = none found); lightColour: their weighted colour, brightest channel 1.
// blood: the spots the wounds intake put on this mesh (weaponBloodFor), item space; w 0 for none.
// apparel: a body part's armour or clothing (the Armour row's strength), not a held item.
// condition: wear, dirt and quality (0..1) from the item's record, and 1 when they were read.
// object: a loose item or placed object (the World object row's strength).
struct HeldPose { float pose[12]; float sectorOrigin[3]; float light[4]; float lightColour[3]; float blood[4][4]; bool apparel;
                  float condition[4]; bool object; };

struct ViewRequest {
    float ao = 0.f, dof = 0.f, ssr = 0.f, smooth = 0.f, sharpen = 0.f, visor = 0.f; // strengths, 0 off
    bool visorHidden = false, ghost = false;
    float ghostStrength = 0.f, conditionStrength = 0.f;
    // A VR eye: the passes run outside the sampled-views detour on the eye's own target, whose
    // matrices the replay left in the view; the desktop gates below give way to these.
    bool eye = false;
    GLint eyeFramebuffer = 0, eyeWidth = 0, eyeHeight = 0;
    // Offscreen sampling (Supersample with view+206): the world went to the game's sampling
    // target (sceneFramebuffer) and was resolved into the view texture at output size; the
    // passes read and write that texture and take the target's depth scaled to it.
    bool offscreen = false;
    GLuint viewTexture = 0, sceneFramebuffer = 0;
    int outputWidth = 0, outputHeight = 0, sceneWidth = 0, sceneHeight = 0;
    float ghostEye[3]{}, ghostFeet[3]{}; // world space, cm
    float hurt = 0.f, winded = 0.f;      // 0..1
    bool adapt = false;                  // eye adaptation (desktop view only): the finish pass only while adjusting
    bool silhouette = false;             // the ghost takes the mask the camera pass drew this frame
    float detail = 0.f;                  // held-item detail strength; only with a mask slot below
    uint64_t heldSlots = 0;              // bit k: the mask holds slot k this frame
    float armour = 0.f;                  // armour and clothing strength
    float objects = 0.f;                 // loose items' and placed objects' strength
    float level = 0.f;                   // level surfaces' strength, already scaled for the frame rate
    float people[6][3]{};                // people near the camera (sector-local, as their mesh is drawn), for level
    float peopleOrigin[6][3]{};          // and their sectors' origins
    float lamps[2][4]{};                 // the two strongest lights near the player: sector-local position, weight
    float lampOrigin[2][3]{};            // their sectors' origins
    float lampColour[2][3]{};            // their colour, brightest channel 1
    unsigned peopleCount = 0;
    HeldPose held[heldSlotLimit]{};
    WoundUniforms wounds;                // wounds in view, already in this view's space
    FireUniforms fires;                  // fire and smoke volumes in view (fire.inl), this view's space
    bool finish() const {
        return smooth > 0.f || sharpen > 0.f || visor > 0.f || ghost || detail > 0.f || level > 0.f || hurt > 0.f || winded > 0.f ||
               wounds.any() || fires.any();
    }
    bool any() const { return ao > 0.f || dof > 0.f || ssr > 0.f || adapt || finish(); }
};

// World point to the native view space (row-major, row vector times matrix).
static void toView(const float* m, const float* p, float* out) {
    for (int i = 0; i < 3; ++i) out[i] = p[0] * m[i] + p[1] * m[4 + i] + p[2] * m[8 + i] + m[12 + i];
}

// View space to a held item's own space, for the shader (column-major, uploads
// untransposed). Whether the view matrix takes sector-local or global points is
// not established here, so both readings of the item's origin are tried and the
// one that puts the item within reach of the camera wins; false when neither
// does, and the item gets no detail that frame.
static std::atomic<unsigned> heldDetailFrames{0}, heldDetailUnanchored{0};
constexpr float heldReachCm = 600.f;
static bool heldViewFrame(const float* view, const HeldPose& h, float* out) {
    float origin[3], best[3], bestSq = heldReachCm * heldReachCm;
    bool found = false;
    for (int candidate = 0; candidate < 2; ++candidate) {
        for (int i = 0; i < 3; ++i) origin[i] = h.pose[9 + i] + (candidate ? h.sectorOrigin[i] : 0.f);
        float o[3];
        toView(view, origin, o);
        const float sq = o[0] * o[0] + o[1] * o[1] + o[2] * o[2];
        if (std::isfinite(sq) && sq < bestSq) {
            bestSq = sq;
            memcpy(best, o, sizeof best);
            found = true;
        }
    }
    if (!found) return false;
    memset(out, 0, 16 * sizeof(float));
    out[15] = 1.f;
    for (int a = 0; a < 3; ++a) {
        float axis[3];
        for (int i = 0; i < 3; ++i)
            axis[i] = h.pose[a * 3] * view[i] + h.pose[a * 3 + 1] * view[4 + i] + h.pose[a * 3 + 2] * view[8 + i];
        const float lengthSq = axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2];
        if (!std::isfinite(lengthSq) || lengthSq < 1e-8f) return false;
        // Row a of the inverse: a point's coordinate along this axis, in the item's own units.
        for (int j = 0; j < 3; ++j) out[j * 4 + a] = axis[j] / lengthSq;
        out[12 + a] = -(axis[0] * best[0] + axis[1] * best[1] + axis[2] * best[2]) / lengthSq;
    }
    for (int i = 0; i < 16; ++i)
        if (!std::isfinite(out[i])) return false;
    return true;
}

// This frame's view space to the previous frame's: inverse(current) * previous,
// both native row-major view matrices (rotation and translation). The result
// uploads untransposed: GLSL's column-major matrix times a column vector is the
// same product. False for a matrix that is not an invertible affine one.
static bool viewToPrevious(const float* m, const float* previous, float* out) {
    if (std::abs(m[3]) > 1e-4f || std::abs(m[7]) > 1e-4f || std::abs(m[11]) > 1e-4f || std::abs(m[15] - 1.f) > 1e-4f)
        return false;
    const float det = m[0] * (m[5] * m[10] - m[6] * m[9]) - m[1] * (m[4] * m[10] - m[6] * m[8]) +
                      m[2] * (m[4] * m[9] - m[5] * m[8]);
    if (!std::isfinite(det) || std::abs(det) < 1e-6f) return false;
    const float k = 1.f / det;
    float inverse[16] = {
        (m[5] * m[10] - m[6] * m[9]) * k, (m[2] * m[9] - m[1] * m[10]) * k, (m[1] * m[6] - m[2] * m[5]) * k, 0.f,
        (m[6] * m[8] - m[4] * m[10]) * k, (m[0] * m[10] - m[2] * m[8]) * k, (m[2] * m[4] - m[0] * m[6]) * k, 0.f,
        (m[4] * m[9] - m[5] * m[8]) * k,  (m[1] * m[8] - m[0] * m[9]) * k,  (m[0] * m[5] - m[1] * m[4]) * k, 0.f,
        0.f, 0.f, 0.f, 1.f};
    for (int c = 0; c < 3; ++c)
        inverse[12 + c] = -(m[12] * inverse[c] + m[13] * inverse[4 + c] + m[14] * inverse[8 + c]);
    for (int row = 0; row < 4; ++row)
        for (int c = 0; c < 4; ++c) {
            float sum = 0.f;
            for (int i = 0; i < 4; ++i) sum += inverse[row * 4 + i] * previous[i * 4 + c];
            out[row * 4 + c] = sum;
        }
    for (int i = 0; i < 16; ++i)
        if (!std::isfinite(out[i])) return false;
    return true;
}

// Offscreen sampling (ViewRequest::offscreen): a framebuffer of ours drawing into the
// view texture, and a depth texture at the view texture's size in the sampling target's
// own depth format, which a depth blit requires. False with the reason it cannot.
// Leaves the read framebuffer 0 and the draw framebuffer ours; binds on the active unit.
// Its queries make the game's thread wait for the driver, so a target, texture and sizes
// already set up are reused without them until the next audit frame (audit: look again).
static bool prepareOffscreenTargets(gl::Api& g, fp::CueResources& r, const ViewRequest& request, const char*& why,
                                    bool audit) {
    constexpr GLenum renderbuffer = 0x8D41, attachmentType = 0x8CD0, attachmentName = 0x8CD1;
    const unsigned key[6] = {request.sceneFramebuffer, request.viewTexture, unsigned(request.sceneWidth),
                             unsigned(request.sceneHeight), unsigned(request.outputWidth), unsigned(request.outputHeight)};
    if (!audit && r.offscreenFbo && !memcmp(key, r.offscreenKey, sizeof key)) {
        g.bindFramebuffer(glc::framebuffer, r.offscreenFbo);
        return true;
    }
    memset(r.offscreenKey, 0, sizeof r.offscreenKey); // set again only once all is checked
    if (!g.bindRenderbuffer || !g.getRenderbufferParameteriv || !g.getAttachmentParameter) {
        why = "offscreen sampling needs renderbuffer queries (OpenGL 3.0)";
        return false;
    }
    GLint type = 0, name = 0, saved = 0, width = 0, height = 0, format = 0;
    drainGl();
    g.bindFramebuffer(glc::readFramebuffer, request.sceneFramebuffer);
    g.getAttachmentParameter(glc::readFramebuffer, glc::depthAttachment, attachmentType, &type);
    g.getAttachmentParameter(glc::readFramebuffer, glc::depthAttachment, attachmentName, &name);
    g.bindFramebuffer(glc::readFramebuffer, 0);
    if (glGetError() != GL_NO_ERROR || type != GLint(renderbuffer) || name <= 0) {
        why = "the offscreen sampling target has no depth renderbuffer";
        drainGl();
        return false;
    }
    glGetIntegerv(0x8CA7 /*RENDERBUFFER_BINDING*/, &saved);
    g.bindRenderbuffer(renderbuffer, GLuint(name));
    g.getRenderbufferParameteriv(renderbuffer, 0x8D42 /*WIDTH*/, &width);
    g.getRenderbufferParameteriv(renderbuffer, 0x8D43 /*HEIGHT*/, &height);
    g.getRenderbufferParameteriv(renderbuffer, 0x8D44 /*INTERNAL_FORMAT*/, &format);
    g.bindRenderbuffer(renderbuffer, GLuint(saved));
    const int outW = request.outputWidth, outH = request.outputHeight;
    // The samples cover the target's corner of the samples' viewport size: the same view as
    // the texture, at the same or a larger size.
    const int sceneW = request.sceneWidth, sceneH = request.sceneHeight;
    if (glGetError() != GL_NO_ERROR || width < sceneW || height < sceneH || sceneW < outW || sceneH < outH ||
        std::abs(double(sceneW) * outH - double(sceneH) * outW) > double((std::max)(sceneW, sceneH))) {
        why = "the offscreen sampling target does not match the view texture's shape";
        drainGl();
        return false;
    }
    GLenum texelFormat = glc::depthComponent, texelType = GL_UNSIGNED_INT, attachment = glc::depthAttachment;
    switch (format) {
    case 0x81A5: case 0x81A6: case 0x81A7: break;                    // DEPTH_COMPONENT16/24/32
    case 0x8CAC: texelType = GL_FLOAT; break;                         // DEPTH_COMPONENT32F
    case 0x88F0: texelFormat = 0x84F9; texelType = 0x84FA; attachment = 0x821A; break; // DEPTH24_STENCIL8
    default:
        why = "the offscreen sampling target's depth format is not one this copy handles";
        return false;
    }
    // The view's depth copies take the target's format here so the blit lands in them
    // directly (see the copies in drawViewEffectsOwned); the framebuffer only holds them.
    if (!r.offscreenDepthFbo) {
        g.genFramebuffers(1, &r.offscreenDepthFbo);
        g.genFramebuffers(1, &r.offscreenFbo);
        if (!r.offscreenDepthFbo || !r.offscreenFbo) {
            r.retireOffscreen(g);
            why = "could not create the offscreen framebuffers";
            return false;
        }
        g.bindFramebuffer(glc::framebuffer, r.offscreenDepthFbo);
        glDrawBuffer(GL_NONE); // depth only: framebuffer state, complete without a colour image
        glReadBuffer(GL_NONE);
        g.bindFramebuffer(glc::framebuffer, 0);
    }
    r.offscreenDepthFormat = format;
    r.offscreenDepthLayout = texelFormat;
    r.offscreenDepthType = texelType;
    r.offscreenDepthAttachment = attachment;
    r.offscreenSceneWidth = sceneW;
    r.offscreenSceneHeight = sceneH;
    // The view texture can be recreated by the game (a resize); attach it every frame.
    g.bindFramebuffer(glc::framebuffer, r.offscreenFbo);
    g.framebufferTexture2D(glc::framebuffer, glc::colorAttachment0, GL_TEXTURE_2D, request.viewTexture, 0);
    GLint encoding = 0;
    g.getAttachmentParameter(glc::framebuffer, glc::colorAttachment0, glc::colorEncoding, &encoding);
    const bool complete = g.checkFramebufferStatus(glc::framebuffer) == glc::framebufferComplete;
    if (!complete || glGetError() != GL_NO_ERROR) {
        g.bindFramebuffer(glc::framebuffer, 0);
        drainGl();
        why = "the view texture cannot be drawn into";
        return false;
    }
    r.offscreenEncoded = encoding == glc::srgb;
    memcpy(r.offscreenKey, key, sizeof key);
    return true;
}

// cueLock held. Unsupported routing or matrices skip this frame only.
static void drawViewEffectsOwned(uintptr_t view, const ViewRequest& request) {
    ViewEffectProjectionSnapshot projection;
    if (!readViewMatrices(view, projection)) return;
    // Strengths, 0..1 (0 skips the effect); used as on/off where only that matters.
    float aoMode = request.ao, dofMode = request.dof, ssrMode = projection.worldUpValid ? request.ssr : 0.f;
    bool finishMode = request.finish();
    if (!aoMode && !dofMode && !ssrMode && !finishMode && !request.adapt) return;
    ViewFrame frame{};
    // Every GL query or error check makes the game's thread wait for the driver's
    // (a threaded driver answers only once it has caught up). The state that does not
    // change between frames is therefore checked in full on an audit frame (the first,
    // then every viewAuditFrames, and after anything failed) and reused in between;
    // only the bindings and the viewport, which do change, are read every frame.
    auto* owner = prepareCue();
    if (!owner) return;
    auto& g = owner->api;
    auto& r = owner->resources;
    const bool audit = owner->viewAuditCountdown == 0;
    owner->viewAuditCountdown = audit ? viewAuditFrames : owner->viewAuditCountdown - 1;
    GLint framebuffer = -1, readFramebuffer = -1;
    if (audit) {
        drainGl();
        glGetIntegerv(glc::readBuffer, &owner->viewReadBuffer);
        glGetIntegerv(glc::sampleBuffers, &owner->viewSampleBuffers);
        glGetIntegerv(glc::maxTextureImageUnits, &owner->viewTextureUnits);
        glGetFloatv(glc::depthRange, owner->viewDepthRange);
        owner->viewAuditFailed = glGetError() != GL_NO_ERROR;
    }
    glGetIntegerv(glc::drawFramebufferBinding, &framebuffer);
    glGetIntegerv(glc::readFramebufferBinding, &readFramebuffer);
    glGetIntegerv(GL_VIEWPORT, frame.viewport);
    const GLfloat* depthRange = owner->viewDepthRange;
    const GLint* viewport = frame.viewport;
    // VR eyes and offscreen views render into their own framebuffers, and a
    // per-object cropped viewport can be left set; all of those stay native.
    RECT client{};
    if (!request.eye && (!hookedWindow || !GetClientRect(hookedWindow, &client))) return;
    const GLint expectedFramebuffer = request.eye ? request.eyeFramebuffer : 0;
    const GLint expectedWidth = request.eye ? request.eyeWidth : client.right, expectedHeight = request.eye ? request.eyeHeight : client.bottom;
    if (owner->viewAuditFailed || framebuffer != expectedFramebuffer || readFramebuffer != expectedFramebuffer ||
        (!expectedFramebuffer && owner->viewReadBuffer != glc::back) || owner->viewSampleBuffers != 0 ||
        owner->viewTextureUnits < int(viewTextureUnits) || !std::isfinite(depthRange[0]) ||
        !std::isfinite(depthRange[1]) || std::abs(depthRange[1] - depthRange[0]) < 1e-6f) {
        owner->viewAuditCountdown = 0; // look again in full next frame
        return skipViewEffects("the framebuffer routing differs from the view's (an overlay or another target is bound)");
    }
    if (request.offscreen) {
        // The passes work on the view texture at its own size, not the window's.
        frame.viewport[0] = frame.viewport[1] = 0;
        frame.viewport[2] = request.outputWidth;
        frame.viewport[3] = request.outputHeight;
    } else if (viewport[0] != 0 || viewport[1] != 0 || viewport[2] != expectedWidth || viewport[3] != expectedHeight ||
               viewport[2] < 32 || viewport[3] < 32) {
        return skipViewEffects("the viewport does not cover the window (a cropped or resized view)");
    }
    const float rangeSpan = depthRange[1] - depthRange[0];
    frame.texel[0] = 1.f / float(viewport[2]);
    frame.texel[1] = 1.f / float(viewport[3]);
    frame.projection[0] = projection.p0;
    frame.projection[1] = projection.p5;
    frame.projection[2] = projection.p8;
    frame.projection[3] = projection.p9;
    frame.depthLinear[0] = 2.f / rangeSpan;
    frame.depthLinear[1] = projection.p10 - 1.f - 2.f * depthRange[0] / rangeSpan;
    frame.depthLinear[2] = projection.p14;
    frame.pixelScale = float(viewport[3]) / 1080.f;
    frame.worldUp[0] = projection.worldUpX;
    frame.worldUp[1] = projection.worldUpY;
    frame.worldUp[2] = projection.worldUpZ;
    if (!std::isfinite(frame.depthLinear[0]) || !std::isfinite(frame.depthLinear[1])) return;

    const unsigned contextIndex = unsigned(owner - cueContexts);

    if (!owner->viewApiAttempted) {
        owner->viewApiAttempted = true;
        owner->viewApiFailed = !loadViewEffectsApi(g);
        if (owner->viewApiFailed) noteViewEffectFailure(viewApiMissing, "OpenGL 3.2 entry points missing");
    }
    if (owner->viewApiFailed) return;
    if (owner->viewTargetsFailed) {
        // Out of memory after a resize, a lost surface: retry a few times, then stay off.
        if (owner->viewRetries >= viewRetryLimit || GetTickCount64() < owner->viewRetryAt) return;
        owner->viewTargetsFailed = false;
        owner->aoAttempted = owner->ssrAttempted = owner->dofAttempted = owner->finishAttempted =
            owner->compositeAttempted = owner->resolveAttempted = owner->dofCompositeAttempted =
            owner->meterAttempted = owner->distanceAttempted = owner->woundAttempted = owner->fireAttempted = false;
    }
    // Multi-output routing is checked on audit frames (see above).
    if (audit) owner->viewOutputsReady = cueOutputReady(g);
    if (!owner->viewOutputsReady) {
        owner->viewAuditCountdown = 0;
        return;
    }
    if (!prepareViewTargets(g, r)) {
        failViewTargets(*owner, "could not create the colour/depth copies");
        return;
    }
    struct Effect {
        float mode; // the effect's strength; 0 leaves its program unbuilt
        unsigned* program;
        bool* attempted;
        const char* shared;
        const char* body;
        unsigned failure;
        const char* name;
    };
    Effect effects[programCount] = { // in ViewProgram order
        {aoMode, &r.aoProgram, &owner->aoAttempted, nullptr, aoShader, viewAoFailed, "ambient occlusion"},
        {ssrMode, &r.ssrProgram, &owner->ssrAttempted, nullptr, ssrShader, viewSsrFailed, "reflections"},
        {dofMode, &r.dofProgram, &owner->dofAttempted, dofLens, dofShader, viewDofFailed, "depth of field"},
        {float(finishMode || request.adapt), &r.finishProgram, &owner->finishAttempted, surfaceShading, finishShader,
         viewFinishFailed, "finishing pass"},
        {float(aoMode || ssrMode), &r.compositeProgram, &owner->compositeAttempted, surfaceShading, compositeShader,
         viewCompositeFailed, "occlusion/reflection composite"},
        // VR eyes alternate on one context, so one history would mix both eyes; they stay per frame.
        {float((aoMode || ssrMode) && !request.eye), &r.resolveProgram, &owner->resolveAttempted, nullptr, resolveShader,
         viewResolveFailed, "occlusion/reflection blending over frames"},
        {dofMode, &r.dofCompositeProgram, &owner->dofCompositeAttempted, dofLens, dofCompositeShader,
         viewDofCompositeFailed, "depth-of-field composite"},
        {float(request.adapt), &r.meterProgram, &owner->meterAttempted, nullptr, meterShader, viewMeterFailed,
         "eye adaptation meter"},
        {float(aoMode || ssrMode), &r.distanceProgram, &owner->distanceAttempted, nullptr, distanceShader,
         viewDistanceFailed, "view-distance target"},
        {float(finishMode && (request.wounds.count || request.wounds.drips || request.wounds.caps)), &r.woundProgram, &owner->woundAttempted, nullptr, woundShader,
         viewWoundsFailed, "wound marks"},
        {float(finishMode && request.fires.count), &r.fireProgram, &owner->fireAttempted, nullptr, fireShader,
         viewFireFailed, "fire and smoke"}};
    for (unsigned i = 0; i < programCount; ++i) {
        if (!effects[i].mode || *effects[i].attempted) continue;
        *effects[i].attempted = true;
        *effects[i].program = buildViewProgram(g, effects[i].shared, effects[i].body, effects[i].failure, effects[i].name,
                                              viewUniforms[contextIndex][i]);
        drainGl(); // compile errors stay with the failed effect
    }
    if (!r.aoProgram) aoMode = 0;
    if (!r.ssrProgram) ssrMode = 0;
    if (!r.dofProgram || !r.dofCompositeProgram) dofMode = 0;
    if (!r.finishProgram) finishMode = false;
    if (!r.compositeProgram) aoMode = ssrMode = 0;
    const bool adaptMode = request.adapt && r.meterProgram && r.finishProgram;
    if (!aoMode && !dofMode && !ssrMode && !finishMode && !adaptMode) return;
    // Blending over frames needs last frame's camera, from a frame drawn just before this one.
    const bool temporal = (aoMode || ssrMode) && !request.eye && r.resolveProgram;
    const unsigned frameIndex = owner->viewFrame;
    const ULONGLONG now = GetTickCount64();
    float toPrevious[16] = {1.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 0.f, 1.f};
    const bool cameraContinues = temporal && owner->previousTick && now - owner->previousTick <= 250 &&
                                 viewToPrevious(projection.view, owner->previousView, toPrevious);

    // Snapshot every piece of state the passes change; nothing is written
    // until the snapshot is complete.
    GLint program = 0, vao = 0, buffer = 0, activeUnit = 0, pack = 0, packStore[3]{};
    GLint texture[viewTextureUnits]{}, sampler[viewTextureUnits]{};
    GLint noise3D = 0; // the held-noise unit's 3D binding, restored when touched
    bool noiseTouched = false;
    GLint polygon[2]{};
    GLboolean colorMask[4]{};
    const GLenum caps[] = {GL_BLEND, GL_DEPTH_TEST, GL_CULL_FACE, GL_SCISSOR_TEST, GL_STENCIL_TEST,
                           glc::framebufferSrgb, glc::rasterizerDiscard, glc::alphaTest, glc::colorLogicOp};
    constexpr unsigned capCount = sizeof caps / sizeof caps[0];
    GLboolean enabled[capCount]{};
    // Bindings change with whatever the game drew last: read every frame. Samplers, pixel
    // packing, polygon mode, colour mask and the rarely used capabilities (stencil,
    // discard, alpha test, logic op) hold still: read on audit frames and reused.
    auto rareCap = [](unsigned i) { return i == 4 || i >= 6; };
    glGetIntegerv(glc::currentProgram, &program);
    glGetIntegerv(glc::vertexArrayBinding, &vao);
    glGetIntegerv(glc::arrayBufferBinding, &buffer);
    glGetIntegerv(glc::activeTexture, &activeUnit);
    for (unsigned unit = 0; unit < viewTextureUnits; ++unit) {
        g.activeTexture(glc::texture0 + unit);
        glGetIntegerv(glc::textureBinding2D, &texture[unit]);
        if (unit == heldNoiseUnit) glGetIntegerv(textureBinding3D, &noise3D);
        if (audit && g.bindSampler) glGetIntegerv(glc::samplerBinding, &owner->viewSampler[unit]);
        sampler[unit] = owner->viewSampler[unit];
    }
    if (audit) {
        glGetIntegerv(glc::pixelPackBufferBinding, &owner->viewPack);
        glGetIntegerv(glc::packRowLength, &owner->viewPackStore[0]);
        glGetIntegerv(glc::packSkipRows, &owner->viewPackStore[1]);
        glGetIntegerv(glc::packSkipPixels, &owner->viewPackStore[2]);
        glGetIntegerv(glc::polygonMode, owner->viewPolygon);
        glGetBooleanv(GL_COLOR_WRITEMASK, owner->viewColorMask);
        for (unsigned i = 0; i < capCount; ++i)
            if (rareCap(i)) owner->viewRareCaps[i] = glIsEnabled(caps[i]);
    }
    pack = owner->viewPack;
    memcpy(packStore, owner->viewPackStore, sizeof packStore);
    memcpy(polygon, owner->viewPolygon, sizeof polygon);
    memcpy(colorMask, owner->viewColorMask, sizeof colorMask);
    for (unsigned i = 0; i < capCount; ++i) enabled[i] = rareCap(i) ? owner->viewRareCaps[i] : glIsEnabled(caps[i]);
    if (audit && glGetError() != GL_NO_ERROR) {
        g.activeTexture(GLenum(activeUnit));
        drainGl();
        owner->viewAuditCountdown = 0;
        return;
    }
    if (owner->viewEncoding < 0) {
        GLint encoding = 0;
        if (g.getAttachmentParameter)
            g.getAttachmentParameter(glc::framebuffer, glc::backLeft, glc::colorEncoding, &encoding);
        owner->viewEncoding = glGetError() == GL_NO_ERROR && encoding == glc::srgb ? 1 : 0;
        drainGl();
    }
    frame.encoded = owner->viewEncoding == 1 && enabled[5]; // caps[5]: the game's sRGB writes

    // GPU timings: a timestamp at each stage boundary, in a ring slot not still in flight,
    // on one frame in eight (reading them back is a query too).
    const unsigned timerSlot = r.timerSlot;
    bool timed = false;
    unsigned stamped = 0;
    ++r.timerCalls;
    if (!request.eye) {
        const unsigned long long callCpu = cpuNowNs();
        if (r.lastCallCpu && callCpu > r.lastCallCpu && callCpu - r.lastCallCpu < 1000000000ull)
            easeMs(viewCpuFrameMs, float(double(callCpu - r.lastCallCpu) * 1e-6));
        r.lastCallCpu = callCpu;
    }
    if (g.queryCounter && g.getInteger64v && audit) {
        // An audit frame already waits for the driver: place the GPU clock against the CPU's.
        long long gpuNow = 0;
        g.getInteger64v(0x8E28 /*TIMESTAMP*/, &gpuNow);
        const unsigned long long cpu = cpuNowNs();
        r.gpuClockKnown = gpuNow > 0 && cpu > 0;
        r.gpuMinusCpuNs = gpuNow - static_cast<long long>(cpu);
    }
    if (g.queryCounter && owner->viewAuditCountdown % 8 == 0) {
        collectViewTimings(g, r);
        timed = !r.timerIssued[timerSlot];
        if (timed && !r.timerQueries[timerSlot][0]) g.genQueries(6, r.timerQueries[timerSlot]);
        timed = timed && r.timerQueries[timerSlot][0];
    }
    auto stampTo = [&](unsigned last) {
        while (timed && stamped <= last) {
            r.timerCpu[timerSlot][stamped] = cpuNowNs();
            g.queryCounter(r.timerQueries[timerSlot][stamped++], 0x8E28 /*TIMESTAMP*/);
        }
    };

    bool ok = false, halfFailed = false, skipped = false;
    float collectedExposure = 1.f, collectedFocus = 0.f;
    __try {
        // Earlier frames' meter and focus readbacks are collected here, before any pass is
        // issued and beside the state snapshot: their fence checks and buffer reads wait for
        // the driver, and here that wait finds nothing of ours queued.
        if (adaptMode && owner->viewAuditCountdown % 4 == 0) collectedExposure = collectMeter(g, r, *owner);
        if (dofMode) collectedFocus = collectFocus(g, *owner, contextIndex);
        stampTo(0);
        // sRGB conversion off before the copies: they and the passes then all
        // see the same stored bytes, so the image round-trips unchanged.
        for (unsigned i = 0; i < capCount; ++i)
            if (enabled[i]) cueEnable(g, caps[i], false);
        glPolygonMode(glc::frontAndBack, glc::fill);
        if (g.indexedMasks) g.colorMaskIndexed(0, 1, 1, 1, 1);
        else glColorMask(1, 1, 1, 1);
        if (g.bindSampler)
            for (unsigned unit = 0; unit < viewTextureUnits; ++unit) g.bindSampler(unit, 0);
        g.bindVertexArray(r.viewVao);
        for (unsigned i = 0; i < 3; ++i) // the focus and meter readbacks need tightly packed rows
            if (packStore[i]) glPixelStorei(glc::packRowLength + i, 0);
        // Offscreen sampling: the passes read and finally write the view texture through a
        // framebuffer of ours, and take the sampling target's depth copied to its size.
        GLuint target = GLuint(expectedFramebuffer), depthSource = GLuint(expectedFramebuffer);
        if (request.offscreen) {
            const char* why = "";
            if (!prepareOffscreenTargets(g, r, request, why, audit)) {
                skipViewEffects(why);
                skipped = true;
                ok = !audit || glGetError() == GL_NO_ERROR;
                __leave;
            }
            target = r.offscreenFbo;
            depthSource = r.offscreenDepthFbo;
            frame.encoded = r.offscreenEncoded;
            g.bindFramebuffer(glc::framebuffer, target);
            setViewport(g, viewport[0], viewport[1], viewport[2], viewport[3]);
        }

        // Eye adaptation reads the native image, before any effect changes it. Once
        // the eye has adjusted the exposure is 1 and, with nothing else on, the frame
        // needs no copy and no pass beyond the meter.
        // The eye adapts over seconds: the meter is read and redrawn on one frame in four.
        float exposure = 1.f;
        if (adaptMode && owner->viewAuditCountdown % 4 != 0) {
            exposure = owner->adapted > 0.f ? owner->exposure : 1.f;
        } else if (adaptMode) {
            exposure = collectedExposure;
            g.useProgram(r.meterProgram);
            const auto& u = viewUniforms[contextIndex][programMeter];
            setViewUniforms(g, u, frame);
            g.uniform2f(u.at[uCells], float(meterWidth), float(meterHeight));
            if (!requestMeter(g, r, *owner, target, viewport)) exposure = 1.f;
            g.bindFramebuffer(glc::framebuffer, target);
            setViewport(g, viewport[0], viewport[1], viewport[2], viewport[3]);
        }
        if (adaptMode) {
            // Below a third of one 8-bit step at mid grey, so ending the pass shows no step.
            if (std::abs(exposure - 1.f) > 0.003f) finishMode = true;
            else exposure = 1.f;
        }
        if (!aoMode && !ssrMode && !dofMode && !finishMode) {
            stampTo(5);
            ok = !audit || glGetError() == GL_NO_ERROR;
            __leave;
        }
        const bool resize = r.viewWidth != viewport[2] || r.viewHeight != viewport[3];
        // The window (desktop, not a VR eye): one blit of colour and depth into textures of the window's own
        // depth format. copyTexSubImage from the window took the driver's slow path (13.75 ms of the 16.7 ms
        // the effects cost at 2560x1440).
        const bool blitWindow = !request.offscreen && target == 0;
        if (temporal) std::swap(r.viewDepth, r.viewDepthPrevious); // blended frames keep last frame's depth
        if (blitWindow) {
            const GLint depthFormat = defaultDepthFormat();
            const bool packed = depthFormat == GLint(0x88F0);
            if (resize || r.viewDepthFormat != depthFormat || !r.viewCopyFbo) {
                g.activeTexture(glc::texture0);
                g.bindTexture(GL_TEXTURE_2D, r.viewColor);
                glTexImage2D(GL_TEXTURE_2D, 0, GLint(glc::rgba8), viewport[2], viewport[3], 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
                g.activeTexture(glc::texture1);
                for (GLuint depth : {r.viewDepthPrevious, r.viewDepth}) {
                    g.bindTexture(GL_TEXTURE_2D, depth);
                    glTexImage2D(GL_TEXTURE_2D, 0, depthFormat, viewport[2], viewport[3], 0,
                                 packed ? 0x84F9 /* DEPTH_STENCIL */ : glc::depthComponent,
                                 packed ? 0x84FA /* UNSIGNED_INT_24_8 */ : GL_UNSIGNED_INT, nullptr);
                }
                r.viewDepthFormat = depthFormat;
                if (!r.viewCopyFbo) g.genFramebuffers(1, &r.viewCopyFbo);
            }
            g.bindFramebuffer(glc::framebuffer, r.viewCopyFbo);
            g.framebufferTexture2D(glc::framebuffer, glc::colorAttachment0, GL_TEXTURE_2D, r.viewColor, 0);
            g.framebufferTexture2D(glc::framebuffer, packed ? 0x821A /* DEPTH_STENCIL_ATTACHMENT */ : glc::depthAttachment,
                                   GL_TEXTURE_2D, r.viewDepth, 0);
            g.bindFramebuffer(glc::readFramebuffer, 0);
            g.blitFramebuffer(viewport[0], viewport[1], viewport[0] + viewport[2], viewport[1] + viewport[3], 0, 0,
                              viewport[2], viewport[3], GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT, glc::nearest);
        } else {
        g.activeTexture(glc::texture0);
        g.bindTexture(GL_TEXTURE_2D, r.viewColor);
        if (resize)
            g.copyTexImage2D(GL_TEXTURE_2D, 0, glc::rgba8, viewport[0], viewport[1], viewport[2], viewport[3], 0);
        else
            g.copyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, viewport[0], viewport[1], viewport[2], viewport[3]);
        g.activeTexture(glc::texture1);
        if (request.offscreen) {
            // The sampling target's depth, blitted straight into the view's depth copy, which
            // takes the target's format for it (a depth blit needs matching formats). Nearest:
            // depth must not blend; a larger target scales down to the view texture.
            if (resize || r.viewDepthFormat != r.offscreenDepthFormat) {
                for (GLuint depth : {r.viewDepthPrevious, r.viewDepth}) {
                    g.bindTexture(GL_TEXTURE_2D, depth);
                    glTexImage2D(GL_TEXTURE_2D, 0, r.offscreenDepthFormat, viewport[2], viewport[3], 0,
                                 r.offscreenDepthLayout, r.offscreenDepthType, nullptr);
                }
                r.viewDepthFormat = r.offscreenDepthFormat;
            }
            g.bindFramebuffer(glc::framebuffer, r.offscreenDepthFbo);
            g.framebufferTexture2D(glc::framebuffer, r.offscreenDepthAttachment, GL_TEXTURE_2D, r.viewDepth, 0);
            g.bindFramebuffer(glc::readFramebuffer, request.sceneFramebuffer);
            g.blitFramebuffer(0, 0, r.offscreenSceneWidth, r.offscreenSceneHeight, 0, 0, viewport[2], viewport[3],
                              GL_DEPTH_BUFFER_BIT, glc::nearest);
        } else if (resize || r.viewDepthFormat != defaultDepthFormat()) {
            for (GLuint depth : {r.viewDepthPrevious, r.viewDepth}) {
                g.bindTexture(GL_TEXTURE_2D, depth);
                g.copyTexImage2D(GL_TEXTURE_2D, 0, GLenum(defaultDepthFormat()), viewport[0], viewport[1], viewport[2],
                                 viewport[3], 0);
            }
            r.viewDepthFormat = defaultDepthFormat();
        } else {
            g.bindTexture(GL_TEXTURE_2D, r.viewDepth);
            g.copyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, viewport[0], viewport[1], viewport[2], viewport[3]);
        }
        }
        r.viewWidth = viewport[2];
        r.viewHeight = viewport[3];
        g.bindFramebuffer(glc::framebuffer, target);
        stampTo(1);

        // All screen-space effects start at native resolution; future performance
        // work must reduce shader or memory cost without silently lowering image size.
        if ((aoMode || ssrMode || dofMode) &&
            !prepareHalfTargets(g, r, viewport[2], viewport[3], target)) {
            publishViewTargetSizes(0, 0, 0, 0, 0, 0, 0, 0, false);
            halfFailed = true;
            aoMode = ssrMode = dofMode = 0;
        }
        // Depth of field needs a focus; decided now so every pass knows whether it is the last.
        float focus = 0.f;
        if (dofMode) {
            focus = collectedFocus;
            g.bindFramebuffer(glc::readFramebuffer, depthSource); // the focus patch reads depth
            requestFocus(g, *owner, contextIndex, viewport, frame.depthLinear);
            g.bindFramebuffer(glc::framebuffer, target);
            if (!(focus > 0.f)) dofMode = 0;
        }
        // Each full-resolution pass reads `source` on unit 0 and writes the other colour
        // target; the last writes the view's framebuffer. Without the targets each pass
        // writes the framebuffer and the next copies it back, as before.
        const bool chain = prepareChainTargets(g, r, viewport[2], viewport[3], target);
        GLuint source = r.viewColor;
        auto beginPass = [&](bool last) {
            g.activeTexture(glc::texture0);
            g.bindTexture(GL_TEXTURE_2D, source);
            g.bindFramebuffer(glc::framebuffer, last || !chain ? target
                                                              : source == r.viewColor ? r.chainFbo[1] : r.chainFbo[0]);
            setViewport(g, viewport[0], viewport[1], viewport[2], viewport[3]);
        };
        auto endPass = [&](bool last) {
            if (last) return;
            if (chain) source = source == r.viewColor ? r.chainTex : r.viewColor;
            else copyViewColor(g, r, viewport);
        };
        // At Strong the occlusion taps also gather one bounce of the light around them;
        // blended over frames only, as a few taps of colour are grainy on their own.
        // Bounced light grows in from the middle of the scale to the old Strong.
        // PROVISIONAL(2026-10-22, bounce gathered from 3 taps drew flickering lines under moving
        // torchlight; off until it can be gathered smoothly, ratify-by: 2026-11-05)
        const float bounceStrength = 0.f;
        const bool bounce = bounceStrength > 0.f;
        // The composite folds into the finish pass whenever there is one: one full-screen pass
        // fewer. With depth of field on, occlusion and reflections then land on the blurred
        // image; being low-frequency they read the same.
        const bool fold = (aoMode || ssrMode) && finishMode;
        // The composite reads each effect's blend when it has one, else this frame's result.
        GLuint results[2] = {r.halfTex[0], r.halfTex[1]};
        if (aoMode || ssrMode) {
            setViewport(g, 0, 0, r.halfWidth, r.halfHeight);
            const float offset = temporal ? 5.588238f * float(frameIndex % 64) : 0.f;
            // Distances first; without the program the taps read the depth as before.
            const bool distances = r.distanceProgram != 0;
            if (distances) {
                g.bindFramebuffer(glc::framebuffer, r.distanceFbo);
                g.useProgram(r.distanceProgram);
                setViewUniforms(g, viewUniforms[contextIndex][programDistance], frame);
                glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
                g.activeTexture(glc::texture0 + 4);
                g.bindTexture(GL_TEXTURE_2D, r.distanceTex);
            }
            if (aoMode) {
                g.bindFramebuffer(glc::framebuffer, r.halfFbo[0]);
                g.useProgram(r.aoProgram);
                const auto& u = viewUniforms[contextIndex][programAo];
                setViewUniforms(g, u, frame);
                g.uniform1f(u.at[uFrameOffset], offset);
                // Blended over frames, a third fewer taps per frame give the same still image.
                g.uniform1i(u.at[uTaps], int(std::lround(temporal ? std::clamp(4.f + 2.f * aoMode, 4.f, 6.f)
                                                                    : std::clamp(4.f + 6.f * aoMode, 4.f, 10.f))));
                g.uniform1f(u.at[uBounceOn], bounce ? 1.f : 0.f);
                g.uniform1i(u.at[uDistanceHalf], 4);
                g.uniform1f(u.at[uDistanceHalfOn], distances ? 1.f : 0.f);
                g.uniform2f(u.at[uHalfSize], float(r.halfWidth), float(r.halfHeight));
                glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
            }
            if (ssrMode) {
                setViewport(g, 0, 0, r.ssrWidth, r.ssrHeight);
                g.bindFramebuffer(glc::framebuffer, r.halfFbo[1]);
                g.useProgram(r.ssrProgram);
                const auto& u = viewUniforms[contextIndex][programSsr];
                setViewUniforms(g, u, frame);
                g.uniform1f(u.at[uFrameOffset], offset);
                g.uniform1f(u.at[uStrength], (std::max)(effectScale(ssrMode, 0.6f, 0.85f), 0.02f));
                g.uniform1i(u.at[uTaps], int(std::lround(temporal ? std::clamp(6.f + 4.f * ssrMode, 6.f, 10.f)
                                                                    : std::clamp(6.f + 12.f * ssrMode, 6.f, 18.f))));
                g.uniform4f(u.at[uWorldUp], frame.worldUp[0], frame.worldUp[1], frame.worldUp[2], 0.f);
                g.uniform1i(u.at[uDistanceHalf], 4);
                g.uniform1f(u.at[uDistanceHalfOn], distances ? 1.f : 0.f);
                glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
            }
            if (temporal) {
                const unsigned next = r.historySlot ^ 1;
                g.useProgram(r.resolveProgram);
                const auto& u = viewUniforms[contextIndex][programResolve];
                setViewUniforms(g, u, frame);
                g.uniform1i(u.at[uCurrent], 2);
                g.uniform1i(u.at[uHistory], 3);
                // Unit 0 is free here: the composite binds its source there afterwards.
                g.activeTexture(glc::texture0);
                g.bindTexture(GL_TEXTURE_2D, r.viewDepthPrevious);
                g.uniform1i(u.at[uPreviousDepth], 0);
                g.uniformMatrix4fv(u.at[uToPrevious], 1, 0, toPrevious);
                g.uniform4f(u.at[uPreviousProjection], owner->previousProjection[0], owner->previousProjection[1],
                            owner->previousProjection[2], owner->previousProjection[3]);
                const float modes[2] = {aoMode, ssrMode};
                for (unsigned e = 0; e < 2; ++e) {
                    if (!modes[e]) continue;
                    // About ten frames carry the blend, a sixth of a second at 60 fps; the
                    // neighbourhood clamp lets a real change through at once.
                    const bool usable = cameraContinues && r.historyFrame[e] == frameIndex;
                    g.uniform1f(u.at[uHistoryWeight], usable ? 0.9f : 0.f);
                    const int width = e ? r.ssrWidth : r.halfWidth, height = e ? r.ssrHeight : r.halfHeight;
                    setViewport(g, 0, 0, width, height);
                    g.uniform2f(u.at[uHalfSize], float(width), float(height));
                    g.activeTexture(glc::texture2);
                    g.bindTexture(GL_TEXTURE_2D, r.halfTex[e]);
                    g.activeTexture(glc::texture3);
                    g.bindTexture(GL_TEXTURE_2D, r.historyTex[e][r.historySlot]);
                    g.bindFramebuffer(glc::framebuffer, r.historyFbo[e][next]);
                    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
                    results[e] = r.historyTex[e][next];
                    r.historyFrame[e] = frameIndex + 1;
                }
                r.historySlot = next;
            }
            stampTo(2);
        }
        if ((aoMode || ssrMode) && !fold) {
            const bool last = !dofMode && !finishMode;
            beginPass(last);
            g.activeTexture(glc::texture2);
            g.bindTexture(GL_TEXTURE_2D, results[0]);
            g.activeTexture(glc::texture3);
            g.bindTexture(GL_TEXTURE_2D, results[1]);
            g.useProgram(r.compositeProgram);
            const auto& u = viewUniforms[contextIndex][programComposite];
            setViewUniforms(g, u, frame);
            g.uniform1i(u.at[uAoHalf], 2);
            g.uniform1i(u.at[uSsrHalf], 3);
            g.uniform2f(u.at[uHalfSize], float(r.halfWidth), float(r.halfHeight));
            g.uniform2f(u.at[uSsrSize], float(r.ssrWidth), float(r.ssrHeight));
            g.uniform1f(u.at[uDistanceHalfOn], r.distanceProgram ? 1.f : 0.f);
            if (r.distanceProgram) {
                g.activeTexture(glc::texture0 + 6);
                g.bindTexture(GL_TEXTURE_2D, r.distanceTex);
                g.uniform1i(u.at[uDistanceHalf], 6);
            }
            g.uniform1f(u.at[uAoStrength], aoMode > 0.f ? (std::max)(effectScale(aoMode, 0.65f, 0.9f), 0.f) : 0.f);
            g.uniform1f(u.at[uBounceStrength], bounceStrength);
            g.uniform1f(u.at[uSsrOn], ssrMode ? 1.f : 0.f);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
            endPass(last);
        }
        stampTo(3);
        if (dofMode) {
            {
                // Blur the image the other effects produced, not the native copy.
                g.activeTexture(glc::texture0);
                g.bindTexture(GL_TEXTURE_2D, source);
                g.activeTexture(glc::texture1);
                g.bindTexture(GL_TEXTURE_2D, r.viewDepth);
                const float maxBlur = dofMaxBlur(dofMode) * frame.pixelScale;
                const float lens = dofStart(dofMode, focus);
                setViewport(g, 0, 0, r.dofWidth, r.dofHeight);
                g.bindFramebuffer(glc::framebuffer, r.dofFbo);
                g.useProgram(r.dofProgram);
                const auto& u = viewUniforms[contextIndex][programDof];
                setViewUniforms(g, u, frame);
                g.uniform1i(u.at[uTaps], int(std::lround(std::clamp(8.f + 8.f * dofMode, 8.f, 16.f))));
                g.uniform1f(u.at[uFocus], focus);
                g.uniform1f(u.at[uMaxBlur], maxBlur);
                g.uniform1f(u.at[uLens], lens);
                glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
            }
            // With a finish pass the blur is mixed in there (dofOn); otherwise its own pass.
            if (!finishMode) {
                const float maxBlur = dofMaxBlur(dofMode) * frame.pixelScale;
                const float lens = dofStart(dofMode, focus);
                const bool last = true;
                beginPass(last);
                g.activeTexture(glc::texture2);
                g.bindTexture(GL_TEXTURE_2D, r.dofTex);
                g.useProgram(r.dofCompositeProgram);
                const auto& c = viewUniforms[contextIndex][programDofComposite];
                setViewUniforms(g, c, frame);
                g.uniform1i(c.at[uDofHalf], 2);
                g.uniform1f(c.at[uFocus], focus);
                g.uniform1f(c.at[uMaxBlur], maxBlur);
                g.uniform1f(c.at[uLens], lens);
                glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
                endPass(last);
            }
        }
        stampTo(4);
        // Wound marks first, on the image the finishing pass reads, only inside their screen box.
        if (finishMode && (request.wounds.count || request.wounds.drips || request.wounds.caps) && r.woundProgram && g.uniform4fv &&
            projection.worldUpValid &&
            (source == r.viewColor || source == r.chainTex) && woundPassReady(g, r)) {
            const WoundUniforms& wounds = request.wounds;
        // The screen box of every wound's reach (centre +- half length + runs + 6 cm, as the shader
        // tests); a wound the camera stands within covers the whole view.
        float box[4] = {1.f, 1.f, 0.f, 0.f};
        for (unsigned k = 0; k < wounds.count; ++k) {
            const float* c = wounds.a[k];
            const float reach = c[3] + wounds.c[k][3] + 6.f, depth = -c[2];
            if (!(depth - reach > 1.f)) { box[0] = box[1] = 0.f; box[2] = box[3] = 1.f; break; }
            const float sx = (projection.p0 * c[0] / depth - projection.p8) * 0.5f + 0.5f;
            const float sy = (projection.p5 * c[1] / depth - projection.p9) * 0.5f + 0.5f;
            const float rx = std::abs(projection.p0) * reach / (depth - reach) * 0.5f;
            const float ry = std::abs(projection.p5) * reach / (depth - reach) * 0.5f;
            box[0] = (std::min)(box[0], sx - rx); box[1] = (std::min)(box[1], sy - ry);
            box[2] = (std::max)(box[2], sx + rx); box[3] = (std::max)(box[3], sy + ry);
        }
        // Each cap's disc.
        for (unsigned k = 0; k < wounds.caps && box[2] - box[0] < 1.f; ++k) {
            const float* c = wounds.capA[k];
            const float reach = c[3] * 1.5f, depth = -c[2];
            if (!(depth - reach > 1.f)) { box[0] = box[1] = 0.f; box[2] = box[3] = 1.f; break; }
            const float sx = (projection.p0 * c[0] / depth - projection.p8) * 0.5f + 0.5f;
            const float sy = (projection.p5 * c[1] / depth - projection.p9) * 0.5f + 0.5f;
            const float rx = std::abs(projection.p0) * reach / (depth - reach) * 0.5f;
            const float ry = std::abs(projection.p5) * reach / (depth - reach) * 0.5f;
            box[0] = (std::min)(box[0], sx - rx); box[1] = (std::min)(box[1], sy - ry);
            box[2] = (std::max)(box[2], sx + rx); box[3] = (std::max)(box[3], sy + ry);
        }
        // Each drop's reach: from 20 to 200 cm below where it fell, three radii across.
        for (unsigned k = 0; k < wounds.drips && box[2] - box[0] < 1.f; ++k) {
            const float* d = wounds.d[k];
            const float radius = d[3] - 10.f * std::floor(d[3] / 10.f);
            const float c[3] = {d[0] - projection.worldUpX * 110.f, d[1] - projection.worldUpY * 110.f,
                                d[2] - projection.worldUpZ * 110.f};
            const float reach = 90.f + radius * 3.f, depth = -c[2];
            if (!(depth - reach > 1.f)) { box[0] = box[1] = 0.f; box[2] = box[3] = 1.f; break; }
            const float sx = (projection.p0 * c[0] / depth - projection.p8) * 0.5f + 0.5f;
            const float sy = (projection.p5 * c[1] / depth - projection.p9) * 0.5f + 0.5f;
            const float rx = std::abs(projection.p0) * reach / (depth - reach) * 0.5f;
            const float ry = std::abs(projection.p5) * reach / (depth - reach) * 0.5f;
            box[0] = (std::min)(box[0], sx - rx); box[1] = (std::min)(box[1], sy - ry);
            box[2] = (std::max)(box[2], sx + rx); box[3] = (std::max)(box[3], sy + ry);
        }
        box[0] = std::clamp(box[0], 0.f, 1.f); box[1] = std::clamp(box[1], 0.f, 1.f);
            box[2] = std::clamp(box[2], 0.f, 1.f); box[3] = std::clamp(box[3], 0.f, 1.f);
            if (box[2] > box[0] && box[3] > box[1]) {
                const auto& u = viewUniforms[contextIndex][programWounds];
                g.activeTexture(glc::texture1);
                g.bindTexture(GL_TEXTURE_2D, r.viewDepth);
                g.activeTexture(glc::texture0);
                g.bindTexture(GL_TEXTURE_2D, source);
                g.bindFramebuffer(glc::framebuffer, r.woundFbo);
                g.framebufferTexture2D(glc::framebuffer, glc::colorAttachment0, GL_TEXTURE_2D, source, 0);
                setViewport(g, 0, 0, viewport[2], viewport[3]);
                glDisable(GL_BLEND);
                g.useProgram(r.woundProgram);
                setViewUniforms(g, u, frame);
                g.uniform1f(u.at[uWoundCount], float(wounds.count));
                g.uniform4fv(u.at[uWoundA], GLsizei(wounds.count), &wounds.a[0][0]);
                g.uniform4fv(u.at[uWoundB], GLsizei(wounds.count), &wounds.b[0][0]);
                g.uniform4fv(u.at[uWoundC], GLsizei(wounds.count), &wounds.c[0][0]);
                g.uniform4f(u.at[uWoundRect], box[0], box[1], box[2], box[3]);
                g.uniform1f(u.at[uDripCount], float(wounds.drips));
                if (wounds.drips) g.uniform4fv(u.at[uDripA], GLsizei(wounds.drips), &wounds.d[0][0]);
                g.uniform1f(u.at[uCapCount], float(wounds.caps));
                if (wounds.caps) {
                    g.uniform4fv(u.at[uCapA], GLsizei(wounds.caps), &wounds.capA[0][0]);
                    g.uniform4fv(u.at[uCapB], GLsizei(wounds.caps), &wounds.capB[0][0]);
                }
                g.uniform4f(u.at[uWorldUp], projection.worldUpX, projection.worldUpY, projection.worldUpZ, 0.f);
                // The box as a quad, in the vertex shader's -1..1 position (uv = position * 0.5 + 0.5).
                const float quad[8] = {box[0] * 2.f - 1.f, box[1] * 2.f - 1.f, box[2] * 2.f - 1.f, box[1] * 2.f - 1.f,
                                       box[0] * 2.f - 1.f, box[3] * 2.f - 1.f, box[2] * 2.f - 1.f, box[3] * 2.f - 1.f};
                g.bindVertexArray(r.woundQuadVao);
                g.bindBuffer(glc::arrayBuffer, r.woundQuadVbo);
                g.bufferData(glc::arrayBuffer, ptrdiff_t(sizeof quad), quad, glc::dynamicDraw);
                g.vertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 8, nullptr);
                g.enableVertexAttribArray(0);
                woundTextureBarrier();
                glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
                woundTextureBarrier(); // the finishing pass reads what was just written
                g.bindVertexArray(r.viewVao);
            }
        }
        // Fire and smoke (fire.inl) next, on the same image, only inside the box that holds every volume.
        if (finishMode && request.fires.count && r.fireProgram && g.uniform4fv && projection.worldUpValid &&
            (source == r.viewColor || source == r.chainTex) && woundPassReady(g, r) && prepareHeldNoise(g, r)) {
            const FireUniforms& fires = request.fires;
            // Each volume's bounding sphere, as the shader builds it; a volume the camera stands in covers the view.
            float box[4] = {1.f, 1.f, 0.f, 0.f};
            for (unsigned k = 0; k < fires.count; ++k) {
                const float* a = fires.a[k];
                const float* b = fires.b[k];
                const float height = std::sqrt(b[0] * b[0] + b[1] * b[1] + b[2] * b[2]);
                if (!(height >= 1.f)) continue;
                const float plume = b[3] < 0.5f && fires.smoke > 0.5f ? 3.2f : 1.f + 0.6f * fires.c[k][2] * fires.smoke;
                const float top = height * plume, reach = top * 0.5f + a[3] * (1.f + 0.9f * (plume - 1.f));
                const float c[3] = {a[0] + b[0] / height * top * 0.5f, a[1] + b[1] / height * top * 0.5f, a[2] + b[2] / height * top * 0.5f};
                const float depth = -c[2];
                if (!(depth - reach > 1.f)) { box[0] = box[1] = 0.f; box[2] = box[3] = 1.f; break; }
                const float sx = (projection.p0 * c[0] / depth - projection.p8) * 0.5f + 0.5f;
                const float sy = (projection.p5 * c[1] / depth - projection.p9) * 0.5f + 0.5f;
                const float rx = std::abs(projection.p0) * reach / (depth - reach) * 0.5f;
                const float ry = std::abs(projection.p5) * reach / (depth - reach) * 0.5f;
                box[0] = (std::min)(box[0], sx - rx); box[1] = (std::min)(box[1], sy - ry);
                box[2] = (std::max)(box[2], sx + rx); box[3] = (std::max)(box[3], sy + ry);
            }
            box[0] = std::clamp(box[0], 0.f, 1.f); box[1] = std::clamp(box[1], 0.f, 1.f);
            box[2] = std::clamp(box[2], 0.f, 1.f); box[3] = std::clamp(box[3], 0.f, 1.f);
            if (box[2] > box[0] && box[3] > box[1]) {
                const auto& u = viewUniforms[contextIndex][programFire];
                g.activeTexture(glc::texture0 + heldNoiseUnit);
                noiseTouched = true;
                g.bindTexture(texture3D, r.heldNoise);
                g.activeTexture(glc::texture1);
                g.bindTexture(GL_TEXTURE_2D, r.viewDepth);
                g.activeTexture(glc::texture0);
                g.bindTexture(GL_TEXTURE_2D, source);
                g.bindFramebuffer(glc::framebuffer, r.woundFbo);
                g.framebufferTexture2D(glc::framebuffer, glc::colorAttachment0, GL_TEXTURE_2D, source, 0);
                setViewport(g, 0, 0, viewport[2], viewport[3]);
                glDisable(GL_BLEND);
                g.useProgram(r.fireProgram);
                setViewUniforms(g, u, frame);
                g.uniform1i(u.at[uFireNoise], GLint(heldNoiseUnit));
                g.uniform1f(u.at[uFireCount], float(fires.count));
                g.uniform4fv(u.at[uFireA], GLsizei(fires.count), &fires.a[0][0]);
                g.uniform4fv(u.at[uFireB], GLsizei(fires.count), &fires.b[0][0]);
                g.uniform4fv(u.at[uFireC], GLsizei(fires.count), &fires.c[0][0]);
                g.uniform4f(u.at[uFireRect], box[0], box[1], box[2], box[3]);
                // Seconds, wrapped every 1000 s so the float keeps its precision.
                g.uniform4f(u.at[uFireLook], float(now % 1000000ull) / 1000.f, fires.steps, fires.smoke, 0.f);
                g.uniform4f(u.at[uFireRight], fires.right[0], fires.right[1], fires.right[2], 0.f);
                g.uniform1f(u.at[uFrameOffset], float(frameIndex % 64u));
                const float quad[8] = {box[0] * 2.f - 1.f, box[1] * 2.f - 1.f, box[2] * 2.f - 1.f, box[1] * 2.f - 1.f,
                                       box[0] * 2.f - 1.f, box[3] * 2.f - 1.f, box[2] * 2.f - 1.f, box[3] * 2.f - 1.f};
                g.bindVertexArray(r.woundQuadVao);
                g.bindBuffer(glc::arrayBuffer, r.woundQuadVbo);
                g.bufferData(glc::arrayBuffer, ptrdiff_t(sizeof quad), quad, glc::dynamicDraw);
                g.vertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 8, nullptr);
                g.enableVertexAttribArray(0);
                woundTextureBarrier();
                glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
                woundTextureBarrier(); // the finishing pass reads what was just written
                g.bindVertexArray(r.viewVao);
            }
        }
        if (finishMode) {
            beginPass(true);
            g.activeTexture(glc::texture1);
            g.bindTexture(GL_TEXTURE_2D, r.viewDepth);
            g.useProgram(r.finishProgram);
            const auto& u = viewUniforms[contextIndex][programFinish];
            setViewUniforms(g, u, frame);
            g.uniform1f(u.at[uSmoothEdges], (std::min)(request.smooth * 2.f, 1.f)); // full smoothing from the old on
            g.uniform1f(u.at[uSharpen], request.sharpen);
            g.uniform1f(u.at[uDofOn], dofMode ? 1.f : 0.f);
            if (dofMode) {
                g.activeTexture(glc::texture0 + 5);
                g.bindTexture(GL_TEXTURE_2D, r.dofTex);
                g.uniform1i(u.at[uDofHalf], 5);
            }
            // The edge-aware upsampling reads the shaded texels' own distances (unit 6).
            g.uniform1f(u.at[uDistanceHalfOn], fold && r.distanceProgram ? 1.f : 0.f);
            if (fold && r.distanceProgram) {
                g.activeTexture(glc::texture0 + 6);
                g.bindTexture(GL_TEXTURE_2D, r.distanceTex);
                g.uniform1i(u.at[uDistanceHalf], 6);
            }
            g.uniform1f(u.at[uSurfacesOn], fold ? 1.f : 0.f);
            if (fold) {
                g.activeTexture(glc::texture3);
                g.bindTexture(GL_TEXTURE_2D, results[0]);
                g.activeTexture(glc::texture0 + 4);
                g.bindTexture(GL_TEXTURE_2D, results[1]);
                g.uniform1i(u.at[uAoHalf], 3);
                g.uniform1i(u.at[uSsrHalf], 4);
                g.uniform2f(u.at[uHalfSize], float(r.halfWidth), float(r.halfHeight));
                g.uniform2f(u.at[uSsrSize], float(r.ssrWidth), float(r.ssrHeight));
                g.uniform1f(u.at[uAoStrength], aoMode > 0.f ? (std::max)(effectScale(aoMode, 0.65f, 0.9f), 0.f) : 0.f);
                g.uniform1f(u.at[uBounceStrength], bounceStrength);
                g.uniform1f(u.at[uSsrOn], ssrMode ? 1.f : 0.f);
            }
            const float visor = (std::min)(request.visor * 1.2f, 0.9f); // the old on, 0.6, at 0.5
            g.uniform1f(u.at[uVisor], request.visorHidden ? -visor : visor);
            float eye[3], feet[3];
            toView(projection.view, request.ghostEye, eye);
            toView(projection.view, request.ghostFeet, feet);
            const bool ghost = request.ghost && std::isfinite(eye[0] + eye[1] + eye[2] + feet[0] + feet[1] + feet[2]);
            g.uniform4f(u.at[uGhostA], eye[0], eye[1], eye[2], ghost ? playerRadiusCm : 0.f);
            g.uniform4f(u.at[uGhostB], feet[0], feet[1], feet[2], request.ghostStrength * 2.f);
            g.uniform2f(u.at[uCondition], request.hurt, request.winded);
            // The mask is at the scene's own size: the view's, or the sampling target's offscreen.
            const bool maskFits = r.silhouetteMask &&
                (request.offscreen ? r.silhouetteWidth == r.offscreenSceneWidth && r.silhouetteHeight == r.offscreenSceneHeight
                                   : r.silhouetteWidth == viewport[2] && r.silhouetteHeight == viewport[3]);
            const bool silhouette = ghost && request.silhouette && maskFits;
            // Held-item and armour detail share the mask: G holds each slot and its material.
            static float rows[heldSlotLimit][16][4];
            memset(rows, 0, sizeof rows);
            bool held = false;
            if ((request.detail > 0.f || request.armour > 0.f) && request.heldSlots && maskFits) {
                for (unsigned k = 0; k < heldSlotLimit; ++k) {
                    if (!(request.heldSlots & (uint64_t(1) << k))) continue;
                    const HeldPose& h = request.held[k];
                    const float strength = h.object ? request.objects : h.apparel ? request.armour : request.detail;
                    float itemFrame[16];
                    if (!(strength > 0.f)) continue;
                    if (!heldViewFrame(projection.view, h, itemFrame)) {
                        heldDetailUnanchored.fetch_add(1, std::memory_order_relaxed);
                        continue;
                    }
                    for (int a = 0; a < 3; ++a) {
                        rows[k][a][0] = itemFrame[a];
                        rows[k][a][1] = itemFrame[4 + a];
                        rows[k][a][2] = itemFrame[8 + a];
                        rows[k][a][3] = itemFrame[12 + a];
                    }
                    // The light direction, world to view (rotation only).
                    float v[3];
                    for (int i = 0; i < 3; ++i)
                        v[i] = h.light[0] * projection.view[i] + h.light[1] * projection.view[4 + i] + h.light[2] * projection.view[8 + i];
                    const float length = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
                    const bool lit = h.light[3] > 0.f && std::isfinite(length) && length > 1e-4f;
                    const float light[4] = {lit ? v[0] / length : 0.f, lit ? v[1] / length : 1.f, lit ? v[2] / length : 0.f,
                                            lit ? h.light[3] : 0.f};
                    memcpy(rows[k][3], light, sizeof light);
                    const float colour[4] = {h.lightColour[0], h.lightColour[1], h.lightColour[2],
                                             std::clamp(effectScale(strength, 0.8f, 1.f), 0.01f, 1.f)};
                    memcpy(rows[k][4], colour, sizeof colour);
                    memcpy(rows[k][5], h.blood, sizeof h.blood);
            memcpy(rows[k][9], h.condition, sizeof h.condition);
            rows[k][10][0] = h.object ? 2.f : h.apparel ? 1.f : 0.f;
            // z: the blood spots in use, so clean items skip the blood loop (spots fill from the front).
            rows[k][9][2] = 0.f;
            for (unsigned spot = 0; spot < 4; ++spot)
                if (h.blood[spot][3] > 0.f) rows[k][9][2] = float(spot + 1);
                    held = true;
                }
            }
            if (held) {
                g.activeTexture(glc::texture0 + heldNoiseUnit);
                noiseTouched = true;
                if (!prepareHeldNoise(g, r)) held = false;
                else g.bindTexture(texture3D, r.heldNoise);
            }
            if (held) {
                g.activeTexture(glc::texture0 + heldDataUnit);
                if (!r.heldData) {
                    g.genTextures(1, &r.heldData);
                    g.bindTexture(GL_TEXTURE_2D, r.heldData);
                    glTexImage2D(GL_TEXTURE_2D, 0, GLint(glc::rgba32f), 16, GLsizei(heldSlotLimit), 0, GL_RGBA, GL_FLOAT, nullptr);
                    g.texParameteri(GL_TEXTURE_2D, glc::minFilter, glc::nearest);
                    g.texParameteri(GL_TEXTURE_2D, glc::magFilter, glc::nearest);
                    g.texParameteri(GL_TEXTURE_2D, glc::wrapS, glc::clampToEdge);
                    g.texParameteri(GL_TEXTURE_2D, glc::wrapT, glc::clampToEdge);
                }
                g.bindTexture(GL_TEXTURE_2D, r.heldData);
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 16, GLsizei(heldSlotLimit), GL_RGBA, GL_FLOAT, rows);
            }
            // Always on their own units: a 3D sampler may never share a unit with the 2D ones.
            g.uniform1i(u.at[uHeldNoise], GLint(heldNoiseUnit));
            g.uniform1i(u.at[uHeldData], GLint(heldDataUnit));
            if (silhouette || held) {
                g.activeTexture(glc::texture2);
                g.bindTexture(GL_TEXTURE_2D, r.silhouetteMask);
            }
            g.uniform1i(u.at[uSilhouette], 2);
            g.uniform1f(u.at[uSilhouetteOn], silhouette ? 1.f : 0.f);
            g.uniform4f(u.at[uHeldDetail], held ? 1.f : 0.f, 0.f, 0.f, 0.f);
            // Level surfaces: world rows (the view matrix is rigid: its rotation transposed, its
            // translation taken back), the people's view-space centres, the strength and its fade.
            {
                const float* m = projection.view;
                float rows3[3][4];
                for (int j = 0; j < 3; ++j) {
                    rows3[j][0] = m[j * 4 + 0];
                    rows3[j][1] = m[j * 4 + 1];
                    rows3[j][2] = m[j * 4 + 2];
                    rows3[j][3] = -(m[12] * m[j * 4 + 0] + m[13] * m[j * 4 + 1] + m[14] * m[j * 4 + 2]);
                }
                g.uniform4f(u.at[uLevelRow0], rows3[0][0], rows3[0][1], rows3[0][2], rows3[0][3]);
                g.uniform4f(u.at[uLevelRow1], rows3[1][0], rows3[1][1], rows3[1][2], rows3[1][3]);
                g.uniform4f(u.at[uLevelRow2], rows3[2][0], rows3[2][1], rows3[2][2], rows3[2][3]);
                float people[6][4]{};
                for (unsigned k = 0; k < request.peopleCount && k < 6; ++k) {
                    // The same sector-local or global question as the held items: of the two readings,
                    // the one in reach of the camera.
                    float best = 600.f * 600.f;
                    for (int candidate = 0; candidate < 2; ++candidate) {
                        float w[3], c[3];
                        for (int i = 0; i < 3; ++i) w[i] = request.people[k][i] + (candidate ? request.peopleOrigin[k][i] : 0.f);
                        toView(projection.view, w, c);
                        const float sq = c[0] * c[0] + c[1] * c[1] + c[2] * c[2];
                        if (std::isfinite(sq) && sq < best) {
                            best = sq;
                            people[k][0] = c[0]; people[k][1] = c[1]; people[k][2] = c[2]; people[k][3] = 1.f;
                        }
                    }
                }
                const bool levelOn = request.level > 0.f && g.uniform4fv && projection.worldUpValid;
                if (g.uniform4fv) g.uniform4fv(u.at[uLevelPeople], 6, people[0]);
                // The lights, into view space by the reading (sector-local or global) in reach.
                float lamps[2][4]{}, lampColours[2][4]{};
                for (unsigned k = 0; k < 2; ++k) {
                    if (!(request.lamps[k][3] > 0.f)) continue;
                    float best = 2000.f * 2000.f;
                    for (int candidate = 0; candidate < 2; ++candidate) {
                        float w[3], c[3];
                        for (int i = 0; i < 3; ++i) w[i] = request.lamps[k][i] + (candidate ? request.lampOrigin[k][i] : 0.f);
                        toView(projection.view, w, c);
                        const float sq = c[0] * c[0] + c[1] * c[1] + c[2] * c[2];
                        if (std::isfinite(sq) && sq < best) {
                            best = sq;
                            lamps[k][0] = c[0]; lamps[k][1] = c[1]; lamps[k][2] = c[2]; lamps[k][3] = request.lamps[k][3];
                        }
                    }
                    lampColours[k][0] = request.lampColour[k][0];
                    lampColours[k][1] = request.lampColour[k][1];
                    lampColours[k][2] = request.lampColour[k][2];
                    lampColours[k][3] = 1.f;
                }
                if (g.uniform4fv) {
                    g.uniform4fv(u.at[uLevelLight], 2, lamps[0]);
                    g.uniform4fv(u.at[uLevelLightColour], 2, lampColours[0]);
                }
                g.uniform4f(u.at[uLevelDetail], levelOn ? std::clamp(effectScale(request.level, 0.5f, 1.f), 0.f, 1.f) : 0.f,
                            250.f, 300.f, silhouette || held ? 1.f : 0.f);
                if (levelOn && !held) { // the level reads the shared noise even when no held item does
                    g.activeTexture(glc::texture0 + heldNoiseUnit);
                    noiseTouched = true;
                    if (prepareHeldNoise(g, r)) g.bindTexture(texture3D, r.heldNoise);
                }
            }
            const float up[3] = {projection.worldUpValid ? projection.worldUpX : 0.f,
                                 projection.worldUpValid ? projection.worldUpY : 1.f,
                                 projection.worldUpValid ? projection.worldUpZ : 0.f};
            g.uniform4f(u.at[uWorldUp], up[0], up[1], up[2], 0.f);
            if (held) heldDetailFrames.fetch_add(1, std::memory_order_relaxed);
            g.uniform1f(u.at[uExposure], exposure);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
            // Wound skins for the next frame's body draws; the snapshot's restore covers its state.
            if (request.wounds.any()) bakeWoundSkins(g, r);
        }
        // The oval check reads the finished image with the same meter, on one frame in eight between the
        // native readings (frames 0 and 4 of each 8 read the native image).
        if (adaptMode && r.meterProgram && owner->viewAuditCountdown % 8 == 2) {
            g.useProgram(r.meterProgram);
            const auto& u = viewUniforms[contextIndex][programMeter];
            setViewUniforms(g, u, frame);
            g.uniform2f(u.at[uCells], float(meterWidth), float(meterHeight));
            requestMeter(g, r, *owner, target, viewport, true);
        }
        stampTo(5);
        ok = !audit || glGetError() == GL_NO_ERROR; // errors are looked for on audit frames
    } __finally {
        g.bindFramebuffer(glc::framebuffer, GLuint(expectedFramebuffer)); // entry required this framebuffer for drawing and reading
        setViewport(g, viewport[0], viewport[1], viewport[2], viewport[3]);
        g.useProgram(GLuint(program));
        g.bindVertexArray(GLuint(vao));
        g.bindBuffer(glc::arrayBuffer, GLuint(buffer));
        g.bindBuffer(glc::pixelPackBuffer, GLuint(pack));
        for (unsigned i = 0; i < 3; ++i)
            if (packStore[i]) glPixelStorei(glc::packRowLength + i, packStore[i]);
        for (unsigned unit = 0; unit < viewTextureUnits; ++unit) {
            g.activeTexture(glc::texture0 + unit);
            g.bindTexture(GL_TEXTURE_2D, GLuint(texture[unit]));
            if (g.bindSampler) g.bindSampler(unit, GLuint(sampler[unit]));
        }
        if (noiseTouched) {
            g.activeTexture(glc::texture0 + heldNoiseUnit);
            g.bindTexture(texture3D, GLuint(noise3D));
        }
        g.activeTexture(GLenum(activeUnit));
        glPolygonMode(glc::front, GLenum(polygon[0]));
        glPolygonMode(glc::backFace, GLenum(polygon[1]));
        if (g.indexedMasks) g.colorMaskIndexed(0, colorMask[0], colorMask[1], colorMask[2], colorMask[3]);
        else glColorMask(colorMask[0], colorMask[1], colorMask[2], colorMask[3]);
        for (unsigned i = 0; i < capCount; ++i)
            if (enabled[i]) cueEnable(g, caps[i], true);
        if (audit) drainGl();
    }
    if (halfFailed) {
        failViewTargets(*owner, "could not create the view-effect targets");
        return;
    }
    if (ok) {
        if (timed && stamped == 6) {
            r.timerIssued[timerSlot] = true;
            r.timerCall[timerSlot] = r.timerCalls;
            r.timerSlot = (timerSlot + 1) % 4;
        }
        if (temporal && (aoMode || ssrMode)) {
            owner->viewFrame = frameIndex + 1;
            owner->previousTick = now;
            memcpy(owner->previousView, projection.view, sizeof owner->previousView);
            memcpy(owner->previousProjection, frame.projection, sizeof owner->previousProjection);
        }
        if (skipped) return;
        skipViewEffects("");
        viewEffectsDraws.fetch_add(1, std::memory_order_relaxed);
        owner->viewRetries = 0;
        viewEffectsUnavailable.fetch_and(~unsigned(viewTargetsFailed), std::memory_order_relaxed);
        return;
    }
    // A GL error inside the passes: drop this context's view resources and stop
    // trying on it; the cue and headwear keep running.
    r.retireView(g);
    drainGl();
    failViewTargets(*owner, "OpenGL error while drawing");
}

// The player's real body through walls (third person, the ghost setting). The
// batch submissions of the player's body (playerBodyNode) during the main view's
// world pass are noted by the headwear submit hook. The camera pass runs its
// highlight pass (0x4F1D0) after every opaque and blended list; that call is
// detoured to replay the noted parts into a mask before the native pass: the
// native material bind (0x73F20), the native per-node submit (material +80,
// 0x201630) and the native part draw (part +90, 0x6B8B0), into a framebuffer of
// our own whose depth is a copy of the scene's, with depth GREATER and blending
// that writes 0 whatever the shader outputs. Every GL change the native calls make
// goes through the game's own state caches; our changes are restored raw.
// The main view's world pass, plain or Supersample (one camera pass per jittered sample);
// not the multi-view pass (sampleLast above 0), whose batch never submits singly.
// The same replay also draws the player's held items (weapons, a shield, a carried
// thing) where they are visible, depth LEQUAL, each into its own mask channel
// (G, B, A), for the held-item detail; the native calls are the same.
// The main view's offscreen-sampled world pass (see ViewRequest::offscreen): the camera
// pass draws into the game's sampling target, whose framebuffer is bound when the highlight-pass
// hook (silhouettePass) runs; it is noted here (0 = not seen) for the effects that run after the resolve.
static thread_local bool offscreenPassArmed = false;
static thread_local GLint offscreenSceneFramebuffer = 0, offscreenSceneSize[2]{}; // its drawn area: the samples' viewport size

constexpr unsigned silhouetteEntryLimit = 256;
// body: replay as the hidden body (R); slot >= 0: replay as held item slot (G + slot).
// sampled: noted from the multi-sample submit (offscreen sampling), which draws every sample itself.
struct SilhouetteEntry { uintptr_t node, material; int part; bool body; int slot; bool sampled; };
struct SilhouetteScope {
    bool armed, body, apparel; // body: the ghost wants the hidden body; apparel: the Armour row is on
    bool objects;              // the World object row is on
    uintptr_t root;
    unsigned count, heldCount, slotCount; // held items are slots 0..heldCount-1, body parts up to slotCount-1
    uintptr_t heldNodes[heldSlotLimit];   // each slot's frame node: the held mesh, or the body part's mesh
    uintptr_t slotParts[heldSlotLimit];   // the body part of an apparel slot, 0 for a held item
    uintptr_t slotPieces[heldSlotLimit];  // an apparel slot's first worn piece (its item record), 0 for a held item
    bool slotObjects[heldSlotLimit];      // the slot is a loose item or placed object (slotParts holds its node)
    float slotDistanceSq[heldSlotLimit];  // a worn-gear or world-object slot's squared distance from the player, cm²
    float centre[3];                      // the player's mesh, for the 15 m reach
    bool centreValid;
    SilhouetteEntry entries[silhouetteEntryLimit];
};
// Bits of the replay's result: the body was drawn, and each slot (heldMaskSlot << k).
constexpr uint64_t bodyMaskReady = 1, heldMaskSlot = 2;
static thread_local SilhouetteScope* silhouetteScope = nullptr; // this thread's main-view world pass
static thread_local uint64_t silhouetteMaskReady = 0;           // consumed by the next finish pass
// A VR eye's world draw is replayed through the same scene render, so its silhouette replay draws a mask in
// that eye's own view; the eye's finish pass then uses it and the desktop's never does (xr_stereo.inl sets
// viewEffectsEyeDraw around each eye's draw and its effects).
static thread_local bool viewEffectsEyeDraw = false, silhouetteMaskFromEye = false;
static thread_local HeldPose heldMaskPoses[heldSlotLimit];      // the held items' poses as the replay drew them
static bool silhouetteAdmitted = false, silhouetteHooked = false;
static std::atomic<unsigned> silhouetteReplays{0}, silhouetteFaults{0};
using SilhouettePass = void (*)(uintptr_t);
using MaterialBind = unsigned (*)(uintptr_t);
using MaterialSubmit = void (*)(uintptr_t, int);
using SingleSubmit = void (*)(uintptr_t, uintptr_t, int);
using PartDraw = void (*)(uintptr_t);
static SilhouettePass originalSilhouettePass = nullptr;
static bool silhouetteSubmitHooked(); // headwear_render.inl: the submit hook that notes the parts
static std::atomic<ULONGLONG> headwearSeen{0}; // headwear_render.inl: last tick a worn head piece was drawn or hidden

// A piece of armour or clothing: a node hung on a character's body part that is
// neither the limb's own mesh nor the skin aggregate; its part, else 0.
static uintptr_t apparelPartOf(uintptr_t node) {
    if (!readable(node, field::sceneNode::name + 16)) return 0;
    const uintptr_t part = *(const uintptr_t*)(node + field::sceneNode::bodyPart);
    if (!part || !readable(part, field::bodyPart::character + 8)) return 0;
    if (*(const uintptr_t*)(part + field::bodyPart::meshNode) == node || !*(const uintptr_t*)(part + field::bodyPart::character))
        return 0;
    if (!strncmp((const char*)(node + field::sceneNode::name), "_SKIN", 5)) return 0;
    return part;
}

// A loose item or placed object the node belongs to: the nearest ancestor (or itself, within six
// levels) whose scene flags mark an item ((+8 & 0x7000) == 0x4000) or an object (0x2000), stopping
// at a sector; 0 for level geometry or anything worn (a node hung on a body part).
static uintptr_t objectRootOf(uintptr_t node) {
    // A character's skin aggregate hangs under its actor node, which can carry object flags.
    if (!readable(node, field::sceneNode::name + 16) || !strncmp((const char*)(node + field::sceneNode::name), "_SKIN", 5))
        return 0;
    for (int depth = 0; node && depth < 6; ++depth) {
        if (!readable(node, field::sceneNode::bodyPart + 8)) return 0;
        const unsigned flags = *(const unsigned*)(node + field::sceneNode::flags);
        if ((flags & 0xfff) == 0x11) return 0;
        const uintptr_t body = *(const uintptr_t*)(node + field::sceneNode::bodyPart);
        if (body && readable(body, field::bodyPart::character + 8) && *(const uintptr_t*)(body + field::bodyPart::character))
            return 0;
        if ((flags & 0x7000) == 0x4000 || (flags & 0x2000)) return node;
        node = *(const uintptr_t*)(node + field::sceneNode::parent);
    }
    return 0;
}

// A node's slot: a held item (one of the held mesh nodes, or hung straight under one),
// else a body part already given a slot this pass; -1 otherwise.
static int heldSlotOf(const SilhouetteScope& s, uintptr_t node) {
    if (!s.slotCount || !readable(node, field::sceneNode::parent + 8)) return -1;
    const uintptr_t parent = *(const uintptr_t*)(node + field::sceneNode::parent);
    for (unsigned k = 0; k < s.heldCount; ++k)
        if (node == s.heldNodes[k] || parent == s.heldNodes[k]) return int(k);
    if (s.slotCount == s.heldCount) return -1;
    const uintptr_t part = s.apparel ? apparelPartOf(node) : 0;
    for (unsigned k = s.heldCount; part && k < s.slotCount; ++k)
        if (!s.slotObjects[k] && s.slotParts[k] == part) return int(k);
    const uintptr_t object = s.objects && !part ? objectRootOf(node) : 0;
    for (unsigned k = s.heldCount; object && k < s.slotCount; ++k)
        if (s.slotObjects[k] && s.slotParts[k] == object) return int(k);
    return -1;
}

// A character's held things: carried weapons, what each hand holds, the shield.
// Their mesh found, at most five.
static unsigned heldPartsOf(uintptr_t character, uintptr_t* found) {
    if (!readable(character, field::character::heldItems + 16) || !readable(character, field::character::shieldPart + 8))
        return 0;
    uintptr_t parts[5]{};
    const int weapons = std::clamp(*(const int*)(character + field::character::weaponCount), 0, 2);
    for (int i = 0; i < weapons; ++i) parts[i] = *(const uintptr_t*)(character + field::character::weapons + size_t(i) * 8);
    parts[2] = *(const uintptr_t*)(character + field::character::heldItems);
    parts[3] = *(const uintptr_t*)(character + field::character::heldItems + 8);
    parts[4] = *(const uintptr_t*)(character + field::character::shieldPart);
    unsigned count = 0;
    for (uintptr_t part : parts) {
        if (!part || !readable(part, field::bodyPart::meshNode + 8)) continue;
        const uintptr_t node = *(const uintptr_t*)(part + field::bodyPart::meshNode);
        if (!readable(node, field::sceneNode::renderTransform + 64)) continue;
        bool seen = false;
        for (unsigned k = 0; k < count; ++k) seen = seen || found[k] == node;
        if (!seen) found[count++] = node;
    }
    return count;
}

static bool sectorOriginOf(uintptr_t node, float* origin);
// A node's drawn origin with its sector's origin added; false when unreadable.
static bool heldGlobalOrigin(uintptr_t node, float* out) {
    const float* sampled = (const float*)(node + field::sceneNode::renderTransform);
    float origin[3];
    if (!readable(node, field::sceneNode::renderTransform + 64) || !sectorOriginOf(node, origin)) return false;
    for (int i = 0; i < 3; ++i) out[i] = sampled[12 + i] + origin[i];
    return std::isfinite(out[0] + out[1] + out[2]);
}

// The player's held things first, then other characters' (the world's roster, the
// same read the third-person blocker test makes), nearest first within 6 m, up to
// heldSlotLimit in all. Only the ones the world pass actually draws reach the mask.
// Others' held items and worn gear, and world objects, count only this near: each costs a replay draw.
constexpr float heldOthersReachCm = 600.f, objectReachCm = 600.f;
// Worn gear and world objects add at most this many replay draws a frame, nearest first.
constexpr unsigned replayBudget = 12;
static std::atomic<unsigned> replayBudgetCut{0}; // draws left out by the budget, all frames
static std::atomic<unsigned> heldMaskReused{0};  // frames that drew no mask and kept the last one
static std::atomic<unsigned> heldObjects{0}; // loose items and placed objects given a slot, all passes
static std::atomic<unsigned> heldOthers{0};
static void collectHeldNodes(SilhouetteScope& s) {
    s.heldCount = 0;
    uintptr_t found[5];
    const unsigned own = heldPartsOf(s.root, found);
    for (unsigned k = 0; k < own && s.heldCount < heldItemLimit; ++k) s.heldNodes[s.heldCount++] = found[k];
    s.slotCount = s.heldCount;
    float* const centre = s.centre;
    if (!s.centreValid) return;
    const uintptr_t world = *(const uintptr_t*)gameAt(fp::sig::world);
    if (!readable(world, field::worldObject::characters + 8)) return;
    const uintptr_t roster = *(const uintptr_t*)(world + field::worldObject::characters);
    if (!readable(roster, field::list::items + 8)) return;
    const int characters = *(const int*)(roster + field::list::count);
    const uintptr_t entries = *(const uintptr_t*)(roster + field::list::items);
    if (characters <= 0 || characters > 4096 || !readable(entries, size_t(characters) * 8)) return;
    struct Candidate { uintptr_t node; float distanceSq; };
    Candidate closest[16];
    unsigned nearCount = 0;
    for (int i = 0; i < characters; ++i) {
        const uintptr_t character = *(const uintptr_t*)(entries + size_t(i) * 8);
        if (!character || character == s.root) continue;
        const unsigned count = heldPartsOf(character, found);
        for (unsigned k = 0; k < count; ++k) {
            float at[3];
            if (!heldGlobalOrigin(found[k], at)) continue;
            const float d[3] = {at[0] - centre[0], at[1] - centre[1], at[2] - centre[2]};
            const float distanceSq = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
            if (!(distanceSq < heldOthersReachCm * heldOthersReachCm)) continue;
            // Keep the nearest sixteen, by insertion.
            const bool full = nearCount == 16;
            if (full && closest[15].distanceSq <= distanceSq) continue;
            unsigned at16 = full ? 15 : nearCount++;
            while (at16 > 0 && closest[at16 - 1].distanceSq > distanceSq) {
                closest[at16] = closest[at16 - 1];
                --at16;
            }
            closest[at16] = {found[k], distanceSq};
        }
    }
    unsigned others = 0;
    for (unsigned k = 0; k < nearCount && s.heldCount < heldItemLimit; ++k) {
        bool seen = false;
        for (unsigned j = 0; j < s.heldCount; ++j) seen = seen || s.heldNodes[j] == closest[k].node;
        if (!seen) {
            s.heldNodes[s.heldCount++] = closest[k].node;
            ++others;
        }
    }
    s.slotCount = s.heldCount;
    heldOthers.store(others, std::memory_order_relaxed);
}

// From the headwear submit hook, for batch submissions only (0x26F4D0's own call).
static void noteSilhouetteSubmission(uintptr_t node, int part, bool sampled) {
    SilhouetteScope* const s = silhouetteScope;
    if (!s || !s->armed || cueGlDepth || s->count == silhouetteEntryLimit || part < 0 || part >= 256) return;
    __try {
        // The single path draws one sample per submit; offscreen sampling's submit draws all of them.
        if ((*(const int*)gameAt(fp::sig::sampleLast) != 0) != sampled || (sampled && !offscreenPassArmed)) return;
        int slot = heldSlotOf(*s, node);
        // A body part's first armour or clothing piece this pass gives that part a slot (within 6 m).
        if (slot < 0 && s->apparel && s->slotCount < heldSlotLimit) {
            const uintptr_t apparelPart = apparelPartOf(node);
            const uintptr_t mesh = apparelPart ? *(const uintptr_t*)(apparelPart + field::bodyPart::meshNode) : 0;
            float at[3];
            if (mesh && s->centreValid && heldGlobalOrigin(mesh, at)) {
                const float d[3] = {at[0] - s->centre[0], at[1] - s->centre[1], at[2] - s->centre[2]};
                if (d[0] * d[0] + d[1] * d[1] + d[2] * d[2] < heldOthersReachCm * heldOthersReachCm) {
                    s->heldNodes[s->slotCount] = mesh;
                    s->slotParts[s->slotCount] = apparelPart;
                    s->slotPieces[s->slotCount] = node;
                    s->slotObjects[s->slotCount] = false;
                    s->slotDistanceSq[s->slotCount] = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
                    slot = int(s->slotCount++);
                }
            }
        }
        // A loose item's or placed object's first piece this pass gives it a slot (within 6 m).
        if (slot < 0 && s->objects && s->slotCount < heldSlotLimit) {
            const uintptr_t object = objectRootOf(node);
            float at[3];
            if (object && s->centreValid && heldGlobalOrigin(object, at)) {
                const float d[3] = {at[0] - s->centre[0], at[1] - s->centre[1], at[2] - s->centre[2]};
                if (d[0] * d[0] + d[1] * d[1] + d[2] * d[2] < objectReachCm * objectReachCm) {
                    s->heldNodes[s->slotCount] = object;
                    s->slotParts[s->slotCount] = object;
                    s->slotPieces[s->slotCount] = object;
                    s->slotObjects[s->slotCount] = true;
                    s->slotDistanceSq[s->slotCount] = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
                    slot = int(s->slotCount++);
                    heldObjects.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }
        const bool body = s->body && playerBodyNode(s->root, node);
        if (slot < 0 && !body) return;
        for (unsigned k = 0; k < s->count; ++k)
            if (s->entries[k].node == node && s->entries[k].part == part) return;
        const uintptr_t material = *(const uintptr_t*)gameAt(fp::sig::currentMaterial);
        if (!readable(material, 0x88)) return;
        s->entries[s->count++] = {node, material, part, body, slot, sampled};
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        silhouetteFaults.fetch_add(1, std::memory_order_relaxed);
    }
}

// Mask and depth copy sized to the viewport; leaves the active unit's binding to the caller.
static bool prepareSilhouetteTargets(gl::Api& g, fp::CueResources& r, int width, int height) {
    if (r.silhouetteFbo && r.silhouetteWidth == width && r.silhouetteHeight == height) return true;
    r.retireSilhouette(g);
    g.genTextures(1, &r.silhouetteMask);
    g.genTextures(1, &r.silhouetteDepth);
    g.genFramebuffers(1, &r.silhouetteFbo);
    if (!r.silhouetteMask || !r.silhouetteDepth || !r.silhouetteFbo) {
        r.retireSilhouette(g);
        return false;
    }
    g.bindTexture(GL_TEXTURE_2D, r.silhouetteMask);
    glTexImage2D(GL_TEXTURE_2D, 0, GLint(glc::rgba8), width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    g.texParameteri(GL_TEXTURE_2D, glc::minFilter, glc::nearest);
    g.texParameteri(GL_TEXTURE_2D, glc::magFilter, glc::nearest);
    g.texParameteri(GL_TEXTURE_2D, glc::wrapS, glc::clampToEdge);
    g.texParameteri(GL_TEXTURE_2D, glc::wrapT, glc::clampToEdge);
    g.bindTexture(GL_TEXTURE_2D, r.silhouetteDepth);
    glTexImage2D(GL_TEXTURE_2D, 0, GLint(glc::depthComponent24), width, height, 0, glc::depthComponent, GL_UNSIGNED_INT, nullptr);
    g.texParameteri(GL_TEXTURE_2D, glc::minFilter, glc::nearest);
    g.texParameteri(GL_TEXTURE_2D, glc::magFilter, glc::nearest);
    g.bindFramebuffer(glc::framebuffer, r.silhouetteFbo);
    g.framebufferTexture2D(glc::framebuffer, glc::colorAttachment0, GL_TEXTURE_2D, r.silhouetteMask, 0);
    g.framebufferTexture2D(glc::framebuffer, glc::depthAttachment, GL_TEXTURE_2D, r.silhouetteDepth, 0);
    const bool complete = g.checkFramebufferStatus(glc::framebuffer) == glc::framebufferComplete;
    g.bindFramebuffer(glc::framebuffer, 0);
    if (!complete || glGetError() != GL_NO_ERROR) {
        r.retireSilhouette(g);
        return false;
    }
    r.silhouetteWidth = width;
    r.silhouetteHeight = height;
    return true;
}

// One noted part, re-checked: still the player's (body) or still held in its slot,
// still drawn by the reviewed functions.
static bool silhouettePart(const SilhouetteScope& s, const SilhouetteEntry& e, uintptr_t& part, bool& body, bool& held) {
    if (!readable(e.node, 0x1d8) || (*(const unsigned*)(e.node + 8) & 0x80000000u)) return false;
    body = e.body && playerBodyNode(s.root, e.node);
    held = e.slot >= 0 && heldSlotOf(s, e.node) == e.slot;
    if (!body && !held) return false;
    const int last = *(const int*)(e.node + 0x1c8);
    const uintptr_t table = *(const uintptr_t*)(e.node + 0x1d0);
    if (e.part > last || last >= 256 || !readable(table, size_t(last + 1) * 8)) return false;
    part = *(const uintptr_t*)(table + size_t(e.part) * 8);
    // Both paths are replayed as one sample through part +90 (see the replay loop).
    return readable(part, 0xa0) && *(const uintptr_t*)(part + 0x90) == gameAt(fp::sig::headwearDraw) &&
           readable(e.material, 0x88) && *(const uintptr_t*)(e.material + 0x80) == gameAt(fp::sig::headwearSubmit);
}

// A held part's material, from the average colour of the texture its material
// binds on unit 0 (unlit, so torchlight cannot fool it), as the mask code the
// finish shader reads: 0 unknown, 0.15 steel, 0.25 wood, 0.35 leather, 0.45 other.
// Read back once per texture from its smallest mip level; cueLock held.
struct HeldTextureKind { GLuint texture; GLint width; float code; };
static HeldTextureKind heldTextureKinds[128];
static unsigned heldTextureKindNext = 0;
static std::atomic<unsigned> heldTextureReads{0};
static float heldMaterialCode(gl::Api& g) {
    GLint unit = 0, texture = 0, packBuffer = 0, pack[3]{};
    glGetIntegerv(glc::activeTexture, &unit);
    g.activeTexture(glc::texture0);
    glGetIntegerv(glc::textureBinding2D, &texture);
    float code = 0.f;
    __try {
        glGetIntegerv(glc::pixelPackBufferBinding, &packBuffer);
        glGetIntegerv(glc::packRowLength, &pack[0]);
        glGetIntegerv(glc::packSkipRows, &pack[1]);
        glGetIntegerv(glc::packSkipPixels, &pack[2]);
        GLint width = 0, height = 0;
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &width);
        if (glGetError() != GL_NO_ERROR || texture <= 0 || width <= 0) __leave;
        for (const HeldTextureKind& k : heldTextureKinds)
            if (k.texture == GLuint(texture) && k.width == width) {
                code = k.code;
                __leave;
            }
        // Writing into a bound pack buffer or with offsets would land elsewhere: leave it unknown.
        if (packBuffer || pack[0] || pack[1] || pack[2]) __leave;
        // The deepest level that exists, or the smallest one up to 64 x 64.
        int level = 0;
        for (int l = 1; l < 16; ++l) {
            GLint w = 0, h = 0;
            glGetTexLevelParameteriv(GL_TEXTURE_2D, l, GL_TEXTURE_WIDTH, &w);
            glGetTexLevelParameteriv(GL_TEXTURE_2D, l, GL_TEXTURE_HEIGHT, &h);
            if (w <= 0 || h <= 0) break;
            level = l;
            if (w * h <= 256 * 256) break; // fine enough to keep a mail texture's rings
        }
        glGetTexLevelParameteriv(GL_TEXTURE_2D, level, GL_TEXTURE_WIDTH, &width);
        glGetTexLevelParameteriv(GL_TEXTURE_2D, level, GL_TEXTURE_HEIGHT, &height);
        if (glGetError() != GL_NO_ERROR || width <= 0 || height <= 0 || width * height > 1024 * 1024) __leave;
        static unsigned char pixels[1024 * 1024 * 4];
        glGetTexImage(GL_TEXTURE_2D, level, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
        if (glGetError() != GL_NO_ERROR) __leave;
        heldTextureReads.fetch_add(1, std::memory_order_relaxed);
        double sum[3]{}, contrast = 0;
        unsigned count = 0;
        for (int i = 0; i < width * height; ++i) {
            const unsigned char* px = pixels + size_t(i) * 4;
            if (px[3] < 128) continue; // cut-out holes
            for (int c = 0; c < 3; ++c) sum[c] += px[c] / 255.0;
            if ((i + 1) % width) contrast += std::abs(int(px[1]) - int(px[5])) / 255.0; // to the right neighbour
            ++count;
        }
        if (count) {
            const float r = float(sum[0] / count), gr = float(sum[1] / count), b = float(sum[2] / count);
            const float hi = (std::max)(r, (std::max)(gr, b)), lo = (std::min)(r, (std::min)(gr, b));
            const float saturation = hi > 1e-4f ? (hi - lo) / hi : 0.f;
            const float luma = 0.2126f * r + 0.7152f * gr + 0.0722f * b;
            // PROVISIONAL(2026-10-22, mail told from plate by its rings' fine contrast; the 0.06
            // threshold is a guess until mail and plate textures are compared, ratify-by: 2026-11-05)
            if (saturation < 0.22f) code = contrast / count > 0.06 ? 0.55f : 0.15f; // grey: mail or steel
            // Warm brown (not a strong red or a paint): leather when dark, else wood.
            else if (r > b * 1.2f && gr > r * 0.45f && saturation < 0.8f) code = luma < 0.22f ? 0.35f : 0.25f;
            else code = 0.45f;                                     // cloth, paint, anything else
        }
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &width);
        heldTextureKinds[heldTextureKindNext++ % 128] = {GLuint(texture), width, code};
    } __finally {
        g.activeTexture(GLenum(unit));
        drainGl();
    }
    return code;
}

// The same, remembered per native material (and the texture object in its first
// slot, +0x48, so a reused material address re-reads): a known part costs no GL
// queries at all, only a lookup.
struct HeldMaterialKind { uintptr_t material, texture; float code; };
static HeldMaterialKind heldMaterialKinds[64];
static unsigned heldMaterialKindNext = 0;
static float heldMaterialCodeFor(gl::Api& g, uintptr_t material) {
    const uintptr_t texture = readable(material, 0x50) ? *(const uintptr_t*)(material + 0x48) : 0;
    for (const HeldMaterialKind& k : heldMaterialKinds)
        if (k.material == material && k.texture == texture) return k.code;
    const float code = heldMaterialCode(g);
    if (code > 0.f) heldMaterialKinds[heldMaterialKindNext++ % 64] = {material, texture, code};
    return code;
}

// The game lights (TLightSource, scene kind 5) near a held item, read only, for its
// surface's light direction. Static reading only (review:
// work/verification/excamera-held-light-review.md): placed and dynamic lights hang
// in a sector's child list, directly or under a placed object, with colour +0x210,
// intensity +0x21c, range +0x224 (cm), owner sector +0x130, and the propagated
// sector-local origin +0x64 the renderer uses. Each sector keeps its own list (four
// sectors cached), rebuilt every half second; each frame re-checks every kept node's
// kind before reading it. Never written, never retained across levels
// beyond a pointer that is re-validated before use.
constexpr unsigned heldLightLimit = 24;
struct LightList { uintptr_t sector; ULONGLONG tick; unsigned count; uintptr_t nodes[heldLightLimit]; };
static LightList lightLists[4];
static std::atomic<unsigned> heldLightsSeen{0};
constexpr size_t lightColour = 0x210, lightIntensity = 0x21c, lightRange = 0x224, lightSize = 0x2e8;

static bool heldLightNode(uintptr_t node) {
    return readable(node, lightSize) && (*(const unsigned*)(node + field::sceneNode::flags) & 0xfff) == 5;
}

static void collectHeldLights(LightList& list, uintptr_t sector) {
    unsigned& heldLightCount = list.count;
    uintptr_t* const heldLightNodes = list.nodes;
    heldLightCount = 0;
    list.sector = sector;
    list.tick = GetTickCount64();
    if (!readable(sector, field::sceneNode::children + 8)) return;
    unsigned visits = 0;
    // The sector's children, and one level under each (a wall lamp's flame under its object).
    const int last = *(const int*)(sector + field::sceneNode::lastChild);
    const uintptr_t children = *(const uintptr_t*)(sector + field::sceneNode::children);
    if (last < 0 || last >= 8192 || !readable(children, size_t(last + 1) * 8)) return;
    for (int i = 0; i <= last && heldLightCount < heldLightLimit && visits < 8192; ++i, ++visits) {
        const uintptr_t child = *(const uintptr_t*)(children + size_t(i) * 8);
        if (!child || !readable(child, field::sceneNode::children + 8)) continue;
        if (heldLightNode(child)) {
            heldLightNodes[heldLightCount++] = child;
            continue;
        }
        const int lastInner = *(const int*)(child + field::sceneNode::lastChild);
        const uintptr_t inner = *(const uintptr_t*)(child + field::sceneNode::children);
        if (lastInner < 0 || lastInner >= 64 || !readable(inner, size_t(lastInner + 1) * 8)) continue;
        for (int j = 0; j <= lastInner && heldLightCount < heldLightLimit && visits < 8192; ++j, ++visits) {
            const uintptr_t grand = *(const uintptr_t*)(inner + size_t(j) * 8);
            if (grand && heldLightNode(grand)) heldLightNodes[heldLightCount++] = grand;
        }
    }
    heldLightsSeen.store(heldLightCount, std::memory_order_relaxed);
}

// A sector's light list: its own entry, rebuilt when half a second old; a sector not cached
// takes the oldest entry.
static const LightList& lightsOf(uintptr_t sector) {
    const ULONGLONG now = GetTickCount64();
    LightList* entry = nullptr;
    for (LightList& list : lightLists)
        if (list.sector == sector) entry = &list;
    if (!entry) {
        entry = &lightLists[0];
        for (LightList& list : lightLists)
            if (list.tick < entry->tick) entry = &list;
        entry->tick = 0;
    }
    if (now - entry->tick > 500) collectHeldLights(*entry, sector);
    return *entry;
}

static bool sectorOriginOf(uintptr_t node, float* origin) {
    const uintptr_t sector = readable(node, field::sceneNode::sector + 8) ? *(const uintptr_t*)(node + field::sceneNode::sector) : 0;
    if (!readable(sector, field::sector::origin + 12)) return false;
    memcpy(origin, (const void*)(sector + field::sector::origin), 12);
    return true;
}

// Inside the replay (game thread, cueLock held): the weighted direction to the lights
// around the held node, as the item's sector sees them.
static void heldLightFor(uintptr_t node, uintptr_t sector, HeldPose& h) {
    memset(h.light, 0, sizeof h.light);
    h.lightColour[0] = h.lightColour[1] = h.lightColour[2] = 1.f;
    const LightList& list = lightsOf(sector);
    float item[3];
    for (int i = 0; i < 3; ++i) item[i] = h.pose[9 + i] + h.sectorOrigin[i];
    // The sector's lights, then any hung on the held item itself (a carried torch's flame).
    uintptr_t lights[heldLightLimit + 16];
    unsigned count = 0;
    for (unsigned k = 0; k < list.count; ++k) lights[count++] = list.nodes[k];
    if (readable(node, field::sceneNode::children + 8)) {
        const int last = *(const int*)(node + field::sceneNode::lastChild);
        const uintptr_t children = *(const uintptr_t*)(node + field::sceneNode::children);
        if (last >= 0 && last < 16 && readable(children, size_t(last + 1) * 8))
            for (int i = 0; i <= last; ++i) {
                const uintptr_t child = *(const uintptr_t*)(children + size_t(i) * 8);
                bool known = false;
                for (unsigned k = 0; k < count; ++k) known = known || lights[k] == child;
                if (child && !known && heldLightNode(child)) lights[count++] = child;
            }
    }
    float sum[3]{}, colour[3]{}, weights = 0.f;
    for (unsigned k = 0; k < count; ++k) {
        const uintptr_t light = lights[k];
        if (!heldLightNode(light)) continue;
        float origin[3];
        if (!sectorOriginOf(light, origin)) continue;
        const float* at = (const float*)(light + field::sceneNode::transformOrigin);
        const float* c = (const float*)(light + lightColour);
        const float intensity = *(const float*)(light + lightIntensity), range = *(const float*)(light + lightRange);
        float d[3];
        for (int i = 0; i < 3; ++i) d[i] = at[i] + origin[i] - item[i];
        const float distance = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        const float luma = 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2];
        if (!std::isfinite(distance + intensity + range + luma) || distance < 1.f || intensity <= 0.f || luma <= 0.f)
            continue;
        float reach = range > 1.f ? 1.f - distance / range : 1.f / (1.f + distance * distance / 250000.f);
        if (reach <= 0.f) continue;
        const float w = (std::min)(intensity, 8.f) * luma * reach * reach;
        for (int i = 0; i < 3; ++i) {
            sum[i] += w * d[i] / distance;
            colour[i] += w * c[i];
        }
        weights += w;
    }
    const float length = std::sqrt(sum[0] * sum[0] + sum[1] * sum[1] + sum[2] * sum[2]);
    if (!(weights > 0.f) || !(length > 1e-4f) || !std::isfinite(length)) return;
    for (int i = 0; i < 3; ++i) h.light[i] = sum[i] / length;
    h.light[3] = (std::min)(weights * 2.f, 1.f);
    const float brightest = (std::max)(colour[0], (std::max)(colour[1], colour[2]));
    if (brightest > 0.f)
        for (int i = 0; i < 3; ++i) h.lightColour[i] = colour[i] / brightest;
}

// An item's condition, read-only from its record, the way FUN_100103db0 finds it: the id at node
// +0x180 (written by the item factory 13FB60; 1316D0/1523F0 read a weapon's the same way) selects a
// database (plain ids itemDatabase, 0x40000000 worldItemDatabase, 0x80000000 levelItemDatabase);
// 104660 binary-searches its 16-byte rows (count +8, rows +0x10, id at +0); 104b10 takes the
// row's offset (+8) and size (+0xC) into the data at +0x18; 103a90 walks the record's chunks (u32
// tag, u32 size including the header). Weapons (0x4000): wear +0x15, dirt +0x16, quality +0x14;
// apparel (0x1000): +0x1B9, +0x1BA, +0x1B8 (11C580 reads them so). Shields have no record bytes.
// Every read is guarded; nothing is called or written.
static std::atomic<unsigned> heldConditionReads{0};
static void heldConditionOf(uintptr_t node, bool apparel, float* out) {
    out[0] = out[1] = out[2] = out[3] = 0.f;
    if (!node || !layout.optionalGroupAvailable("itemCondition") || !readable(node, 0x184)) return;
    const uint32_t id = *(const uint32_t*)(node + 0x180);
    if (!id) return;
    const fp::sig::Target table = (id & 0x80000000u) ? fp::sig::levelItemDatabase
                                  : (id & 0x40000000u) ? fp::sig::worldItemDatabase : fp::sig::itemDatabase;
    const uintptr_t global = gameAt(table);
    if (!global || !readable(global, 8)) return;
    const uintptr_t db = *(const uintptr_t*)global;
    if (!readable(db, 0x20)) return;
    const int count = *(const int*)(db + 8);
    const uintptr_t rows = *(const uintptr_t*)(db + 0x10), data = *(const uintptr_t*)(db + 0x18);
    const int key = int((id & 0xC0000000u) ? id & 0x3FFFFFFFu : id);
    if (count <= 0 || count > (1 << 20) || !readable(rows, size_t(count) * 0x10)) return;
    int lo = 0, hi = count - 1, row = -1;
    while (lo <= hi) {
        const int mid = lo + (hi - lo) / 2;
        const int rowId = *(const int*)(rows + size_t(mid) * 0x10);
        if (key < rowId) hi = mid - 1;
        else if (key > rowId) lo = mid + 1;
        else { row = mid; break; }
    }
    if (row < 0) return;
    const uint32_t offset = *(const uint32_t*)(rows + size_t(row) * 0x10 + 8), size = *(const uint32_t*)(rows + size_t(row) * 0x10 + 0xC);
    const uintptr_t record = data + offset;
    if (!size || size > (1u << 20) || !readable(record, size)) return;
    const uint32_t tag = apparel ? 0x1000u : 0x4000u;
    const size_t at = apparel ? 0x1B8 : 0x14;
    for (uintptr_t chunk = record; chunk + 8 <= record + size;) {
        const uint32_t chunkTag = *(const uint32_t*)chunk, chunkSize = *(const uint32_t*)(chunk + 4);
        if (chunkSize < 8) return;
        if (chunkTag == tag) {
            if (chunk + at + 3 > record + size) return;
            const unsigned char* b = (const unsigned char*)(chunk + at);
            out[0] = b[1] / 255.f; // wear
            out[1] = b[2] / 255.f; // dirt
            out[2] = b[0] / 255.f; // quality
            out[3] = 1.f;
            heldConditionReads.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        chunk += chunkSize;
    }
}

// Each slot node's light and condition, kept for 500 ms (a small ring, newest replacing oldest).
struct HeldNodeCache { uintptr_t node; ULONGLONG tick; float light[4], lightColour[3], condition[4]; };
static HeldNodeCache heldNodeCaches[64];
static unsigned heldNodeCacheNext = 0;
static HeldNodeCache& heldNodeCache(uintptr_t node) {
    for (HeldNodeCache& c : heldNodeCaches)
        if (c.node == node) return c;
    HeldNodeCache& c = heldNodeCaches[heldNodeCacheNext++ % 64];
    c = {};
    c.node = node;
    return c;
}

using BlendColor = void(APIENTRY*)(GLfloat, GLfloat, GLfloat, GLfloat);
static BlendColor heldBlendColor = nullptr;

// cueLock held, inside the camera pass, default framebuffer bound. Returns the
// bodyMaskReady / heldMaskSlot bits of what was drawn.
static uint64_t drawSilhouetteMaskOwned(const SilhouetteScope& s) {
    GLint framebuffer = -1, readFramebuffer = -1, viewport[4]{};
    drainGl();
    glGetIntegerv(glc::drawFramebufferBinding, &framebuffer);
    glGetIntegerv(glc::readFramebufferBinding, &readFramebuffer);
    glGetIntegerv(GL_VIEWPORT, viewport);
    RECT client{};
    // Offscreen sampling draws into the game's sampling target with one viewport per sample
    // (offset by a pixel); the mask takes the target's size and is drawn as sample 0.
    const GLint expected = offscreenPassArmed ? offscreenSceneFramebuffer : 0;
    if (glGetError() != GL_NO_ERROR || framebuffer != expected || readFramebuffer != expected ||
        (offscreenPassArmed && !expected) || viewport[2] < 32 || viewport[3] < 32)
        return false;
    if (!offscreenPassArmed && (!hookedWindow || !GetClientRect(hookedWindow, &client) || viewport[0] != 0 ||
                                viewport[1] != 0 || viewport[2] != client.right || viewport[3] != client.bottom))
        return false;
    auto* owner = prepareCue();
    if (!owner) return false;
    auto& g = owner->api;
    auto& r = owner->resources;
    if (!owner->viewApiAttempted) {
        owner->viewApiAttempted = true;
        owner->viewApiFailed = !loadViewEffectsApi(g);
        if (owner->viewApiFailed) noteViewEffectFailure(viewApiMissing, "OpenGL 3.2 entry points missing");
    }
    if (owner->viewApiFailed || owner->viewTargetsFailed || !cueOutputReady(g)) return false;

    // Everything changed below without the game's caches, read first.
    GLint unitTexture = 0, depthFunc = 0, blend[6]{};
    GLboolean depthMask = 0, colorMask[4]{}, depthTest = 0, blending = 0, scissor = 0, offsetFill = 0;
    GLfloat clear[4]{}, offset[2]{}, blendColor[4]{};
    if (!heldBlendColor) heldBlendColor = (BlendColor)wglGetProcAddress("glBlendColor");
    glGetFloatv(0x8005 /*BLEND_COLOR*/, blendColor);
    offsetFill = glIsEnabled(GL_POLYGON_OFFSET_FILL);
    glGetFloatv(GL_POLYGON_OFFSET_FACTOR, &offset[0]);
    glGetFloatv(GL_POLYGON_OFFSET_UNITS, &offset[1]);
    glGetIntegerv(glc::textureBinding2D, &unitTexture);
    glGetIntegerv(glc::depthFunc, &depthFunc);
    glGetBooleanv(glc::depthWritemask, &depthMask);
    glGetBooleanv(GL_COLOR_WRITEMASK, colorMask);
    glGetFloatv(glc::colorClearValue, clear);
    depthTest = glIsEnabled(GL_DEPTH_TEST);
    scissor = glIsEnabled(GL_SCISSOR_TEST);
    const GLenum blendQueries[6] = {glc::blendSrcRgb, glc::blendDstRgb, glc::blendSrcAlpha, glc::blendDstAlpha,
                                    glc::blendEquationRgb, glc::blendEquationAlpha};
    auto readBlend = [&](GLboolean& on, GLint* values) {
        on = glIsEnabled(GL_BLEND);
        for (unsigned i = 0; i < 6; ++i) glGetIntegerv(blendQueries[i], &values[i]);
    };
    readBlend(blending, blend);
    if (glGetError() != GL_NO_ERROR) return false;
    const auto bind = (MaterialBind)gameAt(fp::sig::materialBind);
    const auto single = (SingleSubmit)gameAt(fp::sig::silhouetteSingleSubmit);
    bool failed = false;
    uint64_t drawn = 0;
    __try {
        __try {
            if (!prepareSilhouetteTargets(g, r, viewport[2], viewport[3])) {
                failed = true;
                __leave;
            }
            // The scene depth as it stands after every opaque list. The sampling target's own depth
            // renderbuffer can serve directly (depth writes stay off below), saving a copy; the
            // default framebuffer's depth cannot be attached, so it is copied.
            GLint type = 0, name = 0;
            g.bindFramebuffer(glc::framebuffer, GLuint(expected));
            if (offscreenPassArmed && g.framebufferRenderbuffer && g.getAttachmentParameter) {
                g.getAttachmentParameter(glc::framebuffer, glc::depthAttachment, 0x8CD0 /*OBJECT_TYPE*/, &type);
                g.getAttachmentParameter(glc::framebuffer, glc::depthAttachment, 0x8CD1 /*OBJECT_NAME*/, &name);
            }
            const bool shared = glGetError() == GL_NO_ERROR && type == 0x8D41 /*RENDERBUFFER*/ && name > 0;
            g.bindFramebuffer(glc::framebuffer, r.silhouetteFbo);
            if (shared) g.framebufferRenderbuffer(glc::framebuffer, glc::depthAttachment, 0x8D41, GLuint(name));
            else g.framebufferTexture2D(glc::framebuffer, glc::depthAttachment, GL_TEXTURE_2D, r.silhouetteDepth, 0);
            if (g.checkFramebufferStatus(glc::framebuffer) != glc::framebufferComplete) {
                g.framebufferTexture2D(glc::framebuffer, glc::depthAttachment, GL_TEXTURE_2D, r.silhouetteDepth, 0);
                failed = true;
                __leave;
            }
            if (!shared) {
                g.bindFramebuffer(glc::framebuffer, GLuint(expected));
                g.bindTexture(GL_TEXTURE_2D, r.silhouetteDepth);
                g.copyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, viewport[2], viewport[3]);
            }
        } __finally {
            g.bindTexture(GL_TEXTURE_2D, GLuint(unitTexture)); // the game's unit cache holds this binding
        }
        if (failed) __leave;
        g.bindFramebuffer(glc::framebuffer, r.silhouetteFbo);
        glDisable(GL_SCISSOR_TEST);
        glColorMask(1, 1, 1, 1);
        glClearColor(1.f, 1.f, 1.f, 1.f);
        glClear(GL_COLOR_BUFFER_BIT);
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_GREATER);
        glDepthMask(0);
        glPolygonOffset(-1.f, -4.f); // held items: their own depth must pass LEQUAL despite rounding
        if (offscreenPassArmed) setViewport(g, 0, 0, viewport[2], viewport[3]); // sample 0's viewport
        // The draw budget: worn gear and world objects past the nearest replayBudget draws are left
        // out (the player's held items and the ghost's body always draw).
        static bool overBudget[silhouetteEntryLimit];
        {
            unsigned order[silhouetteEntryLimit], extra = 0;
            for (unsigned k = 0; k < s.count; ++k) {
                overBudget[k] = false;
                if (s.entries[k].slot >= int(s.heldCount)) order[extra++] = k;
            }
            if (extra > replayBudget) {
                std::partial_sort(order, order + replayBudget, order + extra, [&](unsigned a, unsigned b) {
                    return s.slotDistanceSq[s.entries[a].slot] < s.slotDistanceSq[s.entries[b].slot];
                });
                for (unsigned j = replayBudget; j < extra; ++j) overBudget[order[j]] = true;
                replayBudgetCut.fetch_add(extra - replayBudget, std::memory_order_relaxed);
            }
        }
        for (unsigned k = 0; k < s.count; ++k) {
            const SilhouetteEntry& e = s.entries[k];
            uintptr_t part = 0;
            bool body = false, held = false;
            if (!silhouettePart(s, e, part, body, held)) continue;
            if (overBudget[k]) held = false;
            if (!body && !held) continue;
            bind(e.material);
            const float material = held && heldBlendColor ? heldMaterialCodeFor(g, e.material) : 0.f;
            // The single path submits (uniforms) then draws. A multi-sample entry is replayed as its
            // first sample only, through the single-sample submit (0x200770, which the dispatcher
            // calls when there is one sample) at sample 0's viewport: one draw instead of one per sample.
            if (e.sampled) single(*(const uintptr_t*)gameAt(fp::sig::headwearWorldRenderer), e.node, e.part);
            else ((MaterialSubmit)(*(const uintptr_t*)(e.material + 0x80)))(e.node, e.part);
            // The bind set the blend state through its cache; the draw's own state goes back onto it.
            readBlend(blending, blend);
            glEnable(GL_BLEND);
            g.blendEquationSeparate(glc::funcAdd, glc::funcAdd);
            g.blendFuncSeparate(GL_ZERO, GL_ZERO, GL_ZERO, GL_ZERO);
            const auto draw = (PartDraw)(*(const uintptr_t*)(part + 0x90));
            const uintptr_t drawArgument = *(const uintptr_t*)(part + 0x98);
            if (body) { // R: where the body lies behind something
                glColorMask(1, 0, 0, 0);
                glDepthFunc(GL_GREATER);
                glDisable(GL_POLYGON_OFFSET_FILL);
                draw(drawArgument);
                drawn |= bodyMaskReady;
            }
            if (held && heldBlendColor) { // G: where the slot shows, as 1 + slot * 6 + material (cleared 1 times the constant)
                const float code = float(1 + e.slot * 6 + int(material * 10.f)) / 255.f;
                glColorMask(0, 1, 0, 0);
                glDepthFunc(GL_LEQUAL);
                glEnable(GL_POLYGON_OFFSET_FILL);
                heldBlendColor(code, code, code, code);
                g.blendFuncSeparate(GL_ZERO, 0x8001 /*CONSTANT_COLOR*/, GL_ZERO, 0x8001);
                draw(drawArgument);
                drawn |= heldMaskSlot << e.slot;
            }
            (blending ? glEnable : glDisable)(GL_BLEND);
            g.blendEquationSeparate(GLenum(blend[4]), GLenum(blend[5]));
            g.blendFuncSeparate(GLenum(blend[0]), GLenum(blend[1]), GLenum(blend[2]), GLenum(blend[3]));
        }
        if (glGetError() != GL_NO_ERROR) drawn = 0;
        // The poses the held items and body parts were drawn with, for their detail frames.
        for (unsigned k = 0; k < s.slotCount; ++k) {
            if (!(drawn & (heldMaskSlot << k))) continue;
            const uintptr_t node = s.heldNodes[k];
            const float* sampled = (const float*)(node + field::sceneNode::renderTransform);
            HeldPose& h = heldMaskPoses[k];
            for (int row = 0; row < 4; ++row)
                for (int axis = 0; axis < 3; ++axis) h.pose[row * 3 + axis] = sampled[row * 4 + axis];
            const uintptr_t sector = *(const uintptr_t*)(node + field::sceneNode::sector);
            if (readable(sector, field::sector::origin + 12))
                memcpy(h.sectorOrigin, (const void*)(sector + field::sector::origin), sizeof h.sectorOrigin);
            else
                memset(h.sectorOrigin, 0, sizeof h.sectorOrigin);
            h.apparel = k >= s.heldCount;
            h.object = h.apparel && s.slotObjects[k];
            // Light and condition change slowly: each node's are worked out at most every 500 ms.
            HeldNodeCache& cached = heldNodeCache(node);
            if (GetTickCount64() - cached.tick > 500) {
                heldLightFor(node, sector, h);
                heldConditionOf(h.apparel ? s.slotPieces[k] : node, h.apparel && !h.object, h.condition);
                if (h.object && h.condition[3] == 0.f) heldConditionOf(node, true, h.condition);
                memcpy(cached.light, h.light, sizeof h.light);
                memcpy(cached.lightColour, h.lightColour, sizeof h.lightColour);
                memcpy(cached.condition, h.condition, sizeof h.condition);
                cached.tick = GetTickCount64();
            } else {
                memcpy(h.light, cached.light, sizeof h.light);
                memcpy(h.lightColour, cached.lightColour, sizeof h.lightColour);
                memcpy(h.condition, cached.condition, sizeof h.condition);
            }
            if (h.apparel) memset(h.blood, 0, sizeof h.blood);
            else weaponBloodFor(node, h.blood);
        }
    } __finally {
        (offsetFill ? glEnable : glDisable)(GL_POLYGON_OFFSET_FILL);
        glPolygonOffset(offset[0], offset[1]);
        if (heldBlendColor) heldBlendColor(blendColor[0], blendColor[1], blendColor[2], blendColor[3]);
        g.bindFramebuffer(glc::framebuffer, GLuint(expected)); // the scene's own target (0, or the sampling target)
        setViewport(g, viewport[0], viewport[1], viewport[2], viewport[3]);
        (blending ? glEnable : glDisable)(GL_BLEND);
        g.blendEquationSeparate(GLenum(blend[4]), GLenum(blend[5]));
        g.blendFuncSeparate(GLenum(blend[0]), GLenum(blend[1]), GLenum(blend[2]), GLenum(blend[3]));
        glDepthFunc(GLenum(depthFunc));
        glDepthMask(depthMask);
        (depthTest ? glEnable : glDisable)(GL_DEPTH_TEST);
        (scissor ? glEnable : glDisable)(GL_SCISSOR_TEST);
        glColorMask(colorMask[0], colorMask[1], colorMask[2], colorMask[3]);
        glClearColor(clear[0], clear[1], clear[2], clear[3]);
        drainGl();
    }
    if (drawn) silhouetteReplays.fetch_add(1, std::memory_order_relaxed);
    return drawn;
}

static uint64_t drawSilhouetteMask(const SilhouetteScope& s) {
    if (!supersampleViewMatches() || (*(const int*)gameAt(fp::sig::sampleLast) != 0) != offscreenPassArmed) return 0;
    uint64_t drawn = 0;
    AcquireSRWLockExclusive(&cueLock);
    ++cueGlDepth;
    __try {
        if (!cueDeletesInFlight && !cueRetiring.load() && !cueBindingFailed.load() && !stopping)
            drawn = drawSilhouetteMaskOwned(s);
    } __finally {
        --cueGlDepth;
        ReleaseSRWLockExclusive(&cueLock);
    }
    return drawn;
}

static void silhouettePassBody(uintptr_t camera, uintptr_t caller) {
    if (offscreenPassArmed && !offscreenSceneFramebuffer && caller == gameAt(fp::sig::silhouettePassReturn)) {
        // From the game's own records, not GL queries: a query here, mid-frame, would make the
        // game's thread wait for the driver's (DAT_10049A4F0: the framebuffer it bound last,
        // the sampling target; DAT_10049ACD0: sample 0's viewport, x, y, width, height).
        __try {
            const GLint framebuffer = *(const GLint*)gameAt(fp::sig::boundFramebufferName);
            const int* area = (const int*)gameAt(fp::sig::sampleViewports);
            if (framebuffer > 0 && area[2] >= 32 && area[3] >= 32 && area[2] <= 32768 && area[3] <= 32768) {
                offscreenSceneFramebuffer = framebuffer;
                offscreenSceneSize[0] = area[2];
                offscreenSceneSize[1] = area[3];
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            offscreenSceneFramebuffer = 0;
        }
    }
    SilhouetteScope* const s = silhouetteScope;
    // With Supersample the world is drawn once per jittered sample into the default
    // framebuffer and the last sample's depth stays for the finish pass, so only the
    // last sample's camera pass draws the mask (index + 1 == count; 0 and 1 otherwise).
    if (s && s->armed && s->count && caller == gameAt(fp::sig::silhouettePassReturn)) {
        __try {
            // Offscreen sampling draws up to four samples a pass (index = first sample of the pass).
            const int index = *(const int*)gameAt(fp::sig::supersampleIndex);
            if (index + (offscreenPassArmed ? 4 : 1) < *(const int*)gameAt(fp::sig::supersampleCount)) __leave;
            silhouetteMaskReady = drawSilhouetteMask(*s);
            silhouetteMaskFromEye = viewEffectsEyeDraw;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            silhouetteMaskReady = 0;
            silhouetteFaults.fetch_add(1, std::memory_order_relaxed);
        }
    }
    originalSilhouettePass(camera);
}
static void silhouettePass(uintptr_t camera) {
    silhouettePassBody(camera, (uintptr_t)_ReturnAddress());
}

static bool worldInDefaultFramebuffer(uintptr_t view);
// The main view's world pass in third person with the ghost on (body: true), or
// in either view with the held-item detail on (body: false).
static bool silhouetteWanted(uintptr_t view, bool body = true) {
    const bool consumer = body ? viewMode.load() == 1 && playerGhost.load(std::memory_order_relaxed)
                               : viewMode.load() < 2 && (heldDetail.load(std::memory_order_relaxed) > 0.f ||
                                                         armourDetail.load(std::memory_order_relaxed) > 0.f ||
                                                         objectDetail.load(std::memory_order_relaxed) > 0.f);
    return silhouetteHooked && silhouetteSubmitHooked() && consumer && InterlockedCompareExchange(&active, 0, 0) &&
           InterlockedCompareExchange(&cameraReady, 0, 0) && !InterlockedCompareExchange(&stopping, 0, 0) &&
           !(viewEffectsUnavailable.load(std::memory_order_relaxed) & viewFaulted) &&
           supersampleRenderScope.owned && view == supersampleRenderScope.view && supersampleRenderScope.root &&
           (worldInDefaultFramebuffer(view) || offscreenPassArmed);
}
static bool playerCapsule(ViewRequest& request) {
    if (!TryAcquireSRWLockShared(&aimLock)) return false;
    const bool valid = aim.valid;
    float eye[3];
    memcpy(eye, aim.origin, sizeof eye);
    ReleaseSRWLockShared(&aimLock);
    if (!valid || !std::isfinite(eye[0] + eye[1] + eye[2])) return false;
    for (int i = 0; i < 3; ++i) request.ghostEye[i] = request.ghostFeet[i] = eye[i];
    request.ghostEye[1] -= playerTopBelowEyeCm;
    request.ghostFeet[1] -= playerBottomBelowEyeCm;
    return true;
}

// Hurt ramps from half health to a tenth. Winded starts 0.05 below the stamina
// ceiling (where the game switches to its low-stamina idle), is full 0.5 below
// it, and pulses with a breath every two seconds.
constexpr float hurtStart = 0.5f, hurtSpan = 0.4f, windedGap = 0.05f, windedSpan = 0.45f, breathPeriodMs = 2000.f;
static void readCondition(ViewRequest& request) {
    float health = 0.f, healthMax = 0.f, stamina = 0.f, ceiling = 0.f;
    __try {
        const uintptr_t root = *(const uintptr_t*)gameAt(fp::sig::playerRoot);
        if (!root) return;
        health = *(const float*)(root + field::character::health);
        healthMax = *(const float*)(root + field::character::healthMax);
        stamina = *(const float*)(root + field::character::stamina);
        ceiling = *(const float*)(root + field::character::staminaMax);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }
    if (std::isfinite(health) && std::isfinite(healthMax) && healthMax > 0.f) {
        const float t = std::clamp((hurtStart - health / healthMax) / hurtSpan, 0.f, 1.f);
        request.hurt = t * t * (3.f - 2.f * t);
    }
    if (std::isfinite(stamina) && std::isfinite(ceiling)) {
        const float shortfall = std::clamp((ceiling - windedGap - stamina) / windedSpan, 0.f, 1.f);
        const float phase = float(GetTickCount64() % ULONGLONG(breathPeriodMs)) / breathPeriodMs;
        const float breath = 0.5f + 0.5f * std::sin(6.2831853f * phase);
        request.winded = shortfall * (0.6f + 0.4f * breath);
    }
}

// Level detail's strength this frame: the row's value times a scale that falls from 1 to 0 as the
// frame period (eased, viewCpuFrameMs) grows from 18 to 25 ms, and climbs back slowly (2% a
// frame), so it is the first effect to give way when the game runs slow. And the people near the
// player (the world roster within 4 m, nearest six), whose bodies and faces it leaves alone.
static std::atomic<unsigned> levelPeopleSeen{0};
// The two strongest game lights near the player, read-only from the same kind-5 light list the held
// items use (collectHeldLights; rebuilt from the player's sector at most every half second), for
// level detail: strength = intensity x colour brightness x the light's own range falloff.
static void levelLightsFor(ViewRequest& request) {
    __try {
        const uintptr_t player = *(const uintptr_t*)gameAt(fp::sig::playerRoot);
        const uintptr_t mesh = player && readable(player, field::character::sceneNode + 8)
                                   ? *(const uintptr_t*)(player + field::character::sceneNode) : 0;
        if (!mesh || !readable(mesh, field::sceneNode::renderTransform + 64)) return;
        const uintptr_t sector = *(const uintptr_t*)(mesh + field::sceneNode::sector);
        const LightList& list = lightsOf(sector);
        float centre[3];
        if (!heldGlobalOrigin(mesh, centre)) return;
        float best[2] = {0.f, 0.f};
        for (unsigned k = 0; k < list.count; ++k) {
            const uintptr_t light = list.nodes[k];
            float origin[3];
            if (!heldLightNode(light) || !sectorOriginOf(light, origin)) continue;
            const float* at = (const float*)(light + field::sceneNode::transformOrigin);
            const float* c = (const float*)(light + lightColour);
            const float intensity = *(const float*)(light + lightIntensity), range = *(const float*)(light + lightRange);
            float d[3];
            for (int i = 0; i < 3; ++i) d[i] = at[i] + origin[i] - centre[i];
            const float distance = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
            const float luma = 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2];
            if (!std::isfinite(distance + intensity + range + luma) || intensity <= 0.f || luma <= 0.f) continue;
            const float reach = range > 1.f ? 1.f - distance / range : 1.f / (1.f + distance * distance / 250000.f);
            if (reach <= 0.f) continue;
            const float w = (std::min)(intensity, 8.f) * luma * reach * reach;
            const unsigned slot = w > best[0] ? 0 : w > best[1] ? 1 : 2;
            if (slot == 2) continue;
            if (slot == 0) { // the old strongest moves down
                memcpy(request.lamps[1], request.lamps[0], sizeof request.lamps[0]);
                memcpy(request.lampOrigin[1], request.lampOrigin[0], sizeof request.lampOrigin[0]);
                memcpy(request.lampColour[1], request.lampColour[0], sizeof request.lampColour[0]);
                best[1] = best[0];
            }
            best[slot] = w;
            for (int i = 0; i < 3; ++i) {
                request.lamps[slot][i] = at[i];
                request.lampOrigin[slot][i] = origin[i];
            }
            const float brightest = (std::max)(c[0], (std::max)(c[1], c[2]));
            for (int i = 0; i < 3; ++i) request.lampColour[slot][i] = brightest > 0.f ? c[i] / brightest : 1.f;
        }
        for (unsigned k = 0; k < 2; ++k) request.lamps[k][3] = (std::min)(best[k] * 2.f, 1.f);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

static void levelDetailFor(ViewRequest& request) {
    const float wanted = levelDetail.load(std::memory_order_relaxed);
    if (!(wanted > 0.f) || request.eye) return; // desktop view only (a headset's two eyes would double it)
    const float period = viewCpuFrameMs.load(std::memory_order_relaxed);
    const float target = period > 0.f ? 1.f - std::clamp((period - 18.f) / 7.f, 0.f, 1.f) : 1.f;
    float scale = levelDetailScale.load(std::memory_order_relaxed);
    scale = target < scale ? target : (std::min)(target, scale + 0.02f);
    levelDetailScale.store(scale, std::memory_order_relaxed);
    request.level = wanted * scale;
    if (!(request.level > 0.f)) return;
    __try {
        const uintptr_t player = *(const uintptr_t*)gameAt(fp::sig::playerRoot);
        const uintptr_t mesh = player && readable(player, field::character::sceneNode + 8)
                                   ? *(const uintptr_t*)(player + field::character::sceneNode) : 0;
        float centre[3];
        if (!mesh || !heldGlobalOrigin(mesh, centre)) return;
        auto note = [&](unsigned slot, uintptr_t node) {
            const float* sampled = (const float*)(node + field::sceneNode::renderTransform);
            for (int i = 0; i < 3; ++i) request.people[slot][i] = sampled[12 + i];
            if (!sectorOriginOf(node, request.peopleOrigin[slot])) memset(request.peopleOrigin[slot], 0, 12);
        };
        note(0, mesh);
        request.peopleCount = 1;
        const uintptr_t world = *(const uintptr_t*)gameAt(fp::sig::world);
        if (!readable(world, field::worldObject::characters + 8)) return;
        const uintptr_t roster = *(const uintptr_t*)(world + field::worldObject::characters);
        if (!readable(roster, field::list::items + 8)) return;
        const int characters = *(const int*)(roster + field::list::count);
        const uintptr_t entries = *(const uintptr_t*)(roster + field::list::items);
        if (characters <= 0 || characters > 4096 || !readable(entries, size_t(characters) * 8)) return;
        float distances[6]{};
        for (int i = 0; i < characters; ++i) {
            const uintptr_t character = *(const uintptr_t*)(entries + size_t(i) * 8);
            if (!character || character == player || !readable(character, field::character::sceneNode + 8)) continue;
            const uintptr_t node = *(const uintptr_t*)(character + field::character::sceneNode);
            float at[3];
            if (!node || !heldGlobalOrigin(node, at)) continue;
            const float d[3] = {at[0] - centre[0], at[1] - centre[1], at[2] - centre[2]};
            const float distanceSq = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
            if (!(distanceSq < 400.f * 400.f)) continue;
            unsigned slot = request.peopleCount;
            if (slot == 6) { // keep the nearest: replace the farthest if this one is nearer
                slot = 1;
                for (unsigned k = 2; k < 6; ++k) if (distances[k] > distances[slot]) slot = k;
                if (distances[slot] <= distanceSq) continue;
            } else {
                ++request.peopleCount;
            }
            note(slot, node);
            distances[slot] = distanceSq;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    levelPeopleSeen.store(request.peopleCount, std::memory_order_relaxed);
    levelLightsFor(request);
}

// The light-grid smoothing: a detour on the driver's glShaderSource (the entry the game loads by
// name with wglGetProcAddress). A source that declares "uniform sampler3D VoxelGI" and reads it with
// texture(VoxelGI, ...) gets a tricubic B-spline helper after the declaration and each such read
// redirected to it; every other source, and every source while the row is off, goes through as
// it came. Installed once, on the render thread, with a context current.
using ShaderSourceFn = void(APIENTRY*)(GLuint, GLsizei, const char* const*, const GLint*);
static ShaderSourceFn originalShaderSource = nullptr;
static std::atomic<bool> lightSmoothingHooked{false};
static std::atomic<unsigned> lightSmoothingRewrites{0}, lightSmoothingFaults{0};
// A B-spline (smooth, not interpolating) over the 4 x 4 x 4 cells around the point, from eight
// trilinear reads placed so the hardware weights do the per-axis blends.
static const char* const voxelSmoothSource = R"glsl(
vec4 voxelGISmooth(sampler3D volume, vec3 at) {
    vec3 size = vec3(textureSize(volume, 0));
    vec3 x = at * size - 0.5;
    vec3 i = floor(x), f = x - i;
    vec3 g = 1.0 - f;
    vec3 w0 = g * g * g / 6.0;
    vec3 w1 = (3.0 * f * f * f - 6.0 * f * f + 4.0) / 6.0;
    vec3 w2 = (-3.0 * f * f * f + 3.0 * f * f + 3.0 * f + 1.0) / 6.0;
    vec3 w3 = f * f * f / 6.0;
    vec3 s0 = w0 + w1, s1 = w2 + w3;
    vec3 p0 = (i - 0.5 + w1 / s0) / size, p1 = (i + 1.5 + w3 / s1) / size;
    vec4 a = mix(texture(volume, vec3(p1.x, p0.y, p0.z)), texture(volume, vec3(p0.x, p0.y, p0.z)), s0.x);
    vec4 b = mix(texture(volume, vec3(p1.x, p1.y, p0.z)), texture(volume, vec3(p0.x, p1.y, p0.z)), s0.x);
    vec4 c = mix(texture(volume, vec3(p1.x, p0.y, p1.z)), texture(volume, vec3(p0.x, p0.y, p1.z)), s0.x);
    vec4 d = mix(texture(volume, vec3(p1.x, p1.y, p1.z)), texture(volume, vec3(p0.x, p1.y, p1.z)), s0.x);
    return mix(mix(c, d, s1.y), mix(a, b, s1.y), s0.z);
}
)glsl";

using CreateShaderFn = GLuint(APIENTRY*)(GLenum);
using CompileShaderFn = void(APIENTRY*)(GLuint);
using GetShaderivFn = void(APIENTRY*)(GLuint, GLenum, GLint*);
using DeleteShaderFn = void(APIENTRY*)(GLuint);
static CreateShaderFn smoothCreateShader = nullptr;
static CompileShaderFn smoothCompileShader = nullptr;
static GetShaderivFn smoothGetShaderiv = nullptr;
static DeleteShaderFn smoothDeleteShader = nullptr;
static bool rewriteCompiles(GLuint shader, const char* text) {
    if (!smoothCreateShader || !smoothCompileShader || !smoothGetShaderiv || !smoothDeleteShader) return false;
    GLint type = 0, ok = 0;
    smoothGetShaderiv(shader, 0x8B4F /*SHADER_TYPE*/, &type);
    if (type != 0x8B30 && type != 0x8B31) return false;
    const GLuint scratch = smoothCreateShader(GLenum(type));
    if (!scratch) return false;
    originalShaderSource(scratch, 1, &text, nullptr);
    smoothCompileShader(scratch);
    smoothGetShaderiv(scratch, 0x8B81 /*COMPILE_STATUS*/, &ok);
    smoothDeleteShader(scratch);
    return ok != 0;
}

static void APIENTRY smoothShaderSource(GLuint shader, GLsizei count, const char* const* strings, const GLint* lengths) {
    if (lightSmoothing.load(std::memory_order_relaxed) && strings && count > 0 && count < 256) {
        try {
            std::string text;
            for (GLsizei k = 0; k < count; ++k) {
                if (!strings[k]) { text.clear(); break; }
                if (lengths && lengths[k] >= 0) text.append(strings[k], size_t(lengths[k]));
                else text.append(strings[k]);
            }
            const size_t declared = text.find("uniform sampler3D VoxelGI");
            const size_t end = declared == std::string::npos ? declared : text.find(';', declared);
            if (end != std::string::npos && text.find("texture(VoxelGI,") != std::string::npos &&
                text.find("voxelGISmooth") == std::string::npos) {
                for (size_t at = text.find("texture(VoxelGI,", end); at != std::string::npos;
                     at = text.find("texture(VoxelGI,", at)) {
                    text.replace(at, 7, "voxelGISmooth");
                    at += 13;
                }
                text.insert(end + 1, voxelSmoothSource);
                // Only a rewrite that compiles on this driver replaces the game's source: it is first
                // compiled in a scratch shader of the same type (textureSize needs GLSL 1.30 or later).
                const char* rewritten = text.c_str();
                if (rewriteCompiles(shader, rewritten)) {
                    originalShaderSource(shader, 1, &rewritten, nullptr);
                    lightSmoothingRewrites.fetch_add(1, std::memory_order_relaxed);
                    return;
                }
                lightSmoothingFaults.fetch_add(1, std::memory_order_relaxed);
            }
        } catch (...) {
            lightSmoothingFaults.fetch_add(1, std::memory_order_relaxed);
        }
    }
    originalShaderSource(shader, count, strings, lengths);
}

static void installLightSmoothing() {
    if (lightSmoothingHooked.load(std::memory_order_relaxed) || !wglGetCurrentContext()) return;
    lightSmoothingHooked.store(true, std::memory_order_relaxed); // one attempt, whatever happens
    smoothCreateShader = (CreateShaderFn)wglGetProcAddress("glCreateShader");
    smoothCompileShader = (CompileShaderFn)wglGetProcAddress("glCompileShader");
    smoothGetShaderiv = (GetShaderivFn)wglGetProcAddress("glGetShaderiv");
    smoothDeleteShader = (DeleteShaderFn)wglGetProcAddress("glDeleteShader");
    void* target = (void*)wglGetProcAddress("glShaderSource");
    if (!target || target == (void*)1 || target == (void*)2 || target == (void*)3 || target == (void*)-1) return;
    if (MH_CreateHook(target, (void*)smoothShaderSource, (void**)&originalShaderSource) != MH_OK) return;
    if (MH_EnableHook(target) != MH_OK) {
        MH_RemoveHook(target);
        originalShaderSource = nullptr;
    }
}

static LONG viewEffectsHandledFrame = -1; // swap serial whose desktop image the VR path already processed
// eye: a VR eye target; its passes skip depth of field (a fixed focus plane fights the player's own
// eyes) and the ghost (there is no third person in a headset).
// The window's depth buffer carries stencil (24+8); a copy into plain DEPTH_COMPONENT24 makes the driver
// convert every texel, while the same packed format copies straight. Asked once: the pixel format is fixed.
static GLint defaultDepthFormat() {
    static GLint format = 0;
    if (!format) {
        GLint stencil = 0;
        glGetIntegerv(0x0D57 /* GL_STENCIL_BITS */, &stencil);
        format = stencil > 0 ? GLint(0x88F0) /* DEPTH24_STENCIL8 */ : GLint(glc::depthComponent24);
    }
    return format;
}
static thread_local bool presentingOffscreen = false; // presentViewEffects: the image is in the view texture
static thread_local bool viewEffectsSupersampled = false; // presenting the supersampled pass's image
static void presentViewEffects(uintptr_t view, const ViewRequest* eye = nullptr) {
    installLightSmoothing();
    const int mode = viewMode.load();
    // A free cursor (inventory, a window, RMB) still shows the same world, so the effects stay.
    if (!cameraReady || !active || stopping || mode >= 2) {
        if (!eye) skipViewEffects(mode >= 2 ? "the view mode draws no effects" : "the camera is not ready");
        return;
    }
    // The Settings window stands over the scene and the game paused: its frames skip the effects.
    if (!eye) {
        const uintptr_t options = *reinterpret_cast<const uintptr_t*>(gameAt(fp::sig::guiOptionsForm));
        if (options && readable(options + field::form::visible, 1) && *reinterpret_cast<const unsigned char*>(options + field::form::visible)) {
            skipViewEffects("the Settings window is open");
            return;
        }
    }
    if (!eye && viewEffectsHandledFrame == InterlockedCompareExchange(&swapCalls, 0, 0)) return; // the VR path drew this desktop image
    ViewRequest request = eye ? *eye : ViewRequest{};
    request.ao = ambientOcclusion.load(std::memory_order_relaxed);
    request.dof = mode == 0 && !eye ? depthOfField.load(std::memory_order_relaxed) : 0;
    request.ssr = !eye ? screenSpaceReflections.load(std::memory_order_relaxed) : 0;
    // Supersample already averages jittered samples across every edge.
    const bool supersampled = viewEffectsSupersampled || presentingOffscreen;
    request.smooth = supersampled ? 0.f : edgeSmoothing.load(std::memory_order_relaxed);
    // ...which also softens texture detail; the same switch then sharpens it back a little.
    request.sharpen = supersampled ? edgeSmoothing.load(std::memory_order_relaxed) : 0.f;
    // A worn helmet: a piece hung on the head, or head pieces of the body skin seen in the last half second.
    const bool helmetWorn = headWearNodes.load() > 0 ||
                            GetTickCount64() - headwearSeen.load(std::memory_order_relaxed) < 500;
    request.visor = mode == 0 && helmetWorn ? helmetVisor.load(std::memory_order_relaxed) : 0.f;
    request.visorHidden = helmetMode.load() == fp::HelmetMode::hide;
    request.ghostStrength = playerGhost.load(std::memory_order_relaxed);
    request.ghost = !eye && mode == 1 && request.ghostStrength > 0.f && playerCapsule(request);
    uint64_t mask = silhouetteMaskReady;
    bool maskEye = silhouetteMaskFromEye; // whose view the mask was drawn in
    silhouetteMaskReady = 0;
    silhouetteMaskFromEye = false;
    // A frame whose world pass drew no mask keeps the last one (still in the mask texture, with its
    // poses) for up to two frames, so held items and gear do not blink back to their stock look.
    {
        static thread_local uint64_t lastMask = 0;
        static thread_local bool lastMaskEye = false;
        static thread_local unsigned missed = 0;
        if (mask) {
            lastMask = mask;
            lastMaskEye = maskEye;
            missed = 0;
        } else if (lastMask && missed < 2 && (eye != nullptr) == lastMaskEye) {
            mask = lastMask;
            maskEye = lastMaskEye;
            ++missed;
            heldMaskReused.fetch_add(1, std::memory_order_relaxed);
        } else {
            lastMask = 0;
        }
    }
    request.silhouette = request.ghost && !maskEye && (mask & bodyMaskReady);
    // Held-item and armour detail: the slots the replay drew this frame, in this view's own mask (the desktop's,
    // first or third person, or a VR eye's, drawn by that eye's replay).
    request.heldSlots = (eye != nullptr) == maskEye ? (mask >> 1) & ((uint64_t(1) << heldSlotLimit) - 1) : 0;
    if (request.heldSlots) {
        request.detail = heldDetail.load(std::memory_order_relaxed);
        request.armour = armourDetail.load(std::memory_order_relaxed);
        request.objects = objectDetail.load(std::memory_order_relaxed);
        memcpy(request.held, heldMaskPoses, sizeof request.held);
    }
    levelDetailFor(request);
    // Wounds: desktop and VR eyes alike, in the space of the matrices this view was drawn with.
    if (woundMarks.load(std::memory_order_relaxed) <= 0.f && heldDetail.load(std::memory_order_relaxed) > 0.f &&
        TryAcquireSRWLockExclusive(&woundLock)) {
        __try { takeHits(); } __finally { ReleaseSRWLockExclusive(&woundLock); }
    }
    if (woundMarks.load(std::memory_order_relaxed) > 0.f) {
        ViewEffectProjectionSnapshot matrices;
        if (readViewMatrices(view, matrices)) woundViewUniforms(matrices.view, request.wounds);
    }
    // Fire and smoke: desktop and VR eyes alike, like the wounds.
    if (fireLook.load(std::memory_order_relaxed) > 0.f) {
        ViewEffectProjectionSnapshot matrices;
        if (readViewMatrices(view, matrices) && matrices.worldUpValid)
            fireViewUniforms(matrices.view, viewCpuFrameMs.load(std::memory_order_relaxed), request.fires);
    }
    request.conditionStrength = conditionEffects.load(std::memory_order_relaxed);
    if (request.conditionStrength > 0.f) {
        readCondition(request);
        // The old on is 0.5; the full scale doubles it.
        request.hurt = (std::min)(request.hurt * request.conditionStrength * 2.f, 1.f);
        request.winded = (std::min)(request.winded * request.conditionStrength * 2.f, 1.f);
        request.adapt = !eye;
    }
    if (!request.any()) {
        if (!eye) skipViewEffects("every effect is off");
        return;
    }
    if (!eye && presentingOffscreen) {
        // The view texture the resolve wrote and its size, read from the view.
        __try {
            const uintptr_t texture = *(const uintptr_t*)(view + field::view::viewTexture);
            request.viewTexture = texture ? *(const GLuint*)(texture + field::texture::glName) : 0;
            request.outputWidth = *(const int*)(view + field::view::outputWidth);
            request.outputHeight = *(const int*)(view + field::view::outputHeight);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            request.viewTexture = 0;
        }
        request.sceneFramebuffer = GLuint(offscreenSceneFramebuffer);
        request.sceneWidth = offscreenSceneSize[0];
        request.sceneHeight = offscreenSceneSize[1];
        if (!request.viewTexture || request.outputWidth < 32 || request.outputHeight < 32 ||
            request.outputWidth > 16384 || request.outputHeight > 16384)
            return skipViewEffects("the offscreen view texture could not be read");
        request.offscreen = true;
    }
    AcquireSRWLockExclusive(&cueLock);
    ++cueGlDepth;
    __try {
        if (!cueDeletesInFlight && !cueRetiring.load() && !cueBindingFailed.load() && !stopping)
            drawViewEffectsOwned(view, request);
    } __finally {
        --cueGlDepth;
        ReleaseSRWLockExclusive(&cueLock);
    }
}

// The world passes of the viewport presentation (0x4BAB0) return before the
// native bloom/composite and the UI, which is where the effects belong:
// EXP_render_sampled_views (RVA 0xA0CE0) for its ordinary single-sample call and,
// with offscreen sampling, for Supersample drawn into the game's sampling target and
// resolved into the view texture (0x4BF09); FUN_10004B6D0 for Supersample averaged
// into the default framebuffer.
using RenderSampledViews = void (*)(uintptr_t, unsigned);
static RenderSampledViews originalSampledViews, originalSuperSampledViews;
static bool viewEffectsHooked = false;

// With the view's offscreen-sampling flag set the world goes to the shared
// sampling target and the default framebuffer keeps a stale image.
static bool worldInDefaultFramebuffer(uintptr_t view) {
    __try {
        return *(const char*)(view + field::view::offscreenSampling) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static void presentAfterWorldPass(uintptr_t view, bool offscreen = false) {
    if (!supersampleRenderScope.owned)
        return skipViewEffects("the mod's camera is not active (a menu or a cutscene)");
    if (view != supersampleRenderScope.view) return; // another view (a map, a portrait): never ours
    if (viewEffectsUnavailable.load(std::memory_order_relaxed) & viewFaulted)
        return skipViewEffects("a fault turned the effects off until restart");
    if (!offscreen && !worldInDefaultFramebuffer(view))
        return skipViewEffects("the world went to an offscreen target this hook does not resolve");
    if (offscreen && !offscreenSceneFramebuffer)
        return skipViewEffects(silhouetteHooked ? "the offscreen sampling target was not seen this frame"
                                                : "offscreen sampling needs the silhouette hook (game code differs)");
    presentingOffscreen = offscreen;
    __try {
        fp::profile::invoke<fp::profile::Hook::viewEffects>(presentViewEffects, view, nullptr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        noteViewEffectFailure(viewFaulted, "fault while drawing; effects off until restart");
    }
    presentingOffscreen = false;
}

// The main view's world pass, single or supersampled, notes the player's parts for
// the silhouette (see silhouettePass).
static void armSilhouette(uintptr_t view, SilhouetteScope& scope) {
    scope.armed = scope.body = scope.apparel = scope.objects = scope.centreValid = false;
    scope.count = scope.heldCount = scope.slotCount = 0;
    scope.root = 0;
    silhouetteMaskReady = 0;
    __try {
        scope.body = silhouetteWanted(view, true);
        const bool held = silhouetteWanted(view, false);
        if (scope.body || held) scope.root = supersampleRenderScope.root;
        scope.apparel = held && armourDetail.load(std::memory_order_relaxed) > 0.f;
        scope.objects = held && objectDetail.load(std::memory_order_relaxed) > 0.f;
        if (scope.root) {
            const uintptr_t mesh = readable(scope.root, field::character::sceneNode + 8)
                                       ? *(const uintptr_t*)(scope.root + field::character::sceneNode) : 0;
            scope.centreValid = mesh && heldGlobalOrigin(mesh, scope.centre);
        }
        // The ghost needs the held items too: a shield held out in front of the body
        // must not show the body through it.
        if (held || scope.body) collectHeldNodes(scope);
        if (scope.body || scope.heldCount || scope.apparel || scope.objects) {
            scope.armed = true;
            silhouetteScope = &scope;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        silhouetteFaults.fetch_add(1, std::memory_order_relaxed);
    }
}
static void sampledViewsNativeBody(uintptr_t view, unsigned slot, bool world) {
    SilhouetteScope scope;
    SilhouetteScope* const previous = silhouetteScope;
    if (world) armSilhouette(view, scope);
    __try {
        fp::profile::invoke<fp::profile::Hook::sampledViewsNative>(originalSampledViews, view, slot);
    } __finally {
        silhouetteScope = previous;
    }
}
static void superSampledViewsNativeBody(uintptr_t view, unsigned sampleSetting) {
    SilhouetteScope scope;
    SilhouetteScope* const previous = silhouetteScope;
    armSilhouette(view, scope);
    __try {
        fp::profile::invoke<fp::profile::Hook::superSampledViewsNative>(originalSuperSampledViews, view, sampleSetting);
    } __finally {
        silhouetteScope = previous;
    }
}
static void sampledViewsProfileBody(uintptr_t view, unsigned slot, uintptr_t caller) {
    const bool world = slot == 0 && caller == gameAt(fp::sig::sampledViewsWorldReturn);
    // Supersample with offscreen sampling: one call draws every sample into the game's
    // sampling target and resolves them into the view texture (0x4BF09).
    const bool offscreen = slot != 0 && caller == gameAt(fp::sig::sampledViewsOffscreenReturn) &&
                           !worldInDefaultFramebuffer(view);
    const bool previousArmed = offscreenPassArmed;
    offscreenPassArmed = offscreen;
    offscreenSceneFramebuffer = 0;
    __try {
        sampledViewsNativeBody(view, slot, world || offscreen);
    } __finally {
        offscreenPassArmed = previousArmed;
    }
    if (world || offscreen) presentAfterWorldPass(view, offscreen);
}
static void renderSampledViews(uintptr_t view, unsigned slot) {
    const uintptr_t caller = (uintptr_t)_ReturnAddress();
    fp::profile::invoke<fp::profile::Hook::sampledViews>(sampledViewsProfileBody, view, slot, caller);
}

static void superSampledViewsProfileBody(uintptr_t view, unsigned sampleSetting) {
    superSampledViewsNativeBody(view, sampleSetting);
    viewEffectsSupersampled = true;
    __try {
        presentAfterWorldPass(view);
    } __finally {
        viewEffectsSupersampled = false;
    }
}
static void renderSuperSampledViews(uintptr_t view, unsigned sampleSetting) {
    fp::profile::invoke<fp::profile::Hook::superSampledViews>(superSampledViewsProfileBody, view, sampleSetting);
}

// Before any hook exists (and again on F6 re-arm): the whole hooked function
// and the whole viewport presentation that calls it must equal the reviewed
// bytes in the file (startup contract check) and in the loaded image (here),
// or the group's addresses are withheld and no hook is made. The proof is the
// bytes, not the file hash, so byte-identical patched builds (Hellmode) qualify.
static bool admitViewEffectsEntries(const fp::ImageView& file) {
    bool admitted = false;
    __try {
        admitted = layout.optionalGroupAvailable("viewEffects") && loadedOptionalContractsImpl(file, "viewEffects");
    } __except (EXCEPTION_EXECUTE_HANDLER) { admitted = false; }
    if (admitted) return true;
    if (viewEffectsHooked) {
        snprintf(faultDetail, sizeof faultDetail, "view effect contracts changed after hook creation; restart required");
        return false;
    }
    for (unsigned t = 0; t < fp::sig::targetCount; ++t) {
        const char* group = fp::sig::targetOptionalGroups[t];
        if (group && !strcmp(group, "viewEffects")) layout.rva[t] = 0;
    }
    return true;
}

// Optional: an unresolved or unadmitted renderer leaves the game and the rest of
// the mod as they are, and the status file says why the effects are unavailable.
static void createViewEffectsHook() {
    const bool available = layout.optionalGroupAvailable("viewEffects");
    const bool single = available && MH_CreateHook((void*)gameAt(fp::sig::sampledViews), (void*)renderSampledViews,
                                                   (void**)&originalSampledViews) == MH_OK;
    const bool sampled = single && MH_CreateHook((void*)gameAt(fp::sig::superSampledViews),
                                                 (void*)renderSuperSampledViews,
                                                 (void**)&originalSuperSampledViews) == MH_OK;
    if (single && !sampled) MH_RemoveHook((void*)gameAt(fp::sig::sampledViews));
    viewEffectsHooked = sampled;
    if (!viewEffectsHooked) noteViewEffectFailure(viewHookMissing, "renderer code differs from the reviewed game build");
    // The silhouette needs the world-pass hook above; without it the capsule ghost stays.
    silhouetteHooked = viewEffectsHooked && silhouetteAdmitted && layout.optionalGroupAvailable("silhouette") &&
                       MH_CreateHook((void*)gameAt(fp::sig::silhouettePass), (void*)silhouettePass,
                                     (void**)&originalSilhouettePass) == MH_OK;
}

// The silhouette group: the highlight pass it detours, the camera pass that calls
// it once after the opaque lists, and the material bind it calls, byte-exact in the
// file and the loaded image, like the view-effect group above.
static bool admitSilhouetteEntries(const fp::ImageView& file) {
    bool admitted = false;
    __try {
        admitted = layout.optionalGroupAvailable("silhouette") && loadedOptionalContractsImpl(file, "silhouette");
    } __except (EXCEPTION_EXECUTE_HANDLER) { admitted = false; }
    if (admitted) {
        silhouetteAdmitted = true;
        return true;
    }
    if (silhouetteHooked) {
        snprintf(faultDetail, sizeof faultDetail, "silhouette contracts changed after hook creation; restart required");
        return false;
    }
    silhouetteAdmitted = false;
    for (unsigned t = 0; t < fp::sig::targetCount; ++t) {
        const char* group = fp::sig::targetOptionalGroups[t];
        if (group && !strcmp(group, "silhouette")) layout.rva[t] = 0;
    }
    return true;
}
