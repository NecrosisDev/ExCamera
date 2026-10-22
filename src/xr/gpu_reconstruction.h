#pragma once
#include "reconstruction.h"
#include <GL/gl.h>
#include <windows.h>

namespace fp::xr {
// GPU reconstruction: a backward warp that, for every eye pixel, marches its view ray through
// the captured centre depth until the ray passes behind the stored surface, then samples the
// centre colour there. Magnification gaps close by construction; true disocclusions stay
// transparent, which the compositor shows as black. It is our own code against the frame
// contract and writes straight into the swapchain images.
class GpuReconstructor final : public Reconstructor {
public:
    bool fill = false; // filled mode: disocclusions take the visible background instead of transparency
private:
    using Create = GLuint(APIENTRY*)(GLenum);
    using Source = void(APIENTRY*)(GLuint, GLsizei, const char* const*, const GLint*);
    using Compile = void(APIENTRY*)(GLuint);
    using GetShaderiv = void(APIENTRY*)(GLuint, GLenum, GLint*);
    using Log = void(APIENTRY*)(GLuint, GLsizei, GLsizei*, char*);
    using CreateProgram = GLuint(APIENTRY*)();
    using AttachOrDelete = void(APIENTRY*)(GLuint, GLuint);
    using Link = void(APIENTRY*)(GLuint);
    using Use = void(APIENTRY*)(GLuint);
    using Location = GLint(APIENTRY*)(GLuint, const char*);
    using Uniform1i = void(APIENTRY*)(GLint, GLint);
    using Uniform1f = void(APIENTRY*)(GLint, GLfloat);
    using Uniform2f = void(APIENTRY*)(GLint, GLfloat, GLfloat);
    using Uniform4f = void(APIENTRY*)(GLint, GLfloat, GLfloat, GLfloat, GLfloat);
    using UniformMatrix = void(APIENTRY*)(GLint, GLsizei, GLboolean, const GLfloat*);
    using ActiveTexture = void(APIENTRY*)(GLenum);
    using Bind = void(APIENTRY*)(GLenum, GLuint);
    using Attach = void(APIENTRY*)(GLenum, GLenum, GLenum, GLuint, GLint);
    using Status = GLenum(APIENTRY*)(GLenum);
    using DeleteOne = void(APIENTRY*)(GLuint);
    template <class T> static T procedure(const char* name) {
        auto p = wglGetProcAddress(name);
        if (!p || uintptr_t(p) <= 3 || intptr_t(p) == -1) throw std::runtime_error(std::string("GL procedure missing: ") + name);
        return reinterpret_cast<T>(p);
    }
    Create createShader = procedure<Create>("glCreateShader");
    Source shaderSource = procedure<Source>("glShaderSource");
    Compile compileShader = procedure<Compile>("glCompileShader");
    GetShaderiv getShaderiv = procedure<GetShaderiv>("glGetShaderiv");
    GetShaderiv getProgramiv = procedure<GetShaderiv>("glGetProgramiv");
    Log shaderLog = procedure<Log>("glGetShaderInfoLog");
    Log programLog = procedure<Log>("glGetProgramInfoLog");
    CreateProgram createProgram = procedure<CreateProgram>("glCreateProgram");
    AttachOrDelete attachShader = procedure<AttachOrDelete>("glAttachShader");
    Link linkProgram = procedure<Link>("glLinkProgram");
    Use useProgram = procedure<Use>("glUseProgram");
    Location uniformLocation = procedure<Location>("glGetUniformLocation");
    Uniform1i uniform1i = procedure<Uniform1i>("glUniform1i");
    Uniform1f uniform1f = procedure<Uniform1f>("glUniform1f");
    Uniform2f uniform2f = procedure<Uniform2f>("glUniform2f");
    Uniform4f uniform4f = procedure<Uniform4f>("glUniform4f");
    UniformMatrix uniformMatrix4 = procedure<UniformMatrix>("glUniformMatrix4fv");
    ActiveTexture activeTexture = procedure<ActiveTexture>("glActiveTexture");
    Bind bindFramebuffer = procedure<Bind>("glBindFramebuffer");
    Attach framebufferTexture = procedure<Attach>("glFramebufferTexture2D");
    Status checkFramebuffer = procedure<Status>("glCheckFramebufferStatus");
    DeleteOne deleteShader = procedure<DeleteOne>("glDeleteShader");
    DeleteOne deleteProgram = procedure<DeleteOne>("glDeleteProgram");
    GLuint program_ = 0;
    std::string failure_;

