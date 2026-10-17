// Native field admission with owned byte buffers; EXANIMA_EXE optionally names a read-only archive.
// No Windows APIs, process attachment, native calls, or executable writes.
#ifdef NDEBUG
#error "The tests require assert()."
#endif
#define _CRT_SECURE_NO_WARNINGS
#include "native_contracts.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <limits>

using namespace fp;

static void sameLayout(const Layout& actual, const Layout& expected) {
    assert(!memcmp(actual.rva, expected.rva, sizeof actual.rva));
    assert(actual.resolved == expected.resolved && actual.moved == expected.moved);
    assert(actual.textBegin == expected.textBegin && actual.textEnd == expected.textEnd);
}

struct Fixture {
    static constexpr uint32_t core = 0x40, optional = 0x140, fieldOffset = 0x20;
    std::vector<uint8_t> bytes = std::vector<uint8_t>(0x400, 0xcc);
    ImageView view{bytes.data(), bytes.size(), 0x140000000ull, 0x20, 0x280};
    Layout layout;
    ContractReport report;
    ContractOwner owners[2] = {
        {"core owner", sig::navSampler, "41 53 48 83 EC 28 90 91", 0},
        {"defense owner", sig::defenseReactive, "41 54 48 83 EC 20 90 92", 0}
    };
    FieldContract fields[2] = {
        {"required pointer", 0, fieldOffset, "48 8B 81 88 0E 00 00", nullptr},
        {"defense state", 1, fieldOffset, "8B 81 34 12 00 00", "manualDefense"}
    };
    Fixture() {
        for (unsigned t = 0; t < sig::targetCount; ++t) layout.rva[t] = 0x5000 + t * 0x20;
        layout.rva[sig::navSampler] = core;
        layout.rva[sig::defenseReactive] = optional;
        layout.textBegin = view.textBegin;
        layout.textEnd = view.textEnd;
        layout.resolved = true;
        layout.moved = true;
        stamp(core, owners[0].pattern);
        stamp(optional, owners[1].pattern);
        stamp(core + fieldOffset, fields[0].pattern);
        stamp(optional + fieldOffset, fields[1].pattern);
        assert(layout.optionalGroupAvailable("manualDefense"));
    }
    void stamp(uint32_t at, const char* text) {
        Pattern p;
        assert(parsePattern(text, p) && at <= bytes.size() && p.bytes.size() <= bytes.size() - at);
        memcpy(bytes.data() + at, p.bytes.data(), p.bytes.size());
    }
    bool validate() {
        return detail::validateContractTables(view, layout, report, owners, 2, fields, 2);
    }
    void ownerStillAt(unsigned owner, uint32_t expected) const {
        Pattern p;
        uint32_t at = 0;
        assert(parsePattern(owners[owner].pattern, p));
        assert(findPattern(view, p, view.textBegin, view.textEnd, &at) == 1);
        assert(int64_t(at) + owners[owner].entryOffset == expected);
    }
    void reject(ContractFailure why, unsigned field = 0) {
        const Layout before = layout;
        assert(!validate());
        sameLayout(layout, before);
        assert(report.checked == 2 && report.why.size() == 2 && report.why[field] == why);
        assert(report.firstRequired && report.firstRequiredReason == why);
    }
    void rip(uint32_t ownerAt, uint32_t target, int32_t add = 4) {
        fields[0].pattern = "48 8B 05 ?? ?? ?? ??";
        fields[0].relativeTo = sig::mainViewport;
        fields[0].displacementOffset = 3;
        fields[0].nextOffset = 7;
        fields[0].relativeAdd = add;
        layout.rva[sig::navSampler] = ownerAt;
        layout.rva[sig::mainViewport] = target;
        stamp(ownerAt, owners[0].pattern);
        const uint32_t at = ownerAt + fields[0].offset;
        stamp(at, fields[0].pattern);
        const int64_t relative = int64_t(target) + add - at - fields[0].nextOffset;
        assert(relative >= INT32_MIN && relative <= INT32_MAX);
        const int32_t encoded = int32_t(relative);
        memcpy(bytes.data() + at + fields[0].displacementOffset, &encoded, sizeof encoded);
    }
};

