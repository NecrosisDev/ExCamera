#pragma once
#include <cstddef>

namespace fp {
// Mod message text for the engine log (state.inl modMessage). The engine keeps it as an 8-bit FPC AnsiString;
// nothing on its way formats it (FUN_100025D80 and FUN_1000259A0 only concatenate, 0x84E0), and the on-screen
// lines (measured by 0x28750, drawn by 0x27FA0) index the font's glyph widths by byte, treating only CR, LF and
// space specially: CR and LF break the line, so '%', '[', ']' and '|' are shown as they are. One line of
// ASCII: dashes and quotes simplified, line breaks and tabs as spaces, anything else '?'. Returns the length.
inline size_t engineAscii(const wchar_t* text, char* out, size_t size) {
    if (!size) return 0;
    size_t n = 0;
    for (const wchar_t* c = text; c && *c && n + 1 < size; ++c) {
        wchar_t ch = *c;
        if (ch == 0x2013 || ch == 0x2014) ch = L'-';
        else if (ch == 0x2018 || ch == 0x2019) ch = L'\'';
        else if (ch == 0x201C || ch == 0x201D) ch = L'"';
        else if (ch == L'\n' || ch == L'\r' || ch == L'\t') ch = L' ';
        out[n++] = ch >= 32 && ch < 127 ? char(ch) : '?';
    }
    out[n] = 0;
    return n;
}
} // namespace fp
