#pragma once
// Exanima's own UI art for the F8 window, so it matches the game's styling and fonts. Read by the mod
// itself from the installed game's Resource.rpk (read only, never written). Formats:
// fontbase24r.rfi (alpha, 64 px cells, 16 a row,
// Latin-1 32-255), titlefont.rfi (lum+alpha, A-Z and '+', 128 px cells, 4 a row), guitex.rfi (the
// stone, DXT1), guiskin.rfi (alpha ornaments). Nothing here touches the game's memory.
#include <windows.h>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <cctype>
#include <map>
#include <memory>
#include <string>
#include <vector>
#pragma comment(lib, "msimg32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")

namespace uiart {

struct Image {
    int w = 0, h = 0;
    std::vector<uint8_t> rgba;
    bool ok() const { return w > 0 && h > 0 && rgba.size() == size_t(w) * size_t(h) * 4; }
};

// The game folder: the running executable's (Exanima.exe), or one a test sets.
inline std::wstring& gameFolderOverride() {
    static std::wstring f;
    return f;
}
inline std::wstring gameFolder() {
    if (!gameFolderOverride().empty()) return gameFolderOverride();
    wchar_t p[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, p, MAX_PATH);
    if (!n || n >= MAX_PATH) return {};
    std::wstring s(p);
    const size_t at = s.find_last_of(L'\\');
    return at == std::wstring::npos ? std::wstring() : s.substr(0, at);
}
// One top-level member of an .rpk: magic 01 0C BF AF, u32 table bytes, 32-byte entries
// {name16, u32 offset after the table, u32 size, 8 bytes}.
inline bool readMember(const std::wstring& archive, const char* name, std::vector<uint8_t>& out) {
    out.clear();
    HANDLE f =
        CreateFileW(archive.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    auto readAt = [&](uint64_t at, void* buf, DWORD n) {
        LARGE_INTEGER li;
        li.QuadPart = LONGLONG(at);
        DWORD got = 0;
        return SetFilePointerEx(f, li, nullptr, FILE_BEGIN) && ReadFile(f, buf, n, &got, nullptr) && got == n;
    };
    bool ok = false;
    uint8_t head[8];
    if (readAt(0, head, 8) && head[0] == 0x01 && head[1] == 0x0C && head[2] == 0xBF && head[3] == 0xAF) {
        uint32_t tableBytes;
        memcpy(&tableBytes, head + 4, 4);
        if (tableBytes % 32 == 0 && tableBytes < (16u << 20)) {
            std::vector<uint8_t> table(tableBytes);
            if (readAt(8, table.data(), tableBytes))
                for (size_t o = 0; o + 32 <= table.size(); o += 32) {
                    char entry[17]{};
                    memcpy(entry, table.data() + o, 16);
                    if (_stricmp(entry, name)) continue;
                    uint32_t rel, size;
                    memcpy(&rel, table.data() + o + 16, 4);
                    memcpy(&size, table.data() + o + 20, 4);
                    if (size > (64u << 20)) break;
                    out.resize(size);
                    ok = readAt(8ull + tableBytes + rel, out.data(), size);
                    break;
                }
        }
    }
    CloseHandle(f);
    if (!ok) out.clear();
    return ok;
}
// Run-length packing (rfi.py rle_unpack; the game's threshold 0xC2).
inline bool rleUnpack(const uint8_t* src, size_t n, size_t size, size_t ps, std::vector<uint8_t>& out) {
    out.clear();
    out.reserve(size);
    size_t i = 0;
    const unsigned lim = ps == 1 ? 0x101 - 0xC2 : 0x100 - 0xC2;
    while (out.size() < size) {
        if (i >= n) return false;
        const unsigned b = src[i++];
        if (b > lim) {
            const size_t copies = ps == 1 ? b - lim + 2 : b - lim + 1;
            if (i + ps > n) return false;
            for (size_t k = 0; k < copies; ++k) out.insert(out.end(), src + i, src + i + ps);
            i += ps;
        } else {
            const size_t bytes = (b + 1) * ps;
            if (i + bytes > n) return false;
            out.insert(out.end(), src + i, src + i + bytes);
            i += bytes;
        }
    }
    out.resize(size);
    return true;
}
// Level 0, face 0, as RGBA8 upright (the files are stored bottom row first). The UI formats only:
// ALPHA, LUM, LUMALPHA (raw), RGB/RGBA (raw), DXT1 (bc1). Anything else: false.
inline bool decodeRfi(const std::vector<uint8_t>& d, Image& img) {
    img = Image();
    if (d.size() < 32) return false;
    uint32_t f[8];
    memcpy(f, d.data(), 32);
    if (f[0] != 0x1D2D3DC6u) return false;
    const int w = int(f[1]), h = int(f[2]);
    const uint32_t code = f[4], flags = f[6], unpacked = f[7];
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096) return false;
    size_t ps = 1;
    bool alphaOnly = false, la = false, rgba = false, bc1 = false;
    switch (code) {
    case 0x01002008:
        ps = 1;
        alphaOnly = true;
        break; // ALPHA
    case 0x01004200:
    case 0x01114200:
        ps = 1;
        break; // LUM
    case 0x01006208:
        ps = 2;
        la = true;
        break; // LUMALPHA
    case 0x0100C600:
    case 0x0111C600:
    case 0x1111C600:
        ps = 3;
        break;
    case 0x0100E608:
    case 0x1111E608:
        ps = 4;
        rgba = true;
        break;
    case 0x813BC600:
    case 0x823BC600:
        bc1 = true;
        break; // DXT1
    default:
        return false;
    }
    const size_t level0 = bc1 ? size_t((w + 3) / 4) * size_t((h + 3) / 4) * 8 : size_t(w) * size_t(h) * ps;
    if (unpacked < level0 || unpacked > (256u << 20)) return false;
    std::vector<uint8_t> body;
    if (flags & 0x40000000u) {
        const uint32_t low = code & 0x800CFFFFu;
        const size_t rps = low == 0xC600 ? 3 : low == 0xE608 ? 4 : (low == 0x6208 || low == 0x8400) ? 2 : 1;
        if (!rleUnpack(d.data() + 32, d.size() - 32, level0, rps, body)) return false; // level 0 only
    } else {
        if (d.size() < 32 + level0) return false;
        body.assign(d.begin() + 32, d.begin() + 32 + ptrdiff_t(level0));
    }
    img.w = w;
    img.h = h;
    img.rgba.assign(size_t(w) * h * 4, 0);
    auto put = [&](int x, int y, uint8_t r, uint8_t g, uint8_t b,
                   uint8_t a) { // file row y -> upright row h-1-y
        uint8_t* o = img.rgba.data() + (size_t(h - 1 - y) * w + x) * 4;
        o[0] = r;
        o[1] = g;
        o[2] = b;
        o[3] = a;
    };
    if (!bc1) {
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const uint8_t* p = body.data() + (size_t(y) * w + x) * ps;
                if (alphaOnly)
                    put(x, y, 255, 255, 255, p[0]);
                else if (la)
                    put(x, y, p[0], p[0], p[0], p[1]);
                else if (ps == 1)
                    put(x, y, p[0], p[0], p[0], 255);
                else
                    put(x, y, p[0], p[1], p[2], rgba ? p[3] : 255);
            }
        return true;
    }
    auto c565 = [](uint16_t v, uint8_t* c) {
        c[0] = uint8_t(((v >> 11) & 31) * 255 / 31);
        c[1] = uint8_t(((v >> 5) & 63) * 255 / 63);
        c[2] = uint8_t((v & 31) * 255 / 31);
    };
    size_t off = 0;
    for (int by = 0; by < (h + 3) / 4; ++by)
        for (int bx = 0; bx < (w + 3) / 4; ++bx) {
            uint16_t c0, c1;
            uint32_t bits;
            memcpy(&c0, body.data() + off, 2);
            memcpy(&c1, body.data() + off + 2, 2);
            memcpy(&bits, body.data() + off + 4, 4);
            off += 8;
            uint8_t pal[4][4];
            c565(c0, pal[0]);
            c565(c1, pal[1]);
            pal[0][3] = pal[1][3] = 255;
            for (int i = 0; i < 3; ++i) {
                if (c0 > c1) {
                    pal[2][i] = uint8_t((2 * pal[0][i] + pal[1][i]) / 3);
                    pal[3][i] = uint8_t((pal[0][i] + 2 * pal[1][i]) / 3);
                } else {
                    pal[2][i] = uint8_t((pal[0][i] + pal[1][i]) / 2);
                    pal[3][i] = 0;
                }
            }
            pal[2][3] = pal[3][3] = 255;
            for (int i = 0; i < 16; ++i) {
                const int x = bx * 4 + (i & 3), y = by * 4 + (i >> 2);
                if (x < w && y < h) {
                    const uint8_t* c = pal[(bits >> (2 * i)) & 3];
                    put(x, y, c[0], c[1], c[2], c[3]);
                }
            }
        }
    return true;
}
inline bool loadTexture(const char* member, Image& img) {
    std::vector<uint8_t> d;
    const std::wstring folder = gameFolder();
    return !folder.empty() && readMember(folder + L"\\Resource.rpk", member, d) && decodeRfi(d, img);
}

