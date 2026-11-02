// The cheat engine (dm/host/hle_cheats.c) is not built for Android: it writes its state on GCD queues with blocks and maps main writable with mach_vm_remap.
// The host's calls into it find no cheats here
#include "dm.h"
void dm_cheats_init(void) { if (getenv("DM_CHEATS")) LOG("[RED] cheats: the Android host has no cheat engine yet (DM_CHEATS is ignored)"); }
void dm_cheats_present(void) {}
void dm_cheats_exit(void) {}
