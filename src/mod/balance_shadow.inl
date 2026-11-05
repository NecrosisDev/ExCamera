// Balance shadow (docs/BALANCE-SHADOW.md): every physics step, after the game's own character update, it
// measures each fighter's real balance (centre of mass, capture point, base of support) next to what the game
// decided, and counts problems: bad maths, flung, floating, held up past the feet, shaking, stuck on the ground,
// failed get-ups, falls (split by whether the capture point had left the feet). It writes NO game memory; it is
// the baseline a future balance model must match or beat on the same counts.
// Physics thread: balanceShadowObserve (combat.inl, after the native update). Worker: balanceShadowPulse
// (session loop) writes Documents\ExCamera\balance\ (or a portable copy's own folder) and flags problems
// through modMessage (the game's own log line). Optional group balanceShadow; off with
// [Diagnostics] BalanceShadow=0 in first-person.ini.
#include "../balance_shadow_model.h"
namespace bs = fp::balance;

static std::atomic<bool> balanceShadowEnabled{false}, balanceShadowFault{false};
static std::atomic<int> balanceShadowReason{0}; // 0 off by setting, 1 running, 2 group withheld, 3 no update hook
constexpr unsigned shadowSlots = 32, shadowPartLimit = 32, shadowEventRing = 256;

struct ShadowEvent {
    ULONGLONG at = 0;
    unsigned fighter = 0;
    bool player = false;
    bs::Event kind = bs::Event::count;
    unsigned state = 0;
    float margin = 0, height = 0, uprightness = 0;
};
struct ShadowFighter {
    uintptr_t root = 0;
    unsigned id = 0; // order of first sight this session
    bool player = false;
    ULONGLONG lastSeen = 0;
    bs::Tracker tracker{};
    uint64_t steps = 0, stepsAtSecond = 0, standingSteps = 0, outsideSteps = 0;
    uint32_t events[size_t(bs::Event::count)]{};
    // latest values, for the per-second rows
    unsigned state = 0;
    int motion = -1;
    float uprightness = 0, margin = 0, height = 0, shake = 0;
    bool footDown = false;
};
static SRWLOCK shadowLock = SRWLOCK_INIT;
static ShadowFighter shadowFighters[shadowSlots];
static unsigned shadowNextId = 1;
static ShadowEvent shadowEvents[shadowEventRing];
static unsigned shadowEventHead = 0, shadowEventCount = 0, shadowEventsDropped = 0;
// Session totals (under shadowLock).
static uint32_t shadowTotals[size_t(bs::Event::count)]{};
static uint64_t shadowSteps = 0, shadowStandingSteps = 0, shadowPlayerSteps = 0;
static bs::Spread shadowMargin{-60, 60}, shadowShake{0, 8000}, shadowRate{0, 166}, shadowCostUs{0, 200},
    shadowUpdateUs{0, 2000};
static std::atomic<unsigned> shadowReadFaults{0};

// What one physics step read (physics thread, stack). Plain data so the reader can use SEH.
struct ShadowRead {
    bs::V3 now[shadowPartLimit * 4], before[shadowPartLimit * 4], before2[shadowPartLimit * 4];
    unsigned nodes = 0;
    bs::V2 foot[64];
    unsigned feet = 0;
    float ground = 0;
    unsigned grounded = 0;
    bs::GameState game{};
};

// The character being updated and its parts are the game's own live objects, read on the game's thread in the
// middle of its update, so a pointer check is enough here: a bad read is caught by the __try below (and counted)
// instead of paying a VirtualQuery per part per step.
static bool shadowReadable(uintptr_t a, size_t) { return a >= 0x10000 && a < 0x7FFFFFFF0000ull && !(a & 7); }

