// Per-texel material ids for the item textures the game composes, from its own data (docs/render-assumptions.md,
// "Step 1 findings"). Part of first_person.cpp (one translation unit): included there, in order.
//
// The game builds an item's colour texture in its composer (0x1EBC60, queued by the factory 0x20FB80 in job+0x24):
// per entry a base quad, then each layer alpha-blended over it (SRC_ALPHA, ONE_MINUS_SRC_ALPHA), the alpha being
// what the layer's own material shader computes from its mask (textures.flb: alpha = Tex1.r, times a pattern in
// the {?Mat.TexOpc?} variants, the blade's v scaled by UVScale). Each layer holds the procedural material of its
// id (layer+0x50 = 0x1EAEE0(id), 0x2103F0), so a texel's material is the layer with the largest share of its
// colour: a_l * prod(1 - a_k, k > l), the base keeping prod(1 - a_k).
//
// After the composer returns, the hook replays each layer through the game's own colour draws (so every variant
// computes its own alpha, nothing restated here) into a target of ours, and keeps per texel the largest share and
// its layer's id (best' = max(best * (1 - a), a)). The result is an R8 texture beside the game's colour texture
// (job+0xC = set+0x10, the texture the item's submit binds on unit 0): 255 outside every entry, 0 the base (no
// procedural material), else the id 1-25. The held-item replay looks it up by that texture object and draws the
// id at the mesh's own texcoords (view_effects.inl).

namespace matid {
// The composer's job (0x20FB80 fills it): +0/+4 size, +0xC colour texture object, +0x24 the composer,
// +0x2C entry count, +0x30 entries of 0x1F0 bytes.
namespace job {
inline constexpr size_t width = 0x0, height = 0x4, colour = 0xC, composer = 0x24, entryCount = 0x2C, entries = 0x30;
}
// An entry: +0 origin, +8 size, +0x10 uv scale (floats, 0..1 of the target), +0x18 base texture, +0x28 pattern,
// +0x40 layer count, layers at +0x48 + 0x30 * i (+0 mask texture, +8 material, +0x28 overlay).
namespace entry {
inline constexpr size_t stride = 0x1F0, origin = 0x0, size = 0x8, scale = 0x10, base = 0x18, pattern = 0x28,
                        layerCount = 0x40, layers = 0x48, layerStride = 0x30;
inline constexpr unsigned layerLimit = 8; // 0x48 + 8 * 0x30 = 0x1C8 < 0x1CC, the entry's next field
}
namespace layer {
inline constexpr size_t mask = 0x0, material = 0x8, overlay = 0x28; // from the entry's +0x48 + 0x30 * i
}
inline constexpr unsigned idNone = 255, idCount = 26; // ids 1-25; DAT_10056B030[id]
inline constexpr unsigned overlayUnit = 8;            // the composer binds a layer's overlay there (0x1EC1FC)
inline constexpr unsigned ownUnitA = 14, ownUnitB = 15; // ours, restored with the unit cache afterwards
inline constexpr size_t byteBudget = size_t(96) << 20; // id textures kept, oldest dropped first

struct Texture {
    HGLRC context;
    uintptr_t object; // the game's colour texture object (job+0xC)
    GLuint gameName;  // its GL name when composed (+0x58); a reused object with another name is stale
    GLint width, height;
    GLuint ids;
    ULONGLONG used;
};
static Texture textures[512];
static size_t bytesKept = 0;

struct Resources {
    HGLRC context = nullptr;
    bool attempted = false, failed = false;
    GLuint fillProgram = 0, combineProgram = 0, vao = 0, vbo = 0;
    GLint fillRect = -1, combineRect = -1, combineLayer = -1, combineState = -1, combineId = -1, combineSize = -1;
    GLuint fbo[3]{}, tex[3]{}; // 0 state, 1 next state, 2 one layer's colour and alpha
    GLint width = 0, height = 0;
};
static Resources resources[4];

static std::atomic<unsigned> builds{0}, layersReplayed{0}, skipped{0}, faults{0}, otherContext{0}, dropped{0};
static std::atomic<const char*> lastSkip{""};

// Entry points the shared API table does not carry.
using GetAttachedShaders = void(APIENTRY*)(GLuint, GLsizei, GLsizei*, GLuint*);
using GetActiveUniform = void(APIENTRY*)(GLuint, GLuint, GLsizei, GLsizei*, GLint*, GLenum*, char*);
using GetUniformfv = void(APIENTRY*)(GLuint, GLint, GLfloat*);
using GetUniformiv = void(APIENTRY*)(GLuint, GLint, GLint*);
using UniformFv = void(APIENTRY*)(GLint, GLsizei, const GLfloat*);
using UniformIv = void(APIENTRY*)(GLint, GLsizei, const GLint*);
using UniformMatrixFv = void(APIENTRY*)(GLint, GLsizei, GLboolean, const GLfloat*);
using GetUniformBlockIndex = GLuint(APIENTRY*)(GLuint, const char*);
using GetActiveUniformBlockiv = void(APIENTRY*)(GLuint, GLuint, GLenum, GLint*);
using GetActiveUniformBlockName = void(APIENTRY*)(GLuint, GLuint, GLsizei, GLsizei*, char*);
using UniformBlockBinding = void(APIENTRY*)(GLuint, GLuint, GLuint);
struct ExtraApi {
    bool loaded = false;
    GetAttachedShaders getAttachedShaders = nullptr;
    GetActiveUniform getActiveUniform = nullptr;
    GetUniformfv getUniformfv = nullptr;
    GetUniformiv getUniformiv = nullptr;
    UniformFv uniform1fv = nullptr, uniform2fv = nullptr, uniform3fv = nullptr, uniform4fv = nullptr;
    UniformIv uniform1iv = nullptr;
    UniformMatrixFv uniformMatrix3fv = nullptr, uniformMatrix4fv = nullptr;
    GetUniformBlockIndex getUniformBlockIndex = nullptr;
    GetActiveUniformBlockiv getActiveUniformBlockiv = nullptr;
    GetActiveUniformBlockName getActiveUniformBlockName = nullptr;
    UniformBlockBinding uniformBlockBinding = nullptr;
    void(APIENTRY* getShaderiv)(GLuint, GLenum, GLint*) = nullptr;
    void(APIENTRY* getShaderSource)(GLuint, GLsizei, GLsizei*, char*) = nullptr;
    void(APIENTRY* getShaderInfoLog)(GLuint, GLsizei, GLsizei*, char*) = nullptr;
    void(APIENTRY* getProgramInfoLog)(GLuint, GLsizei, GLsizei*, char*) = nullptr;
};
static ExtraApi extra;
static bool loadExtra() {
    if (extra.loaded) return true;
    extra.loaded = loadGl(extra.getAttachedShaders, "glGetAttachedShaders") &&
                   loadGl(extra.getActiveUniform, "glGetActiveUniform") && loadGl(extra.getUniformfv, "glGetUniformfv") &&
                   loadGl(extra.getUniformiv, "glGetUniformiv") && loadGl(extra.uniform1fv, "glUniform1fv") &&
                   loadGl(extra.uniform2fv, "glUniform2fv") && loadGl(extra.uniform3fv, "glUniform3fv") &&
                   loadGl(extra.uniform4fv, "glUniform4fv") && loadGl(extra.uniform1iv, "glUniform1iv") &&
                   loadGl(extra.uniformMatrix3fv, "glUniformMatrix3fv") &&
                   loadGl(extra.uniformMatrix4fv, "glUniformMatrix4fv") &&
                   loadGl(extra.getUniformBlockIndex, "glGetUniformBlockIndex") &&
                   loadGl(extra.getActiveUniformBlockiv, "glGetActiveUniformBlockiv") &&
                   loadGl(extra.getActiveUniformBlockName, "glGetActiveUniformBlockName") &&
                   loadGl(extra.uniformBlockBinding, "glUniformBlockBinding") &&
                   loadGl(extra.getShaderiv, "glGetShaderiv") && loadGl(extra.getShaderSource, "glGetShaderSource") &&
                   loadGl(extra.getShaderInfoLog, "glGetShaderInfoLog") &&
                   loadGl(extra.getProgramInfoLog, "glGetProgramInfoLog");
    return extra.loaded;
}

// Our two passes over one entry's rectangle, placed as the composer's quad is (0x1EB910: origin .. origin +
// size * scale, in 0..1 of the target; its MPrj, the matrix at 0x2ABC90, maps that onto the target).
static const char* const rectVertex = R"glsl(#version 130
in vec2 corner;
uniform vec4 rect; // origin, extent (size * scale), 0..1 of the target
void main() {
    gl_Position = vec4((rect.xy + corner * rect.zw) * 2.0 - 1.0, 0.0, 1.0);
}
)glsl";
static const char* const fillFragment = R"glsl(#version 130
out vec4 state;
void main() { state = vec4(0.0, 1.0, 0.0, 1.0); } // the base: id 0, the whole share
)glsl";
static const char* const combineFragment = R"glsl(#version 130
uniform sampler2D layerTex; // this layer as the game's own draw wrote it: its alpha is the layer's share now
uniform sampler2D stateTex; // r: id of 255, g: that id's share of the colour
uniform float layerId;
uniform vec2 size;
out vec4 state;
void main() {
    vec2 uv = gl_FragCoord.xy / size;
    float a = texture(layerTex, uv).a;
    vec4 s = texture(stateTex, uv);
    float kept = s.g * (1.0 - a);
    state = a > kept ? vec4(layerId / 255.0, a, 0.0, 1.0) : vec4(s.r, kept, 0.0, 1.0);
}
)glsl";

