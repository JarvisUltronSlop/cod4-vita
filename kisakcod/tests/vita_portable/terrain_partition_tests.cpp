#ifdef NDEBUG
#undef NDEBUG
#endif
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <random>
#include <vector>
#include <vita/vita_terrain_partition.h>

#ifndef _MSC_VER
#define __cdecl
#endif
#define iassert(x) do { if(!(x)){std::fprintf(stderr,"assert line %d: %s\n",__LINE__,#x);std::abort();} } while(0)
#define EQUAL_EPSILON 0.001f
#define SLOWORD(x) int16_t(uint16_t(x))
constexpr bool alwaysfails=false;
static void MyAssertHandler(const char* file,int line,int,const char*,...) {std::fprintf(stderr,"source assert %s:%d\n",file,line);std::abort();}
static const char* va(const char*,...) {return "source vector assertion";}
static float I_fabs(float v){return std::fabs(v);}
static float Vec3Dot(const float* a,const float* b){return a[0]*b[0]+a[1]*b[1]+a[2]*b[2];}
static float Vec3LengthSq(const float* a){return Vec3Dot(a,a);}
static float Vec3Length(const float* a){return std::sqrt(Vec3LengthSq(a));}
static bool Vec3IsNormalized(const float* a){return std::fabs(Vec3LengthSq(a)-1)<0.002f;}
static void Vec3Sub(const float* a,const float* b,float* c){for(int i=0;i<3;++i)c[i]=a[i]-b[i];}
static void Vec3Add(const float* a,const float* b,float* c){for(int i=0;i<3;++i)c[i]=a[i]+b[i];}
static void Vec3Scale(const float* a,float b,float* c){for(int i=0;i<3;++i)c[i]=a[i]*b;}
static void Vec3Mad(const float* a,float b,const float* c,float* d){for(int i=0;i<3;++i)d[i]=a[i]+b*c[i];}
static void Vec3MadMad(const float* a,float b,const float* c,float d,const float* e,float* f){for(int i=0;i<3;++i)f[i]=a[i]+b*c[i]+d*e[i];}
static void Vec3Lerp(const float* a,const float* b,float t,float* c){for(int i=0;i<3;++i)c[i]=a[i]+t*(b[i]-a[i]);}
static void Vec3Cross(const float* a,const float* b,float* c){c[0]=a[1]*b[2]-a[2]*b[1];c[1]=a[2]*b[0]-a[0]*b[2];c[2]=a[0]*b[1]-a[1]*b[0];}
static float Vec3NormalizeTo(const float* v,float* out){float length=v[0]*v[0]+v[1]*v[1]+v[2]*v[2];float root=std::sqrt(length);float denominator=(-root<0)?root:1.0f;float inverse=1.0/denominator;for(int i=0;i<3;++i)out[i]=v[i]*inverse;return root;}
enum SphereEdgeTraceResult {SPHERE_HITS_EDGE,SPHERE_MISSES_EDGE,SPHERE_MAY_HIT_V0,SPHERE_MAY_HIT_V1};
struct trace_t {float fraction=1,normal[3]{};int surfaceFlags=0,contents=0;const char* material=nullptr;bool walkable=false,startsolid=false,allsolid=false;};
struct traceWork_t {
    struct {float start[3]{},end[3]{};} extents;
    float delta[3]{},deltaLen=0,deltaLenSq=0,midpoint[3]{},halfDelta[3]{},halfDeltaAbs[3]{},size[3]{};
    int contents=1;bool isPoint=false,axialCullOnly=false;float radius=0,offsetZ=0;
    struct {struct {int global=1;int* partitions=nullptr;} checkcount;} threadInfo;
};
struct CollisionBorder {float distEq[3],zBase,zSlope,start,length;};
struct CollisionPartition {uint8_t triCount=0,borderCount=0;int firstTri=0;CollisionBorder* borders=nullptr;};
struct CollisionAabbTree {float origin[3]{},halfSize[3]{};uint16_t materialIndex=0,childCount=0;union {int firstChildIndex;int partitionIndex;} u{};};
struct cLeaf_t {uint16_t firstCollAabbIndex=0,collAabbCount=0;};
struct dmaterial_t {char material[64]{};int surfaceFlags=13,contentFlags=1;};
struct {float (*verts)[3];uint16_t* triIndices;uint8_t* triEdgeIsWalkable;CollisionAabbTree* aabbTrees;CollisionPartition* partitions;dmaterial_t* materials;} cm;
static uint64_t terrainTriangleCalls=0;
#include "source_terrain.inc"

