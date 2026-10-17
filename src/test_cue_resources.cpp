#include "cue_resources.h"
#include "test_cue_api.h"
#include <cstdio>

static void constructionFailures() {
    using F = FakeCueApi::Fault;
    const F faults[] = {F::vertexCreate, F::fragmentCreate, F::vertexCompile, F::fragmentCompile,
                        F::programCreate, F::link, F::uniform, F::vaoCreate, F::vboCreate,
                        F::stateQuery, F::upload, F::attribute, F::enable};
    for (const auto fault : faults) {
        FakeCueApi api;
        api.fault = fault;
        fp::CueResources resources;
        assert(!resources.prepare(api) && !resources.live() && api.empty());
        assert(api.currentVao == 91 && api.currentBuffer == 92);
        resources.retire(api);
        assert(api.empty());
    }
    puts("PASS: every shader/program/VAO/VBO construction failure retires partial allocations and restores bindings");
}
static void reuseAndFallback() {
    for (int version : {150, 130, 120}) {
        FakeCueApi api;
        api.supportedVersion = version;
        fp::CueResources resources;
        assert(resources.prepare(api) && resources.ready());
        assert(api.shaders.empty() && api.attached.empty());
        assert(api.programs.size() == 1 && api.vaos.size() == 1 && api.buffers.size() == 1);
        const unsigned created = api.next;
        assert(resources.prepare(api) && api.next == created);
        resources.retire(api);
        resources.retire(api);
        assert(!resources.live() && api.empty() && api.currentVao == 91 && api.currentBuffer == 92);
        assert(api.programCreates == 1 && api.vaoCreates == 1 && api.bufferCreates == 1);
    }
    puts("PASS: GLSL fallbacks leave only the arc resources; successful link retires shaders; repeated preparation reuses handles");
}
static void interruptedConstruction() {
    for (auto fault : {FakeCueApi::Fault::vertexCompile, FakeCueApi::Fault::upload}) {
        FakeCueApi api;
        api.exception = fault;
        fp::CueResources resources;
        bool threw = false;
        try { resources.prepare(api); }
        catch (const char*) { threw = true; }
        assert(threw && resources.live());
        api.exception = FakeCueApi::Fault::none;
        resources.retire(api);
        assert(api.empty() && api.currentVao == 91 && api.currentBuffer == 92);
    }
    puts("PASS: interrupted shader and buffer construction remain owned and can retire with the original bindings");
}
int main() {
    constructionFailures();
    reuseAndFallback();
    interruptedConstruction();
}
