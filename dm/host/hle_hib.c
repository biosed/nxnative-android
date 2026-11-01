// Hibernation ("save state to disk"), DM_HIBERNATE=1, off by default. This file holds the fixed address layout it needs: a snapshot of guest memory is restorable in a later process
// only if every guest address means the same there, so the guest address space and the game's modules are placed at one fixed address on every boot instead of where mmap(NULL)
// happens to put them. One reservation covers both: the module window at its start (the modules one after another, then the host arena), the guest address space after it.
//   HIB_BASE                        modules (map_image, in load order; 16 KB aligned)
//   HIB_BASE + HIB_MODWIN - ARENA   host arena (objects the guest holds pointers to: hle_hib_arena)
//   HIB_BASE + HIB_MODWIN           guest address space (hle_svc.c mem_init_once: alias, stack, heap)
// The base differs by platform. Android arm64 has a 39-bit address space (512 GB) and maps libraries and mmap(NULL) top down from near its end, so 64 GB is free there.
// macOS reserves 0xfc0000000-0x7000000000 (vm_region tag 31; a hint inside it lands at 0x7000000000) and hands out mmap(NULL) upward from 0x7000000000, so the Mac's base is above that, at 512 GB.
// When the range is taken, hibernation is off for the session (the game runs as without it); it is never moved, a snapshot names the base it was written with.
#include "dm.h"
#include <dlfcn.h>
#include <sys/mman.h>
#include <unistd.h>
#ifdef __APPLE__
#include <TargetConditionals.h>
#include <mach-o/dyld.h>
#include <spawn.h>
#endif

#ifdef __APPLE__
#define HIB_BASE   0x8000000000ull     // 512 GB
#else
#define HIB_BASE   0x1000000000ull     // 64 GB
#endif
#define HIB_MODWIN 0x100000000ull      // 4 GB for the modules and the arena (address space only; pages are committed as they are mapped)
#define HIB_ARENA  0x10000000ull       // 256 MB arena at the top of the module window

static int g_hib = -1;                 // -1 unread, 0 off, 1 on
static uint8_t *g_win; static size_t g_win_len;   // the reservation (NULL when not reserved)
static size_t g_mod_used;              // bytes of the module window handed out
static size_t g_arena_used;
static pthread_mutex_t g_hib_lock = PTHREAD_MUTEX_INITIALIZER;

int dm_hib_on(void) {
    if (g_hib < 0) { const char *e = getenv("DM_HIBERNATE"); g_hib = e && atoi(e) > 0; }
    return g_hib;
}
// Off for the rest of the session (the layout could not be had): before any module is mapped, everything falls back to mmap(NULL)
static void hib_off(const char *why) { LOG("[RED] hibernation off for this session: %s", why); g_hib = 0; }

size_t svc_guest_reserve_size(void);   // hle_svc.c: the guest address space block's size (the profile's, or wider with DM_ALIAS_SIZE)

// The reservation, once, before the first module is mapped. PROT_NONE: it takes address space only
static void win_init(void) {
    static int done; if (done) return; done = 1;
    if (!dm_hib_on()) return;
    size_t len = HIB_MODWIN + svc_guest_reserve_size();
    uint8_t *want = (uint8_t *)(uintptr_t)HIB_BASE;
#ifdef MAP_FIXED_NOREPLACE
    uint8_t *p = mmap(want, len, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0);
#else
    uint8_t *p = mmap(want, len, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);   // a hint: the Mac's kernel takes it when the range is free
#endif
    if (p == MAP_FAILED || p != want) {
        if (p != MAP_FAILED) munmap(p, len);
        char why[128]; snprintf(why, sizeof why, "fixed layout at %#llx (+%#zx) not free (got %p)", (unsigned long long)HIB_BASE, len, p == MAP_FAILED ? NULL : (void *)p);
        hib_off(why); return;
    }
    g_win = p; g_win_len = len;
    uintptr_t dm_hib_image_base(void);
    LOG("hibernate: fixed layout at %p: modules + arena %#llx, guest address space %p +%#zx; host image at %#lx", (void *)p, (unsigned long long)HIB_MODWIN, (void *)(p + HIB_MODWIN), len - (size_t)HIB_MODWIN, (unsigned long)dm_hib_image_base());
}