static void testFieldDrift() {
    {
        Fixture f;
        const Layout before = f.layout;
        assert(f.validate());
        sameLayout(f.layout, before);
        assert(f.report.checked == 2 && f.report.why.size() == 2 && !f.report.failures);
        assert(!f.report.optionalFailures && !f.report.firstRequired && !f.report.firstOptional);
    }
    for (unsigned changedByte : {0u, 3u}) {
        Fixture f;
        // REX.W selects a 64-bit pointer load; byte 3 is the member displacement.
        f.bytes[Fixture::core + Fixture::fieldOffset + changedByte] ^= changedByte ? 1 : 8;
        f.ownerStillAt(0, Fixture::core);
        f.ownerStillAt(1, Fixture::optional);
        f.reject(ContractFailure::changed);
        assert(f.report.failures == 1 && !f.report.optionalFailures);
        assert(!strcmp(f.report.firstRequired, f.fields[0].name));
        assert(f.report.why[1] == ContractFailure::none);
    }
    {
        Fixture f;
        const Layout before = f.layout;
        f.bytes[Fixture::optional + Fixture::fieldOffset + 2] ^= 1;
        f.ownerStillAt(1, Fixture::optional);
        assert(f.validate() && f.report.failures == 1 && f.report.optionalFailures == 1);
        assert(!f.report.firstRequired && f.report.firstOptionalReason == ContractFailure::changed);
        assert(!strcmp(f.report.firstOptional, f.fields[1].name));
        assert(!f.layout.optionalGroupAvailable("manualDefense"));
        unsigned withheld = 0;
        for (unsigned t = 0; t < sig::targetCount; ++t) {
            const char* group = sig::targetOptionalGroups[t];
            if (group && !strcmp(group, "manualDefense")) {
                assert(!f.layout.rva[t]);
                ++withheld;
            } else {
                assert(f.layout.rva[t] == before.rva[t]);
            }
        }
        assert(withheld >= 2 && f.layout.resolved == before.resolved && f.layout.moved == before.moved);
        assert(f.layout.textBegin == before.textBegin && f.layout.textEnd == before.textEnd);
    }
    {
        Fixture f;
        f.bytes[Fixture::core + Fixture::fieldOffset + 3] ^= 1;
        f.bytes[Fixture::optional + Fixture::fieldOffset + 2] ^= 1;
        // The optional downgrade must not leak out of a rejected overall candidate.
        f.reject(ContractFailure::changed);
        assert(f.report.failures == 2 && f.report.optionalFailures == 1);
        assert(f.report.firstOptional && f.layout.optionalGroupAvailable("manualDefense"));
        // Reusing a report after success clears every previous failure and label.
        f.bytes[Fixture::core + Fixture::fieldOffset + 3] ^= 1;
        f.bytes[Fixture::optional + Fixture::fieldOffset + 2] ^= 1;
        assert(f.validate() && !f.report.failures && !f.report.optionalFailures);
        assert(!f.report.firstRequired && !f.report.firstOptional);
        for (ContractFailure why : f.report.why) assert(why == ContractFailure::none);
    }
    puts("PASS: changed offsets and operand widths reject intact owners; optional groups are withheld "
         "together; required rejection preserves the original layout");
}

