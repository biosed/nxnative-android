// libc passthrough table: C library functions consistent with AAPCS64 and with no struct layout dependency, connected directly to Apple libc. Goes into the swhle section.
#include "hle.h"
#include <math.h>
#include <ctype.h>
#include <time.h>
// With hibernation (DM_HIBERNATE, hle_hib.c) the guest gets a thunk in the host executable instead of the library's address: the executable sits at a fixed address, a library
// moves when the device restarts, and a snapshot of guest memory keeps whatever function pointers the game copied out of its GOT. Each thunk is one branch to the function
#ifdef __APPLE__
#define THK_US "_"
#else
#define THK_US ""
#endif
typedef struct { void *fn, *thunk; } thk_ent;
#define L(n) __attribute__((used, DM_SECTION(swhle))) static const hle_ent hle_lib_##n = { #n, (void *)n }; \
    __asm__(".text\n.p2align 2\n" THK_US "dm_thk_" #n ":\n\tb " THK_US #n "\n"); extern const char dm_thk_##n[] __asm__(THK_US "dm_thk_" #n); \
    __attribute__((used, DM_SECTION(swthk))) static const thk_ent thk_##n = { (void *)n, (void *)dm_thk_##n };
L(memcpy) L(memset) L(memmove) L(memcmp) L(memchr) L(strlen) L(strnlen) L(strcmp) L(strncmp)
L(strcpy) L(strncpy) L(strcat) L(strncat) L(strchr) L(strrchr) L(strstr) L(strspn) L(strcspn)
L(strpbrk) L(strcasecmp) L(strncasecmp) L(atoi) L(atof) L(strtol) L(strtoul) L(strtoll) L(strtoull)
L(strtod) L(strtof) L(sqrtf) L(cbrtf) L(expf) L(acosf) L(cosf) L(sinf) L(atan2f) L(fmodf) L(cos)
L(sin) L(powf) L(atanf) L(tanf) L(asinf) L(log10) L(pow) L(logf) L(ldexpf) L(log10f) L(log)
L(ldexp) L(frexp) L(atan2) L(modff) L(exp2f) L(exp2) L(log2f) L(log2) L(lroundf)
L(mktime) L(timegm) L(gmtime_r) L(localtime_r) L(strftime)

// The thunk of a libc passthrough function (resolve_import with hibernation on); NULL for any other address
void *dm_libc_thunk(void *fn) {
    unsigned long sz = 0; const thk_ent *t = (const thk_ent *)DM_SECTION_DATA(swthk, &sz);
    for (unsigned long i = 0; t && i < sz / sizeof *t; i++) if (t[i].fn == fn) return t[i].thunk;
    return NULL;
}
