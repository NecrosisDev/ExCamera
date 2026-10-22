#pragma once

namespace fp {
// All handles, including incomplete construction, belong to this owner. The
// caller keeps its context current until prepare/retire has finished.
struct CueResources {
    unsigned program = 0, vao = 0, vbo = 0;
    // Optional first-person headwear vertex stream. Same exact context owner,
    // deletion admission and stop drain; never shared with the arc's upload.
    unsigned headwearVbo = 0;
    // Optional VR hand stream, same owner and lifetime.
    unsigned handVbo = 0, handEbo = 0;
    // Stone skin maps (mod/stone_skin.inl): diffuse, specular, normal. Their own liveness, so a
    // cue rebuild never drops them; retired with the context.
    unsigned stoneTex[3]{};
    bool stoneLive() const { return stoneTex[0] || stoneTex[1] || stoneTex[2]; }
    // Wound skins (mod/wounds_bake.inl): per-character copies of the body diffuse with the wounds
    // baked in, and the bake's program, vertex array, buffer and framebuffer. Their own liveness.
    static constexpr unsigned woundSkinLimit = 6;
    unsigned woundSkinTex[woundSkinLimit]{}, woundSkinNrm[woundSkinLimit]{}, woundSkinSpc[woundSkinLimit]{};
    unsigned woundBakeProgram = 0, woundBakeNormalProgram = 0, woundBakeSpecProgram = 0, woundBakeVao = 0, woundBakeVbo = 0,
             woundBakeFbo = 0;
    bool woundLive() const {
        for (unsigned k = 0; k < woundSkinLimit; ++k)
            if (woundSkinTex[k] || woundSkinNrm[k] || woundSkinSpc[k]) return true;
        return woundBakeProgram || woundBakeNormalProgram || woundBakeSpecProgram || woundBakeVao || woundBakeVbo || woundBakeFbo;
    }
    template <class Api> void retireWound(Api& g) {
        for (unsigned k = 0; k < woundSkinLimit; ++k) {
            if (woundSkinTex[k]) g.deleteTextures(1, &woundSkinTex[k]);
            if (woundSkinNrm[k]) g.deleteTextures(1, &woundSkinNrm[k]);
            if (woundSkinSpc[k]) g.deleteTextures(1, &woundSkinSpc[k]);
            woundSkinTex[k] = woundSkinNrm[k] = woundSkinSpc[k] = 0;
        }
        if (woundBakeNormalProgram) g.deleteProgram(woundBakeNormalProgram);
        if (woundBakeSpecProgram) g.deleteProgram(woundBakeSpecProgram);
        woundBakeNormalProgram = woundBakeSpecProgram = 0;
        if (woundBakeProgram) g.deleteProgram(woundBakeProgram);
        if (woundBakeVao) g.deleteVertexArrays(1, &woundBakeVao);
        if (woundBakeVbo) g.deleteBuffers(1, &woundBakeVbo);
        if (woundBakeFbo && g.deleteFramebuffers) g.deleteFramebuffers(1, &woundBakeFbo);
        woundBakeProgram = woundBakeVao = woundBakeVbo = woundBakeFbo = 0;
    }
    template <class Api> void retireStone(Api& g) {
        for (unsigned& t : stoneTex) {
            if (t) g.deleteTextures(1, &t);
            t = 0;
        }
    }
    // Optional screen-space view effects (mod/view_effects.inl). Each effect has
    // its own program, so a driver rejecting one leaves the others and the cue.
    // They share the fullscreen quad and the copied scene colour/depth.
    unsigned viewVao = 0, viewVbo = 0, viewColor = 0, viewDepth = 0;
    // Last frame's depth copy, swapped with viewDepth while frames are blended (motion rejection).
    unsigned viewDepthPrevious = 0;
    unsigned viewCopyFbo = 0; // the window's colour and depth blitted into viewColor / viewDepth
    int viewDepthFormat = 0; // both copies' internal format; 0 until defined
    unsigned aoProgram = 0, ssrProgram = 0, dofProgram = 0, finishProgram = 0, compositeProgram = 0,
             resolveProgram = 0, dofCompositeProgram = 0, meterProgram = 0, distanceProgram = 0;
    // Wound marks' own pass (view_effects.inl): program, framebuffer over the image, the box's quad.
    unsigned woundProgram = 0, woundFbo = 0, woundQuadVao = 0, woundQuadVbo = 0;
    // Fire and smoke (fire.inl): its program; it draws through the wound pass's framebuffer and quad.
    unsigned fireProgram = 0;
    // Ambient occlusion and reflections render at half resolution: [0] occlusion, [1] reflection.
    unsigned halfFbo[2]{}, halfTex[2]{};
    // Their blend over frames, same size and lifetime: [effect][ping-pong]. historySlot
    // holds the newest; historyFrame[effect] is the blended frame it is usable in, 0 none.
    unsigned historyFbo[2][2]{}, historyTex[2][2]{};
    unsigned historySlot = 0, historyFrame[2]{};
    // Depth of field gathers at half resolution too, filtered for its composite.
    unsigned dofFbo = 0, dofTex = 0;
    // Half-resolution view distances for the occlusion and reflection taps.
    unsigned distanceFbo = 0, distanceTex = 0;
    int halfWidth = 0, halfHeight = 0, ssrWidth = 0, ssrHeight = 0; // reflections: half the half size
    int dofWidth = 0, dofHeight = 0; // the depth-of-field blur keeps a half (a third above 1200 px)
    int viewWidth = 0, viewHeight = 0;
    // The full-resolution passes hand the image on between viewColor and chainTex
    // instead of copying the framebuffer back: chainFbo[0] draws into viewColor, [1] into chainTex.
    unsigned chainTex = 0, chainFbo[2]{};
    int chainWidth = 0, chainHeight = 0;
    // Depth-of-field focus readback: two buffers so the CPU reads last frame's
    // patch while this frame's copy is in flight; never a pipeline stall.
    unsigned focusBuffers[2]{};
    void* focusFences[2]{};
    // Eye adaptation: a 32 x 18 brightness meter, read back the same way as the focus.
    unsigned meterFbo = 0, meterTex = 0, meterBuffers[2]{};
    unsigned meterSourceFbo = 0, meterSourceTex = 0; // a small copy of the image the meter reads
    void* meterFences[2]{};
    // The player's hidden body (third person): a mask drawn during the camera pass, its
    // own copy of the scene depth to test against, and their framebuffer.
    unsigned silhouetteFbo = 0, silhouetteMask = 0, silhouetteDepth = 0;
    int silhouetteWidth = 0, silhouetteHeight = 0;
    // Held-item surfaces: four channels of tiling smooth noise in a small mipmapped 3D texture.
    unsigned heldNoise = 0;
    unsigned heldData = 0; // per held item / body part: frame rows, light, colour, strength, blood (16 x 40 floats4)
    int color = -1;
    unsigned vertex = 0, fragment = 0;
    bool vertexAttached = false, fragmentAttached = false;
    int savedVao = 0, savedBuffer = 0;
    bool borrowedBindings = false;

