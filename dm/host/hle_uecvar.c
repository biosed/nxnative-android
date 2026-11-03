// Unreal console variables found by their registered names, and the texture streaming pool the profile's ue_texture_pool raises.
// The rewriter finds each TAutoConsoleVariable the host may write in main (dm/tools/ue_cvar.py: its static constructor's registration, in any Unreal 4 / 5 build) and names it in main's
// fixups (UE_CVAR:<name>); the object holds at +0x10 the pointer to the variable's value, the game thread's copy, then the render thread's. Called on every present (hle_nvn.c)
#include "hle.h"
#include <mach/mach.h>
#include <TargetConditionals.h>

static int rd(uint64_t a, void *out, size_t n) { vm_size_t got = 0; return a >= 0x1000 && vm_read_overwrite(mach_task_self(), (vm_address_t)a, n, (vm_address_t)out, &got) == KERN_SUCCESS && got == n; }

// main's offset of the TAutoConsoleVariable registered under `name`, 0 if the rewriter found none (not an Unreal main, not one of ue_cvar.py's NAMES, or registered at two objects)
uint64_t dm_ue_cvar(const char *name) {
    for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main"))
        for (int k = 0; k < g_mod[i].n_ue_cvar; k++) if (!strcmp(g_mod[i].ue_cvar[k].name, name)) return g_mod[i].ue_cvar[k].off;
    return 0;
}

