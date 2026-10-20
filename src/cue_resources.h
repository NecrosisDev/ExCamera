#pragma once

namespace fp {
// All handles, including incomplete construction, belong to this owner. The
// caller keeps its context current until prepare/retire has finished.
struct CueResources {
    unsigned program = 0, vao = 0, vbo = 0;
    // Optional first-person headwear vertex stream. Same exact context owner,
    // deletion admission and stop drain; never shared with the arc's upload.
    unsigned headwearVbo = 0;
    int color = -1;
    unsigned vertex = 0, fragment = 0;
    bool vertexAttached = false, fragmentAttached = false;
    int savedVao = 0, savedBuffer = 0;
    bool borrowedBindings = false;

    bool live() const { return program || vao || vbo || headwearVbo || vertex || fragment || borrowedBindings; }
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
    template <class Api> void restoreBindings(Api& g) {
        if (!borrowedBindings) return;
        g.bindVertexArray(unsigned(savedVao));
        g.bindBuffer(0x8892, unsigned(savedBuffer));
        borrowedBindings = false;
    }
    template <class Api> void retire(Api& g) {
        restoreBindings(g);
        releaseShaders(g);
        if (vao) g.deleteVertexArrays(1, &vao);
        vao = 0;
        if (vbo) g.deleteBuffers(1, &vbo);
        vbo = 0;
        if (headwearVbo) g.deleteBuffers(1, &headwearVbo);
        headwearVbo = 0;
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
        if (live()) retire(g);
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
                    retire(g);
                    drain(g);
                    return false;
                }
            }
        }
        const bool ok = ready() && g.getError() == 0;
        if (!ok) retire(g);
        drain(g);
        return ok;
    }
};
} // namespace fp
