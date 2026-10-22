#pragma once
#define XR_NO_PROTOTYPES
#include <windows.h>
#include <unknwn.h>
#include <GL/gl.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include "upper_body.h"
#include "full_body.h"
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace fp::xr {
namespace fs = std::filesystem;
inline void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
inline void check(XrResult r, const char* operation) {
    if (XR_FAILED(r)) throw std::runtime_error(std::string(operation) + ": XrResult=" + std::to_string(r));
}
#define FP_XR_CHECK(call) ::fp::xr::check((call), #call)
#define FP_XR_INSTANCE_FUNCTIONS(X) \
    X(xrDestroyInstance) X(xrGetInstanceProperties) X(xrGetSystem) X(xrGetSystemProperties) \
    X(xrEnumerateViewConfigurationViews) X(xrEnumerateEnvironmentBlendModes) \
    X(xrGetOpenGLGraphicsRequirementsKHR) X(xrCreateSession) X(xrDestroySession) \
    X(xrPollEvent) X(xrBeginSession) X(xrEndSession) X(xrRequestExitSession) \
    X(xrCreateReferenceSpace) X(xrEnumerateReferenceSpaces) X(xrDestroySpace) X(xrLocateSpace) \
    X(xrEnumerateSwapchainFormats) X(xrCreateSwapchain) X(xrDestroySwapchain) X(xrEnumerateSwapchainImages) \
    X(xrAcquireSwapchainImage) X(xrWaitSwapchainImage) X(xrReleaseSwapchainImage) \
    X(xrWaitFrame) X(xrBeginFrame) X(xrLocateViews) X(xrEndFrame) \
    X(xrStringToPath) X(xrCreateActionSet) X(xrDestroyActionSet) X(xrCreateAction) \
    X(xrSuggestInteractionProfileBindings) X(xrAttachSessionActionSets) X(xrCreateActionSpace) \
    X(xrSyncActions) X(xrGetActionStatePose) X(xrGetActionStateBoolean) X(xrGetActionStateVector2f) \
    X(xrApplyHapticFeedback) X(xrStopHapticFeedback)

// The Khronos loader is loaded from an explicit directory, never from the machine-wide
// runtime registration, so an emulation run cannot reach a real headset by accident.
struct Api {
    HMODULE loader{};
    PFN_xrGetInstanceProcAddr get{};
    PFN_xrEnumerateInstanceExtensionProperties xrEnumerateInstanceExtensionProperties{};
    PFN_xrCreateInstance xrCreateInstance{};
#define FP_XR_MEMBER(name) PFN_##name name{};
    FP_XR_INSTANCE_FUNCTIONS(FP_XR_MEMBER)
#undef FP_XR_MEMBER
    template <class T> void load(XrInstance instance, const char* name, T& target) {
        FP_XR_CHECK(get(instance, name, reinterpret_cast<PFN_xrVoidFunction*>(&target)));
        require(target != nullptr, "null OpenXR procedure");
    }
    explicit Api(const fs::path& directory) {
        loader = LoadLibraryExW((directory / L"openxr_loader.dll").c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        require(loader != nullptr, "cannot load the local Khronos loader");
        get = reinterpret_cast<PFN_xrGetInstanceProcAddr>(GetProcAddress(loader, "xrGetInstanceProcAddr"));
        require(get != nullptr, "loader has no xrGetInstanceProcAddr");
        load(XR_NULL_HANDLE, "xrEnumerateInstanceExtensionProperties", xrEnumerateInstanceExtensionProperties);
        load(XR_NULL_HANDLE, "xrCreateInstance", xrCreateInstance);
    }
    // XR_EXT_hand_tracking, present only when the runtime offered it at instance creation.
    PFN_xrCreateHandTrackerEXT xrCreateHandTrackerEXT{};
    PFN_xrDestroyHandTrackerEXT xrDestroyHandTrackerEXT{};
    PFN_xrLocateHandJointsEXT xrLocateHandJointsEXT{};
    void bind(XrInstance instance, bool handTracking) {
#define FP_XR_LOAD(name) load(instance, #name, name);
        FP_XR_INSTANCE_FUNCTIONS(FP_XR_LOAD)
#undef FP_XR_LOAD
        if (!handTracking) return;
        load(instance, "xrCreateHandTrackerEXT", xrCreateHandTrackerEXT);
        load(instance, "xrDestroyHandTrackerEXT", xrDestroyHandTrackerEXT);
        load(instance, "xrLocateHandJointsEXT", xrLocateHandJointsEXT);
    }
    ~Api() { if (loader) FreeLibrary(loader); }
    Api(const Api&) = delete;
    Api& operator=(const Api&) = delete;
};

// Emulation admits only the deterministic test runtime; hardware admits any runtime that
// offers OpenGL. The two never mix inside one process.
enum class RuntimePolicy { emulator, hardware };
inline constexpr const char* emulatorRuntimeName = "ExCamera deterministic emulator";

struct EyeChain {
    XrSwapchain chain{};
    int width = 0, height = 0;
    std::vector<XrSwapchainImageOpenGLKHR> images;
};
struct ImageLease {
    Api& api;
    XrSwapchain chain;
    uint32_t index = 0;
    bool ready = false;
    ImageLease(Api& a, XrSwapchain c) : api(a), chain(c) {
        XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        FP_XR_CHECK(api.xrAcquireSwapchainImage(chain, &acquire, &index));
        XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        wait.timeout = XR_INFINITE_DURATION;
        FP_XR_CHECK(api.xrWaitSwapchainImage(chain, &wait));
        ready = true;
    }
    void release() {
        if (!ready) return;
        XrSwapchainImageReleaseInfo info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        FP_XR_CHECK(api.xrReleaseSwapchainImage(chain, &info));
        ready = false;
    }
    ~ImageLease() {
        if (!ready) return;
        XrSwapchainImageReleaseInfo info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        api.xrReleaseSwapchainImage(chain, &info);
    }
    ImageLease(const ImageLease&) = delete;
    ImageLease& operator=(const ImageLease&) = delete;
};

// One instance, session, pair of reference spaces and pair of colour swapchains, bound to the
// caller's current WGL context. The frame loop and tracking live with the owner, not here.
struct Client {
    Api& api;
    XrInstance instance{};
    XrSession session{};
    XrSpace local{}, view{}, stage{}; // stage: the room-fixed space when the runtime offers one, for a remembered origin
    bool handTracking = false; // extension enabled and the system reports support
    bool viveTrackers = false; // XR_HTCX_vive_tracker_interaction enabled: waist and foot trackers by role
    std::string runtimeName;
    std::array<EyeChain, 2> eyes;
    bool begun = false;
    XrTime displayTime = 0;
    explicit Client(Api& a) : api(a) {}
    ~Client() {
        if (begun && api.xrEndFrame) {
            XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};
            end.displayTime = displayTime;
            end.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
            api.xrEndFrame(session, &end);
        }
        for (auto& eye : eyes)
            if (eye.chain && api.xrDestroySwapchain) api.xrDestroySwapchain(eye.chain);
        if (stage && api.xrDestroySpace) api.xrDestroySpace(stage);
        if (view && api.xrDestroySpace) api.xrDestroySpace(view);
        if (local && api.xrDestroySpace) api.xrDestroySpace(local);
        if (session && api.xrDestroySession) api.xrDestroySession(session);
        if (instance && api.xrDestroyInstance) api.xrDestroyInstance(instance);
    }
    void initialize(HDC dc, HGLRC rc, RuntimePolicy policy) {
        uint32_t count = 0;
        FP_XR_CHECK(api.xrEnumerateInstanceExtensionProperties(nullptr, 0, &count, nullptr));
        std::vector<XrExtensionProperties> extensions(count, {XR_TYPE_EXTENSION_PROPERTIES});
        FP_XR_CHECK(api.xrEnumerateInstanceExtensionProperties(nullptr, count, &count, extensions.data()));
        const auto offered = [&](const char* name) { return std::any_of(extensions.begin(), extensions.end(), [&](auto& p) { return std::string(p.extensionName) == name; }); };
        require(offered(XR_KHR_OPENGL_ENABLE_EXTENSION_NAME), "runtime lacks the OpenGL extension");
        const bool handExtension = offered(XR_EXT_HAND_TRACKING_EXTENSION_NAME);
        viveTrackers = offered(XR_HTCX_VIVE_TRACKER_INTERACTION_EXTENSION_NAME);
        const char* enabled[3]{};
        uint32_t enabledCount = 0;
        enabled[enabledCount++] = XR_KHR_OPENGL_ENABLE_EXTENSION_NAME;
        if (handExtension) enabled[enabledCount++] = XR_EXT_HAND_TRACKING_EXTENSION_NAME;
        if (viveTrackers) enabled[enabledCount++] = XR_HTCX_VIVE_TRACKER_INTERACTION_EXTENSION_NAME;
        XrInstanceCreateInfo create{XR_TYPE_INSTANCE_CREATE_INFO};
        strcpy_s(create.applicationInfo.applicationName, "ExCamera VR");
        create.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 34);
        create.enabledExtensionCount = enabledCount;
        create.enabledExtensionNames = enabled;
        FP_XR_CHECK(api.xrCreateInstance(&create, &instance));
        api.bind(instance, handExtension);
        XrInstanceProperties properties{XR_TYPE_INSTANCE_PROPERTIES};
        FP_XR_CHECK(api.xrGetInstanceProperties(instance, &properties));
        runtimeName = properties.runtimeName;
        const bool emulator = runtimeName == emulatorRuntimeName;
        require(emulator == (policy == RuntimePolicy::emulator), "runtime does not match the requested policy");
        XrSystemGetInfo systemInfo{XR_TYPE_SYSTEM_GET_INFO};
        systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
        XrSystemId system{};
        FP_XR_CHECK(api.xrGetSystem(instance, &systemInfo, &system));
        if (handExtension) {
            XrSystemHandTrackingPropertiesEXT handProperties{XR_TYPE_SYSTEM_HAND_TRACKING_PROPERTIES_EXT};
            XrSystemProperties systemProperties{XR_TYPE_SYSTEM_PROPERTIES};
            systemProperties.next = &handProperties;
            FP_XR_CHECK(api.xrGetSystemProperties(instance, system, &systemProperties));
            handTracking = handProperties.supportsHandTracking == XR_TRUE;
        }
        XrGraphicsRequirementsOpenGLKHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_KHR};
        FP_XR_CHECK(api.xrGetOpenGLGraphicsRequirementsKHR(instance, system, &requirements));
        GLint major = 0, minor = 0;
        glGetIntegerv(0x821B, &major);
        glGetIntegerv(0x821C, &minor);
        const XrVersion version = XR_MAKE_VERSION(major, minor, 0);
        require(version >= requirements.minApiVersionSupported && version <= requirements.maxApiVersionSupported, "GL version outside runtime requirements");
        XrGraphicsBindingOpenGLWin32KHR binding{XR_TYPE_GRAPHICS_BINDING_OPENGL_WIN32_KHR};
        binding.hDC = dc;
        binding.hGLRC = rc;
        XrSessionCreateInfo sessionInfo{XR_TYPE_SESSION_CREATE_INFO};
        sessionInfo.next = &binding;
        sessionInfo.systemId = system;
        FP_XR_CHECK(api.xrCreateSession(instance, &sessionInfo, &session));
        XrReferenceSpaceCreateInfo space{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
        space.poseInReferenceSpace.orientation.w = 1;
        space.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
        FP_XR_CHECK(api.xrCreateReferenceSpace(session, &space, &local));
        space.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
        FP_XR_CHECK(api.xrCreateReferenceSpace(session, &space, &view));
        uint32_t spaceCount = 0;
        FP_XR_CHECK(api.xrEnumerateReferenceSpaces(session, 0, &spaceCount, nullptr));
        std::vector<XrReferenceSpaceType> spaceTypes(spaceCount);
        FP_XR_CHECK(api.xrEnumerateReferenceSpaces(session, spaceCount, &spaceCount, spaceTypes.data()));
        if (std::find(spaceTypes.begin(), spaceTypes.end(), XR_REFERENCE_SPACE_TYPE_STAGE) != spaceTypes.end()) {
            space.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE;
            if (api.xrCreateReferenceSpace(session, &space, &stage) != XR_SUCCESS) stage = {};
        }
        uint32_t modes = 0;
        FP_XR_CHECK(api.xrEnumerateEnvironmentBlendModes(instance, system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &modes, nullptr));
        std::vector<XrEnvironmentBlendMode> blend(modes);
        FP_XR_CHECK(api.xrEnumerateEnvironmentBlendModes(instance, system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, modes, &modes, blend.data()));
        require(std::find(blend.begin(), blend.end(), XR_ENVIRONMENT_BLEND_MODE_OPAQUE) != blend.end(), "opaque blend unavailable");
        uint32_t formats = 0;
        FP_XR_CHECK(api.xrEnumerateSwapchainFormats(session, 0, &formats, nullptr));
        std::vector<int64_t> supported(formats);
        FP_XR_CHECK(api.xrEnumerateSwapchainFormats(session, formats, &formats, supported.data()));
        require(std::find(supported.begin(), supported.end(), GL_RGBA8) != supported.end(), "RGBA8 swapchain unavailable");
        uint32_t views = 0;
        FP_XR_CHECK(api.xrEnumerateViewConfigurationViews(instance, system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &views, nullptr));
        require(views == 2, "two stereo views are required");
        std::array<XrViewConfigurationView, 2> configs{{{XR_TYPE_VIEW_CONFIGURATION_VIEW}, {XR_TYPE_VIEW_CONFIGURATION_VIEW}}};
        FP_XR_CHECK(api.xrEnumerateViewConfigurationViews(instance, system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &views, configs.data()));
        for (size_t i = 0; i < eyes.size(); ++i)
            eyes[i] = createChain(int(configs[i].recommendedImageRectWidth), int(configs[i].recommendedImageRectHeight));
    }
    // A colour swapchain of the given size; the caller owns the returned chain's lifetime.
    EyeChain createChain(int width, int height) {
        EyeChain chain{};
        chain.width = width;
        chain.height = height;
        XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
        info.format = GL_RGBA8;
        info.sampleCount = 1;
        info.width = width;
        info.height = height;
        info.faceCount = info.arraySize = info.mipCount = 1;
        FP_XR_CHECK(api.xrCreateSwapchain(session, &info, &chain.chain));
        uint32_t n = 0;
        FP_XR_CHECK(api.xrEnumerateSwapchainImages(chain.chain, 0, &n, nullptr));
        chain.images.resize(n, {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR});
        FP_XR_CHECK(api.xrEnumerateSwapchainImages(chain.chain, n, &n, reinterpret_cast<XrSwapchainImageBaseHeader*>(chain.images.data())));
        return chain;
    }
    void destroyChain(EyeChain& chain) {
        if (chain.chain && api.xrDestroySwapchain) api.xrDestroySwapchain(chain.chain);
        chain = {};
    }
};