static void Equal(const trace_t& a,const trace_t& b) {
    iassert(std::memcmp(&a.fraction,&b.fraction,sizeof(float))==0);
    iassert(std::memcmp(a.normal,b.normal,sizeof(a.normal))==0);
    iassert(a.surfaceFlags==b.surfaceFlags && a.contents==b.contents && a.material==b.material);
    iassert(a.walkable==b.walkable && a.startsolid==b.startsolid && a.allsolid==b.allsolid);
}
int main(int argc, char** argv) {
    using Layout=kisak::vita::TerrainPartitionLayout;
    std::mt19937 rng(0xc0d413);
    auto random=[&](float a,float b){return a+(b-a)*(rng()/4294967296.0f);};
    uint64_t comparisons=0;
    std::vector<size_t> counts={0u,1u,15u,16u,17u,254u,255u,256u,510u,511u};
    if(argc>1) counts.push_back(SIZE_MAX);
    for(size_t count : counts) {
        const bool physical=count==SIZE_MAX;
        std::vector<std::array<float,3>> vertices;
        std::vector<std::array<uint16_t,3>> triangles;
        if(physical) {
            std::ifstream input(argv[1],std::ios::binary);uint32_t sizes[2]{};
            input.read(reinterpret_cast<char*>(sizes),sizeof(sizes));iassert(input && sizes[0]<=65535 && sizes[1]<1000000);
            vertices.resize(sizes[0]);triangles.resize(sizes[1]);count=sizes[1];
            input.read(reinterpret_cast<char*>(vertices.data()),vertices.size()*sizeof(vertices[0]));
            input.read(reinterpret_cast<char*>(triangles.data()),triangles.size()*sizeof(triangles[0]));iassert(input);
        }
        for(size_t i=0;!physical && i<count;++i) {
            const float x=float(i%32)*64-1024,y=float(i/32)*64-512,z=float(i%5)*8;
            const uint16_t first=uint16_t(vertices.size());
            vertices.push_back({x,y,z});vertices.push_back({x+48,y,z+float(i%3)*12});vertices.push_back({x,y+48,z});
            triangles.push_back({first,uint16_t(first+1),uint16_t(first+2)});
        }
        std::vector<CollisionPartition> parts(Layout::Leaves(count)),oldParts(Layout::Roots(count));
        std::vector<CollisionAabbTree> trees(Layout::Trees(count)),oldTrees(Layout::Roots(count));
        kisak::vita::BuildTerrainPartitions(vertices,triangles,parts.data(),trees.data());
        struct {const decltype(vertices)& vertices_;const decltype(triangles)& triangles_;} collision{vertices,triangles};
        BuildOriginalTerrain(collision,oldParts.data(),oldTrees.data());
        size_t nextTriangle=0;
        for(size_t i=0;i<oldTrees.size();++i) {
            iassert(std::memcmp(oldTrees[i].origin,trees[i].origin,sizeof(trees[i].origin))==0);
            iassert(std::memcmp(oldTrees[i].halfSize,trees[i].halfSize,sizeof(trees[i].halfSize))==0);
            for(unsigned c=0;c<trees[i].childCount;++c) {
                const auto& child=trees[trees[i].u.firstChildIndex+c];const auto& part=parts[child.u.partitionIndex];
                iassert(part.firstTri==nextTriangle && part.triCount>0 && part.triCount<=16);
                nextTriangle+=part.triCount;
            }
        }
        iassert(nextTriangle==count);
        std::vector<uint8_t> walkable((count*3+7)/8,0xff);
        std::vector<int> stamps(std::max(parts.size(),oldParts.size()),0);
        dmaterial_t material{};cm.materials=&material;cm.verts=reinterpret_cast<float(*)[3]>(vertices.data());
        cm.triIndices=reinterpret_cast<uint16_t*>(triangles.data());cm.triEdgeIsWalkable=walkable.data();
        uint64_t calls[2]{};
        for(int trial=0;trial<(physical?20000:5000);++trial) {
            traceWork_t tw{};tw.radius=trial%3==0?0:15;tw.offsetZ=trial%4==0?0:20;
            for(int axis=0;axis<3;++axis) {
                tw.extents.start[axis]=random(-1100,1100);tw.extents.end[axis]=random(-1100,1100);
            }
            if(count && trial%2==0) { // Hit and boundary cases near actual vertices.
                const auto& vertex=vertices[(trial*3)%vertices.size()];
                for(int axis=0;axis<3;++axis)tw.extents.start[axis]=vertex[axis];
                tw.extents.start[2]+=tw.radius+tw.offsetZ+(trial%8)*0.0625f;
                for(int axis=0;axis<3;++axis)tw.extents.end[axis]=tw.extents.start[axis];
                tw.extents.end[2]-=64;
            }
            const bool position=trial%5==0;
            if(position)for(int axis=0;axis<3;++axis)tw.extents.end[axis]=tw.extents.start[axis];
            tw.isPoint=!position && tw.radius==0 && tw.offsetZ==0;
            for(int axis=0;axis<3;++axis) {
                tw.delta[axis]=tw.extents.end[axis]-tw.extents.start[axis];
                tw.midpoint[axis]=(tw.extents.start[axis]+tw.extents.end[axis])*0.5f;
                tw.halfDelta[axis]=tw.delta[axis]*0.5f;tw.halfDeltaAbs[axis]=std::fabs(tw.halfDelta[axis]);
                tw.size[axis]=tw.radius+(axis==2?tw.offsetZ:0);
            }
            tw.deltaLenSq=Vec3LengthSq(tw.delta);tw.deltaLen=std::sqrt(tw.deltaLenSq);
            trace_t results[2];results[0].fraction=results[1].fraction=trial%7==0?0.25f:1.0f;
            for(int version=0;version<2;++version) {
                terrainTriangleCalls=0;
                std::fill(stamps.begin(),stamps.end(),0);tw.threadInfo.checkcount.partitions=stamps.data();
                cm.aabbTrees=version?trees.data():oldTrees.data();cm.partitions=version?parts.data():oldParts.data();
                cLeaf_t leaf{};leaf.collAabbCount=uint16_t(oldTrees.size());
                if(position)CM_MeshTestInLeaf(&tw,&leaf,&results[version]);
                else for(size_t root=0;root<oldTrees.size() && results[version].fraction!=0;++root)
                    CM_TraceThroughAabbTree(&tw,&cm.aabbTrees[root],&results[version]);
                calls[version]+=terrainTriangleCalls;
            }
            Equal(results[0],results[1]);++comparisons;
        }
        if(physical) {
            iassert(calls[1]<calls[0]);
            std::printf("Shipment fixture triangle calls: original %llu, child groups %llu\n",
                static_cast<unsigned long long>(calls[0]),static_cast<unsigned long long>(calls[1]));
        }
    }
    std::printf("%llu original-source point/capsule/position trace comparisons passed; original triangle order retained\n",static_cast<unsigned long long>(comparisons));
}
