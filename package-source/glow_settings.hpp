#pragma once
#include <algorithm>
#include <cmath>
struct GlowSettings {
    int enabled=0, target=2; // 0 hands, 1 weapon, 2 both
    float color[3]={1.f,1.f,1.f};
    float intensity=0.7f;
};
inline GlowSettings NormalizeGlow(GlowSettings value){
    value.enabled=value.enabled?1:0;value.target=std::clamp(value.target,0,2);
    for(auto& channel:value.color)channel=std::isfinite(channel)?std::clamp(channel,0.f,1.f):1.f;
    value.intensity=std::isfinite(value.intensity)?std::clamp(value.intensity,0.f,1.f):0.7f;
    return value;
}
