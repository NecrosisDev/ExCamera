// The status/calibration JSON writer (status_json.h). Portable.
#ifdef NDEBUG
#error "The tests check with assert(); build them without NDEBUG."
#endif
#include "status_json.h"
#include <cassert>
#include <cstdio>
#include <string>

static std::string written(void (*fill)(fp::JsonOut&)) {
    FILE* f = tmpfile();
    assert(f);
    {
        fp::JsonOut j(f);
        fill(j);
        j.finish();
    }
    std::string s(size_t(ftell(f)), '\0');
    rewind(f);
    assert(fread(&s[0], 1, s.size(), f) == s.size());
    fclose(f);
    return s;
}

int main() {
    // The launcher reads these by plain text: the shapes must stay exactly so.
    assert(written([](fp::JsonOut& j) {
               j.unum("pid", 4242).boolean("installed", true).num("revision", 9);
           }) == "{\"pid\":4242,\"installed\":true,\"revision\":9}\n");
    puts("PASS: fields in order, no spaces: the launcher's text matches keep working");
    assert(written([](fp::JsonOut& j) {
               const float v[3] = {1.f, 2.5f, -3.f};
               j.reals("target", v, 3).real("yaw", 0.1f).fixed("hold", 1.25, 3).real("near", 1.0 / 3.0, 3);
           }) == "{\"target\":[1,2.5,-3],\"yaw\":0.100000001,\"hold\":1.250,\"near\":0.333}\n");
    puts("PASS: numbers keep the old formats (%.9g floats, fixed decimals, short reals)");
    assert(
        written([](fp::JsonOut& j) {
            j.object("hand")
                .num("count", 2)
                .array("names")
                .str(nullptr, "17RHand")
                .str(nullptr, "x")
                .end()
                .end();
            j.null("rearm_key").hex("root", 0xabc000);
        }) ==
        "{\"hand\":{\"count\":2,\"names\":[\"17RHand\",\"x\"]},\"rearm_key\":null,\"root\":\"0xabc000\"}\n");
    puts("PASS: nested objects and arrays, null and hex values");
    assert(written([](fp::JsonOut& j) {
               j.str("level", "a\"b\\c\nd").str("buf", "0123456789ABCDEFGH", 16);
           }) == "{\"level\":\"a\\\"b\\\\c\\u000ad\",\"buf\":\"0123456789ABCDEF\"}\n");
    puts("PASS: strings escaped (a level name with a quote no longer breaks the file); buffers capped");
    assert(written([](fp::JsonOut& j) { j.object("a").array("b").num(nullptr, 1); }) ==
           "{\"a\":{\"b\":[1]}}\n");
    assert(written([](fp::JsonOut&) {}) == "{}\n");
    puts("PASS: finish closes whatever is open; an empty object is {}");
    puts("ALL STATUS JSON TESTS PASSED");
}