static GLuint link(gl::Api& g, const char* vertex, const char* fragment) {
    auto compile = [&](GLenum type, const char* text) {
        const GLuint shader = g.createShader(type);
        if (!shader) return GLuint(0);
        g.shaderSource(shader, 1, &text, nullptr);
        g.compileShader(shader);
        GLint ok = 0;
        g.getShaderiv(shader, 0x8B81 /*COMPILE_STATUS*/, &ok);
        if (!ok) {
            g.deleteShader(shader);
            return GLuint(0);
        }
        return shader;
    };
    const GLuint vs = compile(0x8B31 /*VERTEX_SHADER*/, vertex), fs = compile(0x8B30 /*FRAGMENT_SHADER*/, fragment);
    GLuint program = vs && fs ? g.createProgram() : 0;
    if (program) {
        g.attachShader(program, vs);
        g.attachShader(program, fs);
        g.bindAttribLocation(program, 0, "corner");
        g.linkProgram(program);
        GLint ok = 0;
        g.getProgramiv(program, 0x8B82 /*LINK_STATUS*/, &ok);
        if (!ok) {
            g.deleteProgram(program);
            program = 0;
        }
    }
    if (vs) g.deleteShader(vs);
    if (fs) g.deleteShader(fs);
    return program;
}

static Resources* resourcesFor(gl::Api& g, HGLRC context) {
    Resources* r = nullptr;
    for (auto& slot : resources)
        if (slot.context == context) r = &slot;
    if (!r)
        for (auto& slot : resources)
            if (!slot.context) {
                slot.context = context;
                r = &slot;
                break;
            }
    if (!r) return nullptr;
    if (!r->attempted) {
        r->attempted = true;
        r->fillProgram = link(g, rectVertex, fillFragment);
        r->combineProgram = link(g, rectVertex, combineFragment);
        r->failed = !r->fillProgram || !r->combineProgram || !g.genVertexArrays;
        if (!r->failed) {
            r->fillRect = g.getUniformLocation(r->fillProgram, "rect");
            r->combineRect = g.getUniformLocation(r->combineProgram, "rect");
            r->combineLayer = g.getUniformLocation(r->combineProgram, "layerTex");
            r->combineState = g.getUniformLocation(r->combineProgram, "stateTex");
            r->combineId = g.getUniformLocation(r->combineProgram, "layerId");
            r->combineSize = g.getUniformLocation(r->combineProgram, "size");
            static const float corners[8] = {0.f, 0.f, 1.f, 0.f, 0.f, 1.f, 1.f, 1.f};
            // The game keeps its own vertex-array cache (0x6ABF0, DAT_10049B540): both bindings go back.
            GLint vao = 0, buffer = 0;
            glGetIntegerv(glc::vertexArrayBinding, &vao);
            glGetIntegerv(glc::arrayBufferBinding, &buffer);
            g.genVertexArrays(1, &r->vao);
            g.genBuffers(1, &r->vbo);
            g.bindVertexArray(r->vao);
            g.bindBuffer(glc::arrayBuffer, r->vbo);
            g.bufferData(glc::arrayBuffer, sizeof corners, corners, 0x88E4 /*STATIC_DRAW*/);
            g.enableVertexAttribArray(0);
            g.vertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
            g.bindVertexArray(GLuint(vao));
            g.bindBuffer(glc::arrayBuffer, GLuint(buffer));
            g.genFramebuffers(3, r->fbo);
            g.genTextures(3, r->tex);
            r->failed = !r->vao || !r->vbo || !r->fbo[0] || !r->tex[0] || glGetError() != GL_NO_ERROR;
        }
    }
    return r->failed ? nullptr : r;
}

