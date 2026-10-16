// Drawn over the frame: the attack wind-up arc (OpenGL); the game windows that free the cursor; crouch.
// Part of first_person.cpp (one translation unit): included there, in order.
// Attack-origin cue drawn into the finished frame (default framebuffer) just
// before present, beside where the native cursor was drawn this frame. Owns a
// private program/VAO/VBO per GL context and restores every state it touches.
// Any missing entry point, non-default framebuffer or exception: no cue.
namespace gl {
typedef char GLchar;
typedef ptrdiff_t GLsizeiptr;
using CreateShader = GLuint(APIENTRY*)(GLenum);
using ShaderSource = void(APIENTRY*)(GLuint, GLsizei, const GLchar* const*, const GLint*);
using CompileShader = void(APIENTRY*)(GLuint);
using GetShaderiv = void(APIENTRY*)(GLuint, GLenum, GLint*);
using CreateProgram = GLuint(APIENTRY*)();
using AttachShader = void(APIENTRY*)(GLuint, GLuint);
using BindAttribLocation = void(APIENTRY*)(GLuint, GLuint, const GLchar*);
using LinkProgram = void(APIENTRY*)(GLuint);
using GetProgramiv = void(APIENTRY*)(GLuint, GLenum, GLint*);
using UseProgram = void(APIENTRY*)(GLuint);
using GetUniformLocation = GLint(APIENTRY*)(GLuint, const GLchar*);
using Uniform4f = void(APIENTRY*)(GLint, GLfloat, GLfloat, GLfloat, GLfloat);
using GenVertexArrays = void(APIENTRY*)(GLsizei, GLuint*);
using BindVertexArray = void(APIENTRY*)(GLuint);
using GenBuffers = void(APIENTRY*)(GLsizei, GLuint*);
using BindBuffer = void(APIENTRY*)(GLenum, GLuint);
using BufferData = void(APIENTRY*)(GLenum, GLsizeiptr, const void*, GLenum);
using VertexAttribPointer = void(APIENTRY*)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void*);
using EnableVertexAttribArray = void(APIENTRY*)(GLuint);
using BlendFuncSeparate = void(APIENTRY*)(GLenum, GLenum, GLenum, GLenum);
using BlendEquationSeparate = void(APIENTRY*)(GLenum, GLenum);
using BindFramebuffer = void(APIENTRY*)(GLenum, GLuint);
struct Api {
    CreateShader createShader;
    ShaderSource shaderSource;
    CompileShader compileShader;
    GetShaderiv getShaderiv;
    CreateProgram createProgram;
    AttachShader attachShader;
    BindAttribLocation bindAttribLocation;
    LinkProgram linkProgram;
    GetProgramiv getProgramiv;
    UseProgram useProgram;
    GetUniformLocation getUniformLocation;
    Uniform4f uniform4f;
    GenVertexArrays genVertexArrays;
    BindVertexArray bindVertexArray;
    GenBuffers genBuffers;
    BindBuffer bindBuffer;
    BufferData bufferData;
    VertexAttribPointer vertexAttribPointer;
    EnableVertexAttribArray enableVertexAttribArray;
    BlendFuncSeparate blendFuncSeparate;
    BlendEquationSeparate blendEquationSeparate;
};
} // namespace gl
static void drainGl() {
    for (int i = 0; i < 16 && glGetError() != GL_NO_ERROR; ++i) {}
}
static HGLRC cueContext = nullptr;
static gl::Api cueApi{};
static GLuint cueProgram = 0, cueVao = 0, cueVbo = 0;
static GLint cueColor = -1;
static GLuint cueArcProgram = 0, cueArcVao = 0, cueArcVbo = 0;
static GLint cueArcColor = -1; // vertices are x, y, alpha
template <class T> static bool loadGl(T& f, const char* name) {
    f = (T)wglGetProcAddress(name);
    return f && (uintptr_t)f > 3 && (intptr_t)f != -1;
}
static GLuint compileCue(const char* vs, const char* fs) {
    auto& g = cueApi;
    GLint ok = 0;
    GLuint v = g.createShader(0x8B31), f = g.createShader(0x8B30);
    if (!v || !f) return 0;
    g.shaderSource(v, 1, &vs, nullptr);
    g.compileShader(v);
    g.getShaderiv(v, 0x8B81, &ok);
    if (!ok) return 0;
    g.shaderSource(f, 1, &fs, nullptr);
    g.compileShader(f);
    g.getShaderiv(f, 0x8B81, &ok);
    if (!ok) return 0;
    GLuint p = g.createProgram();
    if (!p) return 0;
    g.attachShader(p, v);
    g.attachShader(p, f);
    g.bindAttribLocation(p, 0, "position");
    g.linkProgram(p);
    g.getProgramiv(p, 0x8B82, &ok);
    return ok ? p : 0;
}
static bool prepareCue() {
    HGLRC context = wglGetCurrentContext();
    if (!context) return false;
    if (context == cueContext) return cueProgram != 0;
    cueContext = context;
    cueProgram = 0;
    cueArcProgram = 0; // Old objects belong to the old context; never touched again.
    auto& g = cueApi;
    if (!loadGl(g.createShader, "glCreateShader") || !loadGl(g.shaderSource, "glShaderSource") ||
        !loadGl(g.compileShader, "glCompileShader") || !loadGl(g.getShaderiv, "glGetShaderiv") ||
        !loadGl(g.createProgram, "glCreateProgram") || !loadGl(g.attachShader, "glAttachShader") ||
        !loadGl(g.bindAttribLocation, "glBindAttribLocation") || !loadGl(g.linkProgram, "glLinkProgram") ||
        !loadGl(g.getProgramiv, "glGetProgramiv") || !loadGl(g.useProgram, "glUseProgram") ||
        !loadGl(g.getUniformLocation, "glGetUniformLocation") || !loadGl(g.uniform4f, "glUniform4f") ||
        !loadGl(g.genVertexArrays, "glGenVertexArrays") || !loadGl(g.bindVertexArray, "glBindVertexArray") ||
        !loadGl(g.genBuffers, "glGenBuffers") || !loadGl(g.bindBuffer, "glBindBuffer") ||
        !loadGl(g.bufferData, "glBufferData") || !loadGl(g.vertexAttribPointer, "glVertexAttribPointer") ||
        !loadGl(g.enableVertexAttribArray, "glEnableVertexAttribArray") ||
        !loadGl(g.blendFuncSeparate, "glBlendFuncSeparate") ||
        !loadGl(g.blendEquationSeparate, "glBlendEquationSeparate"))
        return false;
    GLint program = 0;
    glGetIntegerv(0x8B8D, &program); // Compilation does not change bindings, but be exact.
    GLuint p = compileCue("#version 150\nin vec2 position;void main(){gl_Position=vec4(position,0.0,1.0);}",
                          "#version 150\nuniform vec4 color;out vec4 fragment;void main(){fragment=color;}");
    if (!p)
        p = compileCue("#version 130\nin vec2 position;void main(){gl_Position=vec4(position,0.0,1.0);}",
                       "#version 130\nuniform vec4 color;out vec4 fragment;void main(){fragment=color;}");
    if (!p)
        p = compileCue(
            "#version 120\nattribute vec2 position;void main(){gl_Position=vec4(position,0.0,1.0);}",
            "#version 120\nuniform vec4 color;void main(){gl_FragColor=color;}");
    drainGl();
    if (!p) return false;
    cueColor = g.getUniformLocation(p, "color");
    GLint vao = 0, buffer = 0;
    glGetIntegerv(0x85B5, &vao);
    glGetIntegerv(0x8894, &buffer);
    g.genVertexArrays(1, &cueVao);
    g.genBuffers(1, &cueVbo);
    g.bindVertexArray(cueVao);
    g.bindBuffer(0x8892, cueVbo);
    g.bufferData(0x8892, sizeof(float) * 12, nullptr, 0x88E8);
    g.vertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    g.enableVertexAttribArray(0);
    g.bindVertexArray(GLuint(vao));
    g.bindBuffer(0x8892, GLuint(buffer));
    const bool ok = glGetError() == GL_NO_ERROR && cueColor >= 0 && cueVao && cueVbo;
    drainGl();
    if (ok) cueProgram = p;
    if (ok) { // the arc has its own program and buffers so the highlight's stay untouched
        GLuint q = compileCue("#version 150\nin vec3 position;out float v;void "
                              "main(){v=position.z;gl_Position=vec4(position.xy,0.0,1.0);}",
                              "#version 150\nuniform vec4 color;in float v;out vec4 fragment;void "
                              "main(){fragment=vec4(color.rgb,color.a*v);}");
        if (!q)
            q = compileCue("#version 130\nin vec3 position;out float v;void "
                           "main(){v=position.z;gl_Position=vec4(position.xy,0.0,1.0);}",
                           "#version 130\nuniform vec4 color;in float v;out vec4 fragment;void "
                           "main(){fragment=vec4(color.rgb,color.a*v);}");
        if (!q)
            q = compileCue("#version 120\nattribute vec3 position;varying float v;void "
                           "main(){v=position.z;gl_Position=vec4(position.xy,0.0,1.0);}",
                           "#version 120\nuniform vec4 color;varying float v;void "
                           "main(){gl_FragColor=vec4(color.rgb,color.a*v);}");
        drainGl();
        if (q) {
            cueArcColor = g.getUniformLocation(q, "color");
            GLint vao2 = 0, buffer2 = 0;
            glGetIntegerv(0x85B5, &vao2);
            glGetIntegerv(0x8894, &buffer2);
            g.genVertexArrays(1, &cueArcVao);
            g.genBuffers(1, &cueArcVbo);
            g.bindVertexArray(cueArcVao);
            g.bindBuffer(0x8892, cueArcVbo);
            g.bufferData(0x8892, sizeof(float) * 3 * 3 * 68, nullptr, 0x88E8);
            g.vertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, nullptr);
            g.enableVertexAttribArray(0);
            g.bindVertexArray(GLuint(vao2));
            g.bindBuffer(0x8892, GLuint(buffer2));
            if (glGetError() == GL_NO_ERROR && cueArcColor >= 0 && cueArcVao && cueArcVbo) cueArcProgram = q;
            drainGl();
        }
    }
    return ok;
}
// The game's "CursorCom" option (the player's combat cursor colour) is stored here as RGBA bytes; its
// setter is 0x12B830 and the cursor draw 0x13E9C0 tints the combat crosshair with it.
// Returns false when the cue cannot be drawn on this context.
static bool drawCue(fp::CueSide side, float x, float y, float fill, float alpha, int width, int height) {
    const int built =
        *(int*)(gameAt(fp::sig::cursorTextureSize)); // the size 0x13E740 last built the cursor texture for
    const float cursorPx = float(built >= 8 && built <= 256 ? built : fp::defaultCursorPx);
    float arc[3 * 3 * 68];
    int counts[3];
    if (!(alpha > 0) ||
        !fp::cueArc(side, x, y, cursorPx, cueSize.load(), fill, arc, int(sizeof arc / sizeof(float)), counts))
        return true;
    if (!prepareCue() || !cueArcProgram) return false;
    GLint framebuffer = 0;
    glGetIntegerv(0x8CA6, &framebuffer);
    if (framebuffer != 0) return true; // Not the presented image.
    auto& g = cueApi;
    GLint program = 0, vao = 0, buffer = 0, viewport[4]{}, src[2]{}, dst[2]{}, eq[2]{};
    GLboolean mask[4]{};
    glGetIntegerv(0x8B8D, &program);
    glGetIntegerv(0x85B5, &vao);
    glGetIntegerv(0x8894, &buffer);
    glGetIntegerv(GL_VIEWPORT, viewport);
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
    drainGl();
    glViewport(0, 0, width, height);
    for (int i = 1; i < 7; ++i) glDisable(caps[i]);
    glEnable(GL_BLEND);
    g.blendEquationSeparate(0x8006, 0x8006);
    g.blendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glColorMask(1, 1, 1, 1);
    g.useProgram(cueArcProgram);
    g.bindVertexArray(cueArcVao);
    g.bindBuffer(0x8892, cueArcVbo);
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
        g.uniform4f(cueArcColor, 0, 0, 0, a * a);
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
    g.uniform4f(cueArcColor, tint[0], tint[1], tint[2], a);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, counts[0]);
    if (counts[1]) glDrawArrays(GL_TRIANGLE_STRIP, counts[0], counts[1]);
    if (counts[2]) glDrawArrays(GL_TRIANGLE_STRIP, counts[0] + counts[1], counts[2]);
    const bool ok = glGetError() == GL_NO_ERROR;
    g.useProgram(GLuint(program));
    g.bindVertexArray(GLuint(vao));
    g.bindBuffer(0x8892, GLuint(buffer));
    glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
    g.blendFuncSeparate(src[0], dst[0], src[1], dst[1]);
    g.blendEquationSeparate(eq[0], eq[1]);
    glColorMask(mask[0], mask[1], mask[2], mask[3]);
    for (int i = 0; i < 7; ++i) (enabled[i] ? glEnable : glDisable)(caps[i]);
    drainGl();
    if (ok) ++cueDraws;
    return ok;
}
static void presentCue(fp::CueSide side, LONG x, LONG y, float fill, float alpha) {
    if (side == fp::CueSide::none) return;
    if (wglGetCurrentContext() != cueContext) cueGl = 0; // A new context gets its own attempt.
    if (cueGl.load() < 0) return;
    HWND w = hookedWindow;
    RECT rect;
    if (!w || !GetClientRect(w, &rect) || rect.right < 40 || rect.bottom < 40) return;
    __try {
        if (!drawCue(side, float(x), float(y), fill, alpha, rect.right, rect.bottom))
            cueGl = -1;
        else
            cueGl = 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
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
            kind == windowArsenal)
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
static fp::Crouch crouchState; // the game's thread
static void crouchFrame() {
    const uintptr_t root = *(uintptr_t*)(gameAt(fp::sig::playerRoot));
    bool allowed = false, room = true;
    __try {
        if (root && readable(root, 0x1300) && InterlockedCompareExchange(&active, 0, 0) &&
            !InterlockedCompareExchange(&stopping, 0, 0) && viewMode.load() < 2) {
            const unsigned state = *(unsigned*)(root + field::character::stateFlags);
            allowed = !*(unsigned char*)(root + field::character::combatStance) &&
                      !(state & (0xac0 | 0x40204 | 0xa00)) &&
                      !(*(unsigned*)(root + field::character::lifeFlags) & 0x100) &&
                      !dialogueOpen(); // not in combat, down, dead or talking
            if (allowed && crouchState.on) room = crouchHeadroomClear(root);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        allowed = false;
    }
    const bool key = duckKey.load() &&
                     !InterlockedCompareExchange(
                         &windowFree, 0,
                         0); // a press while a game window holds the view (typing, clicking) is not a crouch
    crouchHeld = fp::crouchStep(crouchState, crouchMode.load(), key, allowed, room);
}
