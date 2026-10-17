#pragma once
#include <cassert>
#include <cstddef>
#include <cstdlib>
#include <map>
#include <set>

struct FakeCueApi {
    enum class Fault {
        none, vertexCreate, fragmentCreate, vertexCompile, fragmentCompile, programCreate,
        link, uniform, vaoCreate, vboCreate, stateQuery, upload, attribute, enable
    };
    Fault fault = Fault::none, exception = Fault::none;
    unsigned next = 1, error = 0, shaderCreates = 0, programCreates = 0;
    unsigned vaoCreates = 0, bufferCreates = 0, deletions = 0;
    unsigned currentVao = 91, currentBuffer = 92;
    int supportedVersion = 150;
    std::set<unsigned> shaders, programs, vaos, buffers;
    std::map<unsigned, unsigned> shaderKind;
    std::map<unsigned, int> shaderVersion;
    std::set<unsigned> attached;

    bool empty() const { return shaders.empty() && programs.empty() && vaos.empty() && buffers.empty(); }
    void trigger(Fault stage) {
        if (exception == stage) throw "injected GL failure";
        if (fault == stage) error = 0x502;
    }
    unsigned createShader(unsigned type) {
        if (fault == (type == 0x8B31 ? Fault::vertexCreate : Fault::fragmentCreate)) return 0;
        const unsigned id = next++;
        shaders.insert(id);
        shaderKind[id] = type;
        ++shaderCreates;
        return id;
    }
    void shaderSource(unsigned shader, int count, const char* const* source, const int*) {
        assert(count == 1 && shaders.count(shader));
        shaderVersion[shader] = std::atoi(*source + 9);
    }
    void compileShader(unsigned shader) {
        trigger(shaderKind.at(shader) == 0x8B31 ? Fault::vertexCompile : Fault::fragmentCompile);
    }
    void getShaderiv(unsigned shader, unsigned, int* value) {
        const Fault stage = shaderKind.at(shader) == 0x8B31 ? Fault::vertexCompile : Fault::fragmentCompile;
        *value = fault != stage && shaderVersion.at(shader) <= supportedVersion;
    }
    void deleteShader(unsigned shader) {
        assert(!attached.count(shader) && shaders.erase(shader) == 1);
        ++deletions;
    }
    unsigned createProgram() {
        if (fault == Fault::programCreate) return 0;
        const unsigned id = next++;
        programs.insert(id);
        ++programCreates;
        return id;
    }
    void attachShader(unsigned program, unsigned shader) {
        assert(programs.count(program) && shaders.count(shader));
        attached.insert(shader);
    }
    void detachShader(unsigned program, unsigned shader) {
        assert(programs.count(program) && attached.erase(shader) == 1);
    }
    void bindAttribLocation(unsigned program, unsigned, const char*) { assert(programs.count(program)); }
    void linkProgram(unsigned program) { assert(programs.count(program)); }
    void getProgramiv(unsigned program, unsigned, int* value) {
        assert(programs.count(program));
        *value = fault != Fault::link;
    }
    void deleteProgram(unsigned program) {
        assert(programs.erase(program) == 1);
        ++deletions;
    }
    int getUniformLocation(unsigned program, const char*) {
        assert(programs.count(program));
        return fault == Fault::uniform ? -1 : 0;
    }
    void genVertexArrays(int count, unsigned* id) {
        assert(count == 1);
        *id = fault == Fault::vaoCreate ? 0 : next++;
        if (*id) { vaos.insert(*id); ++vaoCreates; }
    }
    void deleteVertexArrays(int count, const unsigned* id) {
        assert(count == 1 && vaos.erase(*id) == 1);
        if (currentVao == *id) currentVao = 0;
        ++deletions;
    }
    void genBuffers(int count, unsigned* id) {
        assert(count == 1);
        *id = fault == Fault::vboCreate ? 0 : next++;
        if (*id) { buffers.insert(*id); ++bufferCreates; }
    }
    void deleteBuffers(int count, const unsigned* id) {
        assert(count == 1 && buffers.erase(*id) == 1);
        if (currentBuffer == *id) currentBuffer = 0;
        ++deletions;
    }
    void getIntegerv(unsigned key, int* value) {
        if (fault == Fault::stateQuery) { error = 0x500; return; }
        *value = int(key == 0x85B5 ? currentVao : currentBuffer);
    }
    void bindVertexArray(unsigned id) { currentVao = id; }
    void bindBuffer(unsigned, unsigned id) { currentBuffer = id; }
    void bufferData(unsigned, std::ptrdiff_t, const void*, unsigned) { trigger(Fault::upload); }
    void vertexAttribPointer(unsigned, int, unsigned, unsigned char, int, const void*) { trigger(Fault::attribute); }
    void enableVertexAttribArray(unsigned) { trigger(Fault::enable); }
    unsigned getError() { const unsigned result = error; error = 0; return result; }
};
