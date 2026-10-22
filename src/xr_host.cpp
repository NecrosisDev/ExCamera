// OpenXR host: session, tracking, GPU copies and the reconstruction seam. It owns no
// native rendering and runs only on the thread whose WGL context created it.
#include "xr/runtime.h"
#include "xr/backend.h"
#include "xr/eyes.h"
#include "xr/gpu_reconstruction.h"
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <vector>

namespace fp::xr {
namespace {
constexpr GLenum drawFbo = 0x8CA9, readFbo = 0x8CA8, framebuffer = 0x8D40, renderbuffer = 0x8D41, color = 0x8CE0, depth = 0x8D00;
constexpr GLenum drawFramebufferBinding = 0x8CA6, readFramebufferBinding = 0x8CAA, renderbufferBinding = 0x8CA7, framebufferComplete = 0x8CD5;
constexpr GLenum currentProgram = 0x8B8D, vertexArrayBinding = 0x85B5, textureBinding2D = 0x8069;
constexpr GLenum arrayBuffer = 0x8892, elementArrayBuffer = 0x8893, arrayBufferBinding = 0x8894, elementArrayBufferBinding = 0x8895;
constexpr GLenum samples = 0x80A9, contextProfileMask = 0x9126, rgba32f = 0x8814;
constexpr GLenum depthComponent16 = 0x81A5, depthComponent24 = 0x81A6, depthComponent32 = 0x81A7;
// Client pixel-store state is outside glPushAttrib; it is saved and restored on its own.
struct PixelStore {
    PixelStore() {
        glPushClientAttrib(GL_CLIENT_PIXEL_STORE_BIT);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    }
    ~PixelStore() { glPopClientAttrib(); }
};
// The engine may leave a GL error flagged; our own checks must start from a clean flag.
inline void drainErrors() {
    for (unsigned i = 0; i < 16 && glGetError() != GL_NO_ERROR; ++i) {}
}
template <class T> T procedure(const char* name) {
    auto p = wglGetProcAddress(name);
    require(p && uintptr_t(p) > 3 && intptr_t(p) != -1, "required GL procedure missing");
    return reinterpret_cast<T>(p);
}
struct Gl {
    using Gen = void(APIENTRY*)(GLsizei, GLuint*);
    using Delete = void(APIENTRY*)(GLsizei, const GLuint*);
    using Bind = void(APIENTRY*)(GLenum, GLuint);
    using One = void(APIENTRY*)(GLuint);
    using Attach = void(APIENTRY*)(GLenum, GLenum, GLenum, GLuint, GLint);
    using AttachBuffer = void(APIENTRY*)(GLenum, GLenum, GLenum, GLuint);
    using Storage = void(APIENTRY*)(GLenum, GLenum, GLsizei, GLsizei);
    using Status = GLenum(APIENTRY*)(GLenum);
    using Blit = void(APIENTRY*)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum);
    Gen gen = procedure<Gen>("glGenFramebuffers"), genBuffer = procedure<Gen>("glGenRenderbuffers");
    Delete remove = procedure<Delete>("glDeleteFramebuffers"), removeBuffer = procedure<Delete>("glDeleteRenderbuffers");
    Bind bind = procedure<Bind>("glBindFramebuffer"), bindBuffer = procedure<Bind>("glBindRenderbuffer"), arrayBuffer = procedure<Bind>("glBindBuffer");
    One vao = procedure<One>("glBindVertexArray"), program = procedure<One>("glUseProgram");
    Attach attach = procedure<Attach>("glFramebufferTexture2D");
    AttachBuffer attachBuffer = procedure<AttachBuffer>("glFramebufferRenderbuffer");
    Storage storage = procedure<Storage>("glRenderbufferStorage");
    Status status = procedure<Status>("glCheckFramebufferStatus");
    Blit blit = procedure<Blit>("glBlitFramebuffer");
};
// Backs up the native colour/depth target before eye replay and restores it afterwards; all
// GL state touched in between is pushed and popped so the engine's caches stay truthful.
struct Target {
    Gl& gl;
    GLuint backup = 0, texture = 0, depthBuffer = 0, eyeFbo = 0;
    GLint savedDraw = 0, savedRead = 0, savedDrawBuffer = 0, savedReadBuffer = 0, savedProgram = 0, savedVao = 0, savedArray = 0, savedElement = 0, savedRenderbuffer = 0, savedMatrixMode = 0;
    GLfloat savedProjection[16]{}, savedModelview[16]{};
    int width = 0, height = 0, depthBits = 0;
    int copyWidth = 0, copyHeight = 0; // the eye rectangle delivered from the target; 0 means the whole target
    bool pushed = false, backed = false;
    explicit Target(Gl& api) : gl(api) {}
    ~Target() {
        restore();
        if (backup) gl.remove(1, &backup);
        if (eyeFbo) gl.remove(1, &eyeFbo);
        if (texture) glDeleteTextures(1, &texture);
        if (depthBuffer) gl.removeBuffer(1, &depthBuffer);
    }
    void push() {
        glGetIntegerv(drawFramebufferBinding, &savedDraw);
        glGetIntegerv(readFramebufferBinding, &savedRead);
        glGetIntegerv(GL_DRAW_BUFFER, &savedDrawBuffer);
        glGetIntegerv(GL_READ_BUFFER, &savedReadBuffer);
        glGetIntegerv(currentProgram, &savedProgram);
        glGetIntegerv(vertexArrayBinding, &savedVao);
        glGetIntegerv(arrayBufferBinding, &savedArray);
        glGetIntegerv(elementArrayBufferBinding, &savedElement);
        glGetIntegerv(renderbufferBinding, &savedRenderbuffer);
        glGetIntegerv(GL_MATRIX_MODE, &savedMatrixMode);
        glGetFloatv(GL_PROJECTION_MATRIX, savedProjection);
        glGetFloatv(GL_MODELVIEW_MATRIX, savedModelview);
        glPushAttrib(GL_ALL_ATTRIB_BITS);
        pushed = true;
    }
    void pop() noexcept {
        if (!pushed) return;
        gl.bind(drawFbo, GLuint(savedDraw));
        gl.bind(readFbo, GLuint(savedRead));
        glPopAttrib();
        glDrawBuffer(GLenum(savedDrawBuffer));
        glReadBuffer(GLenum(savedReadBuffer));
        gl.program(GLuint(savedProgram));
        gl.vao(GLuint(savedVao));
        gl.arrayBuffer(arrayBuffer, GLuint(savedArray));
        gl.arrayBuffer(elementArrayBuffer, GLuint(savedElement));
        gl.bindBuffer(renderbuffer, GLuint(savedRenderbuffer));
        pushed = false;
        glMatrixMode(GL_PROJECTION);
        glLoadMatrixf(savedProjection);
        glMatrixMode(GL_MODELVIEW);
        glLoadMatrixf(savedModelview);
        glMatrixMode(GLenum(savedMatrixMode));
    }
    void bindNative() {
        gl.bind(drawFbo, GLuint(savedDraw));
        gl.bind(readFbo, GLuint(savedDraw));
        glDrawBuffer(GLenum(savedDrawBuffer));
        glReadBuffer(GLenum(savedDrawBuffer));
    }
    bool admitted() {
        GLint sampleCount = 0, stencil = 0, profile = 0;
        glGetIntegerv(samples, &sampleCount);
        glGetIntegerv(GL_DEPTH_BITS, &depthBits);
        glGetIntegerv(GL_STENCIL_BITS, &stencil);
        glGetIntegerv(contextProfileMask, &profile);
        return sampleCount <= 1 && !stencil && !(profile & 1) && (depthBits == 16 || depthBits == 24 || depthBits == 32);
    }
    GLenum depthFormat() const { return depthBits == 16 ? depthComponent16 : depthBits == 24 ? depthComponent24 : depthComponent32; }
    bool save(int w, int h) {
        if (pushed || w < 1 || h < 1 || w > 8192 || h > 8192 || !admitted()) return false;
        drainErrors();
        push();
        width = w;
        height = h;
        copyWidth = copyHeight = 0;
        if (!backup) gl.gen(1, &backup);
        if (!eyeFbo) gl.gen(1, &eyeFbo);
        if (!texture) glGenTextures(1, &texture);
        if (!depthBuffer) gl.genBuffer(1, &depthBuffer);
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexImage2D(GL_TEXTURE_2D, 0, rgba32f, w, h, 0, GL_RGBA, GL_FLOAT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        gl.bind(framebuffer, backup);
        gl.attach(framebuffer, color, GL_TEXTURE_2D, texture, 0);
        gl.bindBuffer(renderbuffer, depthBuffer);
        gl.storage(renderbuffer, depthFormat(), w, h);
        gl.attachBuffer(framebuffer, depth, renderbuffer, depthBuffer);
        if (gl.status(framebuffer) != framebufferComplete) { pop(); return false; }
        gl.bind(readFbo, GLuint(savedDraw));
        glReadBuffer(GLenum(savedDrawBuffer));
        gl.bind(drawFbo, backup);
        glDrawBuffer(color);
        glDisable(GL_SCISSOR_TEST);
        gl.blit(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT, GL_NEAREST);
        if (glGetError() != GL_NO_ERROR) { pop(); return false; }
        backed = true;
        pop();
        push();
        bindNative();
        return true;
    }
    bool prepare() {
        if (!backed) return false;
        pop();
        push();
        bindNative();
        glDisable(GL_SCISSOR_TEST);
        return true;
    }
    bool restore() noexcept {
        const bool hadState = backed || pushed;
        drainErrors();
        if (backed) {
            gl.bind(readFbo, backup);
            glReadBuffer(color);
            gl.bind(drawFbo, GLuint(savedDraw));
            glDrawBuffer(GLenum(savedDrawBuffer));
            glDisable(GL_SCISSOR_TEST);
            gl.blit(0, 0, width, height, 0, 0, width, height, GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT, GL_NEAREST);
            backed = false;
        }
        pop();
        return !hadState || glGetError() == GL_NO_ERROR;
    }
};
// Copied colour and depth of the drawn centre view, owned by the host, read by reconstruction.
struct Capture {
    Gl& gl;
    GLuint fbo = 0, colorTexture = 0, depthTexture = 0;
    int width = 0, height = 0;
    explicit Capture(Gl& api) : gl(api) {}
    ~Capture() {
        if (fbo) gl.remove(1, &fbo);
        if (colorTexture) glDeleteTextures(1, &colorTexture);
        if (depthTexture) glDeleteTextures(1, &depthTexture);
    }
    bool copy(Target& target, GLenum depthFormat) {
        drainErrors();
        if (!fbo) gl.gen(1, &fbo);
        if (!colorTexture) glGenTextures(1, &colorTexture);
        if (!depthTexture) glGenTextures(1, &depthTexture);
        if (width != target.width || height != target.height) {
            width = target.width;
            height = target.height;
            glBindTexture(GL_TEXTURE_2D, colorTexture);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glBindTexture(GL_TEXTURE_2D, depthTexture);
            glTexImage2D(GL_TEXTURE_2D, 0, depthFormat, width, height, 0, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            gl.bind(framebuffer, fbo);
            gl.attach(framebuffer, color, GL_TEXTURE_2D, colorTexture, 0);
            gl.attach(framebuffer, depth, GL_TEXTURE_2D, depthTexture, 0);
        }
        gl.bind(readFbo, GLuint(target.savedDraw));
        glReadBuffer(GLenum(target.savedDrawBuffer));
        gl.bind(drawFbo, fbo);
        glDrawBuffer(color);
        glDisable(GL_SCISSOR_TEST);
        if (gl.status(drawFbo) != framebufferComplete) return false;
        gl.blit(0, 0, width, height, 0, 0, width, height, GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT, GL_NEAREST);
        return glGetError() == GL_NO_ERROR;
    }
    void read(CenterView& out) const {
        out.rgba.resize(size_t(width) * height * 4);
        out.depth.resize(size_t(width) * height);
        PixelStore packed; // the engine keeps its own pack alignment
        glBindTexture(GL_TEXTURE_2D, colorTexture);
        glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, out.rgba.data());
        glBindTexture(GL_TEXTURE_2D, depthTexture);
        glGetTexImage(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT, GL_FLOAT, out.depth.data());
    }
};
struct Options {
    RuntimePolicy policy = RuntimePolicy::emulator;
    RenderMode mode = RenderMode::geometryStereo;
    EyeLayout layout{};
    bool upperBody = false, fullBody = false, locomotion = false;
    int statusFrames = 180; // accepted frames the first-run status line stays up
};
bool flag(const wchar_t* name) {
    wchar_t value[4]{};
    return GetEnvironmentVariableW(name, value, 4) == 1 && value[0] == L'1';
}
float number(const wchar_t* name, float fallback) {
    wchar_t value[32]{};
    if (!GetEnvironmentVariableW(name, value, 32)) return fallback;
    const float parsed = float(_wtof(value));
    return std::isfinite(parsed) ? parsed : fallback;
}
// Emulation flags only; eye layout and render mode come from the mod's settings through configure().
Options options() {
    Options out{};
    wchar_t text[32]{};
    if (GetEnvironmentVariableW(L"EXCAMERA_XR_RUNTIME", text, 32) && std::wstring(text) == L"hardware") out.policy = RuntimePolicy::hardware;
    out.statusFrames = int(number(L"EXCAMERA_XR_STATUS_FRAMES", 180));
    out.upperBody = flag(L"EXCAMERA_XR_UPPER_BODY");
    out.fullBody = flag(L"EXCAMERA_XR_FULL_BODY");
    out.locomotion = flag(L"EXCAMERA_XR_LOCOMOTION");
    return out;
}
std::atomic<uint64_t> trackingGeneration{0};
std::string reconstructionReason = "geometry";
struct Host {
    HGLRC context = wglGetCurrentContext();
    DWORD thread = GetCurrentThreadId();
    Options config;
    Api api;
    Client client;
    Hands hands;
    Gl gl;
    Target target;
    Capture capture;
    std::unique_ptr<GpuReconstructor> reconstructor;
    FrameContract lastContract{}; // the frame the last reconstruction read; probes compare it with the oracle
    using LowerProvider = BOOL(WINAPI*)(XrTime, LowerSample*);
    LowerProvider lowerProvider = nullptr;
    TrackingFrame tracking{};
    bool focused = false, running = false, lostForGood = false;
    uint64_t trackingEpoch = 0, lastSerial = UINT64_MAX;
    HeadCamera camera;
    std::array<XrView, 2> views{{{XR_TYPE_VIEW}, {XR_TYPE_VIEW}}};
    std::array<XrCompositionLayerProjectionView, 2> layers{{{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}, {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}}};
    unsigned copied = 0;
    std::array<GLuint, 2> delivered{}; // the swapchain textures the last frame was written to
    // Head-locked overlays: the desktop frame while the game's UI is open (menus, the F8 panel),
    // otherwise a small reticle straight ahead. Both are quad layers the compositor places at a
    // fixed distance, so text stays readable and the eyes stay the head.
    EyeChain uiChain{}, reticleChain{}, statusChain{};
    bool uiOpenNow = false, reticleDrawn = false, handsDrawnNow = false, statusDrawn = false, uiInWorldNow = false;
    int statusFramesLeft = 0;
    double frameMs = 0, frameMsPrevious = 0; // smoothed time between frame begins, from the performance counter
    long long lastBeginCounter = 0;
    XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
    static constexpr float reticleDistance = 2.f, reticleWidth = .03f;
    ~Host() {
        client.destroyChain(uiChain);
        client.destroyChain(reticleChain);
        client.destroyChain(statusChain);
        client.destroyChain(wristChain);
        client.destroyChain(beamChain);
    }
    int desktopWidth = 0, desktopHeight = 0;
    void ensureOverlays() {
        RECT rect{};
        if (!GetClientRect(WindowFromDC(wglGetCurrentDC()), &rect) || rect.right <= 0 || rect.bottom <= 0) return;
        if (uiChain.chain && (rect.right != desktopWidth || rect.bottom != desktopHeight)) client.destroyChain(uiChain);
        if (!uiChain.chain) {
            desktopWidth = rect.right;
            desktopHeight = rect.bottom;
            const int longest = (std::max)(desktopWidth, desktopHeight);
            const float shrink = longest > 1024 ? 1024.f / float(longest) : 1.f;
            uiChain = client.createChain((std::max)(1, int(desktopWidth * shrink)), (std::max)(1, int(desktopHeight * shrink)));
        }
        if (!reticleChain.chain) reticleChain = client.createChain(64, 64);
        if (!statusChain.chain && (statusFramesLeft > 0 || config.layout.frameTimeReadout)) statusChain = client.createChain(512, 64);
        if (!wristChain.chain && wristShownNow) wristChain = client.createChain(wristWidth, wristHeight);
        if (!beamChain.chain && beamShownNow) beamChain = client.createChain(beamWidthPixels, beamLengthPixels);
    }
    // A ring with a centre dot, transparent elsewhere; uploaded once per session image.
    void drawReticle(GLuint texture) {
        std::vector<unsigned char> rgba(64 * 64 * 4, 0);
        for (int y = 0; y < 64; ++y)
            for (int x = 0; x < 64; ++x) {
                const float dx = x - 31.5f, dy = y - 31.5f, r = std::sqrt(dx * dx + dy * dy);
                const bool ring = r > 20 && r < 24, dot = r < 4;
                if (!ring && !dot) continue;
                unsigned char* p = rgba.data() + (size_t(y) * 64 + x) * 4;
                p[0] = p[1] = p[2] = 255;
                p[3] = 220;
            }
        PixelStore packed;
        GLint bound = 0;
        glGetIntegerv(textureBinding2D, &bound); // the engine's 2D binding on the active unit
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 64, 64, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
        glBindTexture(GL_TEXTURE_2D, GLuint(bound));
    }
    // One line of text, white on a dark translucent strip, rendered off-screen with GDI (no window)
    // and uploaded bottom-up like a framebuffer. Shown for the first accepted frames so a player
    // sees the session is live, which runtime took it, the render mode and whether hands draw.
    void drawStatus(GLuint texture) {
        std::string text = "ExCamera VR  |  " + client.runtimeName + "  |  " + (config.mode == RenderMode::centerReconstructionFilled ? "reconstruct, filled" : config.mode == RenderMode::centerReconstruction ? "reconstruct from depth" : "draw each eye") + "  |  hands " + (handsDrawnNow ? "on" : "off");
        if (config.layout.frameTimeReadout && frameMs > 0) {
            char timing[48];
            snprintf(timing, sizeof timing, "  |  %.1f ms (%.0f fps)", frameMs, 1000.0 / frameMs);
            text = (statusFramesLeft > 0 ? text : "ExCamera VR") + timing;
        }
        const int w = 512, h = 64;
        std::vector<unsigned char> rgba(size_t(w) * h * 4, 0);
        const HDC dc = CreateCompatibleDC(nullptr);
        BITMAPINFO info{};
        info.bmiHeader = {sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB};
        void* bits = nullptr;
        const HBITMAP bitmap = dc ? CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0) : nullptr;
        if (bitmap && bits) {
            const auto previous = SelectObject(dc, bitmap);
            memset(bits, 0, size_t(w) * h * 4);
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, RGB(255, 255, 255));
            SelectObject(dc, GetStockObject(DEFAULT_GUI_FONT));
            TextOutA(dc, 10, 24, text.c_str(), int(text.size()));
            GdiFlush();
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x) {
                    const unsigned char* s = static_cast<const unsigned char*>(bits) + (size_t(y) * w + x) * 4; // BGRX, top-down
                    unsigned char* d = rgba.data() + (size_t(h - 1 - y) * w + x) * 4;
                    const bool ink = s[0] | s[1] | s[2];
                    d[0] = ink ? s[2] : 16; d[1] = ink ? s[1] : 16; d[2] = ink ? s[0] : 24; d[3] = ink ? 255 : 180;
                }
            SelectObject(dc, previous);
        }
        if (bitmap) DeleteObject(bitmap);
        if (dc) DeleteDC(dc);
        PixelStore packed;
        GLint bound = 0;
        glGetIntegerv(textureBinding2D, &bound);
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
        glBindTexture(GL_TEXTURE_2D, GLuint(bound));
    }
    // The wrist menu: up to four lines, the highlighted one on a light strip; same GDI path as the status line.
    static constexpr int wristWidth = 256, wristHeight = 320, wristLines = int(wristPanelLines); // pixels at 16 per centimetre
    EyeChain wristChain;
    XrCompositionLayerQuad wristQuad{};
    bool wristShownNow = false;
    XrPosef wristPose{{0, 0, 0, 1}, {}};
    std::array<std::string, wristLines> wristText{};
    unsigned wristCount = 0;
    int wristHighlight = -1;
    void wrist(bool shown, const XrPosef& pose, const char* const* lines, unsigned count, int highlight) {
        wristShownNow = shown && valid(pose);
        wristPose = pose;
        wristCount = (std::min)(count, unsigned(wristLines));
        for (unsigned i = 0; i < wristLines; ++i) wristText[i] = i < wristCount && lines && lines[i] ? lines[i] : "";
        wristHighlight = highlight;
    }
    void drawWrist(GLuint texture) {
        const int w = wristWidth, h = wristHeight;
        std::vector<unsigned char> rgba(size_t(w) * h * 4, 0);
        const HDC dc = CreateCompatibleDC(nullptr);
        BITMAPINFO info{};
        info.bmiHeader = {sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB};
        void* bits = nullptr;
        const HBITMAP bitmap = dc ? CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0) : nullptr;
        if (bitmap && bits) {
            const auto previous = SelectObject(dc, bitmap);
            memset(bits, 0, size_t(w) * h * 4);
            SetBkMode(dc, TRANSPARENT);
            SelectObject(dc, GetStockObject(DEFAULT_GUI_FONT));
            const int rowHeight = h / wristLines;
            for (unsigned i = 0; i < wristCount; ++i) {
                const bool lit = int(i) == wristHighlight;
                if (lit) { // the strip itself is painted in the copy below; mark it with a pixel the loop reads
                    RECT strip{0, int(i) * rowHeight, w, int(i + 1) * rowHeight};
                    const HBRUSH brush = CreateSolidBrush(RGB(1, 1, 1));
                    FillRect(dc, &strip, brush);
                    DeleteObject(brush);
                }
                SetTextColor(dc, lit ? RGB(255, 255, 200) : RGB(255, 255, 255));
                TextOutA(dc, 12, int(i) * rowHeight + rowHeight / 2 - 8, wristText[i].c_str(), int(wristText[i].size()));
            }
            GdiFlush();
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x) {
                    const unsigned char* s = static_cast<const unsigned char*>(bits) + (size_t(y) * w + x) * 4; // BGRX, top-down
                    unsigned char* d = rgba.data() + (size_t(h - 1 - y) * w + x) * 4;
                    const bool strip = s[0] == 1 && s[1] == 1 && s[2] == 1, ink = !strip && (s[0] | s[1] | s[2]);
                    d[0] = ink ? s[2] : strip ? 70 : 16; d[1] = ink ? s[1] : strip ? 70 : 16; d[2] = ink ? s[0] : strip ? 90 : 24; d[3] = ink ? 255 : 200;
                }
            SelectObject(dc, previous);
        }
        if (bitmap) DeleteObject(bitmap);
        if (dc) DeleteDC(dc);
        PixelStore packed;
        GLint bound = 0;
        glGetIntegerv(textureBinding2D, &bound);
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
        glBindTexture(GL_TEXTURE_2D, GLuint(bound));
    }
    // The beam: a strip the width of pointerBeamWidth from the controller to the hit, turned to face the head
    // so it never shows edge-on; white, fading toward the hand.
    static constexpr int beamWidthPixels = 8, beamLengthPixels = 256;
    EyeChain beamChain;
    XrCompositionLayerQuad beamQuad{};
    bool beamShownNow = false, beamDrawn = false;
    XrVector3f beamFrom{}, beamTo{};
    float beamU = 0, beamV = 0;
    void beam(bool shown, const XrVector3f& from, const XrVector3f& to, float u, float v) {
        beamShownNow = shown && finite(from) && finite(to);
        beamFrom = from;
        beamTo = to;
        beamU = u;
        beamV = v;
    }
    void drawBeam(GLuint texture) {
        std::vector<unsigned char> rgba(size_t(beamWidthPixels) * beamLengthPixels * 4, 0);
        for (int y = 0; y < beamLengthPixels; ++y)
            for (int x = 0; x < beamWidthPixels; ++x) {
                unsigned char* p = rgba.data() + (size_t(y) * beamWidthPixels + x) * 4;
                const bool edge = x == 0 || x == beamWidthPixels - 1;
                p[0] = p[1] = 255; p[2] = 200;
                p[3] = static_cast<unsigned char>((edge ? 60 : 200) * y / beamLengthPixels); // y 0 at the hand, full at the hit
            }
        PixelStore packed;
        GLint bound = 0;
        glGetIntegerv(textureBinding2D, &bound);
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, beamWidthPixels, beamLengthPixels, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
        glBindTexture(GL_TEXTURE_2D, GLuint(bound));
    }
    const XrCompositionLayerBaseHeader* beamLayer() {
        if (!beamShownNow) return nullptr;
        ensureOverlays();
        if (!beamChain.chain) return nullptr;
        const XrVector3f span = sub(beamTo, beamFrom);
        const float length = fp::xr::length(span);
        if (!(length > .02f)) return nullptr;
        const XrVector3f dir = scale(span, 1 / length);
        XrQuaternionf q = directionRotation({0, 1, 0}, dir); // the strip's up runs along the beam
        const XrVector3f toHead = sub(lastHead.position, beamFrom);
        const XrVector3f facing = sub(toHead, scale(dir, dot(toHead, dir))); // the part of the head direction across the beam
        if (fp::xr::length(facing) > 1e-3f) {
            const XrVector3f want = scale(facing, 1 / fp::xr::length(facing)), have = rotate(q, {0, 0, 1});
            const float turn = std::atan2(dot(cross(have, want), dir), dot(have, want));
            q = multiply(axisAngle(dir, turn), q);
        }
        ImageLease lease(api, beamChain.chain);
        require(lease.index < beamChain.images.size(), "invalid beam image index");
        if (!beamDrawn) { drawBeam(beamChain.images[lease.index].image); beamDrawn = beamChain.images.size() == 1; }
        glFlush();
        lease.release();
        beamQuad = {XR_TYPE_COMPOSITION_LAYER_QUAD};
        beamQuad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
        beamQuad.space = client.local;
        beamQuad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        beamQuad.subImage.swapchain = beamChain.chain;
        beamQuad.subImage.imageRect.extent = {beamWidthPixels, beamLengthPixels};
        beamQuad.pose = {q, add(beamFrom, scale(span, .5f))};
        beamQuad.size = {pointerBeamWidth, length};
        return reinterpret_cast<const XrCompositionLayerBaseHeader*>(&beamQuad);
    }
    const XrCompositionLayerBaseHeader* wristLayer() {
        if (!wristShownNow) return nullptr;
        ensureOverlays();
        if (!wristChain.chain) return nullptr;
        ImageLease lease(api, wristChain.chain);
        require(lease.index < wristChain.images.size(), "invalid wrist image index");
        drawWrist(wristChain.images[lease.index].image);
        glFlush();
        lease.release();
        wristQuad = {XR_TYPE_COMPOSITION_LAYER_QUAD};
        wristQuad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
        wristQuad.space = client.local;
        wristQuad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        wristQuad.subImage.swapchain = wristChain.chain;
        wristQuad.subImage.imageRect.extent = {wristWidth, wristHeight};
        wristQuad.pose = wristPose;
        wristQuad.size = {wristPanelWidth, wristPanelHeight};
        return reinterpret_cast<const XrCompositionLayerBaseHeader*>(&wristQuad);
    }
    void recenter() {
        camera.invalidate(); // begin() takes the next head pose as the game head again
        ++trackingEpoch;
    }
    // A black ring from 60 % of the half-height out past the edge, alpha rising to `strength`, over the bound
    // target: the comfort vignette. Fixed-function so it works on the game's own context; every bit of state
    // it touches is pushed and popped, the framebuffer binding by hand because glPushAttrib does not cover it.
    float comfortStrength = 0;
    void drawVignette(GLuint onto, GLuint texture, int width, int height, float strength) {
        if (!(strength > 0) || width < 1 || height < 1) return;
        GLint savedDraw = 0, savedProgram = 0, savedVao = 0;
        glGetIntegerv(drawFramebufferBinding, &savedDraw);
        glGetIntegerv(currentProgram, &savedProgram);
        glGetIntegerv(vertexArrayBinding, &savedVao);
        glPushAttrib(GL_ALL_ATTRIB_BITS);
        glMatrixMode(GL_PROJECTION); glPushMatrix(); glLoadIdentity();
        glMatrixMode(GL_MODELVIEW); glPushMatrix(); glLoadIdentity();
        gl.program(0);
        gl.vao(0);
        gl.bind(drawFbo, onto);
        if (texture) { gl.attach(drawFbo, color, GL_TEXTURE_2D, texture, 0); glDrawBuffer(color); }
        glViewport(0, 0, width, height);
        glDisable(GL_DEPTH_TEST); glDisable(GL_SCISSOR_TEST); glDisable(GL_CULL_FACE); glDisable(GL_TEXTURE_2D); glDisable(GL_LIGHTING);
        glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glColorMask(1, 1, 1, 1);
        const float aspect = float(width) / float(height), inner = .6f, outer = 2.2f;
        glBegin(GL_TRIANGLE_STRIP);
        for (int i = 0; i <= 48; ++i) {
            const float a = float(i) / 48.f * 6.2831853f, c = std::cos(a), s = std::sin(a);
            glColor4f(0, 0, 0, 0); glVertex2f(inner * c / aspect, inner * s);
            glColor4f(0, 0, 0, (std::min)(strength, 1.f)); glVertex2f(outer * c / aspect, outer * s);
        }
        glEnd();
        if (strength > 1) { // a blink: the whole view dimmed by the excess over the ring
            glColor4f(0, 0, 0, (std::min)(strength - 1.f, 1.f));
            glBegin(GL_TRIANGLE_STRIP);
            glVertex2f(-1, -1); glVertex2f(1, -1); glVertex2f(-1, 1); glVertex2f(1, 1);
            glEnd();
        }
        glMatrixMode(GL_MODELVIEW); glPopMatrix();
        glMatrixMode(GL_PROJECTION); glPopMatrix();
        glPopAttrib();
        gl.bind(drawFbo, GLuint(savedDraw));
        gl.program(GLuint(savedProgram));
        gl.vao(GLuint(savedVao));
    }
    bool comfort(float strength) {
        comfortStrength = std::isfinite(strength) ? (std::max)(0.f, (std::min)(strength, 2.f)) : 0.f; // above 1: a blink (drawVignette)
        if (comfortStrength > 0 && !reconstructs(config.mode) && target.width > 0) {
            GLint current = 0;
            glGetIntegerv(drawFramebufferBinding, &current);
            drawVignette(GLuint(current), 0, target.width, target.height, comfortStrength);
            return glGetError() == GL_NO_ERROR;
        }
        return true;
    }
    // Returns the quad layer for this frame, or nullptr when no overlay could be produced.
    const XrCompositionLayerBaseHeader* overlayLayer() {
        ensureOverlays();
        const bool status = !uiOpenNow && (statusFramesLeft > 0 || config.layout.frameTimeReadout) && statusChain.chain;
        EyeChain& chain = uiOpenNow ? uiChain : status ? statusChain : reticleChain;
        if (!chain.chain) return nullptr;
        ImageLease lease(api, chain.chain);
        require(lease.index < chain.images.size(), "invalid overlay image index");
        const GLuint image = chain.images[lease.index].image;
        if (uiOpenNow) {
            target.push(); // the bound draw framebuffer is the desktop frame about to be presented
            gl.bind(readFbo, GLuint(target.savedDraw));
            glReadBuffer(GLenum(target.savedDrawBuffer));
            gl.bind(drawFbo, target.eyeFbo);
            gl.attach(drawFbo, color, GL_TEXTURE_2D, image, 0);
            glDrawBuffer(color);
            glDisable(GL_SCISSOR_TEST);
            const bool complete = gl.status(drawFbo) == framebufferComplete;
            if (complete) gl.blit(0, 0, desktopWidth, desktopHeight, 0, 0, chain.width, chain.height, GL_COLOR_BUFFER_BIT, GL_LINEAR);
            if (complete && beamShownNow) { // the dot where the beam lands, a filled square in the panel image
                const int dot = pointerDotPixels, x = int(beamU * float(chain.width)) - dot / 2, y = int((1 - beamV) * float(chain.height)) - dot / 2;
                glEnable(GL_SCISSOR_TEST);
                glScissor(x, y, dot, dot);
                glClearColor(1, 1, .6f, 1);
                glClear(GL_COLOR_BUFFER_BIT);
                glDisable(GL_SCISSOR_TEST);
            }
            target.pop();
            if (!complete || glGetError() != GL_NO_ERROR) return nullptr;
        } else if (status) {
            drawStatus(image);
        } else {
            drawReticle(image); // small; redrawing beats tracking which of the images was drawn
        }
        glFlush();
        lease.release();
        quad = {XR_TYPE_COMPOSITION_LAYER_QUAD};
        quad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
        quad.space = client.view;
        quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        quad.subImage.swapchain = chain.chain;
        quad.subImage.imageRect.extent = {chain.width, chain.height};
        const float uiDistance = config.layout.panelDistanceMetres, uiWidth = config.layout.panelWidthMetres;
        quad.pose = {{0, 0, 0, 1}, {0, 0, uiOpenNow || status ? -uiDistance : -reticleDistance}};
        bool inHand = false;
        if (uiOpenNow && uiInWorldNow && camera.calibrated()) { // a sign in the level, where the player looked when recentred
            quad.space = client.local;
            quad.pose = compose(camera.origin(), {{0, 0, 0, 1}, {0, 0, -uiDistance}});
        } else if (uiOpenNow && config.layout.handPanel && config.upperBody) { // held in the left hand, facing the head
            XrPosef held{};
            if (handPanelPose(tracking.sample, held)) { quad.space = client.local; quad.pose = held; inHand = true; }
        }
        const float width = inHand ? handPanelWidth : uiOpenNow ? uiWidth : status ? .8f : reticleWidth;
        quad.size = {width, width * float(chain.height) / float(chain.width)};
        return reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad);
    }
    XrTime change = 0;
    // Optional evidence for an automated in-game run: the delivered eye images and the poses they
    // came from, for the first accepted frames only. Nothing is written without the opt-in.
    fs::path evidenceDirectory;
    unsigned evidenceRemaining = 0;
    XrPosef lastHead{{0, 0, 0, 1}, {}};
    static std::string poseText(XrPosef p) {
        const float v[7] = {p.position.x, p.position.y, p.position.z, p.orientation.x, p.orientation.y, p.orientation.z, p.orientation.w};
        std::string out = "[";
        for (unsigned i = 0; i < 7; ++i) out += (i ? "," : "") + std::to_string(v[i]);
        return out + "]";
    }
    void record() {
        if (!evidenceRemaining || evidenceDirectory.empty()) return;
        --evidenceRemaining;
        std::error_code ignored;
        fs::create_directories(evidenceDirectory, ignored);
        {
            PixelStore packed;
            glPushAttrib(GL_TEXTURE_BIT);
            for (unsigned i = 0; i < 2; ++i) {
                const auto& eye = client.eyes[i];
                std::vector<unsigned char> rgba(size_t(eye.width) * eye.height * 4);
                glBindTexture(GL_TEXTURE_2D, delivered[i]);
                glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
                std::ofstream out(evidenceDirectory / ("frame-" + std::to_string(lastSerial) + (i ? "-right.ppm" : "-left.ppm")), std::ios::binary);
                out << "P6\n" << eye.width << ' ' << eye.height << "\n255\n";
                for (int y = eye.height - 1; y >= 0; --y)
                    for (int x = 0; x < eye.width; ++x) out.write(reinterpret_cast<const char*>(rgba.data() + (size_t(y) * eye.width + x) * 4), 3);
            }
            glPopAttrib();
        }
        std::ofstream log(evidenceDirectory / "frames.jsonl", std::ios::app);
        log << "{\"serial\":" << lastSerial << ",\"display_time\":" << client.displayTime
            << ",\"mode\":\"" << (config.mode == RenderMode::centerReconstructionFilled ? "reconstruction_filled" : config.mode == RenderMode::centerReconstruction ? "reconstruction" : "geometry")
            << "\",\"ipd_override_m\":" << config.layout.ipdOverrideMetres << ",\"units_per_metre\":" << config.layout.unitsPerMetre
            << ",\"head\":" << poseText(lastHead) << ",\"left\":" << poseText(layers[0].pose) << ",\"right\":" << poseText(layers[1].pose) << "}\n";
    }
    Host(const fs::path& directory, Options opts) : config(opts), api(directory), client(api), hands(api), target(gl), capture(gl) {
        require(config.layout.plausible(), "implausible IPD or world scale");
        wchar_t evidence[32768]{};
        if (GetEnvironmentVariableW(L"EXCAMERA_XR_EVIDENCE_DIR", evidence, 32768)) {
            evidenceDirectory = evidence;
            wchar_t frames[16]{};
            evidenceRemaining = GetEnvironmentVariableW(L"EXCAMERA_XR_EVIDENCE_FRAMES", frames, 16) ? unsigned(_wtoi(frames)) : 8u;
        }
        trackingEpoch = (trackingGeneration.fetch_add(1) + 1) << 32;
    statusFramesLeft = (std::max)(0, config.statusFrames);
        if (config.policy == RuntimePolicy::emulator) {
            wchar_t manifest[32768]{};
            require(GetEnvironmentVariableW(L"XR_RUNTIME_JSON", manifest, 32768) > 0 && fs::equivalent(manifest, directory / L"mock_runtime.json"), "select the local XR emulator in the child environment");
        }
        client.initialize(wglGetCurrentDC(), context, config.policy);
        if (config.upperBody) hands.initialize(client);
        if (config.policy == RuntimePolicy::emulator) {
            using Configure = BOOL(WINAPI*)(unsigned);
            const auto runtime = GetModuleHandleW((directory / L"excamera_mock_runtime.dll").c_str());
            require(runtime != nullptr, "emulator runtime module missing");
            if (config.fullBody) lowerProvider = reinterpret_cast<LowerProvider>(GetProcAddress(runtime, "excameraMockLocateLowerBody"));
            if (config.locomotion) {
                const auto motion = reinterpret_cast<Configure>(GetProcAddress(runtime, "excameraMockSetLocomotion"));
                require(motion && motion(1), "emulator locomotion scenario configuration failed");
            }
            const auto configure = reinterpret_cast<Configure>(GetProcAddress(runtime, "excameraMockSetContinuous"));
            require(configure && configure(1), "matching continuous emulation runtime required");
        }
    }
    // The CPU reference warp is evidence-grade only; a hardware session without an external host
    // keeps geometry stereo rather than pretending the approximation is the product.
    void configure(EyeLayout eyes, RenderMode mode) {
        hands.gripAngleDegrees = eyes.gripAngleDegrees;
        hands.relaxedGrab = eyes.relaxedGrab;
        if (eyes.plausible()) config.layout = eyes;
        if (mode == RenderMode::geometryStereo) { config.mode = mode; reconstructionReason = "geometry"; return; }
        std::string reason;
        try {
            if (!reconstructor) reconstructor = std::make_unique<GpuReconstructor>();
        } catch (const std::exception& e) { reason = e.what(); }
        if (reconstructor && reconstructor->prepare(reason)) { config.mode = mode; reconstructor->fill = mode == RenderMode::centerReconstructionFilled; reconstructionReason = reconstructor->name(); }
        else { config.mode = RenderMode::geometryStereo; reconstructionReason = "unavailable: " + reason; }
    }
    bool owner() const { return context == wglGetCurrentContext() && thread == GetCurrentThreadId(); }
    void finish(bool accepted) {
        require(target.restore(), "native colour/depth restoration failed");
        if (!client.begun) return;
        XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
        layer.space = client.local;
        layer.viewCount = 2;
        layer.views = layers.data();
        std::array<const XrCompositionLayerBaseHeader*, 4> headers{reinterpret_cast<const XrCompositionLayerBaseHeader*>(&layer), nullptr, nullptr, nullptr};
        XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};
        end.displayTime = client.displayTime;
        end.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        uint32_t count = accepted && copied == 3 ? 1u : 0u;
        if (count) {
            if (const auto* over = overlayLayer()) headers[count++] = over;
            if (const auto* wristOn = wristLayer()) headers[count++] = wristOn;
            if (const auto* beamOn = beamLayer()) headers[count++] = beamOn;
        }
        end.layerCount = count;
        end.layers = end.layerCount ? headers.data() : nullptr;
        check(api.xrEndFrame(client.session, &end), "xrEndFrame");
        if (end.layerCount && statusFramesLeft > 0) --statusFramesLeft;
        if (end.layerCount) record();
        client.begun = false;
        copied = 0;
    }
    void pump() {
        XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
        XrResult result = XR_SUCCESS;
        while ((result = api.xrPollEvent(client.instance, &event)) == XR_SUCCESS) {
            if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
                const auto state = reinterpret_cast<XrEventDataSessionStateChanged*>(&event)->state;
                focused = state == XR_SESSION_STATE_FOCUSED;
                if (state == XR_SESSION_STATE_READY) {
                    XrSessionBeginInfo info{XR_TYPE_SESSION_BEGIN_INFO};
                    info.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                    check(api.xrBeginSession(client.session, &info), "xrBeginSession");
                    running = true;
                } else if (state == XR_SESSION_STATE_STOPPING) {
                    check(api.xrEndSession(client.session), "xrEndSession");
                    running = false;
                    camera.invalidate();
                } else if (state == XR_SESSION_STATE_EXITING || state == XR_SESSION_STATE_LOSS_PENDING) { running = false; lostForGood = true; }
            } else if (event.type == XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING) {
                const auto& pending = *reinterpret_cast<XrEventDataReferenceSpaceChangePending*>(&event);
                if (pending.referenceSpaceType == XR_REFERENCE_SPACE_TYPE_LOCAL) change = pending.changeTime;
            } else if (event.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) { running = false; lostForGood = true; }
            event = {XR_TYPE_EVENT_DATA_BUFFER};
        }
        require(result == XR_EVENT_UNAVAILABLE, "XR event pump failed");
    }
    bool beginFrame(XrFrameState& state) {
        pump();
        if (!running) return false;
        XrFrameWaitInfo wait{XR_TYPE_FRAME_WAIT_INFO};
        check(api.xrWaitFrame(client.session, &wait, &state), "xrWaitFrame");
        client.displayTime = state.predictedDisplayTime;
        XrFrameBeginInfo frameBegin{XR_TYPE_FRAME_BEGIN_INFO};
        check(api.xrBeginFrame(client.session, &frameBegin), "xrBeginFrame");
        client.begun = true;
        LARGE_INTEGER counter{}, frequency{};
        if (QueryPerformanceCounter(&counter) && QueryPerformanceFrequency(&frequency) && frequency.QuadPart > 0) {
            if (lastBeginCounter) {
                const double ms = double(counter.QuadPart - lastBeginCounter) * 1000.0 / double(frequency.QuadPart);
                frameMs = frameMs > 0 ? frameMs * .9 + ms * .1 : ms; // a slow average reads steadily on a quad
            }
            lastBeginCounter = counter.QuadPart;
        }
        return true;
    }
    // While the game's UI is open the world is not replayed, so the headset would freeze on its
    // last frame; a quad-only frame keeps it live and shows the menu readably in front of the head.
    void overlay(bool uiOpen, bool handsDrawn, bool uiInWorld) {
        uiOpenNow = uiOpen;
        handsDrawnNow = handsDrawn;
        uiInWorldNow = uiInWorld;
        if (!uiOpen || client.begun || !owner()) return;
        XrFrameState state{XR_TYPE_FRAME_STATE};
        if (!beginFrame(state)) return;
        if (config.upperBody) { // the menu button and the pointer need fresh hands while the world is not drawn
            XrSpaceLocation head{XR_TYPE_SPACE_LOCATION};
            check(api.xrLocateSpace(client.view, client.local, client.displayTime, &head), "xrLocateSpace");
            const auto h = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT;
            if ((head.locationFlags & h) == h && valid(head.pose) && camera.calibrated()) {
                tracking.sample = hands.sample(client.local, client.displayTime, head.pose, focused, trackingEpoch);
                tracking.rig = solveUpper(tracking.sample);
                tracking.calibration = camera;
                tracking.tick = GetTickCount64();
                tracking.enabled = true;
            }
        }
        std::array<const XrCompositionLayerBaseHeader*, 3> headers{overlayLayer(), nullptr, nullptr};
        uint32_t count = headers[0] ? 1u : 0u;
        if (count) {
            if (const auto* wristOn = wristLayer()) headers[count++] = wristOn;
            if (const auto* beamOn = beamLayer()) headers[count++] = beamOn;
        }
        XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};
        end.displayTime = client.displayTime;
        end.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        end.layerCount = count;
        end.layers = count ? headers.data() : nullptr;
        client.begun = false;
        check(api.xrEndFrame(client.session, &end), "xrEndFrame");
    }
    bool haptic(unsigned side, float amplitude, uint64_t durationNs) {
        return config.upperBody && running && hands.pulse(side, (std::min)(amplitude * config.layout.hapticStrength, 1.f), XrDuration(durationNs));
    }
    bool stageInLocal(XrPosef& out) {
        if (!client.stage || !client.begun) return false;
        XrSpaceLocation at{XR_TYPE_SPACE_LOCATION};
        check(api.xrLocateSpace(client.stage, client.local, client.displayTime, &at), "xrLocateSpace");
        const auto f = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT;
        if ((at.locationFlags & f) != f || !valid(at.pose)) return false;
        out = at.pose;
        return true;
    }
    bool originInStage(XrPosef& out) {
        XrPosef stage{};
        if (!camera.calibrated() || !stageInLocal(stage)) return false;
        out = compose(inverse(stage), camera.origin());
        return valid(out);
    }
    bool restoreOrigin(const XrPosef& stageOrigin) {
        XrPosef stage{};
        if (!valid(stageOrigin) || !stageInLocal(stage)) return false;
        if (!camera.place(compose(stage, stageOrigin))) return false;
        ++trackingEpoch; // every hold and calibration derived from the old origin is stale
        return true;
    }
    bool begin(uint64_t serial, const NativePose& anchor, float clipNear, float clipFar, StereoFrame& out) {
        if (!owner() || serial == lastSerial) return false;
        finish(false);
        lastSerial = serial;
        tracking = {};
        XrFrameState state{XR_TYPE_FRAME_STATE};
        if (!beginFrame(state)) return false;
        if (change && client.displayTime >= change) { camera.invalidate(); change = 0; ++trackingEpoch; }
        XrSpaceLocation head{XR_TYPE_SPACE_LOCATION};
        check(api.xrLocateSpace(client.view, client.local, client.displayTime, &head), "xrLocateSpace");
        XrViewLocateInfo locate{XR_TYPE_VIEW_LOCATE_INFO};
        locate.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        locate.displayTime = client.displayTime;
        locate.space = client.local;
        XrViewState viewState{XR_TYPE_VIEW_STATE};
        uint32_t count = 0;
        check(api.xrLocateViews(client.session, &locate, &viewState, 2, &count, views.data()), "xrLocateViews");
        const auto h = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT;
        const auto v = XR_VIEW_STATE_ORIENTATION_VALID_BIT | XR_VIEW_STATE_POSITION_VALID_BIT;
        if (!state.shouldRender || count != 2 || (head.locationFlags & h) != h || (viewState.viewStateFlags & v) != v ||
            !valid(head.pose) || !valid(views[0].pose) || !valid(views[1].pose)) {
            finish(false);
            return false;
        }
        if (!camera.calibrated()) require(camera.recenter(head.pose), "XR calibration failed");
        lastHead = head.pose;
        if (config.upperBody) {
            tracking.sample = hands.sample(client.local, client.displayTime, head.pose, focused, trackingEpoch);
            if (lowerProvider && focused && (!lowerProvider(client.displayTime, &tracking.lower) || !lowerValid(tracking.lower, client.displayTime))) tracking.lower = {};
            else if (!lowerProvider && hands.bodyTrackers()) { // real waist and foot trackers (XR_HTCX_vive_tracker_interaction)
                if (!focused || !hands.sampleLower(client.local, client.displayTime, tracking.lower) || !lowerValid(tracking.lower, client.displayTime)) tracking.lower = {};
            }
            tracking.rig = solveUpper(tracking.sample);
            tracking.anchor = anchor;
            tracking.calibration = camera;
            tracking.serial = serial;
            tracking.tick = GetTickCount64();
            tracking.enabled = true;
        }
        out.serial = serial;
        out.nearPlane = clipNear;
        out.farPlane = clipFar > 0 ? clipFar : 100000.f; // 1 km culling corners for the infinite far
        const auto eyes = eyePoses(head.pose, {views[0].pose, views[1].pose}, config.layout);
        XrFovf centre{};
        for (unsigned i = 0; i < 2; ++i) {
            out.eyes[i].pose = nativeEye(anchor, camera.relative(eyes[i]), config.layout.unitsPerMetre);
            out.eyes[i].projection = projection(views[i].fov, clipNear, clipFar);
            layers[i].pose = eyes[i];
            layers[i].fov = views[i].fov;
            layers[i].subImage.swapchain = client.eyes[i].chain;
            layers[i].subImage.imageRect.extent = {client.eyes[i].width, client.eyes[i].height};
            centre.angleLeft = (std::min)(centre.angleLeft, views[i].fov.angleLeft);
            centre.angleRight = (std::max)(centre.angleRight, views[i].fov.angleRight);
            centre.angleUp = (std::max)(centre.angleUp, views[i].fov.angleUp);
            centre.angleDown = (std::min)(centre.angleDown, views[i].fov.angleDown);
        }
        out.center.pose = nativeEye(anchor, camera.relative(head.pose), config.layout.unitsPerMetre);
        out.center.projection = projection(centre, clipNear, clipFar);
        return true;
    }
    // Copies the drawn eye from the native target into a leased swapchain image.
    bool deliver(unsigned i) {
        auto& eye = client.eyes[i];
        drainErrors();
        ImageLease lease(api, eye.chain);
        require(lease.index < eye.images.size(), "invalid XR eye image index");
        gl.bind(readFbo, GLuint(target.savedDraw));
        glReadBuffer(GLenum(target.savedDrawBuffer));
        gl.bind(drawFbo, target.eyeFbo);
        gl.attach(drawFbo, color, GL_TEXTURE_2D, eye.images[lease.index].image, 0);
        glDrawBuffer(color);
        glDisable(GL_SCISSOR_TEST);
        require(gl.status(drawFbo) == framebufferComplete, "incomplete XR copy target");
        gl.blit(0, 0, target.copyWidth ? target.copyWidth : target.width, target.copyHeight ? target.copyHeight : target.height, 0, 0, eye.width, eye.height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        glFlush();
        require(glGetError() == GL_NO_ERROR, "XR eye delivery failed");
        lease.release();
        delivered[i] = eye.images[lease.index].image;
        copied |= 1u << i;
        return true;
    }
    bool copy(unsigned i) {
        if (!owner() || !client.begun || i > 1 || !target.backed) return false;
        deliver(i);
        target.bindNative();
        return true;
    }
    bool reconstruct(const FrameContract& given, const StereoFrame& frame) {
        if (!owner() || !client.begun || !reconstructor || !target.backed) return false;
        FrameContract contract = given;
        if (!capture.copy(target, target.depthFormat())) { target.bindNative(); return false; }
        contract.colorTexture = capture.colorTexture;
        contract.depthTexture = capture.depthTexture;
        contract.predictedTime = client.displayTime;
        if (const char* why = contract.validate()) throw std::runtime_error(std::string("frame contract: ") + why);
        lastContract = contract;
        // Both swapchain images are leased while the warp draws straight into them.
        ImageLease left(api, client.eyes[0].chain), right(api, client.eyes[1].chain);
        require(left.index < client.eyes[0].images.size() && right.index < client.eyes[1].images.size(), "invalid XR eye image index");
        const std::array<GpuReconstructor::Target, 2> targets{{
            {target.eyeFbo, client.eyes[0].images[left.index].image, client.eyes[0].width, client.eyes[0].height},
            {target.eyeFbo, client.eyes[1].images[right.index].image, client.eyes[1].width, client.eyes[1].height}}};
        std::string reason;
        const bool drawn = reconstructor->render(contract, frame.eyes, targets, reason);
        if (drawn && comfortStrength > 0)
            for (const auto& t : targets) drawVignette(t.framebuffer, t.texture, t.width, t.height, comfortStrength);
        glFlush();
        target.bindNative();
        if (!drawn) throw std::runtime_error("reconstruction: " + reason);
        left.release();
        right.release();
        delivered = {targets[0].texture, targets[1].texture};
        copied = 3;
        return true;
    }
};
std::unique_ptr<Host> host;
// Settings seen before the host exists are applied when it is created.
EyeLayout pendingLayout{};
RenderMode pendingMode = RenderMode::geometryStereo;
bool begin(uint64_t serial, const NativePose& anchor, float clipNear, float clipFar, StereoFrame& frame) {
    if (!host) {
        wchar_t directory[32768]{};
        if (!GetEnvironmentVariableW(L"EXCAMERA_XR_EMULATION_DIR", directory, 32768) || !wglGetCurrentContext()) return false;
        host = std::make_unique<Host>(directory, options());
        host->configure(pendingLayout, pendingMode);
    }
    return host->begin(serial, anchor, clipNear, clipFar, frame);
}
RenderMode mode() { return host ? host->config.mode : RenderMode::geometryStereo; }
void configure(EyeLayout eyes, RenderMode mode) {
    pendingLayout = eyes;
    pendingMode = mode;
    if (host && host->owner()) host->configure(eyes, mode);
}
bool save(int w, int h) { return host && host->owner() && host->target.save(w, h); }
void eyeRect(int w, int h) { if (host && host->owner()) { host->target.copyWidth = w; host->target.copyHeight = h; } }
bool prepare(unsigned) { return host && host->owner() && host->target.prepare(); }
bool copy(unsigned i) { return host && host->copy(i); }
bool reconstruct(const FrameContract& contract, const StereoFrame& frame) { return host && host->reconstruct(contract, frame); }
void restore() {
    require(host && host->owner(), "XR target context changed during replay");
    require(host->target.restore(), "native colour/depth restoration failed");
}
void overlay(bool uiOpen, bool handsDrawn, bool uiInWorld) { if (host && host->owner()) host->overlay(uiOpen, handsDrawn, uiInWorld); }
void recenter() { if (host && host->owner()) host->recenter(); }
bool haptic(unsigned side, float amplitude, uint64_t durationNs) { return host && host->owner() && host->haptic(side, amplitude, durationNs); }
bool comfort(float strength) { return host && host->owner() && host->comfort(strength); }
void finish(bool accepted) { if (host && host->owner()) host->finish(accepted); }
bool retire() {
    if (!host) return true;
    if (!host->owner()) return false;
    host->finish(false);
    host.reset();
    return true;
}
bool lost() { return host && host->lostForGood; }
bool originInStage(XrPosef& out) { return host && host->owner() && host->originInStage(out); }
bool restoreOrigin(const XrPosef& stageOrigin) { return host && host->owner() && host->restoreOrigin(stageOrigin); }
void wrist(bool shown, const XrPosef& pose, const char* const* lines, unsigned count, int highlight) { if (host && host->owner()) host->wrist(shown, pose, lines, count, highlight); }
void beam(bool shown, const XrVector3f& from, const XrVector3f& to, float u, float v) { if (host && host->owner()) host->beam(shown, from, to, u, v); }
}
bool trackingFrame(TrackingFrame& out) {
    if (!host || !host->owner() || !host->config.upperBody) return false;
    out = host->tracking;
    if (!out.enabled || GetTickCount64() - out.tick > 100) { out.sample.focused = false; out.rig = {}; out.enabled = true; }
    return true;
}
const char* reconstructionStatus() { return reconstructionReason.c_str(); }
Backend emulatedBackend() { return {configure, begin, mode, save, prepare, copy, reconstruct, restore, overlay, recenter, comfort, haptic, finish, retire, lost, originInStage, restoreOrigin, wrist, beam, eyeRect}; }
}