// A box-filtered copy at scale s (0 < s <= 1), for drawing glyphs and the logo small without shimmer.
// the game's own menu cursor (Resource.rpk menucursor.rfi, 128 x 128, tip at the top left) as a
// Windows cursor of `size` pixels, so the panel shows the same pointer as the game's menus.
inline Image shrink(const Image& src, int x0, int y0, int w, int h, float s);
inline HCURSOR gameCursor(int size) {
    Image src;
    if (!loadTexture("menucursor.rfi", src) || !src.ok()) return nullptr;
    const Image im = shrink(src, 0, 0, src.w, src.h, float(size) / float(src.w));
    if (!im.ok()) return nullptr;
    BITMAPV5HEADER bi{};
    bi.bV5Size = sizeof bi;
    bi.bV5Width = im.w;
    bi.bV5Height = -im.h;
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000;
    bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask = 0x000000FF;
    bi.bV5AlphaMask = 0xFF000000;
    HDC dc = GetDC(nullptr);
    void* bits = nullptr;
    HBITMAP color = CreateDIBSection(dc, (BITMAPINFO*)&bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, dc);
    if (!color || !bits) {
        if (color) DeleteObject(color);
        return nullptr;
    }
    uint8_t* o = (uint8_t*)bits;
    for (size_t i = 0; i < size_t(im.w) * im.h; ++i) {
        const uint8_t* p = im.rgba.data() + i * 4;
        o[i * 4] = p[2];
        o[i * 4 + 1] = p[1];
        o[i * 4 + 2] = p[0];
        o[i * 4 + 3] = p[3];
    }
    HBITMAP mask = CreateBitmap(im.w, im.h, 1, 1, nullptr);
    ICONINFO ii{};
    ii.fIcon = FALSE;
    ii.xHotspot = DWORD(im.w * 6 / 128);
    ii.yHotspot = DWORD(im.h * 6 / 128);
    ii.hbmColor = color;
    ii.hbmMask = mask;
    HCURSOR c = (HCURSOR)CreateIconIndirect(&ii);
    DeleteObject(color);
    DeleteObject(mask);
    return c;
}
inline Image shrink(const Image& src, int x0, int y0, int w, int h, float s) {
    Image out;
    out.w = (std::max)(1, int(w * s + 0.5f));
    out.h = (std::max)(1, int(h * s + 0.5f));
    out.rgba.assign(size_t(out.w) * out.h * 4, 0);
    for (int y = 0; y < out.h; ++y)
        for (int x = 0; x < out.w; ++x) {
            const int sx0 = x0 + int(x / s), sy0 = y0 + int(y / s),
                      sx1 = (std::max)(sx0 + 1, x0 + int((x + 1) / s)),
                      sy1 = (std::max)(sy0 + 1, y0 + int((y + 1) / s));
            double acc[4] = {0, 0, 0, 0};
            int n = 0;
            for (int yy = sy0; yy < sy1 && yy < src.h; ++yy)
                for (int xx = sx0; xx < sx1 && xx < src.w; ++xx) {
                    const uint8_t* p = src.rgba.data() + (size_t(yy) * src.w + xx) * 4;
                    const double a = p[3] / 255.0;
                    acc[0] += p[0] * a;
                    acc[1] += p[1] * a;
                    acc[2] += p[2] * a;
                    acc[3] += p[3];
                    ++n;
                }
            if (!n) continue;
            uint8_t* o = out.rgba.data() + (size_t(y) * out.w + x) * 4;
            const double a = acc[3] / n;
            o[3] = uint8_t(a + 0.5);
            for (int c = 0; c < 3; ++c)
                o[c] = a > 0 ? uint8_t((std::min)(255.0, acc[c] / n / (a / 255.0) + 0.5)) : 0;
        }
    return out;
}

