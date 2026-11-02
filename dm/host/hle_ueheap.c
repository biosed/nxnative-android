// The game heap the host runs for an Unreal Engine game (hle_uemalloc.c), inside the hibernation layout. hle_uemalloc.c puts Unreal's FMalloc on a host malloc zone; that heap holds
// nearly all of the game's objects (and the stacks of the threads Unreal makes), so with hibernation on it cannot live in the host's own malloc, outside the snapshot. Here it is
// dlmalloc (dm/third_party/dlmalloc, mspaces only) in the host heap region at the end of the fixed reservation (hle_hib.c dm_hib_heap_region): the allocator's state is in the
// region too, so the snapshot's memory chunks carry the heap whole, and a restored process goes on allocating from it where the old one stopped.
// The region is cut into UEH_N spaces of UEH_SPACE bytes, one mspace each with a lock of its own: a thread allocates from the space it was given (round robin, the next one when it
// is full); free and realloc find the space by the address. A space grows from its start (dlmalloc's segment requests, UEH_GRAIN at least) and pages beyond what it has used stay
// inaccessible, so the snapshot reads only what the heap touched. Memory is never given back to the system (no trim, no direct mappings): Unreal keeps its peak anyway.
// Host state in a snapshot (section "ueheap"): the spaces' mspace addresses and how far each has grown.
#include "dm.h"
#include "hle.h"
#include <stdatomic.h>
#include <sys/mman.h>

#define UEH_N     4
#define UEH_SPACE 0x100000000ull   // 4 GB of address space each (the region is 16 GB, hle_hib.c HIB_HEAP)
#define UEH_GRAIN (2u << 20)       // segments of 2 MB at least: fewer mprotect calls

static uint8_t *g_base;            // the region (NULL: the heap is not here, hle_uemalloc.c keeps its malloc zone)
static void *g_ms[UEH_N];          // the mspaces (made at a space's first allocation)
static _Atomic uint64_t g_hw[UEH_N];   // bytes of each space in use by its mspace (readable and writable)
static _Atomic int g_rr;           // round robin for the threads' spaces
static pthread_mutex_t g_mk = PTHREAD_MUTEX_INITIALIZER;
static __thread int t_sp = -1;     // the space this thread allocates from
static __thread int t_cur;         // the space whose mspace is asking for memory (ueh_seg)

// dlmalloc's segment requests: the next part of the current space. Never released
static void *ueh_seg(size_t n) {
    int k = t_cur; uint64_t at = atomic_load(&g_hw[k]);
    if (at + n > UEH_SPACE) return (void *)~(uintptr_t)0;   // MFAIL: the space is full
    uint8_t *p = g_base + (uint64_t)k * UEH_SPACE + at;
    if (mprotect(p, n, PROT_READ | PROT_WRITE)) return (void *)~(uintptr_t)0;
    atomic_store(&g_hw[k], at + n);
    return p;
}

#define ONLY_MSPACES 1
#define MSPACES 1
#define USE_LOCKS 1
#define HAVE_MORECORE 0
#define HAVE_MMAP 1
#define HAVE_MREMAP 0
#define MMAP_CLEARS 1                     // fresh pages of the region are zero
#define MMAP(s) ueh_seg(s)
#define DIRECT_MMAP(s) ueh_seg(s)
#define MUNMAP(a, s) (-1)                 // never given back: dlmalloc keeps the memory
#define DEFAULT_GRANULARITY ((size_t)UEH_GRAIN)
#define DEFAULT_MMAP_THRESHOLD (~(size_t)0)   // no direct mappings: big blocks come from the space too
#define DEFAULT_TRIM_THRESHOLD (~(size_t)0)
#define NO_MALLINFO 1
#define NO_MALLOC_STATS 1
#define LACKS_TIME_H 1
#define DLMALLOC_EXPORT static
#define ABORT die("game heap (dlmalloc): corrupted or misused")
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-variable"
#pragma clang diagnostic ignored "-Wunused-but-set-variable"
#pragma clang diagnostic ignored "-Wnull-pointer-arithmetic"
#pragma clang diagnostic ignored "-Wexpansion-to-defined"
#endif
#include "../third_party/dlmalloc/malloc.c"
#ifdef __clang__
#pragma clang diagnostic pop
#endif

