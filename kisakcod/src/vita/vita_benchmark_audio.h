#pragma once
#include <cstdint>
#include <cstddef>
#include <initializer_list>
#include <string_view>
inline unsigned char VitaBenchmarkAudioPathChar(unsigned char c) {
    if (c=='\\') return '/';
    return c>='A' && c<='Z' ? c+('a'-'A') : c;
}
inline bool VitaBenchmarkAudioPrefix(std::string_view name, std::string_view prefix) {
    if (name.size()<prefix.size()) return false;
    for (std::size_t i=0;i<prefix.size();++i)
        if (VitaBenchmarkAudioPathChar(name[i])!=prefix[i]) return false;
    return true;
}
inline bool VitaBenchmarkBackgroundStream(std::string_view name, bool looping) {
    return looping && (VitaBenchmarkAudioPrefix(name,"ambient/") || VitaBenchmarkAudioPrefix(name,"amb_emitters/")
                       || VitaBenchmarkAudioPrefix(name,"music/"));
}
inline bool VitaBenchmarkPcmStream(std::string_view name) {
    for (auto target : {std::string_view("ambient/amb_middle_east1v7_lr.mp3"),
                        std::string_view("ambient/amb_middle_east1v7_lsrs.mp3")})
        if (name.size()==target.size() && VitaBenchmarkAudioPrefix(name,target)) return true;
    return false;
}
inline uint32_t VitaBenchmarkAudioNameHash(std::string_view name) {
    uint32_t value=2166136261u;
    for (unsigned char c:name) { value^=VitaBenchmarkAudioPathChar(c); value*=16777619u; }
    return value;
}
// Main-thread diagnostic control. Changing case stops previous background
// channels before the fresh source session is loaded. Ordinary play uses zero.
void VitaAudioBenchmark_SetMode(uint32_t mode);
