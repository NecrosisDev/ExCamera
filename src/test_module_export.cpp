// The launcher's production resolver reads only the destination's mapped image.
// Fixtures have distinct export layouts and malformed/unreadable boundaries.
#ifdef NDEBUG
#error "The tests check with assert(); build them without NDEBUG."
#endif
#include "module_export.h"
#include <cassert>
#include <cstdio>
#include <vector>

static void put16(std::vector<uint8_t>& image, size_t at, uint16_t v) {
    std::memcpy(image.data() + at, &v, sizeof v);
}
static void put32(std::vector<uint8_t>& image, size_t at, uint32_t v) {
    std::memcpy(image.data() + at, &v, sizeof v);
}
static std::vector<uint8_t> module(uint32_t entry) {
    std::vector<uint8_t> image(0x4000, 0);
    put16(image, 0, 0x5a4d);
    put32(image, 60, 0x80);
    put32(image, 0x80, 0x4550);
    put16(image, 0x84, 0x8664);
    put16(image, 0x86, 2);
    put16(image, 0x94, 240);
    put16(image, 0x98, 0x20b);
    put32(image, 0x98 + 56, uint32_t(image.size()));
    put32(image, 0x98 + 108, 16);
    put32(image, 0x98 + 112, 0x2000);
    put32(image, 0x98 + 116, 0x100);
    for (unsigned s = 0; s < 2; ++s) {
        const size_t at = 0x188 + s * 40;
        put32(image, at + 8, 0x1000);
        put32(image, at + 12, 0x1000 + s * 0x1000);
        put32(image, at + 36, s ? 0x40000040 : 0x60000020);
    }
    put32(image, 0x2014, 1);
    put32(image, 0x2018, 1);
    put32(image, 0x201c, 0x2040);
    put32(image, 0x2020, 0x2050);
    put32(image, 0x2024, 0x2060);
    put32(image, 0x2040, entry);
    put32(image, 0x2050, 0x2070);
    std::memcpy(image.data() + 0x2070, "enable", 7);
    return image;
}
static uint32_t resolve(const std::vector<uint8_t>& image, const char* name = "enable",
                        uint32_t unreadable = UINT32_MAX) {
    return fp::loadedExportRva(uint32_t(image.size()), name, [&](uint32_t at, void* out, size_t n) {
        assert(at <= image.size() && n <= image.size() - at); // owner enforces every read boundary
        if (at <= unreadable && n > unreadable - at) return false;
        std::memcpy(out, image.data() + at, n);
        return true;
    });
}
int main() {
    const auto loaded = module(0x1100), newerFile = module(0x1180);
    assert(resolve(loaded) == 0x1100 && resolve(newerFile) == 0x1180);
    assert(resolve(loaded, "disable") == 0 && resolve(loaded, "enabl") == 0);
    puts("PASS: each loaded version supplies its own export address; names match exactly");
    for (uint32_t bad : {0u, 0x2070u, 0x2100u, 0x4000u, UINT32_MAX}) {
        auto image = loaded;
        put32(image, 0x2040, bad);
        assert(resolve(image) == 0);
    }
    puts("PASS: absent, forwarded, non-executable and outside-image exports are refused");
    for (const auto field : {0x201cu, 0x2020u, 0x2024u, 0x2050u}) {
        auto image = loaded;
        put32(image, field, 0x3fff);
        assert(resolve(image) == 0);
    }
    auto image = loaded;
    put16(image, 0x2060, 1);
    assert(resolve(image) == 0);
    image = loaded;
    put32(image, 0x2018, UINT32_MAX);
    assert(resolve(image) == 0);
    puts("PASS: truncated arrays, strings, invalid ordinals and oversized counts are refused");
    for (size_t size = 0; size < 0x110; ++size) {
        image.assign(loaded.begin(), loaded.begin() + size);
        assert(resolve(image) == 0);
    }
    for (uint32_t bad : {0x3fffu, UINT32_MAX}) {
        image = loaded;
        put32(image, 60, bad);
        assert(resolve(image) == 0);
    }
    image = loaded;
    put32(image, 0x98 + 108, 0);
    assert(resolve(image) == 0);
    image = loaded;
    put16(image, 0x94, 119);
    assert(resolve(image) == 0);
    image = loaded;
    put32(image, 0x188 + 8, UINT32_MAX);
    assert(resolve(image) == 0);
    assert(resolve(loaded, "enable", 0x2040) == 0);
    puts("PASS: malformed PE headers, wrapped section ranges and failed reads are refused");
    puts("MODULE EXPORT TESTS PASSED");
}
