// Null graphics backend for Android (the nvn_mtl.h interface the NVN layer calls; on Apple it is nvn_mtl.m): no GPU, nothing is drawn. A submission's command list is walked for the
// effects the game can see on the CPU -- event values, counter reports, buffer copies, fills and inline updates (GPU address = CPU address) -- which are applied at once, so every
// fence is complete when it is committed. Presents only count. This is the bring-up stage before the Vulkan backend: the game runs, its logic and audio included, without a picture
#include "hle.h"
#include "nvn_obj.h"
#include "nvn_mtl.h"
#include <stdatomic.h>

static uint64_t g_fence;
static _Atomic uint64_t g_presents;
static uint64_t gpu_ticks(void) { uint64_t g = guest_ns(); if (!g) g = now_ns(); return g * 12 / 625; }   // the GPU's 19.2 MHz timer, as nvn_mtl.m reports it

// memory pools: the address ranges the game gave the GPU (mtl_addr answers whether an address lies in one)
#define NPOOL 4096
static struct { const uint8_t *mem; uint64_t size; } g_pool[NPOOL];
static int g_npool;
static pthread_mutex_t g_pool_mu = PTHREAD_MUTEX_INITIALIZER;
void mtl_pool_add(const void *mem, uint64_t size) {
    pthread_mutex_lock(&g_pool_mu);
    if (g_npool < NPOOL) { g_pool[g_npool].mem = mem; g_pool[g_npool].size = size; g_npool++; } else LOG("[RED] null graphics: more than %d memory pools", NPOOL);
    pthread_mutex_unlock(&g_pool_mu);
}
void mtl_pool_del(const void *mem) {
    pthread_mutex_lock(&g_pool_mu);
    for (int i = 0; i < g_npool; i++) if (g_pool[i].mem == mem) { g_pool[i] = g_pool[--g_npool]; break; }
    pthread_mutex_unlock(&g_pool_mu);
}
int mtl_addr(uint64_t addr, uint64_t len, void **buf, uint64_t *off) {
    int hit = 0;
    pthread_mutex_lock(&g_pool_mu);
    for (int i = 0; i < g_npool && !hit; i++) {
        uint64_t b = (uint64_t)(uintptr_t)g_pool[i].mem;
        if (addr >= b && addr + len <= b + g_pool[i].size) { if (buf) *buf = (void *)g_pool[i].mem; if (off) *off = addr - b; hit = 1; }
    }
    pthread_mutex_unlock(&g_pool_mu);
    return hit;
}
void mtl_alias_drop_pool(const void *pool) { (void)pool; }

// submission: the CPU-visible operations of the command list, applied as they come
void mtl_sub_begin(void) {}
void mtl_sub_index(const uint64_t *h, int n) { (void)h; (void)n; }
void mtl_exec_rest(const uint64_t *handles, int n) { (void)handles; (void)n; }
void mtl_sub_copy(uint64_t src, uint64_t dst, uint64_t n) { memmove((void *)dst, (const void *)src, n); }
void mtl_sub_event(volatile uint32_t *val, uint32_t mode, uint32_t v) { if (mode == 1) *val += v; else *val = v; }
void mtl_sub_counter(uint64_t addr, uint32_t type) { uint64_t t = gpu_ticks(), rec[2] = {type == 0 || type == 0x10 ? t : 0, t}; memcpy((void *)addr, rec, sizeof rec); }
void mtl_exec(const uint8_t *p) {
    for (;;) {
        op_hdr h; memcpy(&h, p, sizeof h);
        const uint8_t *a = p + 8;
        switch (h.op) {
            case OP_END: return;
            case OP_JUMP: { const uint8_t *t; memcpy(&t, a, 8); p = t; continue; }
            case OP_CALL: { const uint8_t *t; memcpy(&t, a, 8); mtl_exec(t); break; }
            case OP_SIGNAL_EVENT: { volatile uint32_t *val; uint32_t m, v; memcpy(&val, a, 8); memcpy(&m, a + 8, 4); memcpy(&v, a + 12, 4); mtl_sub_event(val, m, v); break; }
            case OP_REPORT_COUNTER: { uint64_t t, ad; memcpy(&t, a, 8); memcpy(&ad, a + 8, 8); mtl_sub_counter(ad, (uint32_t)t); break; }
            case OP_FENCE_SYNC: { sync_t *s; memcpy(&s, a, 8); s->fence = g_fence + 1; s->fenced = 1; break; }
            case OP_CLEAR_BUF: { const p_clear_buf *cb = (const p_clear_buf *)a; for (uint64_t o = 0; o + 4 <= cb->size; o += 4) memcpy((uint8_t *)cb->addr + o, &cb->value, 4); break; }
            case OP_COPY_B2B: { uint64_t s, d, n; memcpy(&s, a, 8); memcpy(&d, a + 8, 8); memcpy(&n, a + 16, 8); mtl_sub_copy(s, d, n); break; }
            case OP_UPDATE_UB: { uint64_t d, n; memcpy(&d, a, 8); memcpy(&n, a + 8, 8); memcpy((void *)d, a + 16, n); break; }
            default: break;   // drawing state, draws, dispatches, texture copies: nothing to show
        }
        p += h.size;
    }
}
uint64_t mtl_sub_commit(void) { return ++g_fence; }
uint64_t mtl_last_fence(void) { return g_fence; }
int mtl_wait(uint64_t fence, uint64_t timeout_ns) { (void)fence; (void)timeout_ns; return 1; }
void mtl_finish(void) {}
void mtl_drain(void) {}

