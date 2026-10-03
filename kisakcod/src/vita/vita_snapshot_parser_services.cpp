#include <client_mp/client_mp.h>

// The full desktop client owns this 2.8 MiB state object from cl_main_mp.cpp.
// The Vita parser gate deliberately supplies only the same zero-initialized
// storage while the rest of the desktop startup graph remains excluded. The
// live bootstrap will eventually move the fields it actually needs into its
// owned client lifecycle instead of invoking CL_Init wholesale.
clientStatic_t cls = {};
clientUIActive_t clientUIActives[STATIC_MAX_LOCAL_CLIENTS] = {};
