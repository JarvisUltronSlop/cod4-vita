#include <vita/vita_benchmark_textures.h>
#include <vita/vita_benchmark_debris.h>
#include <cstdlib>
#include <cstdio>
using namespace kisak::vita;
void Require(bool ok) { if (!ok) { std::puts("mip selection failed"); std::exit(1); } }
int main() {
    for (auto name : {"com_trashbag1_black", "com_pallet_destroyed", "com_cardboardbox03", "com_junktire", "ch_rubble_chunk01"})
        Require(VitaBenchmarkDebrisModel(name));
    for (auto name : {"com_pallet_stack", "com_trashcan_metal", "me_dumpster", "foliage_litegrass_squareclump", "vehicle_80s_sedan1_red", "com_barrel_blue_rust", "viewmodel_mp5", "com_trashbag1_black_extra"})
        Require(!VitaBenchmarkDebrisModel(name));
    Require(VitaBenchmarkDebrisMaterial("wc/me_trash01"));
    for (auto name : {"wc/me_asphalt_01", "wc/ch_ground_gravel02_decal", "wc/me_ground_drygrass_dec", "wc/me_trash01_extra"})
        Require(!VitaBenchmarkDebrisMaterial(name));
    for (unsigned mode=0;mode<2;++mode) {
        VitaBenchmarkDebris d{}; d.mode=mode;
        for (unsigned c=0;c<4;++c) {
            const bool admitted=d.Admit(c,18,3);
            Require(admitted==!(mode && (c==0 || c==2)));
            if (admitted) d.Submitted(c,18,3,true);
            Require(d.candidates[c]==1 && d.candidateTriangles[c]==18 && d.candidateInstances[c]==3);
            Require(d.skipped[c]+d.submitted[c]==1 && d.skippedTriangles[c]+d.submittedTriangles[c]==18);
        }
        d.Submitted(1,18,3,false); Require(d.failures==1);
    }
    for (uint32_t format : {21u,50u,0x31545844u,0x33545844u,0x35545844u}) {
        size_t bytes=0;
        for (unsigned d=64;d;d/=2) bytes+=VitaTextureMipBytes(format,d,d);
        for (unsigned mode=0;mode<4;++mode) for (unsigned role=0;role<3;++role) {
            auto s=VitaSelectBenchmarkMip(mode,VitaTextureRole(role),64,64,7,format,0,bytes);
            bool reduced=(role==1 && (mode&1)) || (role==2 && (mode&2));
            Require(s.valid && s.reduced==reduced);
            Require(s.width==(reduced?32:64) && s.height==s.width && s.levels==(reduced?6:7));
            Require(s.offset==(reduced?VitaTextureMipBytes(format,64,64):0));
            Require(s.bytes+s.offset==bytes);
        }
        Require(!VitaSelectBenchmarkMip(1,VitaTextureRole::World,64,64,7,format,0,bytes-1).valid);
        auto cube=VitaSelectBenchmarkMip(3,VitaTextureRole::World,64,64,7,format,4,bytes*6);
        Require(cube.valid && !cube.reduced && cube.bytes==bytes*6);
        Require(!VitaSelectBenchmarkMip(3,VitaTextureRole::World,64,64,7,format,4,bytes*6-1).valid);
    }
    auto single=VitaSelectBenchmarkMip(3,VitaTextureRole::Viewmodel,64,64,1,21,0,64*64*4);
    Require(single.valid && !single.reduced);
    auto tiny=VitaSelectBenchmarkMip(1,VitaTextureRole::World,4,4,2,0x31545844u,0,16);
    Require(tiny.valid && !tiny.reduced); // no invalid VitaGL 2x2 base transfer
    auto rect=VitaSelectBenchmarkMip(2,VitaTextureRole::Viewmodel,64,32,2,21,0,(64*32+32*16)*4);
    Require(rect.valid && rect.width==32 && rect.height==16);
    Require(!VitaSelectBenchmarkMip(1,VitaTextureRole::World,0,64,1,21,0,256).valid);
    Require(!VitaSelectBenchmarkMip(4,VitaTextureRole::World,64,64,1,21,0,16384).valid);
    Require(!VitaSelectBenchmarkMip(1,VitaTextureRole::World,64,64,1,123,0,16384).valid);
    std::puts("authored mip selection tests passed");
}