// textures, samplers, programs: no GPU objects
void mtl_tex_init(const tex_t *t) { (void)t; }
void mtl_tex_release(tex_t *t) { (void)t; }
void mtl_tex_cpu_written(const tex_t *t) { (void)t; }
void mtl_reg_tex(int id, const tex_t *t, const view_t *v) { (void)id; (void)t; (void)v; }
void mtl_reg_samp(int id, const samp_par *p) { (void)id; (void)p; }
void mtl_set_tex_pool_n(int n) { (void)n; }
void mtl_prog_release(prog_t *p) { (void)p; }
void mtl_read_texels(const tex_t *t, unsigned level, unsigned layer0, const NVNcopyRegion *r, uint8_t *dst, size_t row, size_t img) {
    (void)t; (void)level; (void)layer0; (void)row;
    memset(dst, 0, img * (size_t)(r->depth > 0 ? r->depth : 1));
}
void mtl_set_depth_zero(int on) { (void)on; }
void mtl_set_origin_lower_left(int on) { (void)on; }
void mtl_set_frame(uint64_t f) { (void)f; }
void mtl_xv_window_tex(uint64_t serial) { (void)serial; }

// presentation: counted; the NVN layer paces presents with its own 60 Hz clock when there is no display link
uint64_t mtl_present(const tex_t *t, double period_s, const int crop[4]) {
    (void)t; (void)period_s; (void)crop;
    uint64_t n = atomic_fetch_add(&g_presents, 1) + 1;
    if (n == 1 || n % 600 == 0) LOG("null graphics: %llu frames presented (nothing is shown)", (unsigned long long)n);
    return n;
}
void mtl_present_wait(uint64_t ticket) { (void)ticket; }
double mtl_presented_time(uint64_t n) { (void)n; return 0; }
int mtl_has_window(void) { return 0; }
int mtl_display_hz(void) { return 0; }
int mtl_fps_target(void) { return 60; }
int mtl_link_active(void) { return 0; }
int mtl_link_wait_tick(int vsyncs, uint64_t timeout_ns) { (void)vsyncs; (void)timeout_ns; return 0; }
uint32_t mtl_present_qos(void) { return 0; }
void mtl_ub_present(double iv_ms) { (void)iv_ms; }

// statistics the NVN layer prints: all zero
void mtl_stats(uint64_t *passes, uint64_t *draws) { *passes = *draws = 0; }
void mtl_vbuf_stats(uint64_t *resolve, uint64_t *reuse, uint64_t *set, uint64_t *skip) { *resolve = *reuse = *set = *skip = 0; }
void mtl_use_stats(uint64_t *call, uint64_t *skip) { *call = *skip = 0; }
uint64_t mtl_upload_ns_take(void) { return 0; }
uint64_t mtl_upload_gpu_ns_take(void) { return 0; }
uint64_t mtl_upload_bytes(void) { return 0; }
uint64_t mtl_udc_saved_bytes(void) { return 0; }
void mtl_ubp_why(uint64_t w[5]) { memset(w, 0, 5 * sizeof *w); }
void mtl_ubp_stats(uint64_t *a, uint64_t *b, uint64_t *c) { *a = *b = *c = 0; }
void mtl_ubd_stats(uint64_t *a, uint64_t *b) { *a = *b = 0; }
void mtl_res_stats(char *out, size_t n) { if (n) snprintf(out, n, "null graphics"); }
int mtl_pd_on(void) { return 0; }
void mtl_pd_note(const char *what, uint64_t v) { (void)what; (void)v; }
void mtl_pkg_stats(uint64_t *uploads, uint64_t *reuploads) { *uploads = *reuploads = 0; }
uint64_t mtl_pkg_miss(void) { return 0; }
void mtl_mem_stat(unsigned long long a, unsigned long long b) { (void)a; (void)b; }
void mtl_lc_stats(uint64_t *a, uint64_t *b) { *a = *b = 0; }
int mtl_gpu_span(uint64_t f, uint64_t *t0, uint64_t *t1) { (void)f; (void)t0; (void)t1; return 0; }
double mtl_gpu_ms(uint64_t f) { (void)f; return 0; }
uint64_t mtl_ds_omit(void) { return 0; }
uint64_t mtl_drawable_ns_take(void) { return 0; }
uint64_t mtl_cut_count(void) { return 0; }
uint64_t mtl_blit_splits(void) { return 0; }
void mtl_create_stats(uint64_t *lib, uint64_t *pso) { *lib = *pso = 0; }
void mtl_create_ns(uint64_t *lib_ns, uint64_t *pso_ns) { *lib_ns = *pso_ns = 0; }
void mtl_bw(uint64_t *load, uint64_t *store, uint64_t *full) { *load = *store = *full = 0; }
void mtl_barrier_stats(uint64_t *skip, uint64_t *full) { *skip = *full = 0; }
void mtl_args_stats(uint64_t *enc, uint64_t *reuse) { *enc = *reuse = 0; }
void mtl_alloc_stats(uint64_t *w, uint64_t *g, int *n) { *w = *g = 0; *n = 0; }
