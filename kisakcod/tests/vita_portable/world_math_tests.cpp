#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstdlib>
#include <cmath>
#include <array>
#include <algorithm>
#include <fstream>
#include <iostream>
#include <iterator>
#include <random>
#include <gfx_vita/gfx_vita_world_math.h>
#include <vita/vita_render_pass_probe.h>
using namespace kisak::vita;
using V=std::array<float,3>;
float dot(V a,V b){return a[0]*b[0]+a[1]*b[1]+a[2]*b[2];}
V normalized(V a){float inv=1/std::sqrt(dot(a,a));for(auto& x:a)x*=inv;return a;}
std::array<float,2> projection(V a,unsigned axis){return {a[(axis+1)%3]/std::abs(a[axis]),a[(axis+2)%3]/std::abs(a[axis])};}
int main(int argc,char** argv) {
#ifdef _WIN32
    _set_error_mode(_OUT_TO_STDERR);
    _set_abort_behavior(0,_WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
 assert(argc==2);std::ifstream f(argv[1]);assert(f);std::string text((std::istreambuf_iterator<char>(f)),{});
 for(size_t p=0;(p=text.find('\r',p))!=std::string::npos;)text.erase(p,1);
 auto a=text.find("kNativeLitFragmentShader = R\"VITA_SHADER(");assert(a!=std::string::npos);a=text.find('(',a)+1;
 auto b=text.find(")VITA_SHADER\"",a);assert(b!=std::string::npos);const auto original=text.substr(a,b-a);
 unsigned count=0;const auto* edits=GfxVitaWorldMathEdits(count);assert(count==9);
 for(bool alpha:{false,true}) {
  std::string base,candidate;assert(alpha?GfxVitaBuildWorldAlpha(original.c_str(),base):GfxVitaBuildMaterialFamily(original.c_str(),1,base));
  assert(GfxVitaBuildWorldMath(original.c_str(),alpha,candidate));
  // Reversing the complete edit set must reproduce the established family
  // byte for byte, including all texture calls, fog, alpha and fallback code.
  for(unsigned i=count;i-->0;) {
   std::string after=edits[i].after;auto at=candidate.find(after);assert(at!=std::string::npos);candidate.replace(at,after.size(),edits[i].before);
  }
  assert(candidate==base);
  for(unsigned i=0;i<count;++i) {
   auto broken=original;std::string before=edits[i].before;auto at=broken.find(before);assert(at!=std::string::npos);broken.erase(at,before.size());
   std::string out="stale";assert(!GfxVitaBuildWorldMath(broken.c_str(),alpha,out) && out.empty());
   assert(!GfxVitaBuildWorldMath((original+"\n"+before).c_str(),alpha,out) && out.empty());
  }
 }
 std::string out="stale";assert(!GfxVitaBuildWorldMath(nullptr,false,out) && out.empty());
 for(unsigned bits=0;bits<32;++bits)assert(GfxVitaWorldMathLane(bits&1,bits&2,bits&4,bits&8,bits&16)==(bits==9?1u:bits==17?2u:0u));
 for(unsigned phase=0;phase<5;++phase) {
  const auto mode=kVitaWorldMathComparison|phase;VitaRenderPassProbe p;p.Begin(mode,0);const bool active=phase>=1 && phase<=3;
  assert(p.worldMath==active && p.worldAlpha==active && p.weaponSpecular==active && p.weaponVertex==active && p.weaponPipeline==active);
  assert(VitaWorldAlphaEnabled(mode)==active && VitaWeaponVertexEnabled(mode)==active && VitaWeaponPipelineEnabled(mode)==active);
  assert(!p.lightmapLayout && !p.worldLifetime && !p.postFilm && !p.drawProbe && !p.Fenced() && !p.ClipProps() && !p.SuppressProps());
  // Exact tags are not composable flags: math | film is now the gloss tag.
  p.Begin(mode|kVitaPostFilmComparison,0);assert(p.worldGloss==active && !p.worldMath);
  for(auto other:{kVitaWorldAlphaComparison,kVitaWorldLifetimeComparison,kVitaPropComparison}){p.Begin(mode|other,0);assert(bool(p.phase)==VitaGraphicsProfileMode(mode|other)); /* d0000000 is now the exact Graphics Profile tag. */}
 }
 // Compare independent double equations to float reassociation, with no
 // orthogonal-basis assumption. Lightmap vectors include short/oblique inputs.
 std::mt19937 rng(0x60c0d4);std::uniform_real_distribution<float> unit(-1,1);
 double maxSun=0,maxFresnel=0,maxCube=0,maxDerivative=0;
 for(unsigned i=0;i<200000;++i) {
  V vector{unit(rng),unit(rng),unit(rng)},sun=normalized({unit(rng),unit(rng),unit(rng)});
  float scale=std::pow(10.f,float(int(i%9)-4));for(auto& x:vector)x*=scale;
  double d=0,length=0;for(unsigned j=0;j<3;++j){d+=double(vector[j])*sun[j];length+=double(vector[j])*vector[j];}
  const double oracle=std::max(0.,d/std::sqrt(length));
  const float base=std::max(0.f,dot(normalized(vector),sun));
  const float candidate=std::max(0.f,dot(vector,sun)*(1/std::sqrt(dot(vector,vector))));
  maxSun=std::max({maxSun,std::abs(candidate-oracle),double(std::abs(candidate-base))});
  float x=float(i%65537)/65536.f,x2=x*x,poly=x2*x2*x;
  maxFresnel=std::max(maxFresnel,std::abs(double(poly)-std::pow(double(x),5)));
  V reflection{unit(rng),unit(rng),unit(rng)};
  unsigned axis=0;for(unsigned j=1;j<3;++j)if(std::abs(reflection[j])>std::abs(reflection[axis]))axis=j;
  auto projected=projection(reflection,axis),normProjected=projection(normalized(reflection),axis);
  auto adjacent=reflection;adjacent[(axis+1)%3]+=0.001f;
  auto projectedAdjacent=projection(adjacent,axis),normAdjacent=projection(normalized(adjacent),axis);
  for(unsigned j=0;j<2;++j) {
   maxCube=std::max(maxCube,double(std::abs(projected[j]-normProjected[j])));
   maxDerivative=std::max(maxDerivative,double(std::abs((projectedAdjacent[j]-projected[j])-(normAdjacent[j]-normProjected[j]))));
  }
 }
 assert(maxSun<0.000001 && maxFresnel<0.000001 && maxCube<0.000001 && maxDerivative<0.000002);
 std::cout<<"200000 numerical cases; max sun="<<maxSun<<" fresnel="<<maxFresnel<<" cube projection="<<maxCube<<" projected derivative="<<maxDerivative<<"\n";
 // Host arithmetic checks cannot qualify Vita compiler precision, cube LOD
 // implementation, appearance or frame time. The hardware A/B/A remains required.
}