// The three working targets at least this size (RGBA8: the combine reads alpha and two channels).
static bool sizeTargets(gl::Api& g, Resources& r, GLint width, GLint height) {
    if (r.width >= width && r.height >= height) return true;
    const GLint w = (std::max)(width, r.width), h = (std::max)(height, r.height);
    for (unsigned i = 0; i < 3; ++i) {
        g.bindTexture(GL_TEXTURE_2D, r.tex[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, GLint(glc::rgba8), w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        g.texParameteri(GL_TEXTURE_2D, glc::minFilter, glc::nearest);
        g.texParameteri(GL_TEXTURE_2D, glc::magFilter, glc::nearest);
        g.bindFramebuffer(glc::framebuffer, r.fbo[i]);
        g.framebufferTexture2D(glc::framebuffer, glc::colorAttachment0, GL_TEXTURE_2D, r.tex[i], 0);
        if (g.checkFramebufferStatus(glc::framebuffer) != glc::framebufferComplete) return false;
    }
    r.width = w;
    r.height = h;
    return glGetError() == GL_NO_ERROR;
}

// Our rectangle, with the game's vertex-array binding (its cache, 0x6ABF0) put back after.
static void drawRect(gl::Api& g, const Resources& r) {
    GLint vao = 0;
    glGetIntegerv(glc::vertexArrayBinding, &vao);
    g.bindVertexArray(r.vao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    g.bindVertexArray(GLuint(vao));
}

static Texture* findTexture(HGLRC context, uintptr_t object) {
    for (auto& t : textures)
        if (t.ids && t.context == context && t.object == object) return &t;
    return nullptr;
}
static void dropTexture(gl::Api& g, Texture& t) {
    if (t.ids) {
        g.deleteTextures(1, &t.ids);
        bytesKept -= size_t(t.width) * size_t(t.height);
        dropped.fetch_add(1, std::memory_order_relaxed);
    }
    t = Texture{};
}
// A slot for a new id texture of this many bytes: a free one, else the oldest, dropping the oldest until the
// budget has room.
static Texture* slotFor(gl::Api& g, HGLRC context, size_t bytes) {
    for (;;) {
        Texture *oldest = nullptr, *free = nullptr;
        for (auto& t : textures) {
            if (!t.ids) {
                if (!free) free = &t;
            } else if (t.context == context && (!oldest || t.used < oldest->used)) {
                oldest = &t;
            }
        }
        if (free && bytesKept + bytes <= byteBudget) return free;
        if (!oldest) return free && bytes <= byteBudget ? free : nullptr;
        dropTexture(g, *oldest);
    }
}

template <class T> static T at(uintptr_t address) {
    T value;
    memcpy(&value, (const void*)address, sizeof value);
    return value;
}

using Composer = void(__fastcall*)(uintptr_t);
using QuadSet = void(__fastcall*)(uintptr_t, uintptr_t, uintptr_t);
using QuadDraw = void(__fastcall*)();
using LayerDraw = void(__fastcall*)(uintptr_t, uintptr_t, uintptr_t, uintptr_t);
using UnitBind = void(__fastcall*)(int, uintptr_t, int);
using ModeSet = void(__fastcall*)(unsigned);
using Streams = void(__fastcall*)(unsigned);
static Composer originalComposer = nullptr;
static std::atomic<bool> hooked{false};
static bool admitted = false;

// The id of a layer's material: its index in the game's material cache (DAT_10056B030, filled by 0x1EAEE0),
// 0 when it is not there.
static unsigned idOf(uintptr_t material) {
    const auto* cache = (const uintptr_t*)gameAt(fp::sig::proceduralMaterials);
    for (unsigned id = 1; id < idCount; ++id)
        if (material && cache[id] == material) return id;
    return 0;
}

// cueLock held, on the composer's thread, its context current, right after it returned. Every GL change goes
// through the game's own caches (units 0-8, blend, program, vertex streams) or is put back raw from a read
// taken first; our two units are restored with their cache entries.
static bool build(uintptr_t jobAt) {
    const GLint width = at<GLint>(jobAt + job::width), height = at<GLint>(jobAt + job::height);
    const uintptr_t colour = at<uintptr_t>(jobAt + job::colour), entries = at<uintptr_t>(jobAt + job::entries);
    const int count = at<int>(jobAt + job::entryCount);
    auto skip = [](const char* why) {
        lastSkip.store(why, std::memory_order_relaxed);
        skipped.fetch_add(1, std::memory_order_relaxed);
        return false;
    };
    if (at<uintptr_t>(jobAt + job::composer) != gameAt(fp::sig::itemComposer)) return skip("the job is not the composer's");
    if (width < 1 || height < 1 || width > 8192 || height > 8192 || !colour || count < 1 || count > 64 || !entries)
        return skip("the job's size, texture or entries are out of range");
    if (!readable(colour, field::texture::glName + 4) || !readable(entries, size_t(count) * entry::stride))
        return skip("the job's texture or entries are not readable");
    auto* owner = prepareCue();
    if (!owner) return skip("no GL context of ours here");
    auto& g = owner->api;
    if (!owner->viewApiAttempted) {
        owner->viewApiAttempted = true;
        owner->viewApiFailed = !loadViewEffectsApi(g);
    }
    if (owner->viewApiFailed || !loadExtra()) return skip("OpenGL 3 entry points missing");
    Resources* const r = resourcesFor(g, owner->context);
    if (!r) return skip("our id programs or targets failed");

    const uintptr_t unitActiveAt = gameAt(fp::sig::textureUnitActive), unitCacheAt = gameAt(fp::sig::textureUnitCache);
    const auto bindUnit = (UnitBind)gameAt(fp::sig::textureUnitBind);
    const auto setQuad = (QuadSet)gameAt(fp::sig::composerQuad);
    const auto drawQuad = (QuadDraw)gameAt(fp::sig::composerQuadDraw);
    const auto setMode = (ModeSet)gameAt(fp::sig::blendModeSet);
    const auto setStreams = (Streams)gameAt(fp::sig::composerStreams);
    const auto colourDraw = (LayerDraw)gameAt(fp::sig::layerColourDraw);
    const auto patternDraw = (LayerDraw)gameAt(fp::sig::layerPatternDraw);
    const auto overlayDraw = (LayerDraw)gameAt(fp::sig::layerOverlayDraw);
    auto* const programCache = (GLuint*)gameAt(fp::sig::programCache);
    const unsigned priorMode = *(const unsigned*)gameAt(fp::sig::blendModeCache);

    constexpr unsigned cachedUnits = 16;
    const int priorUnit = *(const int*)unitActiveAt;
    unsigned char priorCache[cachedUnits * 16];
    memcpy(priorCache, (const void*)unitCacheAt, sizeof priorCache);
    drainGl();
    GLint framebuffer = 0, readFramebuffer = 0, viewport[4]{}, activeTexture = 0;
    GLint ownPrior[2]{};
    GLboolean colorMask[4]{}, scissor = 0, depthTest = 0;
    GLfloat clear[4]{};
    glGetIntegerv(glc::drawFramebufferBinding, &framebuffer);
    glGetIntegerv(glc::readFramebufferBinding, &readFramebuffer);
    glGetIntegerv(GL_VIEWPORT, viewport);
    glGetIntegerv(glc::activeTexture, &activeTexture);
    glGetBooleanv(GL_COLOR_WRITEMASK, colorMask);
    glGetFloatv(glc::colorClearValue, clear);
    scissor = glIsEnabled(GL_SCISSOR_TEST);
    depthTest = glIsEnabled(GL_DEPTH_TEST);
    for (unsigned i = 0; i < 2; ++i) {
        g.activeTexture(glc::texture0 + (i ? ownUnitB : ownUnitA));
        glGetIntegerv(glc::textureBinding2D, &ownPrior[i]);
    }
    if (glGetError() != GL_NO_ERROR) return skip("a GL error before the replay");

    const GLuint gameName = *(const GLuint*)(colour + field::texture::glName);
    bool ok = false;
    unsigned layersDone = 0;
    GLuint ids = 0;
    __try {
        if (!sizeTargets(g, *r, width, height)) {
            skip("our working targets could not be sized");
            __leave;
        }
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_DEPTH_TEST);
        glColorMask(1, 1, 1, 1);
        setViewport(g, 0, 0, width, height);
        // Outside every entry: no id.
        g.bindFramebuffer(glc::framebuffer, r->fbo[0]);
        glClearColor(1.f, 0.f, 0.f, 1.f);
        glClear(GL_COLOR_BUFFER_BIT);
        setMode(0x100); // no blending: the game's cache knows it
        setStreams(0x11); // the composer's position and texcoord streams (0x1EBCA0)
        for (int e = 0; e < count; ++e) {
            const uintptr_t en = entries + size_t(e) * entry::stride;
            if (!at<uintptr_t>(en + entry::base)) continue; // the composer skips these too (0x1EC0F3)
            const float* origin = (const float*)(en + entry::origin);
            const float* size = (const float*)(en + entry::size);
            const float* scale = (const float*)(en + entry::scale);
            const float rect[4] = {origin[0], origin[1], size[0] * scale[0], size[1] * scale[1]};
            // The base owns the entry's rectangle until a layer takes a texel.
            g.useProgram(r->fillProgram);
            g.uniform4f(r->fillRect, rect[0], rect[1], rect[2], rect[3]);
            drawRect(g, *r);
            const int layers = (std::min)(at<int>(en + entry::layerCount), int(entry::layerLimit));
            if (layers < 1) continue;
            setQuad(en, en + entry::size, en + entry::scale);
            const bool patterned = at<uintptr_t>(en + entry::pattern) != 0;
            for (int l = 0; l < layers; ++l) {
                const uintptr_t ly = en + entry::layers + size_t(l) * entry::layerStride;
                const uintptr_t material = at<uintptr_t>(ly + layer::material);
                const uintptr_t overlay = at<uintptr_t>(ly + layer::overlay);
                const unsigned id = idOf(material);
                // Every class draws its colour through 0x1E6CE0 (vmt+0xD8); anything else is not reviewed.
                if (!material || !readable(material, 0x10) ||
                    !readable(*(const uintptr_t*)material + 0xD8, 8) ||
                    *(const uintptr_t*)(*(const uintptr_t*)material + 0xD8) != uintptr_t(colourDraw))
                    continue;
                // This layer alone, as the game draws it: its share of the colour is the alpha it writes.
                g.bindFramebuffer(glc::framebuffer, r->fbo[2]);
                g.useProgram(*programCache); // GL back in step with the game's program cache
                bindUnit(1, at<uintptr_t>(ly + layer::mask), 0);
                if (patterned) {
                    patternDraw(material, en, ly, jobAt);
                } else if (overlay) {
                    bindUnit(int(overlayUnit), overlay, 0);
                    overlayDraw(material, en, ly, jobAt);
                } else {
                    colourDraw(material, en, ly, jobAt);
                }
                drawQuad();
                if (overlay && !patterned) bindUnit(int(overlayUnit), 0, 0);
                // Then the running owner: state (0) and this layer (2) into the next state (1), copied back.
                g.bindFramebuffer(glc::framebuffer, r->fbo[1]);
                g.useProgram(r->combineProgram);
                g.activeTexture(glc::texture0 + ownUnitA);
                g.bindTexture(GL_TEXTURE_2D, r->tex[2]);
                g.activeTexture(glc::texture0 + ownUnitB);
                g.bindTexture(GL_TEXTURE_2D, r->tex[0]);
                g.uniform1i(r->combineLayer, int(ownUnitA));
                g.uniform1i(r->combineState, int(ownUnitB));
                g.uniform1f(r->combineId, float(id));
                g.uniform2f(r->combineSize, float(r->width), float(r->height));
                g.uniform4f(r->combineRect, rect[0], rect[1], rect[2], rect[3]);
                drawRect(g, *r);
                const int x0 = (std::max)(0, int(std::floor(rect[0] * width))),
                          y0 = (std::max)(0, int(std::floor(rect[1] * height))),
                          x1 = (std::min)(int(width), int(std::ceil((rect[0] + rect[2]) * width))),
                          y1 = (std::min)(int(height), int(std::ceil((rect[1] + rect[3]) * height)));
                g.bindFramebuffer(glc::readFramebuffer, r->fbo[1]);
                g.bindFramebuffer(glc::drawFramebuffer, r->fbo[0]);
                if (x1 > x0 && y1 > y0)
                    g.blitFramebuffer(x0, y0, x1, y1, x0, y0, x1, y1, GL_COLOR_BUFFER_BIT, glc::nearest);
                g.bindFramebuffer(glc::framebuffer, r->fbo[0]);
                ++layersDone;
            }
        }
        // The ids, one byte a texel, beside the game's texture.
        const size_t bytes = size_t(width) * size_t(height);
        Texture* slot = findTexture(owner->context, colour);
        if (slot) dropTexture(g, *slot);
        slot = slotFor(g, owner->context, bytes);
        if (!slot) {
            skip("the id texture would not fit the budget");
            __leave;
        }
        g.genTextures(1, &ids);
        g.activeTexture(glc::texture0 + ownUnitA);
        g.bindTexture(GL_TEXTURE_2D, ids);
        glTexImage2D(GL_TEXTURE_2D, 0, GLint(glc::r8), width, height, 0, GL_RED, GL_UNSIGNED_BYTE, nullptr);
        g.texParameteri(GL_TEXTURE_2D, glc::minFilter, glc::nearest);
        g.texParameteri(GL_TEXTURE_2D, glc::magFilter, glc::nearest);
        g.bindFramebuffer(glc::readFramebuffer, r->fbo[0]);
        g.copyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, width, height);
        if (glGetError() != GL_NO_ERROR) {
            g.deleteTextures(1, &ids);
            ids = 0;
            skip("a GL error while replaying the layers");
            __leave;
        }
        *slot = Texture{owner->context, colour, gameName, width, height, ids, GetTickCount64()};
        bytesKept += bytes;
        ok = true;
    } __finally {
        // Units: ours raw, then every cache entry the replay changed back with its own binding.
        for (unsigned i = 0; i < 2; ++i) {
            g.activeTexture(glc::texture0 + (i ? ownUnitB : ownUnitA));
            g.bindTexture(GL_TEXTURE_2D, GLuint(ownPrior[i]));
        }
        auto* const unitCache = (unsigned char*)unitCacheAt;
        for (unsigned u = 0; u < cachedUnits; ++u) {
            const unsigned char* const was = priorCache + u * 16;
            if (!memcmp(unitCache + u * 16, was, 16)) continue;
            const uintptr_t object = *(const uintptr_t*)was;
            const GLenum target = *(const GLenum*)(was + 8);
            g.activeTexture(glc::texture0 + u);
            if (object && target && readable(object, field::texture::glName + 4))
                g.bindTexture(target, *(const GLuint*)(object + field::texture::glName));
            else
                g.bindTexture(GL_TEXTURE_2D, 0);
        }
        memcpy(unitCache, priorCache, sizeof priorCache);
        *(int*)unitActiveAt = priorUnit;
        g.activeTexture(GLenum(activeTexture));
        setMode(priorMode);
        g.useProgram(*programCache);
        g.bindFramebuffer(glc::drawFramebuffer, GLuint(framebuffer));
        g.bindFramebuffer(glc::readFramebuffer, GLuint(readFramebuffer));
        setViewport(g, viewport[0], viewport[1], viewport[2], viewport[3]);
        glColorMask(colorMask[0], colorMask[1], colorMask[2], colorMask[3]);
        glClearColor(clear[0], clear[1], clear[2], clear[3]);
        (scissor ? glEnable : glDisable)(GL_SCISSOR_TEST);
        (depthTest ? glEnable : glDisable)(GL_DEPTH_TEST);
        drainGl();
    }
    if (ok) {
        builds.fetch_add(1, std::memory_order_relaxed);
        layersReplayed.fetch_add(layersDone, std::memory_order_relaxed);
    }
    return ok;
}

static void composer(uintptr_t jobAt) {
    originalComposer(jobAt);
    // Try only: a composition started from inside one of our own passes (cueLock held on this thread) is left
    // without ids rather than waiting on ourselves.
    if (!TryAcquireSRWLockExclusive(&cueLock)) {
        lastSkip.store("the composer ran while our lock was held", std::memory_order_relaxed);
        skipped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    ++cueGlDepth;
    __try {
        __try {
            if (!cueDeletesInFlight && !cueRetiring.load() && !cueBindingFailed.load() && !stopping) build(jobAt);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            faults.fetch_add(1, std::memory_order_relaxed);
        }
    } __finally {
        --cueGlDepth;
        ReleaseSRWLockExclusive(&cueLock);
    }
}

// The id texture beside a game colour texture object, on this context; 0 when there is none or it is stale
// (the object now names another GL texture). cueLock held.
static GLuint lookup(uintptr_t object) {
    if (!object || !readable(object, field::texture::glName + 4)) return 0;
    const HGLRC context = cueCurrentContext();
    Texture* t = findTexture(context, object);
    if (!t) {
        for (const auto& any : textures)
            if (any.ids && any.object == object && any.context != context) {
                otherContext.fetch_add(1, std::memory_order_relaxed);
                break;
            }
        return 0;
    }
    if (*(const GLuint*)(object + field::texture::glName) != t->gameName) return 0;
    t->used = GetTickCount64();
    return t->ids;
}

// The held-item replay's program: the game's own vertex shader text, compiled again by us, so the mesh lands
// exactly where the game drew it, with a fragment shader that writes the id at the mesh's texcoord. The game's
// linker 0x1FA120 detaches its shaders right after glLinkProgram (glDetachShader at 0x1FA246/0x1FA251/0x1FA260),
// so a game program has no shader to reuse (glGetAttachedShaders finds none: every id program had failed on
// that); the hook on 0x1FA120 reads the vertex shader's source while it is still attached and keeps it by the
// program returned. Attribute locations as the game binds them there (vPos 0, vTex 1, vNrm 2, vCol 3, vTx2 4,
// vTng 6); shading_v passes vrTex = vTex, or vriTex with Mat.TexDsp, so both are tried.
using ProgramLink = GLuint(__fastcall*)(GLuint vertex, GLuint fragment, GLuint geometry);
static ProgramLink originalLink = nullptr;
static std::atomic<bool> linkHooked{false};
struct LinkedSource { HGLRC context; GLuint program; char* text; };
static LinkedSource linkedSources[1024];
static unsigned linkedSourceNext = 0;
static SRWLOCK linkedLock = SRWLOCK_INIT;
static std::atomic<unsigned> sourcesKept{0};

static GLuint programLink(GLuint vertex, GLuint fragment, GLuint geometry) {
    char* text = nullptr;
    __try {
        GLint length = 0;
        if (vertex && loadExtra()) extra.getShaderiv(vertex, 0x8B88 /*SHADER_SOURCE_LENGTH*/, &length);
        if (length > 0 && length < (4 << 20) && (text = (char*)malloc(size_t(length))) != nullptr) {
            GLsizei got = 0;
            extra.getShaderSource(vertex, length, &got, text);
            if (got <= 0) {
                free(text);
                text = nullptr;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        free(text);
        text = nullptr;
    }
    const GLuint program = originalLink(vertex, fragment, geometry);
    if (!program || !text) {
        free(text);
        return program;
    }
    const HGLRC context = cueCurrentContext();
    AcquireSRWLockExclusive(&linkedLock);
    LinkedSource* slot = nullptr;
    for (auto& e : linkedSources)
        if (e.text && e.context == context && e.program == program) slot = &e; // a reused name: the newer text
    if (!slot) slot = &linkedSources[linkedSourceNext++ % (sizeof linkedSources / sizeof linkedSources[0])];
    free(slot->text);
    *slot = LinkedSource{context, program, text};
    ReleaseSRWLockExclusive(&linkedLock);
    sourcesKept.fetch_add(1, std::memory_order_relaxed);
    return program;
}
// A copy of the vertex source the game linked this program with, or empty.
static std::string linkedVertexSource(HGLRC context, GLuint program) {
    std::string out;
    AcquireSRWLockShared(&linkedLock);
    for (const auto& e : linkedSources)
        if (e.text && e.context == context && e.program == program) out = e.text;
    ReleaseSRWLockShared(&linkedLock);
    return out;
}

// The driver's own words when an id program does not build: the first few go to material-id-shader.log beside
// first-person.ini, whole.
static unsigned idFailuresLogged = 0;
static void logIdFailure(const char* what, GLuint game, const char* log) {
    if (idFailuresLogged >= 4) return;
    ++idFailuresLogged;
    wchar_t text[200];
    swprintf_s(text, L"Material ids: %hs for game program %u (material-id-shader.log has the details).", what, game);
    modMessage(NoticeKind::warning, text);
    const std::wstring path = settingsFolderFile(L"material-id-shader.log");
    if (path.empty()) return;
    const HANDLE file = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    char head[200];
    const int n = snprintf(head, sizeof head, "== %s (game program %u)\r\n", what, game);
    DWORD written = 0;
    WriteFile(file, head, DWORD(n), &written, nullptr);
    if (log) WriteFile(file, log, DWORD(strlen(log)), &written, nullptr);
    WriteFile(file, "\r\n", 2, &written, nullptr);
    CloseHandle(file);
}
struct IdProgram {
    HGLRC context;
    GLuint game, program;
    GLint idTexture;
    unsigned uniformCount;
    struct Copy { GLint from, to; GLenum type; GLint count; } uniforms[24];
    bool failed;
};
static IdProgram idPrograms[48];
static unsigned idProgramNext = 0;
static std::atomic<unsigned> idProgramsBuilt{0}, idProgramsFailed{0};

static const char* const idFragment[2] = {R"glsl(#version 150
in vec2 vrTex;
uniform sampler2D materialIds;
out vec4 FragColor;
void main() { FragColor = vec4(0.0, 0.0, 0.0, texture(materialIds, vrTex).r); }
)glsl",
                                          R"glsl(#version 150
in vec2 vriTex;
uniform sampler2D materialIds;
out vec4 FragColor;
void main() { FragColor = vec4(0.0, 0.0, 0.0, texture(materialIds, vriTex).r); }
)glsl"};

static const IdProgram* idProgramFor(gl::Api& g, GLuint game) {
    const HGLRC context = cueCurrentContext();
    for (const auto& p : idPrograms)
        if (p.game == game && p.context == context && (p.program || p.failed)) return p.failed ? nullptr : &p;
    IdProgram& p = idPrograms[idProgramNext++ % (sizeof idPrograms / sizeof idPrograms[0])];
    if (p.program && p.context == context) g.deleteProgram(p.program);
    p = IdProgram{context, game, 0, -1, 0, {}, true};
    if (!game || !loadExtra()) return nullptr;
    const std::string source = linkedVertexSource(context, game);
    if (source.empty()) {
        logIdFailure("no vertex source kept (linked before the hook, or the hook is off)", game, nullptr);
        idProgramsFailed.fetch_add(1, std::memory_order_relaxed);
        return nullptr;
    }
    const GLuint vertex = g.createShader(0x8B31);
    const char* sourceText = source.c_str();
    g.shaderSource(vertex, 1, &sourceText, nullptr);
    g.compileShader(vertex);
    GLint compiled = 0;
    g.getShaderiv(vertex, 0x8B81, &compiled);
    if (!compiled) {
        char log[4096] = "";
        extra.getShaderInfoLog(vertex, sizeof log, nullptr, log);
        logIdFailure("the game's vertex source did not compile again", game, log);
        g.deleteShader(vertex);
        idProgramsFailed.fetch_add(1, std::memory_order_relaxed);
        return nullptr;
    }
    char lastLog[4096] = "";
    for (const char* text : idFragment) {
        const GLuint fs = g.createShader(0x8B30);
        g.shaderSource(fs, 1, &text, nullptr);
        g.compileShader(fs);
        const GLuint program = g.createProgram();
        g.attachShader(program, vertex);
        g.attachShader(program, fs);
        static const char* const names[] = {"vPos", "vTex", "vNrm", "vCol", "vTx2", nullptr, "vTng"};
        for (GLuint i = 0; i < 7; ++i)
            if (names[i]) g.bindAttribLocation(program, i, names[i]);
        g.linkProgram(program);
        GLint linked = 0;
        g.getProgramiv(program, 0x8B82, &linked);
        if (!linked) extra.getProgramInfoLog(program, sizeof lastLog, nullptr, lastLog);
        g.detachShader(program, vertex);
        g.detachShader(program, fs);
        g.deleteShader(fs);
        if (!linked) {
            g.deleteProgram(program);
            continue;
        }
        p.program = program;
        break;
    }
    g.deleteShader(vertex);
    drainGl();
    if (!p.program) {
        logIdFailure("the id program did not link", game, lastLog);
        idProgramsFailed.fetch_add(1, std::memory_order_relaxed);
        return nullptr;
    }
    // The uniforms the vertex stage reads, copied from the game's program before each draw.
    GLint uniformTotal = 0;
    g.getProgramiv(p.program, 0x8B86 /*ACTIVE_UNIFORMS*/, &uniformTotal);
    for (GLint i = 0; i < uniformTotal; ++i) {
        char name[64];
        GLsizei length = 0;
        GLint count = 0;
        GLenum type = 0;
        extra.getActiveUniform(p.program, GLuint(i), sizeof name, &length, &count, &type, name);
        if (!strcmp(name, "materialIds")) {
            p.idTexture = g.getUniformLocation(p.program, name);
            continue;
        }
        if (char* bracket = strchr(name, '[')) *bracket = 0;
        const GLint to = g.getUniformLocation(p.program, name), from = g.getUniformLocation(game, name);
        if (to < 0 || from < 0 || p.uniformCount >= sizeof p.uniforms / sizeof p.uniforms[0]) {
            logIdFailure(to < 0 || from < 0 ? "a vertex uniform missing from the game program" : "too many vertex uniforms",
                         game, name);
            g.deleteProgram(p.program);
            p.program = 0;
            idProgramsFailed.fetch_add(1, std::memory_order_relaxed);
            return nullptr;
        }
        p.uniforms[p.uniformCount++] = {from, to, type, count};
    }
    // Uniform blocks bound where the game's program has the same block.
    GLint blocks = 0;
    g.getProgramiv(p.program, 0x8A36 /*ACTIVE_UNIFORM_BLOCKS*/, &blocks);
    for (GLint b = 0; b < blocks; ++b) {
        char name[64];
        GLsizei length = 0;
        extra.getActiveUniformBlockName(p.program, GLuint(b), sizeof name, &length, name);
        const GLuint theirs = extra.getUniformBlockIndex(game, name);
        GLint binding = 0;
        if (theirs != 0xFFFFFFFFu) extra.getActiveUniformBlockiv(game, theirs, 0x8A3F /*UNIFORM_BLOCK_BINDING*/, &binding);
        extra.uniformBlockBinding(p.program, GLuint(b), GLuint(binding));
    }
    if (glGetError() != GL_NO_ERROR || p.idTexture < 0) {
        g.deleteProgram(p.program);
        p.program = 0;
        idProgramsFailed.fetch_add(1, std::memory_order_relaxed);
        return nullptr;
    }
    p.failed = false;
    idProgramsBuilt.fetch_add(1, std::memory_order_relaxed);
    return &p;
}

// The game's current values into ours (the program in use is ours; values read from the game's).
static void copyUniforms(const IdProgram& p) {
    for (unsigned i = 0; i < p.uniformCount; ++i) {
        const auto& c = p.uniforms[i];
        GLfloat f[16 * 4];
        GLint n[16];
        const GLint count = (std::min)(c.count, GLint(4));
        for (GLint k = 0; k < count; ++k) {
            switch (c.type) {
            case GL_FLOAT: extra.getUniformfv(p.game, c.from + k, f + k); break;
            case 0x8B50: extra.getUniformfv(p.game, c.from + k, f + k * 2); break; // vec2
            case 0x8B51: extra.getUniformfv(p.game, c.from + k, f + k * 3); break; // vec3
            case 0x8B52: extra.getUniformfv(p.game, c.from + k, f + k * 4); break; // vec4
            case 0x8B5B: extra.getUniformfv(p.game, c.from + k, f + k * 9); break; // mat3
            case 0x8B5C: extra.getUniformfv(p.game, c.from + k, f + k * 16); break; // mat4
            default: extra.getUniformiv(p.game, c.from + k, n + k); break;
            }
        }
        switch (c.type) {
        case GL_FLOAT: extra.uniform1fv(c.to, count, f); break;
        case 0x8B50: extra.uniform2fv(c.to, count, f); break;
        case 0x8B51: extra.uniform3fv(c.to, count, f); break;
        case 0x8B52: extra.uniform4fv(c.to, count, f); break;
        case 0x8B5B: extra.uniformMatrix3fv(c.to, count, GL_FALSE, f); break;
        case 0x8B5C: extra.uniformMatrix4fv(c.to, count, GL_FALSE, f); break;
        default: extra.uniform1iv(c.to, count, n); break;
        }
    }
}
} // namespace matid

// Called by the held-item replay right after a part's own G draw, with that part's program and textures bound by
// the game: draws the part again with the id program, writing the id into the mask's alpha. False when the part's
// colour texture has no ids or its program could not be mirrored; the alpha then stays 255 (no detail).
static bool drawMaterialIds(gl::Api& g, uintptr_t unitCacheAt, void (*draw)(uintptr_t), uintptr_t argument) {
    if (!matid::hooked.load(std::memory_order_relaxed)) return false;
    const uintptr_t colour = *(const uintptr_t*)unitCacheAt; // unit 0's object: the item submit binds set+0x10 there
    const GLuint ids = matid::lookup(colour);
    if (!ids) return false;
    const GLuint game = *(const GLuint*)gameAt(fp::sig::programCache);
    const matid::IdProgram* p = matid::idProgramFor(g, game);
    if (!p) return false;
    g.useProgram(p->program);
    matid::copyUniforms(*p);
    g.activeTexture(glc::texture0 + matid::ownUnitA);
    g.bindTexture(GL_TEXTURE_2D, ids);
    g.uniform1i(p->idTexture, int(matid::ownUnitA));
    glDisable(GL_BLEND);
    glColorMask(0, 0, 0, 1);
    draw(argument);
    g.useProgram(game);
    return true;
}

static bool admitMaterialIdsEntries(const fp::ImageView& file) {
    bool admitted = false;
    __try {
        admitted = silhouetteAdmitted && layout.optionalGroupAvailable("materialIds") &&
                   loadedOptionalContractsImpl(file, "materialIds");
    } __except (EXCEPTION_EXECUTE_HANDLER) { admitted = false; }
    if (!admitted && matid::hooked.load()) {
        snprintf(faultDetail, sizeof faultDetail, "material id contracts changed after hook creation; restart required");
        return false;
    }
    if (!admitted)
        for (unsigned t = 0; t < fp::sig::targetCount; ++t) {
            const char* group = fp::sig::targetOptionalGroups[t];
            if (group && !strcmp(group, "materialIds")) layout.rva[t] = 0;
        }
    matid::admitted = admitted;
    return true;
}

static void createMaterialIdsHook() {
    matid::hooked = matid::admitted && silhouetteHooked &&
                    MH_CreateHook((void*)gameAt(fp::sig::itemComposer), (void*)matid::composer,
                                  (void**)&matid::originalComposer) == MH_OK;
    // The linker hook keeps vertex sources for the id programs; without it they cannot build (logged).
    matid::linkHooked = matid::hooked &&
                        MH_CreateHook((void*)gameAt(fp::sig::programLink), (void*)matid::programLink,
                                      (void**)&matid::originalLink) == MH_OK;
}
static bool materialIdsReady() { return matid::hooked.load(std::memory_order_relaxed); }
