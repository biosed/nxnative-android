// libdmboot.so, the app's NativeActivity library (make_apk.py android.app.lib_name): loads libdmhost.so at a fixed address, then hands the activity to it.
// Hibernation (dm/host/hle_hib.c) needs the host at one address on every start: guest memory keeps pointers into it (the GOT entries of HLE functions and the copies the game
// makes of them, NVN procedure addresses, return addresses, pointers to its statics). Android loads a library wherever there is room, so this one reserves the range first and
// loads the host into it with android_dlopen_ext (ANDROID_DLEXT_RESERVED_ADDRESS). When the range is taken the host loads anywhere, as before: a snapshot written at another address
// is then not restored (hle_hibsnap.c checks the host's address and identity), and the game starts as usual.
// Kept apart from libdmhost.so (its own rule in dm/android/Makefile) and as small as can be: it is loaded at a random address and only runs at the start
#include <android/dlext.h>
#include <android/log.h>
#include <android/native_activity.h>
#include <dlfcn.h>
#include <stdint.h>
#include <sys/mman.h>

#define HOST_BASE 0xF00000000ull   // 60 GB: below hibernation's layout (0x1000000000, hle_hib.c), far below where Android maps libraries (the top of the 39-bit space)
#define HOST_SIZE 0x10000000ull    // 256 MB reserved; libdmhost.so maps about 30 MB

typedef void (*on_create_fn)(ANativeActivity *, void *, size_t);
__attribute__((visibility("default"))) void ANativeActivity_onCreate(ANativeActivity *a, void *saved, size_t saved_n) {
    static on_create_fn real;
    if (!real) {
        void *want = (void *)(uintptr_t)HOST_BASE, *h = NULL;
        void *p = mmap(want, HOST_SIZE, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0);
        if (p == want) {
            android_dlextinfo info = {.flags = ANDROID_DLEXT_RESERVED_ADDRESS, .reserved_addr = p, .reserved_size = HOST_SIZE};
            h = android_dlopen_ext("libdmhost.so", RTLD_NOW, &info);
            if (!h) { __android_log_print(ANDROID_LOG_WARN, "dmboot", "libdmhost.so not loaded at %p (%s): loading it anywhere", p, dlerror()); munmap(p, HOST_SIZE); }
        } else {
            if (p != MAP_FAILED) munmap(p, HOST_SIZE);
            __android_log_print(ANDROID_LOG_WARN, "dmboot", "the range at %p is taken: libdmhost.so loads anywhere (no hibernation restore this session)", want);
        }
        if (!h) h = dlopen("libdmhost.so", RTLD_NOW);
        if (!h) { __android_log_print(ANDROID_LOG_ERROR, "dmboot", "libdmhost.so: %s", dlerror()); return; }
        real = (on_create_fn)dlsym(h, "ANativeActivity_onCreate");
        if (!real) { __android_log_print(ANDROID_LOG_ERROR, "dmboot", "libdmhost.so has no ANativeActivity_onCreate"); return; }
    }
    real(a, saved, saved_n);
}
