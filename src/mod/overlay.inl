// Drawn over the frame: the attack wind-up arc (OpenGL); the game windows that free the cursor; crouch.
// Part of first_person.cpp (one translation unit): included there, in order.
#include "../cue_resources.h"
namespace gl {
typedef char GLchar;
typedef ptrdiff_t GLsizeiptr;
using CreateShader = GLuint(APIENTRY*)(GLenum);
using ShaderSource = void(APIENTRY*)(GLuint, GLsizei, const GLchar* const*, const GLint*);
using CompileShader = void(APIENTRY*)(GLuint);
using GetShaderiv = void(APIENTRY*)(GLuint, GLenum, GLint*);
using DeleteShader = void(APIENTRY*)(GLuint);
using CreateProgram = GLuint(APIENTRY*)();
using AttachShader = void(APIENTRY*)(GLuint, GLuint);
using DetachShader = void(APIENTRY*)(GLuint, GLuint);
using BindAttribLocation = void(APIENTRY*)(GLuint, GLuint, const GLchar*);
using LinkProgram = void(APIENTRY*)(GLuint);
using GetProgramiv = void(APIENTRY*)(GLuint, GLenum, GLint*);
using DeleteProgram = void(APIENTRY*)(GLuint);
using UseProgram = void(APIENTRY*)(GLuint);
using GetUniformLocation = GLint(APIENTRY*)(GLuint, const GLchar*);
using Uniform4f = void(APIENTRY*)(GLint, GLfloat, GLfloat, GLfloat, GLfloat);
using GenVertexArrays = void(APIENTRY*)(GLsizei, GLuint*);
using DeleteVertexArrays = void(APIENTRY*)(GLsizei, const GLuint*);
using BindVertexArray = void(APIENTRY*)(GLuint);
using GenBuffers = void(APIENTRY*)(GLsizei, GLuint*);
using DeleteBuffers = void(APIENTRY*)(GLsizei, const GLuint*);
using BindBuffer = void(APIENTRY*)(GLenum, GLuint);
using BufferData = void(APIENTRY*)(GLenum, GLsizeiptr, const void*, GLenum);
using VertexAttribPointer = void(APIENTRY*)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void*);
using EnableVertexAttribArray = void(APIENTRY*)(GLuint);
using BlendFuncSeparate = void(APIENTRY*)(GLenum, GLenum, GLenum, GLenum);
using BlendEquationSeparate = void(APIENTRY*)(GLenum, GLenum);
using GetStringi = const GLubyte*(APIENTRY*)(GLenum, GLuint);
using ColorMaskIndexed = void(APIENTRY*)(GLuint, GLboolean, GLboolean, GLboolean, GLboolean);
using EnableIndexed = void(APIENTRY*)(GLenum, GLuint);
using BlendFuncSeparateIndexed = void(APIENTRY*)(GLuint, GLenum, GLenum, GLenum, GLenum);
using BlendEquationSeparateIndexed = void(APIENTRY*)(GLuint, GLenum, GLenum);
using ViewportIndexed = void(APIENTRY*)(GLuint, GLfloat, GLfloat, GLfloat, GLfloat);
using BindFragDataLocation = void(APIENTRY*)(GLuint, GLuint, const GLchar*);
struct Api {
    bool indexedMasks = false, indexedBlend = false, viewportArrays = false, modernShaders = false;
    GLint maxDrawBuffers = 0;
    ColorMaskIndexed colorMaskIndexed = nullptr;
    EnableIndexed enableIndexed = nullptr, disableIndexed = nullptr;
    BlendFuncSeparateIndexed blendFuncIndexed = nullptr;
    BlendEquationSeparateIndexed blendEquationIndexed = nullptr;
    ViewportIndexed viewportIndexed = nullptr;
    BindFragDataLocation bindFragDataLocation = nullptr;
    CreateShader createShader;
    ShaderSource shaderSource;
    CompileShader compileShader;
    GetShaderiv getShaderiv;
    DeleteShader deleteShader;
    CreateProgram createProgram;
    AttachShader attachShader;
    DetachShader detachShader;
    BindAttribLocation bindAttribLocation;
    LinkProgram linkProgram;
    GetProgramiv getProgramiv;
    DeleteProgram deleteProgram;
    UseProgram useProgram;
    GetUniformLocation getUniformLocation;
    Uniform4f uniform4f;
    GenVertexArrays genVertexArrays;
    DeleteVertexArrays deleteVertexArrays;
    BindVertexArray bindVertexArray;
    GenBuffers genBuffers;
    DeleteBuffers deleteBuffers;
    BindBuffer bindBuffer;
    BufferData bufferData;
    VertexAttribPointer vertexAttribPointer;
    EnableVertexAttribArray enableVertexAttribArray;
    BlendFuncSeparate blendFuncSeparate;
    BlendEquationSeparate blendEquationSeparate;
    void(APIENTRY* getIntegerv)(GLenum, GLint*);
    GLenum(APIENTRY* getError)();
};
} // namespace gl
static void drainGl() {
    for (int i = 0; i < 16 && glGetError() != GL_NO_ERROR; ++i) {}
}
template <class T> static bool loadGl(T& f, const char* name) {
    f = (T)wglGetProcAddress(name);
    return f && (uintptr_t)f > 3 && (intptr_t)f != -1;
}
// Capabilities belong to the current context. A missing advertised entry point
// omits this optional cue; it must never turn an indexed family into broadcast writes.
static bool loadCueStateApi(gl::Api& g) {
    drainGl();
    const char* version = (const char*)glGetString(GL_VERSION);
    if (!version || version[0] < '0' || version[0] > '9') return false; // Not desktop GL.
    char* end = nullptr;
    const long major = std::strtol(version, &end, 10);
    if (*end != '.' || end[1] < '0' || end[1] > '9') return false;
    const long minor = std::strtol(end + 1, &end, 10);
    if (major < 2 || major > 99 || minor < 0 || minor > 99 ||
        (*end && *end != '.' && *end != ' ')) return false;
    if (*end == '.') {
        ++end;
        if (*end < '0' || *end > '9') return false;
        while (*end >= '0' && *end <= '9') ++end;
        if (*end && *end != ' ') return false;
    }
    const auto atLeast = [=](int a, int b) { return major > a || (major == a && minor >= b); };
    bool masks = false, blend = false, amdBlend = false, viewports = false;
    const auto extension = [&](const char* name, size_t size) {
        const auto is = [&](const char* wanted) {
            return std::strlen(wanted) == size && std::memcmp(name, wanted, size) == 0;
        };
        masks = masks || is("GL_EXT_draw_buffers2");
        blend = blend || is("GL_ARB_draw_buffers_blend");
        amdBlend = amdBlend || is("GL_AMD_draw_buffers_blend");
        viewports = viewports || is("GL_ARB_viewport_array");
    };
    if (atLeast(3, 0)) {
        gl::GetStringi getStringi = nullptr;
        GLint count = -1;
        if (!loadGl(getStringi, "glGetStringi")) return false;
        glGetIntegerv(0x821D /*NUM_EXTENSIONS*/, &count);
        if (count < 0 || count > 65536 || glGetError() != GL_NO_ERROR) return false;
        for (GLint i = 0; i < count; ++i) {
            const char* name = (const char*)getStringi(GL_EXTENSIONS, GLuint(i));
            if (!name || !*name || std::strpbrk(name, " \t\r\n")) return false;
            extension(name, std::strlen(name));
        }
    } else {
        const char* names = (const char*)glGetString(GL_EXTENSIONS);
        if (!names || std::strpbrk(names, "\t\r\n")) return false;
        while (*names) {
            while (*names == ' ') ++names;
            const char* first = names;
            while (*names && *names != ' ') ++names;
            if (names != first) extension(first, size_t(names - first));
        }
    }
    g.modernShaders = atLeast(3, 0);
    g.indexedMasks = g.modernShaders || masks;
    g.indexedBlend = atLeast(4, 0) || blend || amdBlend;
    g.viewportArrays = atLeast(4, 1) || viewports;
    if (g.indexedBlend && !g.indexedMasks) return false; // Inconsistent extension inventory.
    if (g.indexedMasks &&
        !loadGl(g.colorMaskIndexed, g.modernShaders ? "glColorMaski" : "glColorMaskIndexedEXT")) return false;
    if ((g.indexedMasks || g.viewportArrays) &&
        (!loadGl(g.enableIndexed, g.modernShaders ? "glEnablei" : "glEnableIndexedEXT") ||
         !loadGl(g.disableIndexed, g.modernShaders ? "glDisablei" : "glDisableIndexedEXT"))) return false;
    if (g.indexedBlend) {
        const char* function = atLeast(4, 0) ? "glBlendFuncSeparatei" :
                               blend ? "glBlendFuncSeparateiARB" : "glBlendFuncSeparateIndexedAMD";
        const char* equation = atLeast(4, 0) ? "glBlendEquationSeparatei" :
                               blend ? "glBlendEquationSeparateiARB" : "glBlendEquationSeparateIndexedAMD";
        if (!loadGl(g.blendFuncIndexed, function) || !loadGl(g.blendEquationIndexed, equation)) return false;
    }
    if (g.viewportArrays && !loadGl(g.viewportIndexed, "glViewportIndexedf")) return false;
    if (g.modernShaders && !loadGl(g.bindFragDataLocation, "glBindFragDataLocation")) return false;
    glGetIntegerv(0x8824 /*MAX_DRAW_BUFFERS*/, &g.maxDrawBuffers);
    return g.maxDrawBuffers > 0 && g.maxDrawBuffers <= 64 && glGetError() == GL_NO_ERROR;
}
static bool loadCueApi(gl::Api& g) {
    if (!loadCueStateApi(g)) return false;
    g.getIntegerv = glGetIntegerv;
    g.getError = glGetError;
    return loadGl(g.createShader, "glCreateShader") && loadGl(g.shaderSource, "glShaderSource") &&
           loadGl(g.compileShader, "glCompileShader") && loadGl(g.getShaderiv, "glGetShaderiv") &&
           loadGl(g.deleteShader, "glDeleteShader") && loadGl(g.createProgram, "glCreateProgram") &&
           loadGl(g.attachShader, "glAttachShader") && loadGl(g.detachShader, "glDetachShader") &&
           loadGl(g.bindAttribLocation, "glBindAttribLocation") && loadGl(g.linkProgram, "glLinkProgram") &&
           loadGl(g.getProgramiv, "glGetProgramiv") && loadGl(g.deleteProgram, "glDeleteProgram") &&
           loadGl(g.useProgram, "glUseProgram") && loadGl(g.getUniformLocation, "glGetUniformLocation") &&
           loadGl(g.uniform4f, "glUniform4f") && loadGl(g.genVertexArrays, "glGenVertexArrays") &&
           loadGl(g.deleteVertexArrays, "glDeleteVertexArrays") && loadGl(g.bindVertexArray, "glBindVertexArray") &&
           loadGl(g.genBuffers, "glGenBuffers") && loadGl(g.deleteBuffers, "glDeleteBuffers") &&
           loadGl(g.bindBuffer, "glBindBuffer") && loadGl(g.bufferData, "glBufferData") &&
           loadGl(g.vertexAttribPointer, "glVertexAttribPointer") &&
           loadGl(g.enableVertexAttribArray, "glEnableVertexAttribArray") &&
           loadGl(g.blendFuncSeparate, "glBlendFuncSeparate") &&
           loadGl(g.blendEquationSeparate, "glBlendEquationSeparate");
}
static HGLRC(WINAPI* cueCurrentContext)() = wglGetCurrentContext;
static HDC(WINAPI* cueCurrentDC)() = wglGetCurrentDC;
static BOOL(WINAPI* originalCueMakeCurrent)(HDC, HGLRC) = wglMakeCurrent;
static BOOL(WINAPI* originalCueDeleteContext)(HGLRC) = wglDeleteContext;
static bool (*cueApiLoader)(gl::Api&) = loadCueApi;
struct CueContext {
    HGLRC context = nullptr;
    gl::Api api{};
    fp::CueResources resources{};
    bool attempted = false, failed = false;
};
// A core WGL switch retires before unbinding. Only simultaneously current
// contexts need slots; excess contexts omit the optional cue.
static CueContext cueContexts[8];
static SRWLOCK cueLock = SRWLOCK_INIT;
static unsigned cueDeletesInFlight = 0; // cueLock; blocks handle reuse during native deletion
static volatile LONG cueCallsInFlight = 0;
static CueContext* findCueContext(HGLRC context) {
    if (context)
        for (auto& owner : cueContexts)
            if (owner.context == context) return &owner;
    return nullptr;
}
static bool retireCueContext(CueContext& owner) {
    __try {
        if (owner.resources.live()) owner.resources.retire(owner.api);
        owner = CueContext{};
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        owner.failed = true;
        return false; // Keep incomplete ownership visible to the stop drain.
    }
}
static bool retireCurrentCue() {
    bool resourcesRetired = true;
    AcquireSRWLockExclusive(&cueLock);
    __try {
        if (auto* owner = findCueContext(cueCurrentContext())) resourcesRetired = retireCueContext(*owner);
    } __finally {
        ReleaseSRWLockExclusive(&cueLock);
    }
    return resourcesRetired;
}
static void failCurrentCue() {
    AcquireSRWLockExclusive(&cueLock);
    __try {
        const HGLRC context = cueCurrentContext();
        if (auto* owner = findCueContext(context)) {
            retireCueContext(*owner);
            owner->context = context;
            owner->attempted = owner->failed = true;
        }
    } __finally {
        ReleaseSRWLockExclusive(&cueLock);
    }
}
static bool awaitCueRetirement(DWORD timeout = 2000) {
    const ULONGLONG started = GetTickCount64();
    for (;;) {
        bool pending = true;
        if (TryAcquireSRWLockExclusive(&cueLock)) {
            pending = cueDeletesInFlight != 0 || InterlockedCompareExchange(&cueCallsInFlight, 0, 0) != 0;
            for (const auto& owner : cueContexts) pending = pending || owner.resources.live();
            if (!pending)
                for (auto& owner : cueContexts) owner = CueContext{};
            ReleaseSRWLockExclusive(&cueLock);
        }
        if (!pending) return true;
        if (GetTickCount64() - started >= timeout) return false;
        Sleep(1); // Render/WGL callbacks retain exclusive responsibility for GL deletion.
    }
}
static BOOL WINAPI cueMakeCurrent(HDC dc, HGLRC context) {
    InterlockedIncrement(&cueCallsInFlight);
    __try {
        const HGLRC before = cueCurrentContext();
        if (before && (before != context || cueCurrentDC() != dc ||
                       InterlockedCompareExchange(&stopping, 0, 0)))
            retireCurrentCue();
        return originalCueMakeCurrent(dc, context);
    } __finally {
        InterlockedDecrement(&cueCallsInFlight);
    }
}
static BOOL WINAPI cueDeleteContext(HGLRC context) {
    InterlockedIncrement(&cueCallsInFlight);
    AcquireSRWLockExclusive(&cueLock);
    ++cueDeletesInFlight;
    if (context && context == cueCurrentContext())
        if (auto* owner = findCueContext(context)) retireCueContext(*owner);
    ReleaseSRWLockExclusive(&cueLock);
    BOOL deleted = FALSE;
    DWORD error = 0;
    __try {
        deleted = originalCueDeleteContext(context);
        error = GetLastError();
        return deleted;
    } __finally {
        AcquireSRWLockExclusive(&cueLock);
        // A successful native deletion ends this context's identity. No GL
        // entry point may use its record after the numeric HGLRC is reused.
        if (deleted)
            if (auto* owner = findCueContext(context)) *owner = CueContext{};
        --cueDeletesInFlight;
        ReleaseSRWLockExclusive(&cueLock);
        InterlockedDecrement(&cueCallsInFlight);
        SetLastError(error);
    }
}
static CueContext* prepareCue() { // cueLock held by the draw
    const HGLRC context = cueCurrentContext();
    if (!context || cueDeletesInFlight || InterlockedCompareExchange(&stopping, 0, 0)) return nullptr;
    auto* owner = findCueContext(context);
    if (!owner)
        for (auto& slot : cueContexts)
            if (!slot.context) {
                slot.context = context;
                owner = &slot;
                break;
            }
    if (!owner) return nullptr;
    if (!owner->attempted) {
        owner->attempted = true;
        owner->failed = !cueApiLoader(owner->api);
    }
    return owner->failed ? nullptr : owner;
}
static void cueEnable(const gl::Api& g, GLenum cap, bool enabled) {
    if ((cap == GL_BLEND && g.indexedMasks) || (cap == GL_SCISSOR_TEST && g.viewportArrays))
        (enabled ? g.enableIndexed : g.disableIndexed)(cap, 0);
    else
        (enabled ? glEnable : glDisable)(cap);
}
// Only output zero may be active. This is a temporary frame omission, not an
// API failure: the next native swap can have different framebuffer routing.
static bool cueOutputReady(const gl::Api& g) {
    drainGl();
    for (GLint i = 0; i < g.maxDrawBuffers; ++i) {
        GLint destination = -1;
        glGetIntegerv(0x8825 /*DRAW_BUFFER0*/ + GLenum(i), &destination);
        if (destination < 0 || (i == 0 ? destination == GL_NONE : destination != GL_NONE)) return false;
    }
    return glGetError() == GL_NO_ERROR;
}
// The game's "CursorCom" option (the player's combat cursor colour) is stored here as RGBA bytes; its
// setter is 0x12B830 and the cursor draw 0x13E9C0 tints the combat crosshair with it.
// Returns false when the cue cannot be drawn on this context.
static bool drawCueOwned(fp::CueSide side, float x, float y, float fill, float alpha, int width, int height) {
    const int built =
        *(int*)(gameAt(fp::sig::cursorTextureSize)); // the size 0x13E740 last built the cursor texture for
    const float cursorPx = float(built >= 8 && built <= 256 ? built : fp::defaultCursorPx);
    float arc[3 * 3 * 68];
    int counts[3];
    if (!(alpha > 0) ||
        !fp::cueArc(side, x, y, cursorPx, cueSize.load(), fill, arc, int(sizeof arc / sizeof(float)), counts))
        return true;
    GLint framebuffer = 0;
    drainGl();
    glGetIntegerv(0x8CA6, &framebuffer);
    if (framebuffer != 0 || glGetError() != GL_NO_ERROR) return true; // Not the presented image.
    auto* owner = prepareCue();
    if (!owner) return false;
    auto& g = owner->api;
    if (!cueOutputReady(g)) return true;
    auto& resources = owner->resources;
    if (!resources.ready() && !resources.prepare(g)) return false;
    GLint program = 0, vao = 0, buffer = 0, viewport[4]{}, src[2]{}, dst[2]{}, eq[2]{};
    GLfloat viewportFloat[4]{};
    GLboolean mask[4]{};
    glGetIntegerv(0x8B8D, &program);
    glGetIntegerv(0x85B5, &vao);
    glGetIntegerv(0x8894, &buffer);
    if (g.viewportArrays) glGetFloatv(GL_VIEWPORT, viewportFloat);
    else glGetIntegerv(GL_VIEWPORT, viewport);
    glGetIntegerv(0x80C9, &src[0]);
    glGetIntegerv(0x80CB, &src[1]);
    glGetIntegerv(0x80C8, &dst[0]);
    glGetIntegerv(0x80CA, &dst[1]);
    glGetIntegerv(0x8009, &eq[0]);
    glGetIntegerv(0x883D, &eq[1]);
    glGetBooleanv(GL_COLOR_WRITEMASK, mask);
    const GLenum caps[] = {GL_BLEND,        GL_DEPTH_TEST,   GL_CULL_FACE,
                           GL_SCISSOR_TEST, GL_STENCIL_TEST, 0x8DB9 /*FRAMEBUFFER_SRGB*/,
                           0x8C89 /*RASTERIZER_DISCARD*/};
    GLboolean enabled[7];
    for (int i = 0; i < 7; ++i) enabled[i] = glIsEnabled(caps[i]);
    if (glGetError() != GL_NO_ERROR) return true; // Never restore an incomplete snapshot.
    bool ok = false;
    __try {
        if (g.viewportArrays) g.viewportIndexed(0, 0, 0, float(width), float(height));
        else glViewport(0, 0, width, height);
        for (int i = 1; i < 7; ++i) cueEnable(g, caps[i], false);
        cueEnable(g, GL_BLEND, true);
        if (g.indexedBlend) {
            g.blendEquationIndexed(0, 0x8006, 0x8006);
            g.blendFuncIndexed(0, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        } else {
            g.blendEquationSeparate(0x8006, 0x8006);
            g.blendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        }
        if (g.indexedMasks) g.colorMaskIndexed(0, 1, 1, 1, 1);
        else glColorMask(1, 1, 1, 1);
        g.useProgram(resources.program);
        g.bindVertexArray(resources.vao);
        g.bindBuffer(0x8892, resources.vbo);
        // The arc itself is white; it is drawn the way 0x13E9C0 draws the cursor so the two match: the lines in
        // black at four one-pixel offsets, then everything multiplied by the player's combat cursor colour.
        const int total = counts[0] + counts[1] + counts[2];
        for (int i = 0; i < total; ++i) {
            arc[i * 3] = arc[i * 3] / float(width) * 2 - 1;
            arc[i * 3 + 1] = 1 - arc[i * 3 + 1] / float(height) * 2;
        }
        const float a = alpha * float(cueOpacity.load()) / 100.f;
        const unsigned char* colour = (const unsigned char*)(gameAt(fp::sig::combatCursorColour));
        const float tint[3] = {float(colour[0]) / 255.f, float(colour[1]) / 255.f, float(colour[2]) / 255.f};
        {
            static float shifted[3 * 3 * 68];
            const float ox = 2.f / float(width), oy = 2.f / float(height);
            const float off[4][2] = {{-ox, 0}, {ox, 0}, {0, -oy}, {0, oy}};
            g.uniform4f(resources.color, 0, 0, 0, a * a);
            for (const auto& o : off) {
                for (int i = 0; i < total; ++i) {
                    shifted[i * 3] = arc[i * 3] + o[0];
                    shifted[i * 3 + 1] = arc[i * 3 + 1] + o[1];
                    shifted[i * 3 + 2] = arc[i * 3 + 2];
                }
                g.bufferData(0x8892, ptrdiff_t(sizeof(float) * 3 * total), shifted, 0x88E8);
                glDrawArrays(GL_TRIANGLE_STRIP, 0, counts[0]);
                if (counts[2]) glDrawArrays(GL_TRIANGLE_STRIP, counts[0] + counts[1], counts[2]);
            }
        }
        g.bufferData(0x8892, ptrdiff_t(sizeof(float) * 3 * total), arc, 0x88E8);
        g.uniform4f(resources.color, tint[0], tint[1], tint[2], a);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, counts[0]);
        if (counts[1]) glDrawArrays(GL_TRIANGLE_STRIP, counts[0], counts[1]);
        if (counts[2]) glDrawArrays(GL_TRIANGLE_STRIP, counts[0] + counts[1], counts[2]);
        ok = glGetError() == GL_NO_ERROR;
    } __finally {
        g.useProgram(GLuint(program));
        g.bindVertexArray(GLuint(vao));
        g.bindBuffer(0x8892, GLuint(buffer));
        if (g.viewportArrays)
            g.viewportIndexed(0, viewportFloat[0], viewportFloat[1], viewportFloat[2], viewportFloat[3]);
        else glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
        if (g.indexedBlend) {
            g.blendFuncIndexed(0, src[0], dst[0], src[1], dst[1]);
            g.blendEquationIndexed(0, eq[0], eq[1]);
        } else {
            g.blendFuncSeparate(src[0], dst[0], src[1], dst[1]);
            g.blendEquationSeparate(eq[0], eq[1]);
        }
        if (g.indexedMasks) g.colorMaskIndexed(0, mask[0], mask[1], mask[2], mask[3]);
        else glColorMask(mask[0], mask[1], mask[2], mask[3]);
        for (int i = 0; i < 7; ++i) cueEnable(g, caps[i], enabled[i] != 0);
        drainGl();
    }
    if (ok) ++cueDraws;
    return ok;
}
static bool drawCue(fp::CueSide side, float x, float y, float fill, float alpha, int width, int height) {
    bool ok = false;
    AcquireSRWLockExclusive(&cueLock);
    __try {
        if (!InterlockedCompareExchange(&stopping, 0, 0)) {
            ok = drawCueOwned(side, x, y, fill, alpha, width, height);
            if (!ok)
                if (auto* owner = findCueContext(cueCurrentContext())) {
                    if (owner->resources.live()) owner->resources.retire(owner->api);
                    owner->failed = true;
                }
        }
    } __finally {
        ReleaseSRWLockExclusive(&cueLock);
    }
    return ok;
}
static void presentCue(fp::CueSide side, LONG x, LONG y, float fill, float alpha) {
    if (InterlockedCompareExchange(&stopping, 0, 0)) {
        retireCurrentCue(); // Also runs on cue-free frames while the worker waits.
        cueGl = 0;
        return;
    }
    if (side == fp::CueSide::none) return;
    HWND w = hookedWindow;
    RECT rect;
    if (!w || !GetClientRect(w, &rect) || rect.right < 40 || rect.bottom < 40) return;
    __try {
        cueGl = drawCue(side, float(x), float(y), fill, alpha, rect.right, rect.bottom) ? 1 : -1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        failCurrentCue();
        cueGl = -1;
    }
}
// The game's own windows: a top-level form of a GUI task is open while
// its visible byte (+0x79) is set and its closing byte (+0x11B) is not; +0xB8 is the game's tag for the kind
// (the test 0x29030 makes). The end-of-match menu is the form at base+0x4F01B0.
static constexpr unsigned windowInventory = 0xCE1C2010, windowLoot = 0xCE1C2040, windowContainer = 0xCE1C2013,
                          windowDescription = 0xCE1C2012;
