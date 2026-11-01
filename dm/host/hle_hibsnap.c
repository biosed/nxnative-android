// Hibernation: the snapshot file (DM_HIBERNATE=1). dm_hib_save parks every guest thread at a gate (hle_thread.c), drains the GPU, and writes guest memory and the threads to
// <dir>/snap.tmp, then makes it snap.bin with a rename, so that a process killed at any moment leaves either the previous snapshot or the new one, never half of one.
// File: a header (what the snapshot is valid for: the game, the host executable's identity and address, the fixed layout, the modules, the save folder), then chunks, then the
// chunk table and a trailer. Each chunk is LZ4-compressed (dm/third_party/lz4) with a CRC-32C of its raw bytes; the table has its own CRC, and the trailer is written last:
//   chunk: u32 type, u32 flags, u64 addr, u64 raw_len, u64 stored_len, u32 crc, u32 pad, then stored_len bytes
//   memory chunk (HIB_C_MEM): addr = start, raw = u64 len, u32 prot, u32 page size, a bitmap of the pages that are not all zero, then those pages one after another
//   trailer: u64 table offset, u32 chunk count, u32 table crc, "DMHIBEND"
// Restoring a snapshot (later) reads it into a fresh process with the same layout.
#include "dm.h"
#include "hle.h"
#include "hle_hib.h"
#include "../third_party/lz4/lz4.h"
#include <dirent.h>
#include <stdatomic.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <mach/mach.h>
#ifdef __APPLE__
#include <mach/mach_vm.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#endif

void dm_hib_layout(uint8_t **win, size_t *win_len, size_t *mod_used, uint8_t **arena, size_t *arena_used);
void svc_guest_range(uint8_t **base, size_t *size);
uintptr_t dm_hib_image_base(void);

enum { HIB_C_MEM = 1, HIB_C_THREADS = 2, HIB_C_SECTION = 3 };
#define HIB_VERSION 1
#define MEM_PIECE (1u << 20)          // memory chunks cover at most 1 MB
#define ZPAGE 4096                    // zero pages are left out at this granularity (Android's page size; the Mac's 16 KB pages are four of them)

typedef struct {
    char magic[8]; uint32_t version, hdr_size;
    char game[24];
    uint64_t image_base; uint8_t image_id[20];   // the host executable: its address and its identity (Mach-O LC_UUID / ELF build ID)
    uint32_t pad0;
    uint64_t win, win_len, guest_as, guest_len, arena, arena_used, mod_used;
    uint32_t nmod, pad1;
    struct { char name[16]; uint64_t base, size; char build_id[40]; } mod[8];
    uint64_t created_unix, save_fp;
    uint64_t guest_ns;                // the guest clock when written
} hib_hdr;
typedef struct { uint32_t type, flags; uint64_t addr, raw_len, stored_len; uint32_t crc, pad; } hib_chunk;
typedef struct { uint64_t off; uint32_t type, crc; uint64_t addr, raw_len, stored_len; } hib_tab;

// ---- CRC-32C (the arm64 CRC instructions) ----
__attribute__((target("crc"))) static uint32_t crc32c(uint32_t c, const void *p, size_t n) {
    const uint8_t *b = p; c = ~c;
    while (n >= 8) { uint64_t v; memcpy(&v, b, 8); c = __builtin_arm_crc32cd(c, v); b += 8; n -= 8; }
    while (n--) c = __builtin_arm_crc32cb(c, *b++);
    return ~c;
}