    static constexpr const char* vertex = R"(#version 130
out vec2 pixel;
void main() {
    vec2 corner = vec2((gl_VertexID & 1) * 4 - 1, (gl_VertexID & 2) * 2 - 1);
    pixel = corner * .5 + .5;
    gl_Position = vec4(corner, 0, 1);
})";
    // Depth linearisation mirrors depth.h: ndc from the stored value, then m14 / (ndc + m10).
    static constexpr const char* fragment = R"(#version 130
uniform sampler2D colour;
uniform sampler2D depth;
uniform mat4 eyeToCentre;
uniform vec4 eyeTangents;     // (1/m0, 1/m5, m8, m9) of the eye projection
uniform vec4 centreTerms;     // (m0, m5, m8, m9) of the centre projection
uniform vec2 centreDepthTerms; // (m10, m14) of the centre projection
uniform vec4 depthConvention;  // (rangeNear, rangeFar, reversed, zeroToOne)
uniform vec2 marchRange;       // (near, far) in centre units along the eye ray
uniform float flipV;
uniform float fillHoles;
in vec2 pixel;
out vec4 fragment;
float linear(vec2 uv) {
    float stored = texture(depth, vec2(uv.x, flipV > .5 ? 1. - uv.y : uv.y)).r;
    if (depthConvention.z > .5) stored = 1. - stored;
    float window = (stored - depthConvention.x) / (depthConvention.y - depthConvention.x);
    float ndc = depthConvention.w > .5 ? window : window * 2. - 1.;
    float d = ndc + centreDepthTerms.x;
    return abs(d) < 1e-12 ? 1e30 : centreDepthTerms.y / d;
}
bool project(vec3 p, out vec2 uv, out float viewDepth) {
    viewDepth = -p.z;
    if (viewDepth <= 0.) return false;
    uv = vec2((centreTerms.x * p.x + centreTerms.z * p.z) / viewDepth, (centreTerms.y * p.y + centreTerms.w * p.z) / viewDepth) * .5 + .5;
    return all(greaterThanEqual(uv, vec2(0.))) && all(lessThanEqual(uv, vec2(1.)));
}
void main() {
    vec2 ndc = pixel * 2. - 1.;
    vec3 ray = vec3((ndc.x + eyeTangents.z) * eyeTangents.x, (ndc.y + eyeTangents.w) * eyeTangents.y, -1.);
    float lo = marchRange.x, hi = marchRange.y, hit = -1.;
    const int steps = 24;
    float previous = lo;
    for (int i = 1; i <= steps; ++i) {
        float t = lo * pow(hi / lo, float(i) / float(steps));
        vec3 p = (eyeToCentre * vec4(ray * t, 1.)).xyz;
        vec2 uv; float viewDepth;
        if (!project(p, uv, viewDepth)) { previous = t; continue; }
        if (linear(uv) <= viewDepth) { hit = t; break; }
        previous = t;
    }
    if (hit < 0.) {
        // Nothing nearer than the far plane: the far sentinel (sky, clear colour) is carried over by
        // direction alone, exactly as the CPU oracle warps it at the far distance.
        vec3 p = (eyeToCentre * vec4(ray * hi, 1.)).xyz;
        vec2 uv; float viewDepth;
        if (project(p, uv, viewDepth) && linear(uv) >= hi * .98) fragment = vec4(texture(colour, vec2(uv.x, flipV > .5 ? 1. - uv.y : uv.y)).rgb, 1.);
        else fragment = vec4(0.);
        return;
    }
    float a = previous, b = hit;
    for (int i = 0; i < 6; ++i) {
        float m = .5 * (a + b);
        vec3 p = (eyeToCentre * vec4(ray * m, 1.)).xyz;
        vec2 uv; float viewDepth;
        if (project(p, uv, viewDepth) && linear(uv) <= viewDepth) b = m; else a = m;
    }
    vec3 p = (eyeToCentre * vec4(ray * b, 1.)).xyz;
    vec2 uv; float viewDepth;
    project(p, uv, viewDepth);
    // A surface much nearer than the ray point is an occluder seen edge-on: a disocclusion.
    if (linear(uv) < viewDepth * .97) {
        if (fillHoles < .5) { fragment = vec4(0.); return; }
        // Filled: carry on along the ray for the first stored surface that meets it, which is the
        // background the centre view saw past the occluder; failing that, stretch the occluder.
        for (int i = 1; i <= 12; ++i) {
            float t = b * pow(hi / b, float(i) / 12.);
            vec3 q = (eyeToCentre * vec4(ray * t, 1.)).xyz;
            vec2 uv2; float viewDepth2;
            if (!project(q, uv2, viewDepth2)) continue;
            float stored = linear(uv2);
            if (stored >= viewDepth2 * .97 && stored <= viewDepth2 * 1.03) { uv = uv2; break; }
        }
    }
    fragment = vec4(texture(colour, vec2(uv.x, flipV > .5 ? 1. - uv.y : uv.y)).rgb, 1.);
})";
    GLuint compile(GLenum kind, const char* source) {
        const GLuint shader = createShader(kind);
        shaderSource(shader, 1, &source, nullptr);
        compileShader(shader);
        GLint ok = 0;
        getShaderiv(shader, 0x8B81, &ok);
        if (!ok) {
            char text[1024]{};
            shaderLog(shader, sizeof text, nullptr, text);
            deleteShader(shader);
            throw std::runtime_error(std::string("shader: ") + text);
        }
        return shader;
    }
    void build() {
        if (program_ || !failure_.empty()) return;
        try {
            const GLuint v = compile(0x8B31, vertex), f = compile(0x8B30, fragment);
            program_ = createProgram();
            attachShader(program_, v);
            attachShader(program_, f);
            linkProgram(program_);
            deleteShader(v);
            deleteShader(f);
            GLint ok = 0;
            getProgramiv(program_, 0x8B82, &ok);
            if (!ok) {
                char text[1024]{};
                programLog(program_, sizeof text, nullptr, text);
                deleteProgram(program_);
                program_ = 0;
                throw std::runtime_error(std::string("program: ") + text);
            }
        } catch (const std::exception& e) { failure_ = e.what(); }
    }