    bool viewLive() const {
        return viewVao || viewVbo || viewColor || viewDepth || viewDepthPrevious || offscreenFbo ||
               offscreenDepthFbo || timerQueries[0][0] ||
               timerQueries[1][0] || timerQueries[2][0] || timerQueries[3][0] || aoProgram || ssrProgram || dofProgram || finishProgram || woundProgram || fireProgram || woundFbo || woundQuadVao || woundQuadVbo ||
               compositeProgram || resolveProgram || chainTex || chainFbo[0] || chainFbo[1] || dofCompositeProgram || dofFbo || dofTex || distanceProgram || distanceFbo || distanceTex || halfFbo[0] || halfFbo[1] || halfTex[0] || halfTex[1] ||
               historyFbo[0][0] || historyFbo[0][1] || historyFbo[1][0] || historyFbo[1][1] ||
               historyTex[0][0] || historyTex[0][1] || historyTex[1][0] || historyTex[1][1] ||
               focusBuffers[0] || focusBuffers[1] || focusFences[0] || focusFences[1] || meterProgram || meterFbo ||
               meterTex || meterSourceFbo || meterSourceTex || meterBuffers[0] || meterBuffers[1] || meterFences[0] || meterFences[1] || silhouetteFbo ||
               silhouetteMask || silhouetteDepth || heldNoise || heldData;
    }
    // Cue/headwear handles. Their incomplete construction must not tear down the
    // independently built view effects, and vice versa.
    bool cueLive() const { return program || vao || vbo || headwearVbo || handVbo || handEbo || vertex || fragment || borrowedBindings; }
    bool live() const { return cueLive() || viewLive() || stoneLive() || woundLive(); }
    bool ready() const { return program && vao && vbo && color >= 0 && !borrowedBindings; }