// The game's own body: every part's four nodes this step and the two before, the loaded feet (support contacts
// on or near the ground, with every node of the parts hanging from that shin), and the game's balance fields.
static bool shadowRead(uintptr_t root, ShadowRead& r) {
    __try {
        if (!shadowReadable(root, field::character::onFeet + 1)) return false;
        r.game.state = *reinterpret_cast<const unsigned*>(root + field::character::stateFlags);
        r.game.motion = *reinterpret_cast<const int*>(root + field::character::motion);
        r.game.footDown = *reinterpret_cast<const float*>(root + field::character::footDown) > .5f;
        r.game.uprightness = *reinterpret_cast<const float*>(root + field::character::uprightness);
        r.game.stepping = *reinterpret_cast<const uint8_t*>(root + field::character::stepping) != 0;
        const int count = *reinterpret_cast<const int*>(root + field::character::partCount);
        const auto parts = *reinterpret_cast<const uintptr_t*>(root + field::character::parts);
        if (count <= 0 || count > int(shadowPartLimit) || !shadowReadable(parts, size_t(count) * 8)) return false;
        const int legCount = *reinterpret_cast<const int*>(root + field::character::legCount);
        const auto legs = *reinterpret_cast<const uintptr_t*>(root + field::character::legs);
        uintptr_t loaded[4]{};
        unsigned loadedCount = 0;
        float groundSum = 0;
        if (legCount > 0 && legCount <= 4 && shadowReadable(legs, size_t(legCount) * 8))
            for (int i = 0; i < legCount; ++i) {
                const auto shin = reinterpret_cast<const uintptr_t*>(legs)[i];
                if (!shadowReadable(shin, field::bodyPart::contacts + 8)) continue;
                if (*reinterpret_cast<const int*>(shin + field::bodyPart::contactCount) <= 0) continue;
                const auto contact = *reinterpret_cast<const uintptr_t*>(shin + field::bodyPart::contacts);
                if (!shadowReadable(contact, field::contact::groundHeight + 4)) continue;
                const auto at = reinterpret_cast<const float*>(contact + field::contact::position);
                const float ground = *reinterpret_cast<const float*>(contact + field::contact::groundHeight);
                const unsigned flags = *reinterpret_cast<const unsigned*>(contact + field::contact::flags);
                // A support contact within 20 units of its ground: the game's own "foot down" test without its
                // small per-contact offset (supportHeight 0x16F84B-0x16F872).
                if (!(flags & 2) || !std::isfinite(at[1]) || !std::isfinite(ground) || at[1] > ground + 20) continue;
                if (r.feet < 64) r.foot[r.feet++] = {at[0], at[2]};
                loaded[loadedCount++] = shin;
                groundSum += ground;
            }
        for (int i = 0; i < count && r.nodes + 4 <= shadowPartLimit * 4; ++i) {
            const auto part = reinterpret_cast<const uintptr_t*>(parts)[i];
            if (!shadowReadable(part, field::bodyPart::character + 8)) continue;
            if (*reinterpret_cast<const uintptr_t*>(part + field::bodyPart::character) != root) continue;
            const auto p = reinterpret_cast<const float*>(part + field::bodyPart::points);
            const auto h = reinterpret_cast<const float*>(part + field::bodyPart::history);
            const auto h2 = reinterpret_cast<const float*>(part + field::bodyPart::history2);
            const auto parent = *reinterpret_cast<const uintptr_t*>(part + field::bodyPart::parentPart);
            bool foot = false;
            for (unsigned l = 0; l < loadedCount; ++l) foot |= parent == loaded[l];
            for (unsigned k = 0; k < 4; ++k) {
                r.now[r.nodes] = {p[3 * k], p[3 * k + 1], p[3 * k + 2]};
                r.before[r.nodes] = {h[3 * k], h[3 * k + 1], h[3 * k + 2]};
                r.before2[r.nodes] = {h2[3 * k], h2[3 * k + 1], h2[3 * k + 2]};
                if (foot && r.feet < 64) r.foot[r.feet++] = {p[3 * k], p[3 * k + 2]};
                ++r.nodes;
            }
        }
        r.grounded = loadedCount;
        r.ground = loadedCount ? groundSum / float(loadedCount) : 0;
        return r.nodes > 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // A layout the reads keep tripping over: stop rather than keep faulting every step.
        if (shadowReadFaults.fetch_add(1, std::memory_order_relaxed) >= 1000) balanceShadowFault = true;
        return false;
    }
}

