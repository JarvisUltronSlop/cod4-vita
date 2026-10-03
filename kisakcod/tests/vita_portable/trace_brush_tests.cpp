#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <array>
#include <limits>
#include <vita/vita_trace_brush_filter.h>
#include <vita/vita_ordered_brush_groups.h>
#include <vector>
#include <fstream>

// Minimal host declarations for the fields used by the extracted native code.
// Native compilation separately checks the actual engine ABI. No collision
// arithmetic is copied into the test: the narrow phase comes from cm_trace.cpp.
#ifndef _MSC_VER
#define __cdecl
#endif
#define iassert(x) assert(x)
#define IS_NAN(x) (!std::isfinite(x))
#define COERCE_UNSIGNED_INT(x) std::bit_cast<uint32_t>(float(x))
static void MyAssertHandler(const char*, int, int, const char*, ...) { std::abort(); }
static float I_fabs(float x) { return std::fabs(x); }
static float Vec3Dot(const float* a, const float* b) { return a[0]*b[0]+a[1]*b[1]+a[2]*b[2]; }
struct cplane_s { float normal[3]; float dist; };
struct cbrushside_t { cplane_s* plane; uint32_t materialNum; };
struct cbrush_t {
    float mins[3]; int contents; float maxs[3]; uint32_t numsides;
    cbrushside_t* sides; int16_t axialMaterialNum[2][3];
};
struct traceWork_t {
    struct { float start[3], end[3], invDelta[3]; } extents;
    float radiusOffset[3], radius, offsetZ;
};
struct trace_t {
    float fraction, normal[3]; int surfaceFlags, contents; const char* material;
    int hitType; uint16_t hitId, modelIndex, partName, partGroup;
    bool allsolid, startsolid, walkable;
};
struct { struct { char material[64]; int surfaceFlags; } materials[8]; } cm;
#include "trace_brush_native.inc"

