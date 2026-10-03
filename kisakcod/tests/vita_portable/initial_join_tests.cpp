#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <string>
#include <vector>
constexpr int PM_NORMAL=0, CS_MODELS=1024;
struct playerState_s { int pm_type=1, stats[1]{}, weapon=0, viewmodelIndex=0, commandTime=0; };
struct cg_s { char scriptMainMenu[64]{}; playerState_s predictedPlayerState; } cg;
struct { const char* szInternalName="mp5_mp"; } weapon;
std::vector<std::string> commands;
unsigned popupCloses=0, allCloses=0;
int keyCatchers=16|8;
bool configReady=true;
cg_s* CG_GetLocalClientGlobals(int n){assert(n==0);return &cg;}
const char* CL_GetConfigString(int n,int index) {
    assert(n==0);
    if(index==CS_MODELS+2)return "viewhands_marine_sleeves";
    if(!configReady)return "";
    return index==1970 ? "team_marinesopfor" : index==1971 ? "changeclass_offline" : "";
}
int Dvar_GetInt(const char* name){assert(!std::strcmp(name,"sv_serverId"));return 123;}
void CL_AddReliableCommand(int n,const char* command){assert(n==0);commands.emplace_back(command);}
void CG_CloseScriptMenu(int n,bool response){assert(n==0 && !response);++popupCloses;}
void UI_CloseAllMenus(int n){assert(n==0);++allCloses;}
void Key_RemoveCatcher(int n,int mask){assert(n==0 && mask==~16);keyCatchers&=mask;}
auto BG_GetWeaponDef(int id){assert(id==1);return &weapon;}
void Com_Printf(int,const char*,...){}
class VitaSourceSession {
public:
    struct { bool initialPlayerSpawned=false;char handModelName[64]{}; } status_;
    uint8_t initialJoinStage_=0;
    void AdvanceInitialJoin();
};
#include "source_initial_join.inc"
int main() {
    // Compile the actual join function; mock unrelated source/OS boundaries.
    // Missing configstrings wait without accepting a hidden menu or skipping
    // the reliable GSC protocol. No user click is needed to complete joining.
    VitaSourceSession s;
    std::strcpy(cg.scriptMainMenu,"team_marinesopfor");configReady=false;
    s.AdvanceInitialJoin();assert(commands.empty() && popupCloses==0);
    configReady=true;s.AdvanceInitialJoin();
    assert(commands.size()==1 && commands[0]=="mr 123 0 autoassign" && popupCloses==1);
    s.AdvanceInitialJoin();assert(commands.size()==1);
    std::strcpy(cg.scriptMainMenu,"class_marines");s.AdvanceInitialJoin();
    assert(commands.size()==2 && commands[1]=="mr 123 1 offline_class1_mp,0" && popupCloses==2);
    assert(!s.status_.initialPlayerSpawned && allCloses==0);
    cg.predictedPlayerState={PM_NORMAL,{100},1,2,500};s.AdvanceInitialJoin();
    assert(!s.status_.initialPlayerSpawned && allCloses==1 && popupCloses==3 && keyCatchers==8);
    // Only a subsequent source client update may expose the clean UI packet.
    s.AdvanceInitialJoin();assert(s.status_.initialPlayerSpawned && allCloses==1);
    assert(!std::strcmp(s.status_.handModelName,"viewhands_marine_sleeves"));
    for(int i=0;i<100;++i)s.AdvanceInitialJoin();
    assert(commands.size()==2 && allCloses==1 && popupCloses==3);
}
