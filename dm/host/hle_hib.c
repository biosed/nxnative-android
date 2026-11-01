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
#include "hle.h"
#include "hle_hib.h"
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

// Arena blocks for host objects that come and go (thread-local regions, host-created threads' ThreadType): a 16-byte header with the size class, then the object; a freed block
// goes on its class's list (classes are the sizes rounded up to 64 bytes, at most 64 KB; bigger blocks are never freed, the guest main thread's stack is one)
void dm_hib_layout(uint8_t **win, size_t *win_len, size_t *mod_used, uint8_t **arena, size_t *arena_used);
typedef struct fblk { struct fblk *next; } fblk;
static fblk *g_free[1024];
static void *arena_calloc(size_t size) {
    size_t cls = (size + 63) / 64; if (!cls) cls = 1;
    if (cls < 1024) {
        pthread_mutex_lock(&g_hib_lock); fblk *b = g_free[cls]; if (b) g_free[cls] = b->next; pthread_mutex_unlock(&g_hib_lock);
        if (b) { memset(b, 0, cls * 64); return b; }
    }
    if (cls >= 1024) return dm_hib_arena_alloc((size + 0x3FFF) & ~(size_t)0x3FFF, 0x4000);   // whole pages, no header (a thread stack): never freed
    uint64_t *h = dm_hib_arena_alloc(16 + cls * 64, 64);   // fresh arena pages are zero
    if (!h) return NULL;
    h[0] = cls; return h + 2;
}
static int arena_free(void *p) {
    uint8_t *arena; size_t used; dm_hib_layout(NULL, NULL, NULL, &arena, &used);
    if (!arena || (uint8_t *)p < arena || (uint8_t *)p >= arena + used) return 0;
    size_t cls = ((uint64_t *)p)[-2];
    if (cls < 1024) { pthread_mutex_lock(&g_hib_lock); fblk *b = p; b->next = g_free[cls]; g_free[cls] = b; pthread_mutex_unlock(&g_hib_lock); }
    return 1;
}

// Hibernation's own state in a snapshot (section "arena"): the arena's allocation state and the restartable calls' stub slots (both come out the same in every process that
// loads the same modules, checked on restore)
typedef struct { uint64_t mod_used, arena_used; uint64_t free_[1024]; uint64_t rs[16]; int32_t nrs, pad; } hib_arena;
size_t dm_hib_arena_save(uint8_t **out) {
    hib_arena *h = calloc(1, sizeof *h);
    pthread_mutex_lock(&g_hib_lock);
    h->mod_used = g_mod_used; h->arena_used = g_arena_used; for (int i = 0; i < 1024; i++) h->free_[i] = (uint64_t)(uintptr_t)g_free[i];
    pthread_mutex_unlock(&g_hib_lock);
    *out = (uint8_t *)h; return sizeof *h;
}
int dm_hib_arena_restore_state(const uint8_t *b, size_t n);
// For the snapshot and the audit: the reservation and what is in use of it
void dm_hib_layout(uint8_t **win, size_t *win_len, size_t *mod_used, uint8_t **arena, size_t *arena_used) {
    if (win) *win = g_win;
    if (win_len) *win_len = g_win_len;
    if (mod_used) *mod_used = g_mod_used;
    if (arena) *arena = g_win ? g_win + HIB_MODWIN - HIB_ARENA : NULL;
    if (arena_used) *arena_used = g_arena_used;
}