// A premultiplied 32-bit DIB of an image, tinted (multiplied) by a colour, for AlphaBlend.
struct Bitmap {
    HDC dc = nullptr;
    HBITMAP bmp = nullptr;
    HGDIOBJ old = nullptr;
    int w = 0, h = 0;
    Bitmap() = default;
    Bitmap(const Bitmap&) = delete;
    Bitmap& operator=(const Bitmap&) = delete;
    ~Bitmap() { reset(); }
    void reset() {
        if (dc) {
            SelectObject(dc, old);
            DeleteDC(dc);
        }
        if (bmp) DeleteObject(bmp);
        dc = nullptr;
        bmp = nullptr;
        old = nullptr;
        w = h = 0;
    }
    bool make(const Image& img, COLORREF tint = RGB(255, 255, 255)) {
        reset();
        if (!img.ok()) return false;
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = img.w;
        bi.bmiHeader.biHeight = -img.h;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        bmp = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!bmp || !bits) return false;
        dc = CreateCompatibleDC(nullptr);
        old = SelectObject(dc, bmp);
        w = img.w;
        h = img.h;
        const unsigned tr = GetRValue(tint), tg = GetGValue(tint), tb = GetBValue(tint);
        uint8_t* o = (uint8_t*)bits;
        for (size_t i = 0; i < size_t(w) * h; ++i) {
            const uint8_t* p = img.rgba.data() + i * 4;
            const unsigned a = p[3];
            o[i * 4 + 0] = uint8_t(p[2] * tb / 255 * a / 255);
            o[i * 4 + 1] = uint8_t(p[1] * tg / 255 * a / 255);
            o[i * 4 + 2] = uint8_t(p[0] * tr / 255 * a / 255);
            o[i * 4 + 3] = uint8_t(a);
        }
        return true;
    }
    void draw(HDC to, int x, int y, int sx = 0, int sy = 0, int sw = -1, int sh = -1,
              BYTE alpha = 255) const {
        if (!dc) return;
        if (sw < 0) sw = w;
        if (sh < 0) sh = h;
        BLENDFUNCTION bf{AC_SRC_OVER, 0, alpha, AC_SRC_ALPHA};
        AlphaBlend(to, x, y, sw, sh, dc, sx, sy, sw, sh, bf);
    }
};