static constexpr unsigned windowShop = 0xCE1C2023,
                          windowArsenal = 0xCE1C2024; // the arena merchant and its arsenal
// Native arena forms: 194CD0, 19A370, 198AF0 and 198E40 respectively.
// Their visible lifetime, not the hovered child row, owns the free cursor.
static constexpr unsigned windowMatches = 0xCE10AE10, windowCompany = 0xCE10AE20,
                          windowCompanyRecords = 0xCE10AE21, windowCompanyStatus = 0xCE10AE22;
// dialogueActive: a u8 flag; dialogueChoices: the choices form (both sit on the game's view, not in a window list).
static bool formShown(uintptr_t f) {
    return readable(f, 0x120) && *(unsigned char*)(f + field::form::visible) &&
           !*(unsigned char*)(f + field::form::closing);
}
// Both the flag and the choices on screen: a flag left set by a path that skipped its clear never holds the view.
static bool dialogueOpen() {
    return *(unsigned char*)gameAt(fp::sig::dialogueActive) &&
           formShown(*(uintptr_t*)gameAt(fp::sig::dialogueChoices));
}
// Which of them a task shows: 1 a window, 2 an item description (a popup that may come and go quickly).
static unsigned taskShowsWindow(uintptr_t task) {
    if (!readable(task, 0xb8)) return 0;
    const int n = *(int*)(task + field::guiTask::formCount);
    const uintptr_t forms = *(uintptr_t*)(task + field::guiTask::forms);
    if (n <= 0 || n > 64 || !readable(forms, size_t(n) * 8)) return 0;
    unsigned shown = 0;
    for (int i = 0; i < n; ++i) {
        const uintptr_t f = *(uintptr_t*)(forms + size_t(i) * 8);
        if (!formShown(f)) continue;
        const unsigned kind = *(unsigned*)(f + field::form::kind);
        if (kind == windowInventory || kind == windowLoot || kind == windowContainer || kind == windowShop ||
            kind == windowArsenal || kind == windowMatches || kind == windowCompany ||
            kind == windowCompanyRecords || kind == windowCompanyStatus)
            shown |= 1;
        else if (kind == windowDescription)
            shown |= 2;
    }
    return shown;
}
static unsigned gameWindowOpen() {
    const uintptr_t inGame = *(uintptr_t*)(gameAt(fp::sig::inGameGuiTask)),
                    activeTask = *(uintptr_t*)(gameAt(fp::sig::activeGuiTask)),
                    desktop = *(uintptr_t*)(gameAt(fp::sig::guiDesktop));
    unsigned shown = taskShowsWindow(inGame);
    // Native 139430 visits both owners: arena windows can be children of the
    // desktop even while the in-game task is active. Omitting it lets look
    // recapture at a form's empty margins as native hover returns to the world.
    if (desktop != inGame) shown |= taskShowsWindow(desktop);
    if (activeTask != inGame && activeTask != desktop) shown |= taskShowsWindow(activeTask);
    const uintptr_t endMenu = *(uintptr_t*)(gameAt(fp::sig::endMatchMenu));
    if (endMenu && formShown(endMenu)) shown |= 1;
    if (dialogueOpen()) shown |= 1;
    return shown;
}
// An item carried by the cursor with the button still held (the manager's capture +0x48 and drag format +0x70).
static bool guiDragging() {
    const uintptr_t m = *(uintptr_t*)(gameAt(fp::sig::guiManager));
    return readable(m, 0x78) && *(unsigned*)(m + field::gui::dragFormat) != 0 &&
           *(uintptr_t*)(m + field::gui::capture) != 0;
}
static std::atomic<unsigned> windowFreezes{0};
static constexpr ULONGLONG descriptionSettleMs =
    300; // a description alone holds the view only once it stays (a popup passed over does not)
