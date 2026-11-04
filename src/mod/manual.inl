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
    "ExCamera brings a physically grounded first-person and close over-the-shoulder "
    "perspective to Exanima without altering the authority of its simulation.\r\n\r\n"
    "Your view rides the character's true head and neck: balance, momentum, footwork, "
    "weapon reach, and collisions remain governed by the game's own physics.\r\n\r\n\r\n"
    "[algn=2][tcol=FFE399]CAMERA & VIEW CONTROLS[/tcol]\r\n"
    "[algn=0]\r\n"
    "- [tcol=FFE399]F6[/tcol]: Cycle camera views (First Person, Third Person, and Native Overhead), or restart ExCamera after stopping it. "
    "With [tcol=FFE399]F6 cycle[/tcol] set to first and third only, a tap flips First and Third Person and holding F6 for half a second shows the native overhead camera.\r\n"
    "- [tcol=FFE399]Hold F7[/tcol]: Cleanly stop and unload ExCamera, returning control to the native overhead camera.\r\n"
    "- [tcol=FFE399]F8[/tcol]: Open [tcol=FFE399]Settings -> Mod Settings[/tcol] (in play or at the title screen) to configure camera framing, controls, combat options, VR, and visual effects.\r\n"
    "- [tcol=FFE399]F9 / F10 / F11[/tcol]: Narrow, widen, or reset the base Field of View.\r\n"
    "- [tcol=FFE399]Cursor Look[/tcol]: Hold or toggle your native Controls [tcol=FFE399]Cursor Look[/tcol] binding to free the pointer at will. Inventory, containers, corpses, shops, dialogue, and arena menus release the cursor automatically.\r\n"
    "- [tcol=FFE399]Middle Mouse[/tcol]: Immediately align your view with the cursor inside the aim oval.\r\n"
    "- [tcol=FFE399]Lean & Crouch[/tcol]: Hold [tcol=FFE399]Alt[/tcol] and move the mouse to lean your upper body around corners or over tables; press your native duck key ([tcol=FFE399]X[/tcol] by default) outside combat to crouch.\r\n\r\n\r\n"
    "[algn=2][tcol=FFE399]COMBAT & AIMING[/tcol]\r\n"
    "[algn=0]\r\n"
    "Close-view combat preserves Exanima's directional weapon physics:\r\n\r\n"
    "- Small mouse movements steer your weapon and point of aim within the central aim oval; sweeping toward and past its rim turns your head and body.\r\n"
    "- Move your mouse left or right before striking to choose the starting side of a swing, step through blows to build momentum, and thrust with [inpt13].\r\n"
    "- Defense defaults to Exanima's automatic parry; switching Defense to [tcol=FFE399]Manual[/tcol] in Mod Settings lets holding [tcol=FFE399]Right Mouse[/tcol] ready your guard and actively intercept incoming strikes.\r\n"
    "- Optional [tcol=FFE399]Attack Wind-Up Arc[/tcol], [tcol=FFE399]Blade Drag Cuts[/tcol], [tcol=FFE399]Stabs Sink In[/tcol], and unbound [tcol=FFE399]Kick / Jump[/tcol] keys can be tuned or bound under Mod Settings.\r\n\r\n\r\n"
    "[algn=2][tcol=FFE399]WORLD INTERACTION & ITEM HANDLING[/tcol]\r\n"
    "[algn=0]\r\n"
    "Objects, doors, and containers are handled directly in 3D space:\r\n\r\n"
    "- Click and drag loose items, weapons, doors, and lids within arm's reach, or carry items directly between the world and open inventory or container windows.\r\n"
    "- Roll the [tcol=FFE399]Mouse Wheel[/tcol] while holding an object or door to draw it closer or push it farther away; a brisk forward roll on release flings a held object.\r\n"
    "- Hold [tcol=FFE399]Rotate Held Item[/tcol] ([tcol=FFE399]Right Mouse[/tcol] by default, rebindable in Mod Settings) while dragging an item to turn it freely in 3D without moving your view.\r\n\r\n\r\n"
    "[algn=2][tcol=FFE399]SIGHT, SHADERS & IMMERSION[/tcol]\r\n"
    "[algn=0]\r\n"
    "Perspective-aware rendering options can be tailored in [tcol=FFE399]Mod Settings[/tcol] ([tcol=FFE399]F8[/tcol]):\r\n\r\n"
    "- Smooth bounce lighting, extended doorway room visibility, surface detail, articulated finger grips, and persistent combat wounds.\r\n"
    "- Optional Screen-Space Ambient Occlusion, Screen-Space Reflections, Depth of Field, Edge Smoothing, Helmet View framing, and health or stamina sight effects.\r\n";

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
    if (!layout.optionalGroupAvailable("manual")) return;
    const uintptr_t target = optionalGameAt(fp::sig::loadRpkStream);
    // Verify standard prologue: push rbp; mov rbp, rsp; lea rsp, [rsp - 0x50]
    static const uint8_t sig[] = { 0x55, 0x48, 0x89, 0xE5, 0x48, 0x8D, 0x64, 0x24, 0xB0 };
    if (!target || !readable(target, sizeof(sig)) ||
        std::memcmp(reinterpret_cast<const void*>(target), sig, sizeof(sig)) != 0) {
        return;
    }
    manualPageHooked = MH_CreateHook(reinterpret_cast<void*>(target),
                                     reinterpret_cast<void*>(loadRPKFromStreamHook),
                                     reinterpret_cast<void**>(&originalLoadRPKFromStream)) == MH_OK;
}
