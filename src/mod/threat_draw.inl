// Threat markers, the draw (docs/THREAT-MARKERS.md): on the game's own frame, after the wind-up arc, small soft
// arcs on a ring around the screen centre point toward the threats (threats.inl) that are off screen or near its
// edge. Drawn with the wind-up arc's program and buffers (CueResources) in the combat cursor's colour; the GL
// state it changes is restored as drawCueOwnedProfileBody restores it. No text, no window.
// Part of first_person.cpp's single translation unit; included after overlay.inl.

struct ThreatShown {
    uintptr_t root = 0;
    float shown = 0, aim = 0, angle = 0, boost = 1;
};
static ThreatShown threatShown[threatSlots]; // swap thread only
static ULONGLONG threatShownTick = 0;
static std::atomic<unsigned> threatsShown{0}, threatDraws{0};

static bool drawThreatArcs(float* verts, const int* counts, unsigned markers, const float* alphas, int width,
                           int height) {
    GLint framebuffer = 0;
    drainGl();
    glGetIntegerv(0x8CA6, &framebuffer);
    if (framebuffer != 0 || glGetError() != GL_NO_ERROR) return true; // Not the presented image.
    auto* owner = prepareCue();
    if (!owner || owner->cueFailed) return false;
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
        if (g.viewportArrays) g.viewportIndexed(0, 0, 0, static_cast<float>(width), static_cast<float>(height));
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
        int total = 0;
        for (unsigned m = 0; m < markers * 2; ++m) total += counts[m];
        for (int i = 0; i < total; ++i) { // pixels, y down, to clip space
            verts[i * 3] = verts[i * 3] / static_cast<float>(width) * 2 - 1;
            verts[i * 3 + 1] = 1 - verts[i * 3 + 1] / static_cast<float>(height) * 2;
        }
        g.bufferData(0x8892, static_cast<ptrdiff_t>(sizeof(float) * 3 * total), verts, 0x88E8);
        const unsigned char* colour = reinterpret_cast<const unsigned char*>(gameAt(fp::sig::combatCursorColour));
        const float tint[3] = {static_cast<float>(colour[0]) / 255.f, static_cast<float>(colour[1]) / 255.f,
                               static_cast<float>(colour[2]) / 255.f};
        int first = 0;
        for (unsigned m = 0; m < markers; ++m) {
            g.uniform4f(resources.color, tint[0], tint[1], tint[2], alphas[m]);
            glDrawArrays(GL_TRIANGLE_STRIP, first, counts[m * 2]);
            glDrawArrays(GL_TRIANGLE_STRIP, first + counts[m * 2], counts[m * 2 + 1]);
            first += counts[m * 2] + counts[m * 2 + 1];
        }
        ok = glGetError() == GL_NO_ERROR;
    } __finally {
        g.useProgram(static_cast<GLuint>(program));
        g.bindVertexArray(static_cast<GLuint>(vao));
        g.bindBuffer(0x8892, static_cast<GLuint>(buffer));
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
    if (ok) threatDraws.fetch_add(1, std::memory_order_relaxed);
    return ok;
}