// ---- the host executable's identity ----
#ifndef __APPLE__
// The GNU build ID note of the ELF image loaded at base (read from its program headers in memory: link.h's types clash with dm.h's)
static void elf_build_id(uintptr_t base, uint8_t *out) {
    const uint8_t *eh = (const uint8_t *)base;
    if (!base || memcmp(eh, "\177ELF", 4)) return;
    uint64_t phoff; uint16_t phentsize, phnum; memcpy(&phoff, eh + 0x20, 8); memcpy(&phentsize, eh + 0x36, 2); memcpy(&phnum, eh + 0x38, 2);
    for (int i = 0; i < phnum; i++) {
        const uint8_t *ph = eh + phoff + (uint64_t)i * phentsize; uint32_t type; uint64_t vaddr, memsz;
        memcpy(&type, ph, 4); memcpy(&vaddr, ph + 16, 8); memcpy(&memsz, ph + 40, 8);
        if (type != 4) continue;   // PT_NOTE
        const uint8_t *p = (const uint8_t *)(base + vaddr), *e = p + memsz;
        while (p + 12 <= e) {
            uint32_t nn, dn, ty; memcpy(&nn, p, 4); memcpy(&dn, p + 4, 4); memcpy(&ty, p + 8, 4);
            const uint8_t *d = p + 12 + ((nn + 3) & ~3u);
            if (ty == 3 && nn == 4 && !memcmp(p + 12, "GNU", 4)) { memcpy(out, d, dn < 20 ? dn : 20); return; }
            p = d + ((dn + 3) & ~3u);
        }
    }
}
#endif
static void image_id(uint8_t out[20]) {
    memset(out, 0, 20);
#ifdef __APPLE__
    const struct mach_header_64 *h = (const struct mach_header_64 *)_dyld_get_image_header(0);
    const struct load_command *lc = (const void *)(h + 1);
    for (uint32_t i = 0; i < h->ncmds; i++, lc = (const void *)((const uint8_t *)lc + lc->cmdsize))
        if (lc->cmd == LC_UUID) { memcpy(out, ((const struct uuid_command *)lc)->uuid, 16); return; }
#else
    elf_build_id(dm_hib_image_base(), out);
#endif
}

// ---- the save folder's fingerprint: names, sizes and modification times; a snapshot is only restored over the save data it was written with ----
static uint64_t save_fingerprint(void) {
    const char *dir = getenv("DM_SAVE"); uint64_t h = 1469598103934665603ull;
    if (!dir) return 0;
    for (int pass = 0; pass < 2; pass++) {   // the save folder and its uncommitted twin (<save>.work)
        char d[1024]; snprintf(d, sizeof d, pass ? "%s.work" : "%s", dir);
        DIR *dp = opendir(d); if (!dp) continue;
        struct dirent *e;
        while ((e = readdir(dp))) {
            if (e->d_name[0] == '.') continue;
            char p[2048]; snprintf(p, sizeof p, "%s/%s", d, e->d_name); struct stat st; if (stat(p, &st)) continue;
            uint64_t v[3] = {(uint64_t)st.st_size, (uint64_t)st.st_mtime, (uint64_t)pass};
            for (const char *c = e->d_name; *c; c++) h = (h ^ (uint8_t)*c) * 1099511628211ull;
            for (int k = 0; k < 3; k++) h = (h ^ v[k]) * 1099511628211ull;
        }
        closedir(dp);
    }
    return h;
}

// ---- writer ----
typedef struct { int fd; uint64_t off; hib_tab *tab; uint32_t n, cap; char *zbuf; size_t zcap; int err; uint64_t raw_total, stored_total; } wr_t;
static void wr_bytes(wr_t *w, const void *p, size_t n) {
    const uint8_t *b = p;
    while (n && !w->err) { ssize_t k = write(w->fd, b, n); if (k < 0) { if (errno == EINTR) continue; w->err = errno; break; } b += k; n -= (size_t)k; w->off += (uint64_t)k; }
}
// A chunk compressed into z (size zcap, at least LZ4_compressBound(raw_len)): fills *c, returns what to write after it (z, or raw when it does not compress)
static const void *chunk_pack(hib_chunk *c, uint32_t type, uint64_t addr, const void *raw, size_t raw_len, char *z, size_t zcap) {
    int k = LZ4_compress_default(raw, z, (int)raw_len, (int)zcap);
    *c = (hib_chunk){type, k > 0 ? 1u : 0u, addr, raw_len, k > 0 ? (uint64_t)k : raw_len, crc32c(0, raw, raw_len), 0};
    return k > 0 ? (const void *)z : raw;
}
static void chunk_emit(wr_t *w, const hib_chunk *c, const void *data) {
    if (w->n == w->cap) { w->cap = w->cap ? w->cap * 2 : 1024; w->tab = realloc(w->tab, w->cap * sizeof *w->tab); }
    w->tab[w->n++] = (hib_tab){w->off, c->type, c->crc, c->addr, c->raw_len, c->stored_len};
    wr_bytes(w, c, sizeof *c); wr_bytes(w, data, c->stored_len);
    w->raw_total += c->raw_len; w->stored_total += c->stored_len;
}
static void wr_chunk(wr_t *w, uint32_t type, uint64_t addr, const void *raw, size_t raw_len) {
    size_t bound = (size_t)LZ4_compressBound((int)raw_len);
    if (bound > w->zcap) { free(w->zbuf); w->zcap = bound; w->zbuf = malloc(bound); }
    hib_chunk c; const void *d = chunk_pack(&c, type, addr, raw, raw_len, w->zbuf, w->zcap);
    chunk_emit(w, &c, d);
}

