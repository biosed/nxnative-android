// Hibernation across host builds (an app update between the snapshot and the restore): the words of a snapshot that point into the host image, and how a restore by another
// build maps them. The layout stays fixed (the image loads at the same address, the guest address space and the modules too), so what differs is where each host function and
// static lies inside the image. While a snapshot is written, every 8-byte word of what it saves -- guest memory and the host arena, the thread records, every section -- whose
// value falls in the image is looked at, and the section "hostrefs" keeps the ones a restore must map, each with the symbol it points into (hle_hibsym.c keys: name or file|name).
// Which words: game memory is full of data that only looks like an address in the image (on the Mac the image sits at 4 GB, so every pair of 32-bit values {1, x} does), and
// rewriting such a word would change the game's data. So a word is kept when
//   - it lies where the host writes its own addresses (trusted): the places the loader bound to a host symbol (GOT entries), the thread records and the sections, and the
//     return-address slots of the parked threads' frame chains; or
//   - it lies in game memory and points at what the host hands out: the start of an HLE, NVN or thunk function (the host's tables: swhle, swforce, swnvn, swthk, swrestart,
//     and the NVN thunks and restartable stubs by name), a static registered with HLE_HIB_STATIC / HLE_HIB_EXPORT, or the start of anything a trusted word points to.
// Words in the image's header (below its first function) are left: the image is at the same address in every build.
// A restoring build of another identity maps each kept word to the same symbol in itself: a symbol's start to its new address; inside a data symbol the same offset when the
// symbol's size is the same; inside a function the same offset when the function's code is the same (dm_hsym_code_hash, blind to the offsets of branches and pages). Strict: a
// kept word it cannot map refuses the snapshot (the game then boots as usual).
#include "dm.h"
#include "hle_hib.h"
#include "hle_hibsym.h"

// CLS_HEADER: the image's own header (Mach-O _mh_execute_header, the ELF header): the image is at the same address in every build, so a word there means the same; most are
// data that only looks like it (32-bit pairs such as {1, 1} = 0x100000001 on the Mac)
enum { CLS_NONE = 0, CLS_START = 1, CLS_IN_DATA = 2, CLS_IN_CODE = 3, CLS_HEADER = 4 };
#define CHUNK_MEM 0xFFFFFFFFu
typedef struct { uint64_t loc, val; uint32_t chunk; uint8_t live, pad[3]; } href;   // chunk: the chunk the word lies in (its offset in the chunk's data is loc), or CHUNK_MEM (loc: its address)
typedef struct { char key[120]; uint64_t addr, size, hash; uint8_t kind, pad[7]; } hr_sym;
typedef struct { uint64_t loc, val; uint32_t chunk, sym; uint8_t cls, live, pad[6]; } hr_ref;
typedef struct { char magic[8]; uint32_t nsym, nref; uint64_t lo, hi; } hr_hdr;

static href *g_r; static size_t g_nr, g_cr; static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_lo, g_hi;
static uint64_t *g_site; static size_t g_nsite, g_csite;   // the loader's bound places (sorted)
static void site_add(uint64_t a, void *ctx) { (void)ctx; if (g_nsite == g_csite) { g_csite = g_csite ? 2 * g_csite : 65536; g_site = realloc(g_site, g_csite * 8); } g_site[g_nsite++] = a; }
static uint64_t *g_ex; static size_t g_nex, g_cex;          // exported starts (sorted after building)
static uint64_t (*g_exr)[2]; static size_t g_nexr, g_cexr;  // exported data ranges
static void ex_add(uint64_t a) { if (g_nex == g_cex) { g_cex = g_cex ? 2 * g_cex : 4096; g_ex = realloc(g_ex, g_cex * 8); } g_ex[g_nex++] = a; }
static void exr_add(uint64_t a, uint64_t n) { if (g_nexr == g_cexr) { g_cexr = g_cexr ? 2 * g_cexr : 256; g_exr = realloc(g_exr, g_cexr * sizeof *g_exr); } g_exr[g_nexr][0] = a; g_exr[g_nexr][1] = a + n; g_nexr++; }
static int in_sorted(const uint64_t *v, size_t n, uint64_t a) { size_t lo = 0, hi = n; while (lo < hi) { size_t m = (lo + hi) / 2; if (v[m] < a) lo = m + 1; else hi = m; } return lo < n && v[lo] == a; }
static int in_exr(uint64_t a) { for (size_t i = 0; i < g_nexr; i++) if (a >= g_exr[i][0] && a < g_exr[i][1]) return 1; return 0; }
// What counts as live in a parked thread's stack: the return-address slots of its frame chain (x29 records from the frame it stopped in, each {previous x29, x30}), so that a
// stale return address left between frames by earlier calls does not hold up a restore. A stack whose chain cannot be followed counts as live from 128 bytes under its stack
// pointer (the rewriter parks registers there) to its end
static uint64_t (*g_live)[2]; static int g_nlive;   // whole ranges (stacks whose chain broke off)
static uint64_t *g_lr; static int g_nlr, g_clr;     // return-address slots, sorted
static int u64cmp(const void *a, const void *b) { uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b; return x < y ? -1 : x > y; }
static int is_live(uint64_t a) {
    for (int i = 0; i < g_nlive; i++) if (a >= g_live[i][0] && a < g_live[i][1]) return 1;
    int lo = 0, hi = g_nlr - 1; while (lo <= hi) { int m = (lo + hi) / 2; if (g_lr[m] < a) lo = m + 1; else if (g_lr[m] > a) hi = m - 1; else return 1; }
    return in_sorted(g_site, g_nsite, a);   // a GOT entry or another place the loader bound
}
int svc_map_span(uint64_t a, uint64_t *base, uint64_t *size);   // hle_svc.c

