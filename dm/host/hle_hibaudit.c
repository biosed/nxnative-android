// Hibernation pointer audit (DM_CTL "hib audit <path>", with DM_HIBERNATE=1): scans every readable byte the guest can address -- the modules' data, the host arena and the guest address
// space -- for 8-byte words that point into host memory outside the fixed layout (hle_hib.c): the host executable and libraries, malloc zones, thread stacks, other mappings. A snapshot of
// guest memory written with such a word would carry a pointer that means nothing in the next process, so each one is either a location the host can rewrite on restore, a value the host
// can translate, or a state the snapshot must refuse. The report lists, per target class, how many words, how many distinct values, and for each value its symbol (for images) and the
// first locations that hold it (module + offset, or guest region + offset), so that every class can be traced to the host code that handed it out.
// Reads go through vm_read_overwrite (Mach on Apple, process_vm_readv on Android), which fails on a page unmapped meanwhile instead of faulting: the game keeps running during the scan.
#include "dm.h"
#include <dlfcn.h>
#include <sys/mman.h>
#include <unistd.h>
#include <mach/mach.h>          // vm_read_overwrite (Android: dm/android/compat, process_vm_readv)
#ifdef __APPLE__
#include <mach/mach_vm.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <malloc/malloc.h>
#endif

void dm_hib_layout(uint8_t **win, size_t *win_len, size_t *mod_used, uint8_t **arena, size_t *arena_used);
void svc_guest_range(uint8_t **base, size_t *size);

// ---- host regions: what a value can point at ----
typedef struct { uint64_t lo, hi; int prot; char cls[64]; } hregion_t;
static hregion_t *g_reg; static int g_nreg, g_capreg;
static void reg_add(uint64_t lo, uint64_t hi, int prot, const char *cls) {
    if (g_nreg == g_capreg) { g_capreg = g_capreg ? g_capreg * 2 : 1024; g_reg = realloc(g_reg, g_capreg * sizeof *g_reg); }
    hregion_t *r = &g_reg[g_nreg++]; r->lo = lo; r->hi = hi; r->prot = prot; snprintf(r->cls, sizeof r->cls, "%s", cls);
}
static int reg_cmp(const void *a, const void *b) { const hregion_t *x = a, *y = b; return x->lo < y->lo ? -1 : x->lo > y->lo; }

