#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <set>
#include <iostream>
#define HAVE_SHADER_CACHE 1
#define GL_TRUE 1
#define GL_FALSE 0
#define GL_FRAGMENT_SHADER 35632
#define SHARK_FRAGMENT_SHADER 1
#define SHARK_VERTEX_SHADER 0
#define SHARK_LOG_ERROR 2
#define SCE_SHACCCG_PARAMETERCLASS_MATRIX 1
#define SCE_SHACCCG_PARAMETERCLASS_UNIFORMBLOCK 2
using GLboolean=int;using shark_log_level=int;using SceShaccCgParameter=int;
struct SceGxmProgram {unsigned char data[96];};
struct SceShaccCgCompileOutput {};
struct matrix_uniform {void* ptr;matrix_uniform* chain;};
struct block_uniform {int idx;char name[32];block_uniform* chain;};
struct binds_map {uint32_t data[4];};
struct shader {unsigned type=GL_FRAGMENT_SHADER;binds_map semantics{};int id=0;const SceGxmProgram* prog=nullptr;uint32_t size=0,unif_buf_size=0;char* source=nullptr;matrix_uniform* mat=nullptr;block_uniform* unif_blk=nullptr;};
int compiler_opts=0,compiler_fastmath=0,compiler_fastprecision=0,compiler_fastint=0,gxm_shader_patcher=0;
unsigned allocations=0,failAllocation=0,registered=0,registerCalls=0,clears=0,writes=0,serialized=0;
bool failCompile=false,failRegister=false,failMetadata=false;unsigned compileLogs=1;
std::set<void*> owned;
struct Message {unsigned type,reason;uint64_t id;int line;std::string text;};
std::vector<Message> messages;std::vector<unsigned> events;
void(*logger)(const char*,shark_log_level,int)=nullptr;
void vglCod4ShaderMessage(unsigned type,uint64_t id,unsigned reason,int line,const char* message){messages.push_back({type,reason,id,line,message});}
void* vglMalloc(size_t n){if(++allocations==failAllocation)return nullptr;void* p=std::malloc(n);assert(p);owned.insert(p);return p;}
void vgl_free(const void* p){if(!p)return;assert(owned.erase(const_cast<void*>(p))==1);std::free(const_cast<void*>(p));}
void vgl_fast_memcpy(void* a,const void* b,size_t n){assert(a && b);std::memcpy(a,b,n);}
uint64_t XXH3_64bits(const void*,size_t){return 0x123456789abcdef0ull;}
unsigned sceKernelGetProcessTimeLow(){return 100;}
void shark_install_log_cb(void(*cb)(const char*,shark_log_level,int)){logger=cb;}
SceGxmProgram* shark_compile_shader_extended(const char* source,uint32_t* size,int,int,int,int,int){
    // VitaShaRK consumes this length as input, then writes compiled size on success.
    assert(source && size && *size==std::strlen(source) && *size>0);
    static SceGxmProgram p{};
    if(failCompile){for(unsigned i=0;i<compileLogs;++i)if(logger)logger("synthetic compiler rejection",SHARK_LOG_ERROR,2);return nullptr;}
    *size=sizeof(p);return &p;
}
void shark_clear_output(){++clears;assert(logger==nullptr);}
const SceShaccCgCompileOutput* shark_get_internal_compile_output(){static SceShaccCgCompileOutput o;return failMetadata?nullptr:&o;}
int sceGxmShaderPatcherRegisterProgram(int,const SceGxmProgram*,int* id){++registerCalls;if(failRegister)return -1;*id=7;++registered;return 0;}
void sceGxmShaderPatcherUnregisterProgram(int,int id){assert(id==7 && registered);--registered;}
SceShaccCgParameter sceShaccCgGetFirstParameter(SceShaccCgCompileOutput*){return 1;}
int sceShaccCgGetParameterClass(int p){return p;}
void* sceGxmProgramFindParameterByName(const SceGxmProgram*,const char*){return reinterpret_cast<void*>(1);}
const char* sceShaccCgGetParameterName(int p){return p==1?"matrix":"block";}
int sceShaccCgGetParameterBufferIndex(int){return 1;}
int sceShaccCgGetNextParameter(int p){return p==1?2:0;}
unsigned sceGxmProgramGetDefaultUniformBufferSize(const SceGxmProgram*){return 64;}
unsigned sceGxmProgramParameterGetIndex(const SceGxmProgram*,void*){return 1;}
size_t serialized_shader_size(shader* s,GLboolean binding,uint32_t* count){++serialized;*count=s->mat?1:0;return 4+4*(*count)+(binding?sizeof(binds_map):0)+s->size;}
void cod4_cache_event(unsigned e,unsigned,unsigned){events.push_back(e);}
void vglCod4CacheWrite(const char*,const void* p,size_t n,int){assert(p && n>=64);++writes;}
#include "shader_failure_functions.inc"
#define VGL_MODE_POSTPONED 2
#define PROG_UNLINKED 1
struct program {shader* vshader;shader* fshader;int status;};
int glsl_sema_mode=0,linked=0;
void exercise_postponed_guard(program* p){
#include "shader_postponed_gate.inc"
    ++linked;
}