// The spans of the window to keep, with their protections: the modules' writable pages (their code and read-only data are the same in every process), the arena's and the guest address
// space's readable pages
typedef struct { uint64_t lo, hi; int prot; } span_t;
static int spans(span_t *out, int max) {
    uint8_t *win, *arena; size_t win_len, mod_used, arena_used; dm_hib_layout(&win, &win_len, &mod_used, &arena, &arena_used);
    uint64_t lo = (uint64_t)(uintptr_t)win, hi = lo + win_len, mods_end = lo + mod_used; int n = 0;
#ifdef __APPLE__
    mach_vm_address_t a = lo; mach_vm_size_t s;
    while (a < hi && n < max) {
        natural_t depth = 0; struct vm_region_submap_info_64 info; mach_msg_type_number_t cnt = VM_REGION_SUBMAP_INFO_COUNT_64;
        if (mach_vm_region_recurse(mach_task_self(), &a, &s, &depth, (vm_region_recurse_info_t)&info, &cnt) != KERN_SUCCESS || a >= hi) break;
        uint64_t e = a + s < hi ? a + s : hi; int prot = info.protection & 7;
        int keep = a < mods_end ? (prot & VM_PROT_WRITE) : (prot & VM_PROT_READ);
        if (keep) { if (n && out[n - 1].hi == a && out[n - 1].prot == prot) out[n - 1].hi = e; else out[n++] = (span_t){a, e, prot}; }
        a += s;
    }
#else
    FILE *f = fopen("/proc/self/maps", "r"); char ln[512];
    while (f && fgets(ln, sizeof ln, f) && n < max) {
        unsigned long long a, e; char perm[8] = "";
        if (sscanf(ln, "%llx-%llx %7s", &a, &e, perm) < 3 || e <= lo || a >= hi) continue;
        if (a < lo) a = lo;
        if (e > hi) e = hi;
        int prot = (perm[0] == 'r') | (perm[1] == 'w') << 1 | (perm[2] == 'x') << 2;
        int keep = a < mods_end ? (prot & 2) : (prot & 1);
        if (keep) { if (n && out[n - 1].hi == a && out[n - 1].prot == prot) out[n - 1].hi = e; else out[n++] = (span_t){a, e, prot}; }
    }
    if (f) fclose(f);
#endif
    return n;
}

