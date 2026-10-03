#ifdef NDEBUG
#undef NDEBUG
#endif
#include <array>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

// Only Vita IO, allocation, checksum primitive and GXM validation are mocked.
// The envelope bounds, cleanup and atomic-publication code below are extracted
// verbatim from the dependency patch, not reimplemented by this test.
using GLboolean=unsigned char;
using SceUID=int;
using SceOff=int64_t;
constexpr GLboolean GL_FALSE=0,GL_TRUE=1;
constexpr int SCE_O_RDONLY=1,SCE_O_CREAT=2,SCE_O_WRONLY=4,SCE_O_TRUNC=8;
constexpr int SCE_SEEK_SET=SEEK_SET,SCE_SEEK_END=SEEK_END;
enum class Fault {None,Open,Write,Sync,Rename,Allocation};
Fault fault=Fault::None;
unsigned event=99,allocations=0;
std::array<FILE*,32> files{};
void vglCod4CacheEvent(unsigned value,unsigned,unsigned){event=value;}
unsigned sceKernelGetProcessTimeLow(){return unsigned(std::chrono::steady_clock::now().time_since_epoch().count());}
SceUID sceIoOpen(const char* path,int flags,int) {
    if(fault==Fault::Open)return -1;
    for(int i=1;i<32;++i)if(!files[i]) {
        files[i]=std::fopen(path,flags==SCE_O_RDONLY?"rb":"wb");return files[i]?i:-1;
    }
    return -1;
}
SceOff sceIoLseek(SceUID id,SceOff offset,int whence){return std::fseek(files[id],long(offset),whence)?-1:std::ftell(files[id]);}
int sceIoRead(SceUID id,void* data,size_t bytes){return int(std::fread(data,1,bytes,files[id]));}
int sceIoWrite(SceUID id,const void* data,size_t bytes){return int(std::fwrite(data,1,bytes-(fault==Fault::Write),files[id]));}
int sceIoSyncByFd(SceUID id,int){return fault==Fault::Sync?-1:std::fflush(files[id]);}
int sceIoClose(SceUID id){const int result=std::fclose(files[id]);files[id]=nullptr;return result;}
int sceIoRemove(const char* path){return std::remove(path);}
int sceIoRename(const char* a,const char* b){return fault==Fault::Rename?-1:std::rename(a,b);}
void* vglMalloc(size_t size){if(fault==Fault::Allocation)return nullptr;void* p=std::malloc(size);if(p)++allocations;return p;}
void vgl_free(void* data){if(data){--allocations;std::free(data);}}
uint64_t VITAGL_XXH3_64bits(const void* data,size_t bytes){
    // The pinned checksum library is outside this IO/ownership unit test.
    uint64_t hash=14695981039346656037ull;
    for(size_t i=0;i<bytes;++i){hash^=static_cast<const uint8_t*>(data)[i];hash*=1099511628211ull;}
    return hash;
}
uint32_t sceGxmProgramGetSize(const void* data){uint32_t size;std::memcpy(&size,data,4);return size;}
int sceGxmProgramCheck(const void* data){uint32_t magic;std::memcpy(&magic,static_cast<const char*>(data)+4,4);return magic==0xfeedcafe?0:-1;}
#ifdef _MSC_VER
#define __attribute__(unused)
#endif
#include "cpu_cache_envelope.inc"