static ULONGLONG descriptionSince = 0;
static void windowFreeStep() {
    bool want = false;
    if (windowsFreeze.load() && InterlockedCompareExchange(&active, 0, 0) &&
        !InterlockedCompareExchange(&stopping, 0, 0) && viewMode.load() < 2) {
        unsigned shown = 0;
        bool dragging = false;
        __try {
            shown = gameWindowOpen();
            dragging = InterlockedCompareExchange(&windowFree, 0, 0) && guiDragging();
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            shown = 0;
            dragging = false;
        } // a drag out of a window keeps the cursor until it ends
        const ULONGLONG now = GetTickCount64();
        if (shown & 2) {
            if (!descriptionSince) descriptionSince = now;
        } else
            descriptionSince = 0;
        want = (shown & 1) || dragging ||
               ((shown & 2) && (InterlockedCompareExchange(&windowFree, 0, 0) ||
                                now - descriptionSince >= descriptionSettleMs));
    } else
        descriptionSince = 0;
    if (want != (InterlockedCompareExchange(&windowFree, 0, 0) != 0)) {
        InterlockedExchange(&windowFree, want ? 1 : 0);
        releaseInput();
        if (want) ++windowFreezes;
    } // releaseInput: no view jump when it ends
}
// Whether there is room to stand: the level's walls and ceilings (props are not in the level grid) along a
// ray up from the head. Unknown (no level grid, a fault) counts as room.
static bool crouchHeadroomClear(uintptr_t root) {
    const uintptr_t head = *(uintptr_t*)(root + field::character::headPart);
    if (!readable(head, 0x2d0)) return true;
    const uintptr_t mesh = *(uintptr_t*)(head + field::bodyPart::meshNode);
    const uintptr_t scene = readable(mesh, 0x138) ? *(uintptr_t*)(mesh + field::sceneNode::sector) : 0;
    const uintptr_t level = levelGrid();
    if (!level || !levelRay || !levelRay2 || !readable(scene, 0x70)) return true;
    float from[3];
    for (int i = 0; i < 3; ++i)
        from[i] = *(float*)(head + field::bodyPart::poseOrigin + 4 * i) +
                  *(float*)(scene + field::sector::origin +
                            4 * i); // the head's achieved position, in the level's frame
    const float up[3] = {0, 1, 0};
    return levelBlocked(level, from, up, crouchHeadroom) != 1;
}
static bool crouchAllowed(uintptr_t root) {
    if (!root || !readable(root, 0x1300) || !InterlockedCompareExchange(&active, 0, 0) ||
        InterlockedCompareExchange(&stopping, 0, 0) ||
        !InterlockedCompareExchange(&cameraReady, 0, 0) || viewMode.load() >= 2)
        return false;
    const unsigned state = *(unsigned*)(root + field::character::stateFlags);
    return !*(unsigned char*)(root + field::character::combatStance) &&
           !(state & (0xac0 | 0x40204 | 0xa00)) &&
           !(*(unsigned*)(root + field::character::lifeFlags) & 0x100) && !dialogueOpen();
}
static bool crouchAppliesTo(uintptr_t root) {
    CrouchRequest request;
    AcquireSRWLockShared(&crouchLock);
    request = crouchRequest;
    ReleaseSRWLockShared(&crouchLock);
    if (!request.held || request.root != root || !request.mode || request.mode != crouchMode.load())
        return false;
    __try {
        return root == *(uintptr_t*)gameAt(fp::sig::playerRoot) && crouchAllowed(root) &&
               request.head == *(uintptr_t*)(root + field::character::headPart) &&
               request.scene == *(uintptr_t*)gameAt(fp::sig::currentScene);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
static fp::Crouch crouchState; // render thread, under cameraLock in swap
static uintptr_t crouchRoot = 0, crouchHead = 0, crouchScene = 0;
static bool crouchOwnerSeen = false, crouchWaitRelease = false;
static void crouchFrame() {
    CrouchRequest next;
    const int mode = crouchMode.load();
    const bool physicalKey = duckKey.load();
    bool allowed = false, room = true;
    __try {
        next.root = *(uintptr_t*)gameAt(fp::sig::playerRoot);
        next.scene = *(uintptr_t*)gameAt(fp::sig::currentScene);
        if (readable(next.root, 0x1300)) next.head = *(uintptr_t*)(next.root + field::character::headPart);
        allowed = crouchAllowed(next.root);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        next = {};
        allowed = false;
    }
    if (next.root != crouchRoot || next.head != crouchHead || next.scene != crouchScene) {
        crouchState = {};
        if (crouchOwnerSeen) crouchWaitRelease = physicalKey;
        crouchRoot = next.root;
        crouchHead = next.head;
        crouchScene = next.scene;
        if (next.root) crouchOwnerSeen = true;
    }
    if (!physicalKey) crouchWaitRelease = false;
    if (allowed && mode && crouchState.on) {
        __try {
            room = crouchHeadroomClear(next.root);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            allowed = false; // a failed native query ends this frame's request
        }
    }
    // A press that belonged to a previous character must be released before
    // either hold or toggle can start a new crouch. FP/TP share this request.
    const bool key = physicalKey && !crouchWaitRelease && !InterlockedCompareExchange(&windowFree, 0, 0);
    next.mode = mode;
    next.held = fp::crouchStep(crouchState, mode, key, allowed, room);
    AcquireSRWLockExclusive(&crouchLock);
    crouchRequest = next;
    ReleaseSRWLockExclusive(&crouchLock);
    crouchHeld = next.held;
}