int dm_hib_on(void); void *dm_hib_heap_region(size_t *len);
// Whether the game heap runs here: hibernation on and the region reserved. Asked once, when hle_uemalloc.c installs its allocator
int dm_ueheap_init(void) {
    if (g_base) return 1;
    if (!dm_hib_on()) return 0;
    size_t len = 0; uint8_t *b = dm_hib_heap_region(&len);
    if (!b || len < UEH_N * UEH_SPACE) return 0;
    g_base = b;
    LOG("hibernate: the game heap runs in the fixed layout (%d spaces of %llu GB at %p)", UEH_N, (unsigned long long)(UEH_SPACE >> 30), (void *)b);
    return 1;
}
static int space_of(const void *p) {
    uint64_t o = (uint64_t)(uintptr_t)p - (uint64_t)(uintptr_t)g_base;
    if (!p || o >= UEH_N * UEH_SPACE) die("game heap: %p is not a block of it", p);
    return (int)(o / UEH_SPACE);
}
static void *space(int k) {
    if (g_ms[k]) return g_ms[k];
    pthread_mutex_lock(&g_mk);
    if (!g_ms[k]) { t_cur = k; g_ms[k] = create_mspace(0, 1); if (!g_ms[k]) die("game heap: space %d could not be made", k); }
    pthread_mutex_unlock(&g_mk);
    return g_ms[k];
}
void *dm_ueheap_alloc(size_t n, size_t align) {
    if (t_sp < 0) t_sp = atomic_fetch_add(&g_rr, 1) % UEH_N;
    for (int i = 0; i < UEH_N; i++) {
        int k = (t_sp + i) % UEH_N; void *m = space(k); t_cur = k;
        void *p = align > 16 ? mspace_memalign(m, align, n) : mspace_malloc(m, n);
        if (p) return p;
    }
    return NULL;
}
void dm_ueheap_free(void *p) { if (p) { int k = space_of(p); t_cur = k; mspace_free(g_ms[k], p); } }
size_t dm_ueheap_size(const void *p) { return p ? mspace_usable_size(p) : 0; }
// Realloc keeping 16-byte alignment (bigger alignments: hle_uemalloc.c moves the block itself)
void *dm_ueheap_realloc(void *p, size_t n) {
    if (!p) return dm_ueheap_alloc(n, 16);
    int k = space_of(p); t_cur = k;
    void *q = mspace_realloc(g_ms[k], p, n);
    if (q) return q;
    q = dm_ueheap_alloc(n, 16);   // its space is full: elsewhere
    if (!q) return NULL;
    size_t old = mspace_usable_size(p); memcpy(q, p, old < n ? old : n); dm_ueheap_free(p);
    return q;
}

// ---- hibernation (hle_hibsnap.c section "ueheap") ----
typedef struct { uint32_t version, n; uint64_t base, space, ms[UEH_N], hw[UEH_N]; int32_t rr, pad; } hib_ueh;
size_t dm_hib_ueheap_save(uint8_t **out) {
    hib_ueh *h = calloc(1, sizeof *h);
    h->version = 1; h->n = UEH_N; h->base = (uint64_t)(uintptr_t)g_base; h->space = UEH_SPACE; h->rr = atomic_load(&g_rr);
    for (int k = 0; k < UEH_N; k++) { h->ms[k] = (uint64_t)(uintptr_t)g_ms[k]; h->hw[k] = atomic_load(&g_hw[k]); }
    *out = (uint8_t *)h; return sizeof *h;
}
int dm_hib_ueheap_restore(const uint8_t *b, size_t n) {
    hib_ueh h; if (n != sizeof h) return -1; memcpy(&h, b, sizeof h);
    if (h.version != 1 || h.n != UEH_N || h.space != UEH_SPACE) return -1;
    if (h.base) dm_ueheap_init();   // a restore before the allocator's install (it asks again then)
    if (!h.base) { if (g_base) { LOG("[RED] hibernate: the snapshot's game heap was the host's malloc, this process runs it in the layout"); return -1; } return 0; }
    if (h.base != (uint64_t)(uintptr_t)g_base) { LOG("[RED] hibernate: the snapshot's game heap is at %#llx, this process's at %p", (unsigned long long)h.base, (void *)g_base); return -1; }
    ensure_initialization();
    for (int k = 0; k < UEH_N; k++) {
        if (h.hw[k] > UEH_SPACE) return -1;
        uint8_t *s = g_base + (uint64_t)k * UEH_SPACE;
        if (h.hw[k] && mprotect(s, h.hw[k], PROT_READ | PROT_WRITE)) return -1;
        atomic_store(&g_hw[k], h.hw[k]); g_ms[k] = (void *)(uintptr_t)h.ms[k];
        if (g_ms[k]) {
            mstate m = (mstate)g_ms[k];
            m->magic = mparams.magic;   // dlmalloc checks each mspace against a per-process value
            INITIAL_LOCK(&m->mutex);    // no thread holds it in this process
        }
    }
    atomic_store(&g_rr, h.rr);
    LOG("hibernate: game heap restored: %.1f MB in %d spaces", (double)(h.hw[0] + h.hw[1] + h.hw[2] + h.hw[3]) / 1048576.0, UEH_N);
    return 0;
}