static void testOwnersAndBounds() {
    {
        Fixture f;
        f.bytes[Fixture::core] = 0xcc;
        f.reject(ContractFailure::ownerMissing);
    }
    {
        Fixture f;
        f.stamp(0x1e0, f.owners[0].pattern);
        f.reject(ContractFailure::ownerAmbiguous);
    }
    {
        Fixture f;
        ++f.layout.rva[sig::navSampler];
        f.reject(ContractFailure::ownerDisagrees);
    }
    {
        Fixture f;
        f.layout.resolved = false;
        f.reject(ContractFailure::unresolvedLayout);
    }
    for (const char* pattern : {static_cast<const char*>(nullptr), "", "GG", "?? ??"}) {
        Fixture f;
        f.owners[0].pattern = pattern;
        f.reject(ContractFailure::badPattern);
        Fixture access;
        access.fields[0].pattern = pattern;
        access.reject(ContractFailure::badPattern);
    }
    {
        Fixture f;
        f.fields[0].owner = std::numeric_limits<unsigned>::max();
        f.reject(ContractFailure::badPattern);
    }
    for (int32_t entryOffset : {INT32_MIN, INT32_MAX}) {
        Fixture f;
        f.owners[0].entryOffset = entryOffset;
        f.reject(ContractFailure::outside);
    }
    for (uint32_t fieldAt : {0x27du, 0x280u, UINT32_MAX}) {
        Fixture f;
        f.fields[0].offset = fieldAt == UINT32_MAX ? fieldAt : fieldAt - Fixture::core;
        f.reject(ContractFailure::outside);
    }
    {
        Fixture f;
        // A complete instruction ending exactly at the code boundary is valid.
        const uint32_t at = f.view.textEnd - 7;
        f.fields[0].offset = at - Fixture::core;
        f.stamp(at, f.fields[0].pattern);
        assert(f.validate());
    }
    for (unsigned broken = 0; broken < 4; ++broken) {
        Fixture f;
        if (broken == 0) f.view.data = nullptr;
        if (broken == 1) f.view.textEnd = uint32_t(f.view.size + 1);
        if (broken == 2) f.view.textBegin = f.view.textEnd;
        if (broken == 3) f.view.textBegin = f.view.textEnd + 1;
        f.reject(ContractFailure::outside);
    }
    {
        Fixture f;
        f.fields[1].optionalGroup = "misspelledOptionalGroup";
        f.bytes[Fixture::optional + Fixture::fieldOffset + 2] ^= 1;
        f.reject(ContractFailure::changed, 1);
        assert(f.report.failures == 1 && !f.report.optionalFailures && !f.report.firstOptional);
    }
    {
        Fixture f;
        // An independently anchored helper need not also be a runtime hook target.
        f.owners[0].target = sig::targetCount;
        f.layout.rva[sig::navSampler] = 0x200;
        assert(f.validate());
    }
    puts("PASS: missing, ambiguous and disagreeing owners; malformed tables; unresolved layouts; "
         "overflow and exact code bounds");
}

static void testRelativeReferences() {
    {
        Fixture f;
        f.rip(Fixture::core, 0x300);
        assert(f.validate());
        // Relink the owner and global separately, updating only the encoded rel32.
        memset(f.bytes.data() + Fixture::core, 0xcc, 0x40);
        f.rip(0x90, 0x320);
        f.view.imageBase += 0x100000;
        f.ownerStillAt(0, 0x90);
        assert(f.validate());
        f.layout.rva[sig::mainViewport] += 4;
        f.reject(ContractFailure::referenceDisagrees);
    }
    {
        Fixture f;
        // Signed addends and backward rel32 references are valid.
        f.rip(Fixture::core, 0x30, -4);
        assert(f.validate());
    }
    {
        Fixture f;
        f.rip(Fixture::core, 0x300);
        ++f.bytes[Fixture::core + Fixture::fieldOffset + 3];
        f.ownerStillAt(0, Fixture::core);
        f.reject(ContractFailure::referenceDisagrees);
    }
    {
        Fixture f;
        f.rip(Fixture::core, 0x300);
        f.layout.rva[sig::mainViewport] = 0;
        f.reject(ContractFailure::referenceDisagrees);
    }
    {
        Fixture f;
        f.rip(Fixture::core, 0x3f8); // The +4 addend leaves exactly four bytes in the image.
        f.fields[0].relativeSize = 4;
        assert(f.validate());
        for (uint32_t width : {8u, 0u, UINT32_MAX}) {
            f.fields[0].relativeSize = width;
            f.reject(ContractFailure::referenceDisagrees);
        }
    }
    for (int32_t relative : {INT32_MIN, INT32_MAX}) {
        Fixture f;
        f.rip(Fixture::core, 0x300);
        memcpy(f.bytes.data() + Fixture::core + Fixture::fieldOffset + 3, &relative, sizeof relative);
        f.reject(ContractFailure::referenceDisagrees);
    }
    for (unsigned invalid = 0; invalid < 3; ++invalid) {
        Fixture f;
        f.rip(Fixture::core, 0x300);
        if (invalid == 0) f.fields[0].displacementOffset = UINT32_MAX;
        if (invalid == 1) f.fields[0].displacementOffset = 4;
        if (invalid == 2) f.fields[0].nextOffset = 8;
        f.reject(ContractFailure::badPattern);
    }
    puts("PASS: relinked RIP references retain their resolved destination; target, displacement and "
         "reference-bound errors are rejected");
}

