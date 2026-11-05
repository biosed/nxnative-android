// Hibernation across host builds: the host's own symbol table, read at run time. A snapshot written by one build of the host is restored by another (an app update) by mapping
// every word of the snapshot that points into the host image to the same symbol in the new build (hle_hibsnap.c, section "hostrefs"). The table comes from the image itself:
// on Apple from the executable's LC_SYMTAB in its __LINKEDIT (with the debug map's stabs naming the source file of each local symbol), on Android from libdmhost.so's .symtab
// (the file the loader mapped; the APK keeps it unstripped), where STT_FILE entries name the file of the local symbols after them.
// A symbol's key is its name when that name is unique in the image, else "file|name" (file: the source's base name); a name that is still ambiguous gets no key (nothing that
// points at it can be mapped). Static locals carry clang's "function.name" names, so they are keyed too. Sizes: from the table (ELF, the stabs' N_FUN pairs), else up to the
// next symbol of the same section.
#include "dm.h"
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <mach-o/nlist.h>
#include <mach-o/stab.h>
#endif
#include "hle_hibsym.h"

static hsym *g_sym; static int g_nsym; static char *g_pool; static size_t g_pool_n, g_pool_cap;
static uint64_t g_lo, g_hi;   // the image's address range
static int g_loaded;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;

static uint32_t pool_add(const char *s) {
    size_t n = strlen(s) + 1;
    if (g_pool_n + n > g_pool_cap) { g_pool_cap = (g_pool_n + n) * 2 + 65536; g_pool = realloc(g_pool, g_pool_cap); }
    memcpy(g_pool + g_pool_n, s, n); g_pool_n += n; return (uint32_t)(g_pool_n - n);
}
static const char *base_name(const char *p) { const char *s = strrchr(p, '/'); return s ? s + 1 : p; }
static int g_cap;
static void sym_add(uint64_t a, uint64_t size, const char *name, const char *file, int kind, int sect) {
    if (!name || !*name) return;
    if (g_nsym == g_cap) { g_cap = g_cap ? g_cap * 2 : 16384; g_sym = realloc(g_sym, (size_t)g_cap * sizeof *g_sym); }
    g_sym[g_nsym++] = (hsym){a, size, pool_add(name), pool_add(file ? base_name(file) : ""), 0, (uint8_t)kind, (uint16_t)sect};
}
static int by_addr(const void *x, const void *y) { const hsym *a = x, *b = y; return a->addr < b->addr ? -1 : a->addr > b->addr; }
static int by_name(const void *x, const void *y) {
    const hsym *a = *(hsym *const *)x, *b = *(hsym *const *)y; int c = strcmp(g_pool + a->name, g_pool + b->name);
    return c ? c : strcmp(g_pool + a->file, g_pool + b->file);
}

