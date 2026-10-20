#pragma once
#include <cmath>
#include <cstdint>
#include <emmintrin.h>

namespace panel {
// Fields used by the verified native 2CE40/2CEB0 coordinate transforms.
struct NativeScrollGeometry {
    int32_t left=0, top=0, right=0, bottom=0, width=0, height=0;
    float horizontal=0, vertical=0;
};
inline bool nativeScrollDisplacement(int32_t extent,int32_t begin,int32_t end,float fraction,int64_t& out) {
    if (extent<0 || int64_t(end)<=int64_t(begin) || !std::isfinite(fraction)) return false;
    const int64_t distance=int64_t(extent)-(int64_t(end)-int64_t(begin));
    // Match CVTSI2SS, MULSS, CVTSS2SI: current MXCSR rounding, not std::round.
    const __m128 product=_mm_mul_ss(_mm_cvtsi64_ss(_mm_setzero_ps(),distance),_mm_set_ss(fraction));
    const float value=_mm_cvtss_f32(product);
    if (!std::isfinite(value) || value < -1073741824.f || value > 1073741824.f) return false;
    out=_mm_cvtss_si64(product);
    return true;
}
inline bool correctNativeCheckboxPoint(uint32_t& packed,const NativeScrollGeometry& g,bool& changed) {
    int64_t sx=0,sy=0;
    if (!nativeScrollDisplacement(g.width,g.left,g.right,g.horizontal,sx) ||
        !nativeScrollDisplacement(g.height,g.top,g.bottom,g.vertical,sy)) return false;
    const int64_t x=int16_t(uint16_t(packed))-2*sx;
    const int64_t y=int16_t(uint16_t(packed>>16))-2*sy;
    if (x < -32768 || x > 32767 || y < -32768 || y > 32767) return false;
    changed=sx!=0 || sy!=0;
    packed=uint32_t(uint16_t(x))|(uint32_t(uint16_t(y))<<16);
    return true;
}
} // namespace panel
