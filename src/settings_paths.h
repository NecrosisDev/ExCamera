#pragma once
// Where the launcher finds the player's first-person.ini, the same file the DLL uses (session.inl and state.inl
// userSettingsDir). A test copy with portable.txt beside it keeps its settings beside it. Otherwise the order is
// Documents\ExCamera, the same under USERPROFILE when Windows moved Documents, then the older ExaniMate folder.
// The first that exists is read; paths[0] (else [1]) is where the DLL keeps the file from now on.
#include <string>

namespace fp {
struct SettingsCandidates {
    std::wstring paths[4];
    const std::wstring& target() const { return paths[0].empty() ? paths[1] : paths[0]; }
};
// folder ends in a path separator; docs and fallbackDocs may be empty (unknown).
inline SettingsCandidates settingsCandidates(const std::wstring& folder, bool portable, const std::wstring& docs,
                                             const std::wstring& fallbackDocs) {
    if (portable) return {{folder + L"first-person.ini", L"", L"", L""}};
    return {{
        docs.empty() ? L"" : docs + L"\\ExCamera\\first-person.ini",
        fallbackDocs.empty() ? L"" : fallbackDocs + L"\\ExCamera\\first-person.ini",
        docs.empty() ? L"" : docs + L"\\ExaniMate\\first-person.ini",
        fallbackDocs.empty() ? L"" : fallbackDocs + L"\\ExaniMate\\first-person.ini",
    }};
}
} // namespace fp