shader make(){shader s;s.source=static_cast<char*>(vglMalloc(40));std::strcpy(s.source,"line one\nline two\nline three");s.size=std::strlen(s.source);allocations=0;return s;}
void cleanup(shader& s){cod4_shader_release_metadata(&s);if(s.prog){sceGxmShaderPatcherUnregisterProgram(0,s.id);vgl_free(s.prog);}vgl_free(s.source);assert(owned.empty() && !registered);}
void reset(){assert(owned.empty() && !registered);allocations=failAllocation=registerCalls=clears=writes=serialized=0;failCompile=failRegister=failMetadata=false;compileLogs=1;messages.clear();events.clear();}
int main(){
#ifdef _WIN32
 _set_error_mode(_OUT_TO_STDERR);_set_abort_behavior(0,_WRITE_ABORT_MSG|_CALL_REPORTFAULT);
#endif
    for(unsigned mode=0;mode<7;++mode){
        reset();shader s=make();const unsigned size=s.size;
        failCompile=mode==0;failAllocation=mode==1?1:mode==3?2:mode==4?3:0;failRegister=mode==2;failMetadata=mode==5;
        if(mode==6){failCompile=true;compileLogs=40;}
        assert(!compile_shader(&s,GL_TRUE,"test-cache"));
        assert(!s.prog && s.id==0 && s.size==size && s.source && !s.mat && !s.unif_blk);
        assert(!registered && clears==1 && !writes && !serialized && events==std::vector<unsigned>{10});
        assert(messages.back().reason==(failCompile?10u:mode==1?11u:mode==2?12u:13u));
        if(failCompile){assert(messages[0].id==0x123456789abcdef0ull && messages[1].text=="line two");}
        if(mode==6)assert(messages.size()==18); // 8 messages + 8 source lines + truncation + failure.
        cleanup(s);
    }
    for(unsigned mode=0;mode<3;++mode){
        reset();shader s=make();failAllocation=mode==2?4:0;
        assert(compile_shader(&s,mode==0?GL_FALSE:GL_TRUE,"test-cache"));
        assert(s.prog && s.size==96 && !s.source && s.mat && s.unif_blk && registered==1 && clears==1);
        assert(writes==(mode==2?0u:1u));assert(events.back()==(mode==2?5u:6u));cleanup(s);
    }
    for(unsigned mask=0;mask<4;++mask){shader vertex,fragment;SceGxmProgram code{};vertex.prog=(mask&1)?&code:nullptr;fragment.prog=(mask&2)?&code:nullptr;program p{&vertex,&fragment,9};linked=0;glsl_sema_mode=0;exercise_postponed_guard(&p);assert(linked==(mask==3));if(mask!=3)assert(p.status==PROG_UNLINKED && glsl_sema_mode==VGL_MODE_POSTPONED);}
    reset();shader s=make();size_t size=999;char sentinel[256];std::memset(sentinel,0x5a,sizeof(sentinel));
    assert(!serialize_shader(sentinel,&size,&s,GL_TRUE) && size==0 && sentinel[0]==0x5a && !serialized);
    assert(!serialize_shader(sentinel,nullptr,&s,GL_TRUE));cleanup(s);
    std::cout<<"Actual VitaGL compile/serialization: null compiler, allocation, registration, metadata, bounded error, cleanup and successful cache paths passed\n";
}