// Swap thread, after presentCue. Aims each marker from the latest game-side copy, eases it, and draws the lit ones.
static void presentThreats() {
    const ULONGLONG now = GetTickCount64();
    const float elapsed = threatShownTick ? float((std::min)(now - threatShownTick, ULONGLONG(250))) : 0.f;
    threatShownTick = now;
    const int mode = threatMode.load(std::memory_order_relaxed);
    const bool live = mode != fp::threat::off && InterlockedCompareExchange(&active, 0, 0) &&
                      InterlockedCompareExchange(&cameraReady, 0, 0) && !InterlockedCompareExchange(&stopping, 0, 0) &&
                      InterlockedCompareExchange(&inputEnabled, 0, 0) && !cursorFreeNow() && !cueRetiring.load() &&
                      !cueBindingFailed.load();
    HWND w = hookedWindow;
    RECT rect{};
    if (!w || !GetClientRect(w, &rect) || rect.right < 40 || rect.bottom < 40) return;
    const float width = float(rect.right), height = float(rect.bottom);
    for (ThreatShown& t : threatShown) t.aim = 0;
    if (live) {
        float camera[12]{}, fov = 0;
        bool posed = false;
        if (TryAcquireSRWLockShared(&cameraLock)) {
            memcpy(camera, lastCamera, sizeof camera);
            fov = renderedFov;
            ReleaseSRWLockShared(&cameraLock);
            posed = true;
        }
        ThreatSeen seen[threatSlots];
        AcquireSRWLockShared(&threatLock);
        memcpy(seen, threatTable, sizeof seen);
        ReleaseSRWLockShared(&threatLock);
        const ThreatSeen* player = nullptr;
        for (const ThreatSeen& s : seen)
            if (s.root && s.sample.self && now - s.at < 500) player = &s;
        const bool wanted = player && player->placed && posed &&
                            (mode == fp::threat::always || player->sample.stance);
        const float scale = fp::projectionScale(height, fov);
        for (const ThreatSeen& s : seen) {
            if (!wanted || !s.root || s.sample.self || !s.placed || now - s.at > 300) continue;
            fp::threat::Sample sample = s.sample;
            float apart[3], delta[3];
            for (int k = 0; k < 3; ++k) {
                apart[k] = s.position[k] - player->position[k];
                delta[k] = s.position[k] - camera[9 + k];
            }
            sample.distance = std::sqrt(apart[0] * apart[0] + apart[1] * apart[1] + apart[2] * apart[2]);
            float angle = 0;
            if (!fp::threat::isThreat(sample) || !fp::threat::markerAngle(delta, camera, width, height, scale, angle))
                continue;
            ThreatShown* slot = nullptr;
            for (ThreatShown& t : threatShown)
                if (t.root == s.root) slot = &t;
            if (!slot)
                for (ThreatShown& t : threatShown)
                    if (!slot || (t.shown < slot->shown && !t.aim)) slot = &t;
            if (slot->root != s.root) *slot = ThreatShown{s.root, 0.f, 0.f, angle, 1.f};
            slot->aim = 1;
            slot->angle = angle;
            slot->boost = fp::threat::windingUp(sample) ? fp::threat::windupBoost : 1.f;
        }
    }
    float verts[threatSlots * 64]; // two bands of ten (x, y, alpha) vertices a marker
    int counts[threatSlots * 2];
    float alphas[threatSlots];
    unsigned markers = 0, used = 0;
    const float opacity = float(std::clamp(threatOpacity.load(std::memory_order_relaxed), 5, 100)) / 100.f;
    for (ThreatShown& t : threatShown) {
        t.shown = fp::threat::fade(t.shown, t.aim, elapsed);
        if (!t.root || t.shown < .01f) {
            if (!t.aim) t = ThreatShown{};
            continue;
        }
        int pair[2];
        if (!fp::threat::markerArc(width * .5f, height * .5f, width, height, t.angle, verts + used,
                                   int(sizeof verts / sizeof(float)) - int(used), pair))
            continue;
        counts[markers * 2] = pair[0];
        counts[markers * 2 + 1] = pair[1];
        alphas[markers] = (std::min)(1.f, opacity * t.shown * t.boost);
        used += unsigned(pair[0] + pair[1]) * 3;
        ++markers;
    }
    threatsShown.store(markers, std::memory_order_relaxed);
    if (!markers) return;
    AcquireSRWLockExclusive(&cueLock);
    ++cueGlDepth;
    __try {
        if (!cueDeletesInFlight && !cueRetiring.load() && !cueBindingFailed.load() &&
            !InterlockedCompareExchange(&stopping, 0, 0)) {
            __try {
                drawThreatArcs(verts, counts, markers, alphas, rect.right, rect.bottom);
            } __except (countFaultAndHandle(&windowFaults)) {
            }
        }
    } __finally {
        --cueGlDepth;
        ReleaseSRWLockExclusive(&cueLock);
    }
}