// Standard controller grip poses, grasp, stick, trigger and menu through the core interaction
// profiles. No hand-joint or body-tracking extension is assumed.
class Hands {
    Api& api_;
    XrInstance instance_{};
    XrSession session_{};
    XrActionSet set_{};
    XrAction pose_{}, aim_{}, grasp_{}, move_{}, attack_{}, menu_{}, haptic_{}, kick_{};
    std::array<XrPath, 2> paths_{};
    std::array<XrSpace, 2> spaces_{}, aimSpaces_{};
    std::array<XrHandTrackerEXT, 2> trackers_{};
    std::array<bool, 2> fist_{}, pinch_{}, closing_{}; // last gesture state, for the hysteresis
    // Body trackers by role (waist, left foot, right foot) through XR_HTCX_vive_tracker_interaction, when enabled.
    XrAction trackerPose_{};
    std::array<XrPath, 3> trackerPaths_{};
    std::array<XrSpace, 3> trackerSpaces_{};

public:
    float gripAngleDegrees = 0; // applied to controller grips only; a seen hand is already a hand
    bool relaxedGrab = true;    // a tracked hand closing on a thing counts as a grasp, not only a tight fist

private:
    XrPath path(const char* name) {
        XrPath p{};
        FP_XR_CHECK(api_.xrStringToPath(instance_, name, &p));
        return p;
    }

public:
    explicit Hands(Api& api) : api_(api) {}
    ~Hands() {
        for (auto tracker : trackers_)
            if (tracker) api_.xrDestroyHandTrackerEXT(tracker);
        for (auto space : spaces_)
            if (space) api_.xrDestroySpace(space);
        for (auto space : aimSpaces_)
            if (space) api_.xrDestroySpace(space);
        if (set_) api_.xrDestroyActionSet(set_);
    }
    void initialize(Client& client) {
        instance_ = client.instance;
        session_ = client.session;
        paths_ = {path("/user/hand/left"), path("/user/hand/right")};
        XrActionSetCreateInfo set{XR_TYPE_ACTION_SET_CREATE_INFO};
        strcpy_s(set.actionSetName, "upper_body");
        strcpy_s(set.localizedActionSetName, "Upper body");
        FP_XR_CHECK(api_.xrCreateActionSet(instance_, &set, &set_));
        XrActionCreateInfo action{XR_TYPE_ACTION_CREATE_INFO};
        action.countSubactionPaths = 2;
        action.subactionPaths = paths_.data();
        action.actionType = XR_ACTION_TYPE_POSE_INPUT;
        strcpy_s(action.actionName, "grip_pose");
        strcpy_s(action.localizedActionName, "Grip pose");
        FP_XR_CHECK(api_.xrCreateAction(set_, &action, &pose_));
        const auto create = [&](XrAction& out, XrActionType type, const char* name, const char* label) {
            action.actionType = type;
            strcpy_s(action.actionName, name);
            strcpy_s(action.localizedActionName, label);
            FP_XR_CHECK(api_.xrCreateAction(set_, &action, &out));
        };
        create(aim_, XR_ACTION_TYPE_POSE_INPUT, "aim_pose", "Aim pose");
        create(grasp_, XR_ACTION_TYPE_BOOLEAN_INPUT, "grasp", "Grasp");
        create(move_, XR_ACTION_TYPE_VECTOR2F_INPUT, "move", "Move");
        create(attack_, XR_ACTION_TYPE_BOOLEAN_INPUT, "attack", "Attack");
        create(menu_, XR_ACTION_TYPE_BOOLEAN_INPUT, "menu", "Menu");
        create(haptic_, XR_ACTION_TYPE_VIBRATION_OUTPUT, "haptic", "Haptic");
        create(kick_, XR_ACTION_TYPE_BOOLEAN_INPUT, "kick", "Kick");
        // One suggestion per core profile; the runtime picks the one matching the controllers.
        // A boolean bound to a value input is thresholded by the runtime.
        struct Profile { const char* path; const char* grasp; const char* attack; const char* move; const char* menu; const char* kick; };
        static constexpr Profile profiles[]{ // kick: the stick pressed in, where the stick is a separate control from the pad
            {"/interaction_profiles/khr/simple_controller", "select/click", nullptr, nullptr, "menu/click", nullptr},
            {"/interaction_profiles/oculus/touch_controller", "squeeze/value", "trigger/value", "thumbstick", "menu/click", "thumbstick/click"},
            {"/interaction_profiles/valve/index_controller", "squeeze/value", "trigger/click", "thumbstick", "b/click", "thumbstick/click"},
            {"/interaction_profiles/htc/vive_controller", "squeeze/click", "trigger/click", "trackpad", "menu/click", nullptr},
            {"/interaction_profiles/microsoft/motion_controller", "squeeze/click", "trigger/value", "thumbstick", "menu/click", "thumbstick/click"}};
        for (const auto& profile : profiles) {
            std::vector<XrActionSuggestedBinding> bindings;
            const auto both = [&](XrAction act, const char* component, bool leftOnly = false) {
                if (!component) return;
                for (unsigned side = 0; side < (leftOnly ? 1u : 2u); ++side)
                    bindings.push_back({act, path((std::string("/user/hand/") + (side ? "right" : "left") + "/input/" + component).c_str())});
            };
            both(pose_, "grip/pose");
            both(aim_, "aim/pose");
            both(grasp_, profile.grasp);
            both(attack_, profile.attack);
            both(move_, profile.move);
            both(menu_, profile.menu, std::string(profile.path).find("oculus") != std::string::npos); // Touch has menu on the left only
            both(haptic_, "output/haptic");
            // Vive and the simple profile have no stick click: their kick slot is an emulation ini key
            // (KickBindingVive, KickBindingSimple), unbound unless set, and only one of the profile's own clicks.
            const char* kick = profile.kick;
            std::string chosenKick;
            if (!kick) {
                const char* env = std::strstr(profile.path, "vive") ? "EXCAMERA_XR_KICK_VIVE" : std::strstr(profile.path, "simple") ? "EXCAMERA_XR_KICK_SIMPLE" : nullptr;
                char value[64]{};
                if (env && GetEnvironmentVariableA(env, value, sizeof value) > 0) {
                    static constexpr const char* allowed[] = {"trackpad/click", "menu/click", "trigger/click", "squeeze/click", "select/click"};
                    for (const char* a : allowed)
                        if (!std::strcmp(a, value)) chosenKick = a;
                }
                if (!chosenKick.empty()) kick = chosenKick.c_str();
            }
            both(kick_, kick);
            XrInteractionProfileSuggestedBinding suggested{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
            suggested.interactionProfile = path(profile.path);
            suggested.countSuggestedBindings = uint32_t(bindings.size());
            suggested.suggestedBindings = bindings.data();
            FP_XR_CHECK(api_.xrSuggestInteractionProfileBindings(instance_, &suggested));
        }
        if (client.viveTrackers) { // the waist and feet: one pose action over the three role paths, bound on the tracker profile
            trackerPaths_ = {path("/user/vive_tracker_htcx/role/waist"), path("/user/vive_tracker_htcx/role/left_foot"), path("/user/vive_tracker_htcx/role/right_foot")};
            XrActionCreateInfo tracker{XR_TYPE_ACTION_CREATE_INFO};
            tracker.actionType = XR_ACTION_TYPE_POSE_INPUT;
            tracker.countSubactionPaths = 3;
            tracker.subactionPaths = trackerPaths_.data();
            strcpy_s(tracker.actionName, "body_tracker_pose");
            strcpy_s(tracker.localizedActionName, "Body tracker pose");
            FP_XR_CHECK(api_.xrCreateAction(set_, &tracker, &trackerPose_));
            static constexpr const char* roles[3] = {"waist", "left_foot", "right_foot"};
            std::array<XrActionSuggestedBinding, 3> bindings{};
            for (unsigned i = 0; i < 3; ++i) bindings[i] = {trackerPose_, path((std::string("/user/vive_tracker_htcx/role/") + roles[i] + "/input/grip/pose").c_str())};
            XrInteractionProfileSuggestedBinding suggested{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
            suggested.interactionProfile = path("/interaction_profiles/htc/vive_tracker_htcx");
            suggested.countSuggestedBindings = 3;
            suggested.suggestedBindings = bindings.data();
            FP_XR_CHECK(api_.xrSuggestInteractionProfileBindings(instance_, &suggested));
        }
        XrSessionActionSetsAttachInfo attach{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
        attach.countActionSets = 1;
        attach.actionSets = &set_;
        FP_XR_CHECK(api_.xrAttachSessionActionSets(session_, &attach));
        for (unsigned i = 0; i < 2; ++i) {
            XrActionSpaceCreateInfo space{XR_TYPE_ACTION_SPACE_CREATE_INFO};
            space.action = pose_;
            space.subactionPath = paths_[i];
            space.poseInActionSpace.orientation.w = 1;
            FP_XR_CHECK(api_.xrCreateActionSpace(session_, &space, &spaces_[i]));
            space.action = aim_;
            FP_XR_CHECK(api_.xrCreateActionSpace(session_, &space, &aimSpaces_[i]));
        }
        if (trackerPose_)
            for (unsigned i = 0; i < 3; ++i) {
                XrActionSpaceCreateInfo space{XR_TYPE_ACTION_SPACE_CREATE_INFO};
                space.action = trackerPose_;
                space.subactionPath = trackerPaths_[i];
                space.poseInActionSpace.orientation.w = 1;
                FP_XR_CHECK(api_.xrCreateActionSpace(session_, &space, &trackerSpaces_[i]));
            }
        if (client.handTracking)
            for (unsigned i = 0; i < 2; ++i) {
                XrHandTrackerCreateInfoEXT tracker{XR_TYPE_HAND_TRACKER_CREATE_INFO_EXT};
                tracker.hand = i ? XR_HAND_RIGHT_EXT : XR_HAND_LEFT_EXT;
                tracker.handJointSet = XR_HAND_JOINT_SET_DEFAULT_EXT;
                FP_XR_CHECK(api_.xrCreateHandTrackerEXT(session_, &tracker, &trackers_[i]));
            }
    }
    // One pulse on one hand; the runtime picks the frequency. Failures are the caller's to count.
    bool pulse(unsigned side, float amplitude, XrDuration duration) {
        if (side > 1 || !haptic_ || !(amplitude > 0) || !(amplitude <= 1) || duration <= 0) return false;
        XrHapticActionInfo info{XR_TYPE_HAPTIC_ACTION_INFO};
        info.action = haptic_;
        info.subactionPath = paths_[side];
        XrHapticVibration vibration{XR_TYPE_HAPTIC_VIBRATION};
        vibration.duration = duration;
        vibration.frequency = XR_FREQUENCY_UNSPECIFIED;
        vibration.amplitude = amplitude;
        return XR_SUCCEEDED(api_.xrApplyHapticFeedback(session_, &info, reinterpret_cast<const XrHapticBaseHeader*>(&vibration)));
    }
    // Joints from the tracker when it sees the hand; empty otherwise, so the caller falls back.
    HandJoints locateJoints(unsigned i, XrSpace local, XrTime time) {
        HandJoints joints{};
        if (!trackers_[i]) return joints;
        std::array<XrHandJointLocationEXT, handJointCount> locations{};
        XrHandJointsLocateInfoEXT info{XR_TYPE_HAND_JOINTS_LOCATE_INFO_EXT};
        info.baseSpace = local;
        info.time = time;
        XrHandJointLocationsEXT result{XR_TYPE_HAND_JOINT_LOCATIONS_EXT};
        result.jointCount = handJointCount;
        result.jointLocations = locations.data();
        FP_XR_CHECK(api_.xrLocateHandJointsEXT(trackers_[i], &info, &result));
        if (!result.isActive) return joints;
        constexpr auto flags = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT | XR_SPACE_LOCATION_POSITION_TRACKED_BIT;
        for (unsigned j = 0; j < handJointCount; ++j)
            if ((locations[j].locationFlags & flags) == flags && valid(locations[j].pose) && std::isfinite(locations[j].radius)) {
                joints.pose[j] = locations[j].pose;
                joints.radius[j] = locations[j].radius;
                joints.validMask |= 1u << j;
            }
        joints.tracked = joints.valid(palmJoint) && joints.valid(thumbTipJoint) && joints.valid(indexTipJoint);
        return joints;
    }
    bool bodyTrackers() const { return trackerPose_ != XR_NULL_HANDLE; }
    // The waist and feet as the emulator's lower-body sample, after sample() synced the actions this frame.
    bool sampleLower(XrSpace local, XrTime time, LowerSample& out) {
        out = {};
        if (!trackerPose_) return false;
        out.time = time;
        constexpr auto flags = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT | XR_SPACE_LOCATION_POSITION_TRACKED_BIT;
        for (unsigned i = 0; i < 3; ++i) {
            XrActionStateGetInfo get{XR_TYPE_ACTION_STATE_GET_INFO};
            get.action = trackerPose_;
            get.subactionPath = trackerPaths_[i];
            XrActionStatePose pose{XR_TYPE_ACTION_STATE_POSE};
            if (XR_FAILED(api_.xrGetActionStatePose(session_, &get, &pose)) || !pose.isActive) return false;
            XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
            if (XR_FAILED(api_.xrLocateSpace(trackerSpaces_[i], local, time, &location)) || (location.locationFlags & flags) != flags || !valid(location.pose)) return false;
            out.trackers[i] = {location.pose, true};
        }
        return true;
    }
    BodySample sample(XrSpace local, XrTime time, XrPosef head, bool focused, uint64_t epoch) {
        BodySample result{};
        result.head = head;
        result.time = time;
        result.epoch = epoch;
        result.focused = focused;
        XrActiveActionSet actionSet{set_, XR_NULL_PATH};
        XrActionsSyncInfo sync{XR_TYPE_ACTIONS_SYNC_INFO};
        sync.countActiveActionSets = 1;
        sync.activeActionSets = &actionSet;
        const auto synced = api_.xrSyncActions(session_, &sync);
        if (synced == XR_SESSION_NOT_FOCUSED) { result.focused = false; return result; }
        check(synced, "xrSyncActions");
        if (!focused) return result;
        for (unsigned i = 0; i < 2; ++i) {
            XrActionStateGetInfo get{XR_TYPE_ACTION_STATE_GET_INFO};
            get.subactionPath = paths_[i];
            get.action = pose_;
            XrActionStatePose pose{XR_TYPE_ACTION_STATE_POSE};
            FP_XR_CHECK(api_.xrGetActionStatePose(session_, &get, &pose));
            XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
            FP_XR_CHECK(api_.xrLocateSpace(spaces_[i], local, time, &location));
            constexpr auto flags = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT | XR_SPACE_LOCATION_POSITION_TRACKED_BIT;
            auto& hand = result.hands[i];
            hand.tracked = pose.isActive && (location.locationFlags & flags) == flags && valid(location.pose);
            hand.grip = gripAngled(location.pose, gripAngleDegrees);
            XrSpaceLocation aimAt{XR_TYPE_SPACE_LOCATION};
            FP_XR_CHECK(api_.xrLocateSpace(aimSpaces_[i], local, time, &aimAt));
            hand.aimTracked = pose.isActive && (aimAt.locationFlags & flags) == flags && valid(aimAt.pose);
            hand.aim = aimAt.pose;
            const auto button = [&](XrAction act) {
                get.action = act;
                XrActionStateBoolean state{XR_TYPE_ACTION_STATE_BOOLEAN};
                FP_XR_CHECK(api_.xrGetActionStateBoolean(session_, &get, &state));
                return state.isActive && state.currentState;
            };
            hand.pressed = hand.tracked && button(grasp_);
            hand.trigger = button(attack_);
            hand.menu = button(menu_);
            hand.kick = button(kick_);
            get.action = move_;
            XrActionStateVector2f stick{XR_TYPE_ACTION_STATE_VECTOR2F};
            FP_XR_CHECK(api_.xrGetActionStateVector2f(session_, &get, &stick));
            hand.stickActive = stick.isActive && std::isfinite(stick.currentState.x) && std::isfinite(stick.currentState.y);
            hand.stick = hand.stickActive ? stick.currentState : XrVector2f{};
            // A seen hand replaces the controller: the palm is the hand pose, a fist grasps and
            // a pinch attacks. Otherwise the controller's grip and buttons shape an inferred hand.
            hand.joints = locateJoints(i, local, time);
            if (hand.joints.tracked) {
                hand.grip = hand.joints.pose[palmJoint];
                hand.tracked = true;
                fist_[i] = fist(hand.joints, fist_[i]);
                pinch_[i] = pinch(hand.joints, pinch_[i]);
                closing_[i] = relaxedGrab && closing(hand.joints, closing_[i]);
                hand.pressed = hand.pressed || fist_[i] || closing_[i];
                hand.trigger = hand.trigger || pinch_[i];
            } else if (hand.tracked) {
                hand.joints = inferJoints(hand.grip, hand.pressed, i == 1);
            }
        }
        return result;
    }
    Hands(const Hands&) = delete;
    Hands& operator=(const Hands&) = delete;
};
#undef FP_XR_INSTANCE_FUNCTIONS
}