#ifdef __APPLE__
static const char *tag_name(unsigned t) {
    switch (t) {
    case 1: return "malloc"; case 2: return "malloc small"; case 3: return "malloc large"; case 4: return "malloc huge"; case 7: return "malloc tiny";
    case 8: case 9: return "malloc large reuse"; case 11: return "malloc nano"; case 12: return "malloc medium"; case 13: return "malloc guard";
    case 30: return "stack"; case 21: return "IOKit"; case 32: return "shared region"; case 33: return "dyld"; case 35: return "unshared pmap";
    case 60: return "dyld malloc"; case 64: return "libdispatch"; case 66: return "Accelerate"; case 97: return "CoreUI"; case 98: return "IOAccelerator (Metal)";
    default: return NULL;
    }
}
static void regions_collect(void) {
    mach_vm_address_t a = 0; mach_vm_size_t s;
    for (;;) {
        natural_t depth = 0; struct vm_region_submap_info_64 info; mach_msg_type_number_t cnt = VM_REGION_SUBMAP_INFO_COUNT_64;
        if (mach_vm_region_recurse(mach_task_self(), &a, &s, &depth, (vm_region_recurse_info_t)&info, &cnt) != KERN_SUCCESS) break;
        char cls[64]; const char *tn = info.is_submap ? "shared region" : tag_name(info.user_tag);
        if (tn) snprintf(cls, sizeof cls, "%s", tn); else snprintf(cls, sizeof cls, "mapping tag %u", info.user_tag);
        reg_add(a, a + s, info.protection, cls);
        a += s;
    }
    // images override the region class where they lie (the executable's segments are tag 0 regions; libraries sit in the shared region)
    uint32_t n = _dyld_image_count();
    for (uint32_t i = 0; i < n; i++) {
        const struct mach_header_64 *h = (const struct mach_header_64 *)_dyld_get_image_header(i); intptr_t slide = _dyld_get_image_vmaddr_slide(i);
        const char *path = _dyld_get_image_name(i), *base = strrchr(path, '/'); base = base ? base + 1 : path;
        const struct load_command *lc = (const void *)(h + 1);
        for (uint32_t k = 0; k < h->ncmds; k++, lc = (const void *)((const uint8_t *)lc + lc->cmdsize)) {
            if (lc->cmd != LC_SEGMENT_64) continue;
            const struct segment_command_64 *sg = (const void *)lc;
            if (!sg->vmsize || !strcmp(sg->segname, "__PAGEZERO")) continue;
            char cls[64]; snprintf(cls, sizeof cls, "image %s %s%s", base, sg->segname, i == 0 && !slide ? " (fixed)" : "");
            reg_add(sg->vmaddr + slide, sg->vmaddr + slide + sg->vmsize, 0, cls);
        }
    }
}
#else
static void regions_collect(void) {
    FILE *f = fopen("/proc/self/maps", "r"); if (!f) return;
    char ln[512];
    while (fgets(ln, sizeof ln, f)) {
        unsigned long long lo, hi; char perm[8] = ""; int at = 0;
        if (sscanf(ln, "%llx-%llx %7s %*s %*s %*s %n", &lo, &hi, perm, &at) < 3) continue;
        char *name = ln + at; size_t k = strlen(name); while (k && (name[k - 1] == '\n' || name[k - 1] == ' ')) name[--k] = 0;
        const char *b = strrchr(name, '/'); b = b ? b + 1 : name;
        int prot = (perm[0] == 'r') | (perm[1] == 'w') << 1 | (perm[2] == 'x') << 2;
        char cls[64]; snprintf(cls, sizeof cls, "%s%s", *b ? b : "anonymous", *b && name[0] == '/' ? (prot & 4 ? " text" : " data") : "");
        reg_add(lo, hi, prot, cls);
    }
    fclose(f);
}
#endif
// Image segments lie inside the regions: they are a table of their own, looked up first
static hregion_t *g_img; static int g_nimg;
static const hregion_t *tab_find(const hregion_t *t, int n, uint64_t v) {
    int lo = 0, hi = n - 1;
    while (lo <= hi) { int m = (lo + hi) / 2; if (v < t[m].lo) hi = m - 1; else if (v >= t[m].hi) lo = m + 1; else return &t[m]; }
    return NULL;
}
static void regions_finish(void) {
    qsort(g_reg, g_nreg, sizeof *g_reg, reg_cmp);
    free(g_img); g_img = malloc((g_nreg + 1) * sizeof *g_img); g_nimg = 0; int n = 0;
    for (int i = 0; i < g_nreg; i++) { if (!strncmp(g_reg[i].cls, "image ", 6)) g_img[g_nimg++] = g_reg[i]; else g_reg[n++] = g_reg[i]; }
    g_nreg = n;
}
static const hregion_t *host_find(uint64_t v) { const hregion_t *r = tab_find(g_img, g_nimg, v); return r ? r : tab_find(g_reg, g_nreg, v); }

// ---- what is scanned: the readable parts of the window ----
typedef struct { uint64_t lo, hi; } span_t;
static int readable_spans(uint64_t lo, uint64_t hi, span_t *out, int max) {
    int n = 0;
#ifdef __APPLE__
    mach_vm_address_t a = lo; mach_vm_size_t s;
    while (a < hi && n < max) {
        natural_t depth = 0; struct vm_region_submap_info_64 info; mach_msg_type_number_t cnt = VM_REGION_SUBMAP_INFO_COUNT_64;
        if (mach_vm_region_recurse(mach_task_self(), &a, &s, &depth, (vm_region_recurse_info_t)&info, &cnt) != KERN_SUCCESS || a >= hi) break;
        uint64_t e = a + s < hi ? a + s : hi;
        if (info.protection & VM_PROT_READ) { if (n && out[n - 1].hi == a) out[n - 1].hi = e; else out[n++] = (span_t){a, e}; }
        a += s;
    }
#else
    for (int i = 0; i < g_nreg && n < max; i++) {   // regions_collect read /proc/self/maps just now
        uint64_t a = g_reg[i].lo > lo ? g_reg[i].lo : lo, e = g_reg[i].hi < hi ? g_reg[i].hi : hi;
        if (a < e && (g_reg[i].prot & 1)) { if (n && out[n - 1].hi == a) out[n - 1].hi = e; else out[n++] = (span_t){a, e}; }
    }
#endif
    return n;
}