int main() {
#ifdef _MSC_VER
    _set_error_mode(_OUT_TO_STDERR);
    _set_abort_behavior(0,_WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif

    const std::filesystem::path directory="cpu-cache-fixtures-"+std::to_string(sceKernelGetProcessTimeLow());
    std::filesystem::create_directories(directory);
    const auto path=(directory/"test.gxp").string();
    const auto pending=path+".pending";
    assert(!std::filesystem::exists(path) && !std::filesystem::exists(pending));
    std::array<uint8_t,128> payload{};
    for(unsigned i=0;i<payload.size();++i)payload[i]=uint8_t(i);
    size_t size=123;
    assert(!vglCod4CacheRead(path.c_str(),&size,1) && size==0 && event==0);
    vglCod4CacheEnable(GL_TRUE);
    assert(!vglCod4CacheRead(path.c_str(),&size,1) && event==2);
    const auto write=[&]{vglCod4CacheWrite(path.c_str(),payload.data(),payload.size(),1);};
    const auto verify=[&]{
        void* data=vglCod4CacheRead(path.c_str(),&size,1);
        assert(data && size==payload.size() && event==1);
        assert(!std::memcmp(data,payload.data(),size));vgl_free(data);assert(allocations==0);
    };
    write();assert(event==4 && !std::filesystem::exists(pending));verify();
    for(auto error:{Fault::Open,Fault::Write,Fault::Sync,Fault::Rename}) {
        fault=error;write();assert(event==5 && !std::filesystem::exists(pending));
        fault=Fault::None;verify(); // a failed update preserves the committed entry
    }
    vglCod4CacheWrite(path.c_str(),nullptr,payload.size(),1);assert(event==5);verify();
    // Bit corruption, incompatible envelope kind, short files and bad lengths
    // return no cache data and clean up the single invalid entry.
    {std::fstream file(path,std::ios::binary|std::ios::in|std::ios::out);
     file.seekp(sizeof(cod4_cache_header)+7);file.put(char(255));}
    assert(!vglCod4CacheRead(path.c_str(),&size,1) && event==3 && !std::filesystem::exists(path));
    write();assert(!vglCod4CacheRead(path.c_str(),&size,2) && event==3);
    {std::ofstream file(path,std::ios::binary);file<<"bad";}
    assert(!vglCod4CacheRead(path.c_str(),&size,1) && event==3);
    {cod4_cache_header header{0x31435643,1,0xffffffffu,1,0};
     std::ofstream file(path,std::ios::binary);file.write(reinterpret_cast<const char*>(&header),sizeof(header));}
    assert(!vglCod4CacheRead(path.c_str(),&size,1) && event==3 && allocations==0);
    write();fault=Fault::Allocation;
    assert(!vglCod4CacheRead(path.c_str(),&size,1) && event==3 && allocations==0);fault=Fault::None;
    // A valid checksum alone cannot admit an invalid native program.
    std::array<uint32_t,16> program{};program[0]=sizeof(program);program[1]=0;
    vglCod4CacheWrite(path.c_str(),program.data(),sizeof(program),0);
    assert(!vglCod4CacheRead(path.c_str(),&size,0) && event==3 && allocations==0);
    program[1]=0xfeedcafe;vglCod4CacheWrite(path.c_str(),program.data(),sizeof(program),0);
    void* data=vglCod4CacheRead(path.c_str(),&size,0);assert(data && event==1);vgl_free(data);
    assert(!cod4_cache_program_valid(nullptr,128));
    // Exact compiler-buffer/header pairs observed on Vita: three trailing
    // bytes rejected by the former equality check, although GXM accepts them.
    for(uint32_t declared:{3129u,2841u,125u,126u,127u,128u}) {
        const size_t stored=(declared+3u)&~size_t(3u);
        std::vector<uint8_t> padded(stored,0xa5); // padding need not be zero
        std::memcpy(padded.data(),&declared,4);const uint32_t magic=0xfeedcafe;
        std::memcpy(padded.data()+4,&magic,4);
        if(declared==3129 || declared==2841)assert(declared!=stored); // regression trigger
        assert(cod4_cache_program_valid(padded.data(),stored));
        assert(cod4_cache_program_valid(padded.data(),declared));
        // Each successful-publication case starts without an old Windows file.
        if(std::filesystem::exists(path))assert(sceIoRemove(path.c_str())==0);
        vglCod4CacheWrite(path.c_str(),padded.data(),stored,0);
        data=vglCod4CacheRead(path.c_str(),&size,0);assert(data && size==stored && event==1);
        assert(!std::memcmp(data,padded.data(),stored));vgl_free(data);
        // Checksum covers padding too, even though it is not semantic GXP data.
        if(stored!=declared) {
            std::fstream f(path,std::ios::binary|std::ios::in|std::ios::out);
            f.seekp(sizeof(cod4_cache_header)+stored-1);f.put(char(0));f.close();
            assert(!vglCod4CacheRead(path.c_str(),&size,0) && event==3 && allocations==0);
        }
        assert(!cod4_cache_program_valid(padded.data(),declared-1));
        assert(!cod4_cache_program_valid(padded.data(),stored+4));
        padded[4]=0;assert(!cod4_cache_program_valid(padded.data(),stored));
    }
    program[0]=63;assert(!cod4_cache_program_valid(program.data(),sizeof(program)));
    program[0]=0xffffffff;assert(!cod4_cache_program_valid(program.data(),sizeof(program)));
    if(std::filesystem::exists(path))assert(sceIoRemove(path.c_str())==0);
    program[0]=sizeof(program);vglCod4CacheWrite(path.c_str(),program.data(),sizeof(program),0);
    assert(sceIoRemove(path.c_str())==0 && allocations==0);
    for(auto* file:files)assert(!file);
    assert(std::filesystem::remove(directory));
}
