#pragma once

// Per-HGLRC OpenGL entry-point cache for hot draw paths (headwear, stone skin, wound skins).
// wglGetProcAddress acquires driver/ICD locks when called per draw; caching per HGLRC reduces
// repeated lookups on the render thread to a single pointer comparison while leaving headless
// test harnesses (wglGetCurrentContext() == nullptr) uncached so mock loaders run fresh.
namespace fp::glproc {

template <typename Table, typename Loader>
inline bool loadForCurrentContext(Table& out, Loader&& loader) {
    const HGLRC ctx = wglGetCurrentContext();
    if (!ctx) return loader(out);
    struct Slot {
        HGLRC context = nullptr;
        Table table{};
    };
    thread_local Slot slots[4]{};
    thread_local unsigned nextSlot = 0;
    for (const Slot& s : slots) {
        if (s.context == ctx) {
            out = s.table;
            return true;
        }
    }
    Table loaded{};
    if (!loader(loaded)) return false;
    slots[nextSlot] = {ctx, loaded};
    nextSlot = (nextSlot + 1) % 4;
    out = loaded;
    return true;
}

} // namespace fp::glproc
