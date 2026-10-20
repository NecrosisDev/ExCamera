// Only ordinary inventory previews with an observed resource job may borrow a
// native cancellation at world commit. Native loading, completion and disposal
// are never replaced. Replaced GUI/scene/source owners remain wholly native.
struct InventoryPreview {
    ULONGLONG generation = 0, ownerEpoch = 0;
    unsigned inputGeneration = 0;
    uintptr_t drag = 0, gui = 0, payload = 0, source = 0, viewport = 0, sector = 0;
    uintptr_t root = 0, scene = 0, camera = 0, controller = 0, job = 0, object = 0;
    unsigned item = 0, flags = 0;
    int mode = 2;
    bool completed = false, observed = false, bounded = false;
};
// Admission and completion run on the native game thread. A worker observing a
// queue call has no TLS admission scope and cannot acquire a transaction.
static thread_local InventoryPreview inventoryPreview;
static thread_local ULONGLONG inventoryGeneration = 0, inventoryQueueScope = 0,
                              inventoryCompletionScope = 0;
static thread_local uintptr_t inventoryCompletionObject = 0;
static bool inventoryPreviewOwner(const InventoryPreview& p) {
    if (!p.generation || p.ownerEpoch != inventoryOwnerEpoch.load() ||
        p.inputGeneration != inputOwner.generation() || !base || !onGameThread() ||
        p.mode != viewMode.load() || p.mode >= 2 ||
        !InterlockedCompareExchange(&active, 0, 0) || InterlockedCompareExchange(&stopping, 0, 0) ||
        !InterlockedCompareExchange(&cameraReady, 0, 0)) return false;
    // Compare live identities before dereferencing retained pointers. This is
    // not a lifetime extension for an old GUI, source, sector or scene.
    if (p.gui != *(uintptr_t*)gameAt(fp::sig::guiManager) ||
        p.drag != *(uintptr_t*)gameAt(fp::sig::dragManager) ||
        p.viewport != *(uintptr_t*)gameAt(fp::sig::mainViewport) ||
        p.root != *(uintptr_t*)gameAt(fp::sig::playerRoot) ||
        p.scene != *(uintptr_t*)gameAt(fp::sig::currentScene) ||
        p.root != savedRoot || p.scene != savedScene || p.camera != savedCamera ||
        p.controller != savedController || pickViewportForCamera(p.camera) != p.viewport) return false;
    if (!readable(p.gui, field::gui::dragPayload + sizeof(uintptr_t)) ||
        !readable(p.drag, field::drag::specialSource + 1) ||
        !readable(p.viewport, field::viewport::sceneManager + sizeof(uintptr_t)) ||
        !readable(p.camera, field::camera::controller + sizeof(uintptr_t)) ||
        *(unsigned*)(p.gui + field::gui::dragFormat) != 0xFB01 ||
        *(uintptr_t*)(p.gui + field::gui::dragPayload) != p.payload ||
        *(uintptr_t*)(p.gui + field::gui::dragSource) != p.source ||
        *(uintptr_t*)(p.drag + field::drag::viewport) != p.viewport ||
        *(unsigned char*)(p.drag + field::drag::specialSource) ||
        *(uintptr_t*)(p.viewport + field::viewport::sceneManager) != p.sector ||
        *(uintptr_t*)(p.camera + field::camera::controller) != p.controller) return false;
    if (!readable(p.payload, field::inventoryPayload::flags + sizeof(unsigned)) ||
        *(uintptr_t*)(p.payload + field::inventoryPayload::source) != p.source ||
        *(unsigned*)(p.payload + field::inventoryPayload::item) != p.item ||
        *(unsigned*)(p.payload + field::inventoryPayload::flags) != p.flags ||
        !p.source || p.source == p.viewport ||
        !readable(p.source, field::inventorySource::merchant + 1) ||
        *(unsigned char*)(p.source + field::inventorySource::special) ||
        *(unsigned char*)(p.source + field::inventorySource::merchant)) return false;
    const uintptr_t table = *(uintptr_t*)p.source;
    return readable(table, field::inventorySource::finishSlot + sizeof(uintptr_t)) &&
           *(uintptr_t*)(table + field::inventorySource::finishSlot) == gameAt(fp::sig::inventorySourceFinish);
}
static ULONGLONG inventoryAdmissionBegin(uintptr_t drag, unsigned format) {
    if (!onGameThread()) return 0;
    __try {
        if (!readable(drag, field::drag::viewport + sizeof(uintptr_t)) ||
            *(uintptr_t*)(drag + field::drag::object) ||
            (*(unsigned*)(drag + field::drag::flags) & 0x1000)) return 0;
        // Every fresh native admission supersedes the previous identity, even
        // if this format/source is unsupported. Reused addresses cannot inherit it.
        inventoryPreview = {};
        inventoryQueueScope = 0;
        const ULONGLONG generation = ++inventoryGeneration;
        if (format != 0xFB01 || !base) return 0;
        InventoryPreview p;
        p.generation = generation;
        p.ownerEpoch = inventoryOwnerEpoch.load();
        p.inputGeneration = inputOwner.generation();
        p.drag = drag;
        p.gui = *(uintptr_t*)gameAt(fp::sig::guiManager);
        if (!readable(p.gui, field::gui::dragPayload + sizeof(uintptr_t))) return 0;
        p.payload = *(uintptr_t*)(p.gui + field::gui::dragPayload);
        p.source = *(uintptr_t*)(p.gui + field::gui::dragSource);
        if (!readable(p.payload, field::inventoryPayload::flags + sizeof(unsigned))) return 0;
        p.item = *(unsigned*)(p.payload + field::inventoryPayload::item);
        p.flags = *(unsigned*)(p.payload + field::inventoryPayload::flags);
        p.viewport = *(uintptr_t*)(drag + field::drag::viewport);
        if (!readable(p.viewport, field::viewport::sceneManager + sizeof(uintptr_t))) return 0;
        p.sector = *(uintptr_t*)(p.viewport + field::viewport::sceneManager);
        p.root = *(uintptr_t*)gameAt(fp::sig::playerRoot);
        p.scene = *(uintptr_t*)gameAt(fp::sig::currentScene);
        p.camera = savedCamera;
        p.controller = savedController;
        p.mode = viewMode.load();
        if (!p.root || !p.scene || !p.sector || !p.camera || !p.controller || !inventoryPreviewOwner(p)) return 0;
        inventoryPreview = p;
        inventoryQueueScope = generation;
        return generation;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static void inventoryAdmissionEnd(ULONGLONG generation) {
    if (generation && inventoryQueueScope == generation) inventoryQueueScope = 0;
}
using ResourceQueue = void (*)(uintptr_t, uintptr_t, uintptr_t, uintptr_t);
static ResourceQueue originalResourceQueue;
static void resourceQueue(uintptr_t queue, uintptr_t loader, uintptr_t completion, uintptr_t job) {
    __try {
        auto& p = inventoryPreview;
        if (inventoryQueueScope && inventoryQueueScope == p.generation && !p.job &&
            loader == gameAt(fp::sig::inventoryLoader) && completion == gameAt(fp::sig::inventoryComplete) &&
            inventoryPreviewOwner(p) && readable(job, field::inventoryJob::object + sizeof(uintptr_t)) &&
            *(unsigned*)(job + field::inventoryJob::item) == p.item &&
            *(unsigned*)(job + field::inventoryJob::flags) == p.flags &&
            *(uintptr_t*)(job + field::inventoryJob::sector) == p.sector)
            p.job = job;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    originalResourceQueue(queue, loader, completion, job);
}
using InventoryComplete = void (*)(uintptr_t, uintptr_t, uintptr_t, uintptr_t);
static InventoryComplete originalInventoryComplete;
static void inventoryComplete(uintptr_t job, uintptr_t failed, uintptr_t third, uintptr_t fourth) {
    ULONGLONG generation = 0;
    uintptr_t object = 0;
    __try {
        const auto& p = inventoryPreview;
        if (p.job == job && !p.completed && inventoryPreviewOwner(p) &&
            readable(job, field::inventoryJob::object + sizeof(uintptr_t)) &&
            *(unsigned*)(job + field::inventoryJob::item) == p.item &&
            *(unsigned*)(job + field::inventoryJob::flags) == p.flags &&
            *(uintptr_t*)(job + field::inventoryJob::sector) == p.sector) {
            generation = p.generation;
            object = *(uintptr_t*)(job + field::inventoryJob::object);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    const auto previous = inventoryCompletionScope;
    const auto previousObject = inventoryCompletionObject;
    inventoryCompletionScope = generation;
    inventoryCompletionObject = object;
    __try {
        originalInventoryComplete(job, failed, third, fourth);
    } __finally {
        inventoryCompletionScope = previous;
        inventoryCompletionObject = previousObject;
        // The job may now be freed: never inspect it after native completion.
        if (generation && inventoryPreview.generation == generation) {
            inventoryPreview.completed = true;
            if (!object || inventoryPreview.object != object) inventoryPreview = {};
        }
    }
}
static ULONGLONG inventoryPlacementBegin(uintptr_t object, uintptr_t viewport) {
    __try {
        auto& p = inventoryPreview;
        if (p.object == object) p.observed = false; // an incomplete new attempt is not a proven failure
        const bool completing = inventoryCompletionScope == p.generation &&
                                inventoryCompletionObject == object && !p.completed;
        if (object && p.job && viewport == p.viewport && inventoryPreviewOwner(p) &&
            (completing || (p.completed && p.object == object)) &&
            *(uintptr_t*)(p.drag + field::drag::object) == object) {
            p.object = object;
            p.observed = false;
            p.bounded = false; // publish only after an explicit result, never on exception
            return p.generation;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return 0;
}
static void inventoryPlacementEnd(ULONGLONG generation, uintptr_t object, int result) {
    __try {
        auto& p = inventoryPreview;
        if (generation && generation == p.generation && p.object == object && inventoryPreviewOwner(p) &&
            *(uintptr_t*)(p.drag + field::drag::object) == object)
            { p.observed = true; p.bounded = result > 0; }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}
static void inventoryPlacementHandoff() {
    // 11F610 can recover through 117AA0/118000 without calling 1183C0. Its
    // outcome is native; an earlier inventory-placement failure must not veto it.
    inventoryPreview.observed = false;
}
using InventoryCommit = uintptr_t (*)(uintptr_t, uintptr_t, unsigned);
static InventoryCommit originalInventoryCommit;
static uintptr_t inventoryCommit(uintptr_t drag, uintptr_t data, unsigned code) {
    __try {
        const auto p = inventoryPreview;
        if (p.completed && p.observed && !p.bounded && p.object && p.drag == drag &&
            data == p.gui + field::gui::dragFormat && inventoryPreviewOwner(p) &&
            *(uintptr_t*)(data + (field::gui::dragSource - field::gui::dragFormat)) == p.source &&
            *(uintptr_t*)(drag + field::drag::object) == p.object &&
            *(unsigned*)(drag + field::drag::flags) == 0x2000)
            code = 0x80000000u; // native cross-source cancel: preview disposal, then source cancel
        if (p.drag == drag && data == p.gui + field::gui::dragFormat) inventoryPreview = {};
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return originalInventoryCommit(drag, data, code);
}