static void testGeneratedTables() {
    constexpr size_t ownerCount = sizeof contract::owners / sizeof *contract::owners;
    constexpr size_t fieldCount = sizeof contract::fields / sizeof *contract::fields;
    assert(ownerCount && fieldCount);
    unsigned required = 0, optional = 0, references = 0;
    for (const ContractOwner& owner : contract::owners) {
        Pattern p;
        assert(owner.name && *owner.name && owner.target <= sig::targetCount);
        assert(owner.pattern && parsePattern(owner.pattern, p));
    }
    for (const FieldContract& field : contract::fields) {
        Pattern p;
        assert(field.name && *field.name && field.owner < ownerCount);
        assert(field.pattern && parsePattern(field.pattern, p));
        if (field.optionalGroup && *field.optionalGroup) {
            bool declared = false;
            for (const char* group : sig::targetOptionalGroups)
                declared = declared || (group && !strcmp(group, field.optionalGroup));
            assert(declared);
            ++optional;
        } else {
            ++required;
        }
        assert(field.relativeTo <= sig::targetCount);
        if (field.relativeTo != sig::targetCount) {
            assert(field.relativeSize > 0);
            assert(field.displacementOffset <= p.bytes.size());
            assert(p.bytes.size() - field.displacementOffset >= sizeof(int32_t));
            assert(field.nextOffset <= p.bytes.size());
            ++references;
        }
    }
    assert(required && optional);
    printf("PASS: generated tables contain %zu owners, %u required and %u optional contracts "
           "(%u global references)\n", ownerCount, required, optional, references);
}

static void testArchive(const char* path) {
    std::ifstream in(path, std::ios::binary);
    std::vector<uint8_t> file((std::istreambuf_iterator<char>(in)), {});
    if (file.empty()) {
        puts("FAIL: the archived executable named by EXANIMA_EXE could not be read");
        std::exit(1);
    }
    std::vector<uint8_t> mapped;
    ImageView view;
    assert(mapPeFile(file.data(), file.size(), mapped, view));
    Layout layout;
    ResolveReport resolution;
    const bool resolved = resolveLayout(view, layout, resolution);
    for (unsigned t = 0; t < sig::targetCount; ++t)
        if (resolution.why[t] != Unresolved::none)
            printf("  target %s: %s\n", sig::targetNames[t], unresolvedText(resolution.why[t]));
    assert(resolved && layout.resolved && !resolution.failures);
    const Layout before = layout;
    ContractReport report;
    const bool accepted = validateNativeContracts(view, layout, report);
    for (size_t i = 0; i < report.why.size(); ++i)
        if (report.why[i] != ContractFailure::none)
            printf("  field %s: %s\n", contract::fields[i].name, contractFailureText(report.why[i]));
    assert(accepted && !report.failures && !report.optionalFailures);
    assert(report.checked == sizeof contract::fields / sizeof *contract::fields);
    assert(layout.optionalGroupAvailable("manualDefense"));
    sameLayout(layout, before);
    printf("PASS: read-only archive resolves all %u targets and validates all %u native field/reference contracts\n",
           unsigned(sig::targetCount), report.checked);
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    testFieldDrift();
    testOwnersAndBounds();
    testRelativeReferences();
    testGeneratedTables();
    if (const char* archive = std::getenv("EXANIMA_EXE"))
        testArchive(archive);
    else
        puts("SKIP: EXANIMA_EXE is unset; archived executable check was not requested");
    puts("ALL NATIVE CONTRACT TESTS PASSED");
}
