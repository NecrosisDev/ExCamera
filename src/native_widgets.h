#pragma once
// Native presentation, with no HWND, UI thread, renderer or cursor ownership.
#include "settings_model.h"
#include "game_structs.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <utility>

namespace panel {
struct NativeElement {
    uint16_t kind = 1, reserved = 0;
    int16_t x = 0, y = 0, width = 0, height = 0, style = 0, group = 0;
    const char* text = nullptr;
    uint32_t appearance = 0x403300, flags = 0;
    uintptr_t owner = 0;
    void (*action)(NativeElement*) = nullptr;
    uintptr_t handler = 0;
    float position = 0;
    uint32_t spare = 0;
    uintptr_t help = 0; // +0x40: kind-four slider also uses this as its knob-spacing field
    int64_t identity = 0; // +0x48: native kind-five TEXT COLOUR POINTER, not an element ID!
    uint64_t data[3]{}; // +0x50: kind-four slider's maximum discrete position
};
static_assert(sizeof(NativeElement) == fp::game::nativeElement::stride);
static_assert(offsetof(NativeElement, x) == fp::game::nativeElement::x);
static_assert(offsetof(NativeElement, y) == fp::game::nativeElement::y);
static_assert(offsetof(NativeElement, width) == fp::game::nativeElement::width);
static_assert(offsetof(NativeElement, height) == fp::game::nativeElement::height);
static_assert(offsetof(NativeElement, style) == fp::game::nativeElement::style);
static_assert(offsetof(NativeElement, group) == fp::game::nativeElement::group);
static_assert(offsetof(NativeElement, text) == fp::game::nativeElement::text);
static_assert(offsetof(NativeElement, appearance) == fp::game::nativeElement::appearance);
static_assert(offsetof(NativeElement, flags) == fp::game::nativeElement::flags);
static_assert(offsetof(NativeElement, owner) == fp::game::nativeElement::owner);
static_assert(offsetof(NativeElement, action) == fp::game::nativeElement::action);
static_assert(offsetof(NativeElement, handler) == fp::game::nativeElement::handler);
static_assert(offsetof(NativeElement, help) == fp::game::nativeElement::help);
static_assert(offsetof(NativeElement, identity) == 0x48, "Native colour-pointer ABI, not an arbitrary identity");
static_assert(offsetof(NativeElement, data) == 0x50, "Native slider discrete-step ABI");
struct NativeText {
    int64_t references = -1, length = 0;
    char bytes[512]{};
    void set(const std::wstring& v) {
        const int n = WideCharToMultiByte(1252, WC_NO_BEST_FIT_CHARS, v.c_str(), -1, bytes, int(sizeof bytes),
                                          "?", nullptr);
        if (!n) bytes[0] = 0;
        length = n ? n - 1 : 0;
    }
};
static_assert(offsetof(NativeText, bytes) == fp::game::nativeString::referenceHeader);
static_assert(offsetof(NativeText, bytes) - offsetof(NativeText, length) ==
              fp::game::nativeString::lengthHeader);
} // namespace panel