static double shadowMicroseconds(LARGE_INTEGER from, LARGE_INTEGER to) {
    static LARGE_INTEGER shadowFrequency{};
    if (!shadowFrequency.QuadPart) QueryPerformanceFrequency(&shadowFrequency);
    return shadowFrequency.QuadPart ? double(to.QuadPart - from.QuadPart) * 1e6 / double(shadowFrequency.QuadPart) : 0;
}

static ShadowFighter& shadowSlotFor(uintptr_t root, ULONGLONG now) {
    ShadowFighter* free = nullptr;
    for (auto& f : shadowFighters) {
        if (f.root == root) return f;
        if (!free && (!f.root || now - f.lastSeen > 5000)) free = &f;
    }
    if (!free) free = &shadowFighters[0];
    *free = ShadowFighter{};
    free->root = root;
    free->id = shadowNextId++;
    return *free;
}

// Physics thread, after the native combatUpdate for `root`. `updateUs`: how long that native update took.
static void balanceShadowObserve(uintptr_t root, double updateUs) {
    if (!balanceShadowEnabled.load(std::memory_order_relaxed) || balanceShadowFault.load(std::memory_order_relaxed)) return;
    LARGE_INTEGER t0{}, t1{};
    QueryPerformanceCounter(&t0);
    static thread_local ShadowRead read; // large: kept off the stack
    read = ShadowRead{};
    if (!shadowRead(root, read)) return;
    bs::Sample sample;
    sample.now = read.now, sample.before = read.before, sample.before2 = read.before2, sample.nodes = read.nodes;
    sample.footprint = read.foot, sample.footprintCount = read.feet, sample.ground = read.ground;
    sample.grounded = read.grounded != 0;
    const bs::Measure m = bs::measure(sample);
    const ULONGLONG now = GetTickCount64();
    const bool player = *reinterpret_cast<uintptr_t*>(gameAt(fp::sig::playerRoot)) == root;
    {
        AcquireSRWLockExclusive(&shadowLock);
        ShadowFighter& f = shadowSlotFor(root, now);
        f.player = player;
        f.lastSeen = now;
        const uint32_t events = f.tracker.feed(read.game, m);
        ++f.steps, ++shadowSteps;
        if (player) ++shadowPlayerSteps;
        f.state = read.game.state, f.motion = read.game.motion, f.uprightness = read.game.uprightness;
        f.footDown = read.game.footDown, f.margin = m.margin, f.height = m.height, f.shake = m.shake;
        if (!bs::down(read.game.state) && m.valid) {
            ++f.standingSteps, ++shadowStandingSteps;
            if (read.game.footDown && std::isfinite(m.margin)) {
                shadowMargin.add(m.margin);
                if (m.margin < 0) ++f.outsideSteps;
            }
            shadowShake.add(m.shake);
        }
        shadowUpdateUs.add(float(updateUs));
        for (unsigned e = 0; e < unsigned(bs::Event::count); ++e) {
            if (!(events & (1u << e))) continue;
            ++f.events[e], ++shadowTotals[e];
            if (shadowEventCount == shadowEventRing) {
                ++shadowEventsDropped;
                continue;
            }
            shadowEvents[(shadowEventHead + shadowEventCount++) % shadowEventRing] =
                {now, f.id, player, bs::Event(e), read.game.state, m.margin, m.height, read.game.uprightness};
        }
        QueryPerformanceCounter(&t1);
        shadowCostUs.add(float(shadowMicroseconds(t0, t1)));
        ReleaseSRWLockExclusive(&shadowLock);
    }
}

// ---- Worker side: files, messages, the game's own physics reports ----

