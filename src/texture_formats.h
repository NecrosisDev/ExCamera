#pragma once
#include <cstdint>

namespace fp {
// Texture replacements (mod/texture_replace.inl): which block layout a replacement file must have, from the GL
// internal format the game itself uploads that texture with. The game resolves an image's format word to an
// entry of its runtime format table with 0x72790 (words with (word & 0x80100000) == 0x100000 match under mask
// 0x800CFFFF, others exactly; first match wins) and uploads with the entry's +4 internal format, or its +8
// sRGB form (0x72AD0). The second byte of the word alone does not decide it: 0x813BE601 is DXT1.
enum class Block { none = 0, bc1 = 1, bc3 = 3, bc4 = 4, bc5 = 5 };

inline Block blockForGlInternal(uint32_t internal) {
    switch (internal) {
    case 0x83F0: case 0x83F1: case 0x8C4C: case 0x8C4D: return Block::bc1; // S3TC DXT1 RGB/RGBA (+ sRGB)
    case 0x83F3: case 0x8C4F: return Block::bc3;                           // S3TC DXT5 (+ sRGB)
    case 0x8C70: case 0x8DBB: return Block::bc4;                           // LATC1 luminance, RGTC1 red
    case 0x8DBD: return Block::bc5;                                        // RGTC2 red-green
    default: return Block::none; // LATC2 (0x8C72) and uncompressed formats are left to the game
    }
}

// The compressed groups 0x766F0 appends to the runtime table, byte-exact from the game's data (S3TC 0x2AE290,
// LATC 0x2AE310, RGTC1 0x2AE380 when LATC is absent, RGTC2 0x2AE350), each {format word, GL internal format}.
// The mod resolves through the game's own table at runtime; this copy is for tests and the tools.
struct FormatEntry { uint32_t word, internal; };
inline constexpr FormatEntry s3tcFormats[] = {{0x813BC600, 0x83F0}, {0x813BE601, 0x83F1}, {0x823BC600, 0x83F0},
                                              {0x817BE608, 0x83F3}, {0x827BE608, 0x83F3}, {0x827BA408, 0x83F3}};
inline constexpr FormatEntry latcFormats[] = {{0x813B4200, 0x8C70}, {0x813B2008, 0x8C70}, {0x817B6208, 0x8C72}};
inline constexpr FormatEntry rgtc1Formats[] = {{0x813B4200, 0x8DBB}};
inline constexpr FormatEntry rgtc2Formats[] = {{0x817B8400, 0x8DBD}, {0x927B8400, 0x8DBD}};

inline bool formatWordMatches(uint32_t entry, uint32_t word) {
    if ((word & 0x80100000u) == 0x100000u) return (entry & 0x800CFFFFu) == (word & 0x800CFFFFu);
    return entry == word;
}
// The internal format the compressed groups give a word (0 none), with LATC present or not, as 0x766F0 orders them.
inline uint32_t compressedInternalFor(uint32_t word, bool latc) {
    for (const auto& e : s3tcFormats) if (formatWordMatches(e.word, word)) return e.internal;
    if (latc) for (const auto& e : latcFormats) if (formatWordMatches(e.word, word)) return e.internal;
    if (!latc) for (const auto& e : rgtc1Formats) if (formatWordMatches(e.word, word)) return e.internal;
    for (const auto& e : rgtc2Formats) if (formatWordMatches(e.word, word)) return e.internal;
    return 0;
}
} // namespace fp
