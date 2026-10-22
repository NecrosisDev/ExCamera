// Dev automation (build.py --dev only). dev-load.ini beside the DLL, read once per process:
//   [Load] Slot=Exanima005|Arena001  Ready=<readiness file>; Level, Character and Npcs are rejected.
// The title-screen flow is replayed as its Load confirm does; readiness goes to Ready or dev-ready.json.
// Part of first_person.cpp (one translation unit): included there, in order.
#if FP_DEV_TOOLS

namespace devrva { // installed Exanima, static traces; each function is checked against its prologue below
inline constexpr uintptr_t setMode = 0x126B40;        // (mode) 4 story, 8 arena; points the current session
inline constexpr uintptr_t listSlots = 0x17EC70;      // (session) scans the save folder once; true when nonempty
inline constexpr uintptr_t selectRow = 0x17F230;      // (session, row, pref) -> 1 .rsg, 2 .rcp present
inline constexpr uintptr_t schedule = 0x1246D0;       // (step, 1, 0) as the Load confirm 12F2E0
inline constexpr uintptr_t loadStep = 0x1277F0;       // menu teardown, then story/arena start
inline constexpr uintptr_t session = 0x4F0C20;        // current session object
inline constexpr uintptr_t loadPreference = 0x4F0040; // dword: 1 save, 2 checkpoint
inline constexpr uintptr_t confirmFlag = 0x4EF330;    // byte the confirm clears before scheduling
} // namespace devrva

struct DevPrologue {
    uintptr_t rva;
    unsigned char bytes[8];
};
static constexpr DevPrologue devPrologues[] = {
    {devrva::setMode, {0x53, 0x57, 0x56, 0x48, 0x8d, 0x64, 0x24, 0xd0}},
    {devrva::listSlots, {0x53, 0x56, 0x48, 0x8d, 0x64, 0x24, 0xd8, 0x48}},
    {devrva::selectRow, {0x55, 0x48, 0x89, 0xe5, 0x48, 0x8d, 0xa4, 0x24}},
    {devrva::schedule, {0x53, 0x57, 0x56, 0x48, 0x8d, 0x64, 0x24, 0xd0}},
    {devrva::loadStep, {0x48, 0x8d, 0x64, 0x24, 0xc8, 0xc6, 0x05, 0x74}},
};

enum class DevLoad { none, waiting, loading, inLevel, failed };
static std::atomic<DevLoad> devLoad{DevLoad::none};
static std::wstring devReadyPath;
static char devSlotName[32], devReason[64];
static int devSlotMode = 0, devSlotNumber = 0;
static ULONGLONG devLoadStarted = 0;
static unsigned devCharacters;

static void devFail(const char* reason) {
    strncpy_s(devReason, reason, _TRUNCATE);
    devLoad = DevLoad::failed;
}

static bool devProloguesMatch() {
    for (const DevPrologue& p : devPrologues)
        if (!readable(base + p.rva, sizeof p.bytes) ||
            std::memcmp(reinterpret_cast<const void*>(base + p.rva), p.bytes, sizeof p.bytes))
            return false;
    return true;
}

// Worker thread, once.
static void devReadRequest() {
    const wchar_t* slash = wcsrchr(statusPath, L'\\');
    if (!slash) return;
    const std::wstring dir(statusPath, static_cast<size_t>(slash + 1 - statusPath)), ini = dir + L"dev-load.ini";
    if (GetFileAttributesW(ini.c_str()) == INVALID_FILE_ATTRIBUTES) return;
    wchar_t value[MAX_PATH]{};
    GetPrivateProfileStringW(L"Load", L"Ready", L"", value, MAX_PATH, ini.c_str());
    devReadyPath = *value ? value : dir + L"dev-ready.json";
    for (const wchar_t* key : {L"Level", L"Character", L"Npcs"}) {
        GetPrivateProfileStringW(L"Load", key, L"", value, MAX_PATH, ini.c_str());
        if (*value) return devFail("unsupported_key");
    }
    GetPrivateProfileStringW(L"Load", L"Slot", L"", value, MAX_PATH, ini.c_str());
    WideCharToMultiByte(CP_UTF8, 0, value, -1, devSlotName, sizeof devSlotName, nullptr, nullptr);
    const char* digits = nullptr;
    if (!_strnicmp(devSlotName, "Exanima", 7)) devSlotMode = 4, digits = devSlotName + 7;
    else if (!_strnicmp(devSlotName, "Arena", 5)) devSlotMode = 8, digits = devSlotName + 5;
    char* end = nullptr;
    devSlotNumber = digits ? static_cast<int>(std::strtol(digits, &end, 10)) : 0;
    if (!digits || end == digits || *end || devSlotNumber <= 0) return devFail("bad_slot");
    if (!devProloguesMatch()) return devFail("game_build_mismatch");
    devLoad = DevLoad::waiting;
}