// DM_CTL "hib <command>": "audit <path>" writes the pointer audit (hle_hibaudit.c), "save" writes the snapshot and "verify [path]" checks one (hle_hibsnap.c), "park" / "unpark" stop and start every guest thread
void dm_hib_audit(const char *path);
void dm_hib_ctl(const char *cmd) {
    if (!strncmp(cmd, "audit ", 6)) { dm_hib_audit(cmd + 6); return; }
    if (!strcmp(cmd, "save")) { int dm_hib_save(void); dm_hib_save(); return; }
    if (!strncmp(cmd, "verify", 6)) { int dm_hib_verify(const char *); const char *p = cmd + 6; while (*p == ' ') p++; dm_hib_verify(p); return; }
    if (!strcmp(cmd, "park")) { char why[1024]; if (dm_guest_park_all(3000, why, sizeof why)) LOG("[RED] hib park: %s", why); else LOG("hib park: parked"); return; }
    if (!strcmp(cmd, "unpark")) { dm_guest_unpark_all(); LOG("hib unpark: running"); return; }
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
    { const char *v = getenv("DM_HIB_VERIFY"); if (v && *v) { int dm_hib_verify(const char *); exit(dm_hib_verify(v) ? 1 : 0); } }   // tool mode: check a snapshot file and exit
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

// ---- Restartable host calls (hle.h HLE_HIB_RESTARTABLE) ----
// dm_hib_rs_body is entered with the call's registers as at its entry and x16 = the function. It keeps them in a frame on the caller's stack (the guest's), registers the frame
// with the thread (dm_hib_rs_enter), calls the function and returns its result (x0 / x1, q0-q3). Frame, 0x160 bytes: x0-x8 at 0, x19-x28 at 0x48, x29 / x30 at 0x98, the sp at entry
// at 0xa8, the function at 0xb0, d8-d15 at 0xb8, the results at 0x100. A restored thread loads those registers and enters dm_hib_rs_body again: the call starts over.
// Each restartable import gets an entry stub dm_hib_rs_e<k> that loads its function from slot k; host code calls one through dm_hib_rs_call(fn, arg)
#define RS_N 16
uint64_t dm_hib_rs_slot[RS_N];
#ifdef __APPLE__
#define RS_SLOT(k) "    adrp x16, _dm_hib_rs_slot@PAGE\n    add x16, x16, _dm_hib_rs_slot@PAGEOFF\n    ldr x16, [x16, #" #k "*8]\n"
#else
#define RS_SLOT(k) "    adrp x16, dm_hib_rs_slot\n    add x16, x16, :lo12:dm_hib_rs_slot\n    ldr x16, [x16, #" #k "*8]\n"
#endif
#define RS_ENTRY(k) ".p2align 2\n.globl " DM_ASM_SYM(dm_hib_rs_e##k) "\n" DM_ASM_SYM(dm_hib_rs_e##k) ":\n" RS_SLOT(k) "    b " DM_ASM_SYM(dm_hib_rs_body) "\n"
__asm__(".text\n.p2align 2\n.globl " DM_ASM_SYM(dm_hib_rs_body) "\n" DM_ASM_SYM(dm_hib_rs_body) ":\n"
        "    sub sp, sp, #0x160\n"
        "    stp x0, x1, [sp, #0x00]\n    stp x2, x3, [sp, #0x10]\n    stp x4, x5, [sp, #0x20]\n    stp x6, x7, [sp, #0x30]\n    str x8, [sp, #0x40]\n"
        "    stp x19, x20, [sp, #0x48]\n    stp x21, x22, [sp, #0x58]\n    stp x23, x24, [sp, #0x68]\n    stp x25, x26, [sp, #0x78]\n    stp x27, x28, [sp, #0x88]\n"
        "    stp x29, x30, [sp, #0x98]\n    add x9, sp, #0x160\n    stp x9, x16, [sp, #0xa8]\n"
        "    stp d8, d9, [sp, #0xb8]\n    stp d10, d11, [sp, #0xc8]\n    stp d12, d13, [sp, #0xd8]\n    stp d14, d15, [sp, #0xe8]\n"
        "    add x29, sp, #0x98\n    mov x0, sp\n    bl " DM_ASM_SYM(dm_hib_rs_enter) "\n"
        "    ldp x0, x1, [sp, #0x00]\n    ldp x2, x3, [sp, #0x10]\n    ldp x4, x5, [sp, #0x20]\n    ldp x6, x7, [sp, #0x30]\n    ldr x8, [sp, #0x40]\n    ldr x16, [sp, #0xb0]\n"
        "    blr x16\n"
        "    stp x0, x1, [sp, #0x100]\n    stp q0, q1, [sp, #0x110]\n    stp q2, q3, [sp, #0x130]\n"
        "    bl " DM_ASM_SYM(dm_hib_rs_leave) "\n"
        "    ldp x0, x1, [sp, #0x100]\n    ldp q0, q1, [sp, #0x110]\n    ldp q2, q3, [sp, #0x130]\n"
        "    ldp x29, x30, [sp, #0x98]\n    add sp, sp, #0x160\n    ret\n"
        ".p2align 2\n.globl " DM_ASM_SYM(dm_hib_rs_call) "\n" DM_ASM_SYM(dm_hib_rs_call) ":\n    mov x16, x0\n    mov x0, x1\n    b " DM_ASM_SYM(dm_hib_rs_body) "\n"
        RS_ENTRY(0) RS_ENTRY(1) RS_ENTRY(2) RS_ENTRY(3) RS_ENTRY(4) RS_ENTRY(5) RS_ENTRY(6) RS_ENTRY(7)
        RS_ENTRY(8) RS_ENTRY(9) RS_ENTRY(10) RS_ENTRY(11) RS_ENTRY(12) RS_ENTRY(13) RS_ENTRY(14) RS_ENTRY(15));
extern const char dm_hib_rs_e0[] __asm__(DM_ASM_SYM(dm_hib_rs_e0));
#define RS_STUB_SIZE 16   // adrp, add, ldr, b
static int g_nrs;
// The entry stub for a function marked restartable (NULL for any other): one per function, given out as the guest's import
static void *rs_stub_for(void *fn) {
    unsigned long sz = 0; void *const *t = (void *const *)DM_SECTION_DATA(swrestart, &sz);
    int marked = 0; for (unsigned long i = 0; t && i < sz / sizeof *t; i++) if (t[i] == fn) marked = 1;
    if (!marked) return NULL;
    pthread_mutex_lock(&g_hib_lock);
    int k = 0; while (k < g_nrs && dm_hib_rs_slot[k] != (uint64_t)(uintptr_t)fn) k++;
    if (k == g_nrs) { if (g_nrs == RS_N) die("hibernate: more than %d restartable host calls", RS_N); dm_hib_rs_slot[g_nrs++] = (uint64_t)(uintptr_t)fn; }
    pthread_mutex_unlock(&g_hib_lock);
    return (void *)(dm_hib_rs_e0 + RS_STUB_SIZE * k);
}
// Host code calling a restartable function of its own (a host-created thread's body): through the stub with hibernation, directly otherwise
void dm_hib_rs_call(void *fn, void *arg);
void dm_hib_call(void (*fn)(void *), void *arg) { if (dm_hib_on()) dm_hib_rs_call((void *)fn, arg); else fn(arg); }

// ---- Resuming a restored thread (hle_thread.c hib_resume) ----
// dm_gate_resume(src, dst, ret): copies the 800-byte gate frame src to dst (the bottom of this thread's gate stack top, where entry.S dm_gate keeps it) without a call (dst lies
// below the caller's stack pointer), then dispatches the gate from it (ret 0: the SVC runs from the start) or returns to the guest with its registers (ret 1)
// dm_hib_rs_resume(frame): loads a restartable call's entry registers from its frame (see dm_hib_rs_body) and enters the stub again
__asm__(".text\n.p2align 2\n.globl " DM_ASM_SYM(dm_gate_resume) "\n" DM_ASM_SYM(dm_gate_resume) ":\n"
        "    mov x9, #50\n    mov x12, x0\n    mov x13, x1\n"
        "1:  ldp x10, x11, [x12], #16\n    stp x10, x11, [x13], #16\n    subs x9, x9, #1\n    b.ne 1b\n"
        "    mov sp, x1\n"
        "    cbnz w2, 2f\n"
        "    mov x30, x1\n    b " DM_ASM_SYM(dm_gate_from_frame) "\n"
        "2:  b " DM_ASM_SYM(dm_gate_return_from_frame) "\n"
        ".p2align 2\n.globl " DM_ASM_SYM(dm_hib_rs_resume) "\n" DM_ASM_SYM(dm_hib_rs_resume) ":\n"
        "    mov x17, x0\n"
        "    ldp d8, d9, [x17, #0xb8]\n    ldp d10, d11, [x17, #0xc8]\n    ldp d12, d13, [x17, #0xd8]\n    ldp d14, d15, [x17, #0xe8]\n"
        "    ldp x19, x20, [x17, #0x48]\n    ldp x21, x22, [x17, #0x58]\n    ldp x23, x24, [x17, #0x68]\n    ldp x25, x26, [x17, #0x78]\n    ldp x27, x28, [x17, #0x88]\n"
        "    ldp x29, x30, [x17, #0x98]\n    ldp x9, x16, [x17, #0xa8]\n    mov sp, x9\n"
        "    ldp x0, x1, [x17, #0x00]\n    ldp x2, x3, [x17, #0x10]\n    ldp x4, x5, [x17, #0x20]\n    ldp x6, x7, [x17, #0x30]\n    ldr x8, [x17, #0x40]\n"
        "    b " DM_ASM_SYM(dm_hib_rs_body) "\n");

void dm_gate_resume(const uint64_t *src, uint64_t *dst, int ret); void dm_hib_rs_resume(const uint64_t *frame);
extern uint64_t dm_gate_top[]; extern const uint8_t dm_gate_stubs[];
static void resume_frame(const uint64_t *frame, int mode) {
    if (mode == 2) dm_hib_rs_resume(frame);
    // the frame goes where entry.S dm_gate keeps it: under this thread's gate stack top (its gate slot, thread.c guest_gate_stack)
    int slot = (int)(((const uint8_t *)dm_slot_get(g_keygate) - dm_gate_stubs) / 16);
    dm_gate_resume(frame, (uint64_t *)(uintptr_t)(dm_gate_top[slot] - 800), mode);
}
// The hooks the shared files call through (function pointers, so that unit tests can link those files without this one), set before main
void *dm_libc_thunk(void *fn);   // hle_table.c
static void *thunk_if_on(void *fn) {   // what the guest gets instead of a host function: a libc thunk or a restartable call's stub
    if (!dm_hib_on()) return NULL;
    void *t = dm_libc_thunk(fn); return t ? t : rs_stub_for(fn);
}
__attribute__((constructor)) static void hib_hooks(void) {
    dm_hib_guest_as_hook = dm_hib_guest_as; dm_hib_module_map_hook = dm_hib_module_map; dm_hib_thunk_hook = thunk_if_on; dm_hib_ctl_hook = dm_hib_ctl;
    dm_hib_alloc_hook = arena_calloc; dm_hib_free_hook = arena_free; dm_hib_call_hook = dm_hib_call; dm_hib_native_hold_hook = dm_hib_native_hold;
    void dm_hib_suspend_now(int on); dm_hib_suspend_hook = dm_hib_suspend_now;
    void guest_clock_kick(void); dm_hib_kick_hook = guest_clock_kick;
    void dm_hib_forget_now(void); dm_hib_forget_hook = dm_hib_forget_now; dm_hib_resume_frame_hook = resume_frame;
}

int dm_hib_arena_restore_state(const uint8_t *b, size_t n) {
    if (n != sizeof(hib_arena)) return -1;
    const hib_arena *h = (const hib_arena *)b;
    pthread_mutex_lock(&g_hib_lock);
    int ok = h->mod_used == g_mod_used;
    if (ok) {
        size_t pg = (size_t)getpagesize(), was = (g_arena_used + pg - 1) & ~(pg - 1), now = (h->arena_used + pg - 1) & ~(pg - 1);
        uint8_t *base = g_win + HIB_MODWIN - HIB_ARENA;
        if (now > was && mprotect(base + was, now - was, PROT_READ | PROT_WRITE)) ok = 0;   // the arena's pages as far as the snapshot used them (its memory chunks fill them)
        g_arena_used = h->arena_used; for (int i = 0; i < 1024; i++) g_free[i] = (fblk *)(uintptr_t)h->free_[i];
    }
    pthread_mutex_unlock(&g_hib_lock);
    return ok ? 0 : -1;
}
