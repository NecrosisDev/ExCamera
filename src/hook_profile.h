#pragma once
// One bounded, allocation-free recorder. Only the session worker starts, stops,
// resets or reads a drained capture; callback threads own their individual rows.
#include <array>
#include <atomic>
#include <cstdint>
#include <cstddef>
#include <algorithm>
#include <utility>
#include <type_traits>

namespace fp::profile {
enum class Hook : unsigned {
    nav, navNative, swap, swapNative, viewport, viewportNative,
    meshQueue, meshNative, sector, sectorNative, terrain, terrainNative,
    projection, input, inputNative, pose, poseNative, part, partNative,
    pick, grabPick, pickNative, grabNative, cameraPick, selfMask, selfRestore, sceneWalk,
    viewClearance, level, levelNative, tpAim, tpHide, viewFeatures, cue,
    heldIntegrate, heldNative, cameraLock, memoryQuery, frame,
    wearPass, wearPassNative, wearDraw, wearDrawNative, wearSubmit, wearSubmitNative,
    sampledViews, sampledViewsNative, viewEffects, superSampledViews, superSampledViewsNative, count
};
struct HookInfo { const char* name; const char* kind; };
inline constexpr HookInfo hooks[] = {
    {"nav", "hook"}, {"nav.native", "native"}, {"swap", "hook"}, {"swap.native", "native_wait"},
    {"viewport", "hook"}, {"viewport.native", "native"}, {"mesh_queue", "hook"}, {"mesh_queue.native", "native"},
    {"sector_update", "hook"}, {"sector_update.native", "native"}, {"terrain_pass", "hook"}, {"terrain_pass.native", "native"},
    {"sample_projection_filter", "hook"}, {"input", "hook"}, {"input.native", "native"},
    {"pose_builder", "hook"}, {"pose_builder.native", "native"}, {"part_settle", "hook"}, {"part_settle.native", "native"},
    {"pick_ray", "hook"}, {"grab_pick", "hook"}, {"pick_ray.native", "native"}, {"grab_pick.native", "native"}, {"camera_pose_pick", "phase"},
    {"self_pick_mask", "phase"}, {"self_pick_restore", "phase"}, {"scene_walk", "phase"},
    {"view_clearance", "phase"}, {"level_predicate", "phase"}, {"level_ray.native", "native"},
    {"tp_aim", "phase"}, {"tp_hide_validation", "phase"}, {"view_features", "phase"}, {"attack_cue", "phase"},
    {"held_integrate", "hook"}, {"held_integrate.native", "native"}, {"camera_lock_wait", "wait"},
    {"memory_query", "phase"}, {"present_marker", "marker"},
    {"headwear_colour", "hook"}, {"headwear_colour.native", "native"},
    {"headwear_draw", "hook"}, {"headwear_draw.native", "native"},
    {"headwear_submit", "hook"}, {"headwear_submit.native", "native"},
    {"sampled_views", "hook"}, {"sampled_views.native", "native"}, {"view_effects", "phase"},
    {"supersampled_views", "hook"}, {"supersampled_views.native", "native"}
};
inline constexpr unsigned hookCount = unsigned(Hook::count), threadLimit = 64, depthLimit = 64;
inline constexpr unsigned frameBuckets = 1024; // 0.25 ms intervals; last bucket includes overflow.
static_assert(sizeof hooks / sizeof *hooks == hookCount);
struct Metric {
    uint64_t calls = 0, samples = 0, inclusive = 0, self = 0, maximum = 0, maximumSelf = 0;
};
struct alignas(64) ThreadData {
    uint32_t thread = 0;
    std::array<Metric, hookCount> metrics{};
    uint64_t presents = 0, intervals = 0, frameTicks = 0, maxFrame = 0, lastFrame = 0;
    std::array<uint64_t, frameBuckets> histogram{};
    uint64_t overflowFrames = 0, clockErrors = 0, depthDrops = 0;
    std::array<uint64_t, 4> modes{};
};
struct Token { unsigned depth = 0; bool active = false; };
class Engine {
    using Clock = uint64_t (*)();
    using ThreadId = uint32_t (*)();
    struct Node { Hook hook{}; uint64_t start = 0, children = 0; bool measured = false; };
    struct Context {
        Engine* owner = nullptr;
        unsigned slot = threadLimit, depth = 0;
        uint64_t generation = 0;
        uint32_t random = 0x91e10da5;
        std::array<Node, depthLimit> stack{};
    };
    inline static thread_local Context context_{};
    // One CAS admits a root only while open. Stop closes admission in the same
    // atomic modification order, so zero admitted roots is stable while draining.
    // Bits 0..1: idle/admitting/draining; 2..31: roots; 32..63: generation.
    static constexpr uint64_t rootMask = 0xfffffffcULL, rootUnit = 4;
    std::atomic<uint64_t> state_{0};
    std::atomic<unsigned> nextThread_{0};
    std::atomic<uint64_t> threadDrops_{0};
    std::array<ThreadData, threadLimit> threads_{};
    Clock clock_ = nullptr;
    ThreadId thread_ = nullptr;
    uint64_t generation_ = 0, frequency_ = 0, started_ = 0;
    unsigned every_ = 64;
public:
    bool start(Clock clock, ThreadId thread, uint64_t frequency, unsigned every) {
        if (!clock || !thread || !frequency || !every || (state_.load() & 3)) return false;
        for (auto& row : threads_) { const auto id = row.thread; row = {}; row.thread = id; }
        threadDrops_ = 0;
        clock_ = clock; thread_ = thread; frequency_ = frequency; every_ = every;
        started_ = clock_();
        state_.store((++generation_ << 32) | 1, std::memory_order_release);
        return true;
    }
    void stop() {
        uint64_t s = state_.load(std::memory_order_seq_cst);
        while ((s & 3) == 1 && !state_.compare_exchange_weak(s, (s & ~uint64_t(3)) | 2,
                                                          std::memory_order_seq_cst)) {}
    }
    bool admitting() const { return (state_.load(std::memory_order_seq_cst) & 3) == 1; }
    bool draining() const { return (state_.load(std::memory_order_seq_cst) & 3) == 2; }
    bool ready() const {
        const uint64_t s = state_.load(std::memory_order_acquire);
        return (s & 3) == 2 && !(s & rootMask);
    }
    bool finish() {
        if (!ready()) return false;
        state_.store(generation_ << 32, std::memory_order_release);
        return true;
    }
    Token begin(Hook hook) {
        uint64_t observed = state_.load(std::memory_order_acquire);
        if (!(observed & 3)) return {};
        auto& c = context_;
        if (c.owner != this) {
            if (c.depth) return {};
            c = {}; c.owner = this;
        }
        const bool root = c.depth == 0;
        if (root) {
            if ((observed & 3) != 1) return {};
            const uint64_t epoch = observed >> 32;
            for (;;) {
                if ((observed & 3) != 1 || observed >> 32 != epoch) return {};
                if ((observed & rootMask) == rootMask) { ++threadDrops_; return {}; }
                if (state_.compare_exchange_weak(observed, observed + rootUnit,
                                                std::memory_order_acq_rel)) break;
            }
            if (c.slot == threadLimit) {
                unsigned slot = nextThread_.load(std::memory_order_relaxed);
                while (slot < threadLimit && !nextThread_.compare_exchange_weak(slot, slot + 1,
                                                                              std::memory_order_relaxed)) {}
                if (slot == threadLimit) {
                    threadDrops_.fetch_add(1, std::memory_order_relaxed);
                    state_.fetch_sub(rootUnit, std::memory_order_release);
                    return {};
                }
                c.slot = slot;
                threads_[slot].thread = thread_();
            }
            if (c.generation != observed >> 32) {
                c.generation = observed >> 32;
                c.random ^= threads_[c.slot].thread * 0x9e3779b9u ^ uint32_t(c.generation);
                if (!c.random) c.random = 1;
            }
        }
        auto& row = threads_[c.slot];
        if (c.depth == depthLimit) { ++row.depthDrops; return {}; }
        bool measured = c.depth ? c.stack[0].measured : true;
        if (root && every_ > 1) {
            c.random ^= c.random << 13; c.random ^= c.random >> 17; c.random ^= c.random << 5;
            measured = c.random % every_ == 0;
        }
        ++row.metrics[unsigned(hook)].calls;
        const unsigned depth = c.depth++;
        c.stack[depth] = {hook, measured ? clock_() : 0, 0, measured};
        return {depth, true};
    }
    void end(Token token) {
        if (!token.active) return;
        auto& c = context_;
        auto& row = threads_[c.slot];
        if (c.depth != token.depth + 1) {
            // An escaped native exception must not leave the next callback
            // attached to a stale nesting chain. Report discarded scopes.
            row.depthDrops += c.depth > token.depth + 1 ? c.depth - token.depth - 1 : 1;
            c.depth = token.depth + 1;
        }
        const Node node = c.stack[--c.depth];
        if (node.measured) {
            const uint64_t now = clock_();
            if (now >= node.start) {
                const uint64_t elapsed = now - node.start;
                const uint64_t self = elapsed >= node.children ? elapsed - node.children : 0;
                auto& m = row.metrics[unsigned(node.hook)];
                ++m.samples; m.inclusive += elapsed; m.self += self;
                m.maximum = (std::max)(m.maximum, elapsed); m.maximumSelf = (std::max)(m.maximumSelf, self);
                if (c.depth) c.stack[c.depth - 1].children += elapsed;
            } else ++row.clockErrors;
        }
        if (!c.depth) state_.fetch_sub(rootUnit, std::memory_order_release);
    }
    void present(unsigned mode) {
        const Token token = begin(Hook::frame);
        if (!token.active) return;
        auto& row = threads_[context_.slot];
        const uint64_t now = clock_();
        ++row.presents; ++row.modes[mode < 4 ? mode : 3];
        if (row.lastFrame && now > row.lastFrame) {
            const uint64_t elapsed = now - row.lastFrame;
            ++row.intervals; row.frameTicks += elapsed; row.maxFrame = (std::max)(row.maxFrame, elapsed);
            const double bucket = double(elapsed) * 4000.0 / double(frequency_);
            const unsigned index = bucket >= frameBuckets - 1 ? frameBuckets - 1 : unsigned(bucket);
            ++row.histogram[index];
            if (bucket >= frameBuckets - 1) ++row.overflowFrames;
        } else if (row.lastFrame) ++row.clockErrors;
        row.lastFrame = now;
        end(token);
    }
    // Worker-only access, after ready(): no callback writer remains admitted.
    const auto& rows() const { return threads_; }
    unsigned registeredThreads() const { return (std::min)(nextThread_.load(), threadLimit); }
    uint64_t threadDrops() const { return threadDrops_.load(); }
    uint64_t frequency() const { return frequency_; }
    uint64_t started() const { return started_; }
    unsigned sampleEvery() const { return every_; }
};
inline Engine engine;

// POD-only native boundaries preserve SEH cleanup without adding C++ unwinding
// requirements to the game's callbacks. Callsite-sensitive owners capture their
// original return address before entering this helper.
template<Hook hook, class Function, class... Args>
auto invoke(Function function, Args&&... args) -> decltype(function(std::forward<Args>(args)...)) {
    static_assert(std::is_trivially_copyable_v<Function> &&
                  (std::is_trivially_copyable_v<std::remove_reference_t<Args>> && ...));
    const Token token = engine.begin(hook);
    if (!token.active) return function(std::forward<Args>(args)...);
    // Returning inside __try forces an expensive MSVC local unwind even on
    // success. Fall through the finally and return afterward instead.
    using Result = decltype(function(std::forward<Args>(args)...));
    static_assert(std::is_void_v<Result> || std::is_trivially_copyable_v<Result>);
    if constexpr (std::is_void_v<Result>) {
        __try { function(std::forward<Args>(args)...); }
        __finally { engine.end(token); }
    } else {
        Result result{};
        __try { result = function(std::forward<Args>(args)...); }
        __finally { engine.end(token); }
        return result;
    }
}
} // namespace fp::profile