// The game's UI text font, drawn at a pixel size: glyph ink edges measured from the atlas (as
// uistyle.font_metrics), the atlas box-shrunk once per size and tinted once per colour.
class GameFont {
public:
    bool load() {
        if (tried) return ok;
        tried = true;
        if (!loadTexture("fontbase24r.rfi", atlas) || atlas.w < 1024 || atlas.h < 896) return false;
        int capTop = 64, capBottom = 0;
        for (int code = 32; code < 256; ++code) {
            const int k = code - 32, cx = (k % 16) * 64, cy = (13 - k / 16) * 64;
            // ink: columns with at least two clearly inked pixels (a faint pixel bleeding in from a
            // neighbouring cell must not widen the letter)
            int left = -1, right = -1, top = 64, bottom = -1;
            for (int x = 0; x < 64; ++x) {
                int n = 0;
                for (int y = 0; y < 64; ++y)
                    if (atlas.rgba[(size_t(cy + y) * atlas.w + cx + x) * 4 + 3] > 60) {
                        ++n;
                        top = (std::min)(top, y);
                        bottom = (std::max)(bottom, y + 1);
                    }
                if (n >= 2) {
                    if (left < 0) left = x;
                    right = x + 1;
                }
            }
            glyph[code] = {cx, cy, left, right};
            if (code == 'H' && left >= 0) {
                capTop = top;
                capBottom = bottom;
            }
        }
        cap = (std::max)(8, capBottom - capTop);
        capTopPx = capTop;
        ok = true;
        return ok;
    }
    bool ready() const { return ok; }
    // size: the GDI font height in pixels the panel used for this text (cap height ~ 0.7 of it).
    int width(const std::wstring& s, int size) const {
        if (!ok) return 0;
        const float sc = scale(size);
        float wpx = 0;
        for (wchar_t c : s) wpx += advance(c) * sc;
        return int(wpx + 0.5f);
    }
    bool drawable(const std::wstring& s) const {
        if (!ok) return false;
        for (wchar_t c : s)
            if (c < 32 || c > 255) return false;
        return true;
    }
    int lineHeight(int size) const { return int(64 * scale(size) * 0.62f + 0.5f); }
    // Draws one line with its top-left at (x, y); the line box is lineHeight(size) tall.
    void line(HDC dc, int x, int y, const std::wstring& s, int size, COLORREF colour) {
        if (!ok) return;
        Sheet& sh = sheet(size, colour);
        if (!sh.bmp.dc) return;
        const float sc = sh.scale;
        float pen = float(x);
        const int top = y + int((lineHeight(size) - cap * sc) * 0.5f - capTopPx * sc +
                                0.5f); // capitals centred in the line box
        for (wchar_t c : s) {
            const Glyph& g = glyph[(unsigned)c & 255];
            if (c != L' ' && g.left >= 0) {
                // the cell from its own left edge (thin letters must not be cut by rounding), placed so the
                // ink starts at the pen
                const int sx = int(g.x * sc), sy = int(g.y * sc), sw = (std::max)(1, int(g.right * sc + 2.f)),
                          shh = (std::max)(1, int(64 * sc));
                sh.bmp.draw(dc, int(pen - g.left * sc + 0.5f), top, sx, sy, (std::min)(sw, sh.bmp.w - sx),
                            (std::min)(shh, sh.bmp.h - sy));
            }
            pen += advance(c) * sc;
        }
    }

private:
    struct Glyph {
        int x = 0, y = 0, left = -1, right = -1;
    };
    struct Sheet {
        float scale = 1;
        Bitmap bmp;
    };
    float scale(int size) const { return float(size) * 0.72f / float(cap); }
    float advance(wchar_t c) const {
        const Glyph& g = glyph[(unsigned)c & 255];
        if (c == L' ' || g.left < 0) return cap * 0.42f;
        return float(g.right - g.left) + 3.f; // uistyle's letter_gap
    }
    Sheet& sheet(int size, COLORREF colour) {
        const uint64_t key = (uint64_t(size) << 32) | colour;
        auto it = sheets.find(key);
        if (it != sheets.end()) return *it->second;
        auto s = std::make_unique<Sheet>();
        s->scale = scale(size);
        if (!shrunkBySize.count(size)) shrunkBySize[size] = shrink(atlas, 0, 0, atlas.w, 896, s->scale);
        s->bmp.make(shrunkBySize[size], colour);
        Sheet& r = *s;
        sheets[key] = std::move(s);
        return r;
    }
    bool tried = false, ok = false;
    Image atlas;
    Glyph glyph[256];
    int cap = 30, capTopPx = 20;
    std::map<int, Image> shrunkBySize;
    std::map<uint64_t, std::unique_ptr<Sheet>> sheets;
};

