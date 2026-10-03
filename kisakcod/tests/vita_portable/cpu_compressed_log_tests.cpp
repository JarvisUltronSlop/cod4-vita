#ifdef NDEBUG
#undef NDEBUG
#endif
#include <vita/vita_cpu_compressed_log.h>
#include <cassert>
#include <cstdio>
#include <fstream>
#include <vector>
#ifdef _MSC_VER
#include <crtdbg.h>
#endif
using kisak::vita::VitaCpuCompressedLog;
static VitaCpuCompressedLog logSink;
int main(int argc,char** argv) {
#ifdef _MSC_VER
    _CrtSetReportMode(_CRT_ASSERT,_CRTDBG_MODE_FILE);_CrtSetReportFile(_CRT_ASSERT,_CRTDBG_FILE_STDERR);
#endif
    assert(logSink.Initialize());assert(logSink.ArenaUsed()<=320*1024);
    if(argc==3) {
        std::ifstream input(argv[1],std::ios::binary);std::ofstream output(argv[2],std::ios::binary);
        assert(input && output);char buffer[4096];
        const auto sink=[&](const unsigned char* bytes,size_t count){output.write(reinterpret_cast<const char*>(bytes),count);return bool(output);};
        while(input.read(buffer,sizeof(buffer)) || input.gcount())assert(logSink.Append(buffer,size_t(input.gcount()),sink));
        assert(input.eof());assert(logSink.Flush(sink));return 0;
    }
    assert(argc==1);
    for(size_t size:{0u,1u,32767u,32768u,32769u,262144u}) {
        assert(logSink.Initialize());std::vector<unsigned char> input(size),packed;
        uint32_t rng=123;
        for(auto& byte:input){rng^=rng<<13;rng^=rng>>17;rng^=rng<<5;byte=uint8_t(rng);}
        auto sink=[&](const unsigned char* bytes,size_t count){packed.insert(packed.end(),bytes,bytes+count);return true;};
        for(size_t pos=0;pos<size;pos+=17)assert(logSink.Append(reinterpret_cast<char*>(input.data()+pos),std::min(size-pos,size_t(17)),sink));
        assert(logSink.Flush(sink));const auto before=packed.size();assert(logSink.Flush(sink));assert(before==packed.size());
        size_t offset=0,decoded=0;
        while(offset<packed.size()) {
            z_stream stream{};assert(inflateInit(&stream)==Z_OK);unsigned char output[32768];
            stream.next_in=packed.data()+offset;stream.avail_in=uInt(packed.size()-offset);
            stream.next_out=output;stream.avail_out=sizeof(output);
            assert(inflate(&stream,Z_FINISH)==Z_STREAM_END);
            const size_t count=sizeof(output)-stream.avail_out;assert(count && decoded+count<=input.size());
            assert(!std::memcmp(output,input.data()+decoded,count));decoded+=count;
            offset=packed.size()-stream.avail_in;assert(inflateEnd(&stream)==Z_OK);
        }
        assert(decoded==input.size());
    }
    assert(logSink.Initialize());
    assert(logSink.Append("failure",7,[](const unsigned char*,size_t){return true;}));
    assert(!logSink.Flush([](const unsigned char*,size_t){return false;}));
    std::printf("Bounded zlib arena: %zu bytes; empty/boundary/incompressible/multi-member round trips and sink failure passed\n",logSink.ArenaUsed());
}
