// Texture replacements: a .dds file named after a game texture is uploaded in its place. Part of
// first_person.cpp (one translation unit): included there, in order.
//
// The hook is the game's own upload, 0x73580(texture, image), the one place every loaded texture's pixels pass
// through (docs/render-assumptions.md, "Texture replacements"). The texture's lowercase 16-byte name key sits at
// texture+8 (0x72300 stores it when the texture is created by name). The image is the game's in-memory
// description: +0 the pixel data, +8 width and +0xA height (u16), +0x10 the format word, +0x14 flags (0x80000:
// a mip chain follows), +0x1C the data size; 0x73580 copies its 32 bytes to texture+0x20, sets filtering
// (0x72840/0x72990/0x72A60) and uploads every level down to 1x1 with sizes worked out from width and height
// (0x72AD0). A replacement is a copy of that description pointing at the file's levels instead, passed to the
// same function: the game's own upload, filtering, mip and quality handling apply at the new size. The mod
// makes no GL calls here.
//
// A replacement must be the same block format as the game's (BC1 for 0xC6 colour, BC3 for 0xE6/0xA4 with
// alpha, BC4 for 0x42 one channel, BC5 for 0x84 two channels) and carry a full mip chain; anything else, or a
// texture whose name key is longer than the 16 bytes the key holds, is left to the game, and the reason logged.

static std::atomic<bool> textureReplacements{true}; // the F8 row; off draws the game's own textures only

