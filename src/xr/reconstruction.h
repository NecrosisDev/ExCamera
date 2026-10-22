#pragma once
#include "depth.h"
#include "frustum.h"
#include <string>

namespace fp::xr {
// The two rendering modes are exclusive: either the world is drawn once per eye, or it is drawn
// once from the head and two eye images are synthesised from colour plus depth. Reconstructing
// an already-stereo image, or replaying the world while reconstructing, is never admitted.
// Filled reconstruction marches on past an occluder to the background the centre view can see,
// instead of leaving the disocclusion transparent; honest holes stay the oracle's reference.
enum class RenderMode { geometryStereo, centerReconstruction, centerReconstructionFilled };
inline bool reconstructs(RenderMode m) { return m != RenderMode::geometryStereo; }

struct CenterView {
    FrameContract contract{};
    std::vector<unsigned char> rgba; // subrect pixels, row-major from the contract's origin
    std::vector<float> depth;        // stored depth, same layout
};
struct EyeImage {
    int width = 0, height = 0;
    std::vector<unsigned char> rgba;
    std::vector<unsigned char> covered; // 0 where no source pixel landed: a disocclusion hole
    size_t holes() const { size_t n = 0; for (auto c : covered) n += !c; return n; }
};
class Reconstructor {
public:
    virtual ~Reconstructor() = default;
    virtual const char* name() const = 0;
    // False with a reason when the host cannot run; callers then keep geometry stereo.
    virtual bool available(std::string& reason) const = 0;
    virtual bool synthesize(const CenterView& source, const std::array<Eye, 2>& eyes, std::array<EyeImage, 2>& out, std::string& reason) = 0;
};

// Reference forward warp on the CPU: every source pixel is unprojected with the contract's
// depth, moved into the eye and splatted with a depth test. It exists to measure the geometry
// path against reconstruction in the emulator (disparity, holes, near-hand occlusion), not to
// ship. Hidden surfaces are not recovered; holes stay holes.
class ReferenceReconstructor final : public Reconstructor {
public:
    const char* name() const override { return "reference-forward-warp"; }
    bool available(std::string&) const override { return true; }
    bool synthesize(const CenterView& source, const std::array<Eye, 2>& eyes, std::array<EyeImage, 2>& out, std::string& reason) override {
        const auto& frame = source.contract;
        if (const char* why = frame.validate()) { reason = why; return false; }
        const size_t count = size_t(frame.subrect.width) * size_t(frame.subrect.height);
        if (source.rgba.size() != count * 4 || source.depth.size() != count) { reason = "source buffers do not match the contract"; return false; }
        const auto centreToWorld = poseMatrix(frame.view);
        for (unsigned i = 0; i < 2; ++i) {
            auto& image = out[i];
            if (image.width < 1 || image.height < 1) { reason = "eye image size"; return false; }
            const auto& eye = eyes[i];
            if (!(eye.projection[0] > 0 && eye.projection[5] > 0)) { reason = "eye projection"; return false; }
            const auto toEye = product(viewMatrix(eye.pose), centreToWorld);
            image.rgba.assign(size_t(image.width) * image.height * 4, 0);
            image.covered.assign(size_t(image.width) * image.height, 0);
            std::vector<float> nearest(size_t(image.width) * image.height, std::numeric_limits<float>::infinity());
            std::vector<unsigned char> exact(size_t(image.width) * image.height, 0);
            const float sentinel = frame.depth.reversed ? 0.f : 1.f;
            // The far sentinel carries no geometry but does carry colour (sky, clear): it moves with
            // head rotation only, so it is warped at a distance where eye translation vanishes.
            const float skyDepth = frame.clipFar > 0 ? frame.clipFar : frame.clipNear * 1.e6f;
            for (int y = 0; y < frame.subrect.height; ++y)
                for (int x = 0; x < frame.subrect.width; ++x) {
                    const size_t at = size_t(y) * frame.subrect.width + x;
                    const float stored = source.depth[at];
                    if (!std::isfinite(stored)) continue;
                    const bool sky = stored == sentinel;
                    const auto p = sky ? unproject(x + frame.subrect.x, y + frame.subrect.y, storedFromNdc(ndcFromViewDepth(skyDepth, frame.projection), frame.depth), frame)
                                       : unproject(x + frame.subrect.x, y + frame.subrect.y, stored, frame);
                    const float v[4] = {p.x, p.y, p.z, 1};
                    float e[4]{};
                    for (int r = 0; r < 4; ++r)
                        for (int k = 0; k < 4; ++k) e[r] += toEye[k * 4 + r] * v[k];
                    const float depth = -e[2];
                    if (!(depth > 0) || !std::isfinite(depth)) continue;
                    const float ndcX = (eye.projection[0] * e[0] + eye.projection[8] * e[2]) / depth;
                    const float ndcY = (eye.projection[5] * e[1] + eye.projection[9] * e[2]) / depth;
                    const float fx = (ndcX + 1) * .5f * float(image.width), fy = (ndcY + 1) * .5f * float(image.height);
                    const int px = int(std::floor(fx)), py = int(std::floor(fy));
                    // A source pixel covers a little more than one target pixel when the eye magnifies
                    // the centre view; the exact landing pixel wins, the footprint fills what nothing else hit.
                    for (int dy = 0; dy < 2; ++dy)
                        for (int dx = 0; dx < 2; ++dx) {
                            const int tx = px + dx, ty = py + dy;
                            if (tx < 0 || ty < 0 || tx >= image.width || ty >= image.height) continue;
                            const size_t target = size_t(ty) * image.width + tx;
                            const bool landing = dx == 0 && dy == 0;
                            const bool nearer = depth < nearest[target];
                            if (landing ? (exact[target] && !nearer) : (exact[target] || !nearer)) continue;
                            nearest[target] = depth;
                            if (landing) exact[target] = 1;
                            image.covered[target] = 1;
                            for (unsigned c = 0; c < 4; ++c) image.rgba[target * 4 + c] = source.rgba[at * 4 + c];
                        }
                }
        }
        return true;
    }
};
}
