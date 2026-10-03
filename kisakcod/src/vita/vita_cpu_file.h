#pragma once
#include <cstddef>
#include <cstdint>

namespace kisak::vita {
// A source-owned view over a worker-owned file. Two fixed read windows overlap
// the next read with construction/upload of the current payload. No pack copy.
class VitaCpuFile {
public:
    VitaCpuFile()=default;
    ~VitaCpuFile(){Close();}
    VitaCpuFile(const VitaCpuFile&)=delete;
    VitaCpuFile& operator=(const VitaCpuFile&)=delete;
    bool Open(const char* path);
    bool Read(uint64_t offset,void* destination,size_t bytes);
    void Close();
    bool IsOpen() const{return handle_>=0;}
    uint64_t Size() const{return size_;}
    static bool Reader(void* context,uint64_t offset,void* destination,size_t bytes) {
        return static_cast<VitaCpuFile*>(context)->Read(offset,destination,bytes);
    }
private:
    int handle_=-1,page_=-1;
    uint32_t generation_=0;
    uint64_t size_=0;
};
bool VitaCpuFiles_Start();
void VitaCpuFiles_Drain();
void VitaCpuFiles_Stop();
void VitaCpuFiles_SetGeneration(uint32_t generation);
void VitaCpuFiles_SetProgress(void (*callback)(void*),void* context);
}
