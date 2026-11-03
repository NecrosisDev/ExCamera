// Manual injection: dynamically appends an "ExCamera (First Person)" reference page
// to Exanima's native MANUAL tab in the main menu without modifying base game archives.
// Part of first_person.cpp (one translation unit): included in order.

#pragma pack(push, 1)
struct ManualRPKEntry {
    char name[16];
    uint32_t offset;
    uint32_t size;
    uint32_t flags;
    uint32_t order;
};
#pragma pack(pop)

using LoadRPKFromStreamFn = void (*)(uintptr_t, uintptr_t);
static LoadRPKFromStreamFn originalLoadRPKFromStream = nullptr;
static bool manualPageHooked = false;

// Formatted rich text for the in-game manual document viewer.
// Exanima's document viewer parses tags: [algn=2] (center), [algn=0] (left),
// [tcol=FFE399] (gold highlight), and input tags [inpt13].
static const char excameraManualPageText[] =
    "\r\n[algn=2][tcol=FFE399]FIRST PERSON (EXCAMERA)[/tcol]\r\n"
    "[algn=0]\r\n"
    "ExCamera provides an immersive, physics-driven first-person and close third-person "
    "perspective designed specifically for Exanima's combat and exploration mechanics.\r\n\r\n"
    "Unlike conventional camera mods, ExCamera maintains full simulation integrity: "
    "character momentum, balance, and ragdoll physics remain completely authoritative.\r\n\r\n\r\n"
    "[algn=2][tcol=FFE399]CAMERA CONTROLS & SHORTCUTS[/tcol]\r\n"
    "[algn=0]\r\n"
    "- [tcol=FFE399]F6[/tcol]: Cycle camera modes (First Person, Third Person Follow, and Native Overhead).\r\n"
    "- [tcol=FFE399]F7[/tcol]: Quick-toggle between First Person and Native Overhead camera.\r\n"
    "- [tcol=FFE399]F8[/tcol]: Open the ExCamera Settings Panel to adjust Field of View (FOV), head bob, turning sensitivity, camera height, and visual shaders.\r\n"
    "- [tcol=FFE399]Mouse Wheel[/tcol]: In Third Person, adjust camera distance. When dragging physics objects, adjust hold reach.\r\n\r\n\r\n"
    "[algn=2][tcol=FFE399]COMBAT & AIMING[/tcol]\r\n"
    "[algn=0]\r\n"
    "First-person combat is direct and tactile:\r\n\r\n"
    "- Face your opponent directly with mouse look to align your guard, thrusts, and weapon reach.\r\n"
    "- Left, right, and overhead swings follow your footwork and mouse sweeps. Move your body through strikes to maximize kinetic impact force.\r\n"
    "- Hold [tcol=FFE399]Right Mouse Button (RMB)[/tcol] with a weapon drawn to engage Manual Guard, actively directing your parry angle against incoming blows.\r\n"
    "- Use the Free Aim reticle to target gaps in enemy armour and execute precise thrusts with [inpt13].\r\n\r\n\r\n"
    "[algn=2][tcol=FFE399]INTERACTION & 3D ROTATION[/tcol]\r\n"
    "[algn=0]\r\n"
    "World manipulation is fully tactile and three-dimensional:\r\n\r\n"
    "- Click and drag items, weapons, and containers smoothly through the environment.\r\n"
    "- While dragging an item with Left Mouse, hold [tcol=FFE399]Right Mouse Button (RMB)[/tcol] to rotate the item in true 3D space (pitch and yaw).\r\n"
    "- Drop items freely onto the floor or surface directly from open containers and inventory slots.\r\n"
    "- Hold [tcol=FFE399]Shift[/tcol] while dragging to lock height or slide across level planes.\r\n\r\n\r\n"
    "[algn=2][tcol=FFE399]VISUAL EFFECTS & IMMERSION[/tcol]\r\n"
    "[algn=0]\r\n"
    "ExCamera features perspective-corrected visual shaders:\r\n\r\n"
    "- Perspective-accurate Screen Space Ambient Occlusion (SSAO) and Screen Space Reflections (SSR).\r\n"
    "- Dynamic eye adaptation when transitioning between torchlit corridors and pitch-black chambers.\r\n"
    "- Helmet visor projection: enclosed headwear dynamically frames your peripheral view without obstructing combat awareness.\r\n"
    "- Depth of field and motion smoothing can be fine-tuned or toggled anytime via [tcol=FFE399]F8[/tcol].\r\n";

