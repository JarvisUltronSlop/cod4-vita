#pragma once
#include "gfx_vita_opaque_shader.h"

inline bool GfxVitaEarlyAlphaEligible(bool staticWorld, bool viewmodel, bool sourceState,
    bool alphaTest, bool litNormalInputs, bool hud, bool sky, unsigned benchmark) {
    return staticWorld && !viewmodel && sourceState && alphaTest && litNormalInputs
        && !hud && !sky && benchmark == 0;
}

// Keep the normal lookup ahead of discard: moving an implicit-derivative
// texture lookup into divergent surviving lanes can change mip selection.
// Only normal reconstruction/arithmetic moves behind the original predicates.
inline const char* GfxVitaEarlyAlphaPrefetch() { return R"EARLY(#if !defined(VITA_NO_NORMAL_MAP)
    vec4 earlyEncoded = vec4(0.0);
    if (u_has_normal > 0.5)
        earlyEncoded = texture2D(u_normal, v_texcoord);
#endif
)EARLY"; }

inline bool GfxVitaBuildEarlyAlpha(const char* original, bool sunOnly, std::string& result) {
    result.clear();
    if (!original) return false;
    std::string source(original);
    const std::string block = GfxVitaOpaqueAlphaBlock();
    const std::string anchor = "    float normalLength = dot(v_normal, v_normal);";
    const std::string fetch = "vec4 encoded = texture2D(u_normal, v_texcoord);";
    const auto unique = [&](const std::string& part) {
        const auto at = source.find(part);
        return at != std::string::npos && source.find(part, at + part.size()) == std::string::npos;
    };
    if (!unique(block) || !unique(anchor) || !unique(fetch)
        || source.find(anchor) >= source.find(fetch) || source.find(fetch) >= source.find(block)) return false;
    source.erase(source.find(block), block.size());
    if (GfxVitaShaderHasDiscard(source)) return false;
    source.replace(source.find(fetch), fetch.size(), "vec4 encoded = earlyEncoded;");
    source.insert(source.find(anchor), std::string(GfxVitaEarlyAlphaPrefetch()) + block + "\n");
    if (sunOnly) {
        const std::string version = "#version 100\n";
        if (source.find(version) == std::string::npos) return false;
        source.insert(source.find(version) + version.size(), "#define VITA_SUN_ONLY 1\n");
    }
    result.swap(source);
    return true;
}