static wchar_t shadowFolder[MAX_PATH]{}, shadowStamp[32]{};
static ULONGLONG shadowStarted = 0, shadowLastSecond = 0, shadowLastSummary = 0;
static ULONGLONG shadowLastFlag[size_t(bs::Event::count)]{};
static unsigned shadowSuppressed[size_t(bs::Event::count)]{};
static uint64_t shadowGameLogOffset = 0;
static unsigned shadowPhysicsReports = 0, shadowFlagsShown = 0;
static char shadowLastReport[200]{};

static void shadowAppend(const wchar_t* suffix, const char* text) {
    if (!shadowFolder[0]) return;
    wchar_t path[MAX_PATH];
    swprintf_s(path, L"%sbalance-%s-%s", shadowFolder, shadowStamp, suffix);
    const HANDLE file = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(file, text, DWORD(strlen(text)), &written, nullptr);
    CloseHandle(file);
}

static const wchar_t* shadowProblemText(bs::Event e) {
    switch (e) {
    case bs::Event::nonFinite: return L"has broken body maths (a NaN position)";
    case bs::Event::flung: return L"had a body part flung faster than 30 m/s";
    case bs::Event::floating: return L"is upright with no foot on the ground (floating)";
    case bs::Event::shaking: return L"is shaking while standing still";
    case bs::Event::stuckDown: return L"has been on the ground for 15 s";
    case bs::Event::failedGetUp: return L"failed to get up";
    case bs::Event::fallUnpredicted: return L"fell while its weight was over its feet";
    default: return L"";
    }
}

// The game's own physics error reports ("Excessive constraint delta", "Excessive pivot delta" in 0x15DA80,
// "Object position overflow", "Duplicate RBD grid migration" in 0x80CD0) go through the engine log, which
// saves them to %APPDATA%\Exanima\Exanima.log. New lines since the session started are counted from the file:
// no game memory involved.
static void shadowReadGameLog(bool first) {
    wchar_t path[MAX_PATH];
    if (!GetEnvironmentVariableW(L"APPDATA", path, MAX_PATH)) return;
    wcscat_s(path, L"\\Exanima\\Exanima.log");
    const HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER size{};
    GetFileSizeEx(file, &size);
    if (first || uint64_t(size.QuadPart) < shadowGameLogOffset) shadowGameLogOffset = first ? uint64_t(size.QuadPart) : 0;
    char buffer[8192];
    while (!first && uint64_t(size.QuadPart) > shadowGameLogOffset) {
        LARGE_INTEGER at{};
        at.QuadPart = LONGLONG(shadowGameLogOffset);
        SetFilePointerEx(file, at, nullptr, FILE_BEGIN);
        DWORD got = 0;
        if (!ReadFile(file, buffer, sizeof buffer - 1, &got, nullptr) || !got) break;
        // only whole lines; a partial last line is read again next time
        DWORD end = got;
        while (end && buffer[end - 1] != '\n') --end;
        if (!end) { if (got < sizeof buffer - 1) break; end = got; }
        buffer[end] = 0;
        for (char* line = buffer; line && *line;) {
            char* next = strchr(line, '\n');
            if (next) *next++ = 0;
            if (strstr(line, "delta") || strstr(line, "overflow") || strstr(line, "migration")) {
                ++shadowPhysicsReports;
                strncpy_s(shadowLastReport, line, _TRUNCATE);
                char row[300];
                snprintf(row, sizeof row, "%llu,game_physics_report,,,,,,\"%s\"\r\n",
                         (unsigned long long)(GetTickCount64() - shadowStarted), shadowLastReport);
                shadowAppend(L"events.csv", row);
            }
            line = next;
        }
        shadowGameLogOffset += end;
    }
    CloseHandle(file);
}

