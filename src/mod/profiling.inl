// Worker-only control and publication. Callback code never reads a file or
// waits for this owner; a capture is read only after admitted roots drain.
static std::wstring profileDirectory, profileControl;
static std::wstring profileRequest, profileLabel, profileLastRequest, profileReportPath;
static ULONGLONG profileNextPoll = 0;
static uint64_t profileDeadline = 0, profileEnded = 0;
static unsigned profileDuration = 60;
static const char* profileReason = "complete";
static uint64_t profileClock() {
    LARGE_INTEGER value{};
    return QueryPerformanceCounter(&value) ? uint64_t(value.QuadPart) : 0;
}
static uint32_t profileThread() { return GetCurrentThreadId(); }
static std::string profileUtf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, value.data(), int(value.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) return {};
    std::string out(size_t(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), int(value.size()), &out[0], size, nullptr, nullptr);
    return out;
}
static void profileStatus(const char* state, const char* error = "");
static void profileInitialize() noexcept {
    try {
    const auto documents = userDocumentsDir();
    if (documents.empty()) return;
    const std::wstring parent = documents + L"\\ExCamera";
    profileControl = parent + L"\\profiling.ini";
    profileDirectory = parent + L"\\profiles";
    CreateDirectoryW(parent.c_str(), nullptr);
    CreateDirectoryW(profileDirectory.c_str(), nullptr);
    profileStatus("idle");
    } catch (...) { profileControl.clear(); profileDirectory.clear(); }
}
static bool profileValidRequest(const wchar_t* request) {
    if (wcslen(request) != 32) return false;
    for (unsigned i = 0; i < 32; ++i)
        if (!((request[i] >= L'0' && request[i] <= L'9') || (request[i] >= L'a' && request[i] <= L'f'))) return false;
    return true;
}
static bool profilePublishFile(const std::wstring& path, void (*write)(FILE*)) {
    const std::wstring temporary = path + L".tmp";
    FILE* file = nullptr;
    if (_wfopen_s(&file, temporary.c_str(), L"wb") || !file) return false;
    try { write(file); } catch (...) { fclose(file); return false; }
    const bool ok = ferror(file) == 0;
    const bool closed = fclose(file) == 0;
    if (!ok || !closed) return false;
    return MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
}
static void profileStatus(const char* state, const char* error) {
    if (profileDirectory.empty()) return;
    // State/error vary per publication, not per callback.
    const std::wstring path = profileDirectory + L"\\status.json";
    const std::wstring temporary = path + L".tmp";
    const auto requestUtf8 = profileUtf8(profileRequest), labelUtf8 = profileUtf8(profileLabel), reportUtf8 = profileUtf8(profileReportPath);
    FILE* f = nullptr;
    if (_wfopen_s(&f, temporary.c_str(), L"wb") || !f) return;
    fp::JsonOut j(f);
    j.unum("schema", 1).str("build", FP_BUILD).unum("pid", GetCurrentProcessId())
        .str("request", requestUtf8.c_str()).str("state", state).str("error", error)
        .str("label", labelUtf8.c_str()).str("report", reportUtf8.c_str());
    j.finish();
    const bool ok = ferror(f) == 0, closed = fclose(f) == 0;
    if (ok && closed) MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
}
static std::array<int, 10> profileSettingsStart{}, profileSettingsEnd{};
static std::array<int, 10> profileSettingsSnapshot() {
    SettingsGuard guard;
    return {selectedFov.load(), tpFov.load(), tpDistance.load(), tpShoulder.load(), tpHeight.load(),
            tpLag.load(), nearClip.load(), int(tpBlockers.load()), headSteady.load(), int(rawMouse)};
}
static void profileWriteCapture(FILE* file) {
    const auto& engine = fp::profile::engine;
    fp::JsonOut j(file);
    j.unum("schema", 1).str("build", FP_BUILD).str("variant", FP_PROFILE_VARIANT)
        .str("source_base", "d5507b7af0fc45045a73eb6b8a67f471f895902a")
        .unum("revision", FP_REVISION).unum("pid", GetCurrentProcessId())
        .str("request", profileUtf8(profileRequest).c_str()).str("label", profileUtf8(profileLabel).c_str())
        .str("reason", profileReason).unum("sample_every", engine.sampleEvery())
        .unum("frequency", engine.frequency()).unum("started_ticks", engine.started()).unum("ended_ticks", profileEnded)
        .unum("thread_drops", engine.threadDrops()).boolean("settings_changed", profileSettingsStart != profileSettingsEnd)
        .str("settings_fields", "fov,tp_fov,tp_distance,tp_shoulder,tp_height,tp_lag,near_clip,tp_blockers,head_steady,raw_mouse")
        .array("settings_start");
    for (int value : profileSettingsStart) j.num(nullptr, value);
    j.end().array("settings_end");
    for (int value : profileSettingsEnd) j.num(nullptr, value);
    j.end().str("timing", "QPC wall time; self excludes instrumented children; native/wait rows are separate; no GPU timing")
        .str("sampling", "root invocations sampled randomly; nested scopes share selection; calls exact; timings estimated from samples")
        .real("frame_bucket_ms", .25).array("threads");
    for (const auto& row : engine.rows()) {
        bool used = row.presents != 0;
        for (const auto& metric : row.metrics) used = used || metric.calls != 0;
        if (!used) continue;
        j.object(nullptr).unum("id", row.thread).unum("presents", row.presents).unum("frame_intervals", row.intervals)
            .unum("frame_ticks", row.frameTicks).unum("max_frame_ticks", row.maxFrame)
            .unum("frame_overflow", row.overflowFrames).unum("clock_errors", row.clockErrors).unum("depth_drops", row.depthDrops)
            .array("modes");
        for (auto count : row.modes) j.unum(nullptr, count);
        j.end().array("frame_histogram");
        for (auto count : row.histogram) j.unum(nullptr, count);
        j.end().array("hooks");
        for (unsigned i = 0; i < fp::profile::hookCount; ++i) {
            const auto& metric = row.metrics[i];
            if (!metric.calls) continue;
            j.object(nullptr).str("name", fp::profile::hooks[i].name).str("kind", fp::profile::hooks[i].kind)
                .unum("calls", metric.calls).unum("samples", metric.samples)
                .unum("inclusive_ticks", metric.inclusive).unum("self_ticks", metric.self)
                .unum("max_ticks", metric.maximum).unum("max_self_ticks", metric.maximumSelf).end();
        }
        j.end().end();
    }
    j.end(); j.finish();
}
static void profileStop(const char* reason) {
    if (fp::profile::engine.admitting()) {
        profileReason = reason;
        fp::profile::engine.stop();
    }
}
static void profilePulseImpl(bool allowStart) {
    if (profileControl.empty()) return;
    auto& engine = fp::profile::engine;
    if (engine.admitting() && profileClock() >= profileDeadline) profileStop("complete");
    if (engine.ready()) {
        profileEnded = profileClock();
        profileSettingsEnd = profileSettingsSnapshot();
        const bool saved = profilePublishFile(profileReportPath, profileWriteCapture);
        engine.finish();
        profileStatus(saved ? "complete" : "error", saved ? "" : "report_write_failed");
    }
    const auto now = GetTickCount64();
    if (now < profileNextPoll) return;
    profileNextPoll = now + 1000;
    wchar_t request[40]{}, label[81]{};
    GetPrivateProfileStringW(L"Profile", L"Request", L"", request, 40, profileControl.c_str());
    const bool enabled = GetPrivateProfileIntW(L"Profile", L"Enabled", 0, profileControl.c_str()) != 0;
    if (!enabled || (engine.admitting() && profileRequest != request)) profileStop("cancelled");
    if (!allowStart || !enabled || engine.admitting() || engine.draining() || !profileValidRequest(request) ||
        profileLastRequest == request) return;
    const DWORD target = GetPrivateProfileIntW(L"Profile", L"TargetPID", 0, profileControl.c_str());
    wchar_t expiry[32]{};
    GetPrivateProfileStringW(L"Profile", L"NotAfter", L"0", expiry, 32, profileControl.c_str());
    FILETIME utc{}; GetSystemTimeAsFileTime(&utc);
    const uint64_t unixNow = ((uint64_t(utc.dwHighDateTime) << 32) | utc.dwLowDateTime) / 10000000 - 11644473600ULL;
    const uint64_t notAfter = _wcstoui64(expiry, nullptr, 10);
    if (target != GetCurrentProcessId() || notAfter < unixNow || notAfter > unixNow + 60) return;
    if (!CreateDirectoryW(profileDirectory.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) return;
    GetPrivateProfileStringW(L"Profile", L"Label", L"unlabelled", label, 81, profileControl.c_str());
    const unsigned every = (std::max)(1u, (std::min)(4096u, GetPrivateProfileIntW(L"Profile", L"SampleEvery", 64, profileControl.c_str())));
    profileDuration = (std::max)(5u, (std::min)(300u, GetPrivateProfileIntW(L"Profile", L"Seconds", 60, profileControl.c_str())));
    LARGE_INTEGER rate{};
    profileRequest = profileLastRequest = request; profileLabel = label;
    profileReportPath = profileDirectory + L"\\profile-" + std::to_wstring(GetCurrentProcessId()) + L"-" + profileRequest + L".json";
    profileSettingsStart = profileSettingsSnapshot();
    if (!QueryPerformanceFrequency(&rate) || !engine.start(profileClock, profileThread, uint64_t(rate.QuadPart), every)) {
        profileStatus("error", "timer_or_recorder_unavailable"); return;
    }
    profileDeadline = engine.started() + uint64_t(profileDuration) * uint64_t(rate.QuadPart);
    profileStatus("capturing");
}

// Diagnostics must never terminate the camera worker on an allocation/I/O error.
static void profilePulse(bool allowStart = true) noexcept {
    try { profilePulseImpl(allowStart); } catch (...) {
        fp::profile::engine.stop();
        if (fp::profile::engine.ready()) fp::profile::engine.finish();
        profileNextPoll = GetTickCount64() + 1000;
        try { profileStatus("error", "profiler_exception"); } catch (...) {}
    }
}