static uint64_t comparisons = 0, rejected = 0, groupSkipped = 0;
static uint64_t baselineSkipped = 0, groupProbes = 0, baselineProbes = 0;
static void Compare(const trace_t& a, const trace_t& b) {
    // Byte-compare initialized fields, excluding unspecified structure padding.
    assert(std::bit_cast<uint32_t>(a.fraction) == std::bit_cast<uint32_t>(b.fraction));
    assert(std::memcmp(a.normal,b.normal,sizeof(a.normal)) == 0);
    assert(a.surfaceFlags==b.surfaceFlags && a.contents==b.contents && a.material==b.material);
    assert(a.hitType==b.hitType && a.hitId==b.hitId && a.modelIndex==b.modelIndex);
    assert(a.partName==b.partName && a.partGroup==b.partGroup);
    assert(a.allsolid==b.allsolid && a.startsolid==b.startsolid && a.walkable==b.walkable);
    ++comparisons;
}
static void Check(traceWork_t tw, cbrush_t* brushes, size_t count, int mask, float fraction) {
    for (int j=0;j!=3;++j) {
        const float delta=tw.extents.start[j]-tw.extents.end[j];
        tw.extents.invDelta[j]=delta!=0.0f ? 1.0f/delta : 0.0f;
    }
    trace_t source{}, candidate{}, grouped{};
    source.fraction=fraction; source.normal[1]=0.5f; source.walkable=true;
    source.hitType=2; source.hitId=73; source.modelIndex=4; source.partName=9; source.partGroup=11;
    candidate=grouped=source;
    std::vector<uint16_t> membership(count);for(size_t i=0;i<count;++i)membership[i]=uint16_t(i);
    static kisak::vita::OrderedBrushGroups<> groups;
    static kisak::vita::OrderedBrushGroups<2048,false> baseline;
    assert(groups.Build(brushes,membership.data(),count));
    assert(baseline.Build(brushes,membership.data(),count));
    size_t skipUntil=0, baselineUntil=0;
    const kisak::vita::TraceBrushFilter filter(tw.extents.start,tw.extents.end);
    for (size_t i=0;i!=count;++i) {
        if(i>=skipUntil && i%groups.ProbeWidth()==0) {
            const size_t skipped=groups.Skip(brushes,membership.data(),count,i,filter,tw.radiusOffset);
            skipUntil=i+skipped;groupSkipped+=skipped;++groupProbes;
            if(groups.ProbeWidth()==4 && i%16==0 && skipped<std::min<size_t>(16,count-i))++groupProbes;
        }
        if(i>=baselineUntil && i%baseline.ProbeWidth()==0) {
            const size_t skipped=baseline.Skip(brushes,membership.data(),count,i,filter,tw.radiusOffset);
            baselineUntil=i+skipped;baselineSkipped+=skipped;++baselineProbes;
        }
        if (!(brushes[i].contents & mask)) continue;
        CM_TraceThroughBrush(&tw,&brushes[i],&source);
        if (!filter.Reject(brushes[i].mins,brushes[i].maxs,tw.radiusOffset))
            CM_TraceThroughBrush(&tw,&brushes[i],&candidate);
        else ++rejected;
        if(i>=skipUntil && !filter.Reject(brushes[i].mins,brushes[i].maxs,tw.radiusOffset))
            CM_TraceThroughBrush(&tw,&brushes[i],&grouped);
        Compare(source,candidate); // Check after every brush, including equal hits.
        Compare(source,grouped);
    }
}
int main(int argc,char** argv) {
    std::mt19937 rng(0xC0D409);
    auto real=[&](float lo,float hi) { return lo+(hi-lo)*(rng()/4294967296.0f); };
    for(int i=0;i!=8;++i) { std::snprintf(cm.materials[i].material,64,"surface%d",i); cm.materials[i].surfaceFlags=i*17; }
    std::array<cbrush_t,32> brushes{};
    std::array<cplane_s,32> planes{};
    std::array<cbrushside_t,32> sides{};
    for(int run=0;run!=100000;++run) {
        traceWork_t tw{};
        tw.radius=(run%4==0)?0.0f:real(0,32); tw.offsetZ=real(0,48);
        for(int j=0;j!=3;++j) {
            tw.radiusOffset[j]=tw.radius+(j==2?tw.offsetZ:0);
            tw.extents.start[j]=real(-4096,4096);
            tw.extents.end[j]=(run%5==0)?tw.extents.start[j]:tw.extents.start[j]+real(-2048,2048);
        }
        for(size_t i=0;i!=brushes.size();++i) {
            auto& b=brushes[i]; b.contents=1<<(rng()%3);
            for(int j=0;j!=3;++j) {
                const float center=tw.extents.start[j]+real(-2048,2048), half=real(0,512);
                b.mins[j]=center-half; b.maxs[j]=center+half;
                b.axialMaterialNum[0][j]=rng()%8; b.axialMaterialNum[1][j]=rng()%8;
            }
            // A diagonal clipping plane exercises original side/material math.
            planes[i]={{0.6f,0.8f,0.0f},(b.mins[0]+b.maxs[0])*0.3f+(b.mins[1]+b.maxs[1])*0.4f};
            sides[i]={&planes[i],uint32_t(rng()%8)}; b.sides=&sides[i]; b.numsides=run%2;
        }
        brushes[31]=brushes[0]; // Identical/tied geometry at a later source index.
        Check(tw,brushes.data(),brushes.size(),1+(rng()%7),(run%7==0)?0.0f:((run%3==0)?real(0,1):1.0f));
    }
    // Exact tolerance, adjacent floats, tangency, stationary/startsolid/allsolid,
    // reversed endpoints and large coordinates, on every axis and face.
    for(float origin : {0.0f,4096.0f,-4096.0f,1048576.0f})
    for(float radius : {0.0f,15.0f,32.0f})
    for(int axis=0;axis!=3;++axis) for(int sign : {-1,1}) {
        cbrush_t b{}; b.contents=1;
        traceWork_t tw{}; tw.radius=radius;
        for(int j=0;j!=3;++j) { b.mins[j]=origin-16; b.maxs[j]=origin+16; tw.radiusOffset[j]=radius; tw.extents.start[j]=tw.extents.end[j]=origin; }
        float edge=origin+sign*(16+radius+0.125f);
        for(float from : {origin,edge,std::nextafter(edge,-INFINITY),std::nextafter(edge,INFINITY)})
        for(float to : {origin,edge,std::nextafter(edge,-INFINITY),std::nextafter(edge,INFINITY)}) {
            tw.extents.start[axis]=from; tw.extents.end[axis]=to;
            Check(tw,&b,1,1,1.0f);
        }
    }
    if(argc==2) {
        // Exact read-only VITC sidecar: use real bounds and side planes through
        // the extracted source narrow phase. World membership preserves order.
        std::ifstream file(argv[1],std::ios::binary);assert(file);
        std::vector<uint8_t> data((std::istreambuf_iterator<char>(file)),{});
        auto u32=[&](size_t off){uint32_t v;assert(off+4<=data.size());std::memcpy(&v,data.data()+off,4);return v;};
        auto f32=[&](size_t off){float v;assert(off+4<=data.size());std::memcpy(&v,data.data()+off,4);return v;};
        auto u16=[&](size_t off){uint16_t v;assert(off+2<=data.size());std::memcpy(&v,data.data()+off,2);return v;};
        assert(u32(0)==0x43544956 && u32(4)==3);
        const uint32_t planeCount=u32(8),brushCount=u32(12),sideCount=u32(16),vertexCount=u32(20),triangleCount=u32(24),modelCount=u32(28);
        const size_t brushOffset=32+planeCount*18,sideOffset=brushOffset+brushCount*36;
        const size_t membershipOffset=sideOffset+sideCount*24+vertexCount*12+triangleCount*6+modelCount*28+sideCount*4;
        assert(u32(membershipOffset)==modelCount);
        const size_t membersBase=membershipOffset+8+(modelCount+1)*4;
        const uint32_t worldCount=u32(membershipOffset+12);assert(u32(membershipOffset+8)==0);
        std::vector<cbrush_t> actual(worldCount);std::vector<cplane_s> actualPlanes(sideCount);std::vector<cbrushside_t> actualSides(sideCount);
        for(uint32_t i=0;i<sideCount;++i) {
            size_t o=sideOffset+i*24;
            for(int a=0;a<3;++a)actualPlanes[i].normal[a]=f32(o+a*4);
            actualPlanes[i].dist=f32(o+12);actualSides[i]={&actualPlanes[i],u32(o+20)%8};
        }
        for(uint32_t i=0;i<worldCount;++i) {
            const auto id=u16(membersBase+i*2);assert(id<brushCount);const size_t o=brushOffset+id*36;
            auto& brush=actual[i];for(int a=0;a<3;++a){brush.mins[a]=f32(o+a*4);brush.maxs[a]=f32(o+16+a*4);}
            brush.contents=int(u32(o+12));brush.numsides=u32(o+32);assert(u32(o+28)+brush.numsides<=sideCount);brush.sides=actualSides.data()+u32(o+28);
        }
        const auto before=groupSkipped, oldBefore=baselineSkipped;
        const auto probesBefore=groupProbes, oldProbesBefore=baselineProbes;
        for(unsigned q=0;q<2000;++q) {
            traceWork_t tw{};tw.radius=q%2?15:0;tw.offsetZ=q%2?20:0;
            for(int a=0;a<3;++a) {
                tw.radiusOffset[a]=tw.radius+(a==2?tw.offsetZ:0);
                tw.extents.start[a]=a==2?real(-40,300):real(-1000,1000);
                // Include long point traces as well as short capsule movement.
                const float length=q%4==0?2000.0f:32.0f;
                tw.extents.end[a]=tw.extents.start[a]+real(-length,length);
            }
            Check(tw,actual.data(),actual.size(),0x7fffffff,1.0f);
        }
        std::printf("Shipment ordered groups: %llu of %llu individual bounds skipped across 2000 source trace queries\n",
            static_cast<unsigned long long>(groupSkipped-before),static_cast<unsigned long long>(worldCount)*2000);
        std::printf("Shipment baseline skips=%llu parent probes=%llu; refined skips=%llu parent/child probes=%llu\n",
            (unsigned long long)(baselineSkipped-oldBefore),(unsigned long long)(baselineProbes-oldProbesBefore),
            (unsigned long long)(groupSkipped-before),(unsigned long long)(groupProbes-probesBefore));
    }
    {
        // Both hierarchy levels, partial children, failed rebuild, and a leaf
        // too large for refinement but small enough for baseline groups.
        kisak::vita::OrderedBrushGroups<6> hierarchy;
        uint16_t members[17]{};
        cbrush_t b{};for(int a=0;a<3;++a){b.mins[a]=100;b.maxs[a]=110;}
        const float zero[3]{};kisak::vita::TraceBrushFilter filter(zero,zero);
        for(size_t n=1;n<=17;++n) {
            assert(hierarchy.Build(&b,members,n));
            assert(hierarchy.ProbeWidth()==(n<=16?4:16));
            assert(hierarchy.ProbeWidth(&b,members,n)==hierarchy.ProbeWidth());
            assert(hierarchy.ProbeWidth(&b,members,n+1)==16);
            assert(hierarchy.ProbeWidth(&b,members+1,n)==16);
            assert(hierarchy.Skip(&b,members,n,0,filter,zero)==std::min<size_t>(16,n));
            if(n>=5 && n<=16)assert(hierarchy.Skip(&b,members,n,4,filter,zero)==std::min<size_t>(4,n-4));
        }
        assert(!hierarchy.Build(static_cast<const cbrush_t*>(nullptr),members,17));
        assert(hierarchy.ProbeWidth()==16);
        assert(hierarchy.Skip(&b,members,17,0,filter,zero)==0);
        // Invalid parent cannot suppress a child containing the invalid brush.
        cbrush_t mixed[8]{};uint16_t ids[8]{};
        for(unsigned i=0;i<8;++i){mixed[i]=b;ids[i]=uint16_t(i);}
        mixed[0].mins[0]=std::numeric_limits<float>::quiet_NaN();
        assert(hierarchy.Build(mixed,ids,8));
        assert(hierarchy.Skip(mixed,ids,8,0,filter,zero)==0);
        assert(hierarchy.Skip(mixed,ids,8,4,filter,zero)==4);
    }
    {
        kisak::vita::OrderedBrushGroups<1> bounded;uint16_t members[17]{};
        assert(!bounded.Build(brushes.data(),members,17));
        cbrush_t distant{};for(int a=0;a<3;++a){distant.mins[a]=100;distant.maxs[a]=110;}
        assert(bounded.Build(&distant,members,1));
        const float zero[3]{};kisak::vita::TraceBrushFilter filter(zero,zero);
        assert(bounded.Skip(&distant,members,1,0,filter,zero)==1);
        assert(bounded.Skip(brushes.data(),members,1,0,filter,zero)==0);
        uint16_t otherMembers[1]{};
        assert(bounded.Skip(&distant,otherMembers,1,0,filter,zero)==0);
        assert(bounded.Skip(&distant,members,2,0,filter,zero)==0);
        bounded.Reset();assert(bounded.Skip(&distant,members,1,0,filter,zero)==0);
        distant.mins[0]=std::numeric_limits<float>::quiet_NaN();
        assert(bounded.Build(&distant,members,1));assert(bounded.Skip(&distant,members,1,0,filter,zero)==0);
    }
    assert(rejected>1000000);
    std::printf("%llu per-brush comparisons, %llu conservative rejections; all trace fields identical\n",
        static_cast<unsigned long long>(comparisons),static_cast<unsigned long long>(rejected));
}