// Memory goes out in pieces of at most 1 MB: the pages that are not all zero, compressed by HIB_WORKERS threads at once and written in order
#define HIB_WORKERS 4
#define PIECE_RAW (16 + MEM_PIECE / ZPAGE / 8 + MEM_PIECE)
typedef struct { uint64_t a, len; int prot; } piece_t;
static struct { wr_t *w; piece_t *p; int np; _Atomic int next; int turn; pthread_mutex_t mu; pthread_cond_t cv; } g_mw;
static void *mem_worker(void *arg) {
    (void)arg;
    uint8_t *raw = malloc(PIECE_RAW); size_t zcap = (size_t)LZ4_compressBound(PIECE_RAW); char *z = malloc(zcap);
    for (int i; (i = atomic_fetch_add(&g_mw.next, 1)) < g_mw.np; ) {
        const piece_t *pc = &g_mw.p[i];
        uint8_t *bm = raw + 16, *data = bm + MEM_PIECE / ZPAGE / 8; size_t nd = 0;
        memset(bm, 0, MEM_PIECE / ZPAGE / 8);
        for (uint64_t o = 0; o < pc->len; o += ZPAGE) {
            const uint64_t *p = (const uint64_t *)(uintptr_t)(pc->a + o); uint64_t acc = 0;
            for (size_t k = 0; k < ZPAGE / 8; k++) acc |= p[k];
            if (!acc) continue;
            bm[o / ZPAGE / 8] |= (uint8_t)(1u << (o / ZPAGE % 8));
            memcpy(data + nd, p, ZPAGE); nd += ZPAGE;
        }
        uint64_t hdr[2] = {pc->len, (uint64_t)pc->prot | (uint64_t)ZPAGE << 32}; memcpy(raw, hdr, 16);
        hib_chunk c; const void *d = chunk_pack(&c, HIB_C_MEM, pc->a, raw, 16 + MEM_PIECE / ZPAGE / 8 + nd, z, zcap);
        pthread_mutex_lock(&g_mw.mu);
        while (g_mw.turn != i) pthread_cond_wait(&g_mw.cv, &g_mw.mu);
        chunk_emit(g_mw.w, &c, d);
        g_mw.turn++; pthread_cond_broadcast(&g_mw.cv);
        pthread_mutex_unlock(&g_mw.mu);
    }
    free(raw); free(z);
    return NULL;
}
static void wr_memory(wr_t *w) {
    static span_t sp[65536]; int ns = spans(sp, 65536), np = 0;
    for (int i = 0; i < ns; i++) np += (int)((sp[i].hi - sp[i].lo + MEM_PIECE - 1) / MEM_PIECE);
    piece_t *p = malloc((size_t)np * sizeof *p + 1); np = 0;
    for (int i = 0; i < ns; i++) for (uint64_t a = sp[i].lo; a < sp[i].hi; a += MEM_PIECE) p[np++] = (piece_t){a, sp[i].hi - a < MEM_PIECE ? sp[i].hi - a : MEM_PIECE, sp[i].prot};
    g_mw.w = w; g_mw.p = p; g_mw.np = np; atomic_store(&g_mw.next, 0); g_mw.turn = 0;
    pthread_mutex_init(&g_mw.mu, NULL); pthread_cond_init(&g_mw.cv, NULL);
    pthread_t th[HIB_WORKERS];
    for (int i = 0; i < HIB_WORKERS; i++) pthread_create(&th[i], NULL, mem_worker, NULL);
    for (int i = 0; i < HIB_WORKERS; i++) pthread_join(th[i], NULL);
    pthread_mutex_destroy(&g_mw.mu); pthread_cond_destroy(&g_mw.cv);
    free(p);
}

// The snapshot's folder: DM_HIB_DIR, or "hibernate" beside the save folder
static void hib_dir(char *out, size_t n) {
    const char *d = getenv("DM_HIB_DIR"), *s = getenv("DM_SAVE");
    if (d && *d) snprintf(out, n, "%s", d);
    else if (s && *s) { snprintf(out, n, "%s", s); char *sl = strrchr(out, '/'); if (sl) snprintf(sl + 1, n - (size_t)(sl + 1 - out), "hibernate"); else snprintf(out, n, "hibernate"); }
    else snprintf(out, n, "hibernate");
}

