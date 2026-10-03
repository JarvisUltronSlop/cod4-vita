#pragma once
namespace kisak::vita {
// Absolute user priorities run from 64 (highest) through 191 (lowest).
// The old default-relative 0x10000120 requested 160+32, outside that range.
constexpr int kVitaCpuRecorderPriority=191;

// Preserve both kernel return codes, including when starting a valid thread ID
// fails. The caller owns a successfully started thread; failures own no thread.
template<class Create,class Start,class Dispose,class Report>
int VitaCpuStartRecorder(Create create,Start start,Dispose dispose,Report report) {
    const int id=create(kVitaCpuRecorderPriority);
    report("recorder-create",id);
    if(id<0)return id;
    const int result=start(id);
    report("recorder-start",result);
    if(result<0){dispose(id);return result;}
    return id;
}
}