static void shadowWriteSummary() {
    char text[6144];
    int n = 0;
    auto put = [&](const char* format, auto... values) {
        if (n >= 0 && size_t(n) < sizeof text) n += snprintf(text + n, sizeof text - size_t(n), format, values...);
    };
    AcquireSRWLockShared(&shadowLock);
    const double minutes = double(shadowStandingSteps) / bs::stepsPerSecond / 60.0;
    put("# ExCamera balance shadow summary. Read-only: the game's own balance drove every fighter.\r\n");
    put("# Compare two of these with: python tools/balance_compare.py <baseline> <candidate>\r\n");
    put("balance_model=game\r\n");
    put("session_seconds=%.0f\r\n", double(GetTickCount64() - shadowStarted) / 1000.0);
    put("physics_steps=%llu\r\nplayer_steps=%llu\r\nstanding_fighter_minutes=%.2f\r\n", (unsigned long long)shadowSteps,
        (unsigned long long)shadowPlayerSteps, minutes);
    for (unsigned e = 0; e < unsigned(bs::Event::count); ++e)
        put("event.%s=%u\r\nrate.%s_per_fighter_minute=%.4f\r\n", bs::eventNames[e], shadowTotals[e], bs::eventNames[e],
            minutes > 0 ? double(shadowTotals[e]) / minutes : 0.0);
    put("game_physics_reports=%u\r\n", shadowPhysicsReports);
    put("events_dropped=%u\r\nread_faults=%u\r\nfault=%d\r\n", shadowEventsDropped, shadowReadFaults.load(),
        balanceShadowFault.load() ? 1 : 0);
    auto spread = [&](const char* name, const bs::Spread& s) {
        put("%s.count=%llu\r\n%s.mean=%.2f\r\n%s.p50=%.2f\r\n%s.p95=%.2f\r\n%s.p99=%.2f\r\n%s.peak=%.2f\r\n", name,
            (unsigned long long)s.count, name, s.mean(), name, s.quantile(.5f), name, s.quantile(.95f), name,
            s.quantile(.99f), name, s.count ? s.peak : 0.f);
    };
    spread("margin_cm", shadowMargin);
    spread("shake_cm_s2", shadowShake);
    spread("steps_per_second", shadowRate);
    spread("shadow_cost_us", shadowCostUs);
    spread("game_update_us", shadowUpdateUs);
    ReleaseSRWLockShared(&shadowLock);
    if (!shadowFolder[0]) return;
    wchar_t path[MAX_PATH];
    swprintf_s(path, L"%sbalance-%s-summary.txt", shadowFolder, shadowStamp);
    const HANDLE file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(file, text, DWORD(n > 0 ? (std::min)(size_t(n), sizeof text - 1) : 0), &written, nullptr);
    CloseHandle(file);
}