// ue_texture_pool: r.Streaming.PoolSize, the megabytes Unreal's texture streamer keeps resident; past it the streamer drops the top mips of the textures in view. The game's
// device profile sets it (DRAGON QUEST VII Reimagined: 200 on the Switch, 1000 on Windows) and the streaming manager reads it again whenever it changes. The host raises a smaller
// value the game sets to the target, whenever the game sets it back; a larger one, or -1 (no pool size: the engine sizes its pool otherwise), stays. The profile key applies on the
// Mac; on iOS only DM_UE_TEXTURE_POOL turns it on (Documents/env.txt), until an iPhone run shows the memory fits. DM_UE_TEXTURE_POOL=0 leaves the game's value
// ---- Unreal's dynamic resolution (DM_UE_DRS) ----
// A Switch build's dynamic resolution budgets its frame for 30 fps (r.DynamicRes.FrameTimeBudget 33.3 ms): with the frame limit raised to 60 (hle_fpslock.c), every millisecond
// the host saved went to resolution and the frame settled near 33 ms again (FINAL FANTASY RESONANCE on the Xiaomi Pad 8: the scene at 1040x584, then 1384x780 once the GPU got
// cheaper, ~30 fps either way). DM_UE_DRS=budget16 sets the budget to 16.67 ms (the game's bounds stay); pinned[:<pct>] keeps the screen percentage at <pct> (default 75) by
// setting the dynamic resolution's minimum and maximum to it; off leaves the game's values. Default: budget16 when the frame target is 60 (game profile fps_target or
// DM_FPS_TARGET), else off. The variables are found by the rewriter (ue_cvar.py) and written whenever the game sets them back; every 600 presents their values are logged.
// A float variable's value holds the game thread's copy, then the render thread's; OperationMode is an int
static void ue_drs_tick(void) {
    static int state = -1, mode; static float want_budget, want_pct; static uint64_t o_mode, o_budget, o_min, o_max, o_sp, n; static int nlog;
    if (!state) return;
    if (state < 0) {
        state = 0;
        const char *e = getenv("DM_UE_DRS"), *ft = getenv("DM_FPS_TARGET"); int fps = ft ? atoi(ft) : DM_GAME_FPS_TARGET;
        o_mode = dm_ue_cvar("r.DynamicRes.OperationMode"); o_budget = dm_ue_cvar("r.DynamicRes.FrameTimeBudget"); o_min = dm_ue_cvar("r.DynamicRes.MinScreenPercentage");
        o_max = dm_ue_cvar("r.DynamicRes.MaxScreenPercentage"); o_sp = dm_ue_cvar("r.ScreenPercentage");
        if (!o_budget && !o_min && !o_max) { if (e) LOG("[RED] main: Unreal's dynamic resolution variables not found by the rewriter (ue_cvar.py): DM_UE_DRS=%s not applied", e); return; }
        mode = e ? (!strcmp(e, "off") ? 0 : !strcmp(e, "budget16") ? 1 : !strncmp(e, "pinned", 6) ? 2 : -1) : fps == 60 ? 1 : 0;
        if (mode < 0) { LOG("[RED] DM_UE_DRS=%s: off, budget16 or pinned[:<pct>]; leaving the game's dynamic resolution", e); mode = 0; }
        want_budget = 1000.0f / 60.0f; want_pct = 75.0f;
        if (mode == 2 && e[6] == ':') { float v = (float)atof(e + 7); if (v >= 25.0f && v <= 100.0f) want_pct = v; else LOG("[RED] DM_UE_DRS=%s: the percentage must be 25-100; using 75", e); }
        LOG("main: Unreal dynamic resolution: %s (%s)", mode == 1 ? "budget 16.67 ms, the game's bounds" : mode == 2 ? "pinned" : "the game's own", e ? "DM_UE_DRS" : "frame target");
        if (mode == 2) LOG("main: Unreal dynamic resolution pinned at %.0f%% (minimum = maximum)", want_pct);
        state = 1;
    }
    uint8_t *base = NULL;
    for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) base = g_mod[i].base;
    if (!base) return;
    n++;
    #define VALP(off) ({ uint64_t v_ = 0; (off) && rd((uint64_t)(uintptr_t)(base + (off) + 0x10), &v_, sizeof v_) ? v_ : 0; })
    uint64_t vmode = VALP(o_mode), vbud = VALP(o_budget), vmin = VALP(o_min), vmax = VALP(o_max), vsp = VALP(o_sp);
    #undef VALP
    float fw[2] = {0}; int nw = 0;
    #define SETF(val, want, what) do { if (val) for (int c_ = 0; c_ < 2; c_++) { float *p_ = (float *)(uintptr_t)((val) + 4 * (uint64_t)c_), cur_; \
        if (!rd((uint64_t)(uintptr_t)p_, &cur_, sizeof cur_) || cur_ == (want)) continue; dm_guest_write_prepare(p_, sizeof *p_); __atomic_store_n((uint32_t *)p_, *(uint32_t *)&(float){want}, __ATOMIC_RELAXED); \
        if (nlog < 12) { nlog++; LOG("main: Unreal %s (%s thread) %g -> %g (DM_UE_DRS)", what, c_ ? "render" : "game", cur_, (double)(want)); } } } while (0)
    if (mode == 1) SETF(vbud, want_budget, "r.DynamicRes.FrameTimeBudget");
    if (mode == 2) { SETF(vmin, want_pct, "r.DynamicRes.MinScreenPercentage"); SETF(vmax, want_pct, "r.DynamicRes.MaxScreenPercentage"); }
    #undef SETF
    (void)fw; (void)nw;
    if (n % 600 == 1) {   // what the game runs with now
        float b = 0, mn = 0, mx = 0, sp = 0; int32_t om = -1;
        if (vbud) rd(vbud, &b, 4); if (vmin) rd(vmin, &mn, 4); if (vmax) rd(vmax, &mx, 4); if (vsp) rd(vsp, &sp, 4); if (vmode) rd(vmode, &om, 4);
        LOG("main: Unreal dynamic resolution now: OperationMode %d, FrameTimeBudget %g ms, Min/MaxScreenPercentage %g / %g, r.ScreenPercentage %g", om, b, mn, mx, sp);
    }
}
// ---- render feature cvars (DM_UE_PERF / DM_UE_CVARS) ----
// FINAL FANTASY RESONANCE runs Unreal's desktop deferred renderer from its Switch build; a performance profile turns its costliest features down by writing their console
// variables (found by the rewriter, ue_cvar.py) whenever the game sets them back. DM_UE_PERF=1: what changes no structure of the frame (screen-space reflections off, ambient
// occlusion one level, motion blur, lens flares and colour fringe off, subsurface and refraction at their lowest, no particle or light-function lights, two shadow cascades of
// at most 1024, other shadows at 512); =2: also ambient occlusion and contact shadows off, shadows to 60% of their distance, bloom and eye adaptation at their lowest, 70% view
// distance, half the particle spawn rate, textures one mip down. DM_UE_CVARS=<name>=<value>[,...] sets any found variable (a value with a '.' is a float, else an int), after
// the profile's. Each change is logged (at most 40 lines); the values are written into the game thread's and the render thread's copies
typedef struct { const char *name; int is_float; double v; } ue_set;
static const ue_set k_perf1[] = {
    {"r.SSR.Quality", 0, 0}, {"r.AmbientOcclusionLevels", 0, 1}, {"r.MotionBlurQuality", 0, 0}, {"r.LensFlareQuality", 0, 0}, {"r.SceneColorFringeQuality", 0, 0},
    {"r.SSS.Quality", 0, 0}, {"r.RefractionQuality", 0, 0}, {"r.ParticleLightQuality", 0, 0}, {"r.LightFunctionQuality", 0, 0},
    {"r.Shadow.CSM.MaxCascades", 0, 2}, {"r.Shadow.MaxCSMResolution", 0, 1024}, {"r.Shadow.MaxResolution", 0, 512},
};
static const ue_set k_perf2[] = {
    {"r.AmbientOcclusionLevels", 0, 0}, {"r.ContactShadows", 0, 0}, {"r.Shadow.DistanceScale", 1, 0.6}, {"r.BloomQuality", 0, 1}, {"r.EyeAdaptationQuality", 0, 0},
    {"r.ViewDistanceScale", 1, 0.7}, {"r.EmitterSpawnRateScale", 1, 0.5}, {"r.Streaming.MipBias", 1, 1.0},
};
static void ue_perf_tick(uint8_t *base) {
    static int state = -1, nset, nlog; static struct { uint64_t off; int is_float; double v; const char *name; } set[64];
    if (!state) return;
    if (state < 0) {
        state = 0;
        const char *pe = getenv("DM_UE_PERF"), *ce = getenv("DM_UE_CVARS"); int lvl = pe ? atoi(pe) : 0;
        if (!lvl && !(ce && *ce)) return;
        #define ADD(nm, fl, val) do { uint64_t o_ = dm_ue_cvar(nm); int k_ = 0; while (k_ < nset && strcmp(set[k_].name, nm)) k_++; \
            if (!o_) { LOG("[RED] main: Unreal %s not found by the rewriter (ue_cvar.py): not set", nm); break; } \
            if (k_ == nset) { if (nset == 64) break; nset++; } set[k_].off = o_; set[k_].is_float = (fl); set[k_].v = (val); set[k_].name = (nm); } while (0)
        if (lvl >= 1) for (size_t i = 0; i < sizeof k_perf1 / sizeof *k_perf1; i++) ADD(k_perf1[i].name, k_perf1[i].is_float, k_perf1[i].v);
        if (lvl >= 2) for (size_t i = 0; i < sizeof k_perf2 / sizeof *k_perf2; i++) ADD(k_perf2[i].name, k_perf2[i].is_float, k_perf2[i].v);
        if (ce && *ce) {   // name=value[,name=value...]: the names are kept for the session (strdup)
            char *dup = strdup(ce), *save = NULL;
            for (char *t = strtok_r(dup, ",; ", &save); t; t = strtok_r(NULL, ",; ", &save)) {
                char *eq = strchr(t, '='); if (!eq) { LOG("[RED] DM_UE_CVARS: %s has no value", t); continue; }
                *eq = 0; const char *nm = t, *vs = eq + 1; ADD(nm, strchr(vs, '.') != NULL, atof(vs));
            }
        }
        #undef ADD
        LOG("main: Unreal render settings: performance profile %d, %d variables set (DM_UE_PERF / DM_UE_CVARS)", lvl, nset);
        state = nset > 0;
    }
    for (int k = 0; k < nset; k++) {
        uint64_t val; if (!rd((uint64_t)(uintptr_t)(base + set[k].off + 0x10), &val, sizeof val) || !val) continue;   // not created yet
        for (int c = 0; c < 2; c++) {
            uint32_t *p = (uint32_t *)(uintptr_t)(val + 4 * (uint64_t)c), cur, want;
            if (set[k].is_float) { float f = (float)set[k].v; memcpy(&want, &f, 4); } else { int32_t i = (int32_t)set[k].v; memcpy(&want, &i, 4); }
            if (!rd((uint64_t)(uintptr_t)p, &cur, 4) || cur == want) continue;
            dm_guest_write_prepare(p, 4); __atomic_store_n(p, want, __ATOMIC_RELAXED);
            if (nlog < 40) { nlog++; float cf; int32_t ci; memcpy(&cf, &cur, 4); memcpy(&ci, &cur, 4);
                if (set[k].is_float) LOG("main: Unreal %s (%s thread) %g -> %g", set[k].name, c ? "render" : "game", cf, set[k].v);
                else LOG("main: Unreal %s (%s thread) %d -> %d", set[k].name, c ? "render" : "game", ci, (int)set[k].v); }
        }
    }
}
// DM_UE_TEXTURE_POOL=<MB> raises a smaller pool to <MB> (as the profile's ue_texture_pool); =-<MB> caps a larger one at <MB> (a tablet short of memory: the streamer then keeps fewer
// top mips, in guest memory and in the GPU's copies of them). The value the game runs with is logged every 600 presents either way, when the rewriter found the variable
void dm_ue_cvar_tick(void) {
    ue_drs_tick();
    { uint8_t *b = NULL; for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) b = g_mod[i].base; if (b) ue_perf_tick(b); }
    static int state = -1, target, cap, nlog; static uint64_t obj_off, n; static const char *src;
    if (!state) return;
    if (state < 0) {
        state = 0;
        const char *e = getenv("DM_UE_TEXTURE_POOL");
        target = e ? atoi(e) : TARGET_OS_IPHONE ? 0 : DM_GAME_UE_TEXTURE_POOL; src = e ? "DM_UE_TEXTURE_POOL" : "game profile ue_texture_pool";
        cap = target < 0; if (cap) target = -target;
        obj_off = dm_ue_cvar("r.Streaming.PoolSize");
        if (!obj_off) { if (target > 0) LOG("[RED] main: Unreal r.Streaming.PoolSize not found in main by the rewriter (ue_cvar.py; %s %d MB not applied)", src, target); return; }
        if (target <= 0) LOG("main: Unreal r.Streaming.PoolSize at main+%#llx left as the game sets it (%s)", (unsigned long long)obj_off, e ? src : DM_GAME_UE_TEXTURE_POOL ? "iOS without DM_UE_TEXTURE_POOL" : "no profile value");
        else LOG("main: Unreal r.Streaming.PoolSize at main+%#llx, %s %d MB (%s)", (unsigned long long)obj_off, cap ? "capped at" : "raised to", target, src);
        state = 1;
    }
    uint8_t *base = NULL;
    for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) base = g_mod[i].base;
    uint64_t val;
    if (!base || !rd((uint64_t)(uintptr_t)(base + obj_off + 0x10), &val, sizeof val) || !val) return;   // not created yet
    if (n++ % 600 == 0) { int32_t g = 0, r = 0; rd(val, &g, 4); rd(val + 4, &r, 4); LOG("main: Unreal r.Streaming.PoolSize now %d MB (game thread), %d MB (render thread)", g, r); }
    if (target <= 0) return;
    for (int c = 0; c < 2; c++) {
        int32_t cur, *p = (int32_t *)(uintptr_t)(val + 4 * (uint64_t)c);
        if (!rd((uint64_t)(uintptr_t)p, &cur, sizeof cur) || cur <= 0 || (cap ? cur <= target : cur >= target)) continue;
        dm_guest_write_prepare(p, sizeof *p); __atomic_store_n(p, target, __ATOMIC_RELAXED);
        if (nlog < 8) { nlog++; LOG("main: Unreal r.Streaming.PoolSize (%s thread) %d → %d MB (%s)", c ? "render" : "game", cur, target, src); }
    }
}
