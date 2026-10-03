#pragma once
#include "gfx_vita_material_family.h"
#include <vita/vita_texture_pool.h>

// Diagnostic identity only, from the validated installed Shipment specular
// payload. Map/material names and raw GL handles do not establish identity.
constexpr uint32_t kGfxVitaGravelSpecCertificate=0x4752564cu;
inline bool GfxVitaGravelSpecIdentity(const kisak::vita::VitaTextureIdentity& key) {
    const kisak::vita::VitaSha256::Digest expected={0xae,0xc8,0x03,0x4a,0x93,0xdf,0xab,0x38,0x72,0xce,0xf2,0x9a,0x19,0x0e,0xae,0x78,0xac,0x7f,0x61,0x9f,0x1c,0xc4,0x10,0xa3,0xb8,0x39,0x87,0x99,0x3d,0x47,0x9a,0xbd};
    return key.digest==expected && key.width==256 && key.height==256
        && key.levels==9 && key.format==0x35545844u && key.flags==0 && key.bytes==87408;
}
inline unsigned GfxVitaWorldComponentVariant(unsigned phase) {return phase==2?1u:phase==4?2u:0u;}
inline bool GfxVitaWorldComponentEligible(bool transparentStaticWorld,uint32_t surface,uint32_t certificate) {
    return transparentStaticWorld && surface==588 && certificate==kGfxVitaGravelSpecCertificate;
}
inline bool GfxVitaBuildWorldComponent(const char* original,unsigned variant,std::string& result) {
    result.clear();if(!original || variant<1 || variant>2)return false;
    std::string source;if(!GfxVitaBuildWorldAlpha(original,source))return false;
    if(variant==1) {
        const std::string begin="    if (usePrimaryLightmap && useSecondaryLightmap) {";
        const auto branch=source.find(begin);
        if(branch==std::string::npos || source.find(begin,branch+begin.size())!=std::string::npos)return false;
        const auto first=source.find("        vec2 topCoord =",branch);
        const auto end=source.find("        float sourceSunDot =",first);
        const auto alternate=source.find("    } else {",branch);
        if(first==std::string::npos || end==std::string::npos || alternate==std::string::npos || end>=alternate)return false;
        // Remove both secondary fetches and their reconstruction/contribution.
        // Keep primary sun, its shadowing, alpha/discard and fog unchanged.
        source.replace(first,end-first,"        vec3 secondaryRgb = vec3(0.0);\n");
    } else {
        const std::string marker="#if !defined(VITA_NO_SPECULAR)";
        const auto at=source.find(marker);
        if(at==std::string::npos || source.find(marker,at+marker.size())!=std::string::npos)return false;
        source.replace(at,marker.size(),"#if 0 // diagnostic: specular and reflection contribution disabled");
    }
    result.swap(source);return true;
}

// Run66: isolate each contribution while keeping the shared mask, normal and
// reflection construction available to the other branch. Removing only the
// accumulation lets the compiler eliminate work used exclusively by that lobe.
// The original shader and the historical run65 builder above stay unchanged.
inline bool GfxVitaBuildWorldSpecularSplit(const char* original,unsigned variant,std::string& result) {
    result.clear();if(!original || variant<1 || variant>2)return false;
    std::string source;if(!GfxVitaBuildWorldAlpha(original,source))return false;
    const std::string contribution=variant==1
        ? "            lit += clamp(highlight * mask.rgb * u_sun_color\n"
          "                         * u_specular_color_scale * shadowVisibility,\n"
          "                         vec3(0.0), vec3(1.0));\n"
        : "                lit += clamp(environment * mask.rgb\n"
          "                    * (0.04 + 0.16 * fresnel) * shadowVisibility,\n"
          "                    vec3(0.0), vec3(1.0));\n";
    const auto at=source.find(contribution);
    if(at==std::string::npos || source.find(contribution,at+contribution.size())!=std::string::npos)return false;
    source.erase(at,contribution.size());
    result.swap(source);return true;
}
