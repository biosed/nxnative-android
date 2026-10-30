// Command-line front end on Android (front.h; Mac: front_mac.m, iOS: dm/ios): no window, the guest runs on the calling thread. For bring-up over adb: the host runs as an
// ordinary program in /data/local/tmp, with its arguments and environment as on the Mac (dm/android/Makefile target dmhost)
#include "dm.h"
#include "front.h"
int dm_main(int argc, char **argv);   // dm/host/main.c, compiled with -Dmain=dm_main for the app as well
int main(int argc, char **argv) { return dm_main(argc, argv); }
void front_run(void (*guest)(void)) { guest(); }
void *front_metal_layer(void) { return NULL; }
int front_screen_max_fps(void) { return 0; }
int front_text_input(const char *title, const char *sub, const char *guide, const char *initial, int max_chars, char *out, int out_cap) {
    (void)title; (void)sub; (void)guide; (void)initial; (void)max_chars; (void)out; (void)out_cap; return -1;
}