namespace texrep {
using Upload = void(__fastcall*)(uintptr_t texture, uintptr_t image);
static Upload originalUpload = nullptr;
static std::atomic<bool> hooked{false};
static bool admitted = false;
static std::atomic<unsigned> replaced{0}, refused{0}, files{0};
static char lastFailure[160] = "";
static SRWLOCK lock = SRWLOCK_INIT; // the name table and lastFailure

struct Entry { char key[17]; wchar_t path[MAX_PATH]; bool reported; };
static Entry* entries = nullptr;
static unsigned entryCount = 0;
static bool scanned = false;

// The replacements folder's .dds files, by lowercase name without extension; read once.
static void scan() {
    scanned = true;
    const std::wstring folder = settingsFolderFile(L"textures\\");
    if (folder.empty()) return;
    WIN32_FIND_DATAW found;
    const HANDLE search = FindFirstFileW((folder + L"*.dds").c_str(), &found);
    if (search == INVALID_HANDLE_VALUE) return;
    unsigned capacity = 0;
    do {
        if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::wstring stem = found.cFileName;
        stem.resize(stem.size() - 4);
        if (stem.empty() || stem.size() > 16 || folder.size() + wcslen(found.cFileName) >= MAX_PATH) continue;
        if (entryCount == capacity) {
            capacity = capacity ? capacity * 2 : 64;
            auto* grown = (Entry*)realloc(entries, capacity * sizeof(Entry));
            if (!grown) break;
            entries = grown;
        }
        Entry& e = entries[entryCount++];
        memset(&e, 0, sizeof e);
        for (size_t i = 0; i < stem.size(); ++i) {
            const wchar_t c = stem[i];
            e.key[i] = char(c < 128 ? tolower(c) : '?');
        }
        wcscpy_s(e.path, (folder + found.cFileName).c_str());
    } while (FindNextFileW(search, &found));
    FindClose(search);
    files.store(entryCount, std::memory_order_relaxed);
}

static Entry* find(const char* key16) {
    char stem[17]{};
    memcpy(stem, key16, 16);
    if (char* dot = strstr(stem, ".rfi")) *dot = 0;
    for (unsigned i = 0; i < entryCount; ++i)
        if (!strcmp(entries[i].key, stem)) return &entries[i];
    return nullptr;
}

static void refuse(Entry& e, const char* why) {
    refused.fetch_add(1, std::memory_order_relaxed);
    snprintf(lastFailure, sizeof lastFailure, "%s: %s", e.key, why);
    if (e.reported) return;
    e.reported = true;
    wchar_t text[200];
    swprintf_s(text, L"Texture replacement %hs not used (%hs); the game's own texture is drawn.", e.key, why);
    modMessage(NoticeKind::warning, text);
}

// The DDS block format a game format word asks for: 1 BC1, 3 BC3, 4 BC4, 5 BC5; 0 not replaceable.
static int blockFormatFor(uint32_t word) {
    if (!(word >> 31)) return 0;
    switch ((word >> 8) & 0xFF) {
    case 0xC6: return 1;
    case 0xE6: case 0xA4: return 3;
    case 0x42: return 4;
    case 0x84: return 5;
    }
    return 0;
}
static int blockFormatOfDds(const uint8_t* h, size_t size, size_t& dataAt) {
    if (size < 128 || memcmp(h, "DDS ", 4)) return 0;
    const uint32_t fourCC = *(const uint32_t*)(h + 84);
    dataAt = 128;
    auto cc = [](const char* s) { return uint32_t(s[0]) | uint32_t(s[1]) << 8 | uint32_t(s[2]) << 16 | uint32_t(s[3]) << 24; };
    if (fourCC == cc("DXT1")) return 1;
    if (fourCC == cc("DXT5")) return 3;
    if (fourCC == cc("ATI1") || fourCC == cc("BC4U")) return 4;
    if (fourCC == cc("ATI2") || fourCC == cc("BC5U")) return 5;
    if (fourCC == cc("DX10") && size >= 148) {
        dataAt = 148;
        switch (*(const uint32_t*)(h + 128)) {
        case 71: case 72: return 1;   // BC1_UNORM(_SRGB)
        case 77: case 78: return 3;   // BC3
        case 80: return 4;            // BC4_UNORM
        case 83: return 5;            // BC5_UNORM
        }
    }
    return 0;
}
static size_t chainBytes(uint32_t width, uint32_t height, int block, unsigned& levels) {
    const size_t blockBytes = block == 1 || block == 4 ? 8 : 16;
    size_t total = 0;
    levels = 0;
    for (;;) {
        total += size_t((std::max)(1u, (width + 3) / 4)) * (std::max)(1u, (height + 3) / 4) * blockBytes;
        ++levels;
        if (width <= 1 && height <= 1) break;
        width = (std::max)(1u, width / 2);
        height = (std::max)(1u, height / 2);
    }
    return total;
}

static void upload(uintptr_t texture, uintptr_t image) {
    Entry* e = nullptr;
    uint8_t* file = nullptr;
    __try {
        if (textureReplacements.load(std::memory_order_relaxed) && texture && image &&
            readable(texture + 8, 16) && readable(image, 32)) {
            AcquireSRWLockExclusive(&lock);
            if (!scanned) scan();
            e = entryCount ? find((const char*)(texture + 8)) : nullptr;
            ReleaseSRWLockExclusive(&lock);
        }
        if (e) {
            const uint32_t word = *(const uint32_t*)(image + 0x10), flags = *(const uint32_t*)(image + 0x14);
            const int wanted = blockFormatFor(word);
            const HANDLE handle = CreateFileW(e->path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
            LARGE_INTEGER length{};
            const char* why = nullptr;
            if (!wanted) why = "the game's texture is not block-compressed";
            else if (!(flags & 0x80000)) why = "the game's texture has no mip chain";
            else if (handle == INVALID_HANDLE_VALUE || !GetFileSizeEx(handle, &length) || length.QuadPart > (512ll << 20))
                why = "the file could not be read";
            else if ((file = (uint8_t*)VirtualAlloc(nullptr, size_t(length.QuadPart), MEM_COMMIT | MEM_RESERVE,
                                                    PAGE_READWRITE)) == nullptr)
                why = "out of memory";
            if (!why) {
                DWORD got = 0;
                if (!ReadFile(handle, file, DWORD(length.QuadPart), &got, nullptr) || got != DWORD(length.QuadPart))
                    why = "the file could not be read";
            }
            if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
            size_t dataAt = 0;
            const uint32_t width = !why ? *(const uint32_t*)(file + 16) : 0, height = !why ? *(const uint32_t*)(file + 12) : 0;
            const uint32_t mips = !why ? *(const uint32_t*)(file + 28) : 0;
            unsigned levels = 0;
            if (!why && blockFormatOfDds(file, size_t(length.QuadPart), dataAt) != wanted) why = "a different block format";
            if (!why && (width < 4 || height < 4 || width > 16384 || height > 16384)) why = "an unusable size";
            const size_t bytes = !why ? chainBytes(width, height, wanted, levels) : 0;
            if (!why && mips != levels) why = "not a full mip chain";
            if (!why && dataAt + bytes > size_t(length.QuadPart)) why = "the file is shorter than its levels";
            if (why) {
                AcquireSRWLockExclusive(&lock);
                refuse(*e, why);
                ReleaseSRWLockExclusive(&lock);
            } else {
                alignas(16) uint8_t copy[32];
                memcpy(copy, (const void*)image, 32);
                *(uintptr_t*)copy = uintptr_t(file + dataAt);
                *(uint16_t*)(copy + 8) = uint16_t(width);
                *(uint16_t*)(copy + 0xA) = uint16_t(height);
                *(uint32_t*)(copy + 0x1C) = uint32_t(bytes);
                originalUpload(texture, uintptr_t(copy));
                VirtualFree(file, 0, MEM_RELEASE);
                replaced.fetch_add(1, std::memory_order_relaxed);
                return;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        refused.fetch_add(1, std::memory_order_relaxed);
        snprintf(lastFailure, sizeof lastFailure, "a fault reading a replacement");
    }
    if (file) VirtualFree(file, 0, MEM_RELEASE);
    originalUpload(texture, image);
}
} // namespace texrep

static bool admitTextureReplaceEntries(const fp::ImageView& file) {
    bool admitted = false;
    __try {
        admitted = layout.optionalGroupAvailable("textureReplace") && loadedOptionalContractsImpl(file, "textureReplace");
    } __except (EXCEPTION_EXECUTE_HANDLER) { admitted = false; }
    if (!admitted && texrep::hooked.load()) {
        snprintf(faultDetail, sizeof faultDetail, "texture upload contracts changed after hook creation; restart required");
        return false;
    }
    if (!admitted)
        for (unsigned t = 0; t < fp::sig::targetCount; ++t) {
            const char* group = fp::sig::targetOptionalGroups[t];
            if (group && !strcmp(group, "textureReplace")) layout.rva[t] = 0;
        }
    texrep::admitted = admitted;
    return true;
}

// Off until the block format is taken from the game's own format table. 0x72790 matches the whole format word
// against the table 0x766F0 fills from 0x2AE290..., and the second byte alone does not decide it: 0x813BE601
// (byte 0xE6) is GL_COMPRESSED_RGBA_S3TC_DXT1 (BC1, 8-byte blocks) while 0x817BE608 / 0x827BE608 (also 0xE6)
// are DXT5 (BC3, 16). blockFormatFor would ask a BC3 file for the first and the game would upload it as BC1.
inline constexpr bool textureFormatsFromTable = false;
static void createTextureReplaceHook() {
    if constexpr (!textureFormatsFromTable) {
        texrep::hooked = false;
        return;
    }
    texrep::hooked = texrep::admitted &&
                     MH_CreateHook((void*)gameAt(fp::sig::textureUpload), (void*)texrep::upload,
                                   (void**)&texrep::originalUpload) == MH_OK;
}
