#include "first_person.cpp"
#include "test_cue_api.h"
#include <cassert>

namespace cue_fixture {
static HGLRC current = nullptr;
static HDC dc = nullptr;
static std::map<HGLRC, FakeCueApi> contexts;
static bool failSwitch = false, failDelete = false, reuseDuringDelete = false, failApi = false;
static unsigned apiLoads = 0;
static HANDLE deleteEntered = nullptr, deleteResume = nullptr;
static HGLRC rc(unsigned id) { return reinterpret_cast<HGLRC>(uintptr_t(id)); }
static HDC surface(unsigned id) { return reinterpret_cast<HDC>(uintptr_t(id + 100)); }
static FakeCueApi& api() { assert(current && contexts.count(current)); return contexts.at(current); }
static HGLRC WINAPI currentContext() { return current; }
static HDC WINAPI currentDC() { return dc; }
static BOOL WINAPI makeCurrent(HDC nextDC, HGLRC context) {
    if (failSwitch) {
        current = nullptr;
        dc = nullptr;
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    assert(!context || contexts.count(context));
    current = context;
    dc = context ? nextDC : nullptr;
    return TRUE;
}
static CueContext* prepare() {
    AcquireSRWLockExclusive(&cueLock);
    auto* result = prepareCue();
    ReleaseSRWLockExclusive(&cueLock);
    return result;
}
static BOOL WINAPI deleteContext(HGLRC context) {
    if (deleteEntered) {
        SetEvent(deleteEntered);
        assert(WaitForSingleObject(deleteResume, 2000) == WAIT_OBJECT_0);
        SetLastError(ERROR_BUSY);
        return FALSE;
    }
    if (failDelete) { SetLastError(ERROR_BUSY); return FALSE; }
    assert(contexts.erase(context) == 1);
    if (current == context) { current = nullptr; dc = nullptr; }
    if (reuseDuringDelete) {
        contexts.emplace(context, FakeCueApi{});
        current = context;
        dc = surface(unsigned(uintptr_t(context)));
        assert(!prepare()); // Same numeric handle cannot inherit the dying record.
    }
    SetLastError(1234);
    return TRUE;
}
static GLuint APIENTRY createShader(GLenum v) { return api().createShader(v); }
static void APIENTRY shaderSource(GLuint a, GLsizei b, const gl::GLchar* const* c, const GLint* d) { api().shaderSource(a, b, c, d); }
static void APIENTRY compileShader(GLuint a) { api().compileShader(a); }
static void APIENTRY getShaderiv(GLuint a, GLenum b, GLint* c) { api().getShaderiv(a, b, c); }
static void APIENTRY deleteShader(GLuint a) { api().deleteShader(a); }
static GLuint APIENTRY createProgram() { return api().createProgram(); }
static void APIENTRY attachShader(GLuint a, GLuint b) { api().attachShader(a, b); }
static void APIENTRY detachShader(GLuint a, GLuint b) { api().detachShader(a, b); }
static void APIENTRY bindAttribLocation(GLuint a, GLuint b, const gl::GLchar* c) { api().bindAttribLocation(a, b, c); }
static void APIENTRY linkProgram(GLuint a) { api().linkProgram(a); }
static void APIENTRY getProgramiv(GLuint a, GLenum b, GLint* c) { api().getProgramiv(a, b, c); }
static void APIENTRY deleteProgram(GLuint a) { api().deleteProgram(a); }
static GLint APIENTRY getUniformLocation(GLuint a, const gl::GLchar* b) { return api().getUniformLocation(a, b); }
static void APIENTRY genVertexArrays(GLsizei a, GLuint* b) { api().genVertexArrays(a, b); }
static void APIENTRY deleteVertexArrays(GLsizei a, const GLuint* b) { api().deleteVertexArrays(a, b); }
static void APIENTRY bindVertexArray(GLuint a) { api().bindVertexArray(a); }
static void APIENTRY genBuffers(GLsizei a, GLuint* b) { api().genBuffers(a, b); }
static void APIENTRY deleteBuffers(GLsizei a, const GLuint* b) { api().deleteBuffers(a, b); }
static void APIENTRY bindBuffer(GLenum a, GLuint b) { api().bindBuffer(a, b); }
static void APIENTRY bufferData(GLenum a, gl::GLsizeiptr b, const void* c, GLenum d) { api().bufferData(a, b, c, d); }
static void APIENTRY vertexAttribPointer(GLuint a, GLint b, GLenum c, GLboolean d, GLsizei e, const void* f) { api().vertexAttribPointer(a, b, c, d, e, f); }
static void APIENTRY enableVertexAttribArray(GLuint a) { api().enableVertexAttribArray(a); }
static void APIENTRY getIntegerv(GLenum a, GLint* b) { api().getIntegerv(a, b); }
static GLenum APIENTRY getError() { return api().getError(); }
static bool loadApi(gl::Api& g) {
    ++apiLoads;
    if (failApi) return false;
    g.createShader = createShader;
    g.shaderSource = shaderSource;
    g.compileShader = compileShader;
    g.getShaderiv = getShaderiv;
    g.deleteShader = deleteShader;
    g.createProgram = createProgram;
    g.attachShader = attachShader;
    g.detachShader = detachShader;
    g.bindAttribLocation = bindAttribLocation;
    g.linkProgram = linkProgram;
    g.getProgramiv = getProgramiv;
    g.deleteProgram = deleteProgram;
    g.getUniformLocation = getUniformLocation;
    g.genVertexArrays = genVertexArrays;
    g.deleteVertexArrays = deleteVertexArrays;
    g.bindVertexArray = bindVertexArray;
    g.genBuffers = genBuffers;
    g.deleteBuffers = deleteBuffers;
    g.bindBuffer = bindBuffer;
    g.bufferData = bufferData;
    g.vertexAttribPointer = vertexAttribPointer;
    g.enableVertexAttribArray = enableVertexAttribArray;
    g.getIntegerv = getIntegerv;
    g.getError = getError;
    return true;
}
static void reset() {
    assert(awaitCueRetirement(0));
    current = nullptr;
    dc = nullptr;
    contexts.clear();
    failSwitch = failDelete = reuseDuringDelete = failApi = false;
    apiLoads = 0;
    cueCurrentContext = currentContext;
    cueCurrentDC = currentDC;
    originalCueMakeCurrent = makeCurrent;
    originalCueDeleteContext = deleteContext;
    cueApiLoader = loadApi;
    InterlockedExchange(&stopping, 0);
}
static void add(unsigned id) { contexts.emplace(rc(id), FakeCueApi{}); }
static void bind(unsigned id) { assert(cueMakeCurrent(surface(id), rc(id))); }
static void unbind() { assert(cueMakeCurrent(nullptr, nullptr)); assert(awaitCueRetirement(0)); }
static void reuseAndSwitch() {
    reset(); add(1); add(2); bind(1);
    auto* a = prepare();
    assert(a && a->resources.ready());
    const unsigned created = api().next;
    assert(prepare() == a && api().next == created && apiLoads == 1);
    bind(1);
    assert(prepare() == a && api().next == created && apiLoads == 1);
    bind(2);
    assert(contexts.at(rc(1)).empty() && prepare());
    bind(1);
    assert(contexts.at(rc(2)).empty() && prepare() && api().next > created);
    unbind();
    assert(contexts.at(rc(1)).empty() && contexts.at(rc(2)).empty());
    puts("PASS: same-context frames and unchanged bindings reuse resources; A/B/A switches retire before recreating");
}
static void failedSwitches() {
    reset(); add(1); add(2); bind(1); assert(prepare());
    failSwitch = true;
    assert(!cueMakeCurrent(surface(2), rc(2)) && GetLastError() == ERROR_INVALID_HANDLE);
    assert(!current && contexts.at(rc(1)).empty() && awaitCueRetirement(0));
    failSwitch = false; bind(1); assert(prepare());
    failSwitch = true;
    assert(!cueMakeCurrent(surface(1), rc(1)) && !current);
    InterlockedExchange(&stopping, 1);
    assert(!awaitCueRetirement(0)); // A failed no-op bind may unbind the context too.
    failSwitch = false; bind(1);
    presentCue(fp::CueSide::none, 0, 0, 0, 0);
    assert(contexts.at(rc(1)).empty() && awaitCueRetirement(0));
    puts("PASS: failed switches do not leak the departing resources; failed unchanged binds retain retirement ownership");
}
static void missingApi() {
    reset(); add(1); bind(1);
    failApi = true;
    assert(!prepare() && !prepare() && apiLoads == 1 && api().empty());
    unbind();
    failApi = false;
    bind(1);
    assert(prepare() && apiLoads == 2);
    unbind();
    puts("PASS: missing GL entry points allocate nothing and are retried only after a new context binding");
}
static void failedDrawRetiresOnce() {
    reset(); add(1); bind(1); assert(prepare());
    failCurrentCue();
    assert(api().empty() && !prepare() && !prepare() && apiLoads == 1);
    unbind();
    bind(1);
    assert(prepare() && apiLoads == 2);
    unbind();
    puts("PASS: interrupted draws retire resources and suppress repeated attempts until the context is rebound");
}
static void deletionAndReuse() {
    reset(); add(1); bind(1); assert(prepare());
    failDelete = true;
    assert(!cueDeleteContext(rc(1)) && GetLastError() == ERROR_BUSY);
    assert(current == rc(1) && contexts.at(rc(1)).empty());
    assert(prepare() && apiLoads == 2);
    failDelete = false; reuseDuringDelete = true;
    assert(cueDeleteContext(rc(1)) && GetLastError() == 1234);
    assert(current == rc(1) && contexts.at(rc(1)).empty() && !findCueContext(rc(1)));
    assert(prepare() && apiLoads == 3);
    reuseDuringDelete = false;
    unbind();
    puts("PASS: failed deletion leaves a usable context; successful deletion and immediate HGLRC reuse load fresh ownership");
}
static void foreignCurrentAndStop() {
    reset(); add(1); add(2); bind(1); assert(prepare());
    // An unobserved extension switch is outside the core WGL contract. Never
    // issue deletion through a different current context to hide that gap.
    current = rc(2); dc = surface(2);
    assert(prepare());
    const unsigned currentObjects = api().next;
    failDelete = true;
    assert(!cueDeleteContext(rc(1)) && findCueContext(rc(1))->resources.live());
    assert(api().next == currentObjects);
    InterlockedExchange(&stopping, 1);
    presentCue(fp::CueSide::none, 0, 0, 0, 0);
    assert(contexts.at(rc(2)).empty() && !awaitCueRetirement(0));
    failDelete = false;
    assert(cueDeleteContext(rc(1)) && awaitCueRetirement(0));
    puts("PASS: foreign-current deletion never calls GL in the wrong namespace; stop remains pending until ownership retires");
}
static DWORD WINAPI blockedDelete(void*) { cueDeleteContext(rc(3)); return 0; }
static void inFlightStop() {
    reset();
    InterlockedExchange(&stopping, 1);
    deleteEntered = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    deleteResume = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    assert(deleteEntered && deleteResume);
    HANDLE thread = CreateThread(nullptr, 0, blockedDelete, nullptr, 0, nullptr);
    assert(thread && WaitForSingleObject(deleteEntered, 2000) == WAIT_OBJECT_0);
    assert(!awaitCueRetirement(0));
    SetEvent(deleteResume);
    assert(WaitForSingleObject(thread, 2000) == WAIT_OBJECT_0 && awaitCueRetirement(0));
    CloseHandle(thread); CloseHandle(deleteEntered); CloseHandle(deleteResume);
    deleteEntered = deleteResume = nullptr;
    puts("PASS: shutdown cannot complete while a WGL lifetime callback is still in flight");
}
} // namespace cue_fixture
int main() {
    cue_fixture::reuseAndSwitch();
    cue_fixture::failedSwitches();
    cue_fixture::missingApi();
    cue_fixture::failedDrawRetiresOnce();
    cue_fixture::deletionAndReuse();
    cue_fixture::foreignCurrentAndStop();
    cue_fixture::inFlightStop();
}