    template <class Api> static void drain(Api& g) {
        for (int i = 0; i < 16 && g.getError() != 0; ++i) {}
    }
    template <class Api> void releaseShaders(Api& g) {
        if (vertexAttached) g.detachShader(program, vertex);
        vertexAttached = false;
        if (fragmentAttached) g.detachShader(program, fragment);
        fragmentAttached = false;
        if (vertex) g.deleteShader(vertex);
        vertex = 0;
        if (fragment) g.deleteShader(fragment);
        fragment = 0;
    }
    template <class Api> static void retireProgram(Api& g, unsigned& id) {
        if (id) g.deleteProgram(id);
        id = 0;
    }
    template <class Api> void retireFocus(Api& g) {
        for (unsigned i = 0; i < 2; ++i) {
            if (focusFences[i]) g.deleteSync(focusFences[i]);
            focusFences[i] = nullptr;
        }
        if (focusBuffers[0] || focusBuffers[1]) g.deleteBuffers(2, focusBuffers);
        focusBuffers[0] = focusBuffers[1] = 0;
    }
    template <class Api> void retireMeter(Api& g) {
        for (unsigned i = 0; i < 2; ++i) {
            if (meterFences[i]) g.deleteSync(meterFences[i]);
            meterFences[i] = nullptr;
        }
        if (meterBuffers[0] || meterBuffers[1]) g.deleteBuffers(2, meterBuffers);
        meterBuffers[0] = meterBuffers[1] = 0;
        if (meterFbo) g.deleteFramebuffers(1, &meterFbo);
        meterFbo = 0;
        if (meterTex) g.deleteTextures(1, &meterTex);
        meterTex = 0;
        if (meterSourceFbo) g.deleteFramebuffers(1, &meterSourceFbo);
        meterSourceFbo = 0;
        if (meterSourceTex) g.deleteTextures(1, &meterSourceTex);
        meterSourceTex = 0;
    }
    // Offscreen sampling: our framebuffer onto the game's view texture, and the sampling
    // target's depth copied to that texture's size in the target's own depth format.
    unsigned offscreenFbo = 0, offscreenDepthFbo = 0, offscreenDepthLayout = 0, offscreenDepthType = 0,
             offscreenDepthAttachment = 0;
    int offscreenDepthFormat = 0;
    unsigned offscreenKey[6]{}; // target, view texture, scene and output sizes the set-up above is for
    int offscreenSceneWidth = 0, offscreenSceneHeight = 0;
    bool offscreenEncoded = false;
    template <class Api> void retireOffscreen(Api& g) {
        if (offscreenFbo) g.deleteFramebuffers(1, &offscreenFbo);
        offscreenFbo = 0;
        if (offscreenDepthFbo) g.deleteFramebuffers(1, &offscreenDepthFbo);
        offscreenDepthFbo = 0;
        offscreenDepthFormat = 0;
        for (unsigned& k : offscreenKey) k = 0;
    }
    // GPU timings: six timestamps a frame in a ring of four frames, read back when ready.
    unsigned timerQueries[4][6]{};
    bool timerIssued[4]{};
    unsigned timerSlot = 0;
    // The CPU time (ns) each timestamp was issued at, and which call of the effects issued it,
    // so the status can tell the passes' own GPU work from the GPU waiting on the CPU.
    unsigned long long timerCpu[4][6]{};
    unsigned timerCall[4]{};
    unsigned timerCalls = 0;
    long long gpuMinusCpuNs = 0; // GL_TIMESTAMP minus the CPU clock, measured on audit frames
    bool gpuClockKnown = false;
    unsigned long long lastGpuStamp = 0; // the last collected first timestamp, and its call
    unsigned lastGpuCall = 0;
    unsigned long long lastCallCpu = 0; // the CPU time of the previous desktop call
    template <class Api> void retireTimers(Api& g) {
        for (unsigned i = 0; i < 4; ++i) {
            if (timerQueries[i][0] && g.deleteQueries) g.deleteQueries(6, timerQueries[i]);
            for (unsigned k = 0; k < 6; ++k) timerQueries[i][k] = 0;
            timerIssued[i] = false;
        }
        timerSlot = 0;
        gpuClockKnown = false;
        lastGpuStamp = 0;
    }
    template <class Api> void retireChain(Api& g) {
        if (chainFbo[0] || chainFbo[1]) g.deleteFramebuffers(2, chainFbo);
        chainFbo[0] = chainFbo[1] = 0;
        if (chainTex) g.deleteTextures(1, &chainTex);
        chainTex = 0;
        chainWidth = chainHeight = 0;
    }
    template <class Api> void retireSilhouette(Api& g) {
        if (silhouetteFbo) g.deleteFramebuffers(1, &silhouetteFbo);
        silhouetteFbo = 0;
        if (silhouetteMask) g.deleteTextures(1, &silhouetteMask);
        silhouetteMask = 0;
        if (silhouetteDepth) g.deleteTextures(1, &silhouetteDepth);
        silhouetteDepth = 0;
        silhouetteWidth = silhouetteHeight = 0;
    }
    template <class Api> void retireHalf(Api& g) {
        if (halfFbo[0] || halfFbo[1]) g.deleteFramebuffers(2, halfFbo);
        halfFbo[0] = halfFbo[1] = 0;
        if (halfTex[0] || halfTex[1]) g.deleteTextures(2, halfTex);
        halfTex[0] = halfTex[1] = 0;
        for (unsigned i = 0; i < 2; ++i) {
            if (historyFbo[i][0] || historyFbo[i][1]) g.deleteFramebuffers(2, historyFbo[i]);
            historyFbo[i][0] = historyFbo[i][1] = 0;
            if (historyTex[i][0] || historyTex[i][1]) g.deleteTextures(2, historyTex[i]);
            historyTex[i][0] = historyTex[i][1] = 0;
            historyFrame[i] = 0;
        }
        historySlot = 0;
        if (dofFbo) g.deleteFramebuffers(1, &dofFbo);
        dofFbo = 0;
        if (dofTex) g.deleteTextures(1, &dofTex);
        dofTex = 0;
        if (distanceFbo) g.deleteFramebuffers(1, &distanceFbo);
        distanceFbo = 0;
        if (distanceTex) g.deleteTextures(1, &distanceTex);
        distanceTex = 0;
        halfWidth = halfHeight = ssrWidth = ssrHeight = dofWidth = dofHeight = 0;
    }
    template <class Api> void retireView(Api& g) {
        retireProgram(g, aoProgram);
        retireProgram(g, ssrProgram);
        retireProgram(g, dofProgram);
        retireProgram(g, finishProgram);
        retireProgram(g, compositeProgram);
        retireProgram(g, resolveProgram);
        retireProgram(g, dofCompositeProgram);
        retireProgram(g, meterProgram);
        retireProgram(g, distanceProgram);
        retireProgram(g, woundProgram);
        retireProgram(g, fireProgram);
        if (woundFbo && g.deleteFramebuffers) g.deleteFramebuffers(1, &woundFbo);
        if (woundQuadVao) g.deleteVertexArrays(1, &woundQuadVao);
        if (woundQuadVbo) g.deleteBuffers(1, &woundQuadVbo);
        woundFbo = woundQuadVao = woundQuadVbo = 0;
        retireMeter(g);
        retireSilhouette(g);
        if (heldNoise) g.deleteTextures(1, &heldNoise);
        heldNoise = 0;
        if (heldData) g.deleteTextures(1, &heldData);
        heldData = 0;
        retireChain(g);
        retireTimers(g);
        retireOffscreen(g);
        retireHalf(g);
        retireFocus(g);
        if (viewVao) g.deleteVertexArrays(1, &viewVao);
        viewVao = 0;
        if (viewVbo) g.deleteBuffers(1, &viewVbo);
        viewVbo = 0;
        if (viewColor) g.deleteTextures(1, &viewColor);
        viewColor = 0;
        if (viewDepth) g.deleteTextures(1, &viewDepth);
        viewDepth = 0;
        if (viewDepthPrevious) g.deleteTextures(1, &viewDepthPrevious);
        viewDepthPrevious = 0;
        if (viewCopyFbo) g.deleteFramebuffers(1, &viewCopyFbo);
        viewCopyFbo = 0;
        viewDepthFormat = 0;
        viewWidth = viewHeight = 0;
    }
    template <class Api> void restoreBindings(Api& g) {
        if (!borrowedBindings) return;
        g.bindVertexArray(unsigned(savedVao));
        g.bindBuffer(0x8892, unsigned(savedBuffer));
        borrowedBindings = false;
    }
    template <class Api> void retire(Api& g) {
        retireCue(g);
        if (viewLive()) retireView(g);
        if (stoneLive()) retireStone(g);
        if (woundLive()) retireWound(g);
    }
    template <class Api> void retireCue(Api& g) {
        restoreBindings(g);
        releaseShaders(g);
        if (vao) g.deleteVertexArrays(1, &vao);
        vao = 0;
        if (vbo) g.deleteBuffers(1, &vbo);
        vbo = 0;
        if (headwearVbo) g.deleteBuffers(1, &headwearVbo);
        headwearVbo = 0;
        if (handVbo) g.deleteBuffers(1, &handVbo);
        handVbo = 0;
        if (handEbo) g.deleteBuffers(1, &handEbo);
        handEbo = 0;
        if (program) g.deleteProgram(program);
        program = 0;
        color = -1;
    }
    template <class Api> bool compile(Api& g, const char* vs, const char* fs, bool modern) {
        int ok = 0;
        vertex = g.createShader(0x8B31);
        fragment = g.createShader(0x8B30);
        if (vertex && fragment) {
            g.shaderSource(vertex, 1, &vs, nullptr);
            g.compileShader(vertex);
            g.getShaderiv(vertex, 0x8B81, &ok);
            if (ok) {
                ok = 0;
                g.shaderSource(fragment, 1, &fs, nullptr);
                g.compileShader(fragment);
                g.getShaderiv(fragment, 0x8B81, &ok);
            }
            if (ok) program = g.createProgram();
            if (program) {
                g.attachShader(program, vertex);
                vertexAttached = true;
                g.attachShader(program, fragment);
                fragmentAttached = true;
                g.bindAttribLocation(program, 0, "position");
                if (modern) g.bindFragDataLocation(program, 0, "fragment");
                ok = 0;
                if (!modern || g.getError() == 0) {
                    g.linkProgram(program);
                    g.getProgramiv(program, 0x8B82, &ok);
                } // A failed binding must not be hidden by the compile drain.
            }
        }
        const bool linked = program && ok;
        releaseShaders(g); // Linked code no longer needs the compiler objects.
        if (!linked && program) {
            g.deleteProgram(program);
            program = 0;
        }
        return linked;
    }
    template <class Api> bool prepare(Api& g) {
        if (ready()) return true;
        if (cueLive()) retireCue(g);
        const char* const vs[] = {
            "#version 150\nin vec3 position;out float v;void "
            "main(){v=position.z;gl_Position=vec4(position.xy,0.0,1.0);}",
            "#version 130\nin vec3 position;out float v;void "
            "main(){v=position.z;gl_Position=vec4(position.xy,0.0,1.0);}",
            "#version 120\nattribute vec3 position;varying float v;void "
            "main(){v=position.z;gl_Position=vec4(position.xy,0.0,1.0);}"
        };
        const char* const fs[] = {
            "#version 150\nuniform vec4 color;in float v;out vec4 fragment;void "
            "main(){fragment=vec4(color.rgb,color.a*v);}",
            "#version 130\nuniform vec4 color;in float v;out vec4 fragment;void "
            "main(){fragment=vec4(color.rgb,color.a*v);}",
            "#version 120\nuniform vec4 color;varying float v;void "
            "main(){gl_FragColor=vec4(color.rgb,color.a*v);}"
        };
        drain(g);
        for (unsigned i = g.modernShaders ? 0 : 2; i < (g.modernShaders ? 2u : 3u) && !program; ++i)
            compile(g, vs[i], fs[i], i < 2);
        drain(g);
        if (!program) return false;
        color = g.getUniformLocation(program, "color");
        if (color >= 0) {
            g.genVertexArrays(1, &vao);
            g.genBuffers(1, &vbo);
            if (vao && vbo) {
                g.getIntegerv(0x85B5, &savedVao);
                g.getIntegerv(0x8894, &savedBuffer);
                if (g.getError() == 0) {
                    borrowedBindings = true;
                    g.bindVertexArray(vao);
                    g.bindBuffer(0x8892, vbo);
                    g.bufferData(0x8892, sizeof(float) * 3 * 3 * 68, nullptr, 0x88E8);
                    g.vertexAttribPointer(0, 3, 0x1406, 0, 0, nullptr);
                    g.enableVertexAttribArray(0);
                    restoreBindings(g);
                } else {
                    retireCue(g);
                    drain(g);
                    return false;
                }
            }
        }
        const bool ok = ready() && g.getError() == 0;
        if (!ok) retireCue(g);
        drain(g);
        return ok;
    }
};
} // namespace fp
