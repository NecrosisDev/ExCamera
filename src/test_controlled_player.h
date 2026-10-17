#pragma once
// Include after first_person.cpp in an owned-process fixture. Allocation,
// reciprocal native identities and input reset are shared; each test still
// supplies its own native-call spies and physical keyboard samples.
namespace fixture {
class NativeImage {
    unsigned char* memory = nullptr;
    size_t length = 0;
    uintptr_t previousBase = base;
    fp::Layout previousLayout = layout;

  public:
    explicit NativeImage(size_t bytes) : length(bytes) {
        memory = (unsigned char*)VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (memory) {
            base = (uintptr_t)memory;
            layout = fp::Layout{};
        }
    }
    NativeImage(const NativeImage&) = delete;
    NativeImage& operator=(const NativeImage&) = delete;
    ~NativeImage() { close(); }
    explicit operator bool() const { return memory != nullptr; }
    unsigned char* data() const { return memory; }
    uintptr_t object(size_t offset) const {
        return memory && offset < length ? (uintptr_t)(memory + offset) : 0;
    }
    bool close() {
        if (!memory) return true;
        if (!VirtualFree(memory, 0, MEM_RELEASE)) return false;
        base = previousBase;
        layout = previousLayout;
        memory = nullptr;
        return true;
    }
};
struct ControlledPlayer {
    uintptr_t root = 0, behavior = 0, head = 0, view = 0;
    void connect(bool controlled = true) const {
        *(uintptr_t*)(root + field::character::behaviour) = behavior;
        *(uintptr_t*)(root + field::character::headPart) = head;
        *(uintptr_t*)(behavior + field::behaviour::character) = root;
        *(uintptr_t*)(behavior + field::behaviour::view) = view;
        if (controlled) {
            *(uintptr_t*)gameAt(fp::sig::playerRoot) = root;
            *(uintptr_t*)gameAt(fp::sig::uiView) = view;
        }
    }
};
inline void bindings(const std::array<unsigned, field::bindings::count>& keys) {
    for (size_t i = 0; i < keys.size(); ++i)
        *(unsigned*)(gameAt(fp::sig::movementKeys) + i * field::bindings::stride) = keys[i];
}
inline void resetInput(int mode = 0) {
    // No callbacks run concurrently in these fixtures. This resets the actual
    // owner, rather than relying on unrelated neutral polls to settle it.
    inputOwner.resetQuiescent();
    active = cameraReady = inputEnabled = 1;
    stopping = cursorFree = rmbFree = windowFree = cursorClipped = mouseCaptured = 0;
    viewMode = mode;
    rmbMode = 0;
    rotateHeldKey = leanKey = leanLeftKey = leanRightKey = 0;
    leanKeysMode = false;
    fpManualGuard = tpManualGuard = 0;
    manualDefenseReady = false;
    heldRotating = leaning = duckKey = false;
    leanKeySide = 0;
    guardRequest = {};
    attackHeadingPresentation = {};
    aim = {};
    rawLook = {};
}
} // namespace fixture