// What the snapshot cannot hold yet: then there is none (the game simply restarts as before). NULL when nothing stands in the way
int dm_movie_live(void); int dm_fs_dirs_open(void);
static const char *policy(void) {
    if (dm_movie_live()) return "a movie decoder exists (its state is in libvpx / VideoToolbox)";
    if (dm_fs_dirs_open()) return "a directory handle is open (the guest holds a host pointer)";
    return NULL;
}
static int g_saving;
// Writes the snapshot. Returns 0 on success; the world runs again afterwards either way (the caller decides whether the process then goes on or stops).
// gfx_closed: the caller closed the graphics gate already (its suspend sequence): it is opened for the park, so that a thread waiting at it reaches a gate of its own, and closed again
int dm_hib_save_ex(int gfx_closed) {
    if (!dm_hib_on()) { LOG("[RED] hibernate: not on (DM_HIBERNATE=1 from the start)"); return -1; }
    uint8_t *win, *arena; size_t win_len, mod_used, arena_used; dm_hib_layout(&win, &win_len, &mod_used, &arena, &arena_used);
    if (!win) { LOG("[RED] hibernate: no fixed layout this session"); return -1; }
    if (__atomic_exchange_n(&g_saving, 1, __ATOMIC_SEQ_CST)) { LOG("[RED] hibernate: a snapshot is already being written"); return -1; }
    char dir[1024], tmp[1100], fin[1100]; hib_dir(dir, sizeof dir); mkdir(dir, 0755);
    snprintf(tmp, sizeof tmp, "%s/snap.tmp", dir); snprintf(fin, sizeof fin, "%s/snap.bin", dir);
    uint64_t t0 = now_ns();
    char why[1024]; const char *no = policy();
    if (no) { LOG("hibernate: no snapshot now: %s", no); g_saving = 0; return -1; }
    if (gfx_closed) dm_gfx_suspend(0);
    if (dm_guest_park_all(3000, why, sizeof why)) { LOG("[RED] hibernate: no snapshot: %s", why); if (gfx_closed) dm_gfx_suspend(1); g_saving = 0; return -1; }
    if ((no = policy())) { LOG("hibernate: no snapshot now: %s", no); dm_guest_unpark_all(); if (gfx_closed) dm_gfx_suspend(1); g_saving = 0; return -1; }   // started meanwhile
    uint64_t t1 = now_ns();
    dm_gfx_suspend(1);   // with every thread parked no new GPU work comes; the work in flight is drained
    uint64_t t2 = now_ns();
    int rc = -1;
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) LOG("[RED] hibernate: cannot write %s (errno %d)", tmp, errno);
    else {
        wr_t w = {.fd = fd};
        hib_hdr h; memset(&h, 0, sizeof h);
        memcpy(h.magic, "DMHIBSN1", 8); h.version = HIB_VERSION; h.hdr_size = sizeof h;
        snprintf(h.game, sizeof h.game, "%s", DM_GAME_ID);
        h.image_base = dm_hib_image_base(); image_id(h.image_id);
        uint8_t *gb; size_t gs; svc_guest_range(&gb, &gs);
        h.win = (uint64_t)(uintptr_t)win; h.win_len = win_len; h.guest_as = (uint64_t)(uintptr_t)gb; h.guest_len = gs;
        h.arena = (uint64_t)(uintptr_t)arena; h.arena_used = arena_used; h.mod_used = mod_used;
        h.nmod = (uint32_t)(g_nmod < 8 ? g_nmod : 8);
        for (uint32_t i = 0; i < h.nmod; i++) { snprintf(h.mod[i].name, 16, "%s", g_mod[i].name); h.mod[i].base = (uint64_t)(uintptr_t)g_mod[i].base; h.mod[i].size = g_mod[i].size;
                                                snprintf(h.mod[i].build_id, 40, "%s", g_mod[i].build_id); }
        h.created_unix = (uint64_t)time(NULL); h.save_fp = save_fingerprint(); h.guest_ns = guest_ns();
        wr_bytes(&w, &h, sizeof h);
        dm_hib_hold(1);   // no thread finishes an SVC while memory and the threads are written
        static hib_thr thr[512]; int nt = dm_hib_threads(thr, 512);
        wr_chunk(&w, HIB_C_THREADS, (uint64_t)nt, thr, (size_t)nt * sizeof *thr);
        wr_memory(&w);
        dm_hib_hold(0);
        uint64_t toff = w.off;
        wr_bytes(&w, w.tab, (size_t)w.n * sizeof *w.tab);
        struct { uint64_t off; uint32_t n, crc; char magic[8]; } tr = {toff, w.n, crc32c(0, w.tab, (size_t)w.n * sizeof *w.tab), {'D','M','H','I','B','E','N','D'}};
        wr_bytes(&w, &tr, sizeof tr);
        uint64_t t3 = now_ns();
#ifdef __APPLE__
        int synced = !w.err && fcntl(fd, F_FULLFSYNC) == 0;
#else
        int synced = !w.err && fdatasync(fd) == 0;
#endif
        if (close(fd)) synced = 0;
        if (!w.err && synced && !rename(tmp, fin)) {
            int dfd = open(dir, O_RDONLY); if (dfd >= 0) { fsync(dfd); close(dfd); }
            rc = 0;
            int nthr[5] = {0}; for (int i = 0; i < nt; i++) if (thr[i].stop < 5) nthr[thr[i].stop]++;
            LOG("hibernate: snapshot %s: %d threads (%d at a gate, %d after an SVC, %d in a wait, %d in a restartable host call, %d not started), %u chunks, %.1f MB of memory and state in %.1f MB; "
                "park %.0f ms, GPU drain %.0f ms, write %.0f ms, sync %.0f ms", fin, nt, nthr[1], nthr[2], nthr[4], nthr[3], nthr[0], w.n, w.raw_total / 1048576.0, w.off / 1048576.0,
                (t1 - t0) / 1e6, (t2 - t1) / 1e6, (t3 - t2) / 1e6, (now_ns() - t3) / 1e6);
        } else { LOG("[RED] hibernate: writing %s failed (errno %d, synced %d)", tmp, w.err, synced); unlink(tmp); }
        free(w.tab); free(w.zbuf);
    }
    if (!gfx_closed) dm_gfx_suspend(0);
    dm_guest_unpark_all();
    g_saving = 0;
    return rc;
}
int dm_hib_save(void) { return dm_hib_save_ex(0); }
// The front ends' suspend sequence (the app leaving the screen): the snapshot is written on a thread of its own after the suspend, and the resume waits for it first
static pthread_t g_sth; static int g_sth_on;
static void *suspend_save(void *arg) { (void)arg; dm_hib_save_ex(1); return NULL; }
void dm_hib_suspend_now(int on) {
    if (!dm_hib_on()) return;
    if (on && !g_sth_on) { if (!pthread_create(&g_sth, NULL, suspend_save, NULL)) g_sth_on = 1; }
    else if (!on && g_sth_on) { pthread_join(g_sth, NULL); g_sth_on = 0; }
}

