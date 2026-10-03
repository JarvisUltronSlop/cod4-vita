#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <fstream>
#include <iterator>
#include <gfx_vita/gfx_vita_world_lifetime.h>
#include <vita/vita_render_pass_probe.h>
using namespace kisak::vita;
int main(int argc,char** argv) {
 assert(argc==2);std::ifstream f(argv[1]);assert(f);std::string text((std::istreambuf_iterator<char>(f)),{});
 for(size_t p=0;(p=text.find('\r',p))!=std::string::npos;)text.erase(p,1);
 auto a=text.find("kNativeLitFragmentShader = R\"VITA_SHADER(");assert(a!=std::string::npos);a=text.find('(',a)+1;
 auto b=text.find(")VITA_SHADER\"",a);assert(b!=std::string::npos);const auto original=text.substr(a,b-a);
 std::string baseline,candidate;assert(GfxVitaBuildMaterialFamily(original.c_str(),1,baseline));assert(GfxVitaBuildWorldLifetime(original.c_str(),candidate));
 assert(candidate.find("vec3 sourceLightmapNormal =")==std::string::npos);
 assert(candidate.find("float sourceLightmapSun = 0.0;")!=std::string::npos);
 unsigned count;const auto* edits=GfxVitaWorldLifetimeEdits(count);assert(count==7);
 for(unsigned i=count;i-->0;) {
  const std::string after=edits[i].after;auto at=candidate.find(after);assert(at!=std::string::npos);candidate.replace(at,after.size(),edits[i].before);
 }
 assert(candidate==baseline); // no new texture reads, changed precision, lighting constants, alpha state or arithmetic operations
 for(unsigned i=0;i<count;++i) {
  auto broken=original;const std::string before=edits[i].before;auto at=broken.find(before);assert(at!=std::string::npos);broken.erase(at,before.size());
  std::string out="stale";assert(!GfxVitaBuildWorldLifetime(broken.c_str(),out) && out.empty());
  assert(!GfxVitaBuildWorldLifetime((original+"\n"+before).c_str(),out) && out.empty());
 }
 for(unsigned bits=0;bits<16;++bits)for(unsigned family=0;family<4;++family)
  assert(GfxVitaWorldLifetimeEligible(bits&1,bits&2,bits&4,bits&8,family)==(bits==9 && family==1));
 for(unsigned phase=0;phase<5;++phase) {
  const auto mode=kVitaWorldLifetimeComparison|phase;VitaRenderPassProbe p;p.Begin(mode,0);const bool active=phase>=1 && phase<=3;
  assert(p.worldLifetime==active && p.worldAlpha==active && p.weaponSpecular==active && p.weaponVertex==active && p.weaponPipeline==active);
  assert(VitaWorldLifetimeCandidate(mode)==(phase==2));assert(VitaWorldAlphaEnabled(mode)==active && VitaWeaponVertexEnabled(mode)==active && VitaWeaponPipelineEnabled(mode)==active);
  assert(!p.Fenced() && !p.drawProbe && !p.postFilm && !p.ClipProps() && !p.SuppressProps());
  for(auto other:{kVitaPostFilmComparison,kVitaWorldAlphaComparison,kVitaCurrentDrawProbe}){p.Begin(mode|other,0);assert(!p.phase);}
 }
}