// ---- guest thread stacks: words below a thread's stack pointer are dead (frames that returned), the rest of its stack is live ----
int dm_thr_stack_list(uint32_t *handle, uint64_t *sp, uint64_t *top, int *where, int max);   // hle_thread.c
int svc_map_span(uint64_t a, uint64_t *base, uint64_t *size);   // hle_svc.c
int svc_map_source_span(uint64_t a, uint64_t *base, uint64_t *size);
#define MAXT 512
static struct { uint32_t h; uint64_t sp, lo, hi; int where; } g_thr[MAXT]; static int g_nthr;
static void stacks_collect(void) {
    uint32_t h[MAXT]; uint64_t sp[MAXT], top[MAXT]; int wh[MAXT];
    int n = dm_thr_stack_list(h, sp, top, wh, MAXT); g_nthr = 0;
    for (int i = 0; i < n; i++) {
        uint64_t b = 0, z = 0;
        if (!sp[i] || !svc_map_span(sp[i], &b, &z)) { b = 0; z = 0; }
        g_thr[g_nthr].h = h[i]; g_thr[g_nthr].sp = sp[i]; g_thr[g_nthr].lo = b; g_thr[g_nthr].hi = b + z; g_thr[g_nthr].where = wh[i]; g_nthr++;
    }
}
// 1: below the stack pointer of the thread whose stack holds a (128 bytes under sp stay live: the rewriter parks registers there); 0 otherwise. *th: that thread, live or dead
static int stack_dead(uint64_t a, int *th) {
    for (int i = 0; i < g_nthr; i++)
        if (g_thr[i].hi && a >= g_thr[i].lo && a < g_thr[i].hi) { *th = i; return a + 128 < g_thr[i].sp; }
    *th = -1;
    uint64_t b, z; return svc_map_source_span(a, &b, &z);   // the stale heap copy of a moved stack: never read while the stack is mapped
}

// ---- plausibility: the scan meets game data that happens to fall in a host range (on the Mac the shared cache alone spans 0x180000000-0xfc0000000, and game data is full of
// words like 0x200000000). A word counts as a pointer only when it is what a pointer to that target would be: a function start or a return address in code, a symbol in an
// image's data, the start of a live malloc block, an aligned word in a stack or another mapping (round values excepted). Everything else is counted as noise ----
static int is_return_address(uint64_t v) {
    uint32_t ins; vm_size_t got = 0;
    if ((v & 3) || vm_read_overwrite(mach_task_self(), (vm_address_t)(v - 4), 4, (vm_address_t)&ins, &got) != KERN_SUCCESS || got != 4) return 0;
    return (ins & 0xFC000000u) == 0x94000000u || (ins & 0xFFFFFC1Fu) == 0xD63F0000u;   // BL, BLR
}
uintptr_t dm_hib_image_base(void);
static int plausible(uint64_t v, const char *cls) {
    if (!strcmp(cls, "shared region")) return 0;   // a gap of the shared region between the loaded images' segments: nothing a process can point at
    if (!strncmp(cls, "image ", 6)) {
        Dl_info di; int sym = dladdr((void *)(uintptr_t)v, &di) && di.dli_saddr && (uint64_t)(uintptr_t)di.dli_saddr == v;
        if (strstr(cls, "__TEXT")) return sym || is_return_address(v);
        return sym;
    }
#ifdef __APPLE__
    if (!strncmp(cls, "malloc", 6)) return malloc_size((void *)(uintptr_t)v) != 0;
#endif
    return !(v & 7) && (v & 0xFFFF);
}

// ---- findings ----
typedef struct { uint64_t v; const char *cls; uint64_t count, dead; uint64_t at[6]; int nat, plaus; } hit_t;
static hit_t *g_hit; static size_t g_nhit, g_caphit;   // open addressing by value
static void hit_add(uint64_t v, const char *cls, uint64_t at, int dead) {
    if (g_nhit * 2 >= g_caphit) {
        size_t cap = g_caphit ? g_caphit * 2 : 4096; hit_t *nh = calloc(cap, sizeof *nh);
        for (size_t i = 0; i < g_caphit; i++) if (g_hit[i].v) { size_t j = (g_hit[i].v * 0x9E3779B97F4A7C15ull >> 20) & (cap - 1); while (nh[j].v) j = (j + 1) & (cap - 1); nh[j] = g_hit[i]; }
        free(g_hit); g_hit = nh; g_caphit = cap;
    }
    size_t j = (v * 0x9E3779B97F4A7C15ull >> 20) & (g_caphit - 1);
    while (g_hit[j].v && g_hit[j].v != v) j = (j + 1) & (g_caphit - 1);
    hit_t *h = &g_hit[j];
    if (!h->v) { h->v = v; h->cls = cls; h->plaus = plausible(v, cls); g_nhit++; }
    if (dead) { h->dead++; return; }
    h->count++; if (h->nat < 6) h->at[h->nat++] = at;
}