static bool tryAugmentManualRPK(uintptr_t rpk, uintptr_t stream) {
    if (!stream || !readable(stream, 16)) return false;
    const uint8_t* origData = *reinterpret_cast<const uint8_t**>(stream);
    const uint32_t origSize = *reinterpret_cast<const uint32_t*>(stream + 8);
    if (!origData || origSize < 392 || !readable(reinterpret_cast<uintptr_t>(origData), origSize)) return false;

    const uint32_t magic = *reinterpret_cast<const uint32_t*>(origData);
    const uint32_t tableSize = *reinterpret_cast<const uint32_t*>(origData + 4);
    // Exanima's manual RPK has magic 0xAFBF0C01, 12 entries (table size 384),
    // and entry 0 is "armour".
    if (magic != 0xAFBF0C01 || tableSize != 384 ||
        std::memcmp(origData + 8, "armour\0", 7) != 0) {
        return false;
    }

    const uint32_t newNumEntries = 13;
    const uint32_t newTableSize = newNumEntries * sizeof(ManualRPKEntry);
    const uint32_t origPayloadOffset = 8 + tableSize; // 392
    const uint32_t origPayloadSize = origSize - origPayloadOffset;
    const uint32_t textLen = static_cast<uint32_t>(sizeof(excameraManualPageText) - 1);

    const uint32_t newTotalSize = 8 + newTableSize + origPayloadSize + textLen;
    std::vector<uint8_t> augBuffer(newTotalSize);

    // Header
    *reinterpret_cast<uint32_t*>(&augBuffer[0]) = magic;
    *reinterpret_cast<uint32_t*>(&augBuffer[4]) = newTableSize;

    // Copy original 12 entries
    std::memcpy(&augBuffer[8], origData + 8, tableSize);

    // Entry 12: ExCamera manual page
    ManualRPKEntry entry12{};
    // "first person" is automatically converted to title-case "First Person"
    // by the game's menu item title constructor.
    strncpy_s(entry12.name, sizeof(entry12.name), "first person", _TRUNCATE);
    entry12.offset = origPayloadSize;
    entry12.size = textLen;
    entry12.flags = 0;
    entry12.order = 12; // Append after saving (which has order 11)
    std::memcpy(&augBuffer[8 + tableSize], &entry12, sizeof(entry12));

    // Copy original payload
    std::memcpy(&augBuffer[8 + newTableSize], origData + origPayloadOffset, origPayloadSize);

    // Append ExCamera manual page text
    std::memcpy(&augBuffer[8 + newTableSize + origPayloadSize], excameraManualPageText, textLen);

    // Synthesize stream descriptor for the game loader
    uintptr_t fakeStream[3] = {};
    fakeStream[0] = reinterpret_cast<uintptr_t>(augBuffer.data());
    fakeStream[1] = static_cast<uintptr_t>(augBuffer.size());
    fakeStream[2] = *reinterpret_cast<const uintptr_t*>(stream + 16);

    originalLoadRPKFromStream(rpk, reinterpret_cast<uintptr_t>(fakeStream));
    return true;
}

static void loadRPKFromStreamHook(uintptr_t rpk, uintptr_t stream) {
    bool handled = false;
    __try {
        handled = tryAugmentManualRPK(rpk, stream);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        handled = false;
    }
    if (!handled) {
        originalLoadRPKFromStream(rpk, stream);
    }
}

static void createManualPageHook() {
    uintptr_t target = base + 0x6F710;
    const uintptr_t callSite = base + 0x1CABC0;
    if (readable(callSite, 5) && *reinterpret_cast<const uint8_t*>(callSite) == 0xE8) {
        const int32_t rel = *reinterpret_cast<const int32_t*>(callSite + 1);
        const uintptr_t dest = callSite + 5 + rel;
        if (readable(dest, 16)) target = dest;
    }
    // Verify standard prologue: push rbp; mov rbp, rsp; lea rsp, [rsp - 0x50]; mov [rbp - 0x28], rbx
    static const uint8_t sig[] = { 0x55, 0x48, 0x89, 0xE5, 0x48, 0x8D, 0x64, 0x24, 0xB0 };
    if (!readable(target, sizeof(sig)) || std::memcmp(reinterpret_cast<const void*>(target), sig, sizeof(sig)) != 0) {
        return;
    }
    manualPageHooked = MH_CreateHook(reinterpret_cast<void*>(target),
                                     reinterpret_cast<void*>(loadRPKFromStreamHook),
                                     reinterpret_cast<void**>(&originalLoadRPKFromStream)) == MH_OK;
}