#ifdef __APPLE__
// The debug map: file of each function / static by its address (N_SO opens a file, N_FUN pairs give a function's start and size)
typedef struct { uint64_t a, size; const char *file; } stab_t;
static int stab_cmp(const void *x, const void *y) { const stab_t *a = x, *b = y; return a->a < b->a ? -1 : a->a > b->a; }
static void load_image(void) {
    const struct mach_header_64 *h = (const struct mach_header_64 *)_dyld_get_image_header(0);
    intptr_t slide = _dyld_get_image_vmaddr_slide(0);
    const struct load_command *lc = (const void *)(h + 1);
    uint64_t le_vm = 0, le_off = 0; const struct symtab_command *st = NULL;
    int nsect = 0; uint8_t code[256] = {0};   // section number (1-based) → 1 when it holds instructions
    g_lo = UINT64_MAX; g_hi = 0;
    for (uint32_t i = 0; i < h->ncmds; i++, lc = (const void *)((const uint8_t *)lc + lc->cmdsize)) {
        if (lc->cmd == LC_SEGMENT_64) {
            const struct segment_command_64 *sg = (const void *)lc;
            if (!strcmp(sg->segname, "__LINKEDIT")) { le_vm = sg->vmaddr; le_off = sg->fileoff; }
            else if (strcmp(sg->segname, "__PAGEZERO")) { if (sg->vmaddr + slide < g_lo) g_lo = sg->vmaddr + slide; if (sg->vmaddr + sg->vmsize + slide > g_hi) g_hi = sg->vmaddr + sg->vmsize + slide; }
            const struct section_64 *sc = (const void *)(sg + 1);
            for (uint32_t k = 0; k < sg->nsects; k++) { nsect++; if (nsect < 256) code[nsect] = (sc[k].flags & S_ATTR_PURE_INSTRUCTIONS) != 0; }
        }
        if (lc->cmd == LC_SYMTAB) st = (const void *)lc;
    }
    if (!st || !le_vm) return;
    const uint8_t *le = (const uint8_t *)(uintptr_t)(le_vm + slide - le_off);
    const struct nlist_64 *nl = (const void *)(le + st->symoff); const char *str = (const char *)(le + st->stroff);
    stab_t *sb = malloc((st->nsyms + 1) * sizeof *sb); int nsb = 0; const char *file = NULL;
    for (uint32_t i = 0; i < st->nsyms; i++) {
        uint8_t t = nl[i].n_type; const char *nm = nl[i].n_un.n_strx ? str + nl[i].n_un.n_strx : "";
        if (t == N_SO) { file = *nm && nm[strlen(nm) - 1] != '/' ? nm : (*nm ? file : NULL); continue; }
        if (t == N_FUN && *nm) { uint64_t size = i + 1 < st->nsyms && nl[i + 1].n_type == N_FUN && !nl[i + 1].n_un.n_strx ? nl[i + 1].n_value : 0; sb[nsb++] = (stab_t){nl[i].n_value + slide, size, file}; }
        if ((t == N_STSYM || t == N_LCSYM) && *nm) sb[nsb++] = (stab_t){nl[i].n_value + slide, 0, file};
    }
    qsort(sb, (size_t)nsb, sizeof *sb, stab_cmp);
    for (uint32_t i = 0; i < st->nsyms; i++) {
        uint8_t t = nl[i].n_type;
        if ((t & N_STAB) || (t & N_TYPE) != N_SECT) continue;
        uint64_t a = nl[i].n_value + slide; const char *nm = str + nl[i].n_un.n_strx; if (*nm == '_') nm++;
        const char *f = NULL; uint64_t size = 0;
        int lo = 0, hi = nsb - 1; while (lo <= hi) { int m = (lo + hi) / 2; if (sb[m].a < a) lo = m + 1; else if (sb[m].a > a) hi = m - 1; else { f = sb[m].file; size = sb[m].size; break; } }
        sym_add(a, size, nm, (t & N_EXT) ? NULL : f, code[nl[i].n_sect] ? 'T' : 'D', nl[i].n_sect);
    }
    free(sb);
}
#else
// ELF: the .symtab of the file the loader mapped this image from
static void load_image(void) {
    Dl_info di; if (!dladdr((void *)load_image, &di) || !di.dli_fname || !di.dli_fbase) return;
    uint64_t base = (uint64_t)(uintptr_t)di.dli_fbase;
    int fd = open(di.dli_fname, O_RDONLY); if (fd < 0) { LOG("[RED] hibernate: symbols: cannot open %s (errno %d)", di.dli_fname, errno); return; }
    struct stat st; fstat(fd, &st);
    const uint8_t *f = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0); close(fd);
    if (f == MAP_FAILED) return;
    uint64_t shoff; uint16_t shentsize, shnum, phnum, phentsize; uint64_t phoff;
    memcpy(&phoff, f + 0x20, 8); memcpy(&shoff, f + 0x28, 8); memcpy(&phentsize, f + 0x36, 2); memcpy(&phnum, f + 0x38, 2); memcpy(&shentsize, f + 0x3a, 2); memcpy(&shnum, f + 0x3c, 2);
    g_lo = UINT64_MAX; g_hi = 0;
    for (int i = 0; i < phnum; i++) { const uint8_t *ph = f + phoff + (uint64_t)i * phentsize; uint32_t ty; uint64_t va, msz; memcpy(&ty, ph, 4); memcpy(&va, ph + 16, 8); memcpy(&msz, ph + 40, 8);
        if (ty == 1) { if (base + va < g_lo) g_lo = base + va; if (base + va + msz > g_hi) g_hi = base + va + msz; } }
    const uint8_t *sh = f + shoff; int symi = -1;
    for (int i = 0; i < shnum; i++) { uint32_t ty; memcpy(&ty, sh + (size_t)i * shentsize + 4, 4); if (ty == 2) symi = i; }
    if (symi < 0) { LOG("[RED] hibernate: symbols: %s has no .symtab (stripped)", di.dli_fname); munmap((void *)f, (size_t)st.st_size); return; }
    const uint8_t *s = sh + (size_t)symi * shentsize; uint64_t so, ssz, ent; uint32_t link; memcpy(&so, s + 24, 8); memcpy(&ssz, s + 32, 8); memcpy(&link, s + 40, 4); memcpy(&ent, s + 56, 8);
    uint64_t stro; memcpy(&stro, sh + (size_t)link * shentsize + 24, 8); const char *str = (const char *)f + stro;
    const char *file = NULL;
    for (uint64_t i = 1; i < ssz / ent; i++) {
        const uint8_t *e = f + so + i * ent; uint32_t nm; uint8_t info; uint16_t shn; uint64_t v, sz;
        memcpy(&nm, e, 4); info = e[4]; memcpy(&shn, e + 6, 2); memcpy(&v, e + 8, 8); memcpy(&sz, e + 16, 8);
        int type = info & 15, bind = info >> 4;
        if (type == 4) { file = str + nm; continue; }   // STT_FILE
        if (!shn || shn >= 0xff00 || type > 2 || !nm) continue;
        if (str[nm] == '$') continue;   // mapping symbols ($x, $d)
        uint64_t fl; memcpy(&fl, sh + (size_t)shn * shentsize + 8, 8);
        sym_add(base + v, sz, str + nm, bind == 0 ? file : NULL, (fl & 4) ? 'T' : 'D', shn);
    }
    munmap((void *)f, (size_t)st.st_size);
}
#endif