// Where a scanned word lies, for the report
static void where(uint64_t a, char *out, size_t n) {
    module_t *m = module_of((void *)(uintptr_t)a);
    if (m) { snprintf(out, n, "%s+%#llx", m->name, (unsigned long long)(a - (uint64_t)(uintptr_t)m->base)); return; }
    uint8_t *gb; size_t gs; svc_guest_range(&gb, &gs);
    uint8_t *arena; size_t au; dm_hib_layout(NULL, NULL, NULL, &arena, &au);
    if (arena && a >= (uint64_t)(uintptr_t)arena && a < (uint64_t)(uintptr_t)arena + 0x10000000) { snprintf(out, n, "arena+%#llx", (unsigned long long)(a - (uint64_t)(uintptr_t)arena)); return; }
    int th; stack_dead(a, &th);
    if (gb && a >= (uint64_t)(uintptr_t)gb && a < (uint64_t)(uintptr_t)gb + gs) {
        if (th >= 0) snprintf(out, n, "guest+%#llx(stack %#x sp-%#llx)", (unsigned long long)(a - (uint64_t)(uintptr_t)gb), g_thr[th].h, (unsigned long long)(a - g_thr[th].sp));
        else snprintf(out, n, "guest+%#llx", (unsigned long long)(a - (uint64_t)(uintptr_t)gb));
        return;
    }
    snprintf(out, n, "%#llx", (unsigned long long)a);
}
static int hit_cmp(const void *a, const void *b) {
    const hit_t *x = a, *y = b; int c = strcmp(x->cls, y->cls); if (c) return c;
    return x->count < y->count ? 1 : x->count > y->count ? -1 : 0;
}

