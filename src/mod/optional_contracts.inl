// Proof-critical loaded windows and call-graph closures for an independent optional capability.
// Call only while its native hooks are disabled; never infer an owner from a
// patched loaded prologue. Exception handling stays in the caller so local
// Pattern/vector destructors do not share a function with MSVC SEH.
static bool loadedOptionalContractsImpl(const fp::ImageView& file, const char* group) {
    if (!file.data || !base || !layout.optionalGroupAvailable(group)) return false;
    return fp::validateOptionalClosure(file, layout, group, base,
                                       static_cast<bool (*)(uintptr_t, size_t)>(readable));
}
