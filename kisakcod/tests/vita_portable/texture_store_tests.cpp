#ifdef NDEBUG
#undef NDEBUG
#endif
#include <vita/vita_texture_store.h>
#include <cassert>
#include <filesystem>
#include <vector>
#include <cstdio>
extern int CpuTestLiveThreads();
extern void CpuTestFailNextThreadStart();
int main() {
    using namespace kisak::vita;
    const auto path=std::filesystem::temp_directory_path()/"cod4-portable-texture-store.tmp";
    const auto name=path.string();
    assert(!VitaTextureStore_Start(nullptr));
    CpuTestFailNextThreadStart();
    assert(!VitaTextureStore_Start(name.c_str()) && !CpuTestLiveThreads());
    assert(!VitaTextureStore_Start("missing-texture-directory/file.tmp") && !CpuTestLiveThreads());
    std::vector<unsigned char> source(65539),copy(source.size());
    for(size_t i=0;i<source.size();++i)source[i]=static_cast<unsigned char>(i*17);
    uint32_t stale=0;
    for(unsigned cycle=0;cycle<10;++cycle) {
        assert(VitaTextureStore_Start(name.c_str()));
        assert(!VitaTextureStore_Read(stale,copy.data(),copy.size()));
        auto a=VitaTextureStore_Write(source.data(),source.size());
        auto b=VitaTextureStore_Write(source.data(),7);
        assert(a && b && a!=b && VitaTextureStore_LiveBytes()==source.size()+7);
        assert(!VitaTextureStore_Read(a,copy.data(),source.size()-1));
        assert(VitaTextureStore_Read(a,copy.data(),copy.size()) && source==copy);
        VitaTextureStore_Release(a);
        assert(!VitaTextureStore_Read(a,copy.data(),copy.size()));
        auto replacement=VitaTextureStore_Write(source.data(),source.size());
        assert(replacement && replacement!=a);
        assert(VitaTextureStore_Read(b,copy.data(),7));
        VitaTextureStore_Release(a); // stale release must not release replacement
        assert(VitaTextureStore_Read(replacement,copy.data(),copy.size()) && source==copy);
        assert(!VitaTextureStore_Write(nullptr,1));
        assert(!VitaTextureStore_Write(source.data(),8*1024*1024+1));
        stale=replacement;
        VitaTextureStore_Stop(); // drains live leases on global shutdown
        assert(!CpuTestLiveThreads() && !VitaTextureStore_LiveBytes());
        assert(!std::filesystem::exists(path));
    }
    assert(VitaTextureStore_Start(name.c_str()));
    std::vector<uint32_t> tokens;
    for(unsigned i=0;i<2048;++i) {
        const auto token=VitaTextureStore_Write(source.data(),1);
        assert(token);tokens.push_back(token);
    }
    assert(!VitaTextureStore_Write(source.data(),1));
    VitaTextureStore_Release(tokens[100]);
    auto replacement=VitaTextureStore_Write(source.data(),1);
    assert(replacement && replacement!=tokens[100]);
    assert(!VitaTextureStore_Read(tokens[100],copy.data(),1));
    assert(VitaTextureStore_Read(tokens[101],copy.data(),1));
    VitaTextureStore_Stop();
    VitaTextureStore_Stop();
    assert(!CpuTestLiveThreads());
    std::puts("Texture backing integrity, exhaustion, stale generations and teardown passed");
}