int dm_hsym_load(void) {
    pthread_mutex_lock(&g_mu);
    if (!g_loaded) {
        g_loaded = 1; uint64_t t0 = now_ns();
        load_image();
        qsort(g_sym, (size_t)g_nsym, sizeof *g_sym, by_addr);
        // sizes left open: up to the next symbol of the same section (the image's end for the last)
        for (int i = 0; i < g_nsym; i++) if (!g_sym[i].size) {
            int k = i + 1; while (k < g_nsym && (g_sym[k].addr == g_sym[i].addr || g_sym[k].sect != g_sym[i].sect)) { if (g_sym[k].sect != g_sym[i].sect && g_sym[k].addr > g_sym[i].addr) break; k++; }
            g_sym[i].size = k < g_nsym && g_sym[k].addr > g_sym[i].addr ? g_sym[k].addr - g_sym[i].addr : 0;
        }
        // keys: the name alone when unique, else file:name when that is unique, else none
        hsym **bn = malloc(((size_t)g_nsym + 1) * sizeof *bn); for (int i = 0; i < g_nsym; i++) bn[i] = &g_sym[i];
        qsort(bn, (size_t)g_nsym, sizeof *bn, by_name);
        int nunique = 0, nfile = 0, namb = 0;
        for (int i = 0; i < g_nsym;) {
            int j = i; while (j < g_nsym && !strcmp(g_pool + bn[j]->name, g_pool + bn[i]->name)) j++;
            int same_addr = 1; for (int k = i + 1; k < j; k++) same_addr &= bn[k]->addr == bn[i]->addr;   // aliases of one symbol (a local and its stab twin)
            if (j - i == 1 || same_addr) { for (int k = i; k < j; k++) bn[k]->keyed = 1; nunique++; }
            else for (int k = i; k < j; k++) {
                int dup = (k > i && !strcmp(g_pool + bn[k]->file, g_pool + bn[k - 1]->file)) || (k + 1 < j && !strcmp(g_pool + bn[k]->file, g_pool + bn[k + 1]->file));
                if (!dup && g_pool[bn[k]->file]) { bn[k]->keyed = 2; nfile++; } else namb++;
            }
            i = j;
        }
        free(bn);
        LOG("hibernate: host symbols: %d (%d by name, %d by file and name, %d ambiguous), image %#llx-%#llx, read in %.0f ms", g_nsym, nunique, nfile, namb,
            (unsigned long long)g_lo, (unsigned long long)g_hi, (now_ns() - t0) / 1e6);
    }
    pthread_mutex_unlock(&g_mu);
    return g_nsym;
}
void dm_hsym_range(uint64_t *lo, uint64_t *hi) { dm_hsym_load(); *lo = g_lo; *hi = g_hi; }
// The symbol a lies in (the last one starting at or below it whose size covers it; a symbol of size 0 covers only its own address)
const hsym *dm_hsym_at(uint64_t a) {
    dm_hsym_load();
    int lo = 0, hi = g_nsym - 1, r = -1;
    while (lo <= hi) { int m = (lo + hi) / 2; if (g_sym[m].addr <= a) { r = m; lo = m + 1; } else hi = m - 1; }
    for (int k = r; k >= 0 && k > r - 8; k--) {   // aliases at one address: the keyed one first
        const hsym *s = &g_sym[k];
        if (s->addr <= a && (a < s->addr + s->size || a == s->addr) && s->keyed) return s;
    }
    return r >= 0 && (a < g_sym[r].addr + g_sym[r].size || a == g_sym[r].addr) ? &g_sym[r] : NULL;
}
void dm_hsym_key(const hsym *s, char *out, size_t n) {
    if (!s || !s->keyed) { snprintf(out, n, "%s", ""); return; }
    if (s->keyed == 1) snprintf(out, n, "%s", g_pool + s->name); else snprintf(out, n, "%s|%s", g_pool + s->file, g_pool + s->name);
}
const char *dm_hsym_name(const hsym *s) { return g_pool + s->name; }
// The symbol with this key, or NULL
const hsym *dm_hsym_find(const char *key) {
    dm_hsym_load();
    const char *c = strchr(key, '|'); char file[256] = "", name[512];
    if (c) { snprintf(file, sizeof file, "%.*s", (int)(c - key), key); snprintf(name, sizeof name, "%s", c + 1); } else snprintf(name, sizeof name, "%s", key);
    for (int i = 0; i < g_nsym; i++) {   // linear: a restore looks up a few hundred keys at most (a name index would pay for itself only beyond that)
        const hsym *s = &g_sym[i];
        if (!s->keyed || strcmp(g_pool + s->name, name)) continue;
        if (c ? s->keyed == 2 && !strcmp(g_pool + s->file, file) : s->keyed == 1) return s;
    }
    return NULL;
}
// FNV-1a of a function's code (a return address into it maps to the same offset in another build only when the code is the same)
uint64_t dm_hsym_code_hash(const hsym *s) {
    if (!s || s->kind != 'T' || !s->size || s->size > (64u << 20)) return 0;
    // the instructions with the fields that only say where things are masked out (branch and literal offsets, ADRP pages and the :lo12: immediates that pair with them): a
    // function whose source is the same keeps its hash when the functions and data around it move in another build
    const uint32_t *p = (const uint32_t *)(uintptr_t)s->addr; uint64_t h = 0xcbf29ce484222325ull;
    for (uint64_t i = 0; i < s->size / 4; i++) {
        uint32_t x = p[i];
        if ((x & 0x7C000000u) == 0x14000000u) x &= 0xFC000000u;                // B, BL
        else if ((x & 0xFF000010u) == 0x54000000u) x &= 0xFF00001Fu;           // B.cond
        else if ((x & 0x7E000000u) == 0x34000000u) x &= 0xFF00001Fu;           // CBZ, CBNZ
        else if ((x & 0x7E000000u) == 0x36000000u) x &= 0xFFF8001Fu;           // TBZ, TBNZ
        else if ((x & 0x1F000000u) == 0x10000000u) x &= 0x9F00001Fu;           // ADR, ADRP
        else if ((x & 0x3B000000u) == 0x18000000u) x &= 0xFF00001Fu;           // LDR (literal)
        else if ((x & 0x7F800000u) == 0x11000000u) x &= 0xFFC003FFu;           // ADD (immediate): an ADRP's :lo12:
        else if ((x & 0x3B000000u) == 0x39000000u) x &= 0xFFC003FFu;           // LDR / STR (unsigned offset): an ADRP's :lo12:
        for (int k = 0; k < 4; k++) h = (h ^ (x >> (8 * k) & 0xFF)) * 0x100000001b3ull;
    }
    return h;
}
// Where the image's code starts (its first function past the header): below it lie the headers the loader read
uint64_t dm_hsym_code_start(void) {
    dm_hsym_load();
    for (int i = 0; i < g_nsym; i++) if (g_sym[i].kind == 'T' && g_sym[i].addr > g_lo) return g_sym[i].addr;
    return g_lo;
}
int dm_hsym_count(void) { dm_hsym_load(); return g_nsym; }
const hsym *dm_hsym_get(int i) { return &g_sym[i]; }
