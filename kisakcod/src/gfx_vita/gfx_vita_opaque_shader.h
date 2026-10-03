#pragma once
#include <string>

inline bool GfxVitaOpaqueShaderEligible(bool staticWorld, bool viewmodel, bool sourceState,
    bool alphaTest, bool blend, bool litInputs, bool hud, bool sky, unsigned benchmark) {
    return staticWorld && !viewmodel && sourceState && !alphaTest && !blend
        && litInputs && !hud && !sky && benchmark==0;
}

// Remove only the exact, unreachable alpha-test block. Reject unexpected
// source changes rather than dropping a different discard or shader equation.
inline const char* GfxVitaOpaqueAlphaBlock() {return R"ALPHA(    if (u_alpha_test_enabled > 0.5) {
        if (u_alpha_test_func < 0.5 && material.a <= 0.0)
            discard;
        if (u_alpha_test_func >= 0.5 && u_alpha_test_func < 1.5
            && material.a >= 0.5)
            discard;
        if (u_alpha_test_func >= 1.5 && material.a < 0.5)
            discard;
    })ALPHA";}
inline bool GfxVitaShaderHasDiscard(const std::string& source) {
    const auto identifier=[](char c){return (c>='a' && c<='z') || (c>='A' && c<='Z') || (c>='0' && c<='9') || c=='_';};
    for(size_t i=0;i<source.size();) {
        if(source.compare(i,2,"//")==0){const auto end=source.find('\n',i+2);i=end==std::string::npos?source.size():end;continue;}
        if(source.compare(i,2,"/*")==0){const auto end=source.find("*/",i+2);if(end==std::string::npos)return true;i=end+2;continue;}
        if(identifier(source[i])){const auto start=i;while(i<source.size() && identifier(source[i]))++i;if(source.compare(start,i-start,"discard")==0)return true;}
        else ++i;
    }
    return false;
}
inline bool GfxVitaBuildOpaqueShader(const char* original,bool sunOnly,std::string& result) {
    result=original;
    const std::string block=GfxVitaOpaqueAlphaBlock();
    const auto at=result.find(block);
    if(at==std::string::npos || result.find(block,at+block.size())!=std::string::npos)return false;
    result.erase(at,block.size());
    if(GfxVitaShaderHasDiscard(result))return false;
    if(sunOnly) {
        const std::string version="#version 100\n";const auto v=result.find(version);
        if(v==std::string::npos)return false;
        result.insert(v+version.size(),"#define VITA_SUN_ONLY 1\n");
    }
    return true;
}