// Session start, after the hooks are armed: the setting, the optional group and the update hook.
static void balanceShadowStart(bool updateHooked) {
    const int setting = settingsPath[0] ? int(GetPrivateProfileIntW(L"Diagnostics", L"BalanceShadow", 1, settingsPath)) : 1;
    int reason = 1;
    if (!setting) reason = 0;
    else if (!layout.optionalGroupAvailable("balanceShadow")) reason = 2;
    else if (!updateHooked) reason = 3; // combatUpdate is hooked with manual defense (combat.inl)
    balanceShadowReason = reason;
    if (reason != 1) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    swprintf_s(shadowStamp, L"%04u%02u%02u-%02u%02u%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    const std::wstring folder = settingsFolderFile(L"balance\\");
    if (!folder.empty() && folder.size() < MAX_PATH) {
        SHCreateDirectoryExW(nullptr, folder.c_str(), nullptr);
        wcscpy_s(shadowFolder, folder.c_str());
    }
    shadowStarted = shadowLastSecond = shadowLastSummary = GetTickCount64();
    shadowReadGameLog(true);
    shadowAppend(L"events.csv", "ms,event,fighter,player,state,margin_cm,height_cm,uprightness\r\n");
    shadowAppend(L"seconds.csv", "ms,fighter,player,steps_per_s,state,motion,uprightness,foot_down,margin_cm,height_cm,shake_cm_s2\r\n");
    balanceShadowEnabled = true;
}

// Worker, every loop pass (100 ms): drains events, flags problems, writes the per-second rows and the summary.
static void balanceShadowPulse() {
    if (!balanceShadowEnabled.load()) return;
    const ULONGLONG now = GetTickCount64();
    ShadowEvent drained[shadowEventRing];
    unsigned count = 0;
    AcquireSRWLockExclusive(&shadowLock);
    while (shadowEventCount && count < shadowEventRing) {
        drained[count++] = shadowEvents[shadowEventHead];
        shadowEventHead = (shadowEventHead + 1) % shadowEventRing, --shadowEventCount;
    }
    ReleaseSRWLockExclusive(&shadowLock);
    for (unsigned i = 0; i < count; ++i) {
        const ShadowEvent& e = drained[i];
        char row[200];
        snprintf(row, sizeof row, "%llu,%s,%u,%d,0x%X,%.1f,%.1f,%.3f\r\n", (unsigned long long)(e.at - shadowStarted),
                 bs::eventNames[size_t(e.kind)], e.fighter, e.player ? 1 : 0, e.state, double(e.margin), double(e.height),
                 double(e.uprightness));
        shadowAppend(L"events.csv", row);
        if (!bs::eventIsProblem[size_t(e.kind)]) continue;
        // On screen through the game's log, at most once per problem kind every 15 s; the rest are counted.
        const size_t k = size_t(e.kind);
        if (now - shadowLastFlag[k] < 15000) {
            ++shadowSuppressed[k];
            continue;
        }
        wchar_t text[256];
        swprintf_s(text, L"Balance check: %s %s.%s", e.player ? L"you" : L"an NPC", shadowProblemText(e.kind),
                   shadowSuppressed[k] ? L" (more since the last notice: see the balance log)" : L"");
        modMessage(NoticeKind::warning, text);
        shadowLastFlag[k] = now, shadowSuppressed[k] = 0;
        ++shadowFlagsShown;
    }
    static bool faultFlagged = false;
    if (balanceShadowFault.load() && !faultFlagged) {
        modMessage(NoticeKind::warning, L"Balance check stopped after an internal error; the game is unaffected.");
        faultFlagged = true;
    }
    if (now - shadowLastSecond >= 1000) {
        const double seconds = double(now - shadowLastSecond) / 1000.0;
        shadowLastSecond = now;
        char rows[shadowSlots * 140];
        int n = 0;
        AcquireSRWLockExclusive(&shadowLock);
        for (auto& f : shadowFighters) {
            if (!f.root || now - f.lastSeen > 1000) continue;
            const float rate = float(double(f.steps - f.stepsAtSecond) / seconds);
            f.stepsAtSecond = f.steps;
            shadowRate.add(rate);
            if (n >= 0 && size_t(n) < sizeof rows)
                n += snprintf(rows + n, sizeof rows - size_t(n), "%llu,%u,%d,%.1f,0x%X,%d,%.3f,%d,%.1f,%.1f,%.0f\r\n",
                              (unsigned long long)(now - shadowStarted), f.id, f.player ? 1 : 0, double(rate), f.state,
                              f.motion, double(f.uprightness), f.footDown ? 1 : 0, double(f.margin), double(f.height),
                              double(f.shake));
        }
        ReleaseSRWLockExclusive(&shadowLock);
        if (n > 0) {
            rows[(std::min)(size_t(n), sizeof rows - 1)] = 0;
            shadowAppend(L"seconds.csv", rows);
        }
        shadowReadGameLog(false);
    }
    if (now - shadowLastSummary >= 10000) {
        shadowLastSummary = now;
        shadowWriteSummary();
    }
}

static void balanceShadowFlush() {
    if (!balanceShadowEnabled.load()) return;
    balanceShadowPulse();
    shadowReadGameLog(false);
    shadowWriteSummary();
}

static const char* balanceShadowReasonText() {
    switch (balanceShadowReason.load()) {
    case 0: return "off_by_setting";
    case 1: return "running";
    case 2: return "group_withheld";
    case 3: return "no_update_hook";
    default: return "unknown";
    }
}
