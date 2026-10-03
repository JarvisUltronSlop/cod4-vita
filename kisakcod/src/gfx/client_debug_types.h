#pragma once

// Backend-neutral client debug state shared by the network/client headers.
// Renderer-specific debug drawing remains in gfx_d3d/r_debug.h.

struct trDebugString_t {
    float xyz[3];
    float color[4];
    float scale;
    char text[96];
};

struct clientDebugStringInfo_t {
    int max;
    int num;
    trDebugString_t* strings;
    int* durations;
};

struct trDebugLine_t {
    float start[3];
    float end[3];
    float color[4];
    int depthTest;
};

struct clientDebugLineInfo_t {
    int max;
    int num;
    trDebugLine_t* lines;
    int* durations;
};

struct clientDebug_t {
    int prevFromServer;
    int fromServer;
    clientDebugStringInfo_t clStrings;
    clientDebugStringInfo_t svStringsBuffer;
    clientDebugStringInfo_t svStrings;
    clientDebugLineInfo_t clLines;
    clientDebugLineInfo_t svLinesBuffer;
    clientDebugLineInfo_t svLines;
};