// A word set in the game's title image-font (uistyle.title_text: each cell cut to its ink,
// letters on the font's baseline, 6 px face to face), shrunk to a height.
inline Image titleText(const std::string& text, int height, int gap = 6, int space = 48) {
    Image font;
    if (!loadTexture("titlefont.rfi", font) || font.w < 512) return {};
    const int cw = font.w / 4;
    if (font.h < 7 * cw) return {}; // 27 cells, 4 a row: 7 rows
    struct Cell {
        Image img;
        int bx0, bx1, by0, by1, fx0, fx1;
        bool blank;
    };
    std::vector<Cell> cells;
    auto alphaAt = [](const Image& im, int x, int y) {
        return im.rgba[(size_t(y) * im.w + x) * 4 + 3];
    };
    for (char ch : text) {
        Cell c{};
        c.blank = ch == ' ';
        if (!c.blank) {
            const int i = ch == '+' ? 26 : (std::toupper((unsigned char)ch) - 'A');
            if (i < 0 || i > 26) {
                c.blank = true;
                cells.push_back(std::move(c));
                continue;
            }
            c.img.w = c.img.h = cw;
            c.img.rgba.assign(size_t(cw) * cw * 4, 0);
            for (int y = 0; y < cw; ++y)
                memcpy(c.img.rgba.data() + size_t(y) * cw * 4,
                       font.rgba.data() + (size_t((i / 4) * cw + y) * font.w + (i % 4) * cw) * 4,
                       size_t(cw) * 4);
            // keep the tallest run of inked rows (a letter above can reach into this cell)
            std::vector<std::pair<int, int>> runs;
            int start = -1;
            for (int y = 0; y <= cw; ++y) {
                int m = 0;
                if (y < cw)
                    for (int x = 0; x < cw; ++x) m = (std::max)(m, int(alphaAt(c.img, x, y)));
                if (y < cw && m > 8 && start < 0)
                    start = y;
                else if ((y == cw || m <= 8) && start >= 0) {
                    runs.push_back({start, y});
                    start = -1;
                }
            }
            if (runs.size() > 1) {
                auto keep = *std::max_element(runs.begin(), runs.end(), [](auto a, auto b) {
                    return a.second - a.first < b.second - b.first;
                });
                for (int y = 0; y < cw; ++y)
                    if (y < keep.first || y >= keep.second)
                        memset(c.img.rgba.data() + size_t(y) * cw * 4, 0, size_t(cw) * 4);
            }
            auto box = [&](int th, int& x0, int& x1, int& y0, int& y1) {
                x0 = cw;
                x1 = -1;
                y0 = cw;
                y1 = -1;
                for (int y = 0; y < cw; ++y)
                    for (int x = 0; x < cw; ++x)
                        if (alphaAt(c.img, x, y) > th) {
                            x0 = (std::min)(x0, x);
                            x1 = (std::max)(x1, x + 1);
                            y0 = (std::min)(y0, y);
                            y1 = (std::max)(y1, y + 1);
                        }
            };
            int a, b2;
            box(8, c.bx0, c.bx1, c.by0, c.by1);
            box(200, c.fx0, c.fx1, a, b2);
            if (c.bx1 < 0)
                c.blank = true;
            else if (c.fx1 < 0) {
                c.fx0 = c.bx0;
                c.fx1 = c.bx1;
            }
        }
        cells.push_back(std::move(c));
    }
    int top = 1 << 30, bottom = -1;
    for (const Cell& c : cells)
        if (!c.blank) {
            top = (std::min)(top, c.by0);
            bottom = (std::max)(bottom, c.by1);
        }
    if (bottom < 0) return {};
    std::vector<std::pair<const Cell*, int>> placed;
    int x = 0;
    for (const Cell& c : cells) {
        if (c.blank) {
            x += space;
            continue;
        }
        placed.push_back({&c, x - (c.fx0 - c.bx0)});
        x += (c.fx1 - c.fx0) + gap;
    }
    int minX = 1 << 30, maxX = -(1 << 30);
    for (auto& p : placed) {
        minX = (std::min)(minX, p.second);
        maxX = (std::max)(maxX, p.second + p.first->bx1 - p.first->bx0);
    }
    Image word;
    word.w = maxX - minX;
    word.h = bottom - top;
    word.rgba.assign(size_t(word.w) * word.h * 4, 0);
    for (auto& p : placed) {
        const Cell& c = *p.first;
        for (int y = top; y < bottom; ++y)
            for (int xx = c.bx0; xx < c.bx1; ++xx) {
                const uint8_t* s = c.img.rgba.data() + (size_t(y) * cw + xx) * 4;
                if (!s[3]) continue;
                uint8_t* d =
                    word.rgba.data() + (size_t(y - top) * word.w + (p.second - minX + xx - c.bx0)) * 4;
                const double sa = s[3] / 255.0, da = d[3] / 255.0, oa = sa + da * (1 - sa);
                for (int k = 0; k < 3; ++k)
                    d[k] = uint8_t(oa > 0 ? (s[k] * sa + d[k] * da * (1 - sa)) / oa + 0.5 : 0);
                d[3] = uint8_t(oa * 255 + 0.5);
            }
    }
    return height > 0 && word.h > height ? shrink(word, 0, 0, word.w, word.h, float(height) / float(word.h))
                                         : word;
}

} // namespace uiart