uint32_t dm_hib_hostrefs_fmt(void) { return HIB_FMT(1, sizeof(hr_hdr), sizeof(hr_sym), sizeof(hr_ref)); }
// Before the snapshot's sections (thr NULL): the image's range. With the thread records: their live stacks (the gate frame keeps x29 at word 29 and sp at word 99, a restartable
// call's frame x29 at 0x98 and sp at 0xa8)
void dm_hibrefs_begin(const hib_thr *thr, int nt) {
    dm_hsym_range(&g_lo, &g_hi);
    if (!thr) { pthread_mutex_lock(&g_mu); g_nr = 0; pthread_mutex_unlock(&g_mu); g_nlive = 0; g_nlr = 0; return; }
    free(g_live); g_live = calloc((size_t)nt + 1, sizeof *g_live); g_nlive = 0; g_nlr = 0;
    int nbroken = 0;
    for (int i = 0; i < nt; i++) {
        if (thr[i].stop == HIB_EXITED || thr[i].stop == HIB_NOT_STARTED) continue;
        int rs = thr[i].stop == HIB_RESTART;
        uint64_t sp = rs ? thr[i].frame[0xa8 / 8] : thr[i].frame[99], fp = rs ? thr[i].frame[0x98 / 8] : thr[i].frame[29], b, z;
        if (!sp || !svc_map_span(sp, &b, &z)) { b = sp; z = sp ? 1u << 20 : 0; }   // a stack outside the mapped regions: a megabyte above sp
        uint64_t top = b + z; int ok = 1, steps = 0;
        while (fp) {
            if (fp < sp || fp + 16 > top || (fp & 15) || ++steps > 65536) { ok = 0; break; }
            if (g_nlr == g_clr) { g_clr = g_clr ? 2 * g_clr : 4096; g_lr = realloc(g_lr, (size_t)g_clr * 8); }
            g_lr[g_nlr++] = fp + 8;
            uint64_t next = *(const uint64_t *)(uintptr_t)fp;
            if (next && next <= fp) { ok = 0; break; }
            fp = next;
        }
        if (!ok) { g_live[g_nlive][0] = sp > 128 ? sp - 128 : 0; g_live[g_nlive][1] = top; g_nlive++; nbroken++; }
    }
    qsort(g_lr, (size_t)g_nlr, 8, u64cmp);
    void dm_loader_bound_sites(void (*cb)(uint64_t, void *), void *ctx); g_nsite = 0; dm_loader_bound_sites(site_add, NULL); qsort(g_site, g_nsite, 8, u64cmp);
    LOG("hibernate: frame chains: %d return-address slots, %d stacks taken whole (chain broken); %zu places the loader bound", g_nlr, nbroken, g_nsite);
}
// The words of [p, p + n) that point into the image; loc0: the address of p (memory, chunk CHUNK_MEM) or the offset of p in the chunk's data
void dm_hibrefs_scan(uint32_t chunk, uint64_t loc0, const void *p, size_t n) {
    if (!g_hi) return;
    const uint64_t *w = p; size_t k = n / 8; href loc[256]; int nl = 0;
    for (size_t i = 0; i < k; i++) {
        uint64_t v = w[i]; if (v - g_lo >= g_hi - g_lo) continue;
        uint64_t at = loc0 + i * 8;
        loc[nl++] = (href){at, v, chunk, (uint8_t)(chunk != CHUNK_MEM || is_live(at)), {0}};
        if (nl == 256 || i + 1 == k) {
            pthread_mutex_lock(&g_mu);
            if (g_nr + (size_t)nl > g_cr) { g_cr = (g_nr + (size_t)nl) * 2 + 4096; g_r = realloc(g_r, g_cr * sizeof *g_r); }
            memcpy(g_r + g_nr, loc, (size_t)nl * sizeof *loc); g_nr += (size_t)nl; nl = 0;
            pthread_mutex_unlock(&g_mu);
        }
    }
    if (nl) { pthread_mutex_lock(&g_mu); if (g_nr + (size_t)nl > g_cr) { g_cr = (g_nr + (size_t)nl) * 2 + 4096; g_r = realloc(g_r, g_cr * sizeof *g_r); }
              memcpy(g_r + g_nr, loc, (size_t)nl * sizeof *loc); g_nr += (size_t)nl; pthread_mutex_unlock(&g_mu); }
}
static int by_val(const void *a, const void *b) { const href *x = a, *y = b; return x->val < y->val ? -1 : x->val > y->val; }
// What the host hands out (see the top): from its tables and its registered statics
#include "hle.h"
static void ex_table(const void *t, unsigned long sz) {   // a table section: the image addresses its words hold, and the table itself (a guest may hold an entry's address)
    if (!t || !sz) return;
    exr_add((uint64_t)(uintptr_t)t, sz);
    const uint64_t *w = t; for (unsigned long i = 0; i < sz / 8; i++) if (w[i] - g_lo < g_hi - g_lo) ex_add(w[i]);
}
static void ex_statics(const void *t, unsigned long sz) { const hib_static_ent *e = t; for (unsigned long i = 0; e && i < sz / sizeof *e; i++) exr_add((uint64_t)(uintptr_t)e[i].p, e[i].n); }
static void exports_build(void) {
    g_nex = g_nexr = 0; unsigned long sz;
    sz = 0; ex_table(DM_SECTION_DATA(swhle, &sz), sz);
    sz = 0; ex_table(DM_SECTION_DATA(swforce, &sz), sz);
    sz = 0; ex_table(DM_SECTION_DATA(swnvn, &sz), sz);
    sz = 0; ex_table(DM_SECTION_DATA(swthk, &sz), sz);
    sz = 0; ex_table(DM_SECTION_DATA(swrestart, &sz), sz);
    sz = 0; ex_statics(DM_SECTION_DATA(swhibst, &sz), sz);
    sz = 0; ex_statics(DM_SECTION_DATA(swhibex, &sz), sz);
    for (int i = 0, n = dm_hsym_count(); i < n; i++) { const hsym *h = dm_hsym_get(i); const char *nm = dm_hsym_name(h);   // given out without a table
        if (h->kind == 'T' && (!strncmp(nm, "nvnth_", 6) || !strncmp(nm, "dm_hib_rs_e", 11) || !strncmp(nm, "dm_gate", 7) || !strncmp(nm, "thk_", 4) || !strncmp(nm, "dm_thk_", 7))) ex_add(h->addr); }
}
// The section "hostrefs": the symbols the kept words point into, then the words
size_t dm_hibrefs_section(uint8_t **out) {
    pthread_mutex_lock(&g_mu);
    exports_build();
    uint64_t code0 = dm_hsym_code_start();
    for (size_t i = 0; i < g_nr; i++) if (g_r[i].live && g_r[i].val >= code0) { const hsym *s = dm_hsym_at(g_r[i].val);   // what trusted words point to is handed out too
        if (s && g_r[i].val == s->addr) ex_add(s->addr); }
    qsort(g_ex, g_nex, 8, u64cmp);
    qsort(g_r, g_nr, sizeof *g_r, by_val);
    hr_sym *sy = calloc(g_nr + 1, sizeof *sy); uint32_t ns = 0; hr_ref *rf = calloc(g_nr + 1, sizeof *rf); size_t nk = 0;
    uint64_t nhdr = 0, nleft = 0, ntrust = 0, ngame = 0, nlive_code = 0; const hsym *last = NULL; uint32_t lastk = ~0u;
    for (size_t i = 0; i < g_nr; i++) {
        const href *h = &g_r[i]; const hsym *s = dm_hsym_at(h->val); uint8_t cls = CLS_NONE;
        if (h->val < code0 && (!s || s->addr == g_lo)) { nhdr++; continue; }   // the header: the same in every build
        if (s) cls = h->val == s->addr ? CLS_START : s->kind == 'D' ? CLS_IN_DATA : CLS_IN_CODE;
        if (!h->live) {   // game memory: only what the host hands out
            int keep = (cls == CLS_START && in_sorted(g_ex, g_nex, h->val)) || ((cls == CLS_START || cls == CLS_IN_DATA) && in_exr(h->val));
            if (!keep) {
                if (s && s->kind == 'D' && getenv("DM_HIB_REFS_LOG")) { char k[128]; dm_hsym_key(s, k, sizeof k); LOG("hibrefs left: D %s+%#llx at %#llx", k, (unsigned long long)(h->val - s->addr), (unsigned long long)h->loc); }
                nleft++; continue;
            }
            ngame++;
        } else ntrust++;
        uint32_t k = ~0u;
        if (s) { if (s != last) { last = s; hr_sym *e = &sy[ns]; dm_hsym_key(s, e->key, sizeof e->key); e->addr = s->addr; e->size = s->size; e->kind = s->kind; e->hash = dm_hsym_code_hash(s); lastk = ns++; } k = lastk; }
        if (h->live && cls == CLS_IN_CODE) nlive_code++;
        rf[nk++] = (hr_ref){h->loc, h->val, h->chunk, k, cls, h->live, {0}};
    }
    if (getenv("DM_HIB_REFS_LOG")) {   // diagnostics: the symbols kept words point at, with how many point at the start / inside, and how many of those are trusted
        uint32_t *c = calloc((size_t)ns * 4 + 4, 4);
        for (size_t i = 0; i < nk; i++) if (rf[i].sym < ns) { c[rf[i].sym * 4 + (rf[i].cls == CLS_START ? 0 : 1)]++; if (rf[i].live) c[rf[i].sym * 4 + 2]++; }
        for (uint32_t i = 0; i < ns; i++) LOG("hibrefs: %c %#llx %s start %u inside %u trusted %u", sy[i].kind, (unsigned long long)sy[i].addr, sy[i].key[0] ? sy[i].key : "(unnamed)", c[i * 4], c[i * 4 + 1], c[i * 4 + 2]);
        free(c);
    }
    hr_hdr hd = {{'H','I','B','R','E','F','S','1'}, ns, (uint32_t)nk, g_lo, g_hi};
    size_t n = sizeof hd + ns * sizeof *sy + nk * sizeof *rf; uint8_t *b = malloc(n);
    memcpy(b, &hd, sizeof hd); memcpy(b + sizeof hd, sy, ns * sizeof *sy); memcpy(b + sizeof hd + ns * sizeof *sy, rf, nk * sizeof *rf);
    LOG("hibernate: host references: %zu words into the image: %llu trusted (%llu of them return addresses), %llu in game memory at what the host hands out (%zu functions, %zu data ranges), "
        "%llu in the header and %llu other words of game memory left as they are; %u symbols", g_nr, (unsigned long long)ntrust, (unsigned long long)nlive_code, (unsigned long long)ngame,
        g_nex, g_nexr, (unsigned long long)nhdr, (unsigned long long)nleft, ns);
    g_nr = 0;
    pthread_mutex_unlock(&g_mu);
    free(sy); free(rf);
    *out = b; return n;
}