// ---- reading a snapshot back ----
// Checks a snapshot completely: header, trailer, the chunk table's CRC, and every chunk (decompressed, its CRC). Returns 0 when it is whole; fills *hdr when not NULL.
// The restore reads it the same way; DM_CTL "hib verify [path]" runs it alone
typedef struct { int fd; hib_hdr h; hib_tab *tab; uint32_t n; uint64_t size; } rd_t;
static int rd_pread(int fd, void *p, size_t n, uint64_t off) {
    uint8_t *b = p;
    while (n) { ssize_t k = pread(fd, b, n, (off_t)off); if (k <= 0) { if (k < 0 && errno == EINTR) continue; return -1; } b += k; n -= (size_t)k; off += (uint64_t)k; }
    return 0;
}
static void rd_close(rd_t *r) { if (r->fd >= 0) close(r->fd); free(r->tab); r->fd = -1; r->tab = NULL; }
static int rd_open(rd_t *r, const char *path, char *why, size_t wn) {
    memset(r, 0, sizeof *r); r->fd = open(path, O_RDONLY);
    if (r->fd < 0) { snprintf(why, wn, "cannot open %s (errno %d)", path, errno); return -1; }
    struct stat st; fstat(r->fd, &st); r->size = (uint64_t)st.st_size;
    struct { uint64_t off; uint32_t n, crc; char magic[8]; } tr;
    if (r->size < sizeof r->h + sizeof tr || rd_pread(r->fd, &r->h, sizeof r->h, 0) || rd_pread(r->fd, &tr, sizeof tr, r->size - sizeof tr)) { snprintf(why, wn, "too short"); goto bad; }
    if (memcmp(r->h.magic, "DMHIBSN1", 8) || r->h.version != HIB_VERSION || r->h.hdr_size != sizeof r->h) { snprintf(why, wn, "not a snapshot of this version"); goto bad; }
    if (memcmp(tr.magic, "DMHIBEND", 8)) { snprintf(why, wn, "no trailer (the write did not finish)"); goto bad; }
    if (tr.off + (uint64_t)tr.n * sizeof(hib_tab) + sizeof tr != r->size) { snprintf(why, wn, "chunk table out of place"); goto bad; }
    r->n = tr.n; r->tab = malloc((size_t)tr.n * sizeof *r->tab + 1);
    if (rd_pread(r->fd, r->tab, (size_t)tr.n * sizeof *r->tab, tr.off) || crc32c(0, r->tab, (size_t)tr.n * sizeof *r->tab) != tr.crc) { snprintf(why, wn, "chunk table damaged"); goto bad; }
    return 0;
bad:
    rd_close(r); return -1;
}
// Chunk i, decompressed and checked, into a buffer the caller frees; NULL when damaged
static void *rd_chunk(rd_t *r, uint32_t i, size_t *len) {
    const hib_tab *t = &r->tab[i]; hib_chunk c;
    if (rd_pread(r->fd, &c, sizeof c, t->off) || c.type != t->type || c.stored_len != t->stored_len || c.raw_len != t->raw_len) return NULL;
    void *st = malloc(c.stored_len + 1), *raw = c.flags & 1 ? malloc(c.raw_len + 1) : st;
    int ok = !rd_pread(r->fd, st, c.stored_len, t->off + sizeof c);
    if (ok && (c.flags & 1)) ok = LZ4_decompress_safe(st, raw, (int)c.stored_len, (int)c.raw_len) == (int)c.raw_len;
    if (ok) ok = crc32c(0, raw, c.raw_len) == c.crc;
    if (raw != st) free(st);
    if (!ok) { free(raw); return NULL; }
    *len = c.raw_len; return raw;
}
int dm_hib_verify(const char *path) {
    char p[1100], why[256];
    if (!path || !*path) { char dir[1024]; hib_dir(dir, sizeof dir); snprintf(p, sizeof p, "%s/snap.bin", dir); path = p; }
    rd_t r; uint64_t t0 = now_ns();
    if (rd_open(&r, path, why, sizeof why)) { LOG("[RED] hib verify %s: %s", path, why); return -1; }
    uint64_t raw = 0, mem = 0, pages = 0; int bad = -1, nthr = 0;
    for (uint32_t i = 0; i < r.n && bad < 0; i++) {
        size_t n; uint8_t *d = rd_chunk(&r, i, &n);
        if (!d) { bad = (int)i; break; }
        raw += n;
        if (r.tab[i].type == HIB_C_MEM) { uint64_t len; memcpy(&len, d, 8); mem += len; const uint8_t *bm = d + 16; for (uint64_t o = 0; o < len; o += ZPAGE) pages += bm[o / ZPAGE / 8] >> (o / ZPAGE % 8) & 1; }
        if (r.tab[i].type == HIB_C_THREADS) nthr = (int)r.tab[i].addr;
        free(d);
    }
    if (bad >= 0) LOG("[RED] hib verify %s: chunk %d of %u damaged", path, bad, r.n);
    else LOG("hib verify %s: whole: game %s, written %llu, %u chunks, %d threads, %.1f MB of memory spans (%.1f MB not zero), %.1f MB raw in %.1f MB; read in %.0f ms", path, r.h.game,
             (unsigned long long)r.h.created_unix, r.n, nthr, mem / 1048576.0, pages * ZPAGE / 1048576.0, raw / 1048576.0, r.size / 1048576.0, (now_ns() - t0) / 1e6);
    rd_close(&r);
    return bad >= 0 ? -1 : 0;
}