// Module memory (loader.c map_image): len bytes, readable and writable, at the next 16 KB of the module window; mmap(NULL) without hibernation
void *dm_hib_module_map(size_t len) {
    pthread_mutex_lock(&g_hib_lock);
    win_init();
    void *r;
    if (!g_win) r = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    else {
        size_t at = (g_mod_used + 0x3FFF) & ~(size_t)0x3FFF;
        if (at + len > HIB_MODWIN - HIB_ARENA) die("hibernate: the modules need more than the module window (%#zx + %#zx)", at, len);
        r = mmap(g_win + at, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
        if (r != MAP_FAILED && r != g_win + at) die("hibernate: module map at %p landed at %p", (void *)(g_win + at), r);
        g_mod_used = at + len;
    }
    pthread_mutex_unlock(&g_hib_lock);
    return r;
}

// The guest address space block (hle_svc.c): inside the reservation, still PROT_NONE; NULL without hibernation (the caller maps its own)
void *dm_hib_guest_as(size_t len) {
    pthread_mutex_lock(&g_hib_lock);
    win_init();
    void *r = NULL;
    if (g_win) {
        if (len != g_win_len - HIB_MODWIN) die("hibernate: guest address space %#zx, the reservation was made for %#zx", len, g_win_len - (size_t)HIB_MODWIN);
        r = g_win + HIB_MODWIN;
    }
    pthread_mutex_unlock(&g_hib_lock);
    return r;
}

// Host arena: memory for host objects that guest memory points to (thread-local regions, the guest main thread's stack, ...), at fixed addresses so that those pointers hold
// in a restored process. Bump allocation, never freed; zeroed, readable and writable. NULL when hibernation is off (the caller allocates as before)
void *dm_hib_arena_alloc(size_t len, size_t align) {
    if (!dm_hib_on()) return NULL;
    pthread_mutex_lock(&g_hib_lock);
    win_init();
    void *r = NULL;
    if (g_win) {
        size_t pg = (size_t)getpagesize();
        if (align < 16) align = 16;
        size_t at = (g_arena_used + align - 1) & ~(align - 1);
        if (at + len > HIB_ARENA) die("hibernate: host arena full (%#zx + %#zx)", at, len);
        uint8_t *base = g_win + HIB_MODWIN - HIB_ARENA;
        // pages of the arena are made accessible as it grows
        size_t hi = (at + len + pg - 1) & ~(pg - 1), was = (g_arena_used + pg - 1) & ~(pg - 1);
        if (hi > was && mprotect(base + was, hi - was, PROT_READ | PROT_WRITE)) die("hibernate: arena mprotect (errno %d)", errno);
        r = base + at; g_arena_used = at + len;
    }
    pthread_mutex_unlock(&g_hib_lock);
    return r;
}

// For the snapshot and the audit: the reservation and what is in use of it
void dm_hib_layout(uint8_t **win, size_t *win_len, size_t *mod_used, uint8_t **arena, size_t *arena_used) {
    if (win) *win = g_win;
    if (win_len) *win_len = g_win_len;
    if (mod_used) *mod_used = g_mod_used;
    if (arena) *arena = g_win ? g_win + HIB_MODWIN - HIB_ARENA : NULL;
    if (arena_used) *arena_used = g_arena_used;
}

// DM_CTL "hib <command>": "audit <path>" writes the pointer audit (hle_hibaudit.c)
void dm_hib_audit(const char *path);
void dm_hib_ctl(const char *cmd) {
    if (!strncmp(cmd, "audit ", 6)) { dm_hib_audit(cmd + 6); return; }
    LOG("[RED] DM_CTL hib: unknown command %s", cmd);
}

// The host executable at a fixed address too: guest memory keeps pointers into it (the GOT entries of HLE functions and the copies the game makes of them, NVN procedure
// addresses, return addresses into dm_thread_enter, pointers to static objects such as nn::vi's display). On the Mac the process executes itself again at once with ASLR off
// (posix_spawn's private _POSIX_SPAWN_DISABLE_ASLR, which lldb uses; POSIX_SPAWN_SETEXEC keeps the process ID for whoever started it), so the executable loads at its link
// address. Android loads libdmhost.so at a reserved address instead (dm/android, later). Called first thing in main; returns when nothing needs doing
#if defined(__APPLE__) && !TARGET_OS_IPHONE
extern char **environ;
void dm_hib_exec_fixed(int argc, char **argv) {
    (void)argc;
    if (!dm_hib_on() || !_dyld_get_image_vmaddr_slide(0)) return;
    if (getenv("DM_HIB_REEXEC")) { LOG("[RED] hibernate: the executable still slid by %#lx after executing itself with ASLR off", (long)_dyld_get_image_vmaddr_slide(0)); return; }
    char path[4096]; uint32_t n = sizeof path;
    if (_NSGetExecutablePath(path, &n)) return;
    setenv("DM_HIB_REEXEC", "1", 1);
    posix_spawnattr_t a; posix_spawnattr_init(&a);
    posix_spawnattr_setflags(&a, 0x100 /* _POSIX_SPAWN_DISABLE_ASLR */ | POSIX_SPAWN_SETEXEC);
    int r = posix_spawn(NULL, path, NULL, &a, argv, environ);   // returns only on failure
    LOG("[RED] hibernate: executing itself with ASLR off failed (%d): the executable stays where it is", r);
    unsetenv("DM_HIB_REEXEC");
}
#else
void dm_hib_exec_fixed(int argc, char **argv) { (void)argc; (void)argv; }
#endif
// The host image's base and identity, for the snapshot header and the audit: a snapshot is restored only by the same executable at the same address
uintptr_t dm_hib_image_base(void) {
#ifdef __APPLE__
    return (uintptr_t)_dyld_get_image_header(0);
#else
    Dl_info di; return dladdr((void *)dm_hib_image_base, &di) ? (uintptr_t)di.dli_fbase : 0;
#endif
}

// The hooks the shared files call through (function pointers, so that unit tests can link those files without this one), set before main
extern void *(*dm_hib_guest_as_hook)(size_t), *(*dm_hib_module_map_hook)(size_t), *(*dm_hib_thunk_hook)(void *); extern void (*dm_hib_ctl_hook)(const char *);
void *dm_libc_thunk(void *fn);   // hle_table.c
static void *thunk_if_on(void *fn) { return dm_hib_on() ? dm_libc_thunk(fn) : NULL; }
__attribute__((constructor)) static void hib_hooks(void) {
    dm_hib_guest_as_hook = dm_hib_guest_as; dm_hib_module_map_hook = dm_hib_module_map; dm_hib_thunk_hook = thunk_if_on; dm_hib_ctl_hook = dm_hib_ctl;
}
