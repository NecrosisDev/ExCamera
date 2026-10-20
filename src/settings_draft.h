#pragma once
#include "settings_model.h"

namespace panel {
class SettingsDraft {
public:
    ModSettings initial, value;
    void begin(const ModSettings& live) { initial = value = live; }
    bool step(int id, int direction) {
        if (!rowInfo(id).editable()) return false;
        value.step(id, direction);
        value.normalize();
        return true;
    }
    bool slider(int id, float position) {
        const auto& info = rowInfo(id);
        const auto& r = info.range;
        if (!info.editable() || !r.count || !info.value || !std::isfinite(position)) return false;
        const int at = std::clamp(int(std::lround(std::clamp(position,0.f,1.f)*float(r.count))),0,r.count);
        value.*(info.value) = r.inherit ? (at==0 ? 0 : fp::fovMin+(at-1)*r.step)
                                      : std::clamp(r.min+at*r.step,r.min,r.max);
        value.normalize();
        return true;
    }
    bool capture(int id, int key) {
        const auto& info = rowInfo(id);
        if (!info.editable() || info.kind!=RowInfo::keyList || !info.value ||
            !validSettingBinding(id, key)) return false;
        value.*(info.value)=key;
        value.normalize();
        return true;
    }
    void resetGroup(int group) { value.resetGroup(group); value.normalize(); }
    bool refreshStatus(const ModSettings& live) {
        if (value.trackingStatus==live.trackingStatus && value.rawMouseFallback==live.rawMouseFallback &&
            value.manualDefenseAvailable==live.manualDefenseAvailable) return false;
        value.trackingStatus=live.trackingStatus;
        value.rawMouseFallback=live.rawMouseFallback;
        value.manualDefenseAvailable=live.manualDefenseAvailable;
        return true;
    }
    static float sliderPosition(const ModSettings& m,int id) {
        const auto& info=rowInfo(id);
        const auto& r=info.range;
        if (!r.count || !info.value) return 0.f;
        const int v=m.*(info.value);
        const int index=r.inherit ? (v ? 1+(v-fp::fovMin)/r.step : 0) :
            int(std::lround(double(v-r.min)/double(r.step)));
        return float(std::clamp(index,0,r.count))/float(r.count);
    }
};
} // namespace panel
