#include <psp2/types.h>

// Consumed by vita-elf-create when it emits the kernel process parameters.
// Hardware crash 1 exhausted the default 256 KiB main stack: the bootstrap,
// package probe and typed weapon loader were simultaneously active. Reserve
// a bounded 1 MiB for this source-runtime thread, including nested loaders.
// Keep this symbol through section GC; CMake explicitly roots it at link time.
extern "C" {
extern const SceSize sceUserMainThreadStackSize = 1024u * 1024u;
}