void dm_hib_audit(const char *path) {
    uint8_t *win; size_t win_len, mod_used; uint8_t *arena; size_t arena_used;
    dm_hib_layout(&win, &win_len, &mod_used, &arena, &arena_used);
    if (!win) { LOG("[RED] hib audit: no fixed layout (DM_HIBERNATE=1 is needed from the start)"); return; }
    FILE *f = fopen(path, "w"); if (!f) { LOG("[RED] hib audit: cannot write %s", path); return; }
    uint64_t t0 = now_ns();
    g_nreg = 0; regions_collect(); regions_finish(); stacks_collect();
    uint64_t wlo = (uint64_t)(uintptr_t)win, whi = wlo + win_len;
    // spans to scan: the window's readable parts, less the modules' code (text, the patch areas): instructions, not data
    static span_t sp[65536]; int ns = readable_spans(wlo, whi, sp, 65536);
    uint64_t scanned = 0, nonzero_pages = 0, pages = 0, words_hit = 0, words_dead = 0;
    size_t pg = (size_t)getpagesize(); uint8_t *buf = malloc(1 << 20);
    for (int i = 0; i < ns; i++)
        for (uint64_t a = sp[i].lo; a < sp[i].hi; ) {
            uint64_t len = sp[i].hi - a < (1u << 20) ? sp[i].hi - a : (1u << 20);
            vm_size_t got = 0;
            if (vm_read_overwrite(mach_task_self(), (vm_address_t)a, (vm_size_t)len, (vm_address_t)buf, &got) != KERN_SUCCESS || got != len) { a += len; continue; }
            for (uint64_t off = 0; off < len; off += pg) {
                uint64_t pa = a + off; module_t *m = module_of((void *)(uintptr_t)pa);
                if (m) {   // code: [0, text_end) and the patch areas
                    uint64_t mo = pa - (uint64_t)(uintptr_t)m->base;
                    if (pa < (uint64_t)(uintptr_t)m->base || mo < m->text_end || (m->patch_off >= 0 && mo >= (uint64_t)m->patch_off) || (m->back_len && mo >= m->back_off && mo < m->back_off + m->back_len)) continue;
                }
                pages++;
                const uint64_t *w = (const uint64_t *)(buf + off); int nz = 0;
                for (size_t k = 0; k < pg / 8; k++) {
                    uint64_t v = w[k]; if (!v) continue; nz = 1;
                    if (v < 0x100000000ull || (v >= wlo && v < whi)) continue;   // small numbers and the fixed layout itself
                    const hregion_t *r = host_find(v); if (!r || (r->prot == 0 && strncmp(r->cls, "image ", 6))) continue;   // nothing valid points into an inaccessible region
                    int th, dead = stack_dead(pa + k * 8, &th);
                    hit_add(v, r->cls, pa + k * 8, dead); if (dead) words_dead++; else words_hit++;
                }
                nonzero_pages += nz;
            }
            scanned += len; a += len;
        }
    free(buf);
    // report
    hit_t *list = malloc((g_nhit + 1) * sizeof *list); size_t nl = 0;
    for (size_t i = 0; i < g_caphit; i++) if (g_hit[i].v && g_hit[i].count) list[nl++] = g_hit[i];
    qsort(list, nl, sizeof *list, hit_cmp);
    fprintf(f, "# hib audit: window %#llx +%#zx, modules %#zx, arena %#zx used; %d readable spans, %.1f MB scanned in %.2f s; %llu pages of data, %llu of them not all zero (%.1f MB)\n",
            (unsigned long long)wlo, win_len, mod_used, arena_used, ns, scanned / 1048576.0, (now_ns() - t0) / 1e9, (unsigned long long)pages, (unsigned long long)nonzero_pages, nonzero_pages * pg / 1048576.0);
    fprintf(f, "# %llu live words point outside the layout, %zu distinct values; %llu more lie below a thread's stack pointer (dead frames)\n", (unsigned long long)words_hit, nl, (unsigned long long)words_dead);
    for (int i = 0; i < g_nthr; i++) fprintf(f, "# thread %#x sp %#llx (%s) stack mapping %#llx-%#llx\n", g_thr[i].h, (unsigned long long)g_thr[i].sp,
                                              g_thr[i].where == 1 ? "in an SVC" : g_thr[i].where == 2 ? "guest code" : g_thr[i].where == 3 ? "host code" : "unknown", (unsigned long long)g_thr[i].lo, (unsigned long long)g_thr[i].hi);
    // per class totals: plausible pointers, then the noise
    for (size_t i = 0; i < nl; ) {
        size_t j = i; uint64_t c[2] = {0, 0}, nv[2] = {0, 0}; while (j < nl && !strcmp(list[j].cls, list[i].cls)) { c[list[j].plaus] += list[j].count; nv[list[j].plaus]++; j++; }
        if (c[1]) fprintf(f, "class\t%s\t%llu words\t%llu values\n", list[i].cls, (unsigned long long)c[1], (unsigned long long)nv[1]);
        i = j;
    }
    uint64_t noise = 0, noisev = 0; for (size_t i = 0; i < nl; i++) if (!list[i].plaus) { noise += list[i].count; noisev++; }
    fprintf(f, "# noise (not what a pointer to its target would be): %llu words, %llu values (listed after the plausible ones, marked noise)\n", (unsigned long long)noise, (unsigned long long)noisev);
    for (int pass = 1; pass >= 0; pass--)
    for (size_t i = 0; i < nl; i++) {
        hit_t *h = &list[i]; if (h->plaus != pass) continue;
        Dl_info di; char sym[160] = "";
        if (!strncmp(h->cls, "image ", 6) && dladdr((void *)(uintptr_t)h->v, &di) && di.dli_fbase) {
            const char *b = di.dli_fname ? strrchr(di.dli_fname, '/') : NULL;
            snprintf(sym, sizeof sym, "%s+%#llx %s+%#llx", b ? b + 1 : "?", (unsigned long long)(h->v - (uint64_t)(uintptr_t)di.dli_fbase),
                     di.dli_sname ? di.dli_sname : "?", di.dli_saddr ? (unsigned long long)(h->v - (uint64_t)(uintptr_t)di.dli_saddr) : 0ull);
        }
#ifdef __APPLE__
        if (!strncmp(h->cls, "malloc", 6)) { size_t z = malloc_size((void *)(uintptr_t)h->v); if (z) snprintf(sym, sizeof sym, "malloc block of %zu bytes", z); }
#endif
        fprintf(f, "%s%s\t%#llx\t%llu\t%s\t", pass ? "" : "noise ", h->cls, (unsigned long long)h->v, (unsigned long long)h->count, sym);
        for (int k = 0; k < h->nat; k++) { char w[96]; where(h->at[k], w, sizeof w); fprintf(f, "%s%s", k ? " " : "", w); }
        fputc('\n', f);
    }
    fclose(f);
    uint64_t pw = 0, pv = 0; for (size_t i = 0; i < nl; i++) if (list[i].plaus) { pw += list[i].count; pv++; }
    LOG("hib audit: %.1f MB scanned, %llu live words in host ranges (%zu values), %llu of them plausible pointers (%llu values), %llu dead → %s", scanned / 1048576.0,
        (unsigned long long)words_hit, nl, (unsigned long long)pw, (unsigned long long)pv, (unsigned long long)words_dead, path);
    free(list); free(g_hit); g_hit = NULL; g_nhit = g_caphit = 0;
}
