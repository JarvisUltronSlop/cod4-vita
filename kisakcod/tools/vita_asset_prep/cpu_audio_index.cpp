// Offline metadata from the exact decoder used by the Vita. No audio conversion.
#define DR_WAV_IMPLEMENTATION
#define DR_MP3_IMPLEMENTATION
#include <dr_libs/dr_wav.h>
#include <dr_libs/dr_mp3.h>
#include <vita/vita_cpu_audio_index.h>
#include <filesystem>
#include <fstream>
#include <vector>
#include <array>
#include <algorithm>
#include <cstring>
#include <cstdio>
using namespace kisak::vita;
int main(int argc,char** argv) {
    if(argc!=3)return 2;
    const auto root=std::filesystem::absolute(argv[1]);
    std::vector<std::filesystem::path> paths;
    for(const auto& entry:std::filesystem::recursive_directory_iterator(root))if(entry.is_regular_file()) {
        const auto ext=entry.path().extension().string();if(ext==".mp3" || ext==".wav")paths.push_back(entry.path());
    }
    std::sort(paths.begin(),paths.end());std::vector<VitaCpuAudioIndexRecord> records;
    for(const auto& path:paths) {
        VitaCpuAudioIndexRecord r{};const auto name=path.lexically_relative(root).generic_string();
        if(name.size()>=sizeof(r.path))return 3;
        std::copy(name.begin(),name.end(),r.path);r.mp3=path.extension()==".mp3";
        if(r.mp3) {
            drmp3 decoder{};if(!drmp3_init_file(&decoder,path.string().c_str(),nullptr))return 4;
            r.channels=decoder.channels;r.rate=decoder.sampleRate;r.frames=drmp3_get_pcm_frame_count(&decoder);drmp3_uninit(&decoder);
        } else {
            drwav decoder{};if(!drwav_init_file(&decoder,path.string().c_str(),nullptr))return 5;
            r.channels=decoder.channels;r.rate=decoder.sampleRate;r.frames=decoder.totalPCMFrameCount;drwav_uninit(&decoder);
        }
        r.fileBytes=std::filesystem::file_size(path);
        if(!r.frames || !r.rate || r.channels<1 || r.channels>2 || !r.fileBytes)return 6;
        std::array<char,4096> buffer;const auto size=std::min<uint64_t>(buffer.size(),r.fileBytes);
        std::ifstream file(path,std::ios::binary);file.read(buffer.data(),size);r.prefixCrc=VitaCpuAudioCrc(buffer.data(),size);
        file.seekg(r.fileBytes-size);file.read(buffer.data(),size);r.tailCrc=VitaCpuAudioCrc(buffer.data(),size);
        if(!file)return 7;
        records.push_back(r);
    }
    if(records.empty() || records.size()>kCpuAudioIndexMaximum)return 8;
    std::sort(records.begin(),records.end(),[](const auto& a,const auto& b){return std::strcmp(a.path,b.path)<0;});
    VitaCpuAudioIndexHeader header{kCpuAudioIndexMagic,1,uint32_t(records.size()),sizeof(VitaCpuAudioIndexRecord),VitaCpuAudioCrc(records.data(),records.size()*sizeof(records[0]))};
    std::ofstream output(argv[2],std::ios::binary);output.write(reinterpret_cast<const char*>(&header),sizeof(header));
    output.write(reinterpret_cast<const char*>(records.data()),records.size()*sizeof(records[0]));
    std::printf("Indexed %u unchanged streams with the pinned dr_libs decoder\n",header.count);
    return output?0:9;
}
