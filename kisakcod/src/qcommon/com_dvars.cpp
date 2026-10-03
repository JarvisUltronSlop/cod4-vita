#include <qcommon/qcommon.h>
#include <qcommon/threads.h>
#ifdef KISAK_MP
#include <game_mp/g_public_mp.h>
#endif

// Original shared initialization, separated from the desktop application root.
const dvar_t *cl_paused;
const dvar_t *cl_paused_simple;
const dvar_t *com_animCheck;
const dvar_t *com_authPort;
const dvar_t *com_authServerName;
const dvar_t *com_dedicated;
const dvar_t *com_developer;
const dvar_t *com_developer_script;
const dvar_t *com_developer_script_abort_on_error;
const dvar_t *com_filter_output;
const dvar_t *com_hiDef;
const dvar_t *com_introPlayed;
const dvar_t *com_logfile;
const dvar_t *com_masterPort;
const dvar_t *com_masterServerName;
const dvar_t *com_maxFrameTime;
const dvar_t *com_maxfps;
const dvar_t *com_statmon;
const dvar_t *com_sv_running;
const dvar_t *com_wideScreen;
const dvar_t *sv_paused;
const dvar_t *sys_lockThreads;
const dvar_t *sys_smp_allowed;
const dvar_t *useFastFile;

const char *s_lockThreadNames[4] = { "none", "minimal", "all" };

void Com_InitDvars()
{
#ifdef KISAK_MP
    #if !defined(DEDICATED) && !defined(KISAK_DEDICATED)
        com_dedicated = Dvar_RegisterEnum("dedicated", g_dedicatedEnumNames, 0, DVAR_LATCH, "True if this is a dedicated server");
    #else
        com_dedicated = Dvar_RegisterEnum("dedicated", g_dedicatedEnumNames, 2, DVAR_LATCH, "True if this is a dedicated server");
    #endif

    #if !defined(DEDICATED) && !defined(KISAK_DEDICATED)
        if (com_dedicated->current.integer)
            Dvar_RegisterEnum("dedicated", g_dedicatedEnumNames, 0, DVAR_ROM, "True if this is a dedicated server");
    #endif
#endif

#ifdef KISAK_MP
    com_maxfps = Dvar_RegisterInt("com_maxfps", 85, 0, 1000, DVAR_ARCHIVE, "Cap frames per second");
#elif KISAK_SP
    com_maxfps = Dvar_RegisterInt("com_maxfps", 0, 0, 1000, DVAR_ARCHIVE, "Cap frames per second");
#endif

    useFastFile = Dvar_RegisterBool(
        "useFastFile",
        1,
        DVAR_INIT,
        "Enables loading data from fast files. Only tools can run without fast files.");
    sys_lockThreads = Dvar_RegisterEnum(
        "sys_lockThreads",
        s_lockThreadNames,
        0,
        DVAR_NOFLAG,
        "Prevents specified threads from changing CPUs; improves profiling and may fix some bugs, but can hurt performance");

    sys_smp_allowed = Dvar_RegisterBool("sys_smp_allowed", Sys_GetCpuCount() > 1u, DVAR_INIT, "Allow multi-threading");
#ifdef KISAK_MP
    com_masterServerName = Dvar_RegisterString(
        "masterServerName",
        "cod4master.activision.com",
        DVAR_CHEAT,
        "Master server name for listing public inet games");
    com_authServerName = Dvar_RegisterString(
        "authServerName",
        "cod4master.activision.com",
        DVAR_CHEAT,
        "Authentication server name for listing public inet games");
    com_masterPort = Dvar_RegisterInt("masterPort", 20810, 0, 0xFFFF, DVAR_CHEAT, "Master server port");
    com_authPort = Dvar_RegisterInt("authPort", 20800, 0, 0xFFFF, DVAR_CHEAT, "Auth server port");
#endif
    com_developer = Dvar_RegisterInt("developer", 0, 0, 2, DVAR_NOFLAG, "Enable development options");
    com_developer_script = Dvar_RegisterBool("developer_script", 0, DVAR_NOFLAG, "Enable developer script comments");
    com_developer_script_abort_on_error = Dvar_RegisterBool("developer_script_abort_on_error", 0, DVAR_NOFLAG, "Halt Execution when an error is found in the scripts (Retail does not do this)"); // LWSS ADD
    com_logfile = Dvar_RegisterInt(
        "logfile",
        1,
        0,
        2,
        DVAR_NOFLAG,
        "Write to log file - 0 = disabled, 1 = async file write, 2 = Sync every write");
    com_statmon = Dvar_RegisterBool("com_statmon", 0, 0, "Draw stats monitor");
#ifdef KISAK_MP
    com_timescale = Dvar_RegisterFloat("com_timescale", 1.0, 0.001f, 1000.0f, DVAR_SYSTEMINFO | DVAR_ROM | DVAR_CHEAT | DVAR_TEMP | DVAR_SAVED, "Scale time of each frame");
#elif KISAK_SP
    com_timescale = Dvar_RegisterFloat("com_timescale", 1.0, 0.001f, 1000.0f, DVAR_SAVED | DVAR_CHEAT | DVAR_ROM | DVAR_SYSTEMINFO, "Scale time of each frame");
#endif
    dev_timescale = Dvar_RegisterFloat("timescale", 1.0, 0.001f, 1000.0f, DVAR_CHEAT | DVAR_SYSTEMINFO, "Scale time of each frame");
    com_fixedtime = Dvar_RegisterInt("fixedtime", 0, 0, 1000, 0x80u, "Use a fixed time rate for each frame");
    com_maxFrameTime = Dvar_RegisterInt(
        "com_maxFrameTime",
        100,
        50,
        5000,
        DVAR_NOFLAG,
        "Time slows down if a frame takes longer than this many milliseconds");
    sv_paused = Dvar_RegisterInt("sv_paused", 0, 0, 2, DVAR_ROM, "Pause the server");
    cl_paused = Dvar_RegisterInt("cl_paused", 0, 0, 2, DVAR_ROM, "Pause the client");
    cl_paused_simple = Dvar_RegisterBool(
        "cl_paused_simple",
        0,
        DVAR_NOFLAG,
        "Toggling pause won't do any additional special processing if true.");
    com_sv_running = Dvar_RegisterBool("sv_running", 0, DVAR_ROM, "Server is running");
    com_filter_output = Dvar_RegisterBool("com_filter_output", 0, DVAR_NOFLAG, "Use console filters for filtering output.");
    com_introPlayed = Dvar_RegisterBool("com_introPlayed", 0, DVAR_ARCHIVE, "Intro movie has been played");
    com_animCheck = Dvar_RegisterBool("com_animCheck", 0, DVAR_NOFLAG, "Check anim tree");
    com_hiDef = Dvar_RegisterBool("hiDef", 1, DVAR_ROM, "True if the game video is running in high-def.");
    com_wideScreen = Dvar_RegisterBool(
        "wideScreen",
        1,
        DVAR_ROM,
        "True if the game video is running in 16x9 aspect, false if 4x3.");
}

