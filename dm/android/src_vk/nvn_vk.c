// Vulkan graphics backend for Android: the nvn_mtl.h interface the NVN layer calls (on Apple it is nvn_mtl.m), replaying each submission's command list into a Vulkan command buffer.
// Shaders are the SPIR-V the Metal chain translates (dm/android/tools/spv_android.py: <key>.spv + <key>.vk.txt in DM_VK_DIR), every resource in set 0 with a binding of its own; here the vertex
// stage keeps set 0 and the fragment stage is moved to set 1 (its DescriptorSet decorations rewritten at load), so one pipeline layout holds both.
// Memory: Vulkan cannot use the game's memory pools in place (no VK_EXT_external_memory_host on Mali), so what a draw reads from them -- uniform buffers, vertex and index data, the shader's
// own constants -- is copied into the submission's ring buffer when the draw is recorded. The command list is walked in order on the CPU and its in-order writes (inline uniform updates,
// buffer copies and fills) are applied to guest memory where they stand, so each draw copies what the GPU would have read at that point. Textures are Vulkan images: packaged ones
// (block-linear in the pool) are deswizzled and uploaded at first use; render targets, copies and clears stay on the GPU. Every image lives in VK_IMAGE_LAYOUT_GENERAL, and passes and copies
// are separated by full memory barriers: simple and correct first, faster later.
// Event values, counter reports and texture-to-buffer copies reach guest memory when the GPU has finished the submission (the completion thread), as on the Metal backend.
// With a window (the app, app/front_android.c) presented frames are scaled onto a swapchain, keeping the game's aspect unless DM_SCREEN_FIT=stretch; DM_SHOW_FPS=1 draws a frame-rate
// counter, DM_FPS=30 holds every frame two vsyncs. Without one (the adb front end) nothing is shown: DM_VK_SHOTS=<frame>,<frame>,... writes those frames to DM_VK_SHOT_DIR as BMP files.
#include "../../gfx/nvn/nvn_dirty.h"
#include "hle.h"
#include "nvn_obj.h"
#include "nvn_mtl.h"
#include "../tex/nvn_deswizzle.h"
#define VK_USE_PLATFORM_ANDROID_KHR
#include <vulkan/vulkan.h>
#include <android/native_window.h>
#include <stdatomic.h>
#include <sys/stat.h>
#include <sys/resource.h>

#define VKCHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) die("Vulkan: %s failed (%d)", #x, (int)r_); } while (0)
#define RED_ONCE(id, ...) do { static int once_; if (!once_++) LOG("[RED] vk: " __VA_ARGS__); } while (0)

static VkInstance g_inst; static VkPhysicalDevice g_pd; static VkDevice g_dev; static VkQueue g_q; static uint32_t g_qf;
static VkPhysicalDeviceMemoryProperties g_memp; static VkPhysicalDeviceProperties g_props;
static pthread_mutex_t g_qmu = PTHREAD_MUTEX_INITIALIZER;   // the queue (submissions, the present readback)
static pthread_mutex_t g_rmu = PTHREAD_MUTEX_INITIALIZER;   // texture / sampler registrations and the image table (game threads register while the submit thread reads)

static uint32_t mem_type(uint32_t bits, VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < g_memp.memoryTypeCount; i++) if ((bits >> i & 1) && (g_memp.memoryTypes[i].propertyFlags & want) == want) return i;
    for (uint32_t i = 0; i < g_memp.memoryTypeCount; i++) if (bits >> i & 1) return i;
    die("Vulkan: no memory type for %#x", bits);
}
static VkDeviceMemory mem_alloc(VkMemoryRequirements r, VkMemoryPropertyFlags want) {
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, r.size, mem_type(r.memoryTypeBits, want)};
    VkDeviceMemory m; VKCHECK(vkAllocateMemory(g_dev, &ai, NULL, &m)); return m;
}
static VkBuffer buf_new(VkDeviceSize n, VkBufferUsageFlags u, VkDeviceMemory *m, void **map) {
    VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, n, u, VK_SHARING_MODE_EXCLUSIVE, 0, NULL};
    VkBuffer b; VKCHECK(vkCreateBuffer(g_dev, &bi, NULL, &b));
    VkMemoryRequirements r; vkGetBufferMemoryRequirements(g_dev, b, &r);
    *m = mem_alloc(r, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VKCHECK(vkBindBufferMemory(g_dev, b, *m, 0));
    if (map) VKCHECK(vkMapMemory(g_dev, *m, 0, VK_WHOLE_SIZE, 0, map));
    return b;
}
#define ALL_BUF_USAGE (VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | \
                       VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT | \
                       VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT)

static _Atomic uint64_t g_big_bytes, g_extra_ring_bytes, g_ring0_bytes;   // memory of the big images and of the extra rings now held (the frame stats line)
// ---------- Image memory: 64 MB blocks, each with a list of its free ranges (first fit; a freed range joins its neighbours; a block with no image left is freed) ----------
// Thousands of textures would exceed the allocation count (Vulkan promises only 4096). Blocks used to be handed out in order and freed only when their last image was: a game
// streaming its textures in and out kept every block alive with a few images in it, and Super Mario Odyssey's opening reached 4 GB in 64 blocks and was killed for memory
typedef struct { VkDeviceSize off, size; } mfree;
typedef struct { VkDeviceMemory m; VkDeviceSize size; int live; uint32_t type; mfree *fr; int nfr, cfr; } mblock;
enum { NBLK = 256 };
static mblock g_blk[NBLK]; static int g_nblk; static pthread_mutex_t g_blkmu = PTHREAD_MUTEX_INITIALIZER;   // images are made on the submit thread and freed on the completion thread
static void fr_put(mblock *b, VkDeviceSize off, VkDeviceSize size) {   // return a range, joined with the free ranges next to it (the list is kept sorted)
    int k = 0; while (k < b->nfr && b->fr[k].off < off) k++;
    if (k > 0 && b->fr[k - 1].off + b->fr[k - 1].size == off) { b->fr[k - 1].size += size; if (k < b->nfr && off + size == b->fr[k].off) { b->fr[k - 1].size += b->fr[k].size; memmove(b->fr + k, b->fr + k + 1, (b->nfr - k - 1) * sizeof *b->fr); b->nfr--; } return; }
    if (k < b->nfr && off + size == b->fr[k].off) { b->fr[k].off = off; b->fr[k].size += size; return; }
    if (b->nfr == b->cfr) { b->cfr = b->cfr ? 2 * b->cfr : 16; b->fr = realloc(b->fr, b->cfr * sizeof *b->fr); }
    memmove(b->fr + k + 1, b->fr + k, (b->nfr - k) * sizeof *b->fr); b->fr[k] = (mfree){off, size}; b->nfr++;
}
static int fr_take(mblock *b, VkDeviceSize size, VkDeviceSize align, VkDeviceSize *off) {   // first fit
    for (int k = 0; k < b->nfr; k++) {
        VkDeviceSize o = (b->fr[k].off + align - 1) / align * align, end = b->fr[k].off + b->fr[k].size;
        if (o + size > end) continue;
        VkDeviceSize lo = b->fr[k].off, hi = o + size;
        memmove(b->fr + k, b->fr + k + 1, (b->nfr - k - 1) * sizeof *b->fr); b->nfr--;
        if (o > lo) fr_put(b, lo, o - lo);
        if (end > hi) fr_put(b, hi, end - hi);
        *off = o; return 1;
    }
    return 0;
}
static int img_mem_(VkMemoryRequirements r, VkDeviceMemory *m, VkDeviceSize *off);
static int img_mem(VkMemoryRequirements r, VkDeviceMemory *m, VkDeviceSize *off) { pthread_mutex_lock(&g_blkmu); int b = img_mem_(r, m, off); pthread_mutex_unlock(&g_blkmu); return b; }
static int img_mem_(VkMemoryRequirements r, VkDeviceMemory *m, VkDeviceSize *off) {
    uint32_t type = mem_type(r.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (r.size > (32u << 20)) { *m = mem_alloc(r, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT); *off = 0; atomic_fetch_add(&g_big_bytes, r.size); return -1; }   // big images get memory of their own
    for (int i = 0; i < g_nblk; i++) { mblock *b = &g_blk[i]; if (b->m && b->type == type && fr_take(b, r.size, r.alignment, off)) { b->live++; *m = b->m; return i; } }
    int i = 0; while (i < g_nblk && g_blk[i].m) i++;
    if (i == NBLK) die("Vulkan: image memory blocks exhausted");
    if (i == g_nblk) g_nblk++;
    mblock *b = &g_blk[i];
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, 64u << 20, type};
    VKCHECK(vkAllocateMemory(g_dev, &ai, NULL, &b->m));
    b->size = 64u << 20; b->live = 0; b->type = type; b->nfr = 0; fr_put(b, 0, b->size);
    if (!fr_take(b, r.size, r.alignment, off)) die("Vulkan: an image of %llu bytes does not fit a new memory block", (unsigned long long)r.size);
    b->live++; *m = b->m; return i;
}
static void img_mem_free(int blk, VkDeviceMemory m, VkDeviceSize off, VkDeviceSize size) {
    if (blk < 0) { vkFreeMemory(g_dev, m, NULL); atomic_fetch_sub(&g_big_bytes, size); return; }
    pthread_mutex_lock(&g_blkmu);
    mblock *b = &g_blk[blk]; fr_put(b, off, size);
    if (!--b->live) { vkFreeMemory(g_dev, b->m, NULL); b->m = VK_NULL_HANDLE; b->nfr = 0; }
    pthread_mutex_unlock(&g_blkmu);
}

// ---------- Submission contexts: command buffers, ring buffer, descriptor pool, the writes that wait for completion ----------
#define NCTX_MAX 32
// Submission contexts (DM_VK_CTX=<n>, 4..32, default 20). 12 was ~1.3 frames of Unreal's ~9 submissions, chosen while frames were GPU-bound ("24 bought nothing"). With the
// GPU at 37 ms of a 73 ms frame (FINAL FANTASY RESONANCE's castle, vk19.8) the CPU waited 26 ms a frame for a context whose ring a later submission of the same frame still read
// (its hold): CPU and GPU took turns. More contexts let the CPU record the next frame while the GPU runs this one; each costs a ring (4 MB up, following use)
static int NCTX = 20;
// earlier: NCTX 12   // ~1.3 frames of Unreal's ~9 submissions (24 bought nothing on a GPU-bound frame and cost ring memory): with 8 the CPU could not get one frame ahead, and reusing a context whose ring a later submission still read (its hold) drained the GPU first: CPU and GPU took turns (recording 12.7 + waiting 12.5 ms a frame) // submissions in flight: Unreal submits several command buffers a frame, and with 3 the CPU could not record a frame ahead of the GPU
#define NOQ 4096   // occlusion queries per submission
#define NTQ 1024   // GPU timestamps per submission (512 regions)
// Ring sizes: each context's own ring starts at RING_MIN and follows what its submissions use (ring_fit: grown to the peak, up to RING_MAX; halved after a stretch that used less
// than a quarter; back to RING_MIN on a low-memory signal); a submission that outgrows it gets extra rings of the size it needs, freed when it is done. Eight fixed 48 MB rings
// held 384 MB of host-visible memory whatever the game needed, and with FINAL FANTASY RESONANCE open HyperOS killed other apps for memory
#define RING_MIN (4u << 20)
#define RING_MAX (64u << 20)
#define RING_XMAX (256u << 20)
#define RING_CAP (160ull << 20)   // all contexts' own rings together: past it a ring does not grow, and a submission that outgrows it takes extra rings (freed when it is done)
typedef struct { int kind; void *dst; uint32_t mode, v; uint64_t addr; uint32_t type; const uint8_t *src; size_t n; } post_t;   // kind 5: n bytes from src where they differ from the snapshot at addr (4-byte words); kind 6: v rows of type bytes, mode apart
static uint64_t g_wb_words, g_wb_bytes;   // dispatch write-backs: words the GPU changed and written back / bytes of the ranges looked at (completion thread)   // kind 1 event, 2 counter, 3 copy ring → guest, 4 samples-passed report (src: n query indices; n == SIZE_MAX: unknown, reported visible)
typedef struct vtex vtex;
typedef struct {
    VkCommandPool pool; VkCommandBuffer cb, up; VkFence fence; VkDescriptorPool dp;
    VkBuffer ring; VkDeviceMemory ringm; uint8_t *ringp; VkDeviceSize used;
    post_t *post; int npost, cpost;
    uint64_t fence_val; int up_used;
    VkQueryPool qp;   // GPU timestamps at the start and the end of the submission
    VkQueryPool oq; uint32_t noq; int oq_init;
    VkQueryPool tq; uint32_t ntq, ntq_prev; int tq_init; uint8_t tkind[NTQ / 2]; uint64_t tsig[NTQ / 2]; uint32_t tdraws[NTQ / 2];   // GPU timestamps around each pass / dispatch / copy (gpu_mark): a pair per region, its kind   // occlusion queries of this submission (one per draw made while NVN's samples-passed counter counts), reset at its start
    VkFramebuffer *fb; int nfb, cfb;   // render passes' framebuffers (no dynamic rendering), destroyed when the submission is done
    VkBuffer ring0; VkDeviceMemory ringm0; uint8_t *ringp0;   // the context's own ring; ring / ringp move to extra rings when a submission outgrows it (loading screens upload hundreds of MB)
    VkDeviceSize ring0_size, ring_size, total, last_total, win_max; uint32_t nwin; uint64_t trim_seen;   // ring_size: of the ring in use; total: bytes this submission took from rings
    struct { VkBuffer b; VkDeviceMemory m; VkDeviceSize size; } *extra; int nextra;
    VkDescriptorPool *dps; int ndp, idp;   // descriptor pools, chained as a submission needs more (reset together at its start)
    int plog;   // a pass-logged frame recorded into it: its small write-backs are logged at completion (the values a dispatch computed, e.g. Unreal's exposure)
    VkBufferView *bv; int nbv, cbv;   // texel buffer views of this submission's descriptors (write_set), destroyed when it is done
    int reset_done;   // its command and descriptor pools were reset by the completion thread (ctx_complete), off the submit thread (each reset took ~9% of it)
    uint64_t hold;   // the last later submission that reads this one's ring (gw_find): the ring is not reused, nor its extra rings freed, before that one is done too
    // The work recorded into it (draws and dispatches), the last TRN kept and the heaviest noted: dumped when the GPU is lost (trace_dump), to name what hung it
    struct trace { char kind, gpu_ind; char a[17], b[17]; uint32_t n0, n1, n2, zero_tb; } tr[48]; uint32_t ntr, ndraw, ndisp, max_groups; struct trace heavy;
} ctx_t;
enum { TRN = 48 };
static _Atomic uint64_t g_gpu_ns, g_gpu_cum;   // g_gpu_cum: never reset (the long-frame lines)
static _Atomic uint64_t g_wait_cum, g_gw_wait_cum, g_up_cum, g_xring_n;   // cumulative: context waits, waits on GPU-written data, upload bytes, extra rings made (long frames)
static ctx_t g_ctx[NCTX_MAX]; static ctx_t *C; static int g_ci;
static uint64_t g_fence, g_done;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;
static int g_pend[NCTX_MAX], g_npend;   // contexts submitted and not yet completed, in order (under g_mu)

static uint64_t g_wgen;   // a new generation when the ring changes or the write log overflows: the per-submission copies are reused only within one (and only where no write since overlaps them, copy_in)
static uint32_t g_nwlog;   // writes logged in this generation (wlog_add)
static VkDeviceSize ring_alloc(VkDeviceSize n, VkDeviceSize align) {
    VkDeviceSize o = (C->used + align - 1) / align * align;
    if (o + n > C->ring_size) {   // full: an extra ring for the rest of this submission (what was recorded keeps its buffer), freed when the submission is done
        if (n > RING_XMAX) { RED_ONCE(1, "one copy of %llu MB is larger than a ring buffer: dropped", (unsigned long long)(n >> 20)); return VK_WHOLE_SIZE; }
        VkDeviceSize xs = C->ring0_size > RING_MIN ? C->ring0_size : 16u << 20; while (xs < n) xs *= 2; if (xs > RING_XMAX) xs = RING_XMAX;
        C->extra = realloc(C->extra, (C->nextra + 1) * sizeof *C->extra);
        C->extra[C->nextra].b = buf_new(xs, ALL_BUF_USAGE, &C->extra[C->nextra].m, (void **)&C->ringp); C->extra[C->nextra].size = xs; C->ring = C->extra[C->nextra++].b;
        atomic_fetch_add(&g_extra_ring_bytes, xs); atomic_fetch_add(&g_xring_n, 1); C->ring_size = xs;
        C->used = 0; o = 0; g_wgen++; g_nwlog = 0;   // copies cached in the old ring are not reused from the new one
    }
    C->total += o + n - C->used; C->used = o + n; return o;
}
static VkDeviceSize ring_put(const void *src, VkDeviceSize n, VkDeviceSize align) {
    VkDeviceSize o = ring_alloc(n, align);
    if (o != VK_WHOLE_SIZE) memcpy(C->ringp + o, src, n);
    return o;
}
static void post_add(post_t p) { if (C->npost == C->cpost) { C->cpost = C->cpost ? 2 * C->cpost : 256; C->post = realloc(C->post, C->cpost * sizeof *C->post); } C->post[C->npost++] = p; }
static uint64_t gpu_ticks(void) { uint64_t g = guest_ns(); if (!g) g = now_ns(); return g * 12 / 625; }   // the GPU's 19.2 MHz timer, as nvn_mtl.m reports it
// ---------- What the GPU writes: storage buffers of compute dispatches ----------
// Draws and dispatches read guest memory through copies made when they are recorded, so what a dispatch writes into its storage buffers (particles simulated on the GPU,
// the exposure an auto-exposure pass computes) lives only in the ring until its submission completes and the copy goes back to guest memory (post kind 3). Until then
// later work recorded from the same range is bound to that ring range itself instead of a fresh copy of the guest's stale bytes (gw_find); the entry dies with the ring
// (ctx_complete, when the bytes are back in guest memory)
typedef struct { uint64_t addr, size; VkBuffer buf; VkDeviceSize off; uint8_t *p; uint64_t fence; int ci;   // ci: the context whose ring holds it
                 const uint8_t *snap; int exact; uint64_t chk; int dirty; } gw_t;
// snap: the guest's bytes the dispatch was recorded with (its post kind 5 owns them; cleared here before they are freed), to tell which words the CPU rewrote since (gw_cpu_dirty);
// exact: an exact write (a texture-to-buffer or buffer copy writes every byte of its range), not a dispatch's whole bound storage buffer; chk / dirty: gw_cpu_dirty's answer for a submission
enum { NGW = 512 };
static gw_t g_gw[NGW]; static int g_ngw; static pthread_mutex_t g_gwmu = PTHREAD_MUTEX_INITIALIZER;
static void xr_raise(VkBuffer b, uint64_t until);   // an extra ring a later submission reads stays until it is done (see g_xr)
// Which megabytes of guest memory any entry covers (a count per megabyte, folded into 64K slots): a read touching none of them has no entry to find, and gw_find / gw_exact / gw_any
// answer it without the lock and the walk over every entry -- every uniform, storage and texel buffer of every draw asks, and the entries in flight grow with the GPU's lag.
// Changed under g_gwmu; read without it on the submit thread, which alone adds entries (a removal seen late only sends the read to the walk)
enum { GWPG = 1 << 16 };
static uint16_t g_gw_pg[GWPG];
static uint64_t g_gw_calls, g_gw_walked, g_gw_quick;   // gw_find calls, entries looked at, calls answered by the megabyte counts (the recording line)
static void gw_pg_mark(uint64_t addr, uint64_t size, int d) {
    if (!size) return;
    uint64_t a = addr >> 20, b = (addr + size - 1) >> 20; if (b - a >= GWPG) b = a + GWPG - 1;
    for (uint64_t m = a; m <= b; m++) __atomic_add_fetch(&g_gw_pg[m & (GWPG - 1)], (uint16_t)d, __ATOMIC_RELAXED);
}
static int gw_pg_any(uint64_t addr, uint64_t size) {
    if (!size) return 0;
    uint64_t a = addr >> 20, b = (addr + size - 1) >> 20; if (b - a >= GWPG) return 1;
    for (uint64_t m = a; m <= b; m++) if (__atomic_load_n(&g_gw_pg[m & (GWPG - 1)], __ATOMIC_RELAXED)) return 1;
    return 0;
}
static void gw_del(int i) { gw_pg_mark(g_gw[i].addr, g_gw[i].size, -1); g_gw[i] = g_gw[--g_ngw]; }   // caller holds g_gwmu
static void gw_add(uint64_t addr, uint64_t size, VkBuffer buf, VkDeviceSize off, uint8_t *p, uint64_t fence, int ci, const uint8_t *snap, int exact) {
    pthread_mutex_lock(&g_gwmu);
    for (int i = 0; i < g_ngw; i++) if (g_gw[i].addr == addr && g_gw[i].size == size) { gw_del(i); break; }   // the newer write of one range replaces the older
    if (g_ngw == NGW) { RED_ONCE(62, "more than %d GPU-written buffer ranges in flight: the oldest is forgotten", NGW); gw_pg_mark(g_gw[0].addr, g_gw[0].size, -1); memmove(g_gw, g_gw + 1, (NGW - 1) * sizeof *g_gw); g_ngw--; }
    g_gw[g_ngw++] = (gw_t){addr, size, buf, off, p, fence, ci, snap, exact, 0, 0}; gw_pg_mark(addr, size, 1);
    pthread_mutex_unlock(&g_gwmu);
}
// The ring range holding what the GPU wrote at [addr, addr + size), when an entry covers it and the offset in it keeps `align`. An entry of the submission being recorded is
// used as it is. One of an earlier submission still in flight is used too (may_hold): the submissions run in order on the GPU, so this one reads what that one wrote, and that
// one's ring is held from reuse until this one is done (ctx hold). That used to be a wait for the earlier submission -- the GPU drained in the middle of the frame, every frame,
// since Unreal updates its GPU scene and culls with compute and draws from the results in the next command buffer. Without may_hold (indirect arguments the CPU reads) the earlier
// submission is waited for and its bytes read from guest memory, where the completion put them: 0 then, as for no entry. g_gw_ci: the context of the ring of the last hit
int mtl_wait(uint64_t fence, uint64_t timeout_ns);
static int g_gw_ci; static _Atomic uint64_t g_gw_wait_ns, g_gw_waits, g_gw_held, g_gw_copied, g_gw_why[3], g_gw_b2t, g_gw_b2b, g_gw_gpu_reads, g_gw_cpu_fix, g_gw_b2t_guest;   // b2t / b2b: texture and buffer copies made from GPU-written bytes   // why: 0 indirect draw arguments, 1 range not inside the written one, 2 this submission's, not fitting
static VkDeviceSize ring_alloc(VkDeviceSize n, VkDeviceSize align);
static VkDeviceSize ring_put(const void *src, VkDeviceSize n, VkDeviceSize align);
// Whether the CPU rewrote any of [lo, hi) of a dispatch's range since the dispatch was recorded (the guest's bytes against its snapshot; only the bytes read). On the Switch the GPU
// writes the pool's memory itself, so a later read sees the GPU's bytes where it wrote and the CPU's everywhere else; here the whole bound range was taken from the GPU's ring,
// and bytes the CPU had rewritten there in between (Unreal's per-frame uploads into buffers a compute pass also binds) were read stale. Caller holds g_gwmu
// Cost: a partial read takes the whole range's answer first (one compare a submission): a range the CPU left alone is clean in every part of it. DM_VK_STALE_PATCH=0 leaves
// the comparison out (the GPU's bytes as they are, as before vk19.3). g_t_stale / g_stale_bytes: time and bytes in these compares and patches (the recording line)
static uint64_t g_t_stale, g_stale_bytes, g_t_patch, g_patch_regions, g_npatch;   // g_t_patch: the part in gw_patch; regions: copy regions it recorded
static int stale_patch_on(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_VK_STALE_PATCH"); v = !(e && !strcmp(e, "0")); if (!v) LOG("DM_VK_STALE_PATCH=0: reads of an earlier submission's GPU-written range take its bytes as they are"); } return v; }
static int gw_whole_dirty(gw_t *e) {
    if (e->chk != g_fence + 1) { e->chk = g_fence + 1; e->dirty = memcmp((const void *)(uintptr_t)e->addr, e->snap, e->size) != 0; g_stale_bytes += e->size; }
    return e->dirty;
}
static int gw_cpu_dirty(gw_t *e, uint64_t lo, uint64_t hi) {
    if (!e->snap || hi <= lo || !stale_patch_on()) return 0;
    uint64_t t0 = now_ns(); int r = gw_whole_dirty(e);   // the whole range: once a submission
    if (r && !(lo == e->addr && hi == e->addr + e->size)) { r = memcmp((const void *)(uintptr_t)lo, e->snap + (lo - e->addr), hi - lo) != 0; g_stale_bytes += hi - lo; }
    g_t_stale += now_ns() - t0;
    return r;
}
// The words of [lo, hi) the CPU rewrote since e was recorded, copied from the guest over dst (where lo lands), after the GPU's bytes were copied there (the upload command buffer).
// Compared 8 bytes at a time (64 at a time over equal stretches); only the bytes from the first rewritten word to the last go into the ring
static inline uint64_t ld64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }
static inline uint32_t ld32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static void gw_patch(const gw_t *e, uint64_t lo, uint64_t hi, VkBuffer dst, VkDeviceSize dst_off) {
    if (!e->snap) return;
    uint64_t t0 = now_ns();
    const uint8_t *g = (const uint8_t *)(uintptr_t)lo, *sn = e->snap + (lo - e->addr); uint64_t n = hi - lo;
    static VkBufferCopy *rg; static size_t crg; size_t nr = 0; uint64_t i = 0;   // the runs of rewritten words (exact: the bytes between them may be the GPU's); under g_gwmu
    while (i < n) {
        if (i + 64 <= n && !memcmp(g + i, sn + i, 64)) { i += 64; continue; }
        if (i + 8 <= n && ld64(g + i) == ld64(sn + i)) { i += 8; continue; }
        if (i + 4 <= n ? ld32(g + i) == ld32(sn + i) : !memcmp(g + i, sn + i, n - i)) { i += i + 4 <= n ? 4 : n - i; continue; }
        uint64_t a = i;
        while (i < n) { if (i + 4 <= n) { if (ld32(g + i) == ld32(sn + i)) break; i += 4; continue; } if (!memcmp(g + i, sn + i, n - i)) break; i = n; }   // word by word: an unchanged word may hold the GPU's bytes
        if (nr == crg) { size_t c2 = crg ? 2 * crg : 256; VkBufferCopy *r2 = realloc(rg, c2 * sizeof *rg); if (!r2) break; rg = r2; crg = c2; }
        rg[nr++] = (VkBufferCopy){a, a, i - a};
    }
    g_stale_bytes += n; g_patch_regions += nr; g_npatch++;
    if (nr) {
        uint64_t first = rg[0].srcOffset, last = rg[nr - 1].srcOffset + rg[nr - 1].size;
        VkDeviceSize st = ring_put(g + first, last - first, 16);
        if (st != VK_WHOLE_SIZE) {
            VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT};
            vkCmdPipelineBarrier(C->up, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, NULL, 0, NULL);   // after the GPU's bytes landed
            for (size_t k = 0; k < nr; k++) { rg[k].dstOffset = dst_off + rg[k].srcOffset; rg[k].srcOffset = st + (rg[k].srcOffset - first); }
            for (size_t k = 0; k < nr; k += 4096) vkCmdCopyBuffer(C->up, C->ring, dst, (uint32_t)(nr - k < 4096 ? nr - k : 4096), rg + k);
            C->up_used = 1;
        }
    }
    g_t_stale += now_ns() - t0; g_t_patch += now_ns() - t0;
}
// A read over [addr, addr + size) that earlier submissions still running wrote parts of (Unreal binds its whole GPU-scene buffer; a dispatch updated a few records of it): the guest's
// bytes copied into the ring, then each written part copied over them from its submission's ring at the start of this submission, oldest first (the upload command buffer; those
// submissions are done by then). Each source ring is held until this submission is done. Parts this submission itself writes are not in it (no such case met). Caller holds g_gwmu
static void pass_end(void); static int pass_begin(void); static int g_pass_open_(void);
static VkDeviceSize gw_compose(uint64_t addr, uint64_t size, VkDeviceSize align) {
    VkDeviceSize ro = ring_put((const void *)(uintptr_t)addr, size, align); if (ro == VK_WHOLE_SIZE) return ro;
    int cur = 0;
    for (int i = 0; i < g_ngw; i++) {   // earlier submissions' parts, oldest first: copied at the start of this one (they are complete by then)
        gw_t *e = &g_gw[i]; if (addr >= e->addr + e->size || e->addr >= addr + size) continue;
        if (e->fence == g_fence + 1) { cur = 1; continue; }
        uint64_t lo = e->addr > addr ? e->addr : addr, hi = e->addr + e->size < addr + size ? e->addr + e->size : addr + size;
        VkBuffer rb = C->ring; VkBufferCopy bc = {e->off + (lo - e->addr), ro + (lo - addr), hi - lo}; vkCmdCopyBuffer(C->up, e->buf, rb, 1, &bc); C->up_used = 1;
        if (gw_cpu_dirty(e, lo, hi)) { gw_patch(e, lo, hi, rb, ro + (lo - addr)); atomic_fetch_add(&g_gw_cpu_fix, 1); }   // the CPU's rewrites since, over the GPU's bytes
        ctx_t *o = &g_ctx[e->ci]; if (o->hold < g_fence + 1) o->hold = g_fence + 1; xr_raise(e->buf, g_fence + 1);
    }
    if (cur) {   // parts this submission's own dispatches wrote: copied here, in order, after them (outside a render pass: the open one is ended and begun again)
        int was_open = g_pass_open_(); if (was_open) pass_end();
        for (int i = 0; i < g_ngw; i++) {
            const gw_t *e = &g_gw[i]; if (e->fence != g_fence + 1 || addr >= e->addr + e->size || e->addr >= addr + size) continue;
            uint64_t lo = e->addr > addr ? e->addr : addr, hi = e->addr + e->size < addr + size ? e->addr + e->size : addr + size;
            VkBufferCopy bc = {e->off + (lo - e->addr), ro + (lo - addr), hi - lo}; vkCmdCopyBuffer(C->cb, e->buf, C->ring, 1, &bc);
        }
        { VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT};
          vkCmdPipelineBarrier(C->cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, NULL, 0, NULL); }   // the copies, for what reads the result
        if (was_open) pass_begin();
    }
    return ro;
}
static int gw_find_(uint64_t addr, uint64_t size, VkDeviceSize align, VkBuffer *buf, VkDeviceSize *off, uint8_t **p, int may_hold) {
    int hit = 0; uint64_t wait = 0; int why = 0;
    if (!__atomic_load_n(&g_ngw, __ATOMIC_RELAXED)) return 0;
    g_gw_calls++; if (!gw_pg_any(addr, size)) { g_gw_quick++; return 0; }
    pthread_mutex_lock(&g_gwmu); g_gw_walked += (uint64_t)g_ngw;
    for (int i = g_ngw - 1; i >= 0 && !hit && !wait; i--) {
        gw_t *e = &g_gw[i];
        if (addr >= e->addr + e->size || addr + size <= e->addr) continue;
        int inside = addr >= e->addr && addr + size <= e->addr + e->size, fits = inside && (e->off + (addr - e->addr)) % align == 0;
        if (e->fence != g_fence + 1) {   // an earlier submission's, still running
            if (may_hold && !inside) {   // the read spans more than was written: composed in this submission's ring (gw_compose)
                VkDeviceSize ro = gw_compose(addr, size, align < 16 ? 16 : align);
                if (ro == VK_WHOLE_SIZE) { wait = e->fence; why = 1; break; }
                *buf = C->ring; *off = ro; if (p) *p = C->ringp + ro; g_gw_ci = g_ci; hit = 1; atomic_fetch_add(&g_gw_copied, 1); break;
            }
            if (!may_hold) { wait = e->fence; why = 0; break; }
            ctx_t *o = &g_ctx[e->ci]; if (o->hold < g_fence + 1) o->hold = g_fence + 1; xr_raise(e->buf, g_fence + 1);
            if (inside && gw_cpu_dirty(e, addr, addr + size)) {   // the CPU rewrote words of it since: the GPU's bytes and then the CPU's rewrites over them, in this submission's ring
                VkDeviceSize ro = ring_alloc(size, align < 16 ? 16 : align);
                if (ro == VK_WHOLE_SIZE) { wait = e->fence; why = 1; break; }
                VkBuffer rb = C->ring; uint8_t *rp = C->ringp + ro;
                VkBufferCopy bc = {e->off + (addr - e->addr), ro, size}; vkCmdCopyBuffer(C->up, e->buf, rb, 1, &bc); C->up_used = 1;
                gw_patch(e, addr, addr + size, rb, ro);
                *buf = rb; *off = ro; if (p) *p = rp; g_gw_ci = g_ci; hit = 1; atomic_fetch_add(&g_gw_cpu_fix, 1); break;
            }
            atomic_fetch_add(&g_gw_gpu_reads, 1);
            if (!fits) {   // at an offset the binding cannot take: copied into this submission's ring at the start of the submission (the upload command buffer), when it is complete
                VkDeviceSize ro = ring_alloc(size, align < 16 ? 16 : align);
                if (ro == VK_WHOLE_SIZE) { wait = e->fence; why = 1; break; }
                VkBufferCopy bc = {e->off + (addr - e->addr), ro, size}; vkCmdCopyBuffer(C->up, e->buf, C->ring, 1, &bc); C->up_used = 1;
                *buf = C->ring; *off = ro; if (p) *p = C->ringp + ro; g_gw_ci = g_ci; hit = 1; atomic_fetch_add(&g_gw_copied, 1); break;
            }
            atomic_fetch_add(&g_gw_held, 1);
        } else if (!fits) {   // this submission's own write, not covering the read (or at an offset the binding cannot take): composed (gw_compose)
            if (may_hold) { VkDeviceSize ro = gw_compose(addr, size, align < 16 ? 16 : align);
                            if (ro != VK_WHOLE_SIZE) { *buf = C->ring; *off = ro; if (p) *p = C->ringp + ro; g_gw_ci = g_ci; hit = 1; atomic_fetch_add(&g_gw_copied, 1); break; } }
            why = 2; atomic_fetch_add(&g_gw_why[2], 1); RED_ONCE(63, "a read overlaps a GPU-written buffer range of this submission without fitting in it: read from guest memory, which is stale until the write completes"); break; }
        *buf = e->buf; *off = e->off + (addr - e->addr); if (p) *p = e->p + (addr - e->addr); g_gw_ci = e->ci; hit = 1;
    }
    pthread_mutex_unlock(&g_gwmu);
    if (wait) { uint64_t t0 = now_ns(); mtl_wait(wait, UINT64_MAX); atomic_fetch_add(&g_gw_wait_ns, now_ns() - t0); atomic_fetch_add(&g_gw_wait_cum, now_ns() - t0); atomic_fetch_add(&g_gw_waits, 1); atomic_fetch_add(&g_gw_why[why], 1); }
    return hit;
}
static int gw_find(uint64_t addr, uint64_t size, VkDeviceSize align, VkBuffer *buf, VkDeviceSize *off, uint8_t **p) { return gw_find_(addr, size, align, buf, off, p, 1); }
// Whether every GPU-written entry over [addr, addr + size) is an exact write, and there is one: a buffer-to-texture copy takes its source from such ranges only; over a dispatch's
// bound range it reads the guest's bytes (Unreal's staging written by the CPU), as on the Switch, where a copy reads the pool's memory
static int gw_exact(uint64_t addr, uint64_t size) {
    if (!__atomic_load_n(&g_ngw, __ATOMIC_RELAXED) || !gw_pg_any(addr, size)) return 0;
    int any = 0, ok = 1; pthread_mutex_lock(&g_gwmu);
    for (int i = 0; i < g_ngw; i++) { const gw_t *e = &g_gw[i]; if (addr >= e->addr + e->size || addr + size <= e->addr) continue; any = 1; if (!e->exact) ok = 0; }
    pthread_mutex_unlock(&g_gwmu);
    if (any && !ok) atomic_fetch_add(&g_gw_b2t_guest, 1);
    return any && ok;
}
static void gw_expire(uint64_t fence) {   // completion: the submission's writes are in guest memory now, and its rings go
    pthread_mutex_lock(&g_gwmu);
    for (int i = 0; i < g_ngw;) if (g_gw[i].fence <= fence) gw_del(i); else i++;
    pthread_mutex_unlock(&g_gwmu);
}
// Extra rings kept past their own submission for later ones that read them (under g_gwmu). A later submission can find a range in one after its context completed --
// a dispatch registers the range it read from there as its own write, still in that ring, and a third submission reads it from that entry -- so `until` follows every
// such reader (xr_raise, where gw_find holds a context for a reader), and a ring any live GPU-written entry still points into is not freed either. The release fence
// used to be taken once, when the ring's own context completed: a later reader's write-back then copied from freed memory (FINAL FANTASY RESONANCE, a Limit Burst:
// SIGSEGV in the completion thread's memcpy, its source in the freed ring)
static struct { VkBuffer b; VkDeviceMemory m; uint64_t until; VkDeviceSize size; } *g_xr; static int g_nxr, g_cxr;
static void xr_raise(VkBuffer b, uint64_t until) { for (int i = 0; i < g_nxr; i++) if (g_xr[i].b == b && g_xr[i].until < until) g_xr[i].until = until; }   // caller holds g_gwmu
static void xr_free(uint64_t done) {
    pthread_mutex_lock(&g_gwmu);
    for (int i = 0; i < g_nxr;) {
        int used = g_xr[i].until > done; for (int k = 0; k < g_ngw && !used; k++) used = g_gw[k].buf == g_xr[i].b;
        if (used) { i++; continue; }
        vkDestroyBuffer(g_dev, g_xr[i].b, NULL); vkFreeMemory(g_dev, g_xr[i].m, NULL); atomic_fetch_sub(&g_extra_ring_bytes, g_xr[i].size); g_xr[i] = g_xr[--g_nxr];
    }
    pthread_mutex_unlock(&g_gwmu);
}
// GPU time per region signature: what each kind of pass or dispatch costs a frame (registered by the submit thread with its description, timed by the completion thread)
enum { NGSIG = 1024 };
static struct { uint64_t sig; uint64_t ns, n, draws; char d[120]; } g_gsig[NGSIG]; static pthread_mutex_t g_gsigmu = PTHREAD_MUTEX_INITIALIZER;
static int gsig_slot(uint64_t sig) { uint32_t i = (uint32_t)(sig ^ sig >> 29) % NGSIG; for (int k = 0; k < NGSIG; k++, i = (i + 1) % NGSIG) if (g_gsig[i].sig == sig || !g_gsig[i].sig) return (int)i; return -1; }   // caller holds g_gsigmu
static void gsig_name(uint64_t sig, const char *d) {   // the submit thread: a description for a signature met the first time
    if (!sig) return; pthread_mutex_lock(&g_gsigmu); int i = gsig_slot(sig);
    if (i >= 0 && !g_gsig[i].sig) { g_gsig[i].sig = sig; snprintf(g_gsig[i].d, sizeof g_gsig[i].d, "%s", d); }
    pthread_mutex_unlock(&g_gsigmu);
}
static void gsig_add(uint64_t sig, uint64_t ns, uint32_t draws) {
    if (!sig) return; pthread_mutex_lock(&g_gsigmu); int i = gsig_slot(sig);
    if (i >= 0 && g_gsig[i].sig == sig) { g_gsig[i].ns += ns; g_gsig[i].n++; g_gsig[i].draws += draws; }
    pthread_mutex_unlock(&g_gsigmu);
}
static void gsig_report(uint64_t frames) {   // the 10 costliest passes and dispatches over the last frames, then the sums start again
    static int idx[NGSIG]; pthread_mutex_lock(&g_gsigmu);
    for (int kind = 0; kind < 2; kind++) {
        int n = 0; uint64_t tot = 0;
        for (int i = 0; i < NGSIG; i++) if (g_gsig[i].sig && g_gsig[i].n && (int)(g_gsig[i].sig & 1) == kind) { idx[n++] = i; tot += g_gsig[i].ns; }
        for (int a = 0; a < n; a++) for (int b = a + 1; b < n; b++) if (g_gsig[idx[b]].ns > g_gsig[idx[a]].ns) { int t = idx[a]; idx[a] = idx[b]; idx[b] = t; }
        LOG("vk: costliest %s over the last %llu frames (%d kinds, %.2f ms a frame in all):", kind ? "dispatches" : "render passes", (unsigned long long)frames, n, tot / 1e6 / frames);
        for (int a = 0; a < n && a < 10; a++) { const typeof(g_gsig[0]) *e = &g_gsig[idx[a]];
            LOG("vk:   %.2f ms a frame, %.1f a frame, %.0f draws each: %s", e->ns / 1e6 / frames, (double)e->n / frames, e->n ? (double)e->draws / e->n : 0.0, e->d); }
    }
    for (int i = 0; i < NGSIG; i++) g_gsig[i].ns = g_gsig[i].n = g_gsig[i].draws = 0;
    pthread_mutex_unlock(&g_gsigmu);
}
static _Atomic uint64_t g_gpu_kind_ns[3];   // GPU time inside render passes, dispatches, copies (gpu_mark); the rest of a submission's span is time between them
static void ctx_complete(ctx_t *c) {
    uint64_t ts[2];
    if (vkGetQueryPoolResults(g_dev, c->qp, 0, 2, sizeof ts, ts, 8, VK_QUERY_RESULT_64_BIT) == VK_SUCCESS && ts[1] > ts[0])
        { uint64_t gns = (uint64_t)((double)(ts[1] - ts[0]) * g_props.limits.timestampPeriod); atomic_fetch_add(&g_gpu_ns, gns); atomic_fetch_add(&g_gpu_cum, gns); }
    if (c->ntq >= 2) { static uint64_t t[NTQ]; uint32_t n = c->ntq & ~1u;
        if (vkGetQueryPoolResults(g_dev, c->tq, 0, n, n * 8, t, 8, VK_QUERY_RESULT_64_BIT) == VK_SUCCESS)
            for (uint32_t i = 0; i + 1 < n; i += 2) if (t[i + 1] > t[i]) { uint64_t ns = (uint64_t)((double)(t[i + 1] - t[i]) * g_props.limits.timestampPeriod);
                atomic_fetch_add(&g_gpu_kind_ns[c->tkind[i / 2] % 3], ns); gsig_add(c->tsig[i / 2], ns, c->tdraws[i / 2]); } }
    for (int i = 0; i < c->npost; i++) {
        post_t *p = &c->post[i];
        if (p->kind == 1) { volatile uint32_t *v = p->dst; if (p->mode == 1) *v += p->v; else *v = p->v; }
        else if (p->kind == 2) { uint64_t t = gpu_ticks(), rec[2] = {p->type == 0 || p->type == 0x10 ? t : 0, t}; memcpy((void *)p->addr, rec, sizeof rec); }
        else if (p->kind == 4) {   // a segment of counted draws: added to the queue's samples-passed counter (submission order: one completion thread), written out when addr is a report
            static uint64_t total;
            if (p->n == SIZE_MAX) total += 1u << 20;   // more draws than queries: reported visible
            else for (size_t q = 0; q < p->n; q++) { uint64_t v = 0; if (vkGetQueryPoolResults(g_dev, c->oq, ((const uint32_t *)p->src)[q], 1, 8, &v, 8, VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) total += v; }
            if (p->mode) total = 0;   // the segment ended in a ResetCounter
            if (p->addr) { uint64_t rec[2] = {total, gpu_ticks()}; memcpy((void *)p->addr, rec, sizeof rec); }
            free((void *)p->src);
        }
        else if (p->kind == 5) {   // a dispatch's storage buffer: the words the GPU changed (see dispatch)
            const uint8_t *snap = (const uint8_t *)(uintptr_t)p->addr, *now = p->src; uint8_t *dst = p->dst; size_t i = 0, nw = 0;
            for (; i + 4 <= p->n; i += 4) if (memcmp(now + i, snap + i, 4)) { memcpy(dst + i, now + i, 4); nw++; }
            for (; i < p->n; i++) if (now[i] != snap[i]) dst[i] = now[i];
            pthread_mutex_lock(&g_gwmu); for (int k = 0; k < g_ngw; k++) if (g_gw[k].snap >= snap && g_gw[k].snap < snap + p->n) g_gw[k].snap = NULL; pthread_mutex_unlock(&g_gwmu);   // no reader compares against it any more
            free((void *)snap); g_wb_words += nw; g_wb_bytes += p->n;
        }
        else if (p->kind == 6) { for (uint32_t r = 0; r < p->v; r++) memcpy((uint8_t *)p->dst + (size_t)r * p->mode, p->src + (size_t)r * p->mode, p->type); }   // rows: v rows of type bytes, mode apart
        else if (p->kind == 3) { memcpy(p->dst, p->src, p->n);
            if (c->plog && p->n <= 64) { float f[16]; memcpy(f, p->src, p->n); LOG("plog: written back %#llx +%zu: %g %g %g %g%s", (unsigned long long)(uintptr_t)p->dst, p->n, f[0], p->n > 4 ? f[1] : 0, p->n > 8 ? f[2] : 0, p->n > 12 ? f[3] : 0, p->n > 16 ? " ..." : ""); } }
    }
    c->npost = 0;
    gw_expire(c->fence_val);
    pthread_mutex_lock(&g_gwmu); uint64_t hold = c->hold;
    for (int i = 0; i < c->nextra; i++) {
        int used = hold > c->fence_val; for (int k = 0; k < g_ngw && !used; k++) used = g_gw[k].buf == c->extra[i].b;
        if (used) {   // a later submission reads it (gw_find), or a live entry points into it: freed once nothing does (xr_free)
            if (g_nxr == g_cxr) { g_cxr = g_cxr ? 2 * g_cxr : 16; g_xr = realloc(g_xr, g_cxr * sizeof *g_xr); }
            g_xr[g_nxr++] = (typeof(g_xr[0])){c->extra[i].b, c->extra[i].m, hold > c->fence_val ? hold : c->fence_val + 1, c->extra[i].size}; continue; }
        vkDestroyBuffer(g_dev, c->extra[i].b, NULL); vkFreeMemory(g_dev, c->extra[i].m, NULL); atomic_fetch_sub(&g_extra_ring_bytes, c->extra[i].size); }
    pthread_mutex_unlock(&g_gwmu);
    c->nextra = 0;
    for (int i = 0; i < c->nfb; i++) vkDestroyFramebuffer(g_dev, c->fb[i], NULL);
    c->nfb = 0;
    for (int i = 0; i < c->nbv; i++) vkDestroyBufferView(g_dev, c->bv[i], NULL);
    c->nbv = 0;
    // The pools are idle now and the submit thread does not touch this context until g_done passes it (set after this returns): reset them here, off its time
    VKCHECK(vkResetCommandPool(g_dev, c->pool, 0));
    for (int i = 0; i < c->ndp; i++) VKCHECK(vkResetDescriptorPool(g_dev, c->dps[i], 0));
    c->reset_done = 1;
}
// Lost device (a GPU hang the driver's watchdog ended): the work of every submission not known complete, so the log names the shaders and sizes that were running
static void trace_dump(const char *why) {
    LOG("[RED] vk: %s: the submissions in flight and what they held (newest last):", why);
    for (int k = 0; k < NCTX; k++) {
        ctx_t *c = &g_ctx[k]; if (c->fence_val <= g_done && c != C) continue;
        LOG("vk:  submission %llu%s: %u draws, %u dispatches, largest dispatch %u groups (%s %ux%ux%u)", (unsigned long long)c->fence_val, c == C ? " (being recorded)" : "", c->ndraw, c->ndisp,
            c->max_groups, c->heavy.a, c->heavy.n0, c->heavy.n1, c->heavy.n2);
        uint32_t n = c->ntr < TRN ? c->ntr : TRN;
        for (uint32_t i = 0; i < n; i++) { const struct trace *t = &c->tr[(c->ntr - n + i) % TRN];
            if (t->kind == 'd') LOG("vk:   draw vs %s fs %s count %u x%u%s%s", t->a, t->b[0] ? t->b : "-", t->n0, t->n1, t->gpu_ind ? " (indirect, GPU args)" : "", t->zero_tb ? " zero texel buffers" : "");
            else LOG("vk:   dispatch cs %s %ux%ux%u%s%s", t->a, t->n0, t->n1, t->n2, t->gpu_ind ? " (indirect, GPU args)" : "", t->zero_tb ? " zero texel buffers" : ""); }
    }
}
static void trace_add(char kind, const char *a, const char *b, uint32_t n0, uint32_t n1, uint32_t n2, int gpu_ind, uint32_t zero_tb) {
    struct trace *t = &C->tr[C->ntr++ % TRN]; t->kind = kind; t->gpu_ind = (char)gpu_ind; t->zero_tb = zero_tb; t->n0 = n0; t->n1 = n1; t->n2 = n2;
    snprintf(t->a, sizeof t->a, "%s", a ? a : "-"); snprintf(t->b, sizeof t->b, "%s", b ? b : "");
    if (kind == 'd') C->ndraw++; else { C->ndisp++; uint64_t g = (uint64_t)n0 * n1 * n2; if (g > C->max_groups) { C->max_groups = g > UINT32_MAX ? UINT32_MAX : (uint32_t)g; C->heavy = *t; } }
}
static void garbage_free(uint64_t done);
static void *completion_thread(void *a) {
    (void)a;
    for (;;) {
        pthread_mutex_lock(&g_mu);
        while (!g_npend) pthread_cond_wait(&g_cv, &g_mu);
        ctx_t *c = &g_ctx[g_pend[0]];
        pthread_mutex_unlock(&g_mu);
        VkResult r = vkWaitForFences(g_dev, 1, &c->fence, VK_TRUE, UINT64_MAX);
        if (r != VK_SUCCESS) { trace_dump("waiting for a submission failed"); die("Vulkan: waiting for a submission failed (%d)", (int)r); }
        ctx_complete(c);
        pthread_mutex_lock(&g_mu);
        g_done = c->fence_val; memmove(g_pend, g_pend + 1, --g_npend * sizeof *g_pend);
        pthread_cond_broadcast(&g_cv); pthread_mutex_unlock(&g_mu);
        garbage_free(c->fence_val); xr_free(c->fence_val);
    }
    return NULL;
}

// ---------- Device ----------
static int g_has_u8;
static VkPhysicalDeviceFeatures g_feat;   // what the GPU has (every one of them is enabled at device creation): dualSrcBlend, wideLines, textureCompressionBC
static VkSampler g_dummy_samp; static VkImage g_dummy_img; static VkImageView g_dummy_view; static VkBuffer g_zero_buf, g_support_buf; static VkDeviceMemory g_zero_mem, g_support_mem;
// Stand-ins for a binding nothing real is bound to: a texel buffer of zeros (uniform), one for shader writes (storage, a scratch buffer of its own so the zeros stay zero), a storage image
static VkBufferView g_dummy_utb, g_dummy_stb; static VkBuffer g_scratch_buf; static VkDeviceMemory g_scratch_mem; static VkImage g_dummy_simg; static VkImageView g_dummy_simg_view;
static void dummies(void);
static int g_has_surface;
// Render passes: dynamic rendering where the GPU has it (Vulkan 1.3, or 1.2 / 1.1 with VK_KHR_dynamic_rendering), else classic render passes and framebuffers made per pass
// Every submission, the game's and the presents, waits on GPU for the one before and signals the next value of one timeline semaphore (g_tlv, under g_qmu). Submission order alone with a
// barrier at each start should do; the Mali-G57 (Unisoc T820) did not keep it for a present's blit, whose batch also waits on the swapchain acquire: two thirds of Metroid Dread's menu
// frames showed black. GPUs without timeline semaphores (Vulkan 1.1 without 1.2) wait for the GPU on the CPU before each present instead
static VkSemaphore g_tl; static uint64_t g_tlv; static int g_after_present = 1;   // g_after_present: the next game submission waits on the timeline (mtl_sub_commit)
// Pipelines compiled on earlier runs: the driver's pipeline cache, kept in <DM_VK_DIR>/../pipeline_cache.bin (written every 1800 frames when pipelines were added). A Mali-G57
// compiles one in about 40 ms, and a scene meeting a dozen new effects stalled its frame for 150-240 ms (Super Mario Odyssey); data from another GPU or driver is ignored by it
static VkPipelineCache g_pcache; static char g_pcache_path[1024]; static int g_pcache_saved;
static void pcache_load(void) {
    const char *d = getenv("DM_VK_DIR"); size_t n = 0; void *data = NULL;
    if (d) { snprintf(g_pcache_path, sizeof g_pcache_path, "%s/../pipeline_cache.bin", d); FILE *f = fopen(g_pcache_path, "rb");
             if (f) { fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET); if (sz > 0 && (data = malloc((size_t)sz)) && fread(data, 1, (size_t)sz, f) == (size_t)sz) n = (size_t)sz; fclose(f); } }
    VkPipelineCacheCreateInfo ci = {VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO, NULL, 0, n, data};
    if (vkCreatePipelineCache(g_dev, &ci, NULL, &g_pcache) != VK_SUCCESS) { ci.initialDataSize = 0; ci.pInitialData = NULL; if (vkCreatePipelineCache(g_dev, &ci, NULL, &g_pcache) != VK_SUCCESS) g_pcache = VK_NULL_HANDLE; }
    if (n) LOG("vk: pipeline cache of %zu KB loaded from an earlier run", n >> 10);
    free(data);
}
static int g_npso;
static void pcache_save(void) {
    if (!g_pcache || !g_pcache_path[0] || g_npso == g_pcache_saved) return;
    size_t n = 0; if (vkGetPipelineCacheData(g_dev, g_pcache, &n, NULL) != VK_SUCCESS || !n) return;
    void *data = malloc(n); if (!data) return;
    if (vkGetPipelineCacheData(g_dev, g_pcache, &n, data) == VK_SUCCESS) {
        char tmp[1100]; snprintf(tmp, sizeof tmp, "%s.tmp", g_pcache_path); FILE *f = fopen(tmp, "wb");
        if (f) { int ok = fwrite(data, 1, n, f) == n; ok &= fclose(f) == 0; if (ok) rename(tmp, g_pcache_path); }
        g_pcache_saved = g_npso;
    }
    free(data);
}
static int g_dynrend; static PFN_vkCmdBeginRenderingKHR p_begin_rendering; static PFN_vkCmdEndRenderingKHR p_end_rendering;
static VkDescriptorPool dpool_new(void);
static void vk_init_(void) {
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "nxnative", 1, "dmhost", 1, VK_API_VERSION_1_3};
    const char *iext[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_ANDROID_SURFACE_EXTENSION_NAME};
    uint32_t nie = 0; vkEnumerateInstanceExtensionProperties(NULL, &nie, NULL);
    VkExtensionProperties *iep = calloc(nie ? nie : 1, sizeof *iep); vkEnumerateInstanceExtensionProperties(NULL, &nie, iep);
    int nsurf = 0; for (uint32_t i = 0; i < nie; i++) for (int k = 0; k < 2; k++) nsurf += !strcmp(iep[i].extensionName, iext[k]);
    free(iep); g_has_surface = nsurf == 2;
    VkInstanceCreateInfo ii = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app, 0, NULL, g_has_surface ? 2 : 0, iext};
    VKCHECK(vkCreateInstance(&ii, NULL, &g_inst));
    uint32_t n = 1; VkResult r = vkEnumeratePhysicalDevices(g_inst, &n, &g_pd);
    if ((r != VK_SUCCESS && r != VK_INCOMPLETE) || !n) die("Vulkan: no GPU");
    vkGetPhysicalDeviceProperties(g_pd, &g_props); vkGetPhysicalDeviceMemoryProperties(g_pd, &g_memp);
    uint32_t nq = 0; vkGetPhysicalDeviceQueueFamilyProperties(g_pd, &nq, NULL);
    VkQueueFamilyProperties qp[16]; if (nq > 16) nq = 16; vkGetPhysicalDeviceQueueFamilyProperties(g_pd, &nq, qp);
    for (g_qf = 0; g_qf < nq && !(qp[g_qf].queueFlags & VK_QUEUE_GRAPHICS_BIT); g_qf++) {}
    if (g_qf == nq) die("Vulkan: no graphics queue");
    // Every feature the GPU has is enabled (the translated shaders may use any of them), except the costly robustness beyond robustBufferAccess. The feature structures
    // chained are only those of the GPU's Vulkan version and extensions (1.1 phones have neither the 1.2 / 1.3 structures nor dynamic rendering)
    uint32_t dv = g_props.apiVersion;
    uint32_t ne = 0; vkEnumerateDeviceExtensionProperties(g_pd, NULL, &ne, NULL);
    VkExtensionProperties *ep = calloc(ne ? ne : 1, sizeof *ep); vkEnumerateDeviceExtensionProperties(g_pd, NULL, &ne, ep);
    int has_sc = 0, has_u8 = 0, has_dr = 0;
    for (uint32_t i = 0; i < ne; i++) {
        has_sc |= !strcmp(ep[i].extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME); has_u8 |= !strcmp(ep[i].extensionName, VK_EXT_INDEX_TYPE_UINT8_EXTENSION_NAME);
        has_dr |= !strcmp(ep[i].extensionName, VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME);
    }
    {   // Reading an attachment inside its own pass (a held pass's depth sampled by a contact-shadow draw, now a split: vk19.4) needs one of these; logged to see which this GPU has
        static const char *const fl[] = {"VK_EXT_attachment_feedback_loop_layout", "VK_EXT_attachment_feedback_loop_dynamic_state", "VK_KHR_dynamic_rendering_local_read",
                                         "VK_EXT_rasterization_order_attachment_access", "VK_ARM_rasterization_order_attachment_access"};
        char b[512]; int o = 0;
        for (size_t k = 0; k < sizeof fl / sizeof *fl; k++) { uint32_t sv = 0; for (uint32_t i = 0; i < ne; i++) if (!strcmp(ep[i].extensionName, fl[k])) sv = ep[i].specVersion ? ep[i].specVersion : 1;
            o += snprintf(b + o, sizeof b - o, "%s%s %s", k ? ", " : "", fl[k], sv ? "yes" : "no"); }
        LOG("vk: in-pass attachment reads: %s (Vulkan %u.%u, driver %#x)", b, VK_API_VERSION_MAJOR(dv), VK_API_VERSION_MINOR(dv), g_props.driverVersion);
    }
    free(ep);
    VkPhysicalDeviceIndexTypeUint8FeaturesEXT u8 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INDEX_TYPE_UINT8_FEATURES_EXT};
    VkPhysicalDeviceDynamicRenderingFeaturesKHR dr = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES_KHR};
    VkPhysicalDeviceVulkan13Features f13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceVulkan12Features f12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceVulkan11Features f11 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    VkPhysicalDeviceShaderDrawParametersFeatures sdp = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES};
    void *chain = NULL;
    if (has_u8) { u8.pNext = chain; chain = &u8; }
    if (dv >= VK_API_VERSION_1_3) { f13.pNext = chain; chain = &f13; } else if (has_dr) { dr.pNext = chain; chain = &dr; }
    if (dv >= VK_API_VERSION_1_2) { f12.pNext = chain; chain = &f12; f11.pNext = chain; chain = &f11; } else { sdp.pNext = chain; chain = &sdp; }
    VkPhysicalDeviceFeatures2 f2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, chain};
    vkGetPhysicalDeviceFeatures2(g_pd, &f2);
    f13.robustImageAccess = VK_FALSE;
    g_feat = f2.features;
    const char *ext[4]; uint32_t next = 0;
    if (has_sc && g_has_surface) ext[next++] = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    if (has_u8 && u8.indexTypeUint8) { ext[next++] = VK_EXT_INDEX_TYPE_UINT8_EXTENSION_NAME; g_has_u8 = 1; }
    g_dynrend = dv >= VK_API_VERSION_1_3 ? f13.dynamicRendering : has_dr && dr.dynamicRendering;
    if (getenv("DM_VK_NO_DYNREND")) g_dynrend = 0;   // diagnostics: the render pass path of GPUs without dynamic rendering, on one that has it
    if (g_dynrend && dv < VK_API_VERSION_1_3) ext[next++] = VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME;
    float prio = 1;
    VkDeviceQueueCreateInfo qi = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, g_qf, 1, &prio};
    VkDeviceCreateInfo di = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &f2, 0, 1, &qi, 0, NULL, next, ext, NULL};
    VKCHECK(vkCreateDevice(g_pd, &di, NULL, &g_dev));
    vkGetDeviceQueue(g_dev, g_qf, 0, &g_q);
    pcache_load();
    if (dv >= VK_API_VERSION_1_2 && f12.timelineSemaphore && !getenv("DM_VK_NO_TIMELINE")) {
        VkSemaphoreTypeCreateInfo ti = {VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO, NULL, VK_SEMAPHORE_TYPE_TIMELINE, 0};
        VkSemaphoreCreateInfo si = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &ti, 0};
        if (vkCreateSemaphore(g_dev, &si, NULL, &g_tl) != VK_SUCCESS) g_tl = VK_NULL_HANDLE;
    }
    if (g_dynrend) {
        p_begin_rendering = (PFN_vkCmdBeginRenderingKHR)vkGetDeviceProcAddr(g_dev, dv >= VK_API_VERSION_1_3 ? "vkCmdBeginRendering" : "vkCmdBeginRenderingKHR");
        p_end_rendering = (PFN_vkCmdEndRenderingKHR)vkGetDeviceProcAddr(g_dev, dv >= VK_API_VERSION_1_3 ? "vkCmdEndRendering" : "vkCmdEndRenderingKHR");
        if (!p_begin_rendering || !p_end_rendering) g_dynrend = 0;
    }
    { const char *e = getenv("DM_VK_CTX"); if (e && *e) { int n = atoi(e); NCTX = n < 4 ? 4 : n > NCTX_MAX ? NCTX_MAX : n; } LOG("vk: %d submission contexts (DM_VK_CTX)", NCTX); }
    for (int i = 0; i < NCTX; i++) {
        ctx_t *c = &g_ctx[i];
        VkCommandPoolCreateInfo pi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, VK_COMMAND_POOL_CREATE_TRANSIENT_BIT, g_qf};   // reset as a whole each submission (mtl_sub_begin)
        VKCHECK(vkCreateCommandPool(g_dev, &pi, NULL, &c->pool));
        VkCommandBuffer cbs[2]; VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, c->pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 2};
        VKCHECK(vkAllocateCommandBuffers(g_dev, &ai, cbs)); c->cb = cbs[0]; c->up = cbs[1];
        VkFenceCreateInfo fi = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, NULL, VK_FENCE_CREATE_SIGNALED_BIT};
        VKCHECK(vkCreateFence(g_dev, &fi, NULL, &c->fence));
        c->ring = c->ring0 = buf_new(RING_MIN, ALL_BUF_USAGE, &c->ringm, (void **)&c->ringp); c->ringm0 = c->ringm; c->ringp0 = c->ringp;
        c->ring0_size = c->ring_size = RING_MIN; atomic_fetch_add(&g_ring0_bytes, RING_MIN);
        c->dps = malloc(sizeof *c->dps); c->dps[0] = dpool_new(); c->ndp = 1;
        VkQueryPoolCreateInfo qi2 = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, NULL, 0, VK_QUERY_TYPE_TIMESTAMP, 2};
        VKCHECK(vkCreateQueryPool(g_dev, &qi2, NULL, &c->qp));
        VkQueryPoolCreateInfo qi3 = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, NULL, 0, VK_QUERY_TYPE_OCCLUSION, NOQ};
        VKCHECK(vkCreateQueryPool(g_dev, &qi3, NULL, &c->oq));
        VkQueryPoolCreateInfo qi4 = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, NULL, 0, VK_QUERY_TYPE_TIMESTAMP, NTQ};
        VKCHECK(vkCreateQueryPool(g_dev, &qi4, NULL, &c->tq));
    }
    dummies();
    pthread_t th; if (pthread_create(&th, NULL, completion_thread, NULL)) die("Vulkan: cannot start the completion thread"); pthread_detach(th);
    LOG("vk: dual-source blending %s, wide lines %s (%.1f-%.1f px), BC textures %s", g_feat.dualSrcBlend ? "yes" : "no", g_feat.wideLines ? "yes" : "no", g_props.limits.lineWidthRange[0], g_props.limits.lineWidthRange[1],
        g_feat.textureCompressionBC ? "sampled by the GPU" : "decoded on upload");
    LOG("vk: %s, Vulkan %u.%u.%u, driver %#x; uint8 indices %s, %s, %s", g_props.deviceName, VK_API_VERSION_MAJOR(g_props.apiVersion), VK_API_VERSION_MINOR(g_props.apiVersion),
        VK_API_VERSION_PATCH(g_props.apiVersion), g_props.driverVersion, g_has_u8 ? "yes" : "no", g_dynrend ? "dynamic rendering" : "render passes (no dynamic rendering)",
        g_tl ? "submissions chained by a timeline semaphore" : "no timeline semaphore (presents wait for the GPU)");
}
static pthread_once_t g_once = PTHREAD_ONCE_INIT;
static void vk_init(void) { pthread_once(&g_once, vk_init_); }
static _Atomic int g_ndp_total;
// A descriptor pool of modest size: Unreal's shaders bind a dozen texel buffers, an array of 32 more and an array of 32 samplers a stage, so a submission with many draws chains
// several (dset_alloc); one pool sized for the worst case per context held hundreds of thousands of descriptors in eight contexts
static VkDescriptorPool dpool_new(void) {
    VkDescriptorPoolSize ps[] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 4096}, {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4096}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1024},
                                 {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 512}, {VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 12288}, {VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, 512},
                                 {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1024}, {VK_DESCRIPTOR_TYPE_SAMPLER, 12288}, {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 4096}};
    VkDescriptorPoolCreateInfo dpi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, 0, 1024, sizeof ps / sizeof *ps, ps};
    VkDescriptorPool p; VKCHECK(vkCreateDescriptorPool(g_dev, &dpi, NULL, &p)); atomic_fetch_add(&g_ndp_total, 1); return p;
}

// One-time commands outside the submissions (dummy contents, the present readback): their own pool, the queue lock held, waited for
static VkCommandPool g_opool;
static VkCommandBuffer once_begin(void) {
    pthread_mutex_lock(&g_qmu);
    if (!g_opool) { VkCommandPoolCreateInfo pi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, VK_COMMAND_POOL_CREATE_TRANSIENT_BIT, g_qf}; VKCHECK(vkCreateCommandPool(g_dev, &pi, NULL, &g_opool)); }
    VkCommandBuffer cb; VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, g_opool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    VKCHECK(vkAllocateCommandBuffers(g_dev, &ai, &cb));
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL};
    VKCHECK(vkBeginCommandBuffer(cb, &bi)); return cb;
}
static void once_end(VkCommandBuffer cb) {
    VKCHECK(vkEndCommandBuffer(cb));
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &cb, 0, NULL};
    VKCHECK(vkQueueSubmit(g_q, 1, &si, VK_NULL_HANDLE)); VKCHECK(vkQueueWaitIdle(g_q));
    vkFreeCommandBuffers(g_dev, g_opool, 1, &cb);
    pthread_mutex_unlock(&g_qmu);
}
static uint64_t g_vbar_epoch;   // barriers that make attachment writes visible to vertex shaders (DM_VK_PASS_BARRIER): a texture marked in an older epoch is safe for them
static void barrier_all(VkCommandBuffer cb) {
    if (C && cb == C->cb) g_vbar_epoch++;
    VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
}
// Scoped barriers in place of barrier_all (ALL_COMMANDS → ALL_COMMANDS, every access) after each dispatch and copy: FINAL FANTASY RESONANCE's battle frame has 77 dispatches, each
// followed by a full drain of the GPU; GameTurbo showed the Adreno 825 15% busy at 27 fps while the submissions spanned 24 ms
#define CONSUMER_STAGES (VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_VERTEX_INPUT_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_GEOMETRY_SHADER_BIT | \
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT | \
                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT)
#define CONSUMER_ACCESS (VK_ACCESS_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_INDEX_READ_BIT | VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_UNIFORM_READ_BIT | \
                         VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | \
                         VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT)
static VkPipelineStageFlags g_consumer_stages;   // CONSUMER_STAGES without the geometry stage on a GPU that has none
// A timed region: gpu_mark(kind, 0) before it, gpu_mark(kind, 1) after (kind 0 render pass, 1 dispatch, 2 copy / clear)
// Each region carries a signature (g_tsig_next: a pass's size, attachments and load actions, a dispatch's shader), and its GPU time is summed per signature (gsig_add, on the
// completion thread): the 600-frame lines name the passes and dispatches that cost the most (gsig_report)
static uint64_t g_tsig_next;
static void gpu_mark(int kind, int end) {
    if (C->ntq + 1 >= NTQ && !end) return;
    if (!end && (C->ntq & 1)) return;                       // a region already open
    if (end && !(C->ntq & 1)) return;                       // none open
    if (!end) { C->tkind[C->ntq / 2] = (uint8_t)kind; C->tsig[C->ntq / 2] = g_tsig_next; C->tdraws[C->ntq / 2] = 0; g_tsig_next = 0; }
    vkCmdWriteTimestamp(C->cb, end ? VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, C->tq, C->ntq++);
}
static void barrier_after(VkCommandBuffer cb, VkPipelineStageFlags src, VkAccessFlags src_access) {   // what src wrote, for everything recorded after it
    if (!g_consumer_stages) g_consumer_stages = CONSUMER_STAGES & ~(g_feat.geometryShader ? 0 : VK_PIPELINE_STAGE_GEOMETRY_SHADER_BIT);
    VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, src_access, CONSUMER_ACCESS};
    vkCmdPipelineBarrier(cb, src, g_consumer_stages, 0, 1, &mb, 0, NULL, 0, NULL);
}
#define barrier_cs(cb) barrier_after(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT)
#define barrier_xfer(cb) barrier_after(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT)
static void to_general(VkCommandBuffer cb, VkImage img, VkImageAspectFlags asp) {
    VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, 0, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                              VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, img, {asp, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS}};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
}
static void dummies(void) {
    VkSamplerCreateInfo si = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO}; si.magFilter = si.minFilter = VK_FILTER_LINEAR; si.maxLod = 0;
    VKCHECK(vkCreateSampler(g_dev, &si, NULL, &g_dummy_samp));
    VkImageCreateInfo ii = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, 0, VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, {1, 1, 1}, 1, 1, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL,
                            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_IMAGE_LAYOUT_UNDEFINED};
    VKCHECK(vkCreateImage(g_dev, &ii, NULL, &g_dummy_img));
    VkMemoryRequirements r; vkGetImageMemoryRequirements(g_dev, g_dummy_img, &r); VkDeviceMemory m = mem_alloc(r, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VKCHECK(vkBindImageMemory(g_dev, g_dummy_img, m, 0));
    VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, g_dummy_img, VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, {0}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VKCHECK(vkCreateImageView(g_dev, &vi, NULL, &g_dummy_view));
    void *zp, *sp;
    g_zero_buf = buf_new(65536, ALL_BUF_USAGE, &g_zero_mem, &zp); memset(zp, 0, 65536);
    // Ryujinx's support buffer: alpha test 0, is_bgra[8] at 16, viewport inverse 144, viewport size 160, render scale count 176, render scale float4[73] at 192 (all 1.0)
    g_support_buf = buf_new(4096, ALL_BUF_USAGE, &g_support_mem, &sp); memset(sp, 0, 4096);
    for (int i = 0; i < 73; i++) *(float *)((uint8_t *)sp + 192 + 16 * i) = 1.0f;
    void *scp; g_scratch_buf = buf_new(65536, ALL_BUF_USAGE, &g_scratch_mem, &scp); memset(scp, 0, 65536);
    VkBufferViewCreateInfo bvi = {VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO, NULL, 0, g_zero_buf, VK_FORMAT_R32G32B32A32_SFLOAT, 0, 65536};
    VKCHECK(vkCreateBufferView(g_dev, &bvi, NULL, &g_dummy_utb));
    bvi.buffer = g_scratch_buf; VKCHECK(vkCreateBufferView(g_dev, &bvi, NULL, &g_dummy_stb));
    VkImageCreateInfo si2 = ii; si2.format = VK_FORMAT_R32G32B32A32_SFLOAT; si2.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VKCHECK(vkCreateImage(g_dev, &si2, NULL, &g_dummy_simg));
    vkGetImageMemoryRequirements(g_dev, g_dummy_simg, &r); VkDeviceMemory m2 = mem_alloc(r, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT); VKCHECK(vkBindImageMemory(g_dev, g_dummy_simg, m2, 0));
    VkImageViewCreateInfo svi = vi; svi.image = g_dummy_simg; svi.format = VK_FORMAT_R32G32B32A32_SFLOAT; VKCHECK(vkCreateImageView(g_dev, &svi, NULL, &g_dummy_simg_view));
    VkCommandBuffer cb = once_begin(); to_general(cb, g_dummy_img, VK_IMAGE_ASPECT_COLOR_BIT); to_general(cb, g_dummy_simg, VK_IMAGE_ASPECT_COLOR_BIT);
    VkClearColorValue black = {{0, 0, 0, 1}}, zero = {{0}}; VkImageSubresourceRange rr = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cb, g_dummy_img, VK_IMAGE_LAYOUT_GENERAL, &black, 1, &rr); vkCmdClearColorImage(cb, g_dummy_simg, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &rr); once_end(cb);
}

// ---------- Formats ----------
// Compressed formats Mali cannot sample, decoded on upload into the smallest format that samples the same: BC1-3 and BC7 to RGBA8, BC4 to R8, BC5 to RG8 (UNORM or SNORM: the
// missing channels read 0, alpha 1, as BC4 / BC5 sample), unsigned BC6H to B10G11R11 (its 16-bit floats lose only mantissa bits below what BC6H keeps), signed BC6H to RGBA16F.
// BC4-7 by bcdec (third_party/bcdec, MIT / Unlicense)
enum { DEC_NONE, DEC_BC1, DEC_BC2, DEC_BC3, DEC_BC4, DEC_BC4S, DEC_BC5, DEC_BC5S, DEC_BC7, DEC_BC6H, DEC_BC6HS };
static int dec_bpp(int dec) { return dec == DEC_BC4 || dec == DEC_BC4S ? 1 : dec == DEC_BC5 || dec == DEC_BC5S ? 2 : dec == DEC_BC6HS ? 8 : 4; }   // bytes per decoded pixel
typedef struct { VkFormat f; uint8_t noalpha, dec, depth, stencil; } fmt_t;
static fmt_t fmt_of(int nvn) {
    fmt_t r = {VK_FORMAT_UNDEFINED, 0, 0, 0, 0};
    if (nvn >= 0x79 && nvn <= 0x94) {   // ASTC: 4x4 5x4 5x5 6x5 6x6 8x5 8x6 8x8 10x5 10x6 10x8 10x10 12x10 12x12, LDR then sRGB; Vulkan lists UNORM / SRGB in pairs
        int k = (nvn - 0x79) % 14, srgb = nvn >= 0x87; r.f = (VkFormat)(VK_FORMAT_ASTC_4x4_UNORM_BLOCK + 2 * k + srgb); return r;
    }
    switch (nvn) {
        case 0x01: r.f = VK_FORMAT_R8_UNORM; break; case 0x02: r.f = VK_FORMAT_R8_SNORM; break; case 0x03: r.f = VK_FORMAT_R8_UINT; break; case 0x04: r.f = VK_FORMAT_R8_SINT; break;
        case 0x05: r.f = VK_FORMAT_R16_SFLOAT; break; case 0x06: r.f = VK_FORMAT_R16_UNORM; break; case 0x07: r.f = VK_FORMAT_R16_SNORM; break;
        case 0x08: r.f = VK_FORMAT_R16_UINT; break; case 0x09: r.f = VK_FORMAT_R16_SINT; break;
        case 0x0A: r.f = VK_FORMAT_R32_SFLOAT; break; case 0x0B: r.f = VK_FORMAT_R32_UINT; break; case 0x0C: r.f = VK_FORMAT_R32_SINT; break;
        case 0x0D: r.f = VK_FORMAT_R8G8_UNORM; break; case 0x0E: r.f = VK_FORMAT_R8G8_SNORM; break; case 0x0F: r.f = VK_FORMAT_R8G8_UINT; break; case 0x10: r.f = VK_FORMAT_R8G8_SINT; break;
        case 0x11: r.f = VK_FORMAT_R16G16_SFLOAT; break; case 0x12: r.f = VK_FORMAT_R16G16_UNORM; break; case 0x13: r.f = VK_FORMAT_R16G16_SNORM; break;
        case 0x14: r.f = VK_FORMAT_R16G16_UINT; break; case 0x15: r.f = VK_FORMAT_R16G16_SINT; break;
        case 0x16: r.f = VK_FORMAT_R32G32_SFLOAT; break; case 0x17: r.f = VK_FORMAT_R32G32_UINT; break; case 0x18: r.f = VK_FORMAT_R32G32_SINT; break;
        case 0x6D: r.noalpha = 1; /* fall through */ case 0x25: r.f = VK_FORMAT_R8G8B8A8_UNORM; break;
        case 0x6E: r.noalpha = 1; /* fall through */ case 0x26: r.f = VK_FORMAT_R8G8B8A8_SNORM; break;
        case 0x6F: r.noalpha = 1; /* fall through */ case 0x27: r.f = VK_FORMAT_R8G8B8A8_UINT; break;
        case 0x70: r.noalpha = 1; /* fall through */ case 0x28: r.f = VK_FORMAT_R8G8B8A8_SINT; break;
        case 0x71: r.noalpha = 1; /* fall through */ case 0x29: r.f = VK_FORMAT_R16G16B16A16_SFLOAT; break;
        case 0x72: r.noalpha = 1; /* fall through */ case 0x2A: r.f = VK_FORMAT_R16G16B16A16_UNORM; break;
        case 0x73: r.noalpha = 1; /* fall through */ case 0x2B: r.f = VK_FORMAT_R16G16B16A16_SNORM; break;
        case 0x74: r.noalpha = 1; /* fall through */ case 0x2C: r.f = VK_FORMAT_R16G16B16A16_UINT; break;
        case 0x75: r.noalpha = 1; /* fall through */ case 0x2D: r.f = VK_FORMAT_R16G16B16A16_SINT; break;
        case 0x76: r.noalpha = 1; /* fall through */ case 0x2E: r.f = VK_FORMAT_R32G32B32A32_SFLOAT; break;
        case 0x77: r.noalpha = 1; /* fall through */ case 0x2F: r.f = VK_FORMAT_R32G32B32A32_UINT; break;
        case 0x78: r.noalpha = 1; /* fall through */ case 0x30: r.f = VK_FORMAT_R32G32B32A32_SINT; break;
        case 0x31: r.f = VK_FORMAT_D24_UNORM_S8_UINT; r.depth = 1; r.stencil = 1; break;   // STENCIL8 has no sampled / attachment support on Mali: a D24S8 image's stencil
        case 0x32: r.f = VK_FORMAT_D16_UNORM; r.depth = 1; break;
        case 0x33: case 0x35: r.f = VK_FORMAT_D24_UNORM_S8_UINT; r.depth = 1; r.stencil = 1; break;
        case 0x34: r.f = VK_FORMAT_D32_SFLOAT; r.depth = 1; break;
        case 0x36: r.f = VK_FORMAT_D32_SFLOAT_S8_UINT; r.depth = 1; r.stencil = 1; break;
        case 0x37: r.noalpha = 1; /* fall through */ case 0x38: r.f = VK_FORMAT_R8G8B8A8_SRGB; break;
        case 0x39: r.f = VK_FORMAT_A4B4G4R4_UNORM_PACK16; break;
        case 0x3A: r.noalpha = 1; /* fall through */ case 0x3B: r.f = VK_FORMAT_A1R5G5B5_UNORM_PACK16; break;
        case 0x3C: r.f = VK_FORMAT_R5G6B5_UNORM_PACK16; break;
        case 0x3D: r.f = VK_FORMAT_A2B10G10R10_UNORM_PACK32; break; case 0x3E: r.f = VK_FORMAT_A2B10G10R10_UINT_PACK32; break;
        case 0x3F: r.f = VK_FORMAT_B10G11R11_UFLOAT_PACK32; break; case 0x40: r.f = VK_FORMAT_E5B9G9R9_UFLOAT_PACK32; break;
        case 0x41: r.noalpha = 1; /* fall through */ case 0x42: r.f = VK_FORMAT_R8G8B8A8_UNORM; r.dec = DEC_BC1; break;
        case 0x43: r.f = VK_FORMAT_R8G8B8A8_UNORM; r.dec = DEC_BC2; break;
        case 0x44: r.f = VK_FORMAT_R8G8B8A8_UNORM; r.dec = DEC_BC3; break;
        case 0x45: r.noalpha = 1; /* fall through */ case 0x46: r.f = VK_FORMAT_R8G8B8A8_SRGB; r.dec = DEC_BC1; break;
        case 0x47: r.f = VK_FORMAT_R8G8B8A8_SRGB; r.dec = DEC_BC2; break;
        case 0x48: r.f = VK_FORMAT_R8G8B8A8_SRGB; r.dec = DEC_BC3; break;
        case 0x49: r.f = VK_FORMAT_R8_UNORM; r.dec = DEC_BC4; break; case 0x4A: r.f = VK_FORMAT_R8_SNORM; r.dec = DEC_BC4S; break;
        case 0x4B: r.f = VK_FORMAT_R8G8_UNORM; r.dec = DEC_BC5; break; case 0x4C: r.f = VK_FORMAT_R8G8_SNORM; r.dec = DEC_BC5S; break;
        case 0x4D: r.f = VK_FORMAT_R8G8B8A8_UNORM; r.dec = DEC_BC7; break; case 0x4E: r.f = VK_FORMAT_R8G8B8A8_SRGB; r.dec = DEC_BC7; break;
        case 0x4F: r.f = VK_FORMAT_R16G16B16A16_SFLOAT; r.dec = DEC_BC6HS; break; case 0x50: r.f = VK_FORMAT_B10G11R11_UFLOAT_PACK32; r.dec = DEC_BC6H; break;
        case 0x99: r.noalpha = 1; /* fall through */ case 0x9A: r.f = VK_FORMAT_B8G8R8A8_UNORM; break;
        case 0x9B: r.noalpha = 1; /* fall through */ case 0x9C: r.f = VK_FORMAT_B8G8R8A8_SRGB; break;
        case 0x95: r.f = VK_FORMAT_B5G6R5_UNORM_PACK16; break;
        case 0x96: r.noalpha = 1; /* fall through */ case 0x97: r.f = VK_FORMAT_B5G5R5A1_UNORM_PACK16; break;
        case 0x98: r.f = VK_FORMAT_A1R5G5B5_UNORM_PACK16; break;
    }
    // A GPU that samples BC (textureCompressionBC: Adreno, most desktop-class GPUs) takes the blocks as they are: no decode on upload, and a quarter to an eighth of the memory
    if (r.dec && g_feat.textureCompressionBC) {
        static const VkFormat NAT[16] = {VK_FORMAT_BC1_RGB_UNORM_BLOCK, VK_FORMAT_BC1_RGBA_UNORM_BLOCK, VK_FORMAT_BC2_UNORM_BLOCK, VK_FORMAT_BC3_UNORM_BLOCK, VK_FORMAT_BC1_RGB_SRGB_BLOCK,
            VK_FORMAT_BC1_RGBA_SRGB_BLOCK, VK_FORMAT_BC2_SRGB_BLOCK, VK_FORMAT_BC3_SRGB_BLOCK, VK_FORMAT_BC4_UNORM_BLOCK, VK_FORMAT_BC4_SNORM_BLOCK, VK_FORMAT_BC5_UNORM_BLOCK,
            VK_FORMAT_BC5_SNORM_BLOCK, VK_FORMAT_BC7_UNORM_BLOCK, VK_FORMAT_BC7_SRGB_BLOCK, VK_FORMAT_BC6H_SFLOAT_BLOCK, VK_FORMAT_BC6H_UFLOAT_BLOCK};   // NVN 0x41 … 0x50
        if (nvn >= 0x41 && nvn <= 0x50) { r.f = NAT[nvn - 0x41]; r.dec = DEC_NONE; }
    }
    return r;
}
static int fmt_block(VkFormat f) { return f >= VK_FORMAT_BC1_RGB_UNORM_BLOCK && f <= VK_FORMAT_ASTC_12x12_SRGB_BLOCK; }   // BC, ETC2, EAC, ASTC
// Vertex attribute formats (NVN format → Vulkan, *_UI2F / *_I2F → SCALED), 0 = none
static VkFormat vfmt(int f) {
    static const VkFormat T[0x6D] = {
        [0x01] = VK_FORMAT_R8_UNORM, VK_FORMAT_R8_SNORM, VK_FORMAT_R8_UINT, VK_FORMAT_R8_SINT, VK_FORMAT_R16_SFLOAT, VK_FORMAT_R16_UNORM, VK_FORMAT_R16_SNORM, VK_FORMAT_R16_UINT, VK_FORMAT_R16_SINT,
        VK_FORMAT_R32_SFLOAT, VK_FORMAT_R32_UINT, VK_FORMAT_R32_SINT, VK_FORMAT_R8G8_UNORM, VK_FORMAT_R8G8_SNORM, VK_FORMAT_R8G8_UINT, VK_FORMAT_R8G8_SINT,
        VK_FORMAT_R16G16_SFLOAT, VK_FORMAT_R16G16_UNORM, VK_FORMAT_R16G16_SNORM, VK_FORMAT_R16G16_UINT, VK_FORMAT_R16G16_SINT, VK_FORMAT_R32G32_SFLOAT, VK_FORMAT_R32G32_UINT, VK_FORMAT_R32G32_SINT,
        VK_FORMAT_R8G8B8_UNORM, VK_FORMAT_R8G8B8_SNORM, VK_FORMAT_R8G8B8_UINT, VK_FORMAT_R8G8B8_SINT, VK_FORMAT_R16G16B16_SFLOAT, VK_FORMAT_R16G16B16_UNORM, VK_FORMAT_R16G16B16_SNORM,
        VK_FORMAT_R16G16B16_UINT, VK_FORMAT_R16G16B16_SINT, VK_FORMAT_R32G32B32_SFLOAT, VK_FORMAT_R32G32B32_UINT, VK_FORMAT_R32G32B32_SINT,
        VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SNORM, VK_FORMAT_R8G8B8A8_UINT, VK_FORMAT_R8G8B8A8_SINT, VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R16G16B16A16_UNORM,
        VK_FORMAT_R16G16B16A16_SNORM, VK_FORMAT_R16G16B16A16_UINT, VK_FORMAT_R16G16B16A16_SINT, VK_FORMAT_R32G32B32A32_SFLOAT, VK_FORMAT_R32G32B32A32_UINT, VK_FORMAT_R32G32B32A32_SINT,
        [0x3D] = VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_FORMAT_A2B10G10R10_UINT_PACK32, VK_FORMAT_B10G11R11_UFLOAT_PACK32,
        [0x51] = VK_FORMAT_R8_USCALED, VK_FORMAT_R8_SSCALED, VK_FORMAT_R16_USCALED, VK_FORMAT_R16_SSCALED, VK_FORMAT_R32_UINT, VK_FORMAT_R32_SINT,
        VK_FORMAT_R8G8_USCALED, VK_FORMAT_R8G8_SSCALED, VK_FORMAT_R16G16_USCALED, VK_FORMAT_R16G16_SSCALED, VK_FORMAT_R32G32_UINT, VK_FORMAT_R32G32_SINT,
        VK_FORMAT_R8G8B8_USCALED, VK_FORMAT_R8G8B8_SSCALED, VK_FORMAT_R16G16B16_USCALED, VK_FORMAT_R16G16B16_SSCALED, VK_FORMAT_R32G32B32_UINT, VK_FORMAT_R32G32B32_SINT,
        VK_FORMAT_R8G8B8A8_USCALED, VK_FORMAT_R8G8B8A8_SSCALED, VK_FORMAT_R16G16B16A16_USCALED, VK_FORMAT_R16G16B16A16_SSCALED, VK_FORMAT_R32G32B32A32_UINT, VK_FORMAT_R32G32B32A32_SINT,
        [0x69] = VK_FORMAT_A2B10G10R10_SNORM_PACK32, VK_FORMAT_A2B10G10R10_SINT_PACK32, VK_FORMAT_A2B10G10R10_USCALED_PACK32, VK_FORMAT_A2B10G10R10_SSCALED_PACK32,
    };
    return f > 0 && f < 0x6D ? T[f] : VK_FORMAT_UNDEFINED;
}

// BC1 / BC2 / BC3 → RGBA8 (Mali has no BC sampling); BC4-7 by bcdec
#define BCDEC_IMPLEMENTATION
#define BCDEC_BC4BC5_PRECISE
#define BCDECDEF static
#include "../third_party/bcdec/bcdec.h"
static void bc_color(const uint8_t *b, uint8_t out[16][4], int alpha1) {
    uint16_t c0 = (uint16_t)(b[0] | b[1] << 8), c1 = (uint16_t)(b[2] | b[3] << 8); uint8_t p[4][4];
    for (int i = 0; i < 2; i++) { uint16_t c = i ? c1 : c0; p[i][0] = (uint8_t)((c >> 11) * 255 / 31); p[i][1] = (uint8_t)((c >> 5 & 63) * 255 / 63); p[i][2] = (uint8_t)((c & 31) * 255 / 31); p[i][3] = 255; }
    if (c0 > c1 || !alpha1) for (int k = 0; k < 3; k++) { p[2][k] = (uint8_t)((2 * p[0][k] + p[1][k]) / 3); p[3][k] = (uint8_t)((p[0][k] + 2 * p[1][k]) / 3); }
    else for (int k = 0; k < 3; k++) { p[2][k] = (uint8_t)((p[0][k] + p[1][k]) / 2); p[3][k] = 0; }
    p[2][3] = 255; p[3][3] = c0 > c1 || !alpha1 ? 255 : 0;
    uint32_t idx = (uint32_t)b[4] | (uint32_t)b[5] << 8 | (uint32_t)b[6] << 16 | (uint32_t)b[7] << 24;
    for (int i = 0; i < 16; i++) memcpy(out[i], p[idx >> (2 * i) & 3], 4);
}
static void bc_spread(uint8_t px[16][8]) { for (int i = 15; i >= 0; i--) memmove(px[i], (uint8_t *)px + 4 * i, 4); }   // 16 packed RGBA8 pixels → one per 8-byte slot
static void bc_decode(int dec, const uint8_t *src, size_t src_pitch, uint32_t w, uint32_t h, uint8_t *dst) {   // dst: w * h * dec_bpp(dec)
    uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4, bs = dec == DEC_BC1 || dec == DEC_BC4 || dec == DEC_BC4S ? 8 : 16, pb = (uint32_t)dec_bpp(dec);
    for (uint32_t by = 0; by < bh; by++) for (uint32_t bx = 0; bx < bw; bx++) {
        const uint8_t *b = src + by * src_pitch + bx * bs; uint8_t px[16][8];
        if (dec == DEC_BC1) bc_color(b, (uint8_t (*)[4])px, 1), bc_spread(px);
        else if (dec == DEC_BC4 || dec == DEC_BC4S || dec == DEC_BC5 || dec == DEC_BC5S) {
            int sg = dec == DEC_BC4S || dec == DEC_BC5S, two = dec == DEC_BC5 || dec == DEC_BC5S; uint8_t rg[16][2] = {{0}}, r1[16] = {0};
            if (two) bcdec_bc5(b, rg, 8, sg); else bcdec_bc4(b, r1, 4, sg);   // pitch in bytes: rows of 4 RG / R pixels
            for (int i = 0; i < 16; i++) { px[i][0] = two ? rg[i][0] : r1[i]; px[i][1] = two ? rg[i][1] : 0; px[i][2] = 0; px[i][3] = sg ? 127 : 255; }
        } else if (dec == DEC_BC7) bcdec_bc7(b, px, 32);   // RGBA8 rows of 4 pixels, in a block buffer of 8-byte pixels: pitch 32 bytes
        else if (dec == DEC_BC6H || dec == DEC_BC6HS) {
            uint16_t rgb[16][3]; bcdec_bc6h_half(b, rgb, 12, dec == DEC_BC6HS);   // pitch in halves
            for (int i = 0; i < 16; i++) {
                if (dec == DEC_BC6HS) { uint16_t q[4] = {rgb[i][0], rgb[i][1], rgb[i][2], 0x3C00}; memcpy(px[i], q, 8); continue; }
                // half → unsigned 11 / 10-bit floats: same 5-bit exponent and bias, the mantissa cut to 6 / 5 bits; negative values (none in unsigned BC6H) as 0
                uint32_t r11 = rgb[i][0] & 0x8000 ? 0 : rgb[i][0] >> 4, g11 = rgb[i][1] & 0x8000 ? 0 : rgb[i][1] >> 4, b10 = rgb[i][2] & 0x8000 ? 0 : rgb[i][2] >> 5;
                uint32_t pk = (r11 & 0x7ff) | (g11 & 0x7ff) << 11 | (b10 & 0x3ff) << 22; memcpy(px[i], &pk, 4);
            }
        } else {
            bc_color(b + 8, (uint8_t (*)[4])px, 0); bc_spread(px);
            if (dec == DEC_BC2) for (int i = 0; i < 16; i++) { int a = b[i / 2] >> (4 * (i & 1)) & 15; px[i][3] = (uint8_t)(a * 17); }
            else {
                uint8_t a[8] = {b[0], b[1]};
                if (a[0] > a[1]) for (int i = 2; i < 8; i++) a[i] = (uint8_t)(((8 - i) * a[0] + (i - 1) * a[1]) / 7);
                else { for (int i = 2; i < 6; i++) a[i] = (uint8_t)(((6 - i) * a[0] + (i - 1) * a[1]) / 5); a[6] = 0; a[7] = 255; }
                uint64_t bits = 0; for (int i = 0; i < 6; i++) bits |= (uint64_t)b[2 + i] << (8 * i);
                for (int i = 0; i < 16; i++) px[i][3] = a[bits >> (3 * i) & 7];
            }
        }
        for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) {
            uint32_t X = bx * 4 + x, Y = by * 4 + y;
            if (X < w && Y < h) memcpy(dst + ((size_t)Y * w + X) * pb, dec == DEC_BC7 ? (const uint8_t *)px + y * 32 + x * 4 : px[y * 4 + x], pb);
        }
    }
}

// ---------- Images ----------
typedef struct { uint64_t key; VkImageView v; } vview;
struct vtex {
    uint64_t serial; VkImage img; VkDeviceMemory m; int blk; VkDeviceSize moff, msize; fmt_t f; int nvn_fmt, target;
    uint32_t w, h, d, levels, layers; VkImageAspectFlags aspect; int uploaded, gpu;
    int dirty;   // the CPU wrote its storage since the last upload (WriteTexels, a movie frame, a flushed pool range): uploaded again at its next use, unless the GPU renders into it
    const uint8_t *mem; uint64_t len;   // its storage in guest memory
    uint64_t stamp; const void *span;   // uploaded from a dirty-tracked pool (nvn_dirty.c): the write generation then and the tracked range, re-uploaded when the game writes it
    vview *views; int nviews;
    struct vtex *next;
    uint64_t dead;   // released by the game: freed once the GPU has finished the submission that was being recorded then (fence dead)
    uint64_t lhash, lhash_gen; uint8_t lcpu;
    VkFormat vfl[2]; uint8_t nvfl; VkImageFormatListCreateInfo fli;   // DM_VK_UBWC: the only formats its views may take (nvfl 0: any, MUTABLE_FORMAT without a list)
    VkImageCreateInfo ci; uint8_t packaged, evicted; uint64_t last_frame, last_fence;   // texture eviction (tex_evict): how it was made, whether its content comes from its storage, when it was last used
    uint32_t gpu_lv;   // levels a copy wrote on the GPU (texture or buffer copies into one level): an upload from storage leaves them alone, the rest still follows the game's writes   // lcpu: a linear texture in a pool the CPU writes (a glyph cache): its storage hashed once a submission, uploaded again when it changed
    uint64_t vhaz_epoch; uint8_t vhaz;   // DM_VK_PASS_BARRIER: written by a pass whose end barrier left vertex shaders out, in that barrier epoch (see pass_barrier_on)
    int storage;   // made with VK_IMAGE_USAGE_STORAGE_BIT (an NVN image texture whose format Vulkan can store to)
    int copied;   // a GPU copy (copy_b2t) or a shader's image store wrote into it: hibernation reads it back before a snapshot even when it was uploaded first
    uint8_t late;   // made before the game wrote its storage, watched for that (late_fill)
    uint32_t mir_lv;   // levels a GPU copy wrote whose bytes were written into the storage too (copy_b2t / copy_t2t mirror): the storage holds them, as the image does
    uint32_t linrow, linbpb;   // a linear 2D texture (LINEAR / LINEAR_RENDER_TARGET) the CPU can read: its row pitch and bytes per texel; what passes render into it goes back to its storage (lin_writeback)
};
static vtex **g_gb; static int g_ngb, g_cgb; static pthread_mutex_t g_gbmu = PTHREAD_MUTEX_INITIALIZER;
#define NHASH 4096
static vtex *g_tex[NHASH];
static vtex *tex_find(uint64_t ser) { for (vtex *v = g_tex[ser % NHASH]; v; v = v->next) if (v->serial == ser) return v; return NULL; }
static uint64_t *g_cpu_written; static int g_ncpu, g_ccpu;   // serials WriteTexels wrote (mtl_tex_cpu_written)
static int cpu_written(uint64_t ser) { for (int i = 0; i < g_ncpu; i++) if (g_cpu_written[i] == ser) return 1; return 0; }

static int g_plog;   // DM_VK_PASSLOG=<frame>[+]: that frame's (or every frame's from there) passes, draws, copies, clears and uploads are logged (diagnostics)
// Diagnostics: DM_VK_SKIP=<mask> leaves out kinds of GPU work, to find which one a lost device comes from: 1 buffer-to-texture copies, 2 texture copies, 4 texture-to-buffer
// copies, 8 texture clears, 16 texture uploads, 32 draws, 64 attachment clears
enum { SK_B2T = 1, SK_T2T = 2, SK_T2B = 4, SK_CLEAR = 8, SK_UPLOAD = 16, SK_DRAW = 32, SK_ACLEAR = 64 };
static int vk_skip(void) { static int m = -1; if (m < 0) { const char *e = getenv("DM_VK_SKIP"); m = e ? (int)strtol(e, NULL, 0) : 0; if (m) LOG("DM_VK_SKIP=%#x: GPU work of those kinds is left out", m); } return m; }
static void tex_upload(vtex *v, const tex_t *t);
// A texture with no packaged layout in a CPU_NO_ACCESS pool whose storage already holds data takes it as its initial content, as nvn_mtl.m tex_init_fill: games load a file into
// memory and make a GPU-only pool over it (Metroid Dread's BC6H light probe cubemaps, which sampled as zero and left every lit surface black). The same in a CPU_UNCACHED pool
// (its writes reach memory unflushed) and for storage the CPU flushed before the texture's first use (nvn_flushed; DM_NVN_FLUSH_FILL=0 leaves it out), with COMPRESSIBLE
// textures too: compression applies to what the GPU renders into one, and at its first use the storage holds plain texels (FINAL FANTASY XII THE ZODIAC AGE sets the flag on
// every texture, its CPU-written and flushed battle target ring and line art included, which drew with alpha 0 otherwise). Multisample and depth textures stay GPU textures
// DM_VK_UBWC=1: images whose views never reinterpret their format keep the Adreno's bandwidth compression (UBWC). Every image used to be made MUTABLE_FORMAT with no format list,
// which the driver may answer by leaving the image uncompressed: every render target, depth buffer and texture read and written at full size. Now an image is MUTABLE_FORMAT
// only with a format list -- its own format and its sRGB / UNORM sibling -- or not mutable at all where it has no sibling (depth images included: their views keep their format).
// Storage images stay as they were (any view). A view asking for a format outside the list takes the image's own (counted: g_ubwc_fallback). Default 0 (off) until measured
static int ubwc_on(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_VK_UBWC"); v = e && atoi(e) != 0; if (v) LOG("DM_VK_UBWC=1: images without format reinterpretation keep compression (format lists)"); } return v; }
static uint64_t g_ubwc_fallback, g_ubwc_imgs;
static VkFormat fmt_sibling(VkFormat f) {   // the sRGB / UNORM twin of a format, VK_FORMAT_UNDEFINED when it has none
    static const VkFormat pr[][2] = {{VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SRGB}, {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_B8G8R8A8_SRGB}, {VK_FORMAT_A8B8G8R8_UNORM_PACK32, VK_FORMAT_A8B8G8R8_SRGB_PACK32},
        {VK_FORMAT_R8_UNORM, VK_FORMAT_R8_SRGB}, {VK_FORMAT_R8G8_UNORM, VK_FORMAT_R8G8_SRGB}, {VK_FORMAT_R8G8B8_UNORM, VK_FORMAT_R8G8B8_SRGB},
        {VK_FORMAT_BC1_RGB_UNORM_BLOCK, VK_FORMAT_BC1_RGB_SRGB_BLOCK}, {VK_FORMAT_BC1_RGBA_UNORM_BLOCK, VK_FORMAT_BC1_RGBA_SRGB_BLOCK}, {VK_FORMAT_BC2_UNORM_BLOCK, VK_FORMAT_BC2_SRGB_BLOCK},
        {VK_FORMAT_BC3_UNORM_BLOCK, VK_FORMAT_BC3_SRGB_BLOCK}, {VK_FORMAT_BC7_UNORM_BLOCK, VK_FORMAT_BC7_SRGB_BLOCK},
        {VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK, VK_FORMAT_ETC2_R8G8B8_SRGB_BLOCK}, {VK_FORMAT_ETC2_R8G8B8A1_UNORM_BLOCK, VK_FORMAT_ETC2_R8G8B8A1_SRGB_BLOCK}, {VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK, VK_FORMAT_ETC2_R8G8B8A8_SRGB_BLOCK},
        {VK_FORMAT_ASTC_4x4_UNORM_BLOCK, VK_FORMAT_ASTC_4x4_SRGB_BLOCK}, {VK_FORMAT_ASTC_5x5_UNORM_BLOCK, VK_FORMAT_ASTC_5x5_SRGB_BLOCK}, {VK_FORMAT_ASTC_6x6_UNORM_BLOCK, VK_FORMAT_ASTC_6x6_SRGB_BLOCK},
        {VK_FORMAT_ASTC_8x8_UNORM_BLOCK, VK_FORMAT_ASTC_8x8_SRGB_BLOCK}};
    for (size_t i = 0; i < sizeof pr / sizeof *pr; i++) { if (pr[i][0] == f) return pr[i][1]; if (pr[i][1] == f) return pr[i][0]; }
    return VK_FORMAT_UNDEFINED;
}
static int mem_any(const uint8_t *p, uint64_t n) { uint64_t i = 0, x; for (; i + 8 <= n; i += 8) { memcpy(&x, p + i, 8); if (x) return 1; } for (; i < n; i++) if (p[i]) return 1; return 0; }
int nvn_flushed(const void *p, uint64_t n);   // hle_nvn.c
static int init_fill(const tex_t *t, const fmt_t *f) {
    if (!t->mem || !t->pool || (t->pool->flags & NVN_MEMORY_POOL_FLAGS_VIRTUAL) || t->has_layout) return 0;
    if (t->d.samples > 1 || f->depth || t->d.target == NVN_TEXTURE_TARGET_BUFFER) return 0;
    int no_cpu = (t->pool->flags & NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS) != 0, uncached = (t->pool->flags & NVN_MEMORY_POOL_FLAGS_CPU_UNCACHED) != 0;
    static int flush_fill = -1; if (flush_fill < 0) { const char *e = getenv("DM_NVN_FLUSH_FILL"); flush_fill = !e || atoi(e) != 0; }
    if (!no_cpu && !uncached && (!flush_fill || !nvn_flushed(t->mem, t->L.size))) return 0;
    return mem_any(t->mem, t->L.size);
}
// A texture init_fill would take from its storage, but whose storage is still all zero at its first use: the game's loader has not written it yet (FINAL FANTASY RESONANCE's
// castle materials on the Xiaomi Pad 8, whose file reads are slower than the Mac's: drawn before their data arrived). It used to stay a cleared GPU texture that nothing
// watched, and when the data came the material stayed black (a black albedo under the usual specular: dark, glossy columns and panels). Now its storage is watched from that
// first use and it is uploaded once the game writes it (tex_get), as a texture made from storage is. Not for render targets, images or CPU_NO_ACCESS pools (the CPU writes none).
// DM_VK_LATE_FILL=0 leaves it out
static uint64_t g_late_watch, g_late_fill;
static int late_fill(const tex_t *t, const fmt_t *f) {
    static int on = -1; if (on < 0) { const char *e = getenv("DM_VK_LATE_FILL"); on = !e || atoi(e) != 0; }
    if (!on || !t->mem || !t->pool || (t->pool->flags & (NVN_MEMORY_POOL_FLAGS_VIRTUAL | NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS)) || t->has_layout) return 0;
    if (t->d.samples > 1 || f->depth || t->d.target == NVN_TEXTURE_TARGET_BUFFER) return 0;
    if (t->d.flags & (NVN_TEXTURE_FLAGS_IMAGE | NVN_TEXTURE_FLAGS_LINEAR | NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET)) return 0;
    if ((t->d.flags & NVN_TEXTURE_FLAGS_COMPRESSIBLE) && !fmt_block(f->f) && !f->dec) return 0;   // COMPRESSIBLE marks a render target, except on a block-compressed format
    return !mem_any(t->mem, t->L.size);
}
static uint64_t mem_hash(const uint8_t *p, uint64_t n) { uint64_t h = 0x9e3779b97f4a7c15ull, x, i = 0; for (; i + 8 <= n; i += 8) { memcpy(&x, p + i, 8); h = (h ^ x) * 0x100000001b3ull; } for (; i < n; i++) h = (h ^ p[i]) * 0x100000001b3ull; return h; }
static int g_occ_over;
static _Atomic uint64_t g_presents;   // frames presented (mtl_present)
static uint64_t g_evicted, g_remade;   // textures evicted / made again from their storage (the memory line)
static int tex_remake(vtex *v, const tex_t *t);
static uint64_t g_pkg_up, g_pkg_reup;   // textures uploaded from their storage when made / again after the game wrote it (the packaged textures line, mtl_pkg_stats)
static uint64_t g_bvgen;   // the submission's number (bview, the linear texture hashes)
static uint64_t g_tex_dead, g_treg_dead;   // lookups of a texture object that is no longer one (tex_get) / registrations of one (mtl_reg_tex)
static vtex *tex_get(const tex_t *t, int for_rt) {
    // A texture object that is no longer one -- finalized, its memory reused for other data -- is never made into an image (as nvn_mtl.m tex_mtl_ checks the magic): FINAL
    // FANTASY RESONANCE registers texture objects whose memory holds half-float constants by then (serial 0x3c003c003c003c00), and their slots and image bindings stay bound.
    // An unknown format used to stop them by chance; a plausible one would have made an image of garbage size from a garbage storage pointer
    if (!t || t->magic != MAGIC('T', 'E', 'X', 0)) { if (t) g_tex_dead++; return NULL; }
    pthread_mutex_lock(&g_rmu); vtex *v = tex_find(t->serial); pthread_mutex_unlock(&g_rmu);
    if (v && v->lcpu && !v->gpu && !for_rt && v->lhash_gen != g_bvgen) {   // a CPU-written linear texture (Unreal's Slate font atlas, a glyph cache): the GPU reads it in place on the Switch
        v->lhash_gen = g_bvgen; uint64_t h = mem_hash(t->mem, t->L.size);
        if (h != v->lhash) { v->lhash = h; tex_upload(v, t); }
    }
    if (v) {
        if (v->dead && v->dead < g_fence + 1) v->dead = g_fence + 1;   // released, still named by a command recorded before: kept until this submission is done
        v->last_frame = atomic_load(&g_presents); v->last_fence = g_fence + 1;
        if (v->evicted && !tex_remake(v, t)) return NULL;   // evicted while unused (tex_evict): made again from its storage
        if (for_rt) v->gpu = 1;
        else if (!v->gpu && t->mem && (v->dirty || (v->span && nvn_dirty_since_span(v->span, t->mem, t->L.size, v->stamp)))) { v->dirty = 0; tex_upload(v, t); g_pkg_reup++;
            if (v->late) { v->late = 0; v->packaged = 1; g_late_fill++; } }   // a texture whose data came after its first use (late_fill): from its storage from now on   // never over what the GPU drew: a flush of the pool range around a render target is not new content
        return v;
    }
    fmt_t f = fmt_of(t->d.format);
    if (!f.f) { static int n; if (n++ < 8) LOG("[RED] vk: texture format %#x has no Vulkan format yet (serial %llu)", t->d.format, (unsigned long long)t->serial); return NULL; }
    if (t->d.target == NVN_TEXTURE_TARGET_BUFFER) { RED_ONCE(3, "buffer textures are not implemented yet"); return NULL; }
    v = calloc(1, sizeof *v); v->serial = t->serial; v->f = f; v->nvn_fmt = t->d.format; v->target = t->d.target; v->mem = t->mem; v->len = t->L.size;
    v->w = t->d.width ? t->d.width : 1; v->h = t->d.height ? t->d.height : 1; v->levels = t->d.levels ? t->d.levels : 1;
    int is3d = t->d.target == NVN_TEXTURE_TARGET_3D, cube = t->d.target == NVN_TEXTURE_TARGET_CUBEMAP || t->d.target == NVN_TEXTURE_TARGET_CUBEMAP_ARRAY;
    v->d = is3d ? (t->d.depth ? t->d.depth : 1) : 1;
    v->layers = is3d ? 1 : t->L.layers ? t->L.layers : t->d.target == NVN_TEXTURE_TARGET_2D_ARRAY || t->d.target == NVN_TEXTURE_TARGET_1D_ARRAY ? (t->d.depth ? t->d.depth : 1) : cube ? 6 : 1;
    if (t->d.target == NVN_TEXTURE_TARGET_1D_ARRAY && !t->L.layers) v->layers = t->d.height ? t->d.height : 1;
    v->aspect = f.depth ? VK_IMAGE_ASPECT_DEPTH_BIT | (f.stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0) : VK_IMAGE_ASPECT_COLOR_BIT;
    VkFormatProperties fp; vkGetPhysicalDeviceFormatProperties(g_pd, f.f, &fp);
    VkImageUsageFlags u = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) u |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) u |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    if ((fp.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) && (t->d.flags & NVN_TEXTURE_FLAGS_IMAGE)) u |= VK_IMAGE_USAGE_STORAGE_BIT;
    VkSampleCountFlagBits sc = t->d.samples > 1 ? (VkSampleCountFlagBits)t->d.samples : VK_SAMPLE_COUNT_1_BIT;
    v->storage = (u & VK_IMAGE_USAGE_STORAGE_BIT) != 0;
    VkImageCreateInfo ii = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | (cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0) | (is3d ? VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT : 0),
                            is3d ? VK_IMAGE_TYPE_3D : t->d.target == NVN_TEXTURE_TARGET_1D || t->d.target == NVN_TEXTURE_TARGET_1D_ARRAY ? VK_IMAGE_TYPE_1D : VK_IMAGE_TYPE_2D,
                            f.f, {v->w, t->d.target == NVN_TEXTURE_TARGET_1D_ARRAY ? 1 : v->h, v->d}, v->levels, v->layers, sc, VK_IMAGE_TILING_OPTIMAL, u, VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_IMAGE_LAYOUT_UNDEFINED};
    if (sc != VK_SAMPLE_COUNT_1_BIT) ii.mipLevels = 1;
    if (ubwc_on() && !(u & VK_IMAGE_USAGE_STORAGE_BIT)) {   // see ubwc_on: a format list, or not mutable
        VkFormat sib = f.depth ? VK_FORMAT_UNDEFINED : fmt_sibling(f.f); v->vfl[0] = f.f; v->nvfl = 1;
        if (sib != VK_FORMAT_UNDEFINED) { v->vfl[1] = sib; v->nvfl = 2; v->fli = (VkImageFormatListCreateInfo){VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO, NULL, 2, v->vfl}; ii.pNext = &v->fli; }
        else ii.flags &= ~(VkImageCreateFlags)VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
        g_ubwc_imgs++;
    }
    VkResult r = vkCreateImage(g_dev, &ii, NULL, &v->img);
    if (r != VK_SUCCESS) { LOG("[RED] vk: image %ux%ux%u x%u levels %u format %#x target %d not created (%d)", v->w, v->h, v->d, v->layers, v->levels, t->d.format, t->d.target, (int)r); free(v); return NULL; }
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(g_dev, v->img, &mr); VkDeviceSize off;
    v->blk = img_mem(mr, &v->m, &off); v->moff = off; v->msize = mr.size; VKCHECK(vkBindImageMemory(g_dev, v->img, v->m, off));
    if ((t->d.flags & (NVN_TEXTURE_FLAGS_LINEAR | NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET)) && t->mem && t->d.target == NVN_TEXTURE_TARGET_2D && v->levels == 1 && v->layers == 1 && !f.dec && !f.depth && sc == VK_SAMPLE_COUNT_1_BIT) {
        int bw = 1, bh = 1, bpb = nvn_fmt_bpb(t->d.format, &bw, &bh); uint32_t row = t->d.stride ? (uint32_t)t->d.stride : v->w * (uint32_t)bpb;
        if (bpb && bw == 1 && bh == 1 && row % bpb == 0 && (uint64_t)row * v->h <= t->L.size) { v->linrow = row; v->linbpb = (uint32_t)bpb; }
    }
    to_general(C->up, v->img, v->aspect); C->up_used = 1;
    v->gpu = for_rt;
    pthread_mutex_lock(&g_rmu); v->next = g_tex[v->serial % NHASH]; g_tex[v->serial % NHASH] = v; pthread_mutex_unlock(&g_rmu);
    // A linear 2D texture in a pool the CPU can write (not CPU_NO_ACCESS, at most 16 MB): its storage is what the game wrote there, uploaded now and again whenever it changes
    v->lcpu = (t->d.flags & (NVN_TEXTURE_FLAGS_LINEAR | NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET)) && t->mem && t->pool && !(t->pool->flags & NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS) &&
              !(t->pool->flags & NVN_MEMORY_POOL_FLAGS_VIRTUAL) && t->L.size <= (16u << 20) && !f.depth && sc == VK_SAMPLE_COUNT_1_BIT;
    int packaged = (t->has_layout || cpu_written(t->serial) || init_fill(t, &f) || v->lcpu) && t->mem && !for_rt;
    { static int tl = -1; if (tl < 0) { const char *e = getenv("DM_VK_TEXLOG"); tl = e && atoi(e) ? atoi(e) : 0; } static int n;   // diagnostics: DM_VK_TEXLOG=<n> logs the first n textures made
      if (tl && n++ < tl) LOG("vk: texture %llu: %ux%ux%u x%u levels %u format %#x target %d flags %#x stride %u size %llu pool %#x%s%s%s%s", (unsigned long long)t->serial, v->w, v->h, v->d, v->layers, v->levels,
                              t->d.format, t->d.target, t->d.flags, t->d.stride, (unsigned long long)t->L.size, t->pool ? t->pool->flags : 0, t->has_layout ? " packaged" : "", v->lcpu ? " linear-cpu" : "",
                              packaged ? " uploaded" : "", for_rt ? " render-target" : ""); }
    if (v->lcpu && packaged) { v->lhash = mem_hash(t->mem, t->L.size); v->lhash_gen = g_bvgen; }
    v->ci = ii; v->packaged = packaged && !for_rt; v->last_frame = atomic_load(&g_presents); v->last_fence = g_fence + 1;
    if (packaged) { tex_upload(v, t); g_pkg_up++; }
    else if (!for_rt && late_fill(t, &f) && nvn_dirty_tracked(t->mem, t->L.size)) {   // still empty: cleared now, watched, uploaded when the game writes it (late_fill)
        v->stamp = nvn_dirty_now(); nvn_dirty_protect(t->mem, t->L.size); v->span = nvn_dirty_span(t->mem, t->L.size); v->late = 1; g_late_watch++;
        VkClearColorValue z = {{0}}; VkImageSubresourceRange rr = {VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
        if (!fmt_block(f.f) && (u & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) { vkCmdClearColorImage(C->up, v->img, VK_IMAGE_LAYOUT_GENERAL, &z, 1, &rr); C->up_used = 1; }
    }
    else {   // contents come from the GPU (copies, render passes): start cleared, as a new texture's storage would read on hardware after a clear. Render targets too: a pass that
             // loads one before anything drew into it, or a window texture presented with nothing drawn (FINAL FANTASY XII THE ZODIAC AGE's first frames; magenta on the Metal host
             // until it zero-filled them), would read Vulkan's undefined content
        VkClearColorValue z = {{0}}; VkImageSubresourceRange rr = {VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
        if (!f.depth && !fmt_block(f.f) && sc == VK_SAMPLE_COUNT_1_BIT && (u & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) { vkCmdClearColorImage(C->up, v->img, VK_IMAGE_LAYOUT_GENERAL, &z, 1, &rr); C->up_used = 1; }   // no compressed format (vkCmdClearColorImage)
    }
    return v;
}
// ---------- Texture eviction ----------
// The images of packaged textures mirror what the game holds in its own memory pools (the storage they were uploaded from), so one unused for a long while is freed and made again
// from that storage at its next use: half a gigabyte of images in FINAL FANTASY RESONANCE's battles, most of them for places the camera had left. Only textures whose content is
// their storage (packaged, never rendered into or copied to on the GPU, not written by the CPU since), and only when no submission in flight can use them. While the image memory
// is above EVICT_FLOOR, those unused for EVICT_AGE frames go; on Android's low-memory signal (dm_vk_trim), those unused for a second
#define EVICT_FLOOR (192ull << 20)
#define EVICT_AGE 900
static int tex_remake(vtex *v, const tex_t *t) {
    if (vkCreateImage(g_dev, &v->ci, NULL, &v->img) != VK_SUCCESS) { v->img = VK_NULL_HANDLE; RED_ONCE(90, "an evicted texture could not be made again"); return 0; }
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(g_dev, v->img, &mr); VkDeviceSize off;
    v->blk = img_mem(mr, &v->m, &off); v->moff = off; v->msize = mr.size; VKCHECK(vkBindImageMemory(g_dev, v->img, v->m, off));
    to_general(C->up, v->img, v->aspect); C->up_used = 1;
    v->evicted = 0; g_remade++;
    tex_upload(v, t);
    return 1;
}
static uint64_t g_trim_gen_tex;
static void tex_evict(uint64_t trim_gen) {
    static uint64_t last; uint64_t n = atomic_load(&g_presents); int trim = trim_gen != g_trim_gen_tex; g_trim_gen_tex = trim_gen;
    if (!trim && n - last < 60) return;
    last = n;
    uint64_t held = 0; pthread_mutex_lock(&g_blkmu); for (int i = 0; i < g_nblk; i++) if (g_blk[i].m) held += g_blk[i].size; pthread_mutex_unlock(&g_blkmu);
    if (!trim && held < EVICT_FLOOR) return;
    uint64_t age = trim ? 60 : EVICT_AGE, done; pthread_mutex_lock(&g_mu); done = g_done; pthread_mutex_unlock(&g_mu);
    int nev = 0;
    pthread_mutex_lock(&g_rmu);
    for (int hh = 0; hh < NHASH; hh++) for (vtex *v = g_tex[hh]; v; v = v->next) {
        if (v->dead || v->evicted || !v->packaged || v->gpu || v->gpu_lv || v->dirty || !v->img || v->last_fence > done || n - v->last_frame < age) continue;
        for (int k = 0; k < v->nviews; k++) vkDestroyImageView(g_dev, v->views[k].v, NULL);
        free(v->views); v->views = NULL; v->nviews = 0;
        vkDestroyImage(g_dev, v->img, NULL); img_mem_free(v->blk, v->m, v->moff, v->msize); v->img = VK_NULL_HANDLE; v->evicted = 1; nev++;
    }
    pthread_mutex_unlock(&g_rmu);
    g_evicted += nev;
    if (nev) { static int nl; if (nl++ < 8 || trim) LOG("vk: %d textures unused for %llu frames evicted%s (image memory was %.0f MB)", nev, (unsigned long long)age, trim ? " on the low-memory signal" : "", held / 1048576.0); }
}
// Packaged texture: each level and layer deswizzled from the pool storage into the ring, then copied into the image by the upload command buffer (it runs before this submission's draws)
static uint64_t g_t_up, g_up_bytes; uint64_t g_nsub_stat;
static void tex_upload_(vtex *v, const tex_t *t);
static void tex_upload(vtex *v, const tex_t *t) { uint64_t t0 = now_ns(); tex_upload_(v, t); g_t_up += now_ns() - t0; g_up_bytes += t->L.size; atomic_fetch_add(&g_up_cum, t->L.size); }
static void tex_upload_(vtex *v, const tex_t *t) {
    if (vk_skip() & SK_UPLOAD) return;
    if (g_plog) LOG("plog:  upload %llu fmt %#x %ux%ux%u levels %u layers %u target %d", (unsigned long long)t->serial, t->d.format, v->w, v->h, v->d, v->levels, v->layers, t->d.target);
    int bw = 1, bh = 1, bpb = nvn_fmt_bpb(t->d.format, &bw, &bh);
    if (!bpb) { RED_ONCE(4, "texture format %#x: block size unknown, not uploaded", t->d.format); return; }
    // As nvn_mtl.m does for packaged textures: the storage is write-protected, and a game write to it (streamed texture data arriving after the texture was made, as Super Mario
    // Odyssey's normal maps do; noise until then) brings a new upload at the texture's next use (tex_get). The stamp is taken and the pages protected BEFORE the storage is read
    // (nvn_dirty.h): protected after the read, a write landing while it ran, or on a page a write had already unprotected, was never seen, and the texture kept what it was mid-write
    if (t->mem && !v->gpu && nvn_dirty_tracked(t->mem, t->L.size)) { v->stamp = nvn_dirty_now(); nvn_dirty_protect(t->mem, t->L.size); v->span = nvn_dirty_span(t->mem, t->L.size); }
    for (unsigned lv = 0; lv < v->levels; lv++) {
        if (lv < 32 && (v->gpu_lv >> lv & 1)) continue;   // a copy on the GPU wrote this level: its storage does not hold that
        uint32_t w = v->w >> lv ? v->w >> lv : 1, h = v->h >> lv ? v->h >> lv : 1, d = v->d >> lv ? v->d >> lv : 1;
        if (t->d.target == NVN_TEXTURE_TARGET_1D_ARRAY) h = 1;
        uint32_t bx = (w + bw - 1) / bw, by = (h + bh - 1) / bh; size_t pitch = (size_t)bx * bpb, lsz = pitch * by * d;
        for (unsigned ly = 0; ly < v->layers; ly++) {
            size_t out = v->f.dec ? (size_t)w * h * dec_bpp(v->f.dec) * d : lsz;
            VkDeviceSize o = ring_alloc(out, 16);
            if (o == VK_WHOLE_SIZE) { v->dirty = 1; return; }   // not uploaded: again at the next use (the stamp above would say nothing changed)
            uint8_t *dst = C->ringp + o, *tmp = v->f.dec ? malloc(lsz) : dst;
            int rc = 0;
            if (t->d.flags & (NVN_TEXTURE_FLAGS_LINEAR | NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET)) {
                // Linear storage: rows at the texture's own pitch (its stride, padded past width * bytes per texel), layers L.layer_size apart -- copied row by row. One block
                // of lsz bytes read the rows skewed whenever the pitch was padded, and LINEAR_RENDER_TARGET went through the block-linear deswizzle (glyphs drawn as blocky noise)
                size_t srow = t->d.levels <= 1 && t->d.height && t->L.layer_size >= (uint64_t)pitch * by ? (size_t)(t->L.layer_size / (by * d)) : pitch;
                if (lv == 0 && t->d.stride >= pitch) srow = t->d.stride;
                if (srow < pitch) srow = pitch;
                const uint8_t *src = t->mem + (uint64_t)ly * t->L.layer_size + nvn_tex_level_offset(&t->d, lv);
                if ((uint64_t)ly * t->L.layer_size + nvn_tex_level_offset(&t->d, lv) + srow * (by * d - 1) + pitch > t->L.size) { rc = 1; RED_ONCE(6, "linear texture %ux%u format %#x: rows at pitch %zu run past its storage", v->w, v->h, t->d.format, srow); }
                else for (uint32_t yy = 0; yy < by * d; yy++) memcpy(tmp + (size_t)yy * pitch, src + (size_t)yy * srow, pitch);
            } else rc = nvn_deswizzle(&t->d, lv, ly, t->mem, tmp, pitch);
            if (rc) { RED_ONCE(5, "texture format %#x target %d: deswizzle failed", t->d.format, t->d.target); if (v->f.dec) free(tmp); continue; }
            if (v->f.dec) { for (uint32_t z = 0; z < d; z++) bc_decode(v->f.dec, tmp + pitch * by * z, pitch, w, h, dst + (size_t)w * h * dec_bpp(v->f.dec) * z); free(tmp); }
            VkBufferImageCopy c = {o, 0, 0, {v->aspect & VK_IMAGE_ASPECT_COLOR_BIT ? VK_IMAGE_ASPECT_COLOR_BIT : VK_IMAGE_ASPECT_DEPTH_BIT, lv, ly, 1}, {0, 0, 0}, {w, h, d}};
            vkCmdCopyBufferToImage(C->up, C->ring, v->img, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
        }
    }
    C->up_used = 1; v->uploaded = 1;
}
static void tex_free(vtex *v) {   // released by the game: draws already recorded (and the one being recorded) may still use it
    pthread_mutex_lock(&g_gbmu);
    v->dead = g_fence + 1;
    if (g_ngb == g_cgb) { g_cgb = g_cgb ? 2 * g_cgb : 64; g_gb = realloc(g_gb, g_cgb * sizeof *g_gb); }
    g_gb[g_ngb++] = v;
    pthread_mutex_unlock(&g_gbmu);
}
static void garbage_free(uint64_t done) {
    pthread_mutex_lock(&g_gbmu);
    for (int i = 0; i < g_ngb;) {
        vtex *v = g_gb[i];
        if (v->dead > done) { i++; continue; }
        pthread_mutex_lock(&g_rmu);   // out of the table now (mtl_tex_release left it there)
        for (vtex **pp = &g_tex[v->serial % NHASH]; *pp; pp = &(*pp)->next) if (*pp == v) { *pp = v->next; break; }
        pthread_mutex_unlock(&g_rmu);
        for (int k = 0; k < v->nviews; k++) vkDestroyImageView(g_dev, v->views[k].v, NULL);
        if (!v->evicted) { vkDestroyImage(g_dev, v->img, NULL); img_mem_free(v->blk, v->m, v->moff, v->msize); }   // an evicted one gave its image and memory back already
        free(v->views); free(v);
        g_gb[i] = g_gb[--g_ngb];
    }
    pthread_mutex_unlock(&g_gbmu);
}
// A view of the image: type and format from the NVN view (or the texture), levels and layers, swizzle (the format's missing alpha, the texture's and the view's swizzles)
static VkImageView tex_view(vtex *v, const tex_t *t, const view_t *vw, int has_vw, int want_type, int rt) {
    int target = has_vw && vw->set_target ? vw->target : t->d.target;
    int fmtn = has_vw && vw->set_format ? vw->format : t->d.format;
    uint32_t bl = has_vw && vw->set_levels ? vw->base_level : 0, nl = has_vw && vw->set_levels ? vw->nlevels : v->levels - bl;
    uint32_t b0 = has_vw && vw->set_layers ? vw->min_layer : 0, nly = has_vw && vw->set_layers ? vw->nlayers : v->layers - b0;
    if (rt) nl = 1;   // rt 1: a render target's view; rt 2: a storage image's (one level, no swizzle, its own type); rt 3: a layered render target's (every layer from its first on)
    if (rt == 3) nly = v->layers - (b0 < v->layers ? b0 : 0);
    if (!nl || bl + nl > v->levels) nl = v->levels - (bl < v->levels ? bl : 0);
    if (!nly || b0 + nly > v->layers) nly = v->layers - (b0 < v->layers ? b0 : 0);
    fmt_t f = fmt_of(fmtn);
    if (!f.f || f.dec != v->f.dec || (f.dec && f.f != v->f.f) || fmt_block(f.f) != fmt_block(v->f.f)) f = v->f;   // a block-compressed image is viewed in its own class only
    if (v->f.depth || f.depth) f = v->f;   // a depth-stencil image is viewed in its own format (a STENCIL8 view of a D32S8 one included): the aspect picks depth or stencil
    if (f.dec) f.f = v->f.f;
    if (v->nvfl && f.f != v->vfl[0] && (v->nvfl < 2 || f.f != v->vfl[1])) {   // outside the image's format list (DM_VK_UBWC): its own format
        static int nl; if (nl++ < 8) LOG("[RED] vk: a view in format %d of an image made for %d%s (DM_VK_UBWC): viewed in the image's format", (int)f.f, (int)v->vfl[0], v->nvfl > 1 ? " and its sRGB twin" : "");
        g_ubwc_fallback++; f = v->f; }
    uint8_t swz[4] = {2, 3, 4, 5};
    if (!rt) {
        if (t->set_swz) for (int i = 0; i < 4; i++) swz[i] = (uint8_t)(t->swz >> (3 * i) & 7);
        if (has_vw && vw->set_swizzle) memcpy(swz, vw->swz, 4);
        if (f.noalpha) for (int i = 0; i < 4; i++) if (swz[i] == 5) swz[i] = 1;
    }
    VkImageViewType vt;
    if (want_type >= 0) vt = (VkImageViewType)want_type;
    else switch (target) {
        case NVN_TEXTURE_TARGET_1D: vt = VK_IMAGE_VIEW_TYPE_1D; break; case NVN_TEXTURE_TARGET_1D_ARRAY: vt = VK_IMAGE_VIEW_TYPE_1D_ARRAY; break;
        case NVN_TEXTURE_TARGET_3D: vt = VK_IMAGE_VIEW_TYPE_3D; break;
        case NVN_TEXTURE_TARGET_2D_ARRAY: case NVN_TEXTURE_TARGET_2D_MULTISAMPLE_ARRAY: vt = VK_IMAGE_VIEW_TYPE_2D_ARRAY; break;
        case NVN_TEXTURE_TARGET_CUBEMAP: vt = VK_IMAGE_VIEW_TYPE_CUBE; break; case NVN_TEXTURE_TARGET_CUBEMAP_ARRAY: vt = VK_IMAGE_VIEW_TYPE_CUBE_ARRAY; break;
        default: vt = VK_IMAGE_VIEW_TYPE_2D; break;
    }
    // A render target is a 2D view of its layer (a 2D array for several): Vulkan takes no cube or 3D view as an attachment. A cube face drawn to was a cube view of 6 layers
    // from that face on, past the image's end for faces 1-5 (Super Mario Odyssey's environment probes, its first frame in a kingdom: the Mali-G57 lost the device). A 3D
    // texture's slice is a 2D view of the image made 2D-array compatible, its slice as the layer
    if (rt == 1 || rt == 3) {
        if (target == NVN_TEXTURE_TARGET_3D) { uint32_t dd = v->d >> bl ? v->d >> bl : 1; if (b0 >= dd) b0 = 0; nly = rt == 3 ? dd - b0 : 1; vt = rt == 3 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D; }
        else if (vt == VK_IMAGE_VIEW_TYPE_CUBE || vt == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY || vt == VK_IMAGE_VIEW_TYPE_2D_ARRAY || rt == 3) vt = nly > 1 || rt == 3 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    }
    if (vt == VK_IMAGE_VIEW_TYPE_2D && nly > 1) nly = 1;
    if (vt == VK_IMAGE_VIEW_TYPE_CUBE) { nly = 6; if (b0 + 6 > v->layers) b0 = v->layers >= 6 ? v->layers - 6 : 0; }
    if (vt == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY) { nly -= nly % 6; if (!nly) { vt = VK_IMAGE_VIEW_TYPE_CUBE; nly = 6; b0 = 0; } }
    // Sampling a depth-stencil image reads depth, or its stencil where the texture or the view asks for it (DEPTH_STENCIL_MODE_STENCIL): Unreal's CopyStencilToLightingChannelsPS
    // reads the lighting channel bits from the stencil, and through the depth aspect every light lost its channels (FINAL FANTASY RESONANCE's fields drawn black)
    int want_stencil = has_vw && vw->set_dsmode ? vw->dsmode == NVN_TEXTURE_DEPTH_STENCIL_MODE_STENCIL : t->stencil;
    VkImageAspectFlags asp = v->aspect;
    if (asp & VK_IMAGE_ASPECT_DEPTH_BIT && rt != 1 && rt != 3) asp = want_stencil && (asp & VK_IMAGE_ASPECT_STENCIL_BIT) ? VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_DEPTH_BIT;
    if (want_stencil && rt != 1 && rt != 3 && !(v->aspect & VK_IMAGE_ASPECT_STENCIL_BIT)) RED_ONCE(74, "a stencil view of texture format %#x, which has no stencil here: reads depth", fmtn);
    // The stencil where the shader looks for it: the Switch's GPU returns a stencil-mode read with the stencil in the second component (Unreal's STENCIL_COMPONENT_SWIZZLE .g on the
    // Switch), and the translated shader takes that component's raw bits; a Vulkan stencil view has it in the first, the second reads 0. Unreal's CopyStencilToLightingChannelsPS
    // ((stencil >> 4) & lighting channel bits, from .y) wrote no lighting channel anywhere, and what a channel-limited light should light stayed dark (FINAL FANTASY RESONANCE's battle
    // sprites as dark silhouettes). The second component now reads the first (an identity swizzle {R, R, B, A})
    if (asp == VK_IMAGE_ASPECT_STENCIL_BIT && !rt) for (int i = 0; i < 4; i++) if (swz[i] == 3) swz[i] = 2;
    uint64_t key = (uint64_t)f.f | (uint64_t)vt << 16 | (uint64_t)bl << 20 | (uint64_t)nl << 25 | (uint64_t)b0 << 30 | (uint64_t)nly << 42 | (uint64_t)(swz[0] | swz[1] << 3 | swz[2] << 6 | swz[3] << 9) << 50 | (uint64_t)(rt == 2 || rt == 3) << 62 | (uint64_t)(rt == 1 || rt == 3) << 63;
    if (asp == VK_IMAGE_ASPECT_STENCIL_BIT) key = ~key;   // the stencil aspect's views apart from the depth ones
    for (int i = 0; i < v->nviews; i++) if (v->views[i].key == key) return v->views[i].v;
    VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, v->img, vt, f.f,
                                {(VkComponentSwizzle)(swz[0] + 1), (VkComponentSwizzle)(swz[1] + 1), (VkComponentSwizzle)(swz[2] + 1), (VkComponentSwizzle)(swz[3] + 1)}, {asp, bl, nl, b0, nly}};
    VkImageView view; VkResult r = vkCreateImageView(g_dev, &vi, NULL, &view);
    if (r != VK_SUCCESS) { static int n; if (n++ < 8) LOG("[RED] vk: image view (format %d type %d levels %u+%u layers %u+%u) failed (%d)", f.f, vt, bl, nl, b0, nly, (int)r); return VK_NULL_HANDLE; }
    v->views = realloc(v->views, (v->nviews + 1) * sizeof *v->views); v->views[v->nviews++] = (vview){key, view};
    return view;
}

// ---------- Registrations (game threads) ----------
typedef struct { tex_t t; view_t v; uint8_t ok, has_v; } treg_t;
static treg_t *g_treg; static int g_ntreg;
static samp_par *g_sreg; static uint8_t *g_sreg_ok; static int g_nsreg;
void mtl_reg_tex(int id, const tex_t *t, const view_t *v) {
    if (id < 0) return;
    pthread_mutex_lock(&g_rmu);
    if (id >= g_ntreg) { int n = id + 1024; g_treg = realloc(g_treg, n * sizeof *g_treg); memset(g_treg + g_ntreg, 0, (n - g_ntreg) * sizeof *g_treg); g_ntreg = n; }
    g_treg[id].t = *t; g_treg[id].ok = 1; g_treg[id].has_v = v != NULL; if (v) g_treg[id].v = *v;
    int dead = t->magic != MAGIC('T', 'E', 'X', 0); if (dead) g_treg_dead++;   // kept as the game registered it; tex_get never makes an image of it (the slot reads the black stand-in, as on the Metal host)
    pthread_mutex_unlock(&g_rmu);
    if (dead) { static int n; if (n++ < 4) { const uint64_t *q = (const uint64_t *)t; LOG("vk: texture ID %d registered with an object that is not a texture (finalized or reused memory; starts %016llx %016llx): its slot reads black, as on the Metal host",
                                                                                    id, (unsigned long long)q[0], (unsigned long long)q[1]); } }
}
void mtl_reg_samp(int id, const samp_par *p) {
    if (id < 0) return;
    pthread_mutex_lock(&g_rmu);
    if (id >= g_nsreg) { int n = id + 256; g_sreg = realloc(g_sreg, n * sizeof *g_sreg); g_sreg_ok = realloc(g_sreg_ok, n); memset(g_sreg_ok + g_nsreg, 0, n - g_nsreg); g_nsreg = n; }
    g_sreg[id] = *p; g_sreg_ok[id] = 1;
    pthread_mutex_unlock(&g_rmu);
}
void mtl_set_tex_pool_n(int n) { (void)n; }
void mtl_tex_init(const tex_t *t) { (void)t; }
// Released by the game (TextureFinalize): it stays findable by its serial until it is freed (garbage_free), as commands recorded before the release and replayed after it -- an
// Unreal mip-streaming copy out of the texture being replaced -- still name it. Taken out of the table at once, such a copy found no image, made a new one from the texture's
// storage, which lacks what GPU copies put into the image, and copied black into the new texture's lower levels (FINAL FANTASY RESONANCE's corridor: dark columns at a distance)
void mtl_tex_release(tex_t *t) {
    pthread_mutex_lock(&g_rmu);
    vtex *v = tex_find(t->serial); int fresh = v && !v->dead;
    pthread_mutex_unlock(&g_rmu);
    if (fresh) tex_free(v);
}
void mtl_tex_cpu_written(const tex_t *t) {
    pthread_mutex_lock(&g_rmu);
    vtex *v = tex_find(t->serial); if (v) v->dirty = 1;
    if (!cpu_written(t->serial)) { if (g_ncpu == g_ccpu) { g_ccpu = g_ccpu ? 2 * g_ccpu : 64; g_cpu_written = realloc(g_cpu_written, g_ccpu * sizeof *g_cpu_written); } g_cpu_written[g_ncpu++] = t->serial; }
    pthread_mutex_unlock(&g_rmu);
}
void mtl_prog_release(prog_t *p) { (void)p; }

// Samplers, cached by their parameters
typedef struct { samp_par p; VkSampler s; } scache;
static scache *g_sc; static int g_nsc;
static VkSamplerAddressMode wrap(int w) {
    switch (w) { case 1: return VK_SAMPLER_ADDRESS_MODE_REPEAT; case 6: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT; case 4: case 5: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
                 case 2: case 3: return VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE; default: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE; }
}
static VkSampler samp_get(int sid) {
    if (sid <= 0 || sid >= g_nsreg || !g_sreg_ok[sid]) return g_dummy_samp;
    samp_par p = g_sreg[sid];
    static struct { int sid; samp_par p; VkSampler s; } dm[1024];   // by sampler ID, checked against its parameters (an ID can be registered again with others)
    if (dm[sid % 1024].sid == sid && dm[sid % 1024].s && !memcmp(&dm[sid % 1024].p, &p, sizeof p)) return dm[sid % 1024].s;
    for (int i = 0; i < g_nsc; i++) if (!memcmp(&g_sc[i].p, &p, sizeof p)) { dm[sid % 1024].sid = sid; dm[sid % 1024].p = p; dm[sid % 1024].s = g_sc[i].s; return g_sc[i].s; }
    VkSamplerCreateInfo si = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = p.magf ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    si.minFilter = p.minf == 1 || p.minf == 3 || p.minf == 5 ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    si.mipmapMode = p.minf == 4 || p.minf == 5 ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = wrap(p.ws); si.addressModeV = wrap(p.wt); si.addressModeW = wrap(p.wr);
    si.mipLodBias = p.lod_bias; si.minLod = p.minf >= 2 ? p.lod_min : 0; si.maxLod = p.minf >= 2 ? p.lod_max : 0.25f;
    if (p.aniso > 1) { si.anisotropyEnable = VK_TRUE; si.maxAnisotropy = p.aniso > 16 ? 16 : p.aniso; }
    if (p.cmp_mode) { si.compareEnable = VK_TRUE; si.compareOp = (VkCompareOp)(p.cmp_func ? p.cmp_func - 1 : 7); }
    int white = p.border[0] + p.border[1] + p.border[2] > 1.5f, opaque = p.border[3] > 0.5f;
    si.borderColor = white ? VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE : opaque ? VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK : VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    VkSampler s; if (vkCreateSampler(g_dev, &si, NULL, &s) != VK_SUCCESS) return g_dummy_samp;
    g_sc = realloc(g_sc, (g_nsc + 1) * sizeof *g_sc); g_sc[g_nsc++] = (scache){p, s};
    return s;
}

// ---------- Shaders and pipelines ----------
enum { BT_UBO, BT_SSBO, BT_CIS, BT_TBUF, BT_UTB, BT_STB, BT_SIMG, BT_TEX, BT_SAMP };
enum { K_UNK, K_CBUF, K_C1, K_SUPPORT, K_DRVCB, K_TEX, K_IMG, K_SBUF, K_TBUF, K_UBOP, K_TEXU };   // K_UBOP: a compute uniform buffer read through its address (dmsh_build.py ubo_ptr), bound as storage;
                                                                                                // K_TEXU: a bindless texture handle in uniform slot reg at byte off (tex_ubo)
enum { S_NONE, S_SAMP, S_SAMP_TEX, S_SAMP_ID, S_SAMP_UBO };   // S_SAMP_UBO: the sampler's handle in uniform slot sreg at byte soff (samp_ubo)
typedef struct { uint32_t binding, count; uint8_t type, kind, skind; int16_t reg, sreg, off, soff; } sbind;
typedef struct {
    char key[17]; int stage;   // 0 vertex, 1 fragment, 5 compute (NVN stage numbers)
    VkShaderModule mod, mod_dual; VkDescriptorSetLayout dsl;   // mod_dual: a fragment shader's output 1 as the second source of dual-source blending (spv_dual)
    int dual_tried;
    sbind b[64]; int nb; uint8_t vin[32]; uint32_t vin_mask;   // vin: component type per vertex input location (0 float, 1 uint, 2 int)
    uint8_t *c1; uint32_t c1_len; uint32_t tg[3];
    VkPipelineLayout cpl; VkPipeline cpp;   // compute shaders: their layout and pipeline (dispatch)
    uint8_t dynu[64];   // uniform buffer bindings made dynamic (UNIFORM_BUFFER_DYNAMIC): their ring offset goes with the bind, so a set stays the same while only uniform data changes
    int layer_out;   // writes the Layer builtin (a vertex or geometry shader that picks the attachment layer per primitive): its draws run in a layered pass
    int vid;   // a vertex shader that reads VertexIndex, patched to add the push constant at offset 0 (spv_vid_bias): its layout has the range, draw() pushes vlo
    uint8_t md[8];   // the key as bytes (shader_get's lookup)
    int dyn;   // dynamic slot arrays (profile key tex_dyn_slots): 32-entry texture / texel buffer / sampler arrays indexed at run time by the IDs the shader reads from c2 (see write_set)
    int ssbo_wr;   // the module stores to or atomically updates a storage buffer (spv_ssbo_writes): only then is a draw's storage buffer a write that gets lost
} shader_t;
static shader_t **g_sh; static int g_nsh;
static int g_depth_zero;   // NVN depth mode (mtl_set_depth_zero, defined with the replay state)
static const char *g_vkdir;
static uint32_t *read_file(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint32_t *p = malloc((size_t)sz + 1); if (fread(p, 1, (size_t)sz, f) != (size_t)sz) { free(p); fclose(f); return NULL; }
    fclose(f); ((char *)p)[sz] = 0; *n = (size_t)sz; return p;
}
// NVN's GL depth convention (nvnDeviceSetDepthMode near = -w: Metroid Dread, Super Mario Odyssey...): clip-space z runs -w..w, Vulkan's 0..w. A vertex shader then ends with
// z' = (z + w) / 2 -- the GL entry point dmsh_build.py adds to the Metal libraries (vs_depth), done here on the SPIR-V: before every return of the entry point the position
// (a BuiltIn Position variable, or member of an output block) is read, rewritten and stored back. Returns a new word array (the caller frees it), NULL when no position is written
// Position is decorated Invariant in every vertex shader: games draw a depth prepass with one vertex shader and the scene with another under the depth test EQUAL (Metroid Dread's
// G-buffer pass), which on the Switch matches bit for bit; without the decoration the Mali compiler evaluated the two positions differently and the whole level failed the test
static uint32_t *spv_invariant(const uint32_t *w, size_t nw, size_t *out_n) {
    enum { OpDecorate = 71, OpMemberDecorate = 72, BuiltIn = 11, Position = 0, Invariant = 18 };
    for (size_t i = 5; i < nw;) {   // already invariant: nothing to add
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16; if (!len) return NULL;
        if ((op == OpDecorate && len >= 3 && w[i + 2] == Invariant) || (op == OpMemberDecorate && len >= 4 && w[i + 3] == Invariant)) return NULL;
        i += len;
    }
    uint32_t *o = malloc((nw + 8) * 4); size_t n = 0; int added = 0;
    memcpy(o, w, 20); n = 5;
    for (size_t i = 5; i < nw;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        memcpy(o + n, w + i, len * 4); n += len;
        if (added < 2 && op == OpMemberDecorate && len >= 5 && w[i + 3] == BuiltIn && w[i + 4] == Position) { o[n++] = 4u << 16 | OpMemberDecorate; o[n++] = w[i + 1]; o[n++] = w[i + 2]; o[n++] = Invariant; added++; }
        else if (added < 2 && op == OpDecorate && len >= 4 && w[i + 2] == BuiltIn && w[i + 3] == Position) { o[n++] = 3u << 16 | OpDecorate; o[n++] = w[i + 1]; o[n++] = Invariant; added++; }
        i += len;
    }
    if (!added) { free(o); return NULL; }
    *out_n = n; return o;
}
// PointSize = 1.0 stored first thing in every vertex shader: Vulkan leaves a point's size undefined unless the vertex shader writes it, and the translated shaders write it only when
// the Switch shader did (NVN then takes SetPointSize). Super Mario Odyssey draws points with shaders that do not, and the Mali-G57 lost the device at its first such draw. A shader that
// writes its own size later overrides the 1.0; topologies other than points ignore it
// Dual-source blending: NVN's pixel shader writes the second source as its output 1, and a blend factor SRC1_* reads it; Vulkan reads the second source from the output at
// location 0, index 1 (as Metal's [[color(0), index(1)]], dmsh_build.py fs_dual). A copy of the module whose location-1 output variable is moved there (its Location
// decoration made 0, an Index 1 decoration added); NULL when the shader has no location-1 output
static uint32_t *spv_dual(const uint32_t *w, size_t nw, size_t *out_n) {
    enum { OpVariable = 59, OpDecorate = 71, Output = 3, Location = 30, Index = 32 };
    size_t at = 0;
    for (size_t i = 5; i < nw && !at;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16; if (!len) return NULL;
        if (op == OpDecorate && len >= 4 && w[i + 2] == Location && w[i + 3] == 1)
            for (size_t j = 5; j < nw;) { uint32_t o2 = w[j] & 0xffff, l2 = w[j] >> 16; if (!l2) return NULL; if (o2 == OpVariable && l2 >= 4 && w[j + 2] == w[i + 1] && w[j + 3] == Output) { at = i; break; } j += l2; }
        i += len;
    }
    if (!at) return NULL;
    uint32_t *o = malloc((nw + 4) * 4); if (!o) return NULL;
    memcpy(o, w, (at + 4) * 4); o[at + 3] = 0;
    o[at + 4] = 4u << 16 | OpDecorate; o[at + 5] = w[at + 1]; o[at + 6] = Index; o[at + 7] = 1;
    memcpy(o + at + 8, w + at + 4, (nw - at - 4) * 4);
    *out_n = nw + 4; return o;
}
static void spv_set(uint32_t *spv, size_t nw, uint32_t set) {   // every resource into descriptor set `set` (DescriptorSet decorations, operand 34): the fragment stage's go to set 1
    for (size_t i = 5; i < nw;) { uint32_t w = spv[i], op = w & 0xffff, len = w >> 16; if (!len) break; if (op == 71 && len >= 4 && spv[i + 2] == 34) spv[i + 3] = set; i += len; }
}
static VkShaderModule fs_dual_mod_(shader_t *fs);
static pthread_mutex_t g_dualmu = PTHREAD_MUTEX_INITIALIZER;
static VkShaderModule fs_dual_mod(shader_t *fs) { pthread_mutex_lock(&g_dualmu); VkShaderModule m = fs_dual_mod_(fs); pthread_mutex_unlock(&g_dualmu); return m; }
static VkShaderModule fs_dual_mod_(shader_t *fs) {   // made at the first pipeline that blends with the second source, from the shader file read again
    if (fs->mod_dual || fs->dual_tried) return fs->mod_dual;
    fs->dual_tried = 1;
    char path[1024]; size_t ns = 0, n; snprintf(path, sizeof path, "%s/%s.spv", g_vkdir, fs->key);
    uint32_t *spv = read_file(path, &ns), *w = spv ? spv_dual(spv, ns / 4, &n) : NULL; free(spv);
    if (!w) { LOG("[RED] vk: pixel shader %s: a pipeline blends with the second source, but the shader writes no output 1", fs->key); return VK_NULL_HANDLE; }
    spv_set(w, n, 1);
    VkShaderModuleCreateInfo mi = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, n * 4, w};
    if (vkCreateShaderModule(g_dev, &mi, NULL, &fs->mod_dual) != VK_SUCCESS) { LOG("[RED] vk: pixel shader %s: its dual-source module was rejected", fs->key); fs->mod_dual = VK_NULL_HANDLE; }
    else LOG("vk: pixel shader %s: output 1 is the second source of dual-source blending", fs->key);
    free(w);
    return fs->mod_dual;
}
static uint32_t *spv_point_size(const uint32_t *w, size_t nw, size_t *out_n) {
    enum { OpEntryPoint = 15, OpTypeInt = 21, OpTypeFloat = 22, OpTypePointer = 32, OpConstant = 43, OpFunction = 54, OpLabel = 248, OpVariable = 59, OpStore = 62, OpAccessChain = 65,
           OpDecorate = 71, OpMemberDecorate = 72, Output = 3, BuiltIn = 11, PointSize = 1 };
    uint32_t bound = w[3], entry = 0, f32 = 0, u32 = 0, ps_struct = 0, ps_member = 0, ps_var = 0, var = 0; int member_path = 0; size_t first_fn = 0;
    for (size_t i = 5; i < nw;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16; if (!len) return NULL;
        if (op == OpEntryPoint && !entry) entry = w[i + 2];
        else if (op == OpMemberDecorate && len >= 5 && w[i + 3] == BuiltIn && w[i + 4] == PointSize) { ps_struct = w[i + 1]; ps_member = w[i + 2]; member_path = 1; }
        else if (op == OpDecorate && len >= 4 && w[i + 2] == BuiltIn && w[i + 3] == PointSize) ps_var = w[i + 1];
        else if (op == OpTypeFloat && w[i + 2] == 32) f32 = w[i + 1];
        else if (op == OpTypeInt && w[i + 2] == 32 && !w[i + 3] && !u32) u32 = w[i + 1];
        else if (op == OpFunction && !first_fn) first_fn = i;
        i += len;
    }
    if (!entry || !f32 || !first_fn || (!member_path && !ps_var) || (member_path && !u32)) return NULL;
    for (size_t i = 5; i < first_fn;) {   // the Output variable holding PointSize (the per-vertex block, or the variable itself)
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (op == OpVariable && w[i + 3] == Output) {
            if (!member_path && w[i + 2] == ps_var) var = ps_var;
            if (member_path) for (size_t j = 5; j < first_fn;) { uint32_t o2 = w[j] & 0xffff, l2 = w[j] >> 16; if (o2 == OpTypePointer && w[j + 1] == w[i + 1] && w[j + 3] == ps_struct) var = w[i + 2]; j += l2; }
        }
        i += len;
    }
    if (!var) return NULL;
    // the entry function's first block: after its OpLabel and OpVariables
    size_t at = 0;
    for (size_t i = first_fn; i < nw;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (op == OpFunction && w[i + 2] != entry) { i += len; continue; }
        if (op == OpFunction) { size_t j = i + len; while (j < nw && (w[j] & 0xffff) != OpLabel) j += w[j] >> 16; j += w[j] >> 16; while (j < nw && (w[j] & 0xffff) == OpVariable) j += w[j] >> 16; at = j; break; }
        i += len;
    }
    if (!at) return NULL;
    uint32_t ptr = 0, one = bound++, cidx = bound++, ac = bound++;   // the Output float pointer type is reused when declared (a second declaration of it is invalid)
    for (size_t i = 5; i < first_fn;) { uint32_t op = w[i] & 0xffff, len = w[i] >> 16; if (op == OpTypePointer && w[i + 2] == Output && w[i + 3] == f32) ptr = w[i + 1]; i += len; }
    int new_ptr = !ptr; if (new_ptr) ptr = bound++;
    uint32_t *o = malloc((nw + 32) * 4); size_t n = 0;
    memcpy(o, w, first_fn * 4); n = first_fn;
    if (new_ptr) { o[n++] = 4u << 16 | OpTypePointer; o[n++] = ptr; o[n++] = Output; o[n++] = f32; }
    o[n++] = 4u << 16 | OpConstant; o[n++] = f32; o[n++] = one; o[n++] = 0x3f800000;
    if (member_path) { o[n++] = 4u << 16 | OpConstant; o[n++] = u32; o[n++] = cidx; o[n++] = ps_member; }
    memcpy(o + n, w + first_fn, (at - first_fn) * 4); n += at - first_fn;
    if (member_path) { o[n++] = 5u << 16 | OpAccessChain; o[n++] = ptr; o[n++] = ac; o[n++] = var; o[n++] = cidx; o[n++] = 3u << 16 | OpStore; o[n++] = ac; o[n++] = one; }
    else { o[n++] = 3u << 16 | OpStore; o[n++] = var; o[n++] = one; }
    memcpy(o + n, w + at, (nw - at) * 4); n += nw - at;
    o[3] = bound; *out_n = n; return o;
}
// The vertex index a shader reads, as the game gave it. draw() copies only the vertices a draw uses (from vertex vlo on) and shifts the draw's vertex numbers down by vlo,
// so VertexIndex came out vlo too low. Unreal's vertex shaders here fetch everything but the position from texel buffers by VertexIndex (manual vertex fetch): every mesh
// section whose indices do not start at 0 took another vertex's UVs and tangents (FINAL FANTASY RESONANCE's castle: the upper walls dark, smeared and lit as if facing down,
// the grand staircase dark and jagged; the Metal host binds the pool in place, no shift). Every load of the VertexIndex variable gets a 4-byte push constant (offset 0,
// vertex stage) added, which draw() sets to vlo. Returns a new word array (the caller frees it), NULL when the shader does not read VertexIndex
// DM_VK_VTXIDX_FIX=0 leaves the shaders as they were (VertexIndex vlo too low). g_vid_push / g_vid_nz: draws that pushed the bias, and those whose bias was not 0 (the recording line)
static uint64_t g_vid_push, g_vid_nz;
static int vid_fix_on(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_VK_VTXIDX_FIX"); v = !(e && !strcmp(e, "0")); if (!v) LOG("DM_VK_VTXIDX_FIX=0: vertex shaders read VertexIndex shifted by the draw's first vertex copied"); } return v; }
static uint32_t *spv_vid_bias(const uint32_t *w, size_t nw, size_t *out_n) {
    enum { OpEntryPoint = 15, OpTypeInt = 21, OpTypeStruct = 30, OpTypePointer = 32, OpConstant = 43, OpFunction = 54, OpVariable = 59, OpLoad = 61, OpAccessChain = 65,
           OpDecorate = 71, OpMemberDecorate = 72, OpName = 5, OpIAdd = 128, Input = 1, PushConstant = 9, BuiltIn = 11, VertexIndex = 42, Block = 2, Offset = 35 };
    uint32_t bound = w[3], var = 0, ptr = 0, ty = 0; size_t first_fn = 0, dec_at = 0, ep_at = 0; int nload = 0;
    for (size_t i = 5; i < nw;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16; if (!len || i + len > nw) return NULL;
        if (op == OpDecorate && len >= 4 && w[i + 2] == BuiltIn && w[i + 3] == VertexIndex) { if (var) return NULL; var = w[i + 1]; dec_at = i + len; }
        else if (op == OpMemberDecorate && len >= 5 && w[i + 3] == BuiltIn && w[i + 4] == VertexIndex) return NULL;   // in an input block: not handled (not seen)
        else if (op == OpEntryPoint && !ep_at) ep_at = i;
        else if (op == OpFunction && !first_fn) first_fn = i;
        i += len;
    }
    if (!var || !first_fn || !dec_at || !ep_at) return NULL;
    for (size_t i = 5; i < nw;) {   // the variable's pointer type and the integer it holds; the translator only ever loads it
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (op == OpVariable && len >= 4 && w[i + 2] == var && w[i + 3] == Input) ptr = w[i + 1];
        else if (op == OpLoad && len >= 4 && w[i + 3] == var) nload++;
        i += len;
    }
    for (size_t i = 5; i < first_fn && ptr;) { uint32_t op = w[i] & 0xffff, len = w[i] >> 16; if (op == OpTypePointer && w[i + 1] == ptr) ty = w[i + 3]; i += len; }
    int is_int = 0; for (size_t i = 5; i < first_fn && ty;) { uint32_t op = w[i] & 0xffff, len = w[i] >> 16; if (op == OpTypeInt && w[i + 1] == ty && w[i + 2] == 32) is_int = 1; i += len; }
    if (!ptr || !is_int || !nload) return NULL;
    uint32_t st = bound++, pst = bound++, pvar = bound++, pty = bound++, cz = bound++;
    int add_if = w[1] >= 0x10400;   // SPIR-V 1.4 on: the entry point lists every global variable it uses
    uint32_t *o = malloc((nw + 40 + (size_t)nload * 13) * 4); if (!o) return NULL; size_t n = 0;
    for (size_t i = 0; i < 5; i++) o[n++] = w[i];
    for (size_t i = 5; i < nw;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (i == first_fn) {
            o[n++] = 3u << 16 | OpTypeStruct; o[n++] = st; o[n++] = ty;
            o[n++] = 4u << 16 | OpTypePointer; o[n++] = pst; o[n++] = PushConstant; o[n++] = st;
            o[n++] = 4u << 16 | OpVariable; o[n++] = pst; o[n++] = pvar; o[n++] = PushConstant;
            o[n++] = 4u << 16 | OpTypePointer; o[n++] = pty; o[n++] = PushConstant; o[n++] = ty;
            o[n++] = 4u << 16 | OpConstant; o[n++] = ty; o[n++] = cz; o[n++] = 0;
        }
        if (i == dec_at) { o[n++] = 3u << 16 | OpDecorate; o[n++] = st; o[n++] = Block; o[n++] = 5u << 16 | OpMemberDecorate; o[n++] = st; o[n++] = 0; o[n++] = Offset; o[n++] = 0; }
        if (op == OpEntryPoint && i == ep_at && add_if) { o[n++] = (len + 1) << 16 | op; memcpy(o + n, w + i + 1, (len - 1) * 4); n += len - 1; o[n++] = pvar; i += len; continue; }
        if (op == OpLoad && len >= 4 && w[i + 3] == var) {   // r = load(var) becomes t = load(var); b = load(push constant); r = t + b
            uint32_t r = w[i + 2], t = bound++, ac = bound++, b = bound++;
            memcpy(o + n, w + i, len * 4); o[n + 2] = t; n += len;
            o[n++] = 5u << 16 | OpAccessChain; o[n++] = pty; o[n++] = ac; o[n++] = pvar; o[n++] = cz;
            o[n++] = 4u << 16 | OpLoad; o[n++] = ty; o[n++] = b; o[n++] = ac;
            o[n++] = 5u << 16 | OpIAdd; o[n++] = ty; o[n++] = r; o[n++] = t; o[n++] = b;
            i += len; continue;
        }
        memcpy(o + n, w + i, len * 4); n += len; i += len;
    }
    o[3] = bound; *out_n = n; return o;
}
static uint32_t *spv_gl_depth(const uint32_t *w, size_t nw, size_t *out_n) {
    enum { OpEntryPoint = 15, OpTypeInt = 21, OpTypeFloat = 22, OpTypeVector = 23, OpTypePointer = 32, OpConstant = 43, OpFunction = 54, OpVariable = 59, OpLoad = 61, OpStore = 62,
           OpAccessChain = 65, OpDecorate = 71, OpMemberDecorate = 72, OpCompositeExtract = 81, OpCompositeInsert = 82, OpFAdd = 129, OpFMul = 133, OpReturn = 253, Output = 3 };
    uint32_t bound = w[3], entry = 0, f32 = 0, v4 = 0, u32 = 0, pos_struct = 0, pos_member = 0, pos_var = 0, ptr_v4 = 0, cidx = 0, var = 0; int member_path = 0;
    size_t first_fn = 0;
    for (size_t i = 5; i < nw;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16; if (!len) return NULL;
        if (op == OpEntryPoint && !entry) entry = w[i + 2];
        else if (op == OpMemberDecorate && len >= 5 && w[i + 3] == 11 && w[i + 4] == 0) { pos_struct = w[i + 1]; pos_member = w[i + 2]; member_path = 1; }
        else if (op == OpDecorate && len >= 4 && w[i + 2] == 11 && w[i + 3] == 0) pos_var = w[i + 1];
        else if (op == OpTypeFloat && w[i + 2] == 32) f32 = w[i + 1];
        else if (op == OpTypeInt && w[i + 2] == 32 && !u32) u32 = w[i + 1];
        else if (op == OpFunction && !first_fn) first_fn = i;
        i += len;
    }
    for (size_t i = 5; i < first_fn;) {   // the vector, pointer, index constant and variable, now the type IDs are known
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (op == OpTypeVector && w[i + 2] == f32 && w[i + 3] == 4) v4 = w[i + 1];
        i += len;
    }
    for (size_t i = 5; i < first_fn;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (op == OpTypePointer && w[i + 2] == Output && w[i + 3] == v4) ptr_v4 = w[i + 1];
        if (op == OpConstant && w[i + 1] == u32 && len == 4 && w[i + 3] == pos_member) cidx = w[i + 2];
        if (op == OpVariable && w[i + 3] == Output) {
            if (!member_path && w[i + 2] == pos_var) var = w[i + 2];
            if (member_path) for (size_t j = 5; j < first_fn;) { uint32_t o2 = w[j] & 0xffff, l2 = w[j] >> 16; if (o2 == OpTypePointer && w[j + 1] == w[i + 1] && w[j + 3] == pos_struct) var = w[i + 2]; j += l2; }
        }
        i += len;
    }
    if (!entry || !f32 || !v4 || !var || (member_path && !u32)) return NULL;
    uint32_t decl[16]; size_t nd = 0;
    if (!ptr_v4) { ptr_v4 = bound++; decl[nd++] = 4u << 16 | OpTypePointer; decl[nd++] = ptr_v4; decl[nd++] = Output; decl[nd++] = v4; }
    if (member_path && !cidx) { cidx = bound++; decl[nd++] = 4u << 16 | OpConstant; decl[nd++] = u32; decl[nd++] = cidx; decl[nd++] = pos_member; }
    uint32_t half = bound++; float hf = 0.5f; uint32_t hb; memcpy(&hb, &hf, 4);
    decl[nd++] = 4u << 16 | OpConstant; decl[nd++] = f32; decl[nd++] = half; decl[nd++] = hb;
    size_t nret = 0; for (size_t i = first_fn; i < nw; i += w[i] >> 16) if ((w[i] & 0xffff) == OpReturn) nret++;
    uint32_t *o = malloc((nw + nd + nret * 40) * 4); size_t n = 0; int in_entry = 0;
    for (size_t i = 0; i < 5; i++) o[n++] = w[i];
    for (size_t i = 5; i < nw;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (i == first_fn) { memcpy(o + n, decl, nd * 4); n += nd; }
        if (op == OpFunction) in_entry = w[i + 2] == entry;
        if (op == OpReturn && in_entry) {
            uint32_t p = var, ld = bound++, z = bound++, ww = bound++, sum = bound++, hz = bound++, nv = bound++;
            if (member_path) { p = bound++; o[n++] = 5u << 16 | OpAccessChain; o[n++] = ptr_v4; o[n++] = p; o[n++] = var; o[n++] = cidx; }
            o[n++] = 4u << 16 | OpLoad; o[n++] = v4; o[n++] = ld; o[n++] = p;
            o[n++] = 5u << 16 | OpCompositeExtract; o[n++] = f32; o[n++] = z; o[n++] = ld; o[n++] = 2;
            o[n++] = 5u << 16 | OpCompositeExtract; o[n++] = f32; o[n++] = ww; o[n++] = ld; o[n++] = 3;
            o[n++] = 5u << 16 | OpFAdd; o[n++] = f32; o[n++] = sum; o[n++] = z; o[n++] = ww;
            o[n++] = 5u << 16 | OpFMul; o[n++] = f32; o[n++] = hz; o[n++] = sum; o[n++] = half;
            o[n++] = 6u << 16 | OpCompositeInsert; o[n++] = v4; o[n++] = nv; o[n++] = hz; o[n++] = ld; o[n++] = 2;
            o[n++] = 3u << 16 | OpStore; o[n++] = p; o[n++] = nv;
        }
        memcpy(o + n, w + i, len * 4); n += len; i += len;
    }
    o[3] = bound; *out_n = n; return o;
}
// Whether a module writes a storage buffer: an OpStore or atomic whose pointer derives (access chains) from a StorageBuffer variable, or a Uniform one whose struct is a BufferBlock.
// Unreal binds read-only storage buffers to many vertex and pixel shaders (FINAL FANTASY RESONANCE: none of the eight that warned writes them)
static int spv_ssbo_writes(const uint32_t *w, size_t nw) {
    enum { OpTypePointer = 32, OpVariable = 59, OpStore = 62, OpAccessChain = 65, OpInBoundsAccessChain = 66, OpPtrAccessChain = 67, OpDecorate = 71, BufferBlock = 3 };
    uint32_t bound = nw > 3 ? w[3] : 0; if (!bound || bound > (1u << 22)) return 1;
    uint8_t *k = calloc(bound, 1); if (!k) return 1;   // bit 1 BufferBlock type, 2 pointer to a storage buffer, 4 derived from a storage buffer variable
    uint32_t *pt = calloc(bound, 4); int wr = 0;
    for (size_t i = 5; i < nw;) { uint32_t op = w[i] & 0xffff, n = w[i] >> 16; if (!n) break;
        if (op == OpDecorate && n >= 3 && w[i + 2] == BufferBlock && w[i + 1] < bound) k[w[i + 1]] |= 1;
        i += n; }
    for (size_t i = 5; i < nw && !wr;) { uint32_t op = w[i] & 0xffff, n = w[i] >> 16; if (!n) break;
        if (op == OpTypePointer && n >= 4 && w[i + 1] < bound && w[i + 3] < bound && (w[i + 2] == 12 || (w[i + 2] == 2 && (k[w[i + 3]] & 1)))) k[w[i + 1]] |= 2;
        else if (op == OpVariable && n >= 4 && w[i + 1] < bound && w[i + 2] < bound && (k[w[i + 1]] & 2)) k[w[i + 2]] |= 4;
        else if ((op == OpAccessChain || op == OpInBoundsAccessChain || op == OpPtrAccessChain) && n >= 4 && w[i + 2] < bound && w[i + 3] < bound && (k[w[i + 3]] & 4)) k[w[i + 2]] |= 4;
        else if (op == OpStore && n >= 3 && w[i + 1] < bound && (k[w[i + 1]] & 4)) wr = 1;
        else if (op >= 227 && op <= 242 && n >= 4 && w[i + 3] < bound && (k[w[i + 3]] & 4)) wr = 1;   // OpAtomic*: result type, result, pointer
        i += n; }
    free(k); free(pt); return wr;
}
// Shaders are loaded on the replay thread and on the pipeline pre-creation thread (pso_precreate): one lock for the table and the loads. Their time is the long-frame lines'
// "shader libraries" (mtl_create_ns): a first-time load reads <key>.spv and <key>.vk.txt from the shader folder (150,000 files in one directory on Android's shared storage)
static pthread_mutex_t g_shmu = PTHREAD_MUTEX_INITIALIZER; static _Atomic uint64_t g_sh_ns;
static shader_t *shader_get_(const uint8_t md[8]);
static shader_t *shader_get(const uint8_t md[8]) {
    pthread_mutex_lock(&g_shmu);
    for (int i = 0; i < g_nsh; i++) if (!memcmp(g_sh[i]->md, md, 8)) { shader_t *s = g_sh[i]; pthread_mutex_unlock(&g_shmu); return s; }
    uint64_t t0 = now_ns(); shader_t *s = shader_get_(md); memcpy(s->md, md, 8); atomic_fetch_add(&g_sh_ns, now_ns() - t0);
    pthread_mutex_unlock(&g_shmu); return s;
}
static shader_t *shader_get_(const uint8_t md[8]) {
    char key[17]; for (int i = 0; i < 8; i++) sprintf(key + 2 * i, "%02x", md[i]);
    for (int i = 0; i < g_nsh; i++) if (!strcmp(g_sh[i]->key, key)) return g_sh[i];
    if (!g_vkdir && !(g_vkdir = getenv("DM_VK_DIR"))) die("vk: DM_VK_DIR is not set (the folder of <key>.spv / <key>.vk.txt from dm/android/tools/spv_android.py)");
    char path[1024]; size_t ns, nt;
    snprintf(path, sizeof path, "%s/%s.spv", g_vkdir, key); uint32_t *spv = read_file(path, &ns);
    snprintf(path, sizeof path, "%s/%s.vk.txt", g_vkdir, key); char *txt = (char *)read_file(path, &nt);
    shader_t *s = calloc(1, sizeof *s); strcpy(s->key, key); s->stage = -1;
    g_sh = realloc(g_sh, (g_nsh + 1) * sizeof *g_sh); g_sh[g_nsh++] = s;
    if (!spv || !txt) { LOG("[RED] vk: shader %s not in %s", key, g_vkdir); free(spv); free(txt); return s; }
    for (char *ln = strtok(txt, "\n"); ln; ln = strtok(NULL, "\n")) {
        char a[16], ty[8], kind[16], sk[16]; int b, cnt, reg, off, sreg, soff = -1, l, t;
        if (sscanf(ln, "stage %15s", a) == 1) s->stage = !strcmp(a, "vs") ? 0 : !strcmp(a, "ps") ? 1 : !strcmp(a, "gs") ? 2 : !strcmp(a, "cs") ? 5 : -1;
        else if (sscanf(ln, "tg %u %u %u", &s->tg[0], &s->tg[1], &s->tg[2]) == 3) {}
        else if (!strncmp(ln, "c1 ", 3)) { size_t n = strlen(ln + 3) / 2; s->c1 = malloc(n + 16); s->c1_len = (uint32_t)n; for (size_t i = 0; i < n; i++) sscanf(ln + 3 + 2 * i, "%2hhx", &s->c1[i]); }
        else if (sscanf(ln, "vin %d %d", &l, &t) == 2 && l >= 0 && l < 32) { s->vin[l] = (uint8_t)t; s->vin_mask |= 1u << l; }
        else if (sscanf(ln, "b %d %7s %d %15s %d %d %15s %d %d", &b, ty, &cnt, kind, &reg, &off, sk, &sreg, &soff) >= 8 && s->nb < 64) {   // the 9th field, the sampler's byte offset, is newer
            sbind *e = &s->b[s->nb++]; e->binding = (uint32_t)b; e->count = cnt > 0 ? (uint32_t)cnt : 1; e->reg = (int16_t)reg; e->off = (int16_t)off; e->sreg = (int16_t)sreg; e->soff = (int16_t)soff;
            e->type = !strcmp(ty, "ubo") ? BT_UBO : !strcmp(ty, "ssbo") ? BT_SSBO : !strcmp(ty, "cis") ? BT_CIS : !strcmp(ty, "tbuf") ? BT_TBUF : !strcmp(ty, "utb") ? BT_UTB :
                      !strcmp(ty, "stb") ? BT_STB : !strcmp(ty, "simg") ? BT_SIMG : !strcmp(ty, "tex") ? BT_TEX : BT_SAMP;
            e->kind = !strcmp(kind, "cbuf") ? K_CBUF : !strcmp(kind, "c1") ? K_C1 : !strcmp(kind, "support") ? K_SUPPORT : !strcmp(kind, "drvcb") ? K_DRVCB : !strcmp(kind, "tex") ? K_TEX :
                      !strcmp(kind, "img") ? K_IMG : !strcmp(kind, "sbuf") ? K_SBUF : !strcmp(kind, "tbuf") ? K_TBUF : !strcmp(kind, "ubo_ptr") ? K_UBOP : !strcmp(kind, "tex_ubo") ? K_TEXU : K_UNK;
            e->skind = !strcmp(sk, "samp") ? S_SAMP : !strcmp(sk, "samp_tex") ? S_SAMP_TEX : !strcmp(sk, "samp_id") ? S_SAMP_ID : !strcmp(sk, "samp_ubo") ? S_SAMP_UBO : S_NONE;
#ifndef DM_GAME_TEX_DYN_SLOTS
#define DM_GAME_TEX_DYN_SLOTS 0
#endif
            int dynarr = DM_GAME_TEX_DYN_SLOTS && e->count > 1 && e->reg < 0 && (e->type == BT_UTB || e->type == BT_TBUF || e->type == BT_SAMP || e->type == BT_TEX || e->type == BT_CIS);
            if (dynarr) s->dyn = 1;
            else if (e->kind == K_UNK) LOG("[RED] vk: shader %s binding %d has kind %s, not implemented (reads zero)", key, b, kind);
            if (e->kind == K_TEXU && e->count > 1) LOG("[RED] vk: shader %s binding %d: an array of %u handles in a uniform buffer, not implemented (reads zero)", key, b, e->count);
        }
    }
    free(txt);
    s->ssbo_wr = spv_ssbo_writes(spv, ns / 4);
    for (size_t i = 5; i + 4 < ns / 4;) { uint32_t op = spv[i] & 0xffff, n = spv[i] >> 16; if (!n) break;   // BuiltIn Layer (9): OpDecorate / OpMemberDecorate
        if ((op == 71 && n >= 4 && spv[i + 2] == 11 && spv[i + 3] == 9) || (op == 72 && n >= 5 && spv[i + 3] == 11 && spv[i + 4] == 9)) s->layer_out = 1;
        i += n; }
    if (s->stage == 0 && vid_fix_on()) { size_t nn; uint32_t *w2 = spv_vid_bias(spv, ns / 4, &nn); if (w2) { free(spv); spv = w2; ns = nn * 4; s->vid = 1; } }   // VertexIndex as the game gave it (see spv_vid_bias)
    if (s->stage == 0) { size_t nn; uint32_t *w2 = spv_invariant(spv, ns / 4, &nn); if (w2) { free(spv); spv = w2; ns = nn * 4; } }
    if (s->stage == 0) { size_t nn; uint32_t *w2 = spv_point_size(spv, ns / 4, &nn); if (w2) { free(spv); spv = w2; ns = nn * 4; } }
    if (s->stage == 0 && !g_depth_zero) {   // NVN's GL depth convention (see spv_gl_depth)
        size_t nn; uint32_t *w2 = spv_gl_depth(spv, ns / 4, &nn);
        if (w2) { free(spv); spv = w2; ns = nn * 4; } else LOG("[RED] vk: vertex shader %s: no position written, left as it is for the GL depth convention", key);
    }
    spv_set(spv, ns / 4, s->stage == 1 ? 1 : s->stage == 2 ? 2 : 0);   // fragment resources in set 1, geometry in set 2
    VkShaderModuleCreateInfo mi = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, ns, spv};
    VkResult r = vkCreateShaderModule(g_dev, &mi, NULL, &s->mod); free(spv);
    if (r != VK_SUCCESS) { LOG("[RED] vk: shader %s rejected (%d)", key, (int)r); s->mod = VK_NULL_HANDLE; }
    VkDescriptorSetLayoutBinding lb[64]; VkShaderStageFlags sf = s->stage == 0 ? VK_SHADER_STAGE_VERTEX_BIT : s->stage == 1 ? VK_SHADER_STAGE_FRAGMENT_BIT : s->stage == 2 ? VK_SHADER_STAGE_GEOMETRY_BIT : VK_SHADER_STAGE_COMPUTE_BIT;
    static const VkDescriptorType DT[] = {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER,
                                          VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_DESCRIPTOR_TYPE_SAMPLER};
    // Uniform buffers the ring holds (the game's, c1, driver segments) are dynamic, as many as a third of the GPU's limit per pipeline layout allows (three sets), the game's first;
    // only when the bindings are in ascending order (dynamic offsets are given in binding order)
    { uint32_t lim = g_props.limits.maxDescriptorSetUniformBuffersDynamic / 3, nd = 0; int asc = 1;
      for (int i = 1; i < s->nb; i++) if (s->b[i].binding <= s->b[i - 1].binding) asc = 0;
      for (int pass = 0; pass < 2 && asc; pass++) for (int i = 0; i < s->nb && nd < lim; i++) {
          const sbind *b = &s->b[i]; if (b->type != BT_UBO || b->count != 1 || s->dynu[i]) continue;
          if (pass == 0 ? b->kind == K_CBUF : (b->kind == K_C1 || b->kind == K_DRVCB)) { s->dynu[i] = 1; nd++; } } }
    for (int i = 0; i < s->nb; i++) lb[i] = (VkDescriptorSetLayoutBinding){s->b[i].binding, s->dynu[i] ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC : DT[s->b[i].type], s->b[i].count, sf, NULL};
    VkDescriptorSetLayoutCreateInfo li = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, (uint32_t)s->nb, lb};
    VKCHECK(vkCreateDescriptorSetLayout(g_dev, &li, NULL, &s->dsl));
    return s;
}
static VkDescriptorSetLayout g_empty_dsl;
typedef struct { shader_t *vs, *fs, *gs; VkPipelineLayout pl; } prog_vk;
static prog_vk *g_pl; static int g_npl;
static pthread_mutex_t g_plmu = PTHREAD_MUTEX_INITIALIZER;
static VkPipelineLayout layout_get_(shader_t *vs, shader_t *fs, shader_t *gs);
static VkPipelineLayout layout_get(shader_t *vs, shader_t *fs, shader_t *gs) { pthread_mutex_lock(&g_plmu); VkPipelineLayout l = layout_get_(vs, fs, gs); pthread_mutex_unlock(&g_plmu); return l; }
static VkPipelineLayout layout_get_(shader_t *vs, shader_t *fs, shader_t *gs) {
    for (int i = 0; i < g_npl; i++) if (g_pl[i].vs == vs && g_pl[i].fs == fs && g_pl[i].gs == gs) return g_pl[i].pl;
    if (!g_empty_dsl) { VkDescriptorSetLayoutCreateInfo li = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; VKCHECK(vkCreateDescriptorSetLayout(g_dev, &li, NULL, &g_empty_dsl)); }
    VkDescriptorSetLayout sl[3] = {vs ? vs->dsl : g_empty_dsl, fs ? fs->dsl : g_empty_dsl, gs ? gs->dsl : g_empty_dsl};
    VkPushConstantRange pr = {VK_SHADER_STAGE_VERTEX_BIT, 0, 4};   // a vertex shader reading VertexIndex: the shift draw() made (spv_vid_bias)
    VkPipelineLayoutCreateInfo pi = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, gs ? 3 : fs ? 2 : 1, sl, vs && vs->vid ? 1 : 0, vs && vs->vid ? &pr : NULL};
    VkPipelineLayout pl; VKCHECK(vkCreatePipelineLayout(g_dev, &pi, NULL, &pl));
    g_pl = realloc(g_pl, (g_npl + 1) * sizeof *g_pl); g_pl[g_npl++] = (prog_vk){vs, fs, gs, pl};
    return pl;
}

// ---------- Render passes: dynamic rendering, or a render pass + framebuffer made for each pass ----------
typedef struct { int n; VkImageView cv[8]; VkFormat cf[8]; VkAttachmentLoadOp cl[8]; VkClearColorValue cc[8];
                 VkImageView dv; VkFormat df; int stencil; VkAttachmentLoadOp dl, sl; VkClearDepthStencilValue dc; uint32_t w, h; VkImageLayout layout; uint32_t layers; } rp_desc;
static struct { uint64_t key; VkRenderPass rp; } g_rpc[512]; static int g_nrpc; static pthread_mutex_t g_rpmu = PTHREAD_MUTEX_INITIALIZER;
// The render pass of these attachments (cached; also the "compatible" one pipelines are made against: load actions do not affect compatibility)
static VkRenderPass rp_get(const VkFormat *cf, const VkAttachmentLoadOp *cl, int n, VkFormat df, int stencil, VkAttachmentLoadOp dl, VkAttachmentLoadOp sl, VkImageLayout layout) {
    uint64_t key = 0xcbf29ce484222325ull;
    #define MIX(v) key = (key ^ (uint64_t)(v)) * 0x100000001b3ull
    for (int i = 0; i < n; i++) { MIX(cf[i]); MIX(cl[i]); } MIX(n); MIX(df); MIX(stencil); MIX(dl); MIX(sl); MIX(layout);
    #undef MIX
    pthread_mutex_lock(&g_rpmu);
    for (int i = 0; i < g_nrpc; i++) if (g_rpc[i].key == key) { VkRenderPass r = g_rpc[i].rp; pthread_mutex_unlock(&g_rpmu); return r; }
    VkAttachmentDescription at[9]; VkAttachmentReference cr[8], dr = {VK_ATTACHMENT_UNUSED, layout}; uint32_t na = 0;
    for (int i = 0; i < n; i++) {
        if (!cf[i]) { cr[i] = (VkAttachmentReference){VK_ATTACHMENT_UNUSED, layout}; continue; }
        at[na] = (VkAttachmentDescription){0, cf[i], VK_SAMPLE_COUNT_1_BIT, cl[i], VK_ATTACHMENT_STORE_OP_STORE, VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE, layout, layout};
        cr[i] = (VkAttachmentReference){na++, layout};
    }
    if (df) { at[na] = (VkAttachmentDescription){0, df, VK_SAMPLE_COUNT_1_BIT, dl, VK_ATTACHMENT_STORE_OP_STORE, stencil ? sl : VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                                                stencil ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE, layout, layout}; dr.attachment = na++; }
    VkSubpassDescription sp = {0, VK_PIPELINE_BIND_POINT_GRAPHICS, 0, NULL, (uint32_t)n, cr, NULL, df ? &dr : NULL, 0, NULL};
    VkRenderPassCreateInfo ci = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, NULL, 0, na, at, 1, &sp, 0, NULL};
    VkRenderPass r; VKCHECK(vkCreateRenderPass(g_dev, &ci, NULL, &r));
    if (g_nrpc < 512) g_rpc[g_nrpc++] = (typeof(g_rpc[0])){key, r};
    pthread_mutex_unlock(&g_rpmu);
    return r;
}
// Begins rendering into d's attachments; without dynamic rendering the framebuffer it makes is returned, for the caller to destroy once the GPU is done with it
static VkFramebuffer rp_begin(VkCommandBuffer cb, const rp_desc *d) {
    if (g_dynrend) {
        VkRenderingAttachmentInfo ca[8], da = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO}, sa = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        for (int i = 0; i < d->n; i++) ca[i] = (VkRenderingAttachmentInfo){VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, NULL, d->cv[i], d->layout, 0, NULL, 0,
                                                                          d->cv[i] ? d->cl[i] : VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_STORE, {.color = d->cc[i]}};
        if (d->dv) { da.imageView = d->dv; da.imageLayout = d->layout; da.loadOp = d->dl; da.storeOp = VK_ATTACHMENT_STORE_OP_STORE; da.clearValue.depthStencil = d->dc;
                     if (d->stencil) { sa = da; sa.loadOp = d->sl; } }
        VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO, NULL, 0, {{0, 0}, {d->w, d->h}}, d->layers ? d->layers : 1, 0, (uint32_t)d->n, ca, d->dv ? &da : NULL, d->dv && d->stencil ? &sa : NULL};
        p_begin_rendering(cb, &ri);
        return VK_NULL_HANDLE;
    }
    VkFormat cf[8]; VkImageView views[9]; VkClearValue cv[9]; uint32_t nv = 0;
    for (int i = 0; i < d->n; i++) { cf[i] = d->cv[i] ? d->cf[i] : VK_FORMAT_UNDEFINED; if (d->cv[i]) { cv[nv].color = d->cc[i]; views[nv++] = d->cv[i]; } }
    if (d->dv) { cv[nv].depthStencil = d->dc; views[nv++] = d->dv; }
    VkRenderPass rp = rp_get(cf, d->cl, d->n, d->dv ? d->df : VK_FORMAT_UNDEFINED, d->stencil, d->dl, d->sl, d->layout);
    VkFramebufferCreateInfo fi = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, NULL, 0, rp, nv, views, d->w, d->h, d->layers ? d->layers : 1};
    VkFramebuffer fb; VKCHECK(vkCreateFramebuffer(g_dev, &fi, NULL, &fb));
    VkRenderPassBeginInfo bi = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, NULL, rp, fb, {{0, 0}, {d->w, d->h}}, nv, cv};
    vkCmdBeginRenderPass(cb, &bi, VK_SUBPASS_CONTENTS_INLINE);
    return fb;
}
static void rp_end(VkCommandBuffer cb) { if (g_dynrend) p_end_rendering(cb); else vkCmdEndRenderPass(cb); }

// ---------- Replay state ----------
#define NTEX 256
static struct {
    const prog_t *bst[5], *cprog;   // the program bound to each graphics stage (NVN stage bits 1 vertex, 2 fragment, 4 geometry, 8 / 16 tessellation): games bind a program to some stages and NULL to others
    blend_st blend[8]; color_st color; chmask_st chmask; ms_st ms; poly_st poly; ds_st ds;
    va_st attr[16]; int nattr; vs_st stream[16]; int nstream; NVNbufferRange vbuf[16];
    p_bind_buf ubo[6][16], ssbo[6][16];
    uint64_t tex[6][NTEX], samp[6][128], img[6][32];
    uint32_t st_vmask[2], st_wmask[2], st_ref[2];
    p_poly_offset po; float vp[16][4]; float dr[2]; int has_vp; p_scissor sc; int has_sc;
    float blend_color[4]; int64_t copy_stride, copy_img_stride;
    int tex_base, samp_base;   // host-table bases of the texture / sampler pool bound (OP_SET_TEX_POOL / OP_SET_SAMP_POOL), for handles read from uniform buffers (K_TEXU)
    float line_w; p_prim_restart restart;   // SetLineWidth, SetPrimitiveRestart
} S;
static int g_depth_zero = 1, g_origin_ll;
static struct {   // the current render targets and the open dynamic rendering; clears that come before the pass's first draw become its load actions (a tiler then never reads the old contents)
    int n; vtex *ct[8]; VkImageView cv[8]; vtex *dt; VkImageView dv; uint32_t w, h; int open;
    int merged;   // the open pass went on across a SetRenderTargets (rt_hold_scan, or the same targets again): a draw sampling one of its attachments ends it first (draw_reads_rt)
    uint32_t clv[8], cly[8]; uint8_t cwr[8], cdrawn[8];   // each colour attachment's level and layer, and whether a draw wrote it since its last same-pass snapshot (fb_prepare)
    uint8_t ddrawn;   // the depth / stencil attachment was cleared or written in this pass (draw_reads_rt: an attachment only loaded reads the same from memory)
    uint8_t clr_c[8], clr_d, clr_s; VkClearColorValue cc[8]; float cd; uint32_t cs;
    // Layered rendering: a draw whose vertex (or geometry) shader writes the layer runs in a pass over every layer of the attachments (3D: their depth slices) from the attached one on,
    // through 2D array views (tex_view rt 3). Unreal renders its 32x32x32 colour grading LUT this way, one instance per slice; with a one-slice view all instances landed in slice 0
    // and the tonemapper's lookups read black (FINAL FANTASY RESONANCE's fields: black with white highlights; Pokémon Legends: Z-A on the Metal host, nvn_mtl.m)
    const p_set_rt *rt; int layered, want_layered, lok; VkImageView lcv[8], ldv; uint32_t nlayers;
} P;
static int pass_begin(void);
// A linear texture the GPU rendered into is read by the CPU from its storage: Super Mario Odyssey renders the scene's luminance into a 2x1 linear RG11B10F target and computes
// the exposure from it on the CPU; left in the image, the CPU read stale bytes and the exposure swung between a black and a yellow picture. The Metal host renders such
// textures in place on the pool's buffer; here the pass's result is copied into the ring and back into the storage when the submission completes (post kind 3)
static uint64_t g_lin_wb;
static void lin_writeback(vtex *v) {
    size_t n = (size_t)v->linrow * v->h;
    VkDeviceSize o = ring_alloc(n, 16); if (o == VK_WHOLE_SIZE) return;
    VkBufferImageCopy bc = {o, v->linrow / v->linbpb, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {v->w, v->h, 1}};
    vkCmdCopyImageToBuffer(C->cb, v->img, VK_IMAGE_LAYOUT_GENERAL, C->ring, 1, &bc);
    size_t packed = (size_t)v->w * v->linbpb;   // a pitch past the texels: only the texels go back, the bytes between rows stay the guest's
    if (v->linrow > packed) post_add((post_t){6, (void *)v->mem, v->linrow, v->h, 0, (uint32_t)packed, C->ringp + o, n});
    else post_add((post_t){3, (void *)v->mem, 0, 0, 0, 0, C->ringp + o, n});
    if (!g_lin_wb++) LOG("vk: linear render target %ux%u format %d goes back to its storage after each pass (serial %llu)", v->w, v->h, (int)v->f.f, (unsigned long long)v->serial);
}
// DM_VK_PASS_BARRIER=1: a pass's end barrier leaves the vertex shaders out of its destination, so the next pass's vertex work (the Adreno's binning) may overlap this pass's
// fragments instead of every pass end draining the GPU (59 a frame in the castle). A vertex shader that samples a texture written by such a pass, in the same barrier epoch, gets
// a barrier for it first (draw_vs_hazard: the open pass is ended for it). Submissions still begin with a full barrier. Default 0 (off) until measured
static uint64_t g_nbar_narrow, g_nbar_vs;
static int pass_barrier_on(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_VK_PASS_BARRIER"); v = e && atoi(e) != 0; if (v) LOG("DM_VK_PASS_BARRIER=1: pass ends do not wait for vertex shaders (their reads of the pass's targets get a barrier)"); } return v; }
static const uint8_t *g_rt_hold;   // see rt_hold_scan
static void pass_end(void) {
    g_rt_hold = NULL;
    if (!P.open) { int pend = P.clr_d || P.clr_s; for (int i = 0; i < 8; i++) pend |= P.clr_c[i]; if (pend) pass_begin(); }   // clears with no draw after them still happen
    if (P.open) {
        rp_end(C->cb); P.open = 0; gpu_mark(0, 1);
        // what the pass wrote is read by later passes, samples and copies; their vertex work may start before it ends
        VkPipelineStageFlags dst = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                   VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;   // compute too: a dispatch that reads what the pass drew (Unreal's HZB from depth)
        if (pass_barrier_on()) {   // vertex shaders left out: the next pass's vertex work may overlap this one's fragments; the attachments written are marked for draw_vs_hazard
            dst &= ~(VkPipelineStageFlags)VK_PIPELINE_STAGE_VERTEX_SHADER_BIT; g_nbar_narrow++;
            for (int i = 0; i < P.n; i++) if (P.ct[i] && P.cdrawn[i]) { P.ct[i]->vhaz = 1; P.ct[i]->vhaz_epoch = g_vbar_epoch; }
            if (P.dt && P.ddrawn) { P.dt->vhaz = 1; P.dt->vhaz_epoch = g_vbar_epoch; }
        } else g_vbar_epoch++;
        VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT};
        vkCmdPipelineBarrier(C->cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, dst, 0, 1, &mb, 0, NULL, 0, NULL);
        int wb = 0;
        for (int i = 0; i < P.n; i++) if (P.ct[i] && P.ct[i]->linrow && P.cdrawn[i] && !P.clv[i] && !P.cly[i]) { lin_writeback(P.ct[i]); wb = 1; }
        memset(P.cdrawn, 0, sizeof P.cdrawn); P.ddrawn = 0;
        if (wb) barrier_all(C->cb);
    }
}
static int g_pass_open_(void) { return P.open; }
static uint64_t g_npass, g_nheld, g_nsame;   // render passes begun / SetRenderTargets detours held / same-target SetRenderTargets kept (the GPU line)
static int g_after_split;   // the next pass begins where a held pass was split (draw_reads_rt): its signature says so
static int pass_begin(void) {
    if (P.open) return 1;
    if (!P.n && !P.dt) return 0;
    rp_desc d = {.n = P.n, .w = P.w, .h = P.h, .layout = VK_IMAGE_LAYOUT_GENERAL, .layers = 1};
    if (P.want_layered && !P.lok && P.rt) {   // the attachments' layered views, made at the first layered draw after set_rt
        const p_set_rt *r = P.rt; P.lok = 1; P.nlayers = UINT32_MAX;
        for (int i = 0; i < P.n; i++) if (P.ct[i] && r->color[i]) { vtex *v = P.ct[i]; P.lcv[i] = tex_view(v, r->color[i], &r->cview[i], r->has_cview[i], -1, 3);
            uint32_t have = v->target == NVN_TEXTURE_TARGET_3D ? (v->d >> P.clv[i] ? v->d >> P.clv[i] : 1) - P.cly[i] : v->layers - P.cly[i]; if (have < P.nlayers) P.nlayers = have; }
        if (P.dt && r->depth) { vtex *v = P.dt; uint32_t b0 = r->has_dview && r->dview.set_layers ? r->dview.min_layer : 0; P.ldv = tex_view(v, r->depth, &r->dview, r->has_dview, -1, 3);
            if (v->layers - b0 < P.nlayers) P.nlayers = v->layers - b0; }
        if (P.nlayers == UINT32_MAX || P.nlayers > g_props.limits.maxFramebufferLayers) P.nlayers = P.nlayers == UINT32_MAX ? 1 : g_props.limits.maxFramebufferLayers;
        static int n; if (n++ < 4) LOG("vk: layered pass: %u layers (the vertex shader writes the layer)", P.nlayers);
    }
    P.layered = P.want_layered && P.lok && P.nlayers > 1;
    if (P.layered) d.layers = P.nlayers;
    for (int i = 0; i < P.n; i++) { d.cv[i] = P.layered && P.lcv[i] ? P.lcv[i] : P.cv[i]; d.cf[i] = P.ct[i] ? P.ct[i]->f.f : VK_FORMAT_UNDEFINED; d.cl[i] = P.clr_c[i] ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD; d.cc[i] = P.cc[i]; if (P.clr_c[i]) P.cdrawn[i] = 1; }
    if (P.dt) { d.dv = P.layered && P.ldv ? P.ldv : P.dv; d.df = P.dt->f.f; d.stencil = P.dt->f.stencil; d.dl = P.clr_d ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
                d.sl = P.clr_s ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD; d.dc = (VkClearDepthStencilValue){P.cd, P.cs}; if (P.clr_d || P.clr_s) P.ddrawn = 1; }
    memset(P.clr_c, 0, sizeof P.clr_c); P.clr_d = P.clr_s = 0;
    if (g_plog) { char b[512]; int o = 0; for (int i = 0; i < P.n; i++) o += snprintf(b + o, sizeof b - o, " c%d=%llu/%d%s", i, P.ct[i] ? (unsigned long long)P.ct[i]->serial : 0ull, (int)d.cf[i], d.cl[i] == VK_ATTACHMENT_LOAD_OP_CLEAR ? "/clr" : "");
                  if (P.dt) snprintf(b + o, sizeof b - o, " d=%llu/%d%s%s", (unsigned long long)P.dt->serial, (int)d.df, d.dl == VK_ATTACHMENT_LOAD_OP_CLEAR ? "/clr" : "", d.sl == VK_ATTACHMENT_LOAD_OP_CLEAR ? "/sclr" : "");
                  LOG("plog: pass %ux%u%s", P.w, P.h, b); }
    {   // this pass's signature for the per-pass GPU times: size, attachments (format, cleared), layered, and whether a held pass's split began it
        char b[120]; int o = snprintf(b, sizeof b, "%ux%u", P.w, P.h); uint64_t h = 0xcbf29ce484222325ull;
        #define SMIX(v) h = (h ^ (uint64_t)(v)) * 0x100000001b3ull
        SMIX(P.w); SMIX(P.h); SMIX(P.n); SMIX(d.layers); SMIX(g_after_split);
        for (int i = 0; i < P.n; i++) { SMIX(d.cf[i]); SMIX(d.cl[i]); if (o < (int)sizeof b) o += snprintf(b + o, sizeof b - o, " c%d=%d%s", i, (int)d.cf[i], d.cl[i] == VK_ATTACHMENT_LOAD_OP_CLEAR ? "/clr" : ""); }
        if (P.dt) { SMIX(d.df); SMIX(d.dl); SMIX(d.sl); if (o < (int)sizeof b) o += snprintf(b + o, sizeof b - o, " d=%d%s%s", (int)d.df, d.dl == VK_ATTACHMENT_LOAD_OP_CLEAR ? "/clr" : "", d.sl == VK_ATTACHMENT_LOAD_OP_CLEAR ? "/sclr" : ""); }
        if (d.layers > 1 && o < (int)sizeof b) o += snprintf(b + o, sizeof b - o, " layers %u", d.layers);
        if (g_after_split && o < (int)sizeof b) o += snprintf(b + o, sizeof b - o, " (after a split)");
        #undef SMIX
        g_tsig_next = h & ~1ull; gsig_name(g_tsig_next, b); g_after_split = 0;
    }
    gpu_mark(0, 0); g_npass++; P.merged = 0;
    VkFramebuffer fb = rp_begin(C->cb, &d); P.open = 1;
    if (fb) { if (C->nfb == C->cfb) { C->cfb = C->cfb ? 2 * C->cfb : 256; C->fb = realloc(C->fb, C->cfb * sizeof *C->fb); } C->fb[C->nfb++] = fb; }
    return 1;
}
// Render targets held across a detour (as nvn_mtl.m rt_hold_scan, DM_VK_RT_HOLD=0 turns it off): Unreal's parallel base pass begins each of its command lists by setting other
// render targets (another count) and then the six G-buffer targets again, with only state changes in between (FINAL FANTASY RESONANCE's battle: the G-buffer drawn in three passes a
// frame, each storing six 1384x780 targets and depth and loading them again, a full tile load and store on the Adreno). A SetRenderTargets that would end the open pass looks ahead in
// replay order -- called lists, the callers after a list ends, the submission's remaining command handles: when only state changes lie between it and a SetRenderTargets back to the open
// pass's targets, the open pass goes on and the SetRenderTargets in between are not replayed. Anything that draws, clears, copies, dispatches, writes, waits, orders or signals ends the
// scan (the pass ends as before). The same targets set again keep the open pass too. A draw sampling an attachment of the pass is served as before (same-pass feedback, fb_prepare)
static int rt_hold_on(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_VK_RT_HOLD"); v = !(e && !strcmp(e, "0")); if (!v) LOG("DM_VK_RT_HOLD=0: every SetRenderTargets ends the open pass"); } return v; }
static int view_eq(uint8_t ha, const view_t *a, uint8_t hb, const view_t *b) { return ha == hb && (!ha || !memcmp(a, b, sizeof *a)); }
static int rt_same(const p_set_rt *a, const p_set_rt *b) {   // the same attachments: count, textures (by serial) and views
    if (!a || !b || a->n != b->n || !a->depth != !b->depth || (a->depth && a->depth->serial != b->depth->serial) || !view_eq(a->has_dview, &a->dview, b->has_dview, &b->dview)) return 0;
    for (int k = 0; k < a->n && k < 8; k++) { if (!a->color[k] != !b->color[k] || (a->color[k] && a->color[k]->serial != b->color[k]->serial)) return 0;
                                              if (a->color[k] && !view_eq(a->has_cview[k], &a->cview[k], b->has_cview[k], &b->cview[k])) return 0; }
    return 1;
}
static const uint8_t *g_rs[32]; static int g_nrs;   // the replay's return points: after each CALL being replayed (mtl_exec_)
static const uint64_t *g_rest; static int g_nrest;   // the submission's command handles after the one being replayed (mtl_exec_rest)
static const uint8_t *rt_hold_scan(const uint8_t *p, const p_set_rt *open) {   // p: the op after the detour's SetRenderTargets; the SetRenderTargets back to `open`, or NULL
    if (g_nrs > 32) return NULL;   // calls nested deeper than the return points kept
    const uint8_t *stk[16]; int ns = 0, outer = g_nrs, ri = 0;
    for (int guard = 0; guard < 4096; guard++) {
        op_hdr h; memcpy(&h, p, sizeof h); const uint8_t *a = p + 8;
        switch (h.op) {
            case OP_END:
                if (ns) { p = stk[--ns]; continue; }
                if (outer > 0) { p = g_rs[--outer]; continue; }
                if (ri < g_nrest && g_rest && g_rest[ri]) { p = (const uint8_t *)(uintptr_t)g_rest[ri++]; continue; }
                return NULL;
            case OP_JUMP: memcpy(&p, a, 8); continue;
            case OP_CALL: { const uint8_t *t; memcpy(&t, a, 8); if (ns >= 16) return NULL; stk[ns++] = p + h.size; p = t; continue; }
            case OP_SET_RT: if (rt_same(open, (const p_set_rt *)a)) return p; break;   // another detour target: still only state
            case OP_BEGIN: case OP_VIEWPORTS: case OP_SCISSOR: case OP_DEPTH_RANGE: case OP_BIND_PROGRAM: case OP_BIND_STATE: case OP_BIND_VATTR: case OP_BIND_VSTREAM: case OP_BIND_VBUF:
            case OP_BIND_UBO: case OP_BIND_SSBO: case OP_BIND_TEX: case OP_BIND_SAMP: case OP_STENCIL: case OP_POLY_OFFSET: case OP_SET_TEX_POOL: case OP_SET_SAMP_POOL: case OP_COPY_STRIDE:
            case OP_BLEND_COLOR: case OP_DEPTH_CLAMP: case OP_VP_SWIZZLE: case OP_BIND_TEX1: case OP_PRIM_RESTART: case OP_BIND_UBO_SMALL: case OP_BIND_VBUF1: case OP_ALPHA_REF: case OP_LINE_WIDTH: break;
            default: return NULL;
        }
        p += h.size;
    }
    return NULL;
}
static void set_rt_(const p_set_rt *r);
static void set_rt(const uint8_t *op, const p_set_rt *r, uint32_t size) {
    if (g_rt_hold) {   // inside a held detour: its SetRenderTargets are not replayed; the one back to the open pass's targets ends it
        if (op == g_rt_hold) { g_rt_hold = NULL; P.rt = r; P.merged = 1; }
        return;
    }
    if (P.open && rt_hold_on() && P.rt) {
        if (rt_same(P.rt, r)) { P.rt = r; P.merged = 1; g_nsame++; return; }   // the same targets again: the pass goes on
        const uint8_t *back = rt_hold_scan(op + size, P.rt);
        if (back) { static int logged; g_rt_hold = back; g_nheld++; if (!logged++) LOG("vk: render targets held across a SetRenderTargets detour (first case; DM_VK_RT_HOLD=0 turns it off)"); return; }
    }
    set_rt_(r);
}
static void set_rt_(const p_set_rt *r) {
    pass_end();
    memset(&P, 0, sizeof P);
    P.n = r->n > 8 ? 8 : r->n; P.w = P.h = 0x7fffffff; P.rt = r;
    for (int i = 0; i < P.n; i++) {
        if (!r->color[i]) continue;
        vtex *v = tex_get(r->color[i], 1); if (!v) continue;
        P.ct[i] = v; P.cv[i] = tex_view(v, r->color[i], &r->cview[i], r->has_cview[i], -1, 1);
        uint32_t lv = r->has_cview[i] && r->cview[i].set_levels ? r->cview[i].base_level : 0, w = v->w >> lv ? v->w >> lv : 1, h = v->h >> lv ? v->h >> lv : 1;
        P.clv[i] = lv; P.cly[i] = r->has_cview[i] && r->cview[i].set_layers ? r->cview[i].min_layer : 0; P.cwr[i] = 1;
        if (w < P.w) P.w = w; if (h < P.h) P.h = h;
    }
    if (r->depth) {
        vtex *v = tex_get(r->depth, 1);
        if (v && v->f.depth) { P.dt = v; P.dv = tex_view(v, r->depth, &r->dview, r->has_dview, -1, 1); uint32_t lv = r->has_dview && r->dview.set_levels ? r->dview.base_level : 0;
                               if ((v->w >> lv) < P.w) P.w = v->w >> lv ? v->w >> lv : 1; if ((v->h >> lv) < P.h) P.h = v->h >> lv ? v->h >> lv : 1; }
    }
    if (P.w == 0x7fffffff) P.w = P.h = 1;
}

// Pipelines, cached by everything they are built from
typedef struct {
    shader_t *vs, *fs, *gs; int prim;
    va_st attr[16]; int nattr; vs_st stream[16]; int nstream;
    blend_st blend[8]; color_st color; chmask_st chmask; ds_st ds; poly_st poly; int samples;
    VkFormat cf[8]; int nc; VkFormat df; int stencil;
    float line_w; int restart, dual;   // line_w: line primitives only (0 otherwise); dual: blends with the second source, through the pixel shader's mod_dual
} pso_key;
typedef struct { pso_key k; uint64_t h; VkPipeline p; VkPipelineLayout pl; _Atomic int state; uint8_t rewarm; } pso_ent;   // state: 0 being compiled (pso_worker), 1 ready (p may be NULL: failed), 3 warm: made once by the pre-warm and destroyed (DM_VK_PREWARM_KEEP=0), made again from the driver's cache at its first use; rewarm: being made again so
static int g_npso; static _Atomic uint64_t g_pso_ns;   // pipelines made, and the time spent making them (mtl_create_ns: the long-frame lines)
static uint64_t fnv(const void *p, size_t n) { const uint8_t *b = p; uint64_t h = 0xcbf29ce484222325ull; while (n--) h = (h ^ *b++) * 0x100000001b3ull; return h; }
static VkBlendFactor bfac(int f) {
    switch (f) {
        case 1: return VK_BLEND_FACTOR_ZERO; case 2: return VK_BLEND_FACTOR_ONE; case 3: return VK_BLEND_FACTOR_SRC_COLOR; case 4: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
        case 5: return VK_BLEND_FACTOR_SRC_ALPHA; case 6: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA; case 7: return VK_BLEND_FACTOR_DST_ALPHA; case 8: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
        case 9: return VK_BLEND_FACTOR_DST_COLOR; case 10: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR; case 11: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
        case 0x61: return VK_BLEND_FACTOR_CONSTANT_COLOR; case 0x62: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR; case 0x63: return VK_BLEND_FACTOR_CONSTANT_ALPHA; case 0x64: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
        case 0x10: return VK_BLEND_FACTOR_SRC1_COLOR; case 0x11: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR; case 0x12: return VK_BLEND_FACTOR_SRC1_ALPHA; case 0x13: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;
        default: return VK_BLEND_FACTOR_ONE;
    }
}
static VkBlendOp bop(int e) { switch (e) { case 2: return VK_BLEND_OP_SUBTRACT; case 3: return VK_BLEND_OP_REVERSE_SUBTRACT; case 4: return VK_BLEND_OP_MIN; case 5: return VK_BLEND_OP_MAX; } return VK_BLEND_OP_ADD; }
static VkStencilOp sop(int o) { return o >= 1 && o <= 8 ? (VkStencilOp)(o - 1) : VK_STENCIL_OP_KEEP; }
static VkCompareOp cop(int f) { return f >= 1 && f <= 8 ? (VkCompareOp)(f - 1) : VK_COMPARE_OP_ALWAYS; }
static int bf_src1(int f) { return f >= 0x10 && f <= 0x13; }   // NVN_BLEND_FUNC_SRC1_COLOR … ONE_MINUS_SRC1_ALPHA
// The second source without the GPU feature (Mali), or without the pixel shader's output 1: read as one (ONE_MINUS_*: zero), as before
static int bf_no_src1(int f) { if (!bf_src1(f)) return f; RED_ONCE(10, "dual-source blending: not available here, the second source reads as one"); return f == 0x11 || f == 0x13 ? 1 : 2; }
static VkPipeline pso_create(const pso_key *k, VkPipelineLayout pl) {
    VkPipelineShaderStageCreateInfo st[3] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_VERTEX_BIT, k->vs->mod, "main", NULL}}; uint32_t nst = 1;
    if (k->gs) st[nst++] = (VkPipelineShaderStageCreateInfo){VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_GEOMETRY_BIT, k->gs->mod, "main", NULL};
    if (k->fs) st[nst++] = (VkPipelineShaderStageCreateInfo){VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_FRAGMENT_BIT, k->dual ? k->fs->mod_dual : k->fs->mod, "main", NULL};
    VkVertexInputBindingDescription vb[16]; VkVertexInputAttributeDescription va[16]; uint32_t nvb = 0, nva = 0; uint32_t used = 0;
    for (int i = 0; i < k->nattr && i < 16; i++) {
        if (!(k->vs->vin_mask >> i & 1)) continue;
        VkFormat f = vfmt(k->attr[i].fmt);
        if (!f) { static int n; if (n++ < 8) LOG("[RED] vk: vertex attribute %d format %#x has no Vulkan format", i, k->attr[i].fmt); continue; }
        va[nva++] = (VkVertexInputAttributeDescription){(uint32_t)i, k->attr[i].stream, f, k->attr[i].offset};
        used |= 1u << k->attr[i].stream;
    }
    for (int s = 0; s < 16; s++) if (used >> s & 1) {
        uint32_t stride = s < k->nstream ? k->stream[s].stride : 0, div = s < k->nstream ? k->stream[s].divisor : 0;
        if (div > 1) RED_ONCE(11, "instance divisor %u: drawn as 1", div);
        vb[nvb++] = (VkVertexInputBindingDescription){(uint32_t)s, stride, div ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX};
    }
    VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, NULL, 0, nvb, vb, nva, va};
    static const VkPrimitiveTopology TOPO[] = {VK_PRIMITIVE_TOPOLOGY_POINT_LIST, VK_PRIMITIVE_TOPOLOGY_LINE_LIST, VK_PRIMITIVE_TOPOLOGY_LINE_STRIP, VK_PRIMITIVE_TOPOLOGY_LINE_STRIP,
        VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP,
        VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN, VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY, VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY,
        VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP_WITH_ADJACENCY, VK_PRIMITIVE_TOPOLOGY_PATCH_LIST};
    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, NULL, 0, TOPO[k->prim < 15 ? k->prim : 4], k->restart ? VK_TRUE : VK_FALSE};
    VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, NULL, 0, 1, NULL, 1, NULL};
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = (VkCullModeFlags)k->poly.cull; rs.frontFace = k->poly.front ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;
    if (g_origin_ll) rs.frontFace = rs.frontFace == VK_FRONT_FACE_CLOCKWISE ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;
    rs.depthBiasEnable = k->poly.offset_en != 0; rs.lineWidth = k->line_w > 1 ? k->line_w : 1;
    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, NULL, 0, (VkSampleCountFlagBits)(k->samples > 1 ? k->samples : 1)};
    VkPipelineDepthStencilStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    if (k->df) {
        ds.depthTestEnable = k->ds.dt; ds.depthWriteEnable = k->ds.dw; ds.depthCompareOp = k->ds.dt ? cop(k->ds.df) : VK_COMPARE_OP_ALWAYS;
        if (k->stencil && k->ds.st) {
            ds.stencilTestEnable = VK_TRUE;
            ds.front = (VkStencilOpState){sop(k->ds.fail_f), sop(k->ds.zpass_f), sop(k->ds.zfail_f), cop(k->ds.sf_f), 0xff, 0xff, 0};
            ds.back = (VkStencilOpState){sop(k->ds.fail_b), sop(k->ds.zpass_b), sop(k->ds.zfail_b), cop(k->ds.sf_b), 0xff, 0xff, 0};
        }
    }
    VkPipelineColorBlendAttachmentState ba[8];
    for (int i = 0; i < k->nc; i++) {
        const blend_st *b = &k->blend[i]; int en = k->color.blend_en >> i & 1;
        int sr = b->src_rgb, dr = b->dst_rgb, sa = b->src_a, da = b->dst_a;
        if (!k->dual) { sr = bf_no_src1(sr); dr = bf_no_src1(dr); sa = bf_no_src1(sa); da = bf_no_src1(da); }
        ba[i] = (VkPipelineColorBlendAttachmentState){en, bfac(sr), bfac(dr), bop(b->eq_rgb), bfac(sa), bfac(da), bop(b->eq_a), (k->chmask.mask >> (4 * i)) & 15};
    }
    VkPipelineColorBlendStateCreateInfo cb = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, NULL, 0, VK_FALSE, VK_LOGIC_OP_COPY, (uint32_t)k->nc, ba, {0}};
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_DEPTH_BIAS, VK_DYNAMIC_STATE_BLEND_CONSTANTS,
                            VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_STENCIL_REFERENCE};
    VkPipelineDynamicStateCreateInfo dy = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, NULL, 0, sizeof dyn / sizeof *dyn, dyn};
    VkPipelineRenderingCreateInfo rinfo = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, NULL, 0, (uint32_t)k->nc, k->cf, k->df, k->stencil ? k->df : VK_FORMAT_UNDEFINED};
    VkRenderPass crp = VK_NULL_HANDLE;
    if (!g_dynrend) { VkAttachmentLoadOp ld[8]; for (int i = 0; i < 8; i++) ld[i] = VK_ATTACHMENT_LOAD_OP_LOAD;
                      crp = rp_get(k->cf, ld, k->nc, k->df, k->stencil, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_LOAD_OP_LOAD, VK_IMAGE_LAYOUT_GENERAL); }
    VkGraphicsPipelineCreateInfo gi = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, g_dynrend ? &rinfo : NULL, 0, nst, st, &vi, &ia, NULL, &vp, &rs, &ms, &ds, &cb, &dy, pl, crp, 0, VK_NULL_HANDLE, -1};
    uint64_t t0 = now_ns();
    VkPipeline p; VkResult r = vkCreateGraphicsPipelines(g_dev, g_pcache, 1, &gi, NULL, &p);
    atomic_fetch_add(&g_pso_ns, now_ns() - t0);
    if (r != VK_SUCCESS) { LOG("[RED] vk: pipeline for %s / %s not created (%d)", k->vs->key, k->fs ? k->fs->key : "-", (int)r); p = VK_NULL_HANDLE; }
    return p;
}
// Pipelines by their key: a hash table of entries (each allocated once, so a worker can fill one in while the table grows). A new one is made on a worker thread (pso_worker) when
// may_wait is 0 -- the draw is left out until it is ready, a few frames -- else on the spot as before. Effects meeting the GPU for the first time compiled dozens at once on the
// replay thread: FINAL FANTASY RESONANCE froze 0.5-1.8 s in battle (42 new pipelines in one frame). Only draws in passes with colour and depth (the scene, its effects) wait for
// a worker; passes drawn once (the colour grading LUT, render-to-texture, post-processing, shadows) still make theirs at once
static pso_ent **g_psot; static uint32_t g_psot_cap; static _Atomic uint64_t g_pso_async, g_pso_skipped; static _Atomic int g_pso_pending;
static uint64_t g_pso_last_new;   // the frame of the last new pipeline (pcache_save soon after)
static pthread_mutex_t g_psoq_mu = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t g_psoq_cv = PTHREAD_COND_INITIALIZER; static pso_ent **g_psoq; static int g_npsoq, g_cpsoq;
static _Atomic uint64_t g_pso_rewarm, g_pso_rewarm_ns;   // warm pipelines made again at their first use, and the time that took (DM_VK_PREWARM_KEEP=0)
static void *pso_worker(void *a) {
    (void)a;
    for (;;) {
        pthread_mutex_lock(&g_psoq_mu); while (!g_npsoq) pthread_cond_wait(&g_psoq_cv, &g_psoq_mu);
        pso_ent *e = g_psoq[0]; memmove(g_psoq, g_psoq + 1, --g_npsoq * sizeof *g_psoq); pthread_mutex_unlock(&g_psoq_mu);
        uint64_t t0 = now_ns(); e->p = pso_create(&e->k, e->pl); if (e->rewarm) atomic_fetch_add(&g_pso_rewarm_ns, now_ns() - t0);
        atomic_store_explicit(&e->state, 1, memory_order_release); atomic_fetch_sub(&g_pso_pending, 1);
    }
    return NULL;
}
static void pso_enqueue(pso_ent *e) {
    pthread_mutex_lock(&g_psoq_mu);
    if (g_npsoq == g_cpsoq) { g_cpsoq = g_cpsoq ? 2 * g_cpsoq : 64; g_psoq = realloc(g_psoq, g_cpsoq * sizeof *g_psoq); }
    g_psoq[g_npsoq++] = e; atomic_fetch_add(&g_pso_pending, 1); atomic_fetch_add(&g_pso_async, 1);
    pthread_cond_signal(&g_psoq_cv); pthread_mutex_unlock(&g_psoq_mu);
}
// Every pipeline made is recorded (pipeline_keys.bin beside the pipeline cache: its shaders' keys and the rest of its key), and the next run makes them all again on a thread of its
// own after its first frame (pso_precreate), so an effect or a skill met once does not stall the game when it comes back, also when the driver's cache dropped the pipeline
typedef struct { char vs[17], fs[17], gs[17]; pso_key k; } pso_rec;
static pthread_mutex_t g_psomu = PTHREAD_MUTEX_INITIALIZER; static FILE *g_pso_rec;
static void pso_record(const pso_key *k) {
    if (!g_pso_rec) { const char *d = getenv("DM_VK_DIR"); if (!d) return; char p[1100]; snprintf(p, sizeof p, "%s/../pipeline_keys.bin", d); if (!(g_pso_rec = fopen(p, "ab"))) return; }
    pso_rec r; memset(&r, 0, sizeof r); r.k = *k; r.k.vs = r.k.fs = r.k.gs = NULL;
    if (k->vs) strcpy(r.vs, k->vs->key); if (k->fs) strcpy(r.fs, k->fs->key); if (k->gs) strcpy(r.gs, k->gs->key);
    fwrite(&r, sizeof r, 1, g_pso_rec); fflush(g_pso_rec);
}
static pso_ent *pso_find_(const pso_key *k, uint64_t h, uint32_t *slot) {   // under g_psomu
    if (!g_psot_cap) { g_psot_cap = 4096; g_psot = calloc(g_psot_cap, sizeof *g_psot);
                       for (int i = 0; i < 2; i++) { pthread_t t; if (!pthread_create(&t, NULL, pso_worker, NULL)) pthread_detach(t); } }
    uint32_t i = (uint32_t)h & (g_psot_cap - 1);
    for (; g_psot[i]; i = (i + 1) & (g_psot_cap - 1)) if (g_psot[i]->h == h && !memcmp(&g_psot[i]->k, k, sizeof *k)) return g_psot[i];
    *slot = i; return NULL;
}
static pso_ent *pso_insert_(const pso_key *k, uint64_t h, VkPipelineLayout pl, uint32_t i) {   // under g_psomu
    pso_ent *e = calloc(1, sizeof *e); e->k = *k; e->h = h; e->pl = pl;
    g_psot[i] = e; g_npso++;
    if ((uint32_t)g_npso * 2 > g_psot_cap) {   // grow the table (entries stay where they are)
        uint32_t nc = g_psot_cap * 2; pso_ent **nt = calloc(nc, sizeof *nt);
        for (uint32_t j = 0; j < g_psot_cap; j++) if (g_psot[j]) { uint32_t q = (uint32_t)g_psot[j]->h & (nc - 1); while (nt[q]) q = (q + 1) & (nc - 1); nt[q] = g_psot[j]; }
        free(g_psot); g_psot = nt; g_psot_cap = nc;
    }
    return e;
}
static VkPipeline pso_get(const pso_key *k, VkPipelineLayout pl, int may_wait) {
    uint64_t h = fnv(k, sizeof *k); uint32_t i = 0;
    pthread_mutex_lock(&g_psomu);
    pso_ent *e = pso_find_(k, h, &i);
    if (e) {
        pthread_mutex_unlock(&g_psomu);
        for (;;) {
            int st = atomic_load_explicit(&e->state, memory_order_acquire);
            if (st == 1) return e->p;
            if (st == 3) {   // warm (made by the pre-warm, then destroyed): made again now, from the driver's cache
                int exp = 3; if (!atomic_compare_exchange_strong(&e->state, &exp, 0)) continue;
                e->rewarm = 1; atomic_fetch_add(&g_pso_rewarm, 1);
                if (may_wait) { uint64_t t0 = now_ns(); e->p = pso_create(k, pl); atomic_fetch_add(&g_pso_rewarm_ns, now_ns() - t0); atomic_store_explicit(&e->state, 1, memory_order_release); return e->p; }
                pso_enqueue(e); atomic_fetch_add(&g_pso_skipped, 1); return VK_NULL_HANDLE;
            }
            if (may_wait) { usleep(200); continue; }   // a worker has it: wait for that one
            atomic_fetch_add(&g_pso_skipped, 1); return VK_NULL_HANDLE;
        }
    }
    e = pso_insert_(k, h, pl, i); pthread_mutex_unlock(&g_psomu);
    g_pso_last_new = atomic_load(&g_presents); pso_record(k);
    if (may_wait) { e->p = pso_create(k, pl); atomic_store_explicit(&e->state, 1, memory_order_release); return e->p; }
    pso_enqueue(e);
    atomic_fetch_add(&g_pso_skipped, 1); return VK_NULL_HANDLE;
}

static int hexkey(const char *s, uint8_t md[8]) { for (int i = 0; i < 8; i++) if (sscanf(s + 2 * i, "%2hhx", &md[i]) != 1) return 0; return 1; }
static _Atomic uint64_t g_pre_made, g_pre_had, g_pre_slow;
// DM_VK_PREWARM_KEEP=0: the pre-warm makes each pipeline and destroys it at once, so only the driver's pipeline cache is warm; a pipeline is made again (from that cache) at its first
// use. FINAL FANTASY RESONANCE: 6619 pipelines kept alive cost RSS +485 MB at boot on a tablet already swapping 2 GB. Default 1 (kept, as before)
static int prewarm_keep(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_VK_PREWARM_KEEP"); v = !(e && !strcmp(e, "0")); if (!v) LOG("DM_VK_PREWARM_KEEP=0: the pre-warm keeps only the driver's pipeline cache warm"); } return v; }
static void pso_precreate_one(const pso_rec *r) {
    uint8_t md[8]; pso_key k = r->k;
    if (!r->vs[0] || !hexkey(r->vs, md) || !(k.vs = shader_get(md)) || !k.vs->mod) return;
    if (r->fs[0]) { if (!hexkey(r->fs, md) || !(k.fs = shader_get(md)) || !k.fs->mod) return; }
    if (r->gs[0]) { if (!hexkey(r->gs, md) || !(k.gs = shader_get(md)) || !k.gs->mod) return; }
    if (k.dual && (!k.fs || !fs_dual_mod(k.fs))) return;
    VkPipelineLayout pl = layout_get(k.vs, k.fs, k.gs);
    uint64_t h = fnv(&k, sizeof k); uint32_t i = 0;
    pthread_mutex_lock(&g_psomu);
    if (pso_find_(&k, h, &i)) { pthread_mutex_unlock(&g_psomu); atomic_fetch_add(&g_pre_had, 1); return; }
    pso_ent *e = pso_insert_(&k, h, pl, i); pthread_mutex_unlock(&g_psomu);
    uint64_t t0 = now_ns(); e->p = pso_create(&k, pl); atomic_fetch_add(&g_pre_made, 1);
    if (now_ns() - t0 > 3000000ull) atomic_fetch_add(&g_pre_slow, 1);   // not in the driver's cache: compiled
    if (!prewarm_keep() && e->p) { vkDestroyPipeline(g_dev, e->p, NULL); e->p = VK_NULL_HANDLE; atomic_store_explicit(&e->state, 3, memory_order_release); }   // the driver's cache holds it now
    else atomic_store_explicit(&e->state, 1, memory_order_release);
}
static int g_prewarm_ran;
static int g_prewarm_depth = -1;   // the depth mode the pre-warm loaded shaders under (dm_vk_prewarm), from the last run's vk_depth_mode
static void *pso_precreate(void *a) {
    (void)a; const char *d = getenv("DM_VK_DIR"); if (!d || g_prewarm_ran) return NULL;   // the pre-warm before the game did it already
    char p[1100]; snprintf(p, sizeof p, "%s/../pipeline_keys.bin", d); FILE *f = fopen(p, "rb"); if (!f) return NULL;
    uint64_t t0 = now_ns(); pso_rec r; uint64_t n = 0;
    while (fread(&r, sizeof r, 1, f) == 1) { n++; pso_precreate_one(&r); }
    fclose(f);
    LOG("vk: pipelines of earlier runs made again: %llu of %llu recorded (%llu already there) in %.1f s", (unsigned long long)atomic_load(&g_pre_made), (unsigned long long)n,
        (unsigned long long)atomic_load(&g_pre_had), (now_ns() - t0) / 1e9);
    return NULL;
}
// ---------- Draw resources ----------
typedef struct { uint64_t addr, size, gen; VkDeviceSize off; uint32_t seq; } dcopy;
#define NDC 4096
static dcopy g_dc[NDC]; static int g_ndc;
// The in-order writes of this generation (wlog_add: inline uniform updates, buffer copies and fills), by range: a cached copy stays good unless one written after it overlaps it.
// Every write used to end every cached copy: Unreal updates uniforms between draws, so each draw copied its vertex streams, uniform and texel buffers again, at new ring
// offsets, with new texel buffer views and new descriptor sets (14% of the sets reused, 12 ms a frame in descriptor sets)
enum { NWLOG = 8192 };
static uint64_t g_wlo[NWLOG], g_whi[NWLOG];
static void wlog_add(uint64_t a, uint64_t n) {
    if (g_nwlog == NWLOG) { g_wgen++; g_nwlog = 0; return; }   // too many: a new generation (every copy taken again)
    g_wlo[g_nwlog] = a; g_whi[g_nwlog] = a + n; g_nwlog++;
}
static VkDeviceSize copy_in(uint64_t addr, uint64_t size, VkDeviceSize align) {   // guest memory → ring, reused when the same range is asked for again unchanged (hashed by address)
    dcopy *e = &g_dc[(addr >> 4 ^ addr >> 13) % NDC];
    if (e->addr == addr && e->size >= size && e->gen == g_wgen && e->gen && e->off % align == 0 && e->seq <= g_nwlog) {
        uint64_t hi = addr + size; uint32_t k = e->seq;
        for (; k < g_nwlog; k++) if (g_wlo[k] < hi && g_whi[k] > addr) break;
        if (k == g_nwlog) { if (size >= e->size) e->seq = g_nwlog; return e->off; }   // good up to now: the next lookup scans only later writes
    }
    VkDeviceSize o = ring_put((const void *)(uintptr_t)addr, size, align);
    if (o != VK_WHOLE_SIZE) *e = (dcopy){addr, size, g_wgen, o, g_nwlog};
    return o;
}
static uint64_t g_tex_miss[4];   // texture slots read as the black dummy: 0 no handle bound, 1 handle not registered, 2 texture not created, 3 no view (logged with the frame stats)
// Bindless handles a shader reads from a uniform buffer (tex_ubo / samp_ubo): the word in the bound buffer as guest memory holds it now (the command list's in-order writes
// are applied as it is walked, so this is what the GPU would read at this draw). A separate-sampler handle (type 1) is two words, the texture's and the sampler's, which the
// hardware ORs before taking either ID (as nvn_refl.m pairs them for the Metal host): FINAL FANTASY XII THE ZODIAC AGE's scene composite keeps the depth texture's ID in the
// sampler word. IDs are numbered in the descriptor pools bound when the draw runs (OP_SET_TEX_POOL / OP_SET_SAMP_POOL), as nvn_mtl.m ubo_tex_id
static uint32_t ubo_word(int stage, int reg, int off) {
    if (stage < 0 || stage >= 6 || reg < 0 || reg >= 16 || off < 0) return 0;
    const p_bind_buf *u = &S.ubo[stage][reg]; uint32_t v = 0;
    if (u->addr && (uint64_t)off + 4 <= u->size) memcpy(&v, (const void *)(uintptr_t)(u->addr + (uint64_t)off), 4);
    return v;
}
static uint32_t texu_handle(int stage, const sbind *b) {
    uint32_t h = ubo_word(stage, b->reg, b->off);
    if (b->skind == S_SAMP_UBO && b->soff >= 0 && (b->sreg != b->reg || b->soff != b->off)) h |= ubo_word(stage, b->sreg, b->soff);
    return h;
}
static uint64_t texu_slot(int stage, const sbind *b) {   // as a bound slot's handle: texture ID | sampler ID << 20, both in the host table
    uint32_t h = texu_handle(stage, b), t = h & 0xfffff, sm = h >> 20;
    return (uint64_t)(t ? t + (uint32_t)S.tex_base : 0) | (uint64_t)(sm ? sm + (uint32_t)S.samp_base : 0) << 20;
}
static VkImageView tex_slot(int stage, uint64_t h, VkSampler *sm, int *ok, vtex **vo) {
    int tid = (int)(h & 0xfffff); *ok = 0; if (vo) *vo = NULL;
    if (!tid) { g_tex_miss[0]++; return g_dummy_view; }
    pthread_mutex_lock(&g_rmu);
    treg_t tr = tid < g_ntreg ? g_treg[tid] : (treg_t){0};
    pthread_mutex_unlock(&g_rmu);
    if (!tr.ok) { g_tex_miss[1]++; return g_dummy_view; }
    vtex *v = tex_get(&tr.t, 0); if (!v) { g_tex_miss[2]++; return g_dummy_view; }
    if (vo) *vo = v;
    VkImageView view = tex_view(v, &tr.t, &tr.v, tr.has_v, -1, 0);
    (void)sm; (void)stage; *ok = view != VK_NULL_HANDLE;
    if (!view) g_tex_miss[3]++;
    return view ? view : g_dummy_view;
}
// Same-pass feedback: a draw that samples a colour attachment of its own pass. On the Switch it reads what was there before the draw; Vulkan leaves it undefined, and the Mali-G57
// returned garbage (Super Mario Odyssey's final pass, agl's post-effect program sampling the image it writes, drew yellow, green and red blotches over the picture; the Metal host
// needs the same for 3D World and Captain Toad, profile key feedback_snap). Here it holds for every game: the pass is split before such a draw, the attachment's level and layer
// copied into a snapshot image (one per format and size, reused), and the draw samples the snapshot. Not done when the draw's colour mask leaves that attachment alone, nor again
// while no draw has written the attachment since its last snapshot. A 3D attachment is left as it was
typedef struct { VkFormat f; uint32_t w, h; VkImage img; VkDeviceMemory m; VkImageView view; vtex *src; uint32_t lv, ly; } fbsnap_t;
static fbsnap_t g_fbs[16]; static int g_nfbs;
static vtex *g_fb_src[8]; static VkImageView g_fb_view[8]; static int g_fb_n;
static uint64_t g_fb_copies;
static fbsnap_t *fb_snap_for(vtex *v, int i) {   // the snapshot image for attachment i of the current pass, taken now unless the one it holds is still current
    uint32_t lv = P.clv[i], ly = P.cly[i], w = v->w >> lv ? v->w >> lv : 1, h = v->h >> lv ? v->h >> lv : 1;
    fbsnap_t *e = NULL;
    for (int k = 0; k < g_nfbs; k++) if (g_fbs[k].src == v && g_fbs[k].lv == lv && g_fbs[k].ly == ly && g_fbs[k].f == v->f.f && g_fbs[k].w == w && g_fbs[k].h == h) { e = &g_fbs[k]; break; }
    if (e && !P.cwr[i]) return e;
    if (!e) for (int k = 0; k < g_nfbs; k++) if (g_fbs[k].f == v->f.f && g_fbs[k].w == w && g_fbs[k].h == h) { e = &g_fbs[k]; break; }   // same shape: reuse its image
    if (!e) {
        if (g_nfbs == 16) { RED_ONCE(40, "same-pass feedback: more than 16 snapshot shapes; the draw samples its attachment directly"); return NULL; }
        e = &g_fbs[g_nfbs]; memset(e, 0, sizeof *e); e->f = v->f.f; e->w = w; e->h = h;
        VkImageCreateInfo ii = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, 0, VK_IMAGE_TYPE_2D, v->f.f, {w, h, 1}, 1, 1, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL,
                                VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_IMAGE_LAYOUT_UNDEFINED};
        if (vkCreateImage(g_dev, &ii, NULL, &e->img) != VK_SUCCESS) { RED_ONCE(41, "same-pass feedback: snapshot image not created"); return NULL; }
        VkMemoryRequirements mr; vkGetImageMemoryRequirements(g_dev, e->img, &mr); e->m = mem_alloc(mr, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT); VKCHECK(vkBindImageMemory(g_dev, e->img, e->m, 0));
        VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, e->img, VK_IMAGE_VIEW_TYPE_2D, v->f.f, {0}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        VKCHECK(vkCreateImageView(g_dev, &vi, NULL, &e->view));
        to_general(C->cb, e->img, VK_IMAGE_ASPECT_COLOR_BIT); g_nfbs++;
        LOG("vk: same-pass feedback: snapshot %ux%u format %d made (serial %llu)", w, h, (int)v->f.f, (unsigned long long)v->serial);
    }
    VkImageCopy c = {{VK_IMAGE_ASPECT_COLOR_BIT, lv, ly, 1}, {0, 0, 0}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {w, h, 1}};
    vkCmdCopyImage(C->cb, v->img, VK_IMAGE_LAYOUT_GENERAL, e->img, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
    e->src = v; e->lv = lv; e->ly = ly; P.cwr[i] = 0; g_fb_copies++;
    return e;
}
static void fb_prepare(shader_t *fs) {   // before a draw (pass open): find the sampled colour attachments it writes, snapshot them, and have write_set bind the snapshots
    g_fb_n = 0;
    int want[8] = {0}, any = 0;
    for (int i = 0; i < fs->nb; i++) {
        const sbind *b = &fs->b[i];
        // a bound slot, or a handle read from a uniform buffer: FINAL FANTASY XII THE ZODIAC AGE's full-screen passes sample their own attachment through such handles
        if (b->type != BT_CIS || !((b->kind == K_TEX && b->reg >= 0 && b->reg < NTEX) || (b->kind == K_TEXU && b->count == 1))) continue;
        int tid = (int)((b->kind == K_TEXU ? texu_slot(1, b) : S.tex[1][b->reg]) & 0xfffff); if (!tid) continue;
        pthread_mutex_lock(&g_rmu); treg_t tr = tid < g_ntreg ? g_treg[tid] : (treg_t){0}; pthread_mutex_unlock(&g_rmu);
        if (!tr.ok) continue;
        vtex *v = tex_get(&tr.t, 0); if (!v) continue;
        for (int a = 0; a < P.n; a++) if (P.ct[a] == v && P.cv[a] && v->d <= 1 && (S.chmask.mask >> (4 * a) & 0xf)) { want[a] = 1; any = 1; }
    }
    if (!any) return;
    int need = 0; for (int a = 0; a < P.n; a++) if (want[a] && P.cwr[a]) need = 1;
    if (need) pass_end();
    VkCommandBuffer cb = C->cb;
    for (int a = 0; a < P.n && g_fb_n < 8; a++) if (want[a]) { fbsnap_t *e = fb_snap_for(P.ct[a], a); if (e) { g_fb_src[g_fb_n] = P.ct[a]; g_fb_view[g_fb_n++] = e->view; } }
    if (need) { barrier_xfer(cb); pass_begin(); }   // the snapshot copies (fb_snap_for), for the draw's sampling
}
static gw_t g_wb[16]; static int g_nwb;   // the storage buffers the last compute descriptor set bound (write_set), which its dispatch writes
// Texel buffers (NVN buffer textures: Unreal's light lists, particle and skinning data, its GPU scene): the texture's elements copied into the ring -- at the texel-buffer offset
// alignment, which a game's own pool offsets need not keep (the Metal host shadows them for the same reason) -- and a buffer view made over the copy for this submission. A
// compute shader's storage texel buffer is written back like its storage buffers (g_wb); a draw's writes are lost. Views are cached per submission by buffer, offset, format and size
static struct { VkBuffer b; VkDeviceSize off, n; VkFormat f; uint64_t gen; VkBufferView v; } g_bvc[1024];
static VkBufferView bview(VkBuffer b, VkDeviceSize off, VkDeviceSize n, VkFormat f) {
    unsigned h = (unsigned)(((uintptr_t)b >> 4) ^ (off >> 4) ^ (off >> 14) ^ (unsigned)f * 2654435761u) % 1024;
    if (g_bvc[h].gen == g_bvgen && g_bvc[h].b == b && g_bvc[h].off == off && g_bvc[h].n == n && g_bvc[h].f == f) return g_bvc[h].v;
    VkBufferViewCreateInfo ci = {VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO, NULL, 0, b, f, off, n};
    VkBufferView v; if (vkCreateBufferView(g_dev, &ci, NULL, &v) != VK_SUCCESS) { RED_ONCE(70, "texel buffer view (format %d, %llu bytes) not created", (int)f, (unsigned long long)n); return VK_NULL_HANDLE; }
    if (C->nbv == C->cbv) { C->cbv = C->cbv ? 2 * C->cbv : 256; C->bv = realloc(C->bv, C->cbv * sizeof *C->bv); }
    C->bv[C->nbv++] = v; g_bvc[h] = (typeof(g_bvc[0])){b, off, n, f, g_bvgen, v};
    return v;
}
static VkFormatProperties fmt_props(VkFormat f) {   // vkGetPhysicalDeviceFormatProperties, cached (asked for every texel buffer of every draw)
    static struct { VkFormat f; int ok; VkFormatProperties p; } c[512];
    unsigned h = (unsigned)f * 2654435761u % 512;
    for (int k = 0; k < 512; k++, h = (h + 1) % 512) {
        if (c[h].ok && c[h].f == f) return c[h].p;
        if (!c[h].ok) { c[h].f = f; vkGetPhysicalDeviceFormatProperties(g_pd, f, &c[h].p); c[h].ok = 1; return c[h].p; }
    }
    VkFormatProperties p; vkGetPhysicalDeviceFormatProperties(g_pd, f, &p); return p;
}
static uint64_t g_tbuf_miss[3]; static uint32_t g_ws_zero_tb;   // g_ws_zero_tb: texel buffer elements the last write_sets bound to the zero stand-in (the trace)   // texel buffer slots read as zeros: 0 nothing bound, 1 not a buffer texture / no format, 2 format not usable as a texel buffer here
static VkBufferView tbuf_view(int stage, uint64_t h, int storage) {
    int tid = (int)(h & 0xfffff); if (!tid) { g_tbuf_miss[0]++; return VK_NULL_HANDLE; }
    pthread_mutex_lock(&g_rmu); treg_t tr = tid < g_ntreg ? g_treg[tid] : (treg_t){0}; pthread_mutex_unlock(&g_rmu);
    if (!tr.ok || tr.t.d.target != NVN_TEXTURE_TARGET_BUFFER || !tr.t.mem) { g_tbuf_miss[tr.ok ? 1 : 0]++; return VK_NULL_HANDLE; }
    int fmtn = tr.has_v && tr.v.set_format ? tr.v.format : tr.t.d.format, bw = 1, bh = 1, bpb = nvn_fmt_bpb(fmtn, &bw, &bh);
    fmt_t f = fmt_of(fmtn);
    if (!f.f || f.dec || f.depth || fmt_block(f.f) || !bpb || bw != 1 || bh != 1) { g_tbuf_miss[1]++; RED_ONCE(71, "buffer texture format %#x: no texel buffer format", fmtn); return VK_NULL_HANDLE; }
    VkFormatProperties fp = fmt_props(f.f);
    if (!(fp.bufferFeatures & (storage ? VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT : VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT))) {
        g_tbuf_miss[2]++; static int n; if (n++ < 4) LOG("[RED] vk: buffer texture format %#x (Vulkan %d) is not a %s texel buffer format on this GPU: reads zero", fmtn, (int)f.f, storage ? "storage" : "uniform"); return VK_NULL_HANDLE; }
    uint64_t el = tr.t.d.width ? tr.t.d.width : 1; if (el > g_props.limits.maxTexelBufferElements) el = g_props.limits.maxTexelBufferElements;
    uint64_t n = el * (uint64_t)bpb, addr = (uint64_t)(uintptr_t)tr.t.mem;
    VkDeviceSize al = g_props.limits.minTexelBufferOffsetAlignment; if (al < 16) al = 16;
    VkBuffer b = VK_NULL_HANDLE; VkDeviceSize off = 0; uint8_t *p = NULL;
    int ci = g_ci, from_gw = 0;
    if (gw_find(addr, n, al, &b, &off, &p)) { ci = g_gw_ci; from_gw = 1; }   // what a dispatch wrote (this submission's, or an earlier one's still running)
    else { off = copy_in(addr, n, al); if (off != VK_WHOLE_SIZE && off % al) off = ring_put((const void *)(uintptr_t)addr, n, al);   // a cached copy at a smaller alignment
           if (off == VK_WHOLE_SIZE) return VK_NULL_HANDLE; b = C->ring; p = C->ringp + off; }
    if (storage && stage == 5 && g_nwb < 16) g_wb[g_nwb++] = (gw_t){addr, n, b, off, p, (uint64_t)from_gw, ci};   // fence field: 1 = GPU-written source (see dispatch)
    return bview(b, off, n, f.f);
}
// Element k of a dynamic slot array: the texture bound to slot k, combined, else separate (as nvn_mtl.m NVN_RB_TEXD)
static uint64_t dyn_slot(int stage, int k) { return k >= 0 && k < 128 ? (S.tex[stage][k] ? S.tex[stage][k] : S.tex[stage][128 + k]) : 0; }
static VkDescriptorType dtype(int t) {
    static const VkDescriptorType DT[] = {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER,
                                          VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_DESCRIPTOR_TYPE_SAMPLER};
    return t >= 0 && t < (int)(sizeof DT / sizeof *DT) ? DT[t] : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
}
// The sampler a texture binding pairs with (a combined image-sampler, or a separate sampler binding)
static VkSampler bind_samp(int stage, const sbind *b, uint64_t th, int el) {
    int r = b->sreg >= 0 ? b->sreg + el : -1;
    int sid = b->skind == S_SAMP && r >= 0 && r < 128 ? (int)(S.samp[stage][r] >> 20) : b->skind == S_SAMP_TEX && b->reg >= 0 && b->reg + el < NTEX ? (int)(S.tex[stage][b->reg + el] >> 20) : b->skind == S_SAMP_ID ? b->sreg :
              b->skind == S_SAMP_UBO ? (b->kind == K_TEXU ? (int)(th >> 20) : (int)(ubo_word(stage, b->sreg, b->soff) >> 20) ? (int)(ubo_word(stage, b->sreg, b->soff) >> 20) + S.samp_base : 0) : 0;
    pthread_mutex_lock(&g_rmu); VkSampler sm = samp_get(sid); pthread_mutex_unlock(&g_rmu);
    return sm;
}
// Every element of every binding is written: what is bound where it can be found, a stand-in of the binding's type where not. A descriptor left unwritten is undefined, and a
// shader reading one can take the GPU down (FINAL FANTASY RESONANCE lost the Adreno 825 after about 1,200 frames, its Unreal shaders' texel buffers and sampler arrays unwritten)
// Descriptor sets of this submission by what they hold (layout and every descriptor): a draw whose bindings resolve to the same resources reuses the set instead of allocating and
// writing one again -- Unreal's shaders have 60-80 descriptors a stage (32-entry texel buffer and sampler arrays), and writing every one for every draw cost most of the recording time
static struct { uint64_t key, gen; VkDescriptorSetLayout dsl; VkDescriptorSet ds; } g_dsc[8192];
static uint64_t g_dsc_hit, g_dsc_miss;
static uint64_t g_t_ws, g_t_draw, g_t_disp, g_t_sub;   // time in write_set, in draws (with their write_set), in dispatches, in vkQueueSubmit (the frame stats line)
static VkDescriptorSet write_set_(shader_t *s, int stage);
static int g_vs_haz;   // the vertex shader's write_set met a texture a pass wrote with vertex shaders left out of its end barrier (draw_vs_hazard)
static uint32_t g_dyno[64]; static uint32_t g_ndyno;   // the dynamic offsets of the last write_set, in binding order (for vkCmdBindDescriptorSets)
static VkDescriptorSet write_set(shader_t *s, int stage) { uint64_t t0 = now_ns(); VkDescriptorSet d = write_set_(s, stage); g_t_ws += now_ns() - t0; return d; }
static VkDescriptorSet write_set_(shader_t *s, int stage) {
    if (stage == 5) g_nwb = 0;
    g_ndyno = 0;
    VkDescriptorSet ds = VK_NULL_HANDLE;
    enum { NE = 4096 };
    static VkWriteDescriptorSet w[64]; static VkDescriptorBufferInfo bi[NE]; static VkDescriptorImageInfo ii[NE]; static VkBufferView bv[NE]; int nw = 0, ne = 0;
    for (int i = 0; i < s->nb; i++) {
        const sbind *b = &s->b[i];
        uint32_t cnt = b->count ? b->count : 1;
        if (ne + (int)cnt > NE) { RED_ONCE(72, "shader %s: more than %d descriptors in one set: the rest are left unwritten", s->key, NE); break; }
        VkWriteDescriptorSet *x = &w[nw]; *x = (VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, ds, b->binding, 0, cnt, dtype(b->type)};
        if (b->type == BT_UBO || b->type == BT_SSBO) {
            for (uint32_t e = 0; e < cnt; e++) bi[ne + e] = (VkDescriptorBufferInfo){b->type == BT_SSBO ? g_scratch_buf : g_zero_buf, 0, 65536};
            VkDescriptorBufferInfo *d = &bi[ne];
            if (b->kind == K_CBUF && b->reg >= 0 && b->reg < 16) {
                const p_bind_buf *u = &S.ubo[stage][b->reg];
                uint64_t n = u->size > 65536 ? 65536 : u->size;
                VkBuffer gb; VkDeviceSize go;
                if (u->addr && n && gw_find(u->addr, n, g_props.limits.minUniformBufferOffsetAlignment ? g_props.limits.minUniformBufferOffsetAlignment : 256, &gb, &go, NULL)) *d = (VkDescriptorBufferInfo){gb, go, n};
                else if (u->addr && n) { VkDeviceSize o = copy_in(u->addr, n, 256); if (o != VK_WHOLE_SIZE) *d = (VkDescriptorBufferInfo){C->ring, o, n}; }
            } else if (b->kind == K_C1 && s->c1_len) { VkDeviceSize o = ring_put(s->c1, s->c1_len, 256); if (o != VK_WHOLE_SIZE) *d = (VkDescriptorBufferInfo){C->ring, o, s->c1_len}; }
            else if (b->kind == K_SUPPORT) *d = (VkDescriptorBufferInfo){g_support_buf, 0, 4096};
            else if ((b->kind == K_SBUF || b->kind == K_UBOP) && b->reg >= 0 && b->reg < 16) {   // a copy, or the GPU's own earlier write; a dispatch's are written back (dispatch)
                const p_bind_buf *u = b->kind == K_SBUF ? &S.ssbo[stage][b->reg] : &S.ubo[stage][b->reg];
                VkBuffer gb = VK_NULL_HANDLE; VkDeviceSize go = 0; uint8_t *gp = NULL; int gci = g_ci;
                VkDeviceSize al = b->type == BT_UBO ? g_props.limits.minUniformBufferOffsetAlignment : g_props.limits.minStorageBufferOffsetAlignment; if (!al) al = 256;
                int from_gw = 0;
                if (u->addr && u->size && gw_find(u->addr, u->size, al, &gb, &go, &gp)) { *d = (VkDescriptorBufferInfo){gb, go, u->size}; gci = g_gw_ci; from_gw = 1; }
                else if (u->addr && u->size) { VkDeviceSize o = copy_in(u->addr, u->size, 256); if (o != VK_WHOLE_SIZE) { *d = (VkDescriptorBufferInfo){C->ring, o, u->size}; gb = C->ring; go = o; gp = C->ringp + o; } }
                // A compute shader's storage buffer is what its dispatch may write: only a storage-buffer binding of a module that stores to one (a uniform binding, or a module
                // that only reads, writes nothing, and its range used to be written back whole at completion over whatever the CPU had written there since)
                if (stage == 5 && gb && b->type == BT_SSBO && s->ssbo_wr && g_nwb < 16) g_wb[g_nwb++] = (gw_t){u->addr, u->size, gb, go, gp, (uint64_t)from_gw, gci};
                if (stage != 5 && s->ssbo_wr) { static const shader_t *seen[8]; static int nseen; int k = 0; while (k < nseen && seen[k] != s) k++;
                                  if (k == nseen && nseen < 8) { seen[nseen++] = s; LOG("[RED] vk: shader %s (stage %d) binds storage buffer c%d at %#llx size %llu: what a draw writes to it is lost", s->key, stage, b->reg, (unsigned long long)u->addr, (unsigned long long)u->size); } }
            } else if (b->kind == K_DRVCB) {   // driver segments: c2 holds the bound texture / sampler handles (as nvn_mtl.m fills them), c0 the storage buffer table
                uint8_t seg[4096]; memset(seg, 0, sizeof seg);
                if (b->off == 2 && s->dyn) {   // tex_dyn_slots (as nvn_mtl.m): the tables hold slot indices, which the shader's arithmetic turns into indices of its slot arrays (dyn_slot)
                    for (int r = 0; r < 32; r++) { uint64_t h = S.tex[stage][r]; uint32_t v = !h ? 0 : (uint32_t)r | (h >> 20 ? (uint32_t)r << 20 : 0); memcpy(seg + 0x20 + 8 * r, &v, 4); }
                    for (int r = 0; r < 8; r++) { uint32_t v = (uint32_t)S.img[stage][r]; memcpy(seg + 0x120 + 8 * r, &v, 4); }
                    for (int r = 0; r < 128 && 0x168 + 8 * r + 4 <= 0x568; r++) { uint32_t v = S.tex[stage][128 + r] ? (uint32_t)r : 0; memcpy(seg + 0x168 + 8 * r, &v, 4); }
                    for (int r = 0; r < 128 && 0x568 + 8 * r + 4 <= (int)sizeof seg; r++) { uint32_t v = S.samp[stage][r] ? (uint32_t)r << 20 : 0; memcpy(seg + 0x568 + 8 * r, &v, 4); }
                } else if (b->off == 2) {
                    for (int r = 0; r < 32 && 0x20 + 8 * r < 0x120; r++) { uint32_t v = (uint32_t)S.tex[stage][r]; memcpy(seg + 0x20 + 8 * r, &v, 4); }
                    for (int r = 0; r < 8; r++) { uint32_t v = (uint32_t)S.img[stage][r]; memcpy(seg + 0x120 + 8 * r, &v, 4); }
                    for (int r = 0; r < 128 && 0x168 + 8 * r + 4 <= 0x568; r++) { uint32_t v = (uint32_t)S.tex[stage][128 + r]; memcpy(seg + 0x168 + 8 * r, &v, 4); }
                    for (int r = 0; r < 128 && 0x568 + 8 * r + 4 <= (int)sizeof seg; r++) { uint32_t v = (uint32_t)S.samp[stage][r]; memcpy(seg + 0x568 + 8 * r, &v, 4); }
                } else {
                    int base = stage == 0 ? 0x110 : stage == 1 ? 0x510 : stage == 5 ? 0x310 : -1;
                    if (base >= 0) for (int r = 0; r < 16; r++) { uint64_t a = S.ssbo[stage][r].addr; uint32_t n = (uint32_t)S.ssbo[stage][r].size; memcpy(seg + base + 16 * r, &a, 8); memcpy(seg + base + 16 * r + 8, &n, 4); }
                    if (stage != 5) { const float mp = 2047.0f; memcpy(seg + 0x610, &mp, 4); }
                }
                VkDeviceSize o = ring_put(seg, sizeof seg, 256); if (o != VK_WHOLE_SIZE) *d = (VkDescriptorBufferInfo){C->ring, o, sizeof seg};
            }
            x->pBufferInfo = d;
            if (s->dynu[i]) { x->descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC; g_dyno[g_ndyno++] = (uint32_t)d->offset; d->offset = 0; }   // the offset goes with the bind
        } else if (b->type == BT_CIS || b->type == BT_TEX || b->type == BT_SAMP || b->type == BT_SIMG) {
            int simg = b->type == BT_SIMG;
            for (uint32_t e = 0; e < cnt; e++) {
                VkSampler sm = b->type == BT_TEX || simg ? VK_NULL_HANDLE : g_dummy_samp; VkImageView view = b->type == BT_SAMP ? VK_NULL_HANDLE : simg ? g_dummy_simg_view : g_dummy_view;
                uint64_t th = 0; int have = 0;
                int dyn = s->dyn && cnt > 1 && b->reg < 0;
                if (b->type != BT_SAMP && !simg && (dyn || (b->kind == K_TEX && b->reg >= 0 && b->reg + (int)e < NTEX) || (b->kind == K_TEXU && cnt == 1))) {
                    th = dyn ? dyn_slot(stage, (int)e) : b->kind == K_TEXU ? texu_slot(stage, b) : S.tex[stage][b->reg + e]; have = !dyn;
                    int ok; vtex *tv; view = tex_slot(stage, th, &sm, &ok, &tv);
                    if (stage == 0 && tv && tv->vhaz && tv->vhaz_epoch == g_vbar_epoch) g_vs_haz = 1;   // written by a pass whose end barrier left vertex shaders out (DM_VK_PASS_BARRIER)
                    if (stage == 1 && tv) for (int q = 0; q < g_fb_n; q++) if (tv == g_fb_src[q]) { view = g_fb_view[q]; break; }   // same-pass feedback: the snapshot (fb_prepare)
                } else if (simg && b->kind == K_IMG && b->reg >= 0 && b->reg + (int)e < 32) {   // an image a shader writes: a single-level view with no swizzle, of an image made for storage
                    int tid = (int)(S.img[stage][b->reg + e] & 0xfffff);
                    pthread_mutex_lock(&g_rmu); treg_t tr = tid && tid < g_ntreg ? g_treg[tid] : (treg_t){0}; pthread_mutex_unlock(&g_rmu);
                    vtex *v = tr.ok && tr.t.d.target != NVN_TEXTURE_TARGET_BUFFER ? tex_get(&tr.t, 1) : NULL;
                    VkImageView iv = v && v->storage ? tex_view(v, &tr.t, &tr.v, tr.has_v, -1, 2) : VK_NULL_HANDLE;
                    if (iv) { view = iv; v->copied = 1; }   // written on the GPU: hibernation reads it back (mtl_hib_readback)
                    else RED_ONCE(73, "shader %s: image slot %d is not a storage-capable image here: its writes are lost", s->key, b->reg + (int)e);
                }
                if (b->type == BT_CIS && have) sm = bind_samp(stage, b, th, (int)e);
                else if (b->type == BT_CIS && dyn && (th >> 20)) { pthread_mutex_lock(&g_rmu); sm = samp_get((int)(th >> 20)); pthread_mutex_unlock(&g_rmu); }
                else if (b->type == BT_SAMP && dyn) { pthread_mutex_lock(&g_rmu); sm = samp_get((int)(S.samp[stage][e < 128 ? e : 0] >> 20)); pthread_mutex_unlock(&g_rmu); }   // sampler slot k
                else if (b->type == BT_SAMP && b->skind != S_NONE) sm = bind_samp(stage, b, 0, (int)e);
                ii[ne + e] = (VkDescriptorImageInfo){sm, view, VK_IMAGE_LAYOUT_GENERAL};
            }
            x->pImageInfo = &ii[ne];
        } else if (b->type == BT_TBUF || b->type == BT_UTB || b->type == BT_STB) {
            int st = b->type == BT_STB;
            for (uint32_t e = 0; e < cnt; e++) {
                VkBufferView v = VK_NULL_HANDLE; int r = b->reg >= 0 ? b->reg + (int)e : -1;
                if (s->dyn && cnt > 1 && b->reg < 0) { uint64_t h = dyn_slot(stage, (int)e); if (h) v = tbuf_view(stage, h, st); }   // slot k, when it holds a buffer texture
                else if (b->kind == K_TBUF && r >= 0 && r < NTEX) v = tbuf_view(stage, S.tex[stage][r], st);
                else if (b->kind == K_IMG && r >= 0 && r < 32) v = tbuf_view(stage, S.img[stage][r], st);
                bv[ne + e] = v ? v : st ? g_dummy_stb : g_dummy_utb; if (!v && r >= 0) g_ws_zero_tb++;
            }
            x->pTexelBufferView = &bv[ne];
        } else { RED_ONCE(22, "descriptor type %d not implemented yet (shader %s)", b->type, s->key); continue; }
        if (g_plog) {   // what each binding resolved to (DM_VK_PASSLOG / DM_VK_DUMP_FILE frames)
            C->plog = 1; char ln[512]; int o = snprintf(ln, sizeof ln, "plog:    %s b%u t%d k%d r%d", s->key, b->binding, b->type, b->kind, b->reg);
            if ((b->type == BT_UBO || b->type == BT_SSBO) && b->reg >= 0 && b->reg < 16 && (b->kind == K_CBUF || b->kind == K_SBUF || b->kind == K_UBOP)) {
                const p_bind_buf *u = b->kind == K_SBUF ? &S.ssbo[stage][b->reg] : &S.ubo[stage][b->reg]; float f[4] = {0};
                if (u->addr && u->size >= 16) memcpy(f, (const void *)(uintptr_t)u->addr, 16);
                o += snprintf(ln + o, sizeof ln - o, " buf %#llx +%llu %s guest %g %g %g %g", (unsigned long long)u->addr, (unsigned long long)u->size, bi[ne].buffer == C->ring ? "ring" : bi[ne].buffer == g_zero_buf || bi[ne].buffer == g_scratch_buf ? "zero" : "gpu-written", f[0], f[1], f[2], f[3]);
            } else if (b->type == BT_CIS || b->type == BT_TEX || b->type == BT_SIMG) {
                uint64_t h = b->type == BT_SIMG ? (b->reg >= 0 && b->reg < 32 ? S.img[stage][b->reg] : 0) : b->kind == K_TEXU ? texu_slot(stage, b) : b->reg >= 0 && b->reg < NTEX ? S.tex[stage][b->reg] : 0;
                int tid = (int)(h & 0xfffff); pthread_mutex_lock(&g_rmu); treg_t tr = tid && tid < g_ntreg ? g_treg[tid] : (treg_t){0}; pthread_mutex_unlock(&g_rmu);
                { vtex *pv = NULL; pthread_mutex_lock(&g_rmu); if (tr.ok) pv = tex_find(tr.t.serial); pthread_mutex_unlock(&g_rmu);
                  o += snprintf(ln + o, sizeof ln - o, " lv %u view %d+%d gpu %d gpu_lv %#x", tr.t.d.levels, tr.has_v && tr.v.set_levels ? tr.v.base_level : 0, tr.has_v && tr.v.set_levels ? tr.v.nlevels : 0,
                                pv ? pv->gpu : -1, pv ? pv->gpu_lv : 0);
                  int sid = (int)(h >> 20); if (b->type == BT_CIS && sid > 0 && sid < g_nsreg && g_sreg_ok[sid]) { const samp_par *sp = &g_sreg[sid]; o += snprintf(ln + o, sizeof ln - o, " samp min %d mag %d lod %g..%g bias %g", sp->minf, sp->magf, sp->lod_min, sp->lod_max, sp->lod_bias); } }
                o += snprintf(ln + o, sizeof ln - o, " tex %llu %ux%u fmt %#x%s%s", tr.ok ? (unsigned long long)tr.t.serial : 0ull, tr.t.d.width, tr.t.d.height, tr.t.d.format,
                              ii[ne].imageView == g_dummy_view || ii[ne].imageView == g_dummy_simg_view ? " DUMMY" : "", tr.has_v && tr.v.set_dsmode && tr.v.dsmode ? " stencil" : "");
            } else if (b->type == BT_TBUF || b->type == BT_UTB || b->type == BT_STB) {
                int z = 0; for (uint32_t e = 0; e < cnt; e++) z += bv[ne + e] == g_dummy_utb || bv[ne + e] == g_dummy_stb;
                o += snprintf(ln + o, sizeof ln - o, " texel buffers %u, %d zero", cnt, z);
            }
            LOG("%s", ln);
        }
        ne += (int)cnt; nw++;
    }
    uint64_t key = 0xcbf29ce484222325ull;
    #define HMIX(v) key = (key ^ (uint64_t)(v)) * 0x100000001b3ull
    HMIX((uintptr_t)s->dsl);
    for (int k = 0; k < nw; k++) {
        const VkWriteDescriptorSet *x = &w[k]; HMIX(x->dstBinding | (uint64_t)x->descriptorCount << 32); HMIX(x->descriptorType);
        for (uint32_t e = 0; e < x->descriptorCount; e++) {
            if (x->pBufferInfo) { HMIX((uintptr_t)x->pBufferInfo[e].buffer); HMIX(x->pBufferInfo[e].offset); HMIX(x->pBufferInfo[e].range); }
            else if (x->pImageInfo) { HMIX((uintptr_t)x->pImageInfo[e].sampler); HMIX((uintptr_t)x->pImageInfo[e].imageView); }
            else if (x->pTexelBufferView) HMIX((uintptr_t)x->pTexelBufferView[e]);
        }
    }
    #undef HMIX
    typeof(g_dsc[0]) *ce = &g_dsc[key % 8192];
    if (ce->gen == g_bvgen && ce->key == key && ce->dsl == s->dsl) { g_dsc_hit++; return ce->ds; }
    VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, C->dps[C->idp], 1, &s->dsl};
    while (vkAllocateDescriptorSets(g_dev, &ai, &ds) != VK_SUCCESS) {   // this pool is full: the next of the chain, made when there is none
        if (++C->idp == C->ndp) { if (C->ndp >= 256) { RED_ONCE(20, "256 descriptor pools in one submission: draws are dropped"); C->idp--; return VK_NULL_HANDLE; }
                                  C->dps = realloc(C->dps, (C->ndp + 1) * sizeof *C->dps); C->dps[C->ndp++] = dpool_new(); }
        ai.descriptorPool = C->dps[C->idp];
    }
    for (int k = 0; k < nw; k++) w[k].dstSet = ds;
    vkUpdateDescriptorSets(g_dev, (uint32_t)nw, w, 0, NULL);
    *ce = (typeof(g_dsc[0])){key, g_bvgen, s->dsl, ds}; g_dsc_miss++;
    return ds;
}
static _Atomic uint64_t g_wait_ns, g_rec_ns, g_pwait_ns;   // time the submit thread waited for the GPU, spent recording, and the present waited for its slot
static uint64_t g_stat_draws, g_stat_skip, g_skip_why[8];   // why: 0 no program, 1 shader missing / rejected, 2 no render target, 3 indirect, 4 primitive, 5 pipeline, 6 descriptors
#define SKIP(w) do { g_stat_skip++; g_skip_why[w]++; return; } while (0)
// ---------- Occlusion queries (NVN_COUNTER_TYPE_SAMPLES_PASSED) ----------
// NVN counts the samples that pass from ResetCounter on, and ReportCounter writes the count (and a timestamp) when the GPU completes. Here every draw made while the counter counts gets
// an occlusion query of its own (a query must begin and end inside one render pass), a reset starts a new list, and a report hands the list to the completion, which sums the results
// into the report. Until now every report said 0 samples: Unreal's hardware occlusion culling (its 36-index bounding boxes) took everything it tests as hidden, and FINAL FANTASY
// RESONANCE's field showed only its landscape -- no characters, trees or fires. The Metal host had the same bug once (nvn_mtl.m, visibility result buffers)
static int g_occ_on; static uint32_t *g_occ; static size_t g_nocc, g_cocc;
// The counter is the queue's, cumulative as on NVN: the draws counted since the last report or reset form a segment, which the completion adds in submission order (a reset sets it to 0,
// a report writes it out), so a game that reports before and after a query and subtracts sees the same as one that resets; draws after a submission's last report carry over (occ_flush)
static void occ_seg(uint64_t addr, int reset) {
    size_t n = g_occ_over ? SIZE_MAX : g_nocc; uint32_t *l = NULL;
    if (n != SIZE_MAX && n) { l = malloc(n * sizeof *l); memcpy(l, g_occ, n * sizeof *l); }
    if (n || addr || reset) post_add((post_t){4, NULL, (uint32_t)reset, 0, addr, 1, (const uint8_t *)l, n});
    g_nocc = 0; g_occ_over = 0;
}
static uint64_t g_occ_nrep, g_occ_nrst, g_occ_ndraw;   // the frame stats line
static void occ_reset(void) { g_occ_on = 1; g_occ_nrst++; occ_seg(0, 1); }
static void occ_report(uint64_t addr) { g_occ_on = 1; g_occ_nrep++; occ_seg(addr, 0); }
static void occ_flush(void) { if (g_occ_on) occ_seg(0, 0); }   // the end of a submission: its last segment, counted only
static int occ_begin(VkCommandBuffer cb) {   // the query of the draw about to be made, or -1
    if (!g_occ_on) return -1;
    if (C->noq >= NOQ) { if (!g_occ_over) RED_ONCE(80, "more than %d occlusion-counted draws in one submission: the rest report as visible", NOQ); g_occ_over = 1; return -1; }
    uint32_t q = C->noq++;
    vkCmdBeginQuery(cb, C->oq, q, g_feat.occlusionQueryPrecise ? VK_QUERY_CONTROL_PRECISE_BIT : 0);
    if (g_nocc == g_cocc) { g_cocc = g_cocc ? 2 * g_cocc : 256; g_occ = realloc(g_occ, g_cocc * sizeof *g_occ); }
    g_occ[g_nocc++] = q; g_occ_ndraw++; return (int)q;
}
static void occ_end(VkCommandBuffer cb, int q) { if (q >= 0) vkCmdEndQuery(cb, C->oq, (uint32_t)q); }
#define DRAWQ(call) do { int q_ = occ_begin(cb); call; occ_end(cb, q_); } while (0)
// Indirect arguments, n bytes at addr: 1 when guest memory holds them and they were read into out (written by the CPU, or by a submission that has completed: gw_find waits for
// one still in flight), 2 when a dispatch of this submission writes them -- not run yet, so the GPU must read them: *b / *off is where
static int ind_args(uint64_t addr, uint32_t n, void *out, VkBuffer *b, VkDeviceSize *off, int gpu_ok) {
    if (gw_find_(addr, n, 4, b, off, NULL, gpu_ok)) return 2;   // gpu_ok (dispatches): an earlier submission's arguments are read by the GPU too; draws wait for them and read them here (no whole-stream copies)   // this submission's own: drawn by the GPU; an earlier one's is waited for and read here (no whole-stream copies)
    memcpy(out, (const void *)(uintptr_t)addr, n); return 1;
}
// The bytes of a pool from addr on (at most cap): the extent of an index buffer an indirect draw reads, whose count only the GPU will know
static uint64_t pool_left(uint64_t addr, uint64_t cap);
// Live diagnostics: DM_VK_CTL_FILE=<path>, a file read every 30 frames (vk_ctl) while the game runs. "skip <first>-<last>" leaves out the draws with those numbers within each frame,
// "skipkey <shader key prefix>" the draws whose vertex or pixel shader key begins so; an empty or missing file leaves every draw in. For finding the draw that paints something
// wrong (FINAL FANTASY RESONANCE's blocky battle enemy) by bisection with screenshots, with no input to the game and no restart
static uint32_t g_frame_draw, g_skip_lo = 1, g_skip_hi; static char g_skip_key[8][17]; static int g_nskip_key;
static int ctl_skip(uint32_t didx, const char *vk, const char *fk) {
    if (didx >= g_skip_lo && didx <= g_skip_hi) return 1;
    for (int i = 0; i < g_nskip_key; i++) { size_t l = strlen(g_skip_key[i]); if ((vk && !strncmp(vk, g_skip_key[i], l)) || (fk && !strncmp(fk, g_skip_key[i], l))) return 1; }
    return 0;
}
static void vk_ctl(uint64_t n) {
    static const char *path = (const char *)1; if (path == (const char *)1) path = getenv("DM_VK_CTL_FILE");
    if (!path || n % 30) return;
    FILE *f = fopen(path, "re"); uint32_t lo = 1, hi = 0; int nk = 0; char ln[128], k[17];
    if (f) { while (fgets(ln, sizeof ln, f)) { unsigned a, b; if (sscanf(ln, "skip %u-%u", &a, &b) == 2) { lo = a; hi = b; } else if (sscanf(ln, "skipkey %16s", k) == 1 && nk < 8) strcpy(g_skip_key[nk++], k); } fclose(f); }
    if (lo != g_skip_lo || hi != g_skip_hi || nk != g_nskip_key) LOG("vk: control file: skipping draws %u-%u%s, %d shader key prefixes (frame %llu)", lo, hi, lo > hi ? " (none)" : "", nk, (unsigned long long)n);
    g_skip_lo = lo; g_skip_hi = hi; g_nskip_key = nk;
}
// A pass kept open across a SetRenderTargets (the hold) joins work the game drew into separate passes; a draw that samples one of the pass's attachments -- the scene depth a
// contact-shadow or ambient-occlusion term reads, a colour target -- would read it while the pass still holds it in tile memory (undefined: FINAL FANTASY RESONANCE's castle
// corridor, the contact shadow along the wall flickering frame to frame). Such a draw ends the pass first, as the Metal host does (nvn_mtl.m draw_samples_rt). Every texture
// binding of the draw's shaders is looked at: slots, handles from uniform buffers, dynamic slot arrays
// Only an attachment cleared or written in this pass counts: one the pass only loaded holds in memory what the tile holds, so sampling it reads the same and the pass goes on
// (the split stores and loads every attachment of the pass again, a full tile store and load on the Adreno). g_rmu is taken once for the whole draw. DM_VK_RT_SPLIT=0: no split
static uint64_t g_nsplit, g_nsplit_skip; static uint64_t g_t_split;
static int rt_split_on(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_VK_RT_SPLIT"); v = !(e && !strcmp(e, "0")); if (!v) LOG("DM_VK_RT_SPLIT=0: a held pass is not ended for a draw that samples one of its attachments"); } return v; }
static int rt_serial_(uint64_t h, uint64_t *ser) {   // caller holds g_rmu
    int tid = (int)(h & 0xfffff); if (!tid) return 0;
    int ok = tid < g_ntreg && g_treg[tid].ok; if (ok) *ser = g_treg[tid].t.serial;
    return ok;
}
static int g_rt_unwritten;   // the draw samples an attachment the pass did not write (counted, no split)
static int is_attachment(uint64_t ser) {
    if (P.dt && P.dt->serial == ser) { if (P.ddrawn) return 1; g_rt_unwritten = 1; }
    for (int i = 0; i < P.n; i++) if (P.ct[i] && P.ct[i]->serial == ser) { if (P.cdrawn[i]) return 1; g_rt_unwritten = 1; }
    return 0;
}
static int shader_reads_rt(shader_t *s, int stage) {   // caller holds g_rmu
    if (!s) return 0;
    for (int i = 0; i < s->nb; i++) {
        const sbind *b = &s->b[i]; if (b->type != BT_CIS && b->type != BT_TEX) continue;
        uint32_t cnt = b->count ? b->count : 1; uint64_t ser;
        if (s->dyn && cnt > 1 && b->reg < 0) { for (uint32_t e = 0; e < cnt && e < 128; e++) if (rt_serial_(dyn_slot(stage, (int)e), &ser) && is_attachment(ser)) return 1; continue; }
        if (b->kind == K_TEXU && cnt == 1) { if (rt_serial_(texu_slot(stage, b), &ser) && is_attachment(ser)) return 1; continue; }
        if (b->kind == K_TEX && b->reg >= 0) for (uint32_t e = 0; e < cnt && b->reg + (int)e < NTEX; e++) if (rt_serial_(S.tex[stage][b->reg + e], &ser) && is_attachment(ser)) return 1;
    }
    return 0;
}
static int draw_reads_rt(shader_t *vs, shader_t *fs, shader_t *gs) {
    if (!rt_split_on()) return 0;
    uint64_t t0 = now_ns(); g_rt_unwritten = 0;
    pthread_mutex_lock(&g_rmu); int r = shader_reads_rt(vs, 0) || shader_reads_rt(fs, 1) || shader_reads_rt(gs, 2); pthread_mutex_unlock(&g_rmu);
    if (!r && g_rt_unwritten) g_nsplit_skip++;
    g_t_split += now_ns() - t0;
    return r;
}
static void draw(const p_draw *dw, const p_draw_idx *di) {
    if (!S.bst[0]) SKIP(0);
    uint32_t didx = g_frame_draw++;   // this draw's number within the frame (the live control file's skip ranges)
    // Indirect draws: the record {count, instances, first, base instance} (indexed: {count, instances, first index, base vertex, base instance}). Read here when the CPU can, and
    // drawn as an ordinary draw (only the vertices it uses copied); written by a dispatch of this very submission (Unreal's GPU particles and culling), drawn by the GPU from the
    // ring with the whole vertex streams bound. An indirect-count multi-draw's record i is drawn while i < its count
    VkBuffer ind_b = VK_NULL_HANDLE; VkDeviceSize ind_o = 0;
    if ((dw && dw->ind) || (di && di->ind)) {
        uint64_t mc = dw ? dw->mdi_cnt : di->mdi_cnt; int mi = dw ? dw->mdi_i : di->mdi_i;
        if (mc) { uint32_t cnt = 0; VkBuffer cb_; VkDeviceSize co;
                  if (ind_args(mc, 4, &cnt, &cb_, &co, 0) == 2) RED_ONCE(35, "an indirect-count multi-draw's count is written by a dispatch of the same submission: each of its draws is made (their records may hold zeros)");
                  else if ((uint32_t)mi >= cnt) return; }
        uint64_t vb = 0; for (int s2 = 0; s2 < 16; s2++) if (S.vbuf[s2].address) vb += S.vbuf[s2].size;   // what a GPU-read draw copies (its whole streams)
        uint32_t a[5] = {0}; int k = ind_args(dw ? dw->ind : di->ind, dw ? 16 : 20, a, &ind_b, &ind_o, vb <= (8u << 20));   // small streams: a running submission's arguments are read by the GPU, not waited for
        if (k == 1) {
            if (!a[0] || !a[1]) { g_stat_draws++; return; }   // nothing drawn (0 instances is none, unlike a direct draw's 0)
            if (a[0] > (1u << 24) || a[1] > (1u << 20)) { RED_ONCE(36, "indirect draw of %u vertices x %u instances: not plausible, skipped", a[0], a[1]); SKIP(3); }
            if (dw) { p_draw t = *dw; t.count = (int32_t)a[0]; t.instances = (int32_t)a[1]; t.first = (int32_t)a[2]; t.base_instance = (int32_t)a[3]; t.ind = t.mdi_cnt = 0; draw(&t, NULL); }
            else { p_draw_idx t = *di; uint32_t isz = di->itype == 0 ? 1 : di->itype == 1 ? 2 : 4;
                   t.count = (int32_t)a[0]; t.instances = (int32_t)a[1]; t.ibuf = di->ibuf + (uint64_t)a[2] * isz; t.base_vertex = (int32_t)a[3]; t.base_instance = (int32_t)a[4]; t.ind = t.mdi_cnt = 0; draw(NULL, &t); }
            return;
        }
        static int once; if (!once++) LOG("vk: an indirect draw's arguments come from a dispatch of the same submission: drawn by the GPU (logged once)");
    }
    shader_t *vs = NULL, *fs = NULL, *gs = NULL;
    for (int st = 0; st < 3; st++) {   // the vertex shader of the program bound to the vertex stage, the fragment shader of the one bound to the fragment stage (cached by program and serial)
        const prog_t *p = S.bst[st]; if (!p) continue;
        static struct { const prog_t *p; uint64_t ser; shader_t *sh[3]; } pc[1024];
        unsigned h = (unsigned)(((uintptr_t)p >> 4) ^ p->serial) % 1024;
        if (pc[h].p != p || pc[h].ser != p->serial) {
            pc[h].p = p; pc[h].ser = p->serial; pc[h].sh[0] = pc[h].sh[1] = pc[h].sh[2] = NULL;
            for (unsigned i = 0; i < p->nstage; i++) { uint8_t md[8]; if (!prog_key(p, i, md)) continue; shader_t *s = shader_get(md); if (s->stage >= 0 && s->stage <= 2) pc[h].sh[s->stage] = s; }
        }
        if (st == 2) gs = pc[h].sh[2]; else if (st) fs = pc[h].sh[1]; else vs = pc[h].sh[0];
    }
    // A geometry stage (FINAL FANTASY XII THE ZODIAC AGE's character shaders: 84 geometry shaders, whose pixel shaders read a varying only the geometry shader writes) runs as
    // Vulkan's own, its resources in set 2, with the GPU's geometryShader. Without it, or for tessellation, the draw goes without those stages as before
    if (gs && (!g_feat.geometryShader || !gs->mod)) { RED_ONCE(33, "geometry stage: %s; drawn without it", !g_feat.geometryShader ? "the GPU has no geometry shaders" : "the shader was not loaded"); gs = NULL; }
    if (S.bst[3] || S.bst[4]) RED_ONCE(32, "tessellation stages are bound: not drawn with them yet");
    if (vk_skip() & SK_DRAW) SKIP(4);
    if (!vs || !vs->mod || (fs && !fs->mod)) SKIP(1);
    if ((g_skip_lo <= g_skip_hi || g_nskip_key) && ctl_skip(didx, vs->key, fs ? fs->key : NULL)) { if (g_plog) LOG("plog:  draw %u skipped (control file)", didx); return; }
    if (g_plog) LOG("plog:  draw number %u in the frame", didx);
    { int lay = vs->layer_out || (gs && gs->layer_out);   // a layered pass for a shader that writes the layer; a pass opened the other way is ended first
      if (P.open && ((lay && !P.layered && !(P.lok && P.nlayers <= 1)) || (!lay && P.layered))) pass_end();
      P.want_layered = lay; }
    if (P.open && P.merged && draw_reads_rt(vs, fs, gs)) { pass_end(); g_nsplit++; g_after_split = 1; }   // what was drawn into it is stored first, then read (as the pass the hold joined would have)
    if (!pass_begin()) SKIP(2);
    if (C->ntq & 1) C->tdraws[C->ntq / 2]++;   // draws in the timed pass (its signature's line)
    if (ind_b && (dw ? dw->prim : di->prim) == NVN_DRAW_PRIMITIVE_QUADS) { RED_ONCE(30, "a GPU-written indirect draw of quads is not implemented"); SKIP(3); }
    if (ind_b && di && di->itype == 0 && !g_has_u8) { RED_ONCE(37, "a GPU-written indirect draw with 8-bit indices needs VK_EXT_index_type_uint8"); SKIP(3); }
    int prim = dw ? dw->prim : di->prim;
    if (prim == NVN_DRAW_PRIMITIVE_LINE_LOOP || prim == NVN_DRAW_PRIMITIVE_QUAD_STRIP || prim == NVN_DRAW_PRIMITIVE_PATCHES) { RED_ONCE(31, "primitive %d not implemented yet", prim); SKIP(4); }
    pso_key k; memset(&k, 0, sizeof k);
    k.vs = vs; k.fs = fs; k.gs = gs; k.prim = prim; k.nattr = S.nattr; memcpy(k.attr, S.attr, sizeof k.attr); k.nstream = S.nstream; memcpy(k.stream, S.stream, sizeof k.stream);
    k.nc = P.n; for (int i = 0; i < P.n; i++) { k.cf[i] = P.ct[i] && P.cv[i] ? P.ct[i]->f.f : VK_FORMAT_UNDEFINED; k.blend[i] = S.blend[i]; }
    k.color = S.color; k.chmask = S.chmask; k.poly = S.poly; k.samples = P.ct[0] ? 1 : 1;
    if (S.color.alpha_func != NVN_ALPHA_FUNC_ALWAYS) RED_ONCE(48, "the alpha test (ColorStateSetAlphaTest %d) is not implemented: fragments are not discarded by it", (int)S.color.alpha_func);
    // Lines wider than one pixel (SetLineWidth): FINAL FANTASY XII THE ZODIAC AGE draws its battle target lines 3 pixels wide. The Metal host draws them as quads (it has only
    // one-pixel lines, nvn_geometry.inc line_mesh_draw); Vulkan rasterizes them with wideLines, clamped to the GPU's range. Without the feature they stay one pixel wide
    if (prim == NVN_DRAW_PRIMITIVE_LINES || prim == NVN_DRAW_PRIMITIVE_LINE_STRIP || prim == NVN_DRAW_PRIMITIVE_LINE_LOOP) {
        float lw = S.line_w;
        if (lw > 1 && !g_feat.wideLines) { RED_ONCE(50, "lines %.1f pixels wide: the GPU has no wide lines, drawn one pixel wide", lw); lw = 1; }
        if (lw > g_props.limits.lineWidthRange[1]) lw = g_props.limits.lineWidthRange[1];
        k.line_w = lw > 1 ? lw : 0;
    }
    // Primitive restart: Vulkan restarts at the all-ones index only, and in lists only with VK_EXT_primitive_topology_list_restart
    if (S.restart.on && di) {
        uint32_t all = di->itype == 0 ? 0xff : di->itype == 1 ? 0xffff : 0xffffffffu;
        int strip = prim == NVN_DRAW_PRIMITIVE_LINE_STRIP || prim == NVN_DRAW_PRIMITIVE_TRIANGLE_STRIP || prim == NVN_DRAW_PRIMITIVE_TRIANGLE_FAN || prim == NVN_DRAW_PRIMITIVE_LINE_STRIP_ADJACENCY || prim == NVN_DRAW_PRIMITIVE_TRIANGLE_STRIP_ADJACENCY;
        if (S.restart.index != all) RED_ONCE(51, "primitive restart index %#x: Vulkan restarts at %#x only", S.restart.index, all);
        k.restart = strip;
    }
    // Dual-source blending (FINAL FANTASY XII THE ZODIAC AGE's dual_want shaders): with the GPU's dualSrcBlend, through the pixel shader's output 1 moved to index 1 (spv_dual)
    if (fs && g_feat.dualSrcBlend) for (int i = 0; i < P.n && !k.dual; i++)
        if ((S.color.blend_en >> i & 1) && (bf_src1(S.blend[i].src_rgb) || bf_src1(S.blend[i].dst_rgb) || bf_src1(S.blend[i].src_a) || bf_src1(S.blend[i].dst_a))) k.dual = fs_dual_mod(fs) != VK_NULL_HANDLE;
    if (P.dt) { k.df = P.dt->f.f; k.stencil = P.dt->f.stencil; k.ds = S.ds; }
    if (g_plog) LOG("plog:  draw vs %s fs %s n %u depth t%d w%d f%d stencil %d f%d ops %d/%d/%d ref %u vm %#x wm %#x blend %#x mask %#x cull %d", vs->key, fs ? fs->key : "-", dw ? dw->count : di->count,
                    S.ds.dt, S.ds.dw, S.ds.df, S.ds.st, S.ds.sf_f, S.ds.fail_f, S.ds.zfail_f, S.ds.zpass_f, S.st_ref[0], S.st_vmask[0], S.st_wmask[0], S.color.blend_en, S.chmask.mask, S.poly.cull);
    VkPipelineLayout pl = layout_get(vs, fs, gs);
    VkPipeline pp = pso_get(&k, pl, !(P.dt && P.n));   // the scene's passes (colour and depth) do not wait for a new pipeline
    if (!pp) { if (atomic_load(&g_pso_pending)) { g_stat_skip++; g_skip_why[7]++; return; } SKIP(5); }
    if (fs && P.n) fb_prepare(fs); else g_fb_n = 0;
    g_ws_zero_tb = 0;
    uint32_t dyno[192], ndyno = 0, gdyno[64], ngdyno = 0;
    g_vs_haz = 0;
    VkDescriptorSet sets[2]; sets[0] = write_set(vs, 0); memcpy(dyno, g_dyno, g_ndyno * 4); ndyno = g_ndyno;
    if (g_vs_haz) {   // draw_vs_hazard: the vertex shader samples what a pass wrote with vertex shaders left out of its end barrier: the open pass ends, the barrier, and it begins again
        int was = P.open; if (was) pass_end();
        VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT};
        vkCmdPipelineBarrier(C->cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, VK_PIPELINE_STAGE_VERTEX_SHADER_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
        g_vbar_epoch++; g_nbar_vs++;
        if (was && !pass_begin()) SKIP(2);
    }
    sets[1] = VK_NULL_HANDLE; if (fs) { sets[1] = write_set(fs, 1); memcpy(dyno + ndyno, g_dyno, g_ndyno * 4); ndyno += g_ndyno; }
    g_fb_n = 0;
    VkDescriptorSet gset = VK_NULL_HANDLE; if (gs) { gset = write_set(gs, 2); memcpy(gdyno, g_dyno, g_ndyno * 4); ngdyno = g_ndyno; }
    if (!sets[0] || (fs && !sets[1]) || (gs && !gset)) SKIP(6);
    for (int a = 0; a < P.n; a++) if (S.chmask.mask >> (4 * a) & 0xf) P.cwr[a] = P.cdrawn[a] = 1;
    if (P.dt && ((S.ds.dt && S.ds.dw) || S.ds.st)) P.ddrawn = 1;   // depth writes, or a stencil test (its ops may write)
    VkCommandBuffer cb = C->cb;
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pp);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pl, 0, fs ? 2 : 1, sets, ndyno, dyno);
    if (gs) vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pl, 2, 1, &gset, ngdyno, gdyno);
    // NVN's upper-left origin keeps D3D's y-up clip space; Vulkan's is y-down: the viewport is flipped (negative height)
    float x = 0, y = 0, w = (float)P.w, h = (float)P.h;
    if (S.has_vp) { x = S.vp[0][0]; y = S.vp[0][1]; w = S.vp[0][2]; h = S.vp[0][3]; }
    float yt = g_origin_ll ? (float)P.h - y - h : y;   // lower-left origin: NVN's y counts from the bottom
    VkViewport vv = {x, yt + h, w, -h, S.dr[0], S.dr[1] ? S.dr[1] : 1};
    vkCmdSetViewport(cb, 0, 1, &vv);
    VkRect2D sc = {{0, 0}, {P.w, P.h}};
    if (S.has_sc) {
        int32_t sx = S.sc.x < 0 ? 0 : S.sc.x, sy = S.sc.y < 0 ? 0 : S.sc.y, ex = S.sc.x + S.sc.w, ey = S.sc.y + S.sc.h;
        if (g_origin_ll) { int32_t t = (int32_t)P.h - ey; ey = (int32_t)P.h - S.sc.y; sy = t < 0 ? 0 : t; }
        if (ex > (int32_t)P.w) ex = (int32_t)P.w; if (ey > (int32_t)P.h) ey = (int32_t)P.h;
        sc = (VkRect2D){{sx, sy}, {ex > sx ? (uint32_t)(ex - sx) : 0, ey > sy ? (uint32_t)(ey - sy) : 0}};
    }
    vkCmdSetScissor(cb, 0, 1, &sc);
    vkCmdSetDepthBias(cb, S.po.units, S.po.clamp, S.po.factor);
    vkCmdSetBlendConstants(cb, S.blend_color);
    for (int f = 0; f < 2; f++) {
        VkStencilFaceFlags ff = f ? VK_STENCIL_FACE_BACK_BIT : VK_STENCIL_FACE_FRONT_BIT;
        vkCmdSetStencilCompareMask(cb, ff, S.st_vmask[f]); vkCmdSetStencilWriteMask(cb, ff, S.st_wmask[f]); vkCmdSetStencilReference(cb, ff, S.st_ref[f]);
    }
    // vertex streams: only the vertices this draw reads are copied (games bind whole vertex pools, tens of MB). Per-vertex streams are copied from vertex vlo on and the draw's
    // vertex numbers shifted down by vlo (a shader reading its vertex index gets vlo added back: spv_vid_bias); per-instance streams from instance 0 up to the last one drawn
    uint32_t inst = dw ? (dw->instances ? (uint32_t)dw->instances : 1) : (di->instances ? (uint32_t)di->instances : 1);
    uint32_t isz = di ? (di->itype == 0 ? 1 : di->itype == 1 ? 2 : 4) : 0;
    const uint8_t *isrc = di ? (const uint8_t *)(uintptr_t)di->ibuf : NULL;
    #define IDX(i) (isz == 1 ? isrc[i] : isz == 2 ? ((const uint16_t *)isrc)[i] : ((const uint32_t *)isrc)[i])
    int64_t vlo = 0, vhi = -1;
    if (ind_b) { vlo = 0; vhi = INT32_MAX; }   // the GPU's own arguments: every vertex of the streams
    else if (dw) { vlo = dw->first; vhi = (int64_t)dw->first + dw->count - 1; }
    else if (di->count > 0) {
        // The vertices an index range uses: scanned once, then cached by its address, count and type for this generation while no write lands on it (the depth pass and the base
        // pass draw the same meshes; the scan was most of draw's own time)
        static struct { uint64_t a; int32_t n; uint8_t isz; uint64_t gen; uint32_t seq, lo, hi; } ic[2048];
        typeof(ic[0]) *e = &ic[(di->ibuf >> 4 ^ (uint64_t)di->count * 31) % 2048]; uint64_t ia = di->ibuf, ihi = ia + (uint64_t)di->count * isz;
        uint32_t lo = UINT32_MAX, hi = 0; int hit = 0;
        if (e->a == ia && e->n == di->count && e->isz == isz && e->gen == g_wgen && e->seq <= g_nwlog) {
            uint32_t k = e->seq; for (; k < g_nwlog; k++) if (g_wlo[k] < ihi && g_whi[k] > ia) break;
            if (k == g_nwlog) { e->seq = g_nwlog; lo = e->lo; hi = e->hi; hit = 1; }
        }
        if (!hit) {
            uint32_t restart = isz == 1 ? 0xff : isz == 2 ? 0xffff : 0xffffffffu;
            for (int32_t i = 0; i < di->count; i++) { uint32_t v = IDX(i); if (v == restart) continue; if (v < lo) lo = v; if (v > hi) hi = v; }
            *e = (typeof(ic[0])){ia, di->count, (uint8_t)isz, g_wgen, g_nwlog, lo, hi};
        }
        if (lo <= hi) { vlo = (int64_t)lo + di->base_vertex; vhi = (int64_t)hi + di->base_vertex; }
    }
    if (vhi < vlo) { g_stat_draws++; return; }
    if (vlo < 0) vlo = 0;
    if (vs->vid) { int32_t bias = (int32_t)vlo; vkCmdPushConstants(cb, pl, VK_SHADER_STAGE_VERTEX_BIT, 0, 4, &bias); g_vid_push++; if (bias) g_vid_nz++; }   // the shader adds it back to VertexIndex (spv_vid_bias)
    uint32_t used = 0, ext[16] = {0};
    for (int i = 0; i < S.nattr; i++) if (vs->vin_mask >> i & 1) { uint32_t st = S.attr[i].stream, e = S.attr[i].offset + 16; used |= 1u << st; if (e > ext[st]) ext[st] = e; }
    for (int s = 0; s < 16; s++) if (used >> s & 1) {
        VkDeviceSize o = 0; VkBuffer b = g_zero_buf;
        uint64_t stride = s < S.nstream ? S.stream[s].stride : 0, size = S.vbuf[s].size, from, to;
        int per_inst = s < S.nstream && S.stream[s].divisor;
        if (ind_b) { from = 0; to = size < (32u << 20) ? size : (32u << 20); }
        else if (per_inst) { from = 0; to = ((uint64_t)(dw ? dw->base_instance : di->base_instance) + inst - 1) * stride + ext[s]; }
        else { from = (uint64_t)vlo * stride; to = (uint64_t)vhi * stride + ext[s]; }
        if (to > size) to = size;
        if (S.vbuf[s].address && to > from && gw_find(S.vbuf[s].address + from, to - from, 1, &b, &o, NULL)) {}   // vertices a dispatch wrote (GPU particles)
        else if (S.vbuf[s].address && to > from) { VkDeviceSize ro = copy_in(S.vbuf[s].address + from, to - from, 16); if (ro != VK_WHOLE_SIZE) { b = C->ring; o = ro; } }
        vkCmdBindVertexBuffers(cb, (uint32_t)s, 1, &b, &o);
    }
    if (ind_b) {   // arguments the GPU writes (ind_args): the records are in the ring where the dispatch leaves them
        if (dw) DRAWQ(vkCmdDrawIndirect(cb, ind_b, ind_o, 1, 16));
        else {
            uint64_t n = pool_left(di->ibuf, 8u << 20); VkDeviceSize o = n ? copy_in(di->ibuf, n, 16) : VK_WHOLE_SIZE;
            if (o == VK_WHOLE_SIZE) { RED_ONCE(38, "a GPU-written indirect indexed draw: its index buffer is not in a known pool"); return; }
            vkCmdBindIndexBuffer(cb, C->ring, o, isz == 1 ? VK_INDEX_TYPE_UINT8_EXT : isz == 2 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
            DRAWQ(vkCmdDrawIndexedIndirect(cb, ind_b, ind_o, 1, 20));
        }
    } else if (dw) {
        if (prim == NVN_DRAW_PRIMITIVE_QUADS) {   // quads as two triangles each, through indices made here
            uint32_t nq = (uint32_t)dw->count / 4; if (!nq) return;
            VkDeviceSize o = ring_alloc((VkDeviceSize)nq * 24, 16); if (o == VK_WHOLE_SIZE) return;
            uint32_t *ix = (uint32_t *)(C->ringp + o);
            for (uint32_t q = 0; q < nq; q++) { uint32_t b = q * 4; uint32_t v[6] = {b, b + 1, b + 2, b, b + 2, b + 3}; memcpy(ix + 6 * q, v, 24); }
            vkCmdBindIndexBuffer(cb, C->ring, o, VK_INDEX_TYPE_UINT32);
            DRAWQ(vkCmdDrawIndexed(cb, nq * 6, inst, 0, (int32_t)(dw->first - vlo), (uint32_t)dw->base_instance));
        } else DRAWQ(vkCmdDraw(cb, (uint32_t)dw->count, inst, (uint32_t)(dw->first - vlo), (uint32_t)dw->base_instance));
    } else {
        int32_t voff = (int32_t)(di->base_vertex - vlo);
        if (prim == NVN_DRAW_PRIMITIVE_QUADS || (isz == 1 && !g_has_u8)) {   // quads, or 8-bit indices without the extension: widened to 32 bits on the CPU
            uint32_t n = (uint32_t)di->count, nq = prim == NVN_DRAW_PRIMITIVE_QUADS ? n / 4 : 0, out = nq ? nq * 6 : n;
            VkDeviceSize o = ring_alloc((VkDeviceSize)out * 4, 16); if (o == VK_WHOLE_SIZE) return;
            uint32_t *ix = (uint32_t *)(C->ringp + o);
            if (nq) for (uint32_t q = 0; q < nq; q++) { uint32_t b = 4 * q; uint32_t v[6] = {IDX(b), IDX(b + 1), IDX(b + 2), IDX(b), IDX(b + 2), IDX(b + 3)}; memcpy(ix + 6 * q, v, 24); }
            else for (uint32_t i = 0; i < n; i++) ix[i] = IDX(i);
            vkCmdBindIndexBuffer(cb, C->ring, o, VK_INDEX_TYPE_UINT32);
            DRAWQ(vkCmdDrawIndexed(cb, out, inst, 0, voff, (uint32_t)di->base_instance));
        } else {
            VkDeviceSize o = copy_in(di->ibuf, (uint64_t)di->count * isz, 16); if (o == VK_WHOLE_SIZE) return;
            vkCmdBindIndexBuffer(cb, C->ring, o, isz == 1 ? VK_INDEX_TYPE_UINT8_EXT : isz == 2 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
            DRAWQ(vkCmdDrawIndexed(cb, (uint32_t)di->count, inst, 0, voff, (uint32_t)di->base_instance));
        }
    }
    #undef IDX
    trace_add('d', vs->key, fs ? fs->key : NULL, ind_b ? 0 : (uint32_t)(dw ? dw->count : di->count), inst, 0, ind_b != VK_NULL_HANDLE, g_ws_zero_tb);
    g_stat_draws++;
}
// Compute: the bound compute program's shader, its own pipeline and layout (one set), recorded outside render passes; afterwards every storage buffer it had is
// GPU-written: written back to guest memory at completion and read from the ring by the work recorded until then (gw_add)
static void dispatch(const p_dispatch *d) {
    if (vk_skip() & SK_DRAW) return;
    const prog_t *p = S.cprog; if (!p) return;
    // Indirect: the three group counts read here when the CPU can (ind_args), else by the GPU from the ring where an earlier dispatch of this submission writes them
    uint32_t g[3] = {(uint32_t)d->x, (uint32_t)d->y, (uint32_t)d->z}; VkBuffer ib = VK_NULL_HANDLE; VkDeviceSize io = 0;
    if (d->ind) { if (ind_args(d->ind, 12, g, &ib, &io, 1) == 1) ib = VK_NULL_HANDLE;
                  else { static int once; if (!once++) LOG("vk: an indirect dispatch's group counts come from a dispatch of the same submission: read by the GPU (logged once)"); } }
    else if (d->x <= 0 || d->y <= 0 || d->z <= 0) return;
    if (!ib) {
        if (!g[0] || !g[1] || !g[2]) return;
        for (int i = 0; i < 3; i++) if (g[i] > g_props.limits.maxComputeWorkGroupCount[i]) {   // past the GPU's limit: undefined, and a lost device on some
            static int n; if (n++ < 4) LOG("[RED] vk: dispatch of %ux%ux%u groups exceeds the GPU's %ux%ux%u: skipped", g[0], g[1], g[2], g_props.limits.maxComputeWorkGroupCount[0], g_props.limits.maxComputeWorkGroupCount[1], g_props.limits.maxComputeWorkGroupCount[2]); return; }
    }
    static struct { const prog_t *p; uint64_t ser; shader_t *cs; } pc[256];
    unsigned h = (unsigned)(((uintptr_t)p >> 4) ^ p->serial) % 256;
    if (pc[h].p != p || pc[h].ser != p->serial) {
        pc[h].p = p; pc[h].ser = p->serial; pc[h].cs = NULL;
        for (unsigned i = 0; i < p->nstage; i++) { uint8_t md[8]; if (!prog_key(p, i, md)) continue; shader_t *sh = shader_get(md); if (sh->stage == 5) pc[h].cs = sh; }
    }
    shader_t *cs = pc[h].cs; if (!cs || !cs->mod) { RED_ONCE(64, "compute program without a usable compute shader: dispatch skipped"); return; }
    if (!cs->cpp) {
        VkPipelineLayoutCreateInfo li = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 1, &cs->dsl, 0, NULL}; VKCHECK(vkCreatePipelineLayout(g_dev, &li, NULL, &cs->cpl));
        VkComputePipelineCreateInfo ci = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, NULL, 0, {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_COMPUTE_BIT, cs->mod, "main", NULL}, cs->cpl, VK_NULL_HANDLE, -1};
        if (vkCreateComputePipelines(g_dev, g_pcache, 1, &ci, NULL, &cs->cpp) != VK_SUCCESS) { LOG("[RED] vk: compute pipeline for %s not created", cs->key); cs->mod = VK_NULL_HANDLE; return; }
    }
    pass_end();
    g_ws_zero_tb = 0;
    VkDescriptorSet ds = write_set(cs, 5); if (!ds) return;
    if (g_plog) LOG("plog:  dispatch cs %s %ux%ux%u%s, %d storage buffers", cs->key, g[0], g[1], g[2], ib ? " (indirect, GPU-written)" : "", g_nwb);
    vkCmdBindPipeline(C->cb, VK_PIPELINE_BIND_POINT_COMPUTE, cs->cpp);
    vkCmdBindDescriptorSets(C->cb, VK_PIPELINE_BIND_POINT_COMPUTE, cs->cpl, 0, 1, &ds, g_ndyno, g_dyno);
    { char b[64]; snprintf(b, sizeof b, "cs %s", cs->key); g_tsig_next = fnv(cs->key, strlen(cs->key)) | 1; gsig_name(g_tsig_next, b); }
    gpu_mark(1, 0);
    if (ib) vkCmdDispatchIndirect(C->cb, ib, io); else vkCmdDispatch(C->cb, g[0], g[1], g[2]);
    gpu_mark(1, 1);
    trace_add('c', cs->key, NULL, ib ? 0 : g[0], ib ? 0 : g[1], ib ? 0 : g[2], ib != VK_NULL_HANDLE, g_ws_zero_tb);
    barrier_cs(C->cb);
    for (int i = 0; i < g_nwb; i++) {
        gw_t *w = &g_wb[i]; int from_gw = w->fence != 0;
        // Back into guest memory at completion, only what the GPU changed. The GPU writes the bytes a shader stores and no others: the whole range written back put the bytes the
        // dispatch was recorded with over everything the CPU wrote there while the submission ran (a frame or two in flight). So the bytes it starts from are kept (the guest's,
        // copied into the ring now), and the completion writes back the 4-byte words that differ from them (post kind 5). A range already set to go back in this submission keeps
        // its first entry (its snapshot holds the guest's bytes; the ring holds the final ones). A range another submission's GPU work wrote goes back whole, as before
        int dup = 0; const uint8_t *dsnap = NULL;
        for (int k = 0; k < C->npost && !dup; k++) { const post_t *q = &C->post[k]; uint64_t qa = (uint64_t)(uintptr_t)q->dst;
            if (q->kind == 3 && qa == w->addr && q->src == w->p && q->n == w->size) dup = 1;   // the same range again
            else if (q->kind == 5 && w->addr >= qa && w->addr + w->size <= qa + q->n && q->src + (w->addr - qa) == w->p) { dup = 1; dsnap = (const uint8_t *)(uintptr_t)q->addr + (w->addr - qa); } }   // inside one that goes back by its changed words
        uint8_t *snap = !dup && !from_gw && w->ci == g_ci && w->p ? malloc(w->size) : NULL;
        if (snap) memcpy(snap, w->p, w->size);
        // The range is the GPU's until the submission completes (gw_find), with the bytes it started from to tell the CPU's later rewrites (gw_cpu_dirty)
        gw_add(w->addr, w->size, w->buf, w->off, w->p, g_fence + 1, w->ci, snap ? snap : dsnap, 0);   // this submission's fence value (mtl_sub_commit assigns g_fence + 1)
        if (dup) continue;
        if (snap) post_add((post_t){5, (void *)(uintptr_t)w->addr, 0, 0, (uint64_t)(uintptr_t)snap, 0, w->p, w->size});
        else post_add((post_t){3, (void *)(uintptr_t)w->addr, 0, 0, 0, 0, w->p, w->size});
    }
    g_stat_draws++;
}
static void clear_color(const p_clear_color *c) {
    if (c->index < 0 || c->index >= P.n || !P.cv[c->index]) return;
    if ((c->mask & 15) != 15) RED_ONCE(40, "a color clear with a partial channel mask clears every channel");
    if (!P.open) { P.clr_c[c->index] = 1; memcpy(&P.cc[c->index], c->rgba, 16); return; }   // before the pass's first draw: its load action
    VkClearAttachment a = {VK_IMAGE_ASPECT_COLOR_BIT, (uint32_t)c->index, {.color = {{c->rgba[0], c->rgba[1], c->rgba[2], c->rgba[3]}}}};
    VkClearRect r = {{{0, 0}, {P.w, P.h}}, 0, P.layered ? P.nlayers : 1};   // a layered pass clears every layer, as NVN clears every layer of a layered target
    vkCmdClearAttachments(C->cb, 1, &a, 1, &r);
}
static void clear_ds(const p_clear_ds *c) {
    if (!P.dt) return;
    if (!P.open) { if (c->depth_mask) { P.clr_d = 1; P.cd = c->depth; } if (P.dt->f.stencil && c->stencil_mask) { P.clr_s = 1; P.cs = (uint32_t)c->stencil; } return; }
    if (!pass_begin()) return;
    VkImageAspectFlags asp = (c->depth_mask ? VK_IMAGE_ASPECT_DEPTH_BIT : 0) | (P.dt->f.stencil && c->stencil_mask ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
    if (!asp) return;
    VkClearAttachment a = {asp, 0, {.depthStencil = {c->depth, (uint32_t)c->stencil}}};
    VkClearRect r = {{{0, 0}, {P.w, P.h}}, 0, P.layered ? P.nlayers : 1};   // a layered pass clears every layer, as NVN clears every layer of a layered target
    vkCmdClearAttachments(C->cb, 1, &a, 1, &r);
}
// Copies and blits are checked against the image before they are recorded: a region past a level's extent, a level or layer the image lacks, or a blit the format does not support
// (VK_FORMAT_FEATURE_BLIT_SRC / DST, linear filtering) is invalid usage, and the Mali-G57 answered such work in Super Mario Odyssey with a lost device. Equal-size blits fall back to a copy
static int lvl_fits(const vtex *v, uint32_t lv, uint32_t layer, uint32_t nlayers, int32_t x, int32_t y, int32_t z, uint32_t w, uint32_t h, uint32_t d) {
    if (lv >= v->levels || x < 0 || y < 0 || z < 0) return 0;
    uint32_t lw = v->w >> lv ? v->w >> lv : 1, lh = v->h >> lv ? v->h >> lv : 1, ld = v->d >> lv ? v->d >> lv : 1;
    if (v->target == NVN_TEXTURE_TARGET_1D_ARRAY) lh = 1;
    if ((uint64_t)x + w > lw || (uint64_t)y + h > lh || (uint64_t)z + d > ld) return 0;
    return v->target == NVN_TEXTURE_TARGET_3D || (uint64_t)layer + nlayers <= v->layers;
}
static VkFormatFeatureFlags fmt_feat(VkFormat f) { VkFormatProperties fp; vkGetPhysicalDeviceFormatProperties(g_pd, f, &fp); return fp.optimalTilingFeatures; }
static int copylog_on(void) {   // DM_VK_COPYLOG=<n>: log the first n buffer-to-texture and texture copies
    static int left = -1; if (left < 0) { const char *e = getenv("DM_VK_COPYLOG"); left = e ? atoi(e) : 0; if (left) LOG("DM_VK_COPYLOG=%d: copies into textures are logged", left); }
    if (left <= 0) return 0;
    left--; return 1;
}
// A GPU copy's bytes also go into the destination's storage when the texture takes its content from there (as nvn_mtl.m tex_storage_dst / nvn_tex_mirror_*): any later upload
// from the storage -- the texture made again after the game released it, a new texture object over the same storage, an eviction -- then has the copied levels too. The level
// stays the GPU's for this image (gpu_lv); the storage now holds the same bytes (mir_lv)
int nvn_tex_mirror_b2t(const tex_t *t, unsigned level, unsigned layer0, const NVNcopyRegion *r, const uint8_t *src, size_t row, size_t img);   // hle_nvn.c
int nvn_tex_mirror_t2t(const tex_t *s, unsigned sl, unsigned ss, const NVNcopyRegion *sr, const tex_t *d, unsigned dl, unsigned ds, const NVNcopyRegion *dr);
static uint64_t g_mir_b2t, g_mir_t2t;
static int mirror_on(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_VK_MIRROR"); v = !(e && !strcmp(e, "0")); if (!v) LOG("DM_VK_MIRROR=0: GPU copies into textures are not written into their storage"); } return v; }   // DM_VK_MIRROR=0: off
static int storage_backed(const tex_t *t, const vtex *v) {
    if (!mirror_on()) return 0;
    if (!t->mem || !t->pool || (t->pool->flags & (NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS | NVN_MEMORY_POOL_FLAGS_VIRTUAL)) || t->d.samples > 1) return 0;
    if (t->d.target == NVN_TEXTURE_TARGET_BUFFER || t->d.target == NVN_TEXTURE_TARGET_3D || (t->d.flags & (NVN_TEXTURE_FLAGS_LINEAR | NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET | NVN_TEXTURE_FLAGS_COMPRESSIBLE | NVN_TEXTURE_FLAGS_IMAGE))) return 0;
    return v && !v->gpu;
}
static int storage_holds(const vtex *v, unsigned lv) { return v && !v->gpu && lv < 32 && ((v->packaged && !(v->gpu_lv >> lv & 1)) || (v->mir_lv >> lv & 1)); }   // the level's bytes are in the storage
static void copy_b2t(const p_copy_b2t *c) {
    if (vk_skip() & SK_B2T) return;
    if (g_plog) LOG("plog:  b2t serial %llu fmt %#x %dx%d at %d,%d,%d", (unsigned long long)c->tex->serial, c->tex->d.format, c->r.width, c->r.height, c->r.xoffset, c->r.yoffset, c->r.zoffset);
    vtex *v = tex_get(c->tex, 0); if (!v) return;
    if (v->f.dec) { RED_ONCE(50, "buffer-to-texture copy into a BC texture: not decoded yet"); return; }
    v->copied = 1;
    int bw = 1, bh = 1, bpb = nvn_fmt_bpb(c->tex->d.format, &bw, &bh); if (!bpb) return;
    const NVNcopyRegion *r = &c->r; uint32_t w = (uint32_t)r->width, h = (uint32_t)r->height, d = (uint32_t)(r->depth > 0 ? r->depth : 1);
    size_t row = S.copy_stride ? (size_t)S.copy_stride : (size_t)((w + bw - 1) / bw) * bpb, img = S.copy_img_stride ? (size_t)S.copy_img_stride : row * ((h + bh - 1) / bh);
    pass_end();
    // The source as the GPU will hold it: what a dispatch or a texture-to-buffer copy wrote there (this submission's, or an earlier one's still running) is only in a ring until
    // its submission completes, and the guest's bytes are stale until then; else the guest's bytes
    VkBuffer sb; VkDeviceSize o; uint8_t *sp = NULL; int from_gpu = gw_exact(c->src, img * d) && gw_find(c->src, img * d, 16, &sb, &o, &sp);
    if (!from_gpu) { o = copy_in(c->src, img * d, 16); if (o == VK_WHOLE_SIZE) return; sb = C->ring; sp = C->ringp + o; }   // the ring after copy_in: a full one is replaced by an extra ring
    else atomic_fetch_add(&g_gw_b2t, 1);
    if (copylog_on()) {   // diagnostics (DM_VK_COPYLOG=<n>): the first n copies into textures, with what the copy reads, to set beside the Metal host's [pass] copy lines
        uint64_t hh = 0xcbf29ce484222325ull, nz = 0, nb = img * d; if (sp) for (uint64_t i = 0; i < nb; i++) { hh = (hh ^ sp[i]) * 0x100000001b3ull; nz += sp[i] != 0; }
        LOG("copylog: frame %llu b2t serial %llu fmt %#x %ux%ux%u at %d,%d,%d from %#llx row %zu image %zu, %s, %llu bytes, %llu non-zero, hash %016llx", (unsigned long long)atomic_load(&g_presents),
            (unsigned long long)c->tex->serial, c->tex->d.format, w, h, d, r->xoffset, r->yoffset, r->zoffset, (unsigned long long)c->src, row, img, from_gpu ? "GPU-written" : "guest memory",
            (unsigned long long)nb, (unsigned long long)nz, (unsigned long long)hh); }
    uint32_t lv = c->has_view && c->view.set_levels ? c->view.base_level : 0, ly = c->has_view && c->view.set_layers ? c->view.min_layer : 0;
    if (lv < 32) v->gpu_lv |= 1u << lv;   // this level's content now comes from copies (see copy_t2t)
    if (!from_gpu && storage_backed(c->tex, v) && !nvn_tex_mirror_b2t(c->tex, lv, ly, r, (const uint8_t *)(uintptr_t)c->src, row, img)) { if (lv < 32) v->mir_lv |= 1u << lv; g_mir_b2t++; }
    int is3d = v->target == NVN_TEXTURE_TARGET_3D;
    if (!lvl_fits(v, lv, is3d ? 0 : ly + (uint32_t)r->zoffset, is3d ? 1 : d, r->xoffset, r->yoffset, is3d ? r->zoffset : 0, w, h, is3d ? d : 1)) {
        static int n; if (n++ < 8) LOG("[RED] vk: buffer-to-texture copy %dx%dx%d at %d,%d,%d level %u outside the %ux%ux%u texture (%u levels, %u layers): skipped", (int)w, (int)h, (int)d, r->xoffset, r->yoffset, r->zoffset, lv, v->w, v->h, v->d, v->levels, v->layers);
        return;
    }
    VkBufferImageCopy bc = {o, (uint32_t)(row / bpb * bw), (uint32_t)(img / row * bh), {v->aspect & VK_IMAGE_ASPECT_COLOR_BIT ? VK_IMAGE_ASPECT_COLOR_BIT : VK_IMAGE_ASPECT_DEPTH_BIT, lv, is3d ? 0 : ly + (uint32_t)r->zoffset, is3d ? 1 : d},
                            {r->xoffset, r->yoffset, is3d ? r->zoffset : 0}, {w, h, is3d ? d : 1}};
    gpu_mark(2, 0); vkCmdCopyBufferToImage(C->cb, sb, v->img, VK_IMAGE_LAYOUT_GENERAL, 1, &bc); gpu_mark(2, 1);
    barrier_xfer(C->cb);
}
static void copy_t2t(const p_copy_t2t *c) {
    if (vk_skip() & SK_T2T) return;
    if (g_plog) LOG("plog:  t2t %llu (fmt %#x) %dx%d → %llu (fmt %#x) %dx%d", (unsigned long long)c->src->serial, c->src->d.format, c->sr.width, c->sr.height, (unsigned long long)c->dst->serial, c->dst->d.format, c->dr.width, c->dr.height);
    // The destination is fetched as a texture, not a render target: Unreal's texture streaming copies the mips two textures share on the GPU and streams the others into the
    // new texture's storage; made as a render target it was never uploaded (and one made already stopped following its storage), so the streamed mips never arrived and sprites
    // stayed on their coarsest level, magnified into blocks (FINAL FANTASY RESONANCE's battle). The level copied into is the GPU's from now on (gpu_lv)
    vtex *s = tex_get(c->src, 0), *d = tex_get(c->dst, 0); if (!s || !d) return;
    pass_end();
    uint32_t sl = c->has_sv && c->sv.set_levels ? c->sv.base_level : 0, dl = c->has_dv && c->dv.set_levels ? c->dv.base_level : 0;
    if (dl < 32) d->gpu_lv |= 1u << dl;
    if (storage_backed(c->dst, d) && storage_holds(s, sl) && c->sr.width == c->dr.width && c->sr.height == c->dr.height
        && !nvn_tex_mirror_t2t(c->src, sl, (unsigned)c->sr.zoffset, &c->sr, c->dst, dl, (unsigned)c->dr.zoffset, &c->dr)) { if (dl < 32) d->mir_lv |= 1u << dl; g_mir_t2t++; }
    if (copylog_on()) LOG("copylog: frame %llu t2t serial %llu (fmt %#x, level %u, %s) %dx%d at %d,%d -> serial %llu (fmt %#x, level %u) at %d,%d", (unsigned long long)atomic_load(&g_presents),
                          (unsigned long long)c->src->serial, c->src->d.format, sl, s->gpu ? "rendered" : s->gpu_lv ? "copied into" : "from its storage", c->sr.width, c->sr.height, c->sr.xoffset, c->sr.yoffset,
                          (unsigned long long)c->dst->serial, c->dst->d.format, dl, c->dr.xoffset, c->dr.yoffset);
    VkImageAspectFlags sa = s->aspect & VK_IMAGE_ASPECT_COLOR_BIT ? VK_IMAGE_ASPECT_COLOR_BIT : VK_IMAGE_ASPECT_DEPTH_BIT, da = d->aspect & VK_IMAGE_ASPECT_COLOR_BIT ? VK_IMAGE_ASPECT_COLOR_BIT : VK_IMAGE_ASPECT_DEPTH_BIT;
    if (!lvl_fits(s, sl, (uint32_t)c->sr.zoffset, 1, c->sr.xoffset, c->sr.yoffset, 0, (uint32_t)c->sr.width, (uint32_t)c->sr.height, 1) ||
        !lvl_fits(d, dl, (uint32_t)c->dr.zoffset, 1, c->dr.xoffset, c->dr.yoffset, 0, (uint32_t)c->dr.width, (uint32_t)c->dr.height, 1) || !c->sr.width || !c->sr.height || !c->dr.width || !c->dr.height) {
        static int n; if (n++ < 8) LOG("[RED] vk: texture copy %dx%d at %d,%d (level %u) → %dx%d at %d,%d (level %u) outside its textures (%ux%u, %ux%u): skipped", c->sr.width, c->sr.height, c->sr.xoffset, c->sr.yoffset, sl,
                                       c->dr.width, c->dr.height, c->dr.xoffset, c->dr.yoffset, dl, s->w, s->h, d->w, d->h);
        return;
    }
    int same = c->sr.width == c->dr.width && c->sr.height == c->dr.height;
    VkFormatFeatureFlags sf = fmt_feat(s->f.f), df = fmt_feat(d->f.f);
    int lin = (c->flags & 1) && !same, blit_ok = (sf & VK_FORMAT_FEATURE_BLIT_SRC_BIT) && (df & VK_FORMAT_FEATURE_BLIT_DST_BIT) && (!lin || (sf & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)) && sa == da;
    if (same && (!blit_ok || s->f.f == d->f.f) && sa == da) {   // one size: a copy (formats of one size class), which every format supports
        VkImageCopy ic = {{sa, sl, (uint32_t)c->sr.zoffset, 1}, {c->sr.xoffset, c->sr.yoffset, 0}, {da, dl, (uint32_t)c->dr.zoffset, 1}, {c->dr.xoffset, c->dr.yoffset, 0}, {(uint32_t)c->sr.width, (uint32_t)c->sr.height, 1}};
        vkCmdCopyImage(C->cb, s->img, VK_IMAGE_LAYOUT_GENERAL, d->img, VK_IMAGE_LAYOUT_GENERAL, 1, &ic);
    } else if (blit_ok) {
        VkImageBlit b = {{sa, sl, (uint32_t)c->sr.zoffset, 1}, {{c->sr.xoffset, c->sr.yoffset, 0}, {c->sr.xoffset + c->sr.width, c->sr.yoffset + c->sr.height, 1}},
                         {da, dl, (uint32_t)c->dr.zoffset, 1}, {{c->dr.xoffset, c->dr.yoffset, 0}, {c->dr.xoffset + c->dr.width, c->dr.yoffset + c->dr.height, 1}}};
        vkCmdBlitImage(C->cb, s->img, VK_IMAGE_LAYOUT_GENERAL, d->img, VK_IMAGE_LAYOUT_GENERAL, 1, &b, lin ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
    } else { static int n; if (n++ < 8) LOG("[RED] vk: scaled texture copy between formats %d → %d that cannot be blitted: skipped", (int)s->f.f, (int)d->f.f); return; }
    barrier_xfer(C->cb);
}
static void copy_t2b(const p_copy_t2t *c) {   // src = the texture, dst field of the record = the destination buffer address (see nvn_obj.h)
    if (vk_skip() & SK_T2B) return;
    if (g_plog) LOG("plog:  t2b %llu %dx%d", (unsigned long long)c->src->serial, c->sr.width, c->sr.height);
    vtex *s = tex_get(c->src, 0); if (!s) return;
    int bw = 1, bh = 1, bpb = nvn_fmt_bpb(c->src->d.format, &bw, &bh); if (!bpb || s->f.dec) { RED_ONCE(51, "texture-to-buffer copy of format %#x not implemented", c->src->d.format); return; }
    pass_end();
    uint32_t w = (uint32_t)c->sr.width, h = (uint32_t)c->sr.height; size_t row = S.copy_stride ? (size_t)S.copy_stride : (size_t)((w + bw - 1) / bw) * bpb, n = row * ((h + bh - 1) / bh);
    VkDeviceSize o = ring_alloc(n, 16); if (o == VK_WHOLE_SIZE) return;
    uint32_t sl = c->has_sv && c->sv.set_levels ? c->sv.base_level : 0;
    if (!lvl_fits(s, sl, (uint32_t)c->sr.zoffset, 1, c->sr.xoffset, c->sr.yoffset, 0, w, h, 1)) { static int nn; if (nn++ < 8) LOG("[RED] vk: texture-to-buffer copy %ux%u outside the %ux%u texture: skipped", w, h, s->w, s->h); return; }
    VkBufferImageCopy bc = {o, (uint32_t)(row / bpb * bw), 0, {s->aspect & VK_IMAGE_ASPECT_COLOR_BIT ? VK_IMAGE_ASPECT_COLOR_BIT : VK_IMAGE_ASPECT_DEPTH_BIT, sl, (uint32_t)c->sr.zoffset, 1},
                            {c->sr.xoffset, c->sr.yoffset, 0}, {w, h, 1}};
    gpu_mark(2, 0); vkCmdCopyImageToBuffer(C->cb, s->img, VK_IMAGE_LAYOUT_GENERAL, C->ring, 1, &bc); gpu_mark(2, 1);
    barrier_xfer(C->cb);
    // Until the submission completes and the bytes go back to guest memory, later reads of the destination (a buffer-to-texture copy, a buffer copy, a draw's buffers) take them
    // from the ring, as a dispatch's writes (gw_add); they read the guest's stale bytes before
    gw_add((uint64_t)(uintptr_t)c->dst, n, C->ring, o, C->ringp + o, g_fence + 1, g_ci, NULL, 1);   // an exact write: every byte of the range
    size_t packed = (size_t)((w + bw - 1) / bw) * bpb, rows = (h + bh - 1) / bh;
    if (row > packed) post_add((post_t){6, (void *)c->dst, (uint32_t)row, (uint32_t)rows, 0, (uint32_t)packed, C->ringp + o, n});   // a row stride past the texels: only the texels go back, the bytes between rows stay the guest's
    else post_add((post_t){3, (void *)c->dst, 0, 0, 0, 0, C->ringp + o, n});
}
static void clear_tex(const p_clear_tex *c) {
    if (vk_skip() & SK_CLEAR) return;
    if (g_plog) LOG("plog:  clear tex %llu fmt %#x", (unsigned long long)c->tex->serial, c->tex->d.format);
    vtex *v = tex_get(c->tex, 1); if (!v) return;
    pass_end();
    uint32_t lv = c->has_view && c->view.set_levels ? c->view.base_level : 0;
    if (lv >= v->levels) { static int n; if (n++ < 8) LOG("[RED] vk: clear of level %u of a %u-level texture: skipped", lv, v->levels); return; }
    VkClearColorValue cc; memcpy(&cc, &c->c, 16);
    VkImageSubresourceRange rr = {VK_IMAGE_ASPECT_COLOR_BIT, lv, 1, 0, VK_REMAINING_ARRAY_LAYERS};
    if (v->f.depth) { VkClearDepthStencilValue dv = {c->c.f[0], 0}; rr.aspectMask = v->aspect; vkCmdClearDepthStencilImage(C->cb, v->img, VK_IMAGE_LAYOUT_GENERAL, &dv, 1, &rr); }
    else vkCmdClearColorImage(C->cb, v->img, VK_IMAGE_LAYOUT_GENERAL, &cc, 1, &rr);
    barrier_xfer(C->cb);
}

// ---------- Submission ----------
// The context's own ring sized to what its submissions take (its previous one is complete, and nothing holds its ring: mtl_sub_begin waited for both)
static _Atomic uint64_t g_trim_gen, g_wait_fence_ns, g_wait_hold_ns;
void dm_vk_trim(void) { atomic_fetch_add(&g_trim_gen, 1); }
void tex_evict_hook(void) { tex_evict(atomic_load(&g_trim_gen)); }   // the front end's low-memory signal (app/front_android.c): every ring back to RING_MIN at its next use
static void ring_fit(ctx_t *c) {
    c->last_total = c->total; if (c->total > c->win_max) c->win_max = c->total;
    VkDeviceSize want = c->ring0_size; uint64_t tg = atomic_load(&g_trim_gen);
    if (c->trim_seen != tg) { c->trim_seen = tg; want = RING_MIN; c->win_max = 0; c->nwin = 0; }
    else if (c->last_total > c->ring0_size && c->ring0_size < RING_MAX) { while (want < c->last_total && want < RING_MAX) want *= 2;
                                                                         if (atomic_load(&g_ring0_bytes) + (want - c->ring0_size) > RING_CAP) want = c->ring0_size; }
    else if (++c->nwin >= 120) { if (c->win_max * 4 < c->ring0_size && c->ring0_size > RING_MIN) want = c->ring0_size / 2; c->win_max = 0; c->nwin = 0; }
    if (want == c->ring0_size) return;
    vkDestroyBuffer(g_dev, c->ring0, NULL); vkFreeMemory(g_dev, c->ringm0, NULL); atomic_fetch_sub(&g_ring0_bytes, c->ring0_size);
    c->ring0 = buf_new(want, ALL_BUF_USAGE, &c->ringm0, (void **)&c->ringp0); c->ring0_size = want; atomic_fetch_add(&g_ring0_bytes, want);
}
void mtl_sub_begin(void) {
    vk_init();
    g_ci = (g_ci + 1) % NCTX; ctx_t *c = &g_ctx[g_ci];
    uint64_t w0 = now_ns();
    pthread_mutex_lock(&g_mu);
    { uint64_t t1 = now_ns(); while (c->fence_val > g_done) pthread_cond_wait(&g_cv, &g_mu); uint64_t t2 = now_ns(); while (c->hold > g_done) pthread_cond_wait(&g_cv, &g_mu);
      atomic_fetch_add(&g_wait_fence_ns, t2 - t1); atomic_fetch_add(&g_wait_hold_ns, now_ns() - t2); }   // its own submission still running / a later one still reading its ring
    pthread_mutex_unlock(&g_mu);
    pthread_mutex_lock(&g_gwmu); c->hold = 0; pthread_mutex_unlock(&g_gwmu);
    atomic_fetch_add(&g_wait_ns, now_ns() - w0); atomic_fetch_add(&g_wait_cum, now_ns() - w0);
    ring_fit(c);
    C = c;
    { void tex_evict_hook(void); tex_evict_hook(); } C->used = 0; C->up_used = 0; g_ndc = 0; g_wgen++; g_nwlog = 0; C->ring = C->ring0; C->ringp = C->ringp0; C->ring_size = C->ring0_size; C->total = 0;
    g_bvgen++;   // texel buffer views are this submission's (bview)
    C->ntr = C->ndraw = C->ndisp = C->max_groups = 0; memset(&C->heavy, 0, sizeof C->heavy); C->plog = 0;
    if (!C->reset_done) for (int i = 0; i < C->ndp; i++) VKCHECK(vkResetDescriptorPool(g_dev, C->dps[i], 0));   // normally done by the completion thread (ctx_complete)
    C->idp = 0;
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL};
    if (!C->reset_done) VKCHECK(vkResetCommandPool(g_dev, C->pool, 0));   // both command buffers at once, their memory kept; normally done already by the completion thread
    C->reset_done = 0;
    VKCHECK(vkBeginCommandBuffer(C->cb, &bi)); VKCHECK(vkBeginCommandBuffer(C->up, &bi));
    barrier_all(C->up); g_vbar_epoch++;   // after everything submitted before (see mtl_sub_commit: no semaphore wait between the game's submissions)
    vkCmdResetQueryPool(C->up, C->qp, 0, 2); vkCmdWriteTimestamp(C->up, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, C->qp, 0);
    if (!C->oq_init) { vkCmdResetQueryPool(C->up, C->oq, 0, NOQ); C->oq_init = 1; } else if (C->noq) vkCmdResetQueryPool(C->up, C->oq, 0, C->noq);   // only those used last time
    C->noq = 0; g_occ_over = 0;
    if (!C->tq_init) { vkCmdResetQueryPool(C->up, C->tq, 0, NTQ); C->tq_init = 1; } else if (C->ntq) vkCmdResetQueryPool(C->up, C->tq, 0, C->ntq);
    C->ntq = 0;
}
void mtl_sub_index(const uint64_t *h, int n) { (void)h; (void)n; }
void mtl_exec_rest(const uint64_t *handles, int n) { g_rest = handles; g_nrest = n; }   // the handles after the one replayed next (rt_hold_scan)
// A buffer copy is made on the CPU as the command list is walked (the in-order writes, wlog). Where the source holds bytes the GPU wrote and has not put back in guest memory
// yet (gw_find), the copy is made on the GPU from them into the ring instead, and the destination becomes a GPU-written range like the source (gw_add, back at completion)
static int gw_any(uint64_t a, uint64_t n) { int r = 0; if (!gw_pg_any(a, n)) return 0; pthread_mutex_lock(&g_gwmu); for (int i = 0; i < g_ngw && !r; i++) r = a < g_gw[i].addr + g_gw[i].size && g_gw[i].addr < a + n; pthread_mutex_unlock(&g_gwmu); return r; }
void mtl_sub_copy(uint64_t src, uint64_t dst, uint64_t n) {
    memmove((void *)dst, (const void *)src, n); wlog_add(dst, n);
    if (!n || !g_ngw || !gw_any(src, n)) return;
    pass_end();
    VkBuffer sb; VkDeviceSize so; if (!gw_find(src, n, 16, &sb, &so, NULL)) { memmove((void *)dst, (const void *)src, n); return; }   // complete now (gw_find waited for it): in guest memory
    VkDeviceSize ro = ring_alloc(n, 16); if (ro == VK_WHOLE_SIZE) { RED_ONCE(93, "a buffer copy of GPU-written bytes found no room in the ring: copied from guest memory"); return; }
    VkBufferCopy bc = {so, ro, n}; vkCmdCopyBuffer(C->cb, sb, C->ring, 1, &bc); barrier_xfer(C->cb);
    gw_add(dst, n, C->ring, ro, C->ringp + ro, g_fence + 1, g_ci, NULL, 1); post_add((post_t){3, (void *)(uintptr_t)dst, 0, 0, 0, 0, C->ringp + ro, n});
    atomic_fetch_add(&g_gw_b2b, 1);
}
void mtl_sub_event(volatile uint32_t *val, uint32_t mode, uint32_t v) { post_add((post_t){1, (void *)val, mode, v}); }
void mtl_sub_counter(uint64_t addr, uint32_t type) { post_add((post_t){2, NULL, 0, 0, addr, type}); }
static void mtl_exec_(const uint8_t *p);
void mtl_exec(const uint8_t *p) { uint64_t t0 = now_ns(); mtl_exec_(p); atomic_fetch_add(&g_rec_ns, now_ns() - t0); }
// NVN's initial command buffer state, as the Metal backend's state_reset: games set some of it only by state objects or never (Metroid Dread never sets the stencil value mask,
// which starts at 0xff; left at 0 its G-buffer draws, stencil test "not equal 2", failed everywhere and the level's opaque geometry stayed black)
static void state_defaults(void) {
    for (int i = 0; i < 8; i++) S.blend[i] = (blend_st){i, NVN_BLEND_FUNC_ONE, NVN_BLEND_FUNC_ZERO, NVN_BLEND_FUNC_ONE, NVN_BLEND_FUNC_ZERO, NVN_BLEND_EQUATION_ADD, NVN_BLEND_EQUATION_ADD};
    S.color = (color_st){0, NVN_LOGIC_OP_COPY, NVN_ALPHA_FUNC_ALWAYS}; S.chmask.mask = 0xffffffffu;
    S.poly = (poly_st){NVN_FACE_NONE, NVN_FRONT_FACE_CCW, NVN_POLYGON_MODE_FILL, 0};
    S.ds = (ds_st){0, 1, NVN_DEPTH_FUNC_LESS, 0, NVN_STENCIL_FUNC_ALWAYS, NVN_STENCIL_FUNC_ALWAYS, 1, 1, 1, 1, 1, 1};
    S.st_vmask[0] = S.st_vmask[1] = S.st_wmask[0] = S.st_wmask[1] = 0xff; S.dr[1] = 1; S.line_w = 1;
}
static void mtl_exec_(const uint8_t *p) {
    static int inited; if (!inited) { inited = 1; state_defaults(); }
    for (;;) {
        op_hdr h; memcpy(&h, p, sizeof h);
        const uint8_t *a = p + 8;
        switch (h.op) {
            case OP_END: return;
            case OP_JUMP: { const uint8_t *t; memcpy(&t, a, 8); p = t; continue; }
            case OP_CALL: { const uint8_t *t; memcpy(&t, a, 8); if (g_nrs < 32) g_rs[g_nrs] = p + h.size; g_nrs++; mtl_exec_(t); g_nrs--; break; }   // g_rs: where the replay returns (rt_hold_scan)
            case OP_SIGNAL_EVENT: { volatile uint32_t *val; uint32_t m, v; memcpy(&val, a, 8); memcpy(&m, a + 8, 4); memcpy(&v, a + 12, 4); mtl_sub_event(val, m, v); break; }
            case OP_REPORT_COUNTER: { uint64_t t, ad; memcpy(&t, a, 8); memcpy(&ad, a + 8, 8); if (t == NVN_COUNTER_TYPE_SAMPLES_PASSED) occ_report(ad); else mtl_sub_counter(ad, (uint32_t)t); break; }
            case OP_RESET_COUNTER: { uint64_t t; memcpy(&t, a, 8); if (t == NVN_COUNTER_TYPE_SAMPLES_PASSED) occ_reset(); break; }
            case OP_FENCE_SYNC: { sync_t *s; memcpy(&s, a, 8); s->fence = g_fence + 1; s->fenced = 1; break; }
            case OP_CLEAR_BUF: { const p_clear_buf *cb = (const p_clear_buf *)a; for (uint64_t o = 0; o + 4 <= cb->size; o += 4) memcpy((uint8_t *)cb->addr + o, &cb->value, 4); wlog_add(cb->addr, cb->size); break; }
            case OP_COPY_B2B: { uint64_t s, d, n; memcpy(&s, a, 8); memcpy(&d, a + 8, 8); memcpy(&n, a + 16, 8); mtl_sub_copy(s, d, n); break; }
            case OP_UPDATE_UB: { uint64_t d, n; memcpy(&d, a, 8); memcpy(&n, a + 8, 8); memcpy((void *)d, a + 16, n); wlog_add(d, n); break; }
            case OP_SET_RT: set_rt(p, (const p_set_rt *)a, h.size); break;
            case OP_CLEAR_COLOR: clear_color((const p_clear_color *)a); break;
            case OP_CLEAR_DS: clear_ds((const p_clear_ds *)a); break;
            case OP_DRAW: { uint64_t t0 = now_ns(); p_draw t; draw(draw_rec(a, h.size, &t), NULL); g_t_draw += now_ns() - t0; break; }
            case OP_DRAW_IDX: { uint64_t t0 = now_ns(); p_draw_idx t; draw(NULL, draw_idx_rec(a, h.size, &t)); g_t_draw += now_ns() - t0; break; }
            case OP_DISPATCH: { uint64_t t0 = now_ns(); dispatch((const p_dispatch *)a); g_t_disp += now_ns() - t0; break; }
            case OP_COPY_B2T: copy_b2t((const p_copy_b2t *)a); break;
            case OP_COPY_T2T: copy_t2t((const p_copy_t2t *)a); break;
            case OP_COPY_T2B: copy_t2b((const p_copy_t2t *)a); break;
            case OP_CLEAR_TEX: clear_tex((const p_clear_tex *)a); break;
            case OP_BARRIER: pass_end(); break;
            case OP_SCISSOR: S.sc = *(const p_scissor *)a; S.has_sc = 1; break;
            case OP_BIND_PROGRAM: { const p_bind_program *b = (const p_bind_program *)a; if (b->stages & 0x20) S.cprog = b->prog; for (int st = 0; st < 5; st++) if (b->stages >> st & 1) S.bst[st] = b->prog; break; }
            case OP_BIND_STATE: {
                const p_bind_state *b = (const p_bind_state *)a; blend_st bs;
                switch (b->kind) {
                    case 0: memcpy(&bs, b->bytes, sizeof bs); S.blend[bs.target] = bs; break;
                    case 1: memcpy(&S.color, b->bytes, sizeof S.color); break; case 2: memcpy(&S.chmask, b->bytes, sizeof S.chmask); break;
                    case 3: memcpy(&S.ms, b->bytes, sizeof S.ms); break; case 4: memcpy(&S.poly, b->bytes, sizeof S.poly); break; case 5: memcpy(&S.ds, b->bytes, sizeof S.ds); break;
                }
                break;
            }
            case OP_BIND_VATTR: { const p_bind_vattr *b = (const p_bind_vattr *)a; memcpy(S.attr, b->st, 4 * b->n); S.nattr = b->n; break; }
            case OP_BIND_VSTREAM: { const p_bind_vstream *b = (const p_bind_vstream *)a; memcpy(S.stream, b->st, 8 * b->n); S.nstream = b->n; break; }
            case OP_BIND_VBUF: case OP_BIND_VBUF1: { p_bind_vbuf t1; const p_bind_vbuf *b = vbuf_rec(h.op, a, &t1); if (b->first >= 0 && b->first + b->n <= 16) memcpy(&S.vbuf[b->first], b->r, sizeof *b->r * b->n); break; }
            case OP_BIND_UBO: case OP_BIND_UBO_SMALL: { p_bind_buf t1; const p_bind_buf *b = bind_buf_rec(h.op, a, &t1); if (b->stage < 6 && b->index < 16) S.ubo[b->stage][b->index] = *b; break; }
            case OP_BIND_SSBO: { const p_bind_buf *b = (const p_bind_buf *)a; if (b->stage < 6 && b->index < 16) S.ssbo[b->stage][b->index] = *b; break; }
            case OP_BIND_TEX: case OP_BIND_TEX1: case OP_BIND_SAMP: case OP_BIND_IMG: {
                p_bind_handles t1; const p_bind_handles *b = bind_rec(h.op, a, &t1); uint64_t hh[32]; memcpy(hh, b->h, 8 * (b->n < 32 ? b->n : 32));
                if (b->defer && b->src) memcpy(hh, (const void *)(uintptr_t)b->src, 8 * (b->n < 32 ? b->n : 32));   // BindTexturesDeferred: the handles as they are now
                uint64_t *dst = h.op == OP_BIND_TEX || h.op == OP_BIND_TEX1 ? S.tex[b->stage] : h.op == OP_BIND_SAMP ? S.samp[b->stage] : S.img[b->stage];
                int lim = h.op == OP_BIND_TEX || h.op == OP_BIND_TEX1 ? NTEX : h.op == OP_BIND_SAMP ? 128 : 32;
                if (b->stage < 6 && b->first >= 0 && b->first + b->n <= lim) memcpy(dst + b->first, hh, 8 * b->n);
                break;
            }
            case OP_STENCIL: { const p_stencil *b = (const p_stencil *)a; for (int f = 0; f < 2; f++) if (b->face & (1 << f)) { if (b->which == 0) S.st_vmask[f] = b->value_mask; else if (b->which == 1) S.st_wmask[f] = b->mask; else S.st_ref[f] = b->ref; } break; }
            case OP_POLY_OFFSET: S.po = *(const p_poly_offset *)a; break;
            case OP_VIEWPORTS: { const p_viewports *b = (const p_viewports *)a; for (int i = 0; i < b->n; i++) memcpy(S.vp[b->first + i], b->v[i], 16); S.has_vp = 1; break; }
            case OP_DEPTH_RANGE: { const p_depth_range *b = (const p_depth_range *)a; S.dr[0] = b->n; S.dr[1] = b->f; break; }
            case OP_BLEND_COLOR: memcpy(S.blend_color, ((const p_blend_color *)a)->rgba, 16); break;
            case OP_SET_TEX_POOL: case OP_SET_SAMP_POOL: { int nvn_dpool_base(const void *pool, int tex); int tex = h.op == OP_SET_TEX_POOL, bs = nvn_dpool_base(((const p_set_pool *)a)->pool, tex);
                if (tex) S.tex_base = bs; else S.samp_base = bs; break; }
            case OP_LINE_WIDTH: S.line_w = ((const p_line_width *)a)->w; break;
            case OP_PRIM_RESTART: S.restart = *(const p_prim_restart *)a; break;
            case OP_ALPHA_REF: break;   // the test itself is reported at the draw (not implemented)
            case OP_DRAW_TEX: RED_ONCE(49, "DrawTexture is not implemented: not drawn"); break;
            case OP_COPY_STRIDE: { const p_copy_stride *cs = (const p_copy_stride *)a; if (cs->image) S.copy_img_stride = cs->stride; else S.copy_stride = cs->stride; break; }
            default: break;
        }
        p += h.size;
    }
}
static void occ_flush(void);
uint64_t mtl_sub_commit(void) {
    pass_end(); occ_flush();
    vkCmdWriteTimestamp(C->cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, C->qp, 1);
    barrier_all(C->up);   // the uploads, clears and copies at the start are done and visible before the submission's own work (nothing orders two command buffers of one batch)
    VKCHECK(vkEndCommandBuffer(C->cb)); VKCHECK(vkEndCommandBuffer(C->up));
    uint64_t fence = ++g_fence; C->fence_val = fence;
    VkCommandBuffer cbs[2] = {C->up, C->cb};
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 2, cbs, 0, NULL};
    VKCHECK(vkResetFences(g_dev, 1, &C->fence));
    VkTimelineSemaphoreSubmitInfo ts = {VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO}; uint64_t wv, sv; VkPipelineStageFlags wst = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    pthread_mutex_lock(&g_qmu);
    // The game's submissions are ordered among themselves by the barrier each begins with (one queue: a barrier waits for everything submitted before it). Each still signals the
    // timeline (the present waits for the frame's last one), but waits on it only after a present: the Mali-G57 did not keep a present's blit in order with what followed, and a
    // semaphore wait on every one of Unreal's ~9 submissions a frame left the Adreno 825 idle between them (GPU 10% busy at 19 fps)
    if (g_tl) { sv = ++g_tlv; ts.signalSemaphoreValueCount = 1; ts.pSignalSemaphoreValues = &sv; si.pNext = &ts; si.signalSemaphoreCount = 1; si.pSignalSemaphores = &g_tl;
                if (g_after_present) { wv = sv - 1; ts.waitSemaphoreValueCount = 1; ts.pWaitSemaphoreValues = &wv; si.waitSemaphoreCount = 1; si.pWaitSemaphores = &g_tl; si.pWaitDstStageMask = &wst; }
                g_after_present = 0; }
    uint64_t ts0 = now_ns(); VkResult r = vkQueueSubmit(g_q, 1, &si, C->fence); g_t_sub += now_ns() - ts0; pthread_mutex_unlock(&g_qmu);
    { static uint64_t nsub; nsub++; extern uint64_t g_nsub_stat; g_nsub_stat = nsub; }
    if (r != VK_SUCCESS) { trace_dump("submission failed"); die("Vulkan: submission failed (%d)", (int)r); }
    pthread_mutex_lock(&g_mu); g_pend[g_npend++] = g_ci; pthread_cond_broadcast(&g_cv); pthread_mutex_unlock(&g_mu);
    return fence;
}
uint64_t mtl_last_fence(void) { return g_fence; }
int mtl_wait(uint64_t fence, uint64_t timeout_ns) {
    pthread_mutex_lock(&g_mu);
    if (timeout_ns == UINT64_MAX || timeout_ns > 60ull * 1000000000ull) { while (g_done < fence) pthread_cond_wait(&g_cv, &g_mu); }
    else {
        struct timespec t; clock_gettime(CLOCK_REALTIME, &t); uint64_t ns = (uint64_t)t.tv_nsec + timeout_ns; t.tv_sec += (time_t)(ns / 1000000000ull); t.tv_nsec = (long)(ns % 1000000000ull);
        while (g_done < fence) if (pthread_cond_timedwait(&g_cv, &g_mu, &t)) break;
    }
    int ok = g_done >= fence; pthread_mutex_unlock(&g_mu); return ok;
}
void mtl_finish(void) { mtl_wait(g_fence, UINT64_MAX); }
void mtl_drain(void) { mtl_finish(); pcache_save(); }   // going to the background: the pipelines made so far are kept for the next run

// ---------- Presentation: counted; DM_VK_SHOTS writes chosen frames as BMP files ----------
static void shot(const tex_t *t, uint64_t n) {
    static char *list; static const char *dir; static int init;
    if (!init) { init = 1; const char *e = getenv("DM_VK_SHOTS"); list = e ? strdup(e) : NULL; dir = getenv("DM_VK_SHOT_DIR"); if (!dir) dir = "."; }
    if (!list) return;
    char key[24]; snprintf(key, sizeof key, ",%llu,", (unsigned long long)n); char hay[1024]; snprintf(hay, sizeof hay, ",%s,", list);
    if (!strstr(hay, key)) return;
    mtl_finish();
    pthread_mutex_lock(&g_rmu); vtex *v = tex_find(t->serial); pthread_mutex_unlock(&g_rmu);
    if (!v || (v->f.f != VK_FORMAT_R8G8B8A8_UNORM && v->f.f != VK_FORMAT_R8G8B8A8_SRGB && v->f.f != VK_FORMAT_B8G8R8A8_UNORM)) { LOG("[RED] vk: frame %llu: the presented texture is not an 8-bit RGBA image (%s)", (unsigned long long)n, v ? "format" : "never drawn"); return; }
    uint32_t w = v->w, h = v->h; VkDeviceMemory m; void *px;
    VkBuffer b = buf_new((VkDeviceSize)w * h * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &m, &px);
    VkCommandBuffer cb = once_begin(); barrier_all(cb);
    VkBufferImageCopy c = {0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {w, h, 1}};
    vkCmdCopyImageToBuffer(cb, v->img, VK_IMAGE_LAYOUT_GENERAL, b, 1, &c); once_end(cb);
    char path[1024]; snprintf(path, sizeof path, "%s/frame%05llu.bmp", dir, (unsigned long long)n);
    FILE *f = fopen(path, "wb");
    if (f) {   // 32-bit BMP, bottom-up rows, BGRA
        uint32_t sz = 54 + w * h * 4; uint8_t hd[54] = {'B', 'M'}; memcpy(hd + 2, &sz, 4); uint32_t o = 54, hs = 40; memcpy(hd + 10, &o, 4); memcpy(hd + 14, &hs, 4);
        memcpy(hd + 18, &w, 4); memcpy(hd + 22, &h, 4); hd[26] = 1; hd[28] = 32;
        fwrite(hd, 1, 54, f);
        uint8_t *row = malloc((size_t)w * 4);
        for (uint32_t y = h; y-- > 0;) {
            const uint8_t *s = (const uint8_t *)px + (size_t)y * w * 4;
            for (uint32_t x = 0; x < w; x++) { int bgr = v->f.f == VK_FORMAT_B8G8R8A8_UNORM; row[4 * x] = s[4 * x + (bgr ? 0 : 2)]; row[4 * x + 1] = s[4 * x + 1]; row[4 * x + 2] = s[4 * x + (bgr ? 2 : 0)]; row[4 * x + 3] = 255; }
            fwrite(row, 1, (size_t)w * 4, f);
        }
        free(row); fclose(f); LOG("vk: frame %llu written to %s (%ux%u; %llu draws so far, %llu dropped)", (unsigned long long)n, path, w, h, (unsigned long long)g_stat_draws, (unsigned long long)g_stat_skip);
    }
    vkDestroyBuffer(g_dev, b, NULL); vkFreeMemory(g_dev, m, NULL);
}
// Diagnostics: DM_VK_RTDUMP=<frame> writes every color image the GPU rendered into, as it stands at that frame's present, to DM_VK_SHOT_DIR as
// rt_<serial>_<w>x<h>_<VkFormat>.raw (level 0, layer 0, tightly packed), for comparing a frame's passes with the Mac's; DM_VK_RTDUMP_DEC=1 adds the textures decoded
// from BC6H on upload. DM_VK_PASSLOG=<frame> logs that frame's passes (targets, clears) and draws (shaders, depth / stencil / blend state)
static int rt_bpp(VkFormat f) {
    switch (f) {
        case VK_FORMAT_R8_UNORM: return 1; case VK_FORMAT_R8G8_UNORM: case VK_FORMAT_R16_SFLOAT: case VK_FORMAT_R16_UNORM: return 2;
        case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_R8G8B8A8_SRGB: case VK_FORMAT_B8G8R8A8_UNORM: case VK_FORMAT_B8G8R8A8_SRGB: case VK_FORMAT_B10G11R11_UFLOAT_PACK32:
        case VK_FORMAT_A2B10G10R10_UNORM_PACK32: case VK_FORMAT_R16G16_SFLOAT: case VK_FORMAT_R16G16_UNORM: case VK_FORMAT_R16G16_SNORM: case VK_FORMAT_R32_SFLOAT: case VK_FORMAT_R8G8B8A8_SNORM: return 4;
        case VK_FORMAT_R16G16B16A16_SFLOAT: case VK_FORMAT_R32G32_SFLOAT: return 8; case VK_FORMAT_R32G32B32A32_SFLOAT: return 16;
        default: return 0;
    }
}
static void rt_dump(uint64_t n, int force) {
    static int64_t at = -2, cnt = 1; if (at == -2) { const char *e = getenv("DM_VK_RTDUMP"); at = e ? atoll(e) : -1; const char *c = e ? strchr(e, ',') : NULL; if (c) cnt = atoll(c + 1); }   // <frame>[,<frames>]
    if (!force && ((int64_t)n < at || (int64_t)n >= at + cnt)) return;
    const char *base = getenv("DM_VK_SHOT_DIR"); if (!base) base = ".";
    char dir[512]; if (cnt > 1) { snprintf(dir, sizeof dir, "%s/f%llu", base, (unsigned long long)n); mkdir(dir, 0777); chmod(dir, 0777); } else snprintf(dir, sizeof dir, "%s", base);
    mtl_finish(); int nd = 0;
    pthread_mutex_lock(&g_rmu);
    for (int hsh = 0; hsh < NHASH; hsh++) for (vtex *v = g_tex[hsh]; v; v = v->next) {
        int bpp = rt_bpp(v->f.f); if (!v->img) continue; if (!(v->gpu || (getenv("DM_VK_RTDUMP_DEC") && v->f.dec >= DEC_BC4)) || !bpp || !(v->aspect & VK_IMAGE_ASPECT_COLOR_BIT)) continue;
        VkDeviceMemory m; void *px; VkDeviceSize sz = (VkDeviceSize)v->w * v->h * bpp;
        VkBuffer b = buf_new(sz, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &m, &px);
        VkCommandBuffer cb = once_begin(); barrier_all(cb);
        VkBufferImageCopy c = {0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {v->w, v->h, 1}};
        vkCmdCopyImageToBuffer(cb, v->img, VK_IMAGE_LAYOUT_GENERAL, b, 1, &c); once_end(cb);
        char path[1024]; snprintf(path, sizeof path, "%s/rt_%llu_%ux%u_%d.raw", dir, (unsigned long long)v->serial, v->w, v->h, (int)v->f.f);
        FILE *f = fopen(path, "wb"); if (f) { fwrite(px, 1, (size_t)sz, f); fclose(f); chmod(path, 0644); nd++; }   // readable by adb (an app's files are 0600)
        vkDestroyBuffer(g_dev, b, NULL); vkFreeMemory(g_dev, m, NULL);
    }
    pthread_mutex_unlock(&g_rmu);
    LOG("vk: frame %llu: %d render targets written to %s", (unsigned long long)n, nd, dir);
}
// ---------- Window ----------
__attribute__((weak)) void *front_native_window(int *gen) { (void)gen; return NULL; }   // app/front_android.c; none in the adb front end
__attribute__((weak)) void front_window_lock(int on) { (void)on; }
// pre: the transform the swapchain was made with (a 180-degree one when the tablet is held upside down: the picture is turned in the final blit and the overlay's rectangles, so
// the compositor need not turn it); tf/cext: the surface's current transform and extent then, to tell a real change from a driver that keeps saying SUBOPTIMAL
static struct { VkSurfaceKHR surf; VkSwapchainKHR sc; VkImage img[8]; VkImageView view[8]; uint32_t n; VkFormat fmt; VkExtent2D ext, cext; int gen;
                VkSurfaceTransformFlagBitsKHR pre, tf; uint64_t made_ns, chk_ns, nsub; const char *why; } W;
#define NPRES 3
static struct { VkCommandPool pool; VkCommandBuffer cb; VkFence fence; VkSemaphore acq, done; VkFramebuffer fb; } g_pres[NPRES]; static int g_pi;
static const char *tf_name(VkSurfaceTransformFlagBitsKHR t) {
    switch (t) { case VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR: return "0"; case VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR: return "90"; case VK_SURFACE_TRANSFORM_ROTATE_180_BIT_KHR: return "180";
                 case VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR: return "270"; default: return "mirrored"; }
}
static void win_drop(void) {
    // the queue is externally synchronized: no submission while the device drains
    if (W.sc) { pthread_mutex_lock(&g_qmu); vkDeviceWaitIdle(g_dev); pthread_mutex_unlock(&g_qmu); for (uint32_t i = 0; i < W.n; i++) vkDestroyImageView(g_dev, W.view[i], NULL); vkDestroySwapchainKHR(g_dev, W.sc, NULL); W.sc = VK_NULL_HANDLE; }
    if (W.surf) { vkDestroySurfaceKHR(g_inst, W.surf, NULL); W.surf = VK_NULL_HANDLE; }
}
static int win_make(ANativeWindow *aw) {
    VkAndroidSurfaceCreateInfoKHR si = {VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR, NULL, 0, aw};
    if (vkCreateAndroidSurfaceKHR(g_inst, &si, NULL, &W.surf) != VK_SUCCESS) { RED_ONCE(90, "no Vulkan surface for the window"); return 0; }
    VkBool32 ok = 0; vkGetPhysicalDeviceSurfaceSupportKHR(g_pd, g_qf, W.surf, &ok); if (!ok) { RED_ONCE(91, "the queue cannot present to the window"); return 0; }
    VkSurfaceCapabilitiesKHR cap; vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g_pd, W.surf, &cap);
    uint32_t nf = 0; vkGetPhysicalDeviceSurfaceFormatsKHR(g_pd, W.surf, &nf, NULL); VkSurfaceFormatKHR sf[32]; if (nf > 32) nf = 32; vkGetPhysicalDeviceSurfaceFormatsKHR(g_pd, W.surf, &nf, sf);
    VkSurfaceFormatKHR pick = sf[0]; for (uint32_t i = 0; i < nf; i++) if (sf[i].format == VK_FORMAT_R8G8B8A8_UNORM || sf[i].format == VK_FORMAT_B8G8R8A8_UNORM) { pick = sf[i]; break; }
    uint32_t npm = 0; vkGetPhysicalDeviceSurfacePresentModesKHR(g_pd, W.surf, &npm, NULL); VkPresentModeKHR pm[8]; if (npm > 8) npm = 8; vkGetPhysicalDeviceSurfacePresentModesKHR(g_pd, W.surf, &npm, pm);
    VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR; for (uint32_t i = 0; i < npm; i++) if (pm[i] == VK_PRESENT_MODE_MAILBOX_KHR) mode = pm[i];   // the host paces presents itself; mailbox never blocks it
    // The swapchain is made the window's way up and Android's compositor turns it (a phone whose screen is portrait by nature, playing in landscape, reports its
    // current extent and transform rotated); the window's own size is that way up
    // Upright or upside down the swapchain is made the screen's current way (180 degrees is turned by us, for free in the blit); a quarter turn is left to the compositor
    VkSurfaceTransformFlagBitsKHR pre = cap.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR : cap.currentTransform;
    if (cap.currentTransform == VK_SURFACE_TRANSFORM_ROTATE_180_BIT_KHR && (cap.supportedTransforms & VK_SURFACE_TRANSFORM_ROTATE_180_BIT_KHR)) pre = VK_SURFACE_TRANSFORM_ROTATE_180_BIT_KHR;
    W.pre = pre; W.tf = cap.currentTransform; W.cext = cap.currentExtent;
    W.ext = (VkExtent2D){(uint32_t)ANativeWindow_getWidth(aw), (uint32_t)ANativeWindow_getHeight(aw)};
    if (pre == cap.currentTransform && cap.currentExtent.width != UINT32_MAX) W.ext = cap.currentExtent;
    if (W.ext.width < cap.minImageExtent.width) W.ext.width = cap.minImageExtent.width; if (W.ext.width > cap.maxImageExtent.width) W.ext.width = cap.maxImageExtent.width;
    if (W.ext.height < cap.minImageExtent.height) W.ext.height = cap.minImageExtent.height; if (W.ext.height > cap.maxImageExtent.height) W.ext.height = cap.maxImageExtent.height;
    uint32_t nimg = cap.minImageCount + 1; if (cap.maxImageCount && nimg > cap.maxImageCount) nimg = cap.maxImageCount;
    VkCompositeAlphaFlagBitsKHR ca = cap.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR : VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
    VkSwapchainCreateInfoKHR ci = {VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, NULL, 0, W.surf, nimg, pick.format, pick.colorSpace, W.ext, 1,
                                   VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL, pre, ca, mode, VK_TRUE, VK_NULL_HANDLE};
    if (vkCreateSwapchainKHR(g_dev, &ci, NULL, &W.sc) != VK_SUCCESS) { RED_ONCE(92, "no swapchain for the window"); return 0; }
    W.fmt = pick.format; W.n = 8; vkGetSwapchainImagesKHR(g_dev, W.sc, &W.n, W.img);
    for (uint32_t i = 0; i < W.n; i++) { VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, W.img[i], VK_IMAGE_VIEW_TYPE_2D, W.fmt, {0}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
                                         VKCHECK(vkCreateImageView(g_dev, &vi, NULL, &W.view[i])); }
    if (!g_pres[0].pool) for (int i = 0; i < NPRES; i++) {
        VkCommandPoolCreateInfo pi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, g_qf}; VKCHECK(vkCreateCommandPool(g_dev, &pi, NULL, &g_pres[i].pool));
        VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, g_pres[i].pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1}; VKCHECK(vkAllocateCommandBuffers(g_dev, &ai, &g_pres[i].cb));
        VkFenceCreateInfo fi = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, NULL, VK_FENCE_CREATE_SIGNALED_BIT}; VKCHECK(vkCreateFence(g_dev, &fi, NULL, &g_pres[i].fence));
        VkSemaphoreCreateInfo sci = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO}; VKCHECK(vkCreateSemaphore(g_dev, &sci, NULL, &g_pres[i].acq)); VKCHECK(vkCreateSemaphore(g_dev, &sci, NULL, &g_pres[i].done));
    }
    LOG("vk: swapchain %ux%u, %u images, format %d, %s, transform %s (screen %s), why: %s", W.ext.width, W.ext.height, W.n, W.fmt, mode == VK_PRESENT_MODE_MAILBOX_KHR ? "mailbox" : "fifo",
        tf_name(pre), tf_name(cap.currentTransform), W.why ? W.why : "new window");
    W.why = NULL; W.made_ns = now_ns(); W.nsub = 0;
    return 1;
}
// A present or acquire said SUBOPTIMAL. Android says it every frame while the swapchain's transform is not the screen's (a quarter turn, which the compositor does), so
// making a new swapchain then only shows black frames, over and over. A new one is made only when the screen's transform or size really changed since this one was made,
// and at most twice a second
static void win_suboptimal(void) {
    W.nsub++;
    uint64_t t = now_ns(); if (t - W.made_ns < 500000000ull || t - W.chk_ns < 500000000ull) return;
    W.chk_ns = t; VkSurfaceCapabilitiesKHR cap; if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g_pd, W.surf, &cap) != VK_SUCCESS) return;
    if (cap.currentTransform != W.tf) W.why = "the screen turned";
    else if (cap.currentExtent.width != W.cext.width || cap.currentExtent.height != W.cext.height) W.why = "the window's size changed";
    else return;
    LOG("vk: suboptimal %llu times; %s (transform %s -> %s, %ux%u -> %ux%u)", (unsigned long long)W.nsub, W.why, tf_name(W.tf), tf_name(cap.currentTransform),
        W.cext.width, W.cext.height, cap.currentExtent.width, cap.currentExtent.height);
    W.gen = -1;
}
// The overlay's rectangles in window coordinates, turned with the swapchain
static void ovl_clear(VkCommandBuffer cb, const VkClearAttachment *a, uint32_t n, VkClearRect *r) {
    if (W.pre == VK_SURFACE_TRANSFORM_ROTATE_180_BIT_KHR)
        for (uint32_t i = 0; i < n; i++) { r[i].rect.offset.x = (int32_t)W.ext.width - r[i].rect.offset.x - (int32_t)r[i].rect.extent.width;
                                           r[i].rect.offset.y = (int32_t)W.ext.height - r[i].rect.offset.y - (int32_t)r[i].rect.extent.height; }
    vkCmdClearAttachments(cb, 1, a, n, r);
}
// Over the picture: the frame-rate counter (cleared rectangles, a 3x5 font) and the on-screen controls (front_overlay): sprites from the controls' atlas, textured quads
// alpha-blended over the picture (ovl_sprites), brighter while pressed; without an atlas (the adb program, an APK without one) button outlines and labels as before
typedef struct { float x, y, w, h; int on; char label[3]; int sprite; float alpha; } dm_ovl;   // sprite: cell of the atlas, -1 none; alpha: the layout's opacity (app/front_android.c)
__attribute__((weak)) int front_overlay(dm_ovl *out, int max) { (void)out; (void)max; return 0; }
// The atlas (app/front_android.c: the APK's assets/controls/atlas.png, decoded premultiplied RGBA; dm/android/tools/controls_atlas.py makes it from the SVG masters in app/res_svg)
__attribute__((weak)) int front_overlay_atlas(const void **rgba, int *w, int *h, int *cell, int *cols) { (void)rgba; (void)w; (void)h; (void)cell; (void)cols; return 0; }
__attribute__((weak)) void front_overlay_atlas_release(void) {}
static const uint32_t OVL_VS[] =
#include "ovl_vert.inc"
;
static const uint32_t OVL_FS[] =
#include "ovl_frag.inc"
;
static struct { int state; VkImage img; VkDeviceMemory mem; VkImageView view; VkSampler samp; VkDescriptorSetLayout dsl; VkDescriptorPool dp; VkDescriptorSet ds; VkPipelineLayout pl;
                VkPipeline pipe; VkFormat pfmt; int w, h, cell, cols; } G_;   // state: 0 not tried, 1 ready, -1 no atlas
typedef struct { float rect[4], uv[4], tint[4], scr[4]; } ovl_pc;   // ovl.vert / ovl.frag push constants
static void ovl_layout(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to, VkAccessFlags sa, VkAccessFlags da, VkPipelineStageFlags ss, VkPipelineStageFlags ds) {
    VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, sa, da, from, to, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, img, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    vkCmdPipelineBarrier(cb, ss, ds, 0, 0, NULL, 0, NULL, 1, &b);
}
static int ovl_init(void) {   // the atlas as a sampled image, once (the first frame with on-screen controls)
    if (G_.state) return G_.state > 0;
    G_.state = -1;
    const void *px = NULL; int w = 0, h = 0, cell = 0, cols = 0;
    if (!front_overlay_atlas(&px, &w, &h, &cell, &cols) || !px || w <= 0 || h <= 0 || cell <= 0 || cols <= 0) { LOG("vk: on-screen controls: no sprite atlas, drawn as outlines"); return 0; }
    VkImageCreateInfo ii = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, 0, VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, {(uint32_t)w, (uint32_t)h, 1}, 1, 1, VK_SAMPLE_COUNT_1_BIT,
                            VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_IMAGE_LAYOUT_UNDEFINED};
    if (vkCreateImage(g_dev, &ii, NULL, &G_.img) != VK_SUCCESS) { front_overlay_atlas_release(); return 0; }
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(g_dev, G_.img, &mr); VkDeviceSize off; img_mem(mr, &G_.mem, &off); VKCHECK(vkBindImageMemory(g_dev, G_.img, G_.mem, off));
    VkDeviceSize n = (VkDeviceSize)w * h * 4; VkDeviceMemory sm; void *map; VkBuffer sb = buf_new(n, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &sm, &map); memcpy(map, px, n);
    front_overlay_atlas_release();
    VkCommandBuffer cb = once_begin();
    ovl_layout(cb, G_.img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy bc = {0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {(uint32_t)w, (uint32_t)h, 1}};
    vkCmdCopyBufferToImage(cb, sb, G_.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bc);
    ovl_layout(cb, G_.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
               VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    once_end(cb); vkDestroyBuffer(g_dev, sb, NULL); vkFreeMemory(g_dev, sm, NULL);
    VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, G_.img, VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, {0}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VKCHECK(vkCreateImageView(g_dev, &vi, NULL, &G_.view));
    VkSamplerCreateInfo si = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, NULL, 0, VK_FILTER_LINEAR, VK_FILTER_LINEAR, VK_SAMPLER_MIPMAP_MODE_NEAREST, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                              VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, 0, VK_FALSE, 1, VK_FALSE, VK_COMPARE_OP_ALWAYS, 0, 0, VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK, VK_FALSE};
    VKCHECK(vkCreateSampler(g_dev, &si, NULL, &G_.samp));
    VkDescriptorSetLayoutBinding lb = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL};
    VkDescriptorSetLayoutCreateInfo li = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, 1, &lb}; VKCHECK(vkCreateDescriptorSetLayout(g_dev, &li, NULL, &G_.dsl));
    VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1}; VkDescriptorPoolCreateInfo pi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, 0, 1, 1, &ps};
    VKCHECK(vkCreateDescriptorPool(g_dev, &pi, NULL, &G_.dp));
    VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, G_.dp, 1, &G_.dsl}; VKCHECK(vkAllocateDescriptorSets(g_dev, &ai, &G_.ds));
    VkDescriptorImageInfo di = {G_.samp, G_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet wd = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, G_.ds, 0, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &di, NULL, NULL}; vkUpdateDescriptorSets(g_dev, 1, &wd, 0, NULL);
    VkPushConstantRange pr = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(ovl_pc)};
    VkPipelineLayoutCreateInfo pli = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 1, &G_.dsl, 1, &pr}; VKCHECK(vkCreatePipelineLayout(g_dev, &pli, NULL, &G_.pl));
    G_.w = w; G_.h = h; G_.cell = cell; G_.cols = cols; G_.state = 1;
    LOG("vk: on-screen controls: sprite atlas %dx%d (cells of %d) ready", w, h, cell);
    return 1;
}
static int ovl_pipe(VkFormat fmt) {   // the sprites' pipeline for the swapchain's format (made again only if a new swapchain has another one; the old one is left, as frames in flight may use it)
    if (G_.pipe && G_.pfmt == fmt) return 1;
    VkShaderModule m[2]; VkShaderModuleCreateInfo mi[2] = {{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, sizeof OVL_VS, OVL_VS}, {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, sizeof OVL_FS, OVL_FS}};
    VKCHECK(vkCreateShaderModule(g_dev, &mi[0], NULL, &m[0])); VKCHECK(vkCreateShaderModule(g_dev, &mi[1], NULL, &m[1]));
    VkPipelineShaderStageCreateInfo st[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_VERTEX_BIT, m[0], "main", NULL},
                                             {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_FRAGMENT_BIT, m[1], "main", NULL}};
    VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, NULL, 0, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP, VK_FALSE};
    VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, NULL, 0, 1, NULL, 1, NULL};
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, NULL, 0, VK_FALSE, VK_FALSE, VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE, VK_FRONT_FACE_COUNTER_CLOCKWISE, VK_FALSE, 0, 0, 0, 1};
    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, NULL, 0, VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState ba = {VK_TRUE, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_OP_ADD, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_OP_ADD, 0xf};   // premultiplied
    VkPipelineColorBlendStateCreateInfo cbs = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, NULL, 0, VK_FALSE, VK_LOGIC_OP_COPY, 1, &ba, {0}};
    VkDynamicState dy[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR}; VkPipelineDynamicStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, NULL, 0, 2, dy};
    VkPipelineRenderingCreateInfo prc = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, NULL, 0, 1, &fmt, VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED};
    VkAttachmentLoadOp ld = VK_ATTACHMENT_LOAD_OP_LOAD;
    VkGraphicsPipelineCreateInfo ci = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, g_dynrend ? &prc : NULL, 0, 2, st, &vi, &ia, NULL, &vp, &rs, &ms, NULL, &cbs, &ds, G_.pl,
                                       g_dynrend ? VK_NULL_HANDLE : rp_get(&fmt, &ld, 1, VK_FORMAT_UNDEFINED, 0, ld, ld, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL), 0, VK_NULL_HANDLE, -1};
    VkPipeline p; VkResult r = vkCreateGraphicsPipelines(g_dev, g_pcache, 1, &ci, NULL, &p);
    vkDestroyShaderModule(g_dev, m[0], NULL); vkDestroyShaderModule(g_dev, m[1], NULL);
    if (r != VK_SUCCESS) { RED_ONCE(94, "the on-screen controls' pipeline was not created (%d): drawn as outlines", (int)r); G_.state = -1; return 0; }
    G_.pipe = p; G_.pfmt = fmt; return 1;
}
static void ovl_sprites(VkCommandBuffer cb, const dm_ovl *ov, int nov) {   // inside the overlay's pass: each sprite item a quad
    VkViewport v = {0, 0, (float)W.ext.width, (float)W.ext.height, 0, 1}; VkRect2D sc = {{0, 0}, W.ext};
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, G_.pipe); vkCmdSetViewport(cb, 0, 1, &v); vkCmdSetScissor(cb, 0, 1, &sc);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, G_.pl, 0, 1, &G_.ds, 0, NULL);
    float iw = 1.0f / (float)W.ext.width, ih = 1.0f / (float)W.ext.height, rot = W.pre == VK_SURFACE_TRANSFORM_ROTATE_180_BIT_KHR;
    for (int i = 0; i < nov; i++) {
        const dm_ovl *o = &ov[i]; if (o->sprite < 0 || o->alpha <= 0) continue;
        int col = o->sprite % G_.cols, row = o->sprite / G_.cols;
        ovl_pc pc = {{o->x, o->y, o->w, o->h}, {((float)(col * G_.cell) + 0.5f) / (float)G_.w, ((float)(row * G_.cell) + 0.5f) / (float)G_.h, (float)(G_.cell - 1) / (float)G_.w, (float)(G_.cell - 1) / (float)G_.h},
                     {o->on ? 1.25f : 1.0f, o->on ? 0.12f : 0.0f, o->alpha, 0}, {iw, ih, rot, 0}};
        vkCmdPushConstants(cb, G_.pl, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, &pc);
        vkCmdDraw(cb, 4, 1, 0, 0);
    }
}
static double g_fps_shown;
static uint16_t glyph(char c) {   // rows top to bottom, 3 bits each
    static const char *const G[] = {"0111101101101111", "1010010010010010", "2111001111100111", "3111001111001111", "4101101111001001", "5111100111001111", "6111100111101111",
                                    "7111001001001001", "8111101111101111", "9111101111001111", "A010101111101101", "B110101110101110", "X101101010101101", "Y101101010010010",
                                    "L100100100100111", "R110101110101101", "Z111001010100111", "+000010111010000", "-000000111000000",
                                    "M101111111101101", "E111100111100111", "N110101101101101", "I111010010010111", "P110101110100100", "K101101110101101", "S111100111001111",
                                    "H101101111101101", "D110101101101110", "T111010010010010", "U101101101101111", "W101101111111101", "/001001010100100", ".000000000000010",
                                    "J001001001101111"};
    for (size_t i = 0; i < sizeof G / sizeof *G; i++) if (G[i][0] == c) { uint16_t g = 0; for (int k = 1; k <= 15; k++) g = (uint16_t)(g << 1 | (G[i][k] == '1')); return g; }
    return 0;
}
static uint32_t text_rects(VkClearRect *r, uint32_t nr, uint32_t max, const char *t, int32_t x0, int32_t y0, uint32_t cell) {
    for (size_t d = 0; t[d] && nr + 15 <= max; d++) { uint16_t g = glyph(t[d]);
        for (int row = 0; row < 5; row++) for (int c = 0; c < 3; c++) if (g >> (14 - 3 * row - c) & 1)
            r[nr++] = (VkClearRect){{{x0 + (int32_t)((d * 4 + c) * cell), y0 + (int32_t)(row * cell)}, {cell, cell}}, 0, 1}; }
    return nr;
}
static VkFramebuffer overlay_draw(VkCommandBuffer cb, uint32_t img, int show_fps, const dm_ovl *ov, int nov) {
    rp_desc d = {.n = 1, .cv = {W.view[img]}, .cf = {W.fmt}, .cl = {VK_ATTACHMENT_LOAD_OP_LOAD}, .w = W.ext.width, .h = W.ext.height, .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkFramebuffer fb = rp_begin(cb, &d);
    enum { MAXR = 1024 }; static VkClearRect lines[MAXR], fills[MAXR], fg[MAXR], bg[64]; uint32_t nl = 0, nf = 0, ng = 0, nb = 0;
    #define CLIP(x0, y0, w_, h_) do { int32_t X = (int32_t)(x0), Y = (int32_t)(y0), XW = X + (int32_t)(w_), YH = Y + (int32_t)(h_); if (X < 0) X = 0; if (Y < 0) Y = 0; \
        if (XW > (int32_t)W.ext.width) XW = (int32_t)W.ext.width; if (YH > (int32_t)W.ext.height) YH = (int32_t)W.ext.height; \
        if (XW > X && YH > Y) rect = (VkClearRect){{{X, Y}, {(uint32_t)(XW - X), (uint32_t)(YH - Y)}}, 0, 1}; else rect.rect.extent.width = 0; } while (0)
    VkClearRect rect;
    if (show_fps) {
        char t[8]; snprintf(t, sizeof t, "%.0f", g_fps_shown); uint32_t cell = W.ext.height / 120 ? W.ext.height / 120 : 2;
        CLIP(cell * 2, cell * 2, (strlen(t) * 4 + 1) * cell, 7 * cell); if (rect.rect.extent.width) bg[nb++] = rect;
        ng = text_rects(fg, ng, MAXR, t, (int32_t)(cell * 3), (int32_t)(cell * 3), cell);
    }
    int spr = nov && ovl_init() && ovl_pipe(W.fmt);   // sprites: their own pipeline, after the rectangles
    for (int i = 0; i < nov && nl + 4 <= MAXR; i++) {
        const dm_ovl *o = &ov[i]; float t = o->w / 18 > 2 ? o->w / 18 : 2;
        if (spr && o->sprite >= 0) continue;
        if (o->on) { CLIP(o->x, o->y, o->w, o->h); if (rect.rect.extent.width && nf < MAXR) fills[nf++] = rect; }
        CLIP(o->x, o->y, o->w, t); if (rect.rect.extent.width) lines[nl++] = rect;
        CLIP(o->x, o->y + o->h - t, o->w, t); if (rect.rect.extent.width) lines[nl++] = rect;
        CLIP(o->x, o->y, t, o->h); if (rect.rect.extent.width) lines[nl++] = rect;
        CLIP(o->x + o->w - t, o->y, t, o->h); if (rect.rect.extent.width) lines[nl++] = rect;
        if (o->label[0]) { uint32_t cell = (uint32_t)(o->h / 9) ? (uint32_t)(o->h / 9) : 1, tw = (uint32_t)strlen(o->label) * 4 * cell - cell;
                           ng = text_rects(fg, ng, MAXR, o->label, (int32_t)(o->x + (o->w - tw) / 2), (int32_t)(o->y + (o->h - 5 * cell) / 2), cell); }
    }
    #undef CLIP
    VkClearAttachment black = {VK_IMAGE_ASPECT_COLOR_BIT, 0, {.color = {{0, 0, 0, 1}}}}, white = {VK_IMAGE_ASPECT_COLOR_BIT, 0, {.color = {{0.85f, 0.85f, 0.9f, 1}}}},
                      grey = {VK_IMAGE_ASPECT_COLOR_BIT, 0, {.color = {{0.35f, 0.35f, 0.45f, 1}}}}, green = {VK_IMAGE_ASPECT_COLOR_BIT, 0, {.color = {{0.3f, 1, 0.3f, 1}}}};
    if (nb) ovl_clear(cb, &black, nb, bg);
    if (nf) ovl_clear(cb, &grey, nf, fills);
    if (nl) ovl_clear(cb, &white, nl, lines);
    if (ng) ovl_clear(cb, nov ? &white : &green, ng, fg);
    if (spr) ovl_sprites(cb, ov, nov);
    rp_end(cb);
    return fb;
}
static void img_barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to) {
    VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, from, to,
                              VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, img, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
}
// The window is going away (app/front_android.c, NativeActivity onNativeWindowDestroyed, holding front_window_lock): the swapchain and surface go now, before Android
// takes the window back; the device, pipelines and every game image stay. The next present after a new window makes a new swapchain (the generation differs)
void dm_vk_window_lost(void) { if (!g_dev) return; win_drop(); W.gen = -1; LOG("vk: window gone: swapchain and surface released"); }
static void win_present(const tex_t *t, const int crop[4]) {
    int gen; ANativeWindow *aw = front_native_window(&gen);
    front_window_lock(1);
    aw = front_native_window(&gen);
    if (!aw || gen != W.gen) { win_drop(); W.gen = gen; if (aw) win_make(aw); }
    if (!aw || !W.sc) { front_window_lock(0); return; }
    pthread_mutex_lock(&g_rmu); vtex *v = tex_find(t->serial); pthread_mutex_unlock(&g_rmu);
    if (!v) { front_window_lock(0); return; }
    typeof(g_pres[0]) *P_ = &g_pres[g_pi]; g_pi = (g_pi + 1) % NPRES;
    { uint64_t w0 = now_ns(); vkWaitForFences(g_dev, 1, &P_->fence, VK_TRUE, UINT64_MAX); atomic_fetch_add(&g_pwait_ns, now_ns() - w0); }
    if (P_->fb) { vkDestroyFramebuffer(g_dev, P_->fb, NULL); P_->fb = VK_NULL_HANDLE; }
    uint32_t idx; VkResult r = vkAcquireNextImageKHR(g_dev, W.sc, 1000000000ull, P_->acq, VK_NULL_HANDLE, &idx);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) { W.gen = -1; W.why = "out of date at acquire"; front_window_lock(0); return; }
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) { front_window_lock(0); return; }
    if (r == VK_SUBOPTIMAL_KHR) win_suboptimal();
    vkResetFences(g_dev, 1, &P_->fence);
    VkCommandBuffer cb = P_->cb; vkResetCommandBuffer(cb, 0);
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL}; vkBeginCommandBuffer(cb, &bi);
    barrier_all(cb);   // the game's submissions before this one wrote the image
    img_barrier(cb, W.img[idx], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkClearColorValue black = {{0, 0, 0, 1}}; VkImageSubresourceRange rr = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cb, W.img[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &rr);
    int32_t sx = crop ? crop[0] : 0, sy = crop ? crop[1] : 0, sw = crop && crop[2] ? crop[2] : (int32_t)v->w, sh = crop && crop[3] ? crop[3] : (int32_t)v->h;
    static int stretch = -1; if (stretch < 0) { const char *e = getenv("DM_SCREEN_FIT"); stretch = e && !strcmp(e, "stretch"); }
    int32_t dw = (int32_t)W.ext.width, dh = (int32_t)W.ext.height, dx = 0, dy = 0;
    if (!stretch) { if ((int64_t)dw * sh > (int64_t)dh * sw) { dw = (int32_t)((int64_t)dh * sw / sh); dx = ((int32_t)W.ext.width - dw) / 2; } else { dh = (int32_t)((int64_t)dw * sh / sw); dy = ((int32_t)W.ext.height - dh) / 2; } }
    VkImageBlit b = {{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {{sx, sy, 0}, {sx + sw, sy + sh, 1}}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {{dx, dy, 0}, {dx + dw, dy + dh, 1}}};
    if (W.pre == VK_SURFACE_TRANSFORM_ROTATE_180_BIT_KHR) {   // upside down: the destination corners swapped turn the picture half round
        b.dstOffsets[0] = (VkOffset3D){(int32_t)W.ext.width - dx, (int32_t)W.ext.height - dy, 0}; b.dstOffsets[1] = (VkOffset3D){(int32_t)W.ext.width - dx - dw, (int32_t)W.ext.height - dy - dh, 1}; }
    vkCmdBlitImage(cb, v->img, VK_IMAGE_LAYOUT_GENERAL, W.img[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b, VK_FILTER_LINEAR);
    static int show = -1; if (show < 0) { const char *e = getenv("DM_SHOW_FPS"); show = e && atoi(e); }
    static dm_ovl ov[64]; int nov = front_overlay(ov, 64);   // buttons, and a ring of dots and a knob per stick held
    if (show || nov) { img_barrier(cb, W.img[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL); P_->fb = overlay_draw(cb, idx, show, ov, nov);
                img_barrier(cb, W.img[idx], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR); }
    else img_barrier(cb, W.img[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    vkEndCommandBuffer(cb);
    VkPipelineStageFlags ws[2] = {VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT};
    VkSemaphore wsem[2] = {P_->acq, g_tl}, ssem[2] = {P_->done, g_tl}; uint64_t wv[2] = {0, 0}, sv[2] = {0, 0};
    VkTimelineSemaphoreSubmitInfo ts = {VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO, NULL, 2, wv, 2, sv};
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, g_tl ? &ts : NULL, g_tl ? 2 : 1, wsem, ws, 1, &cb, g_tl ? 2 : 1, ssem};
    VkPresentInfoKHR pi = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, NULL, 1, &P_->done, 1, &W.sc, &idx, NULL};
    pthread_mutex_lock(&g_qmu);
    if (g_tl) { wv[1] = g_tlv; sv[1] = ++g_tlv; g_after_present = 1; }
    vkQueueSubmit(g_q, 1, &si, P_->fence); r = vkQueuePresentKHR(g_q, &pi); pthread_mutex_unlock(&g_qmu);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) { W.gen = -1; W.why = "out of date at present"; } else if (r == VK_SUBOPTIMAL_KHR && W.gen != -1) win_suboptimal();
    front_window_lock(0);
}
static _Atomic uint64_t g_presents;
// ---------- Shader pre-warm before the game starts ----------
// Every pipeline earlier runs made (pipeline_keys.bin) is made again before the game boots, on worker threads, behind a screen "MENYIAPKAN SHADER x / y" with a progress bar, so
// effects and skills met before never stall the game for a first-time shader load or compile (100-300 ms each in FINAL FANTASY RESONANCE's battles). The player's own tap on the
// screen skips the wait (dm_vk_prewarm_cancel, from the front end's input); the rest is made in the background as the game starts. The first run after the list grows takes
// longer (real compiles), later ones only seconds (the driver's pipeline cache)
static _Atomic int g_pw_cancel; static pso_rec *g_pw_recs; static uint32_t g_pw_total; static _Atomic uint32_t g_pw_next, g_pw_done;
void dm_vk_prewarm_cancel(void) { atomic_store(&g_pw_cancel, 1); }
static void *pw_worker(void *a) {
    (void)a;
    for (uint32_t i; (i = atomic_fetch_add(&g_pw_next, 1)) < g_pw_total;) { pso_precreate_one(&g_pw_recs[i]); atomic_fetch_add(&g_pw_done, 1); }
    return NULL;
}
static void screen_draw(const char *t1, const char *t2, uint32_t done, uint32_t total);
static void pw_draw(uint32_t done, uint32_t total) {   // one frame of the pre-warm screen
    char t1[64]; snprintf(t1, sizeof t1, "MENYIAPKAN SHADER %u / %u", done, total);
    screen_draw(t1, "KETUK UNTUK LEWATI", done, total);
}
// The hibernation restore's screen (app/front_android.c, before the host reads the snapshot): drawn until the game's first frame replaces it
void dm_vk_message(const char *t1, const char *t2) {
    vk_init();
    int gen; for (int i = 0; i < 40; i++) { front_window_lock(1); ANativeWindow *aw = front_native_window(&gen); front_window_lock(0); if (aw) break; usleep(50000); }
    for (int i = 0; i < 3; i++) screen_draw(t1, t2, 0, 0);
}
static void screen_draw(const char *t1, const char *t2, uint32_t done, uint32_t total) {   // one frame: t1 large, t2 small, a progress bar when total > 0
    int gen; front_window_lock(1); ANativeWindow *aw = front_native_window(&gen);
    if (!aw || gen != W.gen) { win_drop(); W.gen = gen; if (aw) win_make(aw); }
    if (!aw || !W.sc) { front_window_lock(0); return; }
    typeof(g_pres[0]) *P_ = &g_pres[g_pi]; g_pi = (g_pi + 1) % NPRES;
    vkWaitForFences(g_dev, 1, &P_->fence, VK_TRUE, UINT64_MAX);
    if (P_->fb) { vkDestroyFramebuffer(g_dev, P_->fb, NULL); P_->fb = VK_NULL_HANDLE; }
    uint32_t idx; VkResult r = vkAcquireNextImageKHR(g_dev, W.sc, 1000000000ull, P_->acq, VK_NULL_HANDLE, &idx);
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) { if (r == VK_ERROR_OUT_OF_DATE_KHR) { W.gen = -1; W.why = "out of date at acquire"; } front_window_lock(0); return; }
    if (r == VK_SUBOPTIMAL_KHR) win_suboptimal();
    vkResetFences(g_dev, 1, &P_->fence);
    VkCommandBuffer cb = P_->cb; vkResetCommandBuffer(cb, 0);
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL}; vkBeginCommandBuffer(cb, &bi);
    img_barrier(cb, W.img[idx], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkClearColorValue bgc = {{0.06f, 0.07f, 0.11f, 1}}; VkImageSubresourceRange rr = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cb, W.img[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &bgc, 1, &rr);
    img_barrier(cb, W.img[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    rp_desc d = {.n = 1, .cv = {W.view[idx]}, .cf = {W.fmt}, .cl = {VK_ATTACHMENT_LOAD_OP_LOAD}, .w = W.ext.width, .h = W.ext.height, .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    P_->fb = rp_begin(cb, &d);
    static VkClearRect tr[2048]; uint32_t nt = 0, ww = W.ext.width, hh = W.ext.height, cell = hh / 90 ? hh / 90 : 2;
    nt = text_rects(tr, nt, 2048, t1, (int32_t)((ww - strlen(t1) * 4 * cell) / 2), (int32_t)(hh / 2 - 9 * cell), cell);
    uint32_t c2 = cell * 2 / 3 ? cell * 2 / 3 : 1; nt = text_rects(tr, nt, 2048, t2, (int32_t)((ww - strlen(t2) * 4 * c2) / 2), (int32_t)(hh / 2 + 8 * cell), c2);
    uint32_t bw = ww * 6 / 10, bx = (ww - bw) / 2, by = hh / 2, bh = cell * 3, fw = total ? (uint32_t)((uint64_t)bw * done / total) : 0;
    VkClearRect outline = {{{(int32_t)bx, (int32_t)by}, {bw, bh}}, 0, 1}, fill = {{{(int32_t)bx, (int32_t)by}, {fw ? fw : 1, bh}}, 0, 1};
    VkClearAttachment white = {VK_IMAGE_ASPECT_COLOR_BIT, 0, {.color = {{0.85f, 0.85f, 0.9f, 1}}}}, dark = {VK_IMAGE_ASPECT_COLOR_BIT, 0, {.color = {{0.2f, 0.22f, 0.3f, 1}}}},
                      bar = {VK_IMAGE_ASPECT_COLOR_BIT, 0, {.color = {{0.35f, 0.7f, 1.0f, 1}}}};
    if (total) { ovl_clear(cb, &dark, 1, &outline); if (fw) ovl_clear(cb, &bar, 1, &fill); }
    if (nt) ovl_clear(cb, &white, nt, tr);
    rp_end(cb);
    img_barrier(cb, W.img[idx], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    vkEndCommandBuffer(cb);
    VkPipelineStageFlags ws = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 1, &P_->acq, &ws, 1, &cb, 1, &P_->done};
    VkPresentInfoKHR pi = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, NULL, 1, &P_->done, 1, &W.sc, &idx, NULL};
    pthread_mutex_lock(&g_qmu); vkQueueSubmit(g_q, 1, &si, P_->fence); r = vkQueuePresentKHR(g_q, &pi); pthread_mutex_unlock(&g_qmu);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) { W.gen = -1; W.why = "out of date at present"; } else if (r == VK_SUBOPTIMAL_KHR && W.gen != -1) win_suboptimal();
    front_window_lock(0);
}
int dm_vk_prewarm(void) {   // the front end, before the game's main: 0 when there is nothing to make
    const char *d = getenv("DM_VK_DIR"); if (!d) return 0;
    const char *e = getenv("DM_VK_PREWARM"); if (e && !atoi(e)) return 0;
    char p[1100]; snprintf(p, sizeof p, "%s/../pipeline_keys.bin", d); FILE *f = fopen(p, "rb"); if (!f) return 0;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint32_t n = sz > 0 ? (uint32_t)(sz / (long)sizeof(pso_rec)) : 0;
    if (!n || !(g_pw_recs = malloc((size_t)n * sizeof *g_pw_recs))) { fclose(f); return 0; }
    n = (uint32_t)fread(g_pw_recs, sizeof *g_pw_recs, n, f); fclose(f); if (!n) return 0;
    snprintf(p, sizeof p, "%s/../vk_depth_mode", d); if ((f = fopen(p, "r"))) { int m; if (fscanf(f, "%d", &m) == 1) { g_prewarm_depth = m != 0; g_depth_zero = g_prewarm_depth; } fclose(f); }
    vk_init(); g_prewarm_ran = 1; g_pw_total = n;
    long rss0 = 0; char ln[128]; FILE *st = fopen("/proc/self/status", "re"); if (st) { while (fgets(ln, sizeof ln, st)) sscanf(ln, "VmRSS: %ld", &rss0); fclose(st); }
    uint64_t t0 = now_ns(); int nw = 4; for (int i = 0; i < nw; i++) { pthread_t t; if (!pthread_create(&t, NULL, pw_worker, NULL)) pthread_detach(t); }
    LOG("vk: shader pre-warm: %u pipelines recorded by earlier runs, %d worker threads%s", n, nw, g_prewarm_depth >= 0 ? (g_prewarm_depth ? ", depth mode near = 0" : ", depth mode GL") : "");
    while (atomic_load(&g_pw_done) < n && !atomic_load(&g_pw_cancel)) { pw_draw(atomic_load(&g_pw_done), n); usleep(50000); }
    long rss1 = 0; if ((st = fopen("/proc/self/status", "re"))) { while (fgets(ln, sizeof ln, st)) sscanf(ln, "VmRSS: %ld", &rss1); fclose(st); }
    LOG("vk: shader pre-warm %s after %.1f s: %u of %u done, %llu pipelines made (%llu compiled, the rest from the driver's cache), %llu already there; %.0f ms loading shaders; RSS %+.0f MB%s",
        atomic_load(&g_pw_cancel) ? "skipped by a tap" : "finished", (now_ns() - t0) / 1e9, atomic_load(&g_pw_done), n, (unsigned long long)atomic_load(&g_pre_made),
        (unsigned long long)atomic_load(&g_pre_slow), (unsigned long long)atomic_load(&g_pre_had), atomic_load(&g_sh_ns) / 1e6, (rss1 - rss0) / 1024.0,
        atomic_load(&g_pw_cancel) ? "; the rest is made in the background" : "");
    return 1;
}
// ADPF (Android's performance hints): a hint session for the thread that records and presents (the game's render thread, which calls this), told each frame's work against a
// 16.7 ms target, and on Android 15 and later the GPU's part too, so the governors raise the CPU and GPU clocks for a game that keeps missing its target. GameTurbo showed the GPU
// 12% busy at 30 fps: the GPU clock stayed low, and our GPU timestamps grew with it. Looked up at run time (API 33 / 35 functions)
#include <dlfcn.h>
#include <sys/syscall.h>
static void adpf_frame(uint64_t cpu_ns, uint64_t gpu_ns) {
    static int state; static void *session; static void *(*mk_work)(void); static void (*w_start)(void *, int64_t), (*w_total)(void *, int64_t), (*w_cpu)(void *, int64_t), (*w_gpu)(void *, int64_t), (*w_free)(void *);
    static int (*report)(void *, int64_t); static int (*report2)(void *, void *);
    if (state < 0) return;
    if (!state) {
        state = -1; const char *e = getenv("DM_ADPF"); if (e && !atoi(e)) return;
        void *h = dlopen("libandroid.so", RTLD_NOW); if (!h) return;
        void *(*getm)(void) = (void *(*)(void))dlsym(h, "APerformanceHint_getManager");
        void *(*mks)(void *, const int32_t *, size_t, int64_t) = (void *(*)(void *, const int32_t *, size_t, int64_t))dlsym(h, "APerformanceHint_createSession");
        report = (int (*)(void *, int64_t))dlsym(h, "APerformanceHint_reportActualWorkDuration");
        report2 = (int (*)(void *, void *))dlsym(h, "APerformanceHint_reportActualWorkDuration2");
        mk_work = (void *(*)(void))dlsym(h, "AWorkDuration_create"); w_free = (void (*)(void *))dlsym(h, "AWorkDuration_release");
        w_start = (void (*)(void *, int64_t))dlsym(h, "AWorkDuration_setWorkPeriodStartTimestampNanos"); w_total = (void (*)(void *, int64_t))dlsym(h, "AWorkDuration_setActualTotalDurationNanos");
        w_cpu = (void (*)(void *, int64_t))dlsym(h, "AWorkDuration_setActualCpuDurationNanos"); w_gpu = (void (*)(void *, int64_t))dlsym(h, "AWorkDuration_setActualGpuDurationNanos");
        if (!getm || !mks || !report) return;
        void *m = getm(); int32_t tid = (int32_t)syscall(SYS_gettid);
        if (!m || !(session = mks(m, &tid, 1, 16666667))) return;
        state = 1; LOG("vk: ADPF hint session for the render thread (tid %d), target 16.7 ms%s", tid, report2 && mk_work && w_gpu ? ", with the GPU's work (Android 15)" : "");
    }
    if (report2 && mk_work && w_start && w_total && w_cpu && w_gpu) {
        void *w = mk_work(); if (!w) return;
        uint64_t now = now_ns(), tot = cpu_ns > gpu_ns ? cpu_ns : gpu_ns;
        w_start(w, (int64_t)(now - tot)); w_total(w, (int64_t)tot); w_cpu(w, (int64_t)cpu_ns); w_gpu(w, (int64_t)gpu_ns); report2(session, w); if (w_free) w_free(w);
    } else report(session, (int64_t)cpu_ns);
}
uint64_t mtl_present(const tex_t *t, double period_s, const int crop[4]) {
    (void)period_s;
    uint64_t n = atomic_fetch_add(&g_presents, 1) + 1;
    g_frame_draw = 0; vk_ctl(n);
    {   // the frame's work for the hint session: the time since the last present (its CPU side) and the GPU time per frame of the last completed submissions
        static uint64_t lastp, gprev; uint64_t now = now_ns(), g = atomic_load(&g_gpu_ns), gd = g >= gprev ? g - gprev : 0; gprev = g;
        if (lastp && now - lastp < 1000000000ull) adpf_frame(now - lastp, gd < 200000000ull ? gd : 0);
        lastp = now; }
    {   // a long frame (over 50 ms from the last present): what it held -- new pipelines, draws left out for theirs, waits for a context and for GPU-written data, page faults
        // (major ones: read back from swap or a file), extra rings, descriptor pools, uploads, GPU time. At most 20 lines every 600 frames
        static uint64_t lp, c_pso, c_pson, c_skip, c_wait, c_up, c_xr, c_dp, c_gww, c_gpu, win; static long c_mj, c_mn; static int nl;
        uint64_t now = now_ns(); struct rusage ru; getrusage(RUSAGE_SELF, &ru);
        uint64_t pso = (uint64_t)g_npso, pson = atomic_load(&g_pso_ns), skip = atomic_load(&g_pso_skipped), wt = atomic_load(&g_wait_cum), up = atomic_load(&g_up_cum),
                 xr = atomic_load(&g_xring_n), dp = (uint64_t)atomic_load(&g_ndp_total), gww = atomic_load(&g_gw_wait_cum), gpu = atomic_load(&g_gpu_cum);
        if (n / 600 != win) { win = n / 600; nl = 0; }
        if (lp && now - lp > 50000000ull && nl < 20) { nl++;
            LOG("vk: long frame %llu: %.1f ms; new pipelines %llu (%.1f ms), draws left out %llu; waiting for a context %.1f ms, for GPU-written data %.1f ms; page faults %ld major, %ld minor; extra rings %llu, descriptor pools %llu; uploads %.1f MB; GPU %.1f ms",
                (unsigned long long)n, (now - lp) / 1e6, (unsigned long long)(pso - c_pso), (pson - c_pson) / 1e6, (unsigned long long)(skip - c_skip), (wt - c_wait) / 1e6, (gww - c_gww) / 1e6,
                ru.ru_majflt - c_mj, ru.ru_minflt - c_mn, (unsigned long long)(xr - c_xr), (unsigned long long)(dp - c_dp), (up - c_up) / 1048576.0, (gpu - c_gpu) / 1e6); }
        lp = now; c_pso = pso; c_pson = pson; c_skip = skip; c_wait = wt; c_up = up; c_xr = xr; c_dp = dp; c_gww = gww; c_gpu = gpu; c_mj = ru.ru_majflt; c_mn = ru.ru_minflt;
    }
    if (n == 2) { pthread_t t; if (!pthread_create(&t, NULL, pso_precreate, NULL)) pthread_detach(t); }   // the pipelines earlier runs made, again, behind the game's first frames   // draws are numbered from 0 in each frame; the live control file (vk_ctl)
    {   // pacing for DM_FPS=30 (every frame two vsyncs) and the frame-rate counter's reading
        static uint64_t last, t0, n0; uint64_t now = now_ns();
        if (mtl_fps_target() == 30 && last && now - last < 33333333ull) { usleep((useconds_t)((33333333ull - (now - last)) / 1000)); now = now_ns(); }
        last = now; if (!t0) { t0 = now; n0 = n; } else if (now - t0 >= 1000000000ull) { g_fps_shown = (double)(n - n0) * 1e9 / (double)(now - t0); t0 = now; n0 = n; }
    }
    if (t && g_has_surface && !g_tl) mtl_finish();   // no timeline semaphore: the frame's work is done before its present reads the window texture
    if (t && g_has_surface) win_present(t, crop);
    if (n == 1 || n % 600 == 0) LOG("vk: %llu frames presented, %llu draws, %llu dropped (no program %llu, shader %llu, no target %llu, indirect %llu, primitive %llu, pipeline %llu, descriptors %llu), %d pipelines, %d shaders",
                                    (unsigned long long)n, (unsigned long long)g_stat_draws, (unsigned long long)g_stat_skip, (unsigned long long)g_skip_why[0], (unsigned long long)g_skip_why[1], (unsigned long long)g_skip_why[2],
                                    (unsigned long long)g_skip_why[3], (unsigned long long)g_skip_why[4], (unsigned long long)g_skip_why[5], (unsigned long long)g_skip_why[6], g_npso, g_nsh);
    if (n % 600 == 0 && getenv("DM_VK_TEXCOUNT")) {   // diagnostics: live textures by kind, and how many share their storage start with an older one
        int ng = 0, nu = 0, nn = 0, dup = 0; pthread_mutex_lock(&g_rmu);
        for (int h = 0; h < NHASH; h++) for (vtex *v = g_tex[h]; v; v = v->next) {
            if (v->gpu) ng++; else if (v->uploaded) nu++; else nn++;
            for (int h2 = 0; h2 < NHASH && v->mem; h2++) for (vtex *o = g_tex[h2]; o; o = o->next) if (o != v && o->mem == v->mem && o->serial < v->serial && o->w == v->w && o->nvn_fmt == v->nvn_fmt) { dup++; h2 = NHASH; break; }
        }
        pthread_mutex_unlock(&g_rmu);
        LOG("vk: live textures: %d rendered to, %d uploaded, %d neither; %d on the storage of an older one of the same size and format", ng, nu, nn, dup);
    }
    if (n % 600 == 0) { uint64_t live = 0, held = 0; int nb = 0; pthread_mutex_lock(&g_blkmu); for (int i = 0; i < g_nblk; i++) if (g_blk[i].m) { nb++; held += g_blk[i].size; live += g_blk[i].live; } pthread_mutex_unlock(&g_blkmu);
                        LOG("vk: image memory: %d blocks of 64 MB (%.0f MB, %llu live images in them), big images %.0f MB now; rings %.0f MB extra", nb, held / 1048576.0, (unsigned long long)live, atomic_load(&g_big_bytes) / 1048576.0, atomic_load(&g_extra_ring_bytes) / 1048576.0); }
    if (n % 600 == 0 && g_fb_copies) LOG("vk: same-pass feedback snapshots so far: %llu", (unsigned long long)g_fb_copies);
    if (n % 600 == 0) LOG("vk: texture slots read as black so far: no handle %llu, handle not registered %llu, texture not created %llu, no view %llu; lookups of a dead texture object %llu (registrations of one %llu)", (unsigned long long)g_tex_miss[0],
                          (unsigned long long)g_tex_miss[1], (unsigned long long)g_tex_miss[2], (unsigned long long)g_tex_miss[3], (unsigned long long)g_tex_dead, (unsigned long long)g_treg_dead);
    if (n % 600 == 0) { extern uint64_t g_nsub_stat; static uint64_t ls;
        LOG("vk: recording per frame over the last 600: draws %.2f ms (descriptor sets %.2f ms, %.0f%% reused), dispatches %.2f ms, texture uploads %.2f ms (%.1f MB), vkQueueSubmit %.2f ms; %.1f submissions a frame;"
            " GPU-written data of a running submission used in place %llu times, copied %llu times, waited for %llu times (%.2f ms a frame; indirect draws %llu, partial ranges %llu, misfits in-submission %llu); copies from GPU-written bytes: into textures %llu, buffers %llu; dispatch write-backs %.1f KB of %.1f KB looked at; reads of an earlier submission's GPU-written range: its bytes as they are %llu, with the CPU's rewrites since patched in %llu; texture copies from guest memory over a dispatch's range %llu; those compares and patches %.2f ms (%.1f MB compared; patches %.2f ms, %.1f a frame, %.0f copy regions a frame); held-pass sampling checks %.2f ms; vertex index bias pushed %.1f draws a frame (%.1f not 0); GPU-written range lookups %.0f a frame, %.0f answered at once, %.0f entries looked at",
            g_t_draw / 600e6, g_t_ws / 600e6, g_dsc_hit + g_dsc_miss ? 100.0 * g_dsc_hit / (g_dsc_hit + g_dsc_miss) : 0.0, g_t_disp / 600e6, g_t_up / 600e6, g_up_bytes / 600e6 * 1e3 / 1e3, g_t_sub / 600e6,
            (g_nsub_stat - ls) / 600.0, (unsigned long long)atomic_exchange(&g_gw_held, 0), (unsigned long long)atomic_exchange(&g_gw_copied, 0), (unsigned long long)atomic_exchange(&g_gw_waits, 0), atomic_exchange(&g_gw_wait_ns, 0) / 600e6,
            (unsigned long long)atomic_exchange(&g_gw_why[0], 0), (unsigned long long)atomic_exchange(&g_gw_why[1], 0), (unsigned long long)atomic_exchange(&g_gw_why[2], 0),
            (unsigned long long)atomic_exchange(&g_gw_b2t, 0), (unsigned long long)atomic_exchange(&g_gw_b2b, 0), g_wb_words * 4 / 1024.0, g_wb_bytes / 1024.0,
            (unsigned long long)atomic_exchange(&g_gw_gpu_reads, 0), (unsigned long long)atomic_exchange(&g_gw_cpu_fix, 0), (unsigned long long)atomic_exchange(&g_gw_b2t_guest, 0),
            g_t_stale / 600e6, g_stale_bytes / 600.0 / 1048576.0, g_t_patch / 600e6, g_npatch / 600.0, g_patch_regions / 600.0, g_t_split / 600e6, g_vid_push / 600.0, g_vid_nz / 600.0, g_gw_calls / 600.0, g_gw_quick / 600.0, g_gw_walked / 600.0);
        g_wb_words = g_wb_bytes = 0; g_t_stale = g_stale_bytes = 0; g_t_patch = g_patch_regions = g_npatch = 0; g_vid_push = g_vid_nz = 0; g_t_split = 0; g_gw_calls = g_gw_quick = g_gw_walked = 0;
        ls = g_nsub_stat; g_t_draw = g_t_ws = g_t_disp = g_t_up = g_t_sub = 0; g_up_bytes = 0; g_dsc_hit = g_dsc_miss = 0; }
    if (n % 600 == 0) {   // memory: the process (RSS, its anonymous part, PSS) and what the backend holds
        long rss = 0, anon = 0, pss = 0; char ln[256]; FILE *f = fopen("/proc/self/status", "re");
        if (f) { while (fgets(ln, sizeof ln, f)) { sscanf(ln, "VmRSS: %ld", &rss); sscanf(ln, "RssAnon: %ld", &anon); } fclose(f); }
        if ((f = fopen("/proc/self/smaps_rollup", "re"))) { while (fgets(ln, sizeof ln, f)) if (sscanf(ln, "Pss: %ld", &pss) == 1) break; fclose(f); }
        int nb = 0; pthread_mutex_lock(&g_blkmu); for (int i = 0; i < g_nblk; i++) if (g_blk[i].m) nb++; pthread_mutex_unlock(&g_blkmu);
        long cached = 0, avail = 0; if ((f = fopen("/proc/meminfo", "re"))) { while (fgets(ln, sizeof ln, f)) { sscanf(ln, "Cached: %ld", &cached); sscanf(ln, "MemAvailable: %ld", &avail); } fclose(f); }
        extern _Atomic uint64_t g_fs_dropped;
        LOG("vk: memory: process RSS %.0f MB (anonymous %.0f MB), PSS %.0f MB; Vulkan rings %.0f MB + extra %.0f MB, image blocks %d x 64 MB, big images %.0f MB, %d descriptor pools;"
            " system: available %.0f MB, page cache %.0f MB; game file reads dropped from the page cache so far %.0f MB; textures evicted %llu, made again %llu; drawn before their data came and filled when it did %llu of %llu; GPU copies written into storage too: buffer %llu, texture %llu",
            rss / 1024.0, anon / 1024.0, pss / 1024.0, atomic_load(&g_ring0_bytes) / 1048576.0, atomic_load(&g_extra_ring_bytes) / 1048576.0, nb, atomic_load(&g_big_bytes) / 1048576.0, atomic_load(&g_ndp_total),
            avail / 1024.0, cached / 1024.0, atomic_load(&g_fs_dropped) / 1048576.0, (unsigned long long)g_evicted, (unsigned long long)g_remade, (unsigned long long)g_late_fill, (unsigned long long)g_late_watch, (unsigned long long)g_mir_b2t, (unsigned long long)g_mir_t2t);
    }
    if (n % 600 == 0) LOG("vk: occlusion queries so far: %llu samples-passed reports, %llu resets, %llu draws counted", (unsigned long long)g_occ_nrep, (unsigned long long)g_occ_nrst, (unsigned long long)g_occ_ndraw);
    if (n % 600 == 0) LOG("vk: texel buffer slots read as zeros so far: nothing bound %llu, not a buffer texture %llu, format not usable %llu", (unsigned long long)g_tbuf_miss[0],
                          (unsigned long long)g_tbuf_miss[1], (unsigned long long)g_tbuf_miss[2]);
    if (n % 600 == 0) { uint64_t gp = atomic_exchange(&g_gpu_kind_ns[0], 0), gd = atomic_exchange(&g_gpu_kind_ns[1], 0), gc = atomic_exchange(&g_gpu_kind_ns[2], 0);
                        LOG("vk: GPU per frame over the last 600: render passes %.2f ms, dispatches %.2f ms, copies %.2f ms (the rest of the submissions' span is time between them); %.1f render passes a frame, SetRenderTargets detours held %.1f and same targets kept %.1f a frame, held passes ended for a draw that samples an attachment %.1f a frame (left open %.1f: the attachment it samples was not written in the pass)",
                            gp / 600e6, gd / 600e6, gc / 600e6, g_npass / 600.0, g_nheld / 600.0, g_nsame / 600.0, g_nsplit / 600.0, g_nsplit_skip / 600.0); g_npass = 0; g_nheld = 0; g_nsame = 0; g_nsplit = 0; g_nsplit_skip = 0; }
    if (n % 600 == 0) gsig_report(600);
    if (n % 600 == 0 && (ubwc_on() || pass_barrier_on())) { LOG("vk: DM_VK_UBWC %d: %llu images made with a format list or not mutable so far, %llu views outside the list; DM_VK_PASS_BARRIER %d: %.1f pass ends a frame without vertex shaders, %.1f vertex-shader barriers a frame",
        ubwc_on(), (unsigned long long)g_ubwc_imgs, (unsigned long long)g_ubwc_fallback, pass_barrier_on(), g_nbar_narrow / 600.0, g_nbar_vs / 600.0); g_nbar_narrow = g_nbar_vs = 0; }
    if (n % 600 == 0) { LOG("vk: waiting for a free submission context per frame: %.2f ms for its own submission, %.2f ms for a later one still reading its ring (%d contexts)",
                        atomic_exchange(&g_wait_fence_ns, 0) / 600e6, atomic_exchange(&g_wait_hold_ns, 0) / 600e6, NCTX); }
    if (n % 600 == 0) { LOG("vk: per frame over the last 600: GPU %.2f ms, recording %.2f ms, waiting for the GPU %.2f ms, present waiting %.2f ms", atomic_exchange(&g_gpu_ns, 0) / 600e6,
                            atomic_exchange(&g_rec_ns, 0) / 600e6, atomic_exchange(&g_wait_ns, 0) / 600e6, atomic_exchange(&g_pwait_ns, 0) / 600e6); }
    if (t) shot(t, n);
    // On demand: DM_VK_DUMP_FILE=<path>; when that file appears (checked every 30 frames) the next frame is pass-logged and its render targets written, and the file is removed
    // (a capture where the frame number cannot be known in advance: wherever the player happens to be)
    static uint64_t trig; static const char *tf = (const char *)1; if (tf == (const char *)1) tf = getenv("DM_VK_DUMP_FILE");
    if (trig && n == trig) { rt_dump(n, 1); trig = 0; } else rt_dump(n, 0);
    if (tf && !trig && n % 30 == 0 && !access(tf, F_OK)) { trig = n + 1; unlink(tf); LOG("vk: %s found: frame %llu is logged and its render targets written", tf, (unsigned long long)trig); }
    if (n % 1800 == 0 || (g_pso_last_new && n - g_pso_last_new == 120 && !atomic_load(&g_pso_pending))) pcache_save();   // also soon after new pipelines settle
    if (n % 600 == 0) LOG("vk: pipelines: %d made, %llu on worker threads, %llu made again from earlier runs, %llu draws left out while theirs was made, %.0f ms making them, %.0f ms loading shaders, in all; warm ones made again at first use %llu (%.0f ms)", g_npso,
                          (unsigned long long)atomic_load(&g_pso_async), (unsigned long long)atomic_load(&g_pre_made), (unsigned long long)atomic_load(&g_pso_skipped), atomic_load(&g_pso_ns) / 1e6, atomic_load(&g_sh_ns) / 1e6,
                          (unsigned long long)atomic_load(&g_pso_rewarm), atomic_load(&g_pso_rewarm_ns) / 1e6);
    { static int64_t pl = -2; static int on; if (pl == -2) { const char *e = getenv("DM_VK_PASSLOG"); pl = e ? atoll(e) : -1; on = e && strchr(e, '+'); }   // <frame>+: every frame from there on
      g_plog = (on ? (int64_t)n + 1 >= pl && pl >= 0 : (int64_t)n + 1 == pl) || (trig && n + 1 == trig); if (g_plog && on) LOG("plog: frame %llu", (unsigned long long)n + 1); }
    return n;
}
void mtl_present_wait(uint64_t ticket) { (void)ticket; }
double mtl_presented_time(uint64_t n) { (void)n; return 0; }
int mtl_has_window(void) { return 0; }
int mtl_display_hz(void) { return 0; }
int mtl_fps_target(void) { static int f = -1; if (f < 0) { const char *e = getenv("DM_FPS"); f = e && atoi(e) == 30 ? 30 : 60; } return f; }
int mtl_link_active(void) { return 0; }
int mtl_link_wait_tick(int vsyncs, uint64_t timeout_ns) { (void)vsyncs; (void)timeout_ns; return 0; }
uint32_t mtl_present_qos(void) { return 0; }
void mtl_ub_present(double iv_ms) { (void)iv_ms; }
void mtl_set_depth_zero(int on) {   // vertex shaders loaded from here on follow it (spv_gl_depth); the games set it once at device init, before their first shader
    if (on != g_depth_zero && g_nsh && g_prewarm_depth < 0) LOG("[RED] vk: depth mode changed to %s after %d shaders were loaded: those keep the other convention", on ? "near = 0" : "GL (near = -w)", g_nsh);
    if (g_prewarm_depth >= 0 && on != g_prewarm_depth && g_nsh) { static int n; if (on && !n++) LOG("[RED] vk: the game's depth mode (near = 0) differs from the one the shader pre-warm used"); }
    g_depth_zero = on;
    // kept for the next run's pre-warm (dm_vk_prewarm loads shaders before the game sets it): the last mode the game set
    const char *d = getenv("DM_VK_DIR"); if (d) { char p[1100]; snprintf(p, sizeof p, "%s/../vk_depth_mode", d); FILE *f = fopen(p, "w"); if (f) { fprintf(f, "%d\n", on); fclose(f); } }
}
void mtl_set_origin_lower_left(int on) { g_origin_ll = on; }
void mtl_set_frame(uint64_t f) { (void)f; }
void mtl_xv_window_tex(uint64_t serial) { (void)serial; }
void mtl_read_texels(const tex_t *t, unsigned level, unsigned layer0, const NVNcopyRegion *r, uint8_t *dst, size_t row, size_t img) {
    (void)t; (void)level; (void)layer0; (void)row; RED_ONCE(80, "CPU texture reads return zeros");
    memset(dst, 0, img * (size_t)(r->depth > 0 ? r->depth : 1));
}

// Memory pools: nothing to do (draws copy what they read); mtl_addr answers for the NVN layer whether an address is in one
#define NPOOL 4096
static struct { const uint8_t *mem; uint64_t size; } g_pool[NPOOL];
static int g_npool;
static pthread_mutex_t g_pool_mu = PTHREAD_MUTEX_INITIALIZER;
void mtl_pool_add(const void *mem, uint64_t size) { pthread_mutex_lock(&g_pool_mu); if (g_npool < NPOOL) { g_pool[g_npool].mem = mem; g_pool[g_npool].size = size; g_npool++; } pthread_mutex_unlock(&g_pool_mu); }
static uint64_t pool_left(uint64_t addr, uint64_t cap) {
    uint64_t n = 0; pthread_mutex_lock(&g_pool_mu);
    for (int i = 0; i < g_npool; i++) { uint64_t b = (uint64_t)(uintptr_t)g_pool[i].mem; if (addr >= b && addr < b + g_pool[i].size) { n = b + g_pool[i].size - addr; break; } }
    pthread_mutex_unlock(&g_pool_mu); return n < cap ? n : cap;
}
void mtl_pool_del(const void *mem) { pthread_mutex_lock(&g_pool_mu); for (int i = 0; i < g_npool; i++) if (g_pool[i].mem == mem) { g_pool[i] = g_pool[--g_npool]; break; } pthread_mutex_unlock(&g_pool_mu); }
int mtl_addr(uint64_t addr, uint64_t len, void **buf, uint64_t *off) {
    int hit = 0; pthread_mutex_lock(&g_pool_mu);
    for (int i = 0; i < g_npool && !hit; i++) { uint64_t b = (uint64_t)(uintptr_t)g_pool[i].mem; if (addr >= b && addr + len <= b + g_pool[i].size) { if (buf) *buf = (void *)g_pool[i].mem; if (off) *off = addr - b; hit = 1; } }
    pthread_mutex_unlock(&g_pool_mu); return hit;
}
void mtl_alias_drop_pool(const void *pool) { (void)pool; }

// statistics the NVN layer prints
void mtl_stats(uint64_t *passes, uint64_t *draws) { *passes = 0; *draws = g_stat_draws; }
void mtl_vbuf_stats(uint64_t *resolve, uint64_t *reuse, uint64_t *set, uint64_t *skip, uint64_t *copies, uint64_t *copy_bytes) { *resolve = *reuse = *set = *skip = *copies = *copy_bytes = 0; }
void mtl_use_stats(uint64_t *call, uint64_t *skip) { *call = *skip = 0; }
uint64_t mtl_upload_ns_take(void) { return 0; }
uint64_t mtl_upload_gpu_ns_take(void) { return 0; }
uint64_t mtl_upload_bytes(void) { return 0; }
uint64_t mtl_udc_saved_bytes(void) { return 0; }
void mtl_ubp_why(uint64_t w[5]) { memset(w, 0, 5 * sizeof *w); }
void mtl_ubp_stats(uint64_t *a, uint64_t *b, uint64_t *c) { *a = *b = *c = 0; }
void mtl_ubd_stats(uint64_t *a, uint64_t *b) { *a = *b = 0; }
void mtl_res_stats(char *out, size_t n) { if (n) snprintf(out, n, "vulkan"); }
int mtl_pd_on(void) { return 0; }   // the Metal host's pass dump (queue events in it, hle_nvn.c): none here
void mtl_pd_note(const char *what, uint64_t v) { (void)what; (void)v; }
void mtl_pkg_stats(uint64_t *uploads, uint64_t *reuploads) { *uploads = g_pkg_up; *reuploads = g_pkg_reup; }
uint64_t mtl_pkg_miss(void) { return 0; }
void mtl_mem_stat(unsigned long long a, unsigned long long b) { (void)a; (void)b; }
void mtl_lc_stats(uint64_t *a, uint64_t *b) { *a = *b = 0; }
int mtl_gpu_span(uint64_t f, uint64_t *t0, uint64_t *t1) { (void)f; (void)t0; (void)t1; return 0; }
double mtl_gpu_ms(uint64_t f) { (void)f; return 0; }
uint64_t mtl_ds_omit(void) { return 0; }
uint64_t mtl_drawable_ns_take(void) { return 0; }
uint64_t mtl_cut_count(void) { return 0; }
uint64_t mtl_blit_splits(void) { return 0; }
void mtl_create_stats(uint64_t *lib, uint64_t *pso) { *lib = 0; *pso = (uint64_t)g_npso; }
void mtl_create_ns(uint64_t *lib_ns, uint64_t *pso_ns) { *lib_ns = atomic_load(&g_sh_ns); *pso_ns = atomic_load(&g_pso_ns); }   // shader loads, pipeline creation (all threads)
void mtl_bw(uint64_t *load, uint64_t *store, uint64_t *full) { *load = *store = *full = 0; }
void mtl_barrier_stats(uint64_t *skip, uint64_t *full) { *skip = *full = 0; }
void mtl_args_stats(uint64_t *enc, uint64_t *reuse) { *enc = *reuse = 0; }
void mtl_alloc_stats(uint64_t *w, uint64_t *g, int *n) { *w = *g = 0; *n = 0; }
// Entry points the NVN layer gained after this backend was written (nvn_mtl.h / hle_nvn.c), answered the Vulkan way: images register into the texture table as before
// (RegisterImage), a queue's captured window origin is the one the replay follows from its submission on, the rest are Metal-only features (present snapshots, the display link,
// the pipeline archive, frame captures) that are off here, and statistics of work this backend does not do
void mtl_reg_image(int id, const tex_t *t, const view_t *v) { mtl_reg_tex(id, t, v); }
void mtl_set_queue_origin_lower_left(int on) { g_origin_ll = on; }
int mtl_wait_acq(uint64_t fence, uint64_t timeout_ns) { return mtl_wait(fence, timeout_ns); }
void mtl_queue_activate(uint64_t serial) { (void)serial; }
void mtl_queue_forget(uint64_t serial) { (void)serial; }
void mtl_prepare(void) {}
int mtl_snap_on(void) { return 0; }
uint64_t mtl_snap_acquire_wait(void) { return 0; }
int mtl_link_wait_ticks(int ticks, uint64_t timeout_ns) { (void)ticks; (void)timeout_ns; return 0; }
int mtl_link_tick(uint64_t n, double *target, double *lead_ms, int *ticks, double *gap_ms) { (void)n; (void)target; (void)lead_ms; (void)ticks; (void)gap_ms; return 0; }
int mtl_link_stalled(void) { return 0; }
int mtl_link_rate(void) { return 60; }
int mtl_pso_prebuild_tool(void) { return 0; }
void mtl_pso_line(char *out, size_t n) { if (n) snprintf(out, n, "vulkan: %d made", g_npso); }
void nvn_cap_submit(int n, const uint64_t *h) { (void)n; (void)h; }
void nvn_cap_present(const tex_t *t, const int crop[4]) { (void)t; (void)crop; }
void dm_gfx_shot_at(long k, const char *path) { (void)k; (void)path; LOG("[RED] vk: DM_CTL shot is Metal-only (DM_VK_SHOTS writes frames here)"); }
void dm_gfx_texraw(uint64_t serial, const char *path) { (void)serial; (void)path; }
void dm_gfx_texlist(int target) { (void)target; }
void dm_gfx_texdump_next(const char *path) { (void)path; }
void dm_gfx_passdump_next(long n) { (void)n; }
void mtl_ab_gpu(uint64_t f, double gpu_ms, double frame_ms) { (void)f; (void)gpu_ms; (void)frame_ms; }
unsigned mtl_cpu_tex_count(void) { return (unsigned)g_ncpu; }
uint64_t mtl_pkg_edge_skips(void) { return 0; }
uint64_t mtl_copy_cut_count(void) { return 0; }
void mtl_sm_stats(uint64_t *a, uint64_t *b, uint64_t *c) { *a = *b = *c = 0; }
void mtl_hybrid_stats(uint64_t *draws, uint64_t *made, uint64_t *none) { *draws = *made = *none = 0; }
void mtl_geomesh_stats(uint64_t *a, uint64_t *b) { *a = *b = 0; }
void mtl_geohoist_stats(uint64_t *a, uint64_t *b, uint64_t *c) { *a = *b = 0; memset(c, 0, 6 * sizeof *c); }
void mtl_cshoist_stats(uint64_t *a, uint64_t *b, uint64_t *c) { *a = *b = 0; memset(c, 0, 8 * sizeof *c); }

// ---------- Hibernation (dm/host/hle_hibsnap.c): this backend's lasting state in a snapshot ----------
// The images, views, samplers and pipelines are caches filled from guest memory as draws need them. What the game set up through NVN calls is kept: the memory pools, the texture
// and sampler descriptor tables, the textures the CPU writes, the depth and origin modes, and the fence counter (sync objects in guest memory hold fence values: every one up to the
// snapshot's is complete in the restored process). GPU-only content: before the snapshot, every registered texture the GPU filled (copies, render passes) is read back into its
// storage in guest memory and listed as written by the CPU, so the restored process uploads it from there (mtl_hib_readback, as nvn_mtl.m does; FINAL FANTASY XII THE ZODIAC AGE fills
// its UI atlases by GPU copies). Single-sampled colour textures only, and none the host decodes for want of the format (their image is not the storage's format)
typedef struct { uint32_t npool, ntreg, nsreg, ncpu; int32_t origin_ll, depth_zero; uint64_t fence; } hib_vk;
typedef struct { uint64_t mem, size; } hib_vpool;
typedef struct { int32_t idx, has_v; tex_t t; view_t v; } hib_vtreg;
typedef struct { int32_t idx, pad; samp_par p; } hib_vsreg;
static uint64_t *g_hib_rb; static uint32_t g_hib_nrb;
void mtl_hib_readback(void) {   // inside a submission (hle_nvn.c dm_hib_nvn_prepare), every guest thread parked and the GPU drained
    free(g_hib_rb); g_hib_rb = NULL; g_hib_nrb = 0;
    pthread_mutex_lock(&g_rmu);
    tex_t *ts = malloc(((size_t)g_ntreg + 1) * sizeof *ts); int nt = 0;
    for (int i = 0; i < g_ntreg; i++) if (g_treg[i].ok && g_treg[i].t.serial) ts[nt++] = g_treg[i].t;
    pthread_mutex_unlock(&g_rmu);
    typedef struct { tex_t t; unsigned lv, ly; uint32_t w, h, dz; size_t row, img; VkDeviceSize off; } rb_t;   // dz: a 3D level's slices (one copy of the whole level)
    rb_t *rb = malloc(((size_t)nt * 16 + 1) * sizeof *rb); int nrb = 0; VkDeviceSize total = 0;
    g_hib_rb = malloc(((size_t)nt + 1) * sizeof *g_hib_rb);
    for (int i = 0; i < nt; i++) {
        const tex_t *t = &ts[i]; int dup = 0;
        for (uint32_t k = 0; k < g_hib_nrb && !dup; k++) dup = g_hib_rb[k] == t->serial;
        if (dup || !t->mem || !t->pool || (t->pool->flags & NVN_MEMORY_POOL_FLAGS_VIRTUAL) || t->d.samples > 1 || t->d.target == NVN_TEXTURE_TARGET_BUFFER
            || (t->d.flags & (NVN_TEXTURE_FLAGS_LINEAR | NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET)) || t->has_layout) continue;
        pthread_mutex_lock(&g_rmu); vtex *v = tex_find(t->serial); int cpu = cpu_written(t->serial); pthread_mutex_unlock(&g_rmu);
        if (!v || v->dead || !v->img || v->evicted || cpu ||   // released: kept findable until freed (mtl_tex_release), its storage may be another's now
            v->f.dec || v->f.depth || !(v->aspect & VK_IMAGE_ASPECT_COLOR_BIT) || (v->uploaded && !v->gpu && !v->copied && !(v->gpu_lv & ~v->mir_lv))) continue;   // copied levels whose bytes reached the storage too (mir_lv) need nothing   // not made, uploaded from its storage, or not its storage's format
        int bw = 1, bh = 1, bpb = nvn_fmt_bpb(t->d.format, &bw, &bh); if (!bpb) continue;
        for (unsigned lv = 0; lv < v->levels && lv < 16; lv++) {
            uint32_t w = v->w >> lv ? v->w >> lv : 1, h = v->h >> lv ? v->h >> lv : 1, dz = t->d.target == NVN_TEXTURE_TARGET_3D ? (v->d >> lv ? v->d >> lv : 1) : 1;
            size_t row = (size_t)((w + bw - 1) / bw) * bpb, img = row * ((h + bh - 1) / bh);
            for (unsigned ly = 0; ly < v->layers; ly++) {
                rb = realloc(rb, ((size_t)nrb + 1) * sizeof *rb);
                rb[nrb++] = (rb_t){*t, lv, ly, w, h, dz, row, img, total}; total += (img * dz + 15) & ~(VkDeviceSize)15;
            }
        }
        g_hib_rb[g_hib_nrb++] = t->serial;
    }
    if (nrb) {
        VkDeviceMemory m; void *map = NULL; VkBuffer b = buf_new(total, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &m, &map);
        VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT};
        pass_end();
        vkCmdPipelineBarrier(C->cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
        for (int i = 0; i < nrb; i++) {
            pthread_mutex_lock(&g_rmu); vtex *v = tex_find(rb[i].t.serial); pthread_mutex_unlock(&g_rmu);
            VkBufferImageCopy c = {rb[i].off, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, rb[i].lv, rb[i].ly, 1}, {0, 0, 0}, {rb[i].w, rb[i].h, rb[i].dz}};
            vkCmdCopyImageToBuffer(C->cb, v->img, VK_IMAGE_LAYOUT_GENERAL, b, 1, &c);
        }
        C->used = 1;
        uint64_t f = mtl_sub_commit(); mtl_wait(f, UINT64_MAX);   // the copies done: their bytes into the textures' storage
        for (int i = 0; i < nrb; i++) {
            NVNcopyRegion r = {0, 0, 0, (int)rb[i].w, (int)rb[i].h, (int)rb[i].dz};
            if (nvn_tex_mirror_b2t(&rb[i].t, rb[i].lv, rb[i].ly, &r, (const uint8_t *)map + rb[i].off, rb[i].row, rb[i].img)) { static int n; if (n++ < 4) LOG("[RED] hibernate: texture ser=%llu level %u not written into its storage", (unsigned long long)rb[i].t.serial, rb[i].lv); }
        }
        vkDestroyBuffer(g_dev, b, NULL); vkFreeMemory(g_dev, m, NULL);
        mtl_sub_begin();   // the caller commits a submission of its own
    }
    LOG("hibernate: %u GPU-filled textures read back into their storage (%.1f MB)", g_hib_nrb, total / 1048576.0);
    free(rb); free(ts);
}
void nvn_dirty_add_range(const void *mem, size_t size);
size_t mtl_hib_save(uint8_t **out) {
    hib_vk h = {0};
    pthread_mutex_lock(&g_pool_mu); h.npool = (uint32_t)g_npool; pthread_mutex_unlock(&g_pool_mu);
    pthread_mutex_lock(&g_rmu); for (int i = 0; i < g_ntreg; i++) h.ntreg += g_treg[i].ok != 0; for (int i = 0; i < g_nsreg; i++) h.nsreg += g_sreg_ok[i] != 0; h.ncpu = (uint32_t)g_ncpu + g_hib_nrb; pthread_mutex_unlock(&g_rmu);
    h.origin_ll = g_origin_ll; h.depth_zero = g_depth_zero; pthread_mutex_lock(&g_mu); h.fence = g_fence; pthread_mutex_unlock(&g_mu);
    size_t n = sizeof h + h.npool * sizeof(hib_vpool) + h.ntreg * sizeof(hib_vtreg) + h.nsreg * sizeof(hib_vsreg) + h.ncpu * 8;
    uint8_t *b = calloc(1, n), *p = b; memcpy(p, &h, sizeof h); p += sizeof h;
    pthread_mutex_lock(&g_pool_mu); for (uint32_t i = 0; i < h.npool; i++) { hib_vpool e = {(uint64_t)(uintptr_t)g_pool[i].mem, g_pool[i].size}; memcpy(p, &e, sizeof e); p += sizeof e; } pthread_mutex_unlock(&g_pool_mu);
    pthread_mutex_lock(&g_rmu);
    for (int i = 0; i < g_ntreg; i++) if (g_treg[i].ok) { hib_vtreg e = {i, g_treg[i].has_v, g_treg[i].t, g_treg[i].v}; memcpy(p, &e, sizeof e); p += sizeof e; }
    for (int i = 0; i < g_nsreg; i++) if (g_sreg_ok[i]) { hib_vsreg e = {i, 0, g_sreg[i]}; memcpy(p, &e, sizeof e); p += sizeof e; }
    memcpy(p, g_cpu_written, (size_t)g_ncpu * 8); p += (size_t)g_ncpu * 8;
    pthread_mutex_unlock(&g_rmu);
    memcpy(p, g_hib_rb, (size_t)g_hib_nrb * 8); free(g_hib_rb); g_hib_rb = NULL; g_hib_nrb = 0;   // read back for the snapshot: uploaded from their storage when restored
    *out = b; return n;
}
int mtl_hib_restore(const uint8_t *b, size_t n) {
    hib_vk h; if (n < sizeof h) return -1; memcpy(&h, b, sizeof h);
    if (n != sizeof h + h.npool * sizeof(hib_vpool) + h.ntreg * sizeof(hib_vtreg) + h.nsreg * sizeof(hib_vsreg) + h.ncpu * 8) return -1;
    const uint8_t *p = b + sizeof h;
    for (uint32_t i = 0; i < h.npool; i++) { hib_vpool e; memcpy(&e, p, sizeof e); p += sizeof e; mtl_pool_add((const void *)(uintptr_t)e.mem, e.size); nvn_dirty_add_range((const void *)(uintptr_t)e.mem, e.size); }
    for (uint32_t i = 0; i < h.ntreg; i++) { hib_vtreg e; memcpy(&e, p, sizeof e); p += sizeof e; mtl_reg_tex(e.idx, &e.t, e.has_v ? &e.v : NULL); }
    for (uint32_t i = 0; i < h.nsreg; i++) { hib_vsreg e; memcpy(&e, p, sizeof e); p += sizeof e; mtl_reg_samp(e.idx, &e.p); }
    pthread_mutex_lock(&g_rmu);
    for (uint32_t i = 0; i < h.ncpu; i++) { uint64_t s; memcpy(&s, p, 8); p += 8;
        if (!cpu_written(s)) { if (g_ncpu == g_ccpu) { g_ccpu = g_ccpu ? 2 * g_ccpu : 64; g_cpu_written = realloc(g_cpu_written, g_ccpu * sizeof *g_cpu_written); } g_cpu_written[g_ncpu++] = s; } }
    pthread_mutex_unlock(&g_rmu);
    g_origin_ll = h.origin_ll; g_depth_zero = h.depth_zero;
    pthread_mutex_lock(&g_mu); g_fence = g_done = h.fence; pthread_cond_broadcast(&g_cv); pthread_mutex_unlock(&g_mu);   // every fence the snapshot's syncs hold is complete
    LOG("hibernate: Vulkan replay state restored: %u pools, %u texture and %u sampler descriptors, %u CPU-written textures, fence %llu", h.npool, h.ntreg, h.nsreg, h.ncpu, (unsigned long long)h.fence);
    return 0;
}
