#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <fstream>
#include <iostream>
#include <iterator>
#include <random>
#include <limits>
#include <gfx_vita/gfx_vita_world_basis.h>
#include <gfx_vita/gfx_vita_world_basis_audit.h>
#include <vita/vita_render_pass_probe.h>
using namespace kisak::vita;
using V=std::array<float,3>;
struct Vertex {V normal,tangent;};
float Dot(V a,V b){return a[0]*b[0]+a[1]*b[1]+a[2]*b[2];}
V Scale(V a,float s){for(auto& x:a)x*=s;return a;}
V Add(V a,V b){for(unsigned i=0;i<3;++i)a[i]+=b[i];return a;}
V Norm(V a){return Scale(a,1/std::sqrt(Dot(a,a)));}
V Cross(V a,V b){return {a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]};}
std::array<V,3> Basis(V n,V t){n=Norm(n);t=Norm(Add(t,Scale(n,-Dot(n,t))));return {n,t,Norm(Cross(n,t))};}
V Interpolate(V a,V weights,V reciprocalW) {
 const float d=Dot(weights,reciprocalW);V out{};
 for(unsigned i=0;i<3;++i)out[i]=(weights[0]*(a[i]*reciprocalW[0])+weights[1]*(a[i]*reciprocalW[1])+weights[2]*(a[i]*reciprocalW[2]))/d;
 return out;
}
int main(int argc,char** argv) {
 assert(argc==2);std::ifstream file(argv[1]);assert(file);std::string text((std::istreambuf_iterator<char>(file)),{});
 for(size_t p=0;(p=text.find('\r',p))!=std::string::npos;)text.erase(p,1);
 auto shader=[&](const char* name){auto a=text.find(name);assert(a!=std::string::npos);a=text.find('(',a)+1;auto b=text.find(")VITA_SHADER\"",a);assert(b!=std::string::npos);return text.substr(a,b-a);};
 auto vs=shader("kNativeLitVertexShader = R\"VITA_SHADER("),fs=shader("kNativeLitFragmentShader = R\"VITA_SHADER(");
 for(bool alpha:{false,true}) {
  std::string v,f,base;assert(GfxVitaBuildWorldBasis(vs.c_str(),fs.c_str(),alpha,v,f));
  assert(alpha?GfxVitaBuildWorldAlpha(fs.c_str(),base):GfxVitaBuildMaterialFamily(fs.c_str(),1,base));
  assert(f.find("vec3 bitangent = v_bitangent;")!=std::string::npos);
  assert(f.find("vec3 tangentBasis =")==std::string::npos);
  // The fragment tail, beginning at the authored packed-normal decode,
  // is byte identical: every normal map, lightmap, specular, reflection,
  // alpha/discard and fog operation remains in its original order.
  const std::string tail="        // The retail lm_* shaders";
  assert(f.substr(f.find(tail))==base.substr(base.find(tail)));
  assert(v.find("v_bitangent = normalize(cross(normal, tangent));")!=std::string::npos);
  assert(v.substr(v.find("    v_worldposition ="))==vs.substr(vs.find("    v_worldposition =")));
  for(auto bad:{std::string(),vs+"\nvarying vec3 v_tangent;"})assert(!GfxVitaBuildWorldBasis(bad.c_str(),fs.c_str(),alpha,v,f) && v.empty() && f.empty());
  assert(!GfxVitaBuildWorldBasis(vs.c_str(),(fs+"\nvarying vec3 v_tangent;").c_str(),alpha,v,f) && v.empty() && f.empty());
 }
 Vertex a{{0,0,1},{1,0,0}},b=a,c=a;std::array<Vertex,6> vertices{a,b,c,Vertex{{0,1,0},{1,0,0}},Vertex{{0,1,0},{1,0,0}},Vertex{{0,1,0},{1,0,0}}};
 uint16_t indices[]={0,1,2,3,4,5};auto get=[&](uint32_t i)->const Vertex&{assert(i<6);return vertices[i];};
 assert(GfxVitaConstantWorldBasis(indices,6,6,get)); // different constant basis per triangle is valid
 vertices[1].tangent[1]=.1f;assert(!GfxVitaConstantWorldBasis(indices,6,6,get));vertices[1]=a;
 vertices[1].normal[0]=.001f;assert(GfxVitaConstantWorldBasis(indices,6,6,get)); // same actual packed input
 vertices[1].normal[0]=std::numeric_limits<float>::quiet_NaN();assert(!GfxVitaConstantWorldBasis(indices,6,6,get));vertices[1]=a;
 for(unsigned i=0;i<3;++i)vertices[i].tangent={0,0,1};assert(!GfxVitaConstantWorldBasis(indices,6,6,get));
 for(unsigned i=0;i<3;++i)vertices[i]=a;
 assert(!GfxVitaConstantWorldBasis(nullptr,6,6,get));assert(!GfxVitaConstantWorldBasis(indices,5,6,get));assert(!GfxVitaConstantWorldBasis(indices,6,5,get));
 // Audit labels are mutually exclusive and never change certification.
 auto audit=GfxVitaAuditWorldBasis(indices,6,6,get);
 assert(audit.inputValid && audit.triangles==2 && audit.reasons[0]==2 && audit.runs==1 && audit.constantRuns==1 && audit.longestConstantRun==2);
 vertices[1].normal[0]=.1f;assert(GfxVitaAuditBasisTriangle(indices,6,get)==GfxVitaBasisReason::NormalVaries);
 vertices[1]=a;vertices[1].tangent[1]=.1f;assert(GfxVitaAuditBasisTriangle(indices,6,get)==GfxVitaBasisReason::TangentVaries);
 vertices[1].normal[0]=.1f;assert(GfxVitaAuditBasisTriangle(indices,6,get)==GfxVitaBasisReason::BothVary);
 vertices[1]=a;vertices[1].normal[0]=std::numeric_limits<float>::infinity();assert(GfxVitaAuditBasisTriangle(indices,6,get)==GfxVitaBasisReason::Nonfinite);
 vertices[1]=a;vertices[1].tangent=vertices[1].normal;assert(GfxVitaAuditBasisTriangle(indices,6,get)==GfxVitaBasisReason::Degenerate);
 assert(GfxVitaAuditBasisTriangle(indices,2,get)==GfxVitaBasisReason::InvalidIndex);
 audit=GfxVitaAuditWorldBasis(indices,6,6,get);assert(audit.inputValid && audit.reasons[5]==1 && audit.reasons[0]==1 && audit.runs==2 && audit.constantRuns==1 && audit.longestConstantRun==1);
 vertices[1]=a;vertices[4].tangent[0]=.5f;
 uint16_t alternating[]={0,1,2,3,4,5,0,1,2,3,4,5,0,1,2};
 audit=GfxVitaAuditWorldBasis(alternating,15,6,get);assert(audit.inputValid && audit.runs==5 && audit.constantRuns==3 && audit.reasons[0]==3 && audit.reasons[2]==2);
 assert(!GfxVitaAuditWorldBasis(nullptr,6,6,get).inputValid);
 assert(!GfxVitaAuditWorldBasis(indices,5,6,get).inputValid);
 assert(!GfxVitaAuditWorldBasis(indices,6,5,get).inputValid);
 vertices[4]=vertices[3];
 GfxVitaWorldBasisState state;state.pending=true;
 {GfxVitaWorldBasisScope scope(state,false);assert(state.active && !state.pending);}assert(!state.active);
 {GfxVitaWorldBasisScope scope(state,false);assert(!state.active);}state.pending=true;
 {GfxVitaWorldBasisScope scope(state,true);assert(!state.active && !state.pending);}state.pending=true;state.Reset();assert(!state.pending);
 for(unsigned phase=0;phase<5;++phase) {
  const auto mode=kVitaWorldBasisComparison|phase;VitaRenderPassProbe p;p.Begin(mode,0);const bool active=phase>=1 && phase<=3;
  assert(p.worldBasis==active && p.worldAlpha==active && p.weaponVertex==active && p.weaponPipeline==active);
  assert(VitaWorldAlphaEnabled(mode)==active && VitaWeaponVertexEnabled(mode)==active);
  assert(!p.Fenced() && !p.drawProbe && !p.lightmapLayout && !p.worldMath && !p.postFilm && !p.ClipProps());
 }
 std::mt19937 rng(61);std::uniform_real_distribution<float> unit(-1,1),positive(.001f,1.f);
 float error=0;unsigned accepted=0;
 for(unsigned i=0;i<200000;++i) {
  Vertex v{{unit(rng),unit(rng),unit(rng)},{unit(rng),unit(rng),unit(rng)}};auto k=GfxVitaWorldBasis(v);if(!k.valid)continue;++accepted;
  V n{},t{};for(unsigned j=0;j<3;++j){n[j]=k.bytes[j]/127.f;t[j]=k.bytes[j+3]/127.f;}
  V weights{positive(rng),positive(rng),positive(rng)},rw{positive(rng),positive(rng),positive(rng)};weights=Scale(weights,1/(weights[0]+weights[1]+weights[2]));
  auto before=Basis(Interpolate(n,weights,rw),Interpolate(t,weights,rw)),vertex=Basis(n,t);
  std::array<V,3> after;for(unsigned j=0;j<3;++j)after[j]=Interpolate(vertex[j],weights,rw);
  V mapped{unit(rng),unit(rng),0};mapped[2]=std::sqrt(std::max(0.f,1-Dot(mapped,mapped)));
  auto shade=[&](std::array<V,3> b){return Norm(Add(Add(Scale(b[1],mapped[0]),Scale(b[2],mapped[1])),Scale(b[0],mapped[2])));};
  auto bn=shade(before),an=shade(after);
  for(unsigned j=0;j<3;++j){error=std::max(error,std::abs(bn[j]-an[j]));for(unsigned l=0;l<3;++l)error=std::max(error,std::abs(before[l][j]-after[l][j]));}
 }
 assert(accepted>180000 && error<0.00001f);
 for(unsigned i=0;i<5000;++i) {
  for(auto& v:vertices)v=Vertex{{unit(rng),unit(rng),unit(rng)},{unit(rng),unit(rng),unit(rng)}};
  if(i%2==0)vertices[1]=vertices[2]=vertices[0];
  if(i%3==0)vertices[4]=vertices[5]=vertices[3];
  const auto result=GfxVitaAuditWorldBasis(indices,6,6,get);
  unsigned sum=0;for(auto n:result.reasons)sum+=n;
  assert(result.inputValid && sum==2);
  assert(GfxVitaConstantWorldBasis(indices,6,6,get)==(result.reasons[0]==2));
 }
 std::cout<<accepted<<" certified perspective interpolation cases; max basis/mapped-normal difference="<<error<<"\n";
}