// Game thread at the title screen: mode, slot row, preference, then the confirm's schedule.
static const char* devStartLoad() {
    using SetMode = void (*)(int);
    using ListSlots = bool (*)(uintptr_t);
    using SelectRow = int (*)(uintptr_t, int, int);
    using Schedule = void (*)(uintptr_t, bool, bool);
    reinterpret_cast<SetMode>(base + devrva::setMode)(devSlotMode);
    const uintptr_t session = *reinterpret_cast<const uintptr_t*>(base + devrva::session);
    if (!readable(session, 0x38)) return "no_session";
    reinterpret_cast<ListSlots>(base + devrva::listSlots)(session);
    const int count = *reinterpret_cast<const int*>(session + 0x08);
    if (count <= 0 || count > 999) return "no_slots";
    int available = 0, row = 0;
    for (; row < count; ++row) {
        available = reinterpret_cast<SelectRow>(base + devrva::selectRow)(session, row, 1);
        if (*reinterpret_cast<const int*>(session + 0x34) == devSlotNumber) break;
    }
    if (row == count) return "slot_not_found";
    if (!(available & 3)) return "slot_files_missing";
    *reinterpret_cast<int*>(base + devrva::loadPreference) = available & 1 ? 1 : 2;
    *reinterpret_cast<unsigned char*>(base + devrva::confirmFlag) = 0;
    reinterpret_cast<Schedule>(base + devrva::schedule)(base + devrva::loadStep, true, false);
    return nullptr;
}

static bool devStartLoadGuarded(const char** reason) {
    __try {
        *reason = devStartLoad();
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Swap hook (game thread). Starts once the title has shown for three seconds with no character.
static void devFrame() {
    static ULONGLONG titleSince = 0;
    if (devLoad.load() != DevLoad::waiting) return;
    if (*reinterpret_cast<const uintptr_t*>(gameAt(fp::sig::playerRoot))) {
        titleSince = 0;
        return;
    }
    const ULONGLONG now = GetTickCount64();
    if (!titleSince) titleSince = now;
    if (now - titleSince < 3000) return;
    const char* reason = nullptr;
    if (!devStartLoadGuarded(&reason)) return devFail("load_fault");
    if (reason) return devFail(reason);
    devLoadStarted = now;
    devLoad = DevLoad::loading;
}

static unsigned devCharacterCount() {
    const uintptr_t world = *reinterpret_cast<const uintptr_t*>(gameAt(fp::sig::world));
    if (!readable(world, field::worldObject::characters + sizeof(uintptr_t))) return 0;
    const uintptr_t list = *reinterpret_cast<const uintptr_t*>(world + field::worldObject::characters);
    if (!readable(list, field::list::items + sizeof(uintptr_t))) return 0;
    const int count = *reinterpret_cast<const int*>(list + field::list::count);
    return count > 0 ? static_cast<unsigned>(count) : 0;
}

static bool devInLevel() {
    __try {
        const bool in = *reinterpret_cast<const uintptr_t*>(gameAt(fp::sig::playerRoot)) &&
                        InterlockedCompareExchange(&cameraReady, 0, 0) && InterlockedCompareExchange(&active, 0, 0);
        devCharacters = in ? devCharacterCount() : 0;
        return in;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static void devWriteReady(FILE* f) {
    fp::JsonOut j(f);
    const DevLoad state = devLoad.load();
    j.str("state", state == DevLoad::inLevel ? "in_level" : state == DevLoad::failed ? "failed" : "loading");
    j.unum("pid", GetCurrentProcessId());
    if (state == DevLoad::failed) j.str("reason", devReason);
    if (state == DevLoad::inLevel) {
        j.str("level", "");
        j.str("character", devSlotName);
        j.unum("characters", devCharacters);
    }
    j.finish();
}

// Worker thread, every loop pass.
static void devPulse() {
    static bool requested = false;
    if (!requested) {
        requested = true;
        devReadRequest();
    }
    if (devReadyPath.empty()) return;
    static ULONGLONG next = 0;
    const ULONGLONG now = GetTickCount64();
    if (now < next) return;
    next = now + 1000;
    if (devLoad.load() == DevLoad::loading) {
        if (devInLevel()) devLoad = DevLoad::inLevel;
        else if (now - devLoadStarted > 180000) devFail("timeout");
    }
    profilePublishFile(devReadyPath, devWriteReady);
}

#else
static void devPulse() {}
static void devFrame() {}
#endif
