#pragma once
#include "d3d9_resource.h"
#include "sm3_runtime.h"

namespace olvr {
using OutlastEffectConstants=std::vector<std::pair<unsigned,unsigned>>;
inline OutlastEffectConstants outlast_effect_constants(const sm3::Words& shader,bool bloom,bool grain) {
    OutlastEffectConstants result;
    if (bloom && grain) return result;
    const auto info=sm3::inspect(sm3::parse(shader));
    for (const auto& constant:info.constants) {
        // Packed neighbours include DOF limits, movie time and grain offsets.
        // Suppress only the recovered effect-strength channels.
        unsigned mask=0;
        if (!bloom && constant.name=="BloomScaleAndThreshold") mask=1;
        if (!bloom && constant.name=="BloomTintAndScreenBlendThreshold") mask=7;
        if (!grain && constant.name=="GrainParams") mask=2;
        if (!grain && constant.name=="ImageAdjustments1") mask=8;
        if (!mask) continue;
        if (constant.set!=2 || constant.count!=1 || constant.number>=224)
            throw std::invalid_argument("Unsupported native live-effect constant binding");
        for (const auto& other:info.constants)
            if (&other!=&constant && other.set==2 && constant.number>=other.number &&
                constant.number<other.number+other.count)
                throw std::invalid_argument("Native live-effect constant aliases another setting");
        result.emplace_back(constant.number,mask);
    }
    return result;
}
inline void outlast_apply_effect_constants(IDirect3DDevice9& device,const OutlastEffectConstants& constants) {
    for (const auto& entry:constants) {
        float value[4]{};
        gpu_check(device.GetPixelShaderConstantF(entry.first,value,1),"Read authored native effect channels");
        for (unsigned lane=0;lane<4;++lane) if (entry.second&(1u<<lane)) value[lane]=0;
        gpu_check(device.SetPixelShaderConstantF(entry.first,value,1),"Apply live native effect channels");
    }
}
}
