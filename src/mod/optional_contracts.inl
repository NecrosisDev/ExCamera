// Exact proof-critical loaded windows for an independent optional capability.
// Call only while its native hooks are disabled; never infer an owner from a
// patched loaded prologue. Exception handling stays in the caller so local
// Pattern/vector destructors do not share a function with MSVC SEH.
static bool loadedOptionalContractsImpl(const fp::ImageView& file, const char* group) {
    if (!file.data || !base || !layout.optionalGroupAvailable(group)) return false;
    // Locate each owner in the already validated mapped FILE. The loaded
    // prologue may have our retained trampoline, so never search it to infer
    // a new owner. Exact comparison also detects changed rel32 call targets.
    // The reviewed fixed-base source has no PE base relocations. RIP-relative
    // instructions also retain their bytes when rebased; any different future
    // image must pass exact comparison rather than masking arbitrary changes.
    constexpr size_t owners = sizeof(fp::contract::owners) / sizeof(fp::contract::owners[0]);
    std::vector<uint32_t> addresses(owners, 0);
    unsigned checked = 0;
    for (const auto& field : fp::contract::fields) {
        if (!field.optionalGroup || strcmp(field.optionalGroup, group)) continue;
        if (field.owner >= owners) return false;
        if (!addresses[field.owner]) {
            const auto& owner = fp::contract::owners[field.owner];
            fp::Pattern pattern;
            uint32_t at = 0;
            if (!fp::parsePattern(owner.pattern, pattern) ||
                fp::findPattern(file, pattern, file.textBegin, file.textEnd, &at) != 1)
                return false;
            const int64_t entry = static_cast<int64_t>(at) + owner.entryOffset;
            if (entry < file.textBegin || entry >= file.textEnd ||
                (owner.target < fp::sig::targetCount &&
                 layout.rva[owner.target] != static_cast<uint32_t>(entry)))
                return false;
            addresses[field.owner] = static_cast<uint32_t>(entry);
        }
        fp::Pattern pattern;
        const uint64_t at = static_cast<uint64_t>(addresses[field.owner]) + field.offset;
        if (!fp::parsePattern(field.pattern, pattern) || at > file.textEnd ||
            pattern.bytes.size() > file.textEnd - at ||
            memcmp(reinterpret_cast<const void*>(base + static_cast<uintptr_t>(at)), file.data + at, pattern.bytes.size()))
            return false;
        ++checked;
    }
    if (!checked) return false; // A target group without its reviewed witnesses is not a capability.
    return true;
}
