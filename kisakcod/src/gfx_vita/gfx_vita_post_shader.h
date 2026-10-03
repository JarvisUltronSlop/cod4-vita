#pragma once
#include <string>

inline bool GfxVitaPostFilmEligible(bool gameplay,bool authored,bool film,bool glow) {
    return gameplay && authored && film && !glow;
}
// Resolve only the two feature flags proven by the draw's existing effect
// state. Preserve every expression, texture sample, precision and uniform.
inline bool GfxVitaBuildPostFilm(const char* original,std::string& result) {
    result.clear();if(!original)return false;
    std::string source(original);
    const char* names[]={"u_film_enabled","u_glow_enabled"};
    for(unsigned i=0;i<2;++i) {
        const std::string uniform=std::string("uniform float ")+names[i]+";";
        const auto at=source.find(uniform);
        if(at==std::string::npos || source.find(uniform,at+uniform.size())!=std::string::npos)return false;
        source.replace(at,uniform.size(),std::string("const float ")+names[i]+(i?" = 0.0;":" = 1.0;"));
    }
    result.swap(source);return true;
}