// ---- restoring with another build ----
typedef struct { uint64_t loc, oldv, newv; uint32_t chunk; } hpatch;
static hpatch *g_p; static size_t g_np;
// Decides every word before anything changes: 0 when each one the restore needs is mapped (the patches kept for dm_hibrefs_patch_chunk and dm_hibrefs_apply_mem), else -1 with why
int dm_hibrefs_plan(const uint8_t *b, size_t n, char *why, size_t wn) {
    hr_hdr hd; if (n < sizeof hd) { snprintf(why, wn, "host references damaged"); return -1; } memcpy(&hd, b, sizeof hd);
    if (memcmp(hd.magic, "HIBREFS1", 8) || n != sizeof hd + (size_t)hd.nsym * sizeof(hr_sym) + (size_t)hd.nref * sizeof(hr_ref)) { snprintf(why, wn, "host references of another format"); return -1; }
    uint64_t lo, hi; dm_hsym_range(&lo, &hi);
    if (!dm_hsym_load()) { snprintf(why, wn, "this host build has no symbol table"); return -1; }
    const hr_sym *sy = (const hr_sym *)(b + sizeof hd); const hr_ref *rf = (const hr_ref *)(b + sizeof hd + (size_t)hd.nsym * sizeof *sy);
    // each symbol: its address in this build (0: not here), and whether a word inside it keeps its offset
    uint64_t *na = calloc((size_t)hd.nsym + 1, 8); uint8_t *same = calloc((size_t)hd.nsym + 1, 1);
    int nmiss = 0, nchg = 0;
    for (uint32_t i = 0; i < hd.nsym; i++) {
        const hsym *s = sy[i].key[0] ? dm_hsym_find(sy[i].key) : NULL;
        if (!s || s->kind != sy[i].kind) { nmiss++; continue; }
        na[i] = s->addr;
        same[i] = s->size == sy[i].size && (sy[i].kind == 'D' || dm_hsym_code_hash(s) == sy[i].hash);
        nchg += !same[i];
    }
    free(g_p); g_p = malloc(((size_t)hd.nref + 1) * sizeof *g_p); g_np = 0;
    uint64_t nsame = 0; int bad = 0;
    for (uint32_t i = 0; i < hd.nref && !bad; i++) {
        const hr_ref *r = &rf[i]; const hr_sym *s = r->sym < hd.nsym ? &sy[r->sym] : NULL; uint64_t nv = 0; int ok = 0;
        if (r->cls == CLS_START) { ok = na[r->sym] && (s->kind == 'T' || same[r->sym]); nv = na[r->sym]; }
        else if (r->cls == CLS_IN_DATA || r->cls == CLS_IN_CODE) { ok = na[r->sym] && same[r->sym]; nv = na[r->sym] + (r->val - s->addr); }
        if (!ok) {
            snprintf(why, wn, "a %s word at %s%#llx points to %s%s+%#llx, which this build %s", r->live ? "host" : "game memory", r->chunk == CHUNK_MEM ? "" : "chunk data +", (unsigned long long)r->loc,
                     s ? s->key : "(no symbol)", s && !s->key[0] ? "(unnamed)" : "", (unsigned long long)(s ? r->val - s->addr : 0),
                     !s ? "cannot name" : !na[r->sym] ? "does not have" : "has changed");
            bad = 1; break;
        }
        if (nv == r->val) { nsame++; continue; }
        if (getenv("DM_HIB_REFS_LOG")) LOG("hibrefs patch: %s%#llx %#llx -> %#llx (%s+%#llx, class %d, %s)", r->chunk == CHUNK_MEM ? "" : "chunk +", (unsigned long long)r->loc,
                                            (unsigned long long)r->val, (unsigned long long)nv, s ? s->key : "-", (unsigned long long)(s ? r->val - s->addr : 0), r->cls, r->live ? "trusted" : "game");
        g_p[g_np++] = (hpatch){r->loc, r->val, nv, r->chunk};
    }
    free(na); free(same);
    if (bad) { g_np = 0; return -1; }
    LOG("hibernate: another host build: %u host references mapped by %u symbols (%d not in this build, %d changed; none of them needed), %zu words to rewrite, %llu already where they were",
        hd.nref, hd.nsym, nmiss, nchg, g_np, (unsigned long long)nsame);
    return 0;
}
// The patches into one chunk's data (a section or the thread records), before it is used
int dm_hibrefs_patch_chunk(uint32_t chunk, uint8_t *d, size_t n) {
    for (size_t i = 0; i < g_np; i++) if (g_p[i].chunk == chunk) {
        if (g_p[i].loc + 8 > n) return -1;
        uint64_t v; memcpy(&v, d + g_p[i].loc, 8); if (v != g_p[i].oldv) return -1;
        memcpy(d + g_p[i].loc, &g_p[i].newv, 8);
    }
    return 0;
}
// The patches into memory, once the memory chunks are back
int dm_hibrefs_apply_mem(void) {
    size_t n = 0;
    for (size_t i = 0; i < g_np; i++) if (g_p[i].chunk == CHUNK_MEM) {
        uint64_t *w = (uint64_t *)(uintptr_t)g_p[i].loc;
        if (*w != g_p[i].oldv) { LOG("[RED] hibernate: host reference at %#llx is %#llx, the snapshot recorded %#llx", (unsigned long long)g_p[i].loc, (unsigned long long)*w, (unsigned long long)g_p[i].oldv); return -1; }
        *w = g_p[i].newv; n++;
    }
    LOG("hibernate: %zu host references in memory rewritten for this build", n);
    return 0;
}