public:
    struct Target { GLuint framebuffer, texture; int width, height; };
    ~GpuReconstructor() { if (program_) deleteProgram(program_); }
    const char* name() const override { return "gpu-backward-warp"; }
    bool available(std::string& reason) const override {
        if (!failure_.empty()) reason = failure_;
        return failure_.empty();
    }
    // Compiles the program on the current context; a failure is remembered and reported.
    bool prepare(std::string& reason) {
        build();
        if (!program_) reason = failure_;
        return program_ != 0;
    }
    bool synthesize(const CenterView&, const std::array<Eye, 2>&, std::array<EyeImage, 2>&, std::string& reason) override {
        reason = "the GPU warp renders into swapchain images, not CPU buffers";
        return false;
    }
    // Draws both eyes into their targets. The caller owns every other GL binding and restores it.
    bool render(const FrameContract& frame, const std::array<Eye, 2>& eyes, const std::array<Target, 2>& targets, std::string& reason) {
        build();
        if (!program_) { reason = failure_; return false; }
        if (const char* why = frame.validate()) { reason = why; return false; }
        const auto centreView = viewMatrix(frame.view);
        useProgram(program_);
        activeTexture(0x84C0);
        glBindTexture(GL_TEXTURE_2D, frame.colorTexture);
        activeTexture(0x84C1);
        glBindTexture(GL_TEXTURE_2D, frame.depthTexture);
        glTexParameteri(GL_TEXTURE_2D, 0x884C, GL_NONE); // sample raw depth, never a comparison
        activeTexture(0x84C0);
        uniform1i(uniformLocation(program_, "colour"), 0);
        uniform1i(uniformLocation(program_, "depth"), 1);
        const auto& m = frame.projection;
        uniform4f(uniformLocation(program_, "centreTerms"), m[0], m[5], m[8], m[9]);
        uniform2f(uniformLocation(program_, "centreDepthTerms"), m[10], m[14]);
        uniform4f(uniformLocation(program_, "depthConvention"), frame.depth.rangeNear, frame.depth.rangeFar, frame.depth.reversed ? 1.f : 0.f,
                  frame.depth.range == DepthConvention::Range::zeroToOne ? 1.f : 0.f);
        uniform1f(uniformLocation(program_, "flipV"), frame.originBottomLeft ? 0.f : 1.f);
        uniform1f(uniformLocation(program_, "fillHoles"), fill ? 1.f : 0.f);
        const float farDistance = frame.clipFar > 0 ? frame.clipFar : frame.clipNear * 1.e5f;
        uniform2f(uniformLocation(program_, "marchRange"), frame.clipNear, farDistance * 1.02f); // the last tap reaches the far sentinel
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_BLEND);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_CULL_FACE);
        for (unsigned i = 0; i < 2; ++i) {
            const auto& eye = eyes[i];
            const auto eyeToCentre = product(centreView, poseMatrix(eye.pose));
            uniformMatrix4(uniformLocation(program_, "eyeToCentre"), 1, GL_FALSE, eyeToCentre.data());
            uniform4f(uniformLocation(program_, "eyeTangents"), 1 / eye.projection[0], 1 / eye.projection[5], eye.projection[8], eye.projection[9]);
            bindFramebuffer(0x8D40, targets[i].framebuffer);
            framebufferTexture(0x8D40, 0x8CE0, GL_TEXTURE_2D, targets[i].texture, 0);
            if (checkFramebuffer(0x8D40) != 0x8CD5) { reason = "incomplete eye target"; useProgram(0); return false; }
            glDrawBuffer(0x8CE0);
            glViewport(0, 0, targets[i].width, targets[i].height);
            glDrawArrays(GL_TRIANGLES, 0, 3);
        }
        useProgram(0);
        if (glGetError() != GL_NO_ERROR) { reason = "GL error during the warp"; return false; }
        return true;
    }
};
}
