// Heat and energy, measured off the game's threads (vk19.59, improvement plan v4 Build 1 "T0"): a sampling thread "dm-stats" that wakes once a second for the thermal
// headroom and the battery, and at the end of each 600-frame window (vk_stats_window, from the present) for the per-thread CPU time of the whole process, the CPU
// clusters' clock (time_in_state) and limits (scaling_max_freq), and the size of the process's memory map. It logs two lines per window:
//   vk: CPU per frame over the last N frames (T s): X ms in all threads (a second, cores busy); RHIThread / the host's dm threads / the game's, per frame and a second; the busiest 8
//   vk: heat and energy over the last N frames (T s): clusters <cpus> avg MHz (limit MHz) ...; thermal status s, headroom now h0 (lowest l0), in 10 s h10 (highest seen m10);
//       battery -Q uAh (q mAh/h), current mean I mA, V mV, plugged p; memory map L lines
// The thermal headroom is asked at most once a second (Android answers NaN to faster calls): the present value and the 10-second forecast on alternate seconds. The battery
// comes from the front end over JNI (dm_front_battery, BatteryManager: CHARGE_COUNTER and CURRENT_NOW every second, the voltage and plug state from the sticky
// ACTION_BATTERY_CHANGED intent once a window). Nothing here touches the game's threads or the backend's state: the window request is one atomic store and a signal
#include "dm.h"
#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

int dm_front_battery(long *charge_uah, long *current_ua, int *mv, int *plugged, int want_v) __attribute__((weak));
int dm_front_panel_mhz_now(void) __attribute__((weak));   // the panel's refresh rate now (Display.getRefreshRate over JNI, this thread only; vk19.63)

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;
static uint64_t g_req_frames;   // frames at the window's end (the present's count), 0: none asked
static uint64_t g_start_frames;   // the present's count when the thread was started
static uint64_t mono_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }
static int read_small(const char *p, char *b, int n) { int fd = open(p, O_RDONLY | O_CLOEXEC); if (fd < 0) return -1; int k = (int)read(fd, b, (size_t)n - 1); close(fd); if (k < 0) return -1; b[k] = 0; return k; }

// Per-thread CPU time: /proc/self/task/<tid>/schedstat, its first field (ns on a CPU). Threads are kept by tid with their last value; a thread gone since leaves its row
enum { NTH = 512 };
static struct { int tid; uint64_t ns, d; char comm[16]; int seen; } g_th[NTH]; static int g_nth;
static uint64_t cpu_window(void) {   // every thread's time since the last call (g_th[].d); returns the sum
    DIR *d = opendir("/proc/self/task"); if (!d) return 0;
    for (int i = 0; i < g_nth; i++) { g_th[i].seen = 0; g_th[i].d = 0; }
    struct dirent *e; uint64_t sum = 0; char p[64], b[128];
    while ((e = readdir(d))) {
        int tid = atoi(e->d_name); if (tid <= 0) continue;
        snprintf(p, sizeof p, "/proc/self/task/%d/schedstat", tid); if (read_small(p, b, sizeof b) <= 0) continue;
        uint64_t ns = strtoull(b, NULL, 10); int k = 0;
        for (; k < g_nth; k++) if (g_th[k].tid == tid) break;
        if (k == g_nth) { if (g_nth == NTH) continue; k = g_nth++; g_th[k].tid = tid; g_th[k].ns = 0; }   // new since the last window: all of its time counts (it started inside it, or this is the baseline)
        snprintf(p, sizeof p, "/proc/self/task/%d/comm", tid);   // each window: the guest's threads are named after they start
        if (read_small(p, g_th[k].comm, sizeof g_th[k].comm) > 0) g_th[k].comm[strcspn(g_th[k].comm, "\n")] = 0; else strcpy(g_th[k].comm, "?");
        g_th[k].d = ns >= g_th[k].ns ? ns - g_th[k].ns : 0; g_th[k].ns = ns; g_th[k].seen = 1; sum += g_th[k].d;
    }
    closedir(d);
    for (int i = 0; i < g_nth;) if (!g_th[i].seen) g_th[i] = g_th[--g_nth]; else i++;   // ended threads: their time in the window up to their end is lost (small)
    return sum;
}
static void busiest(char *out, size_t n, double fr) {   // the 8 busiest threads of the window, ms a frame
    int idx[8], ni = 0; size_t o = 0; out[0] = 0;
    for (int r = 0; r < 8; r++) { int best = -1; for (int i = 0; i < g_nth; i++) { int used = 0; for (int q = 0; q < ni; q++) used |= idx[q] == i; if (!used && g_th[i].d && (best < 0 || g_th[i].d > g_th[best].d)) best = i; } if (best < 0) break; idx[ni++] = best; }
    for (int q = 0; q < ni && o < n; q++) o += (size_t)snprintf(out + o, n - o, "%s%s (%d) %.2f", q ? ", " : "", g_th[idx[q]].comm, g_th[idx[q]].tid, g_th[idx[q]].d / 1e6 / fr);
}

// CPU clusters: cpufreq policies, their average clock over the window (time_in_state: "<kHz> <10 ms units>" lines) and their current limit
enum { NPOL = 8, NFREQ = 64 };
static struct { int cpu; uint64_t f[NFREQ], t[NFREQ]; int nf; } g_pol[NPOL]; static int g_npol = -1;
static void clusters(char *out, size_t n) {
    if (g_npol < 0) { g_npol = 0; for (int c = 0; c < 16 && g_npol < NPOL; c++) { char p[96]; snprintf(p, sizeof p, "/sys/devices/system/cpu/cpufreq/policy%d", c); if (!access(p, F_OK)) g_pol[g_npol++].cpu = c; } }
    size_t o = 0; out[0] = 0;
    for (int i = 0; i < g_npol && o < n; i++) {
        char p[128], b[4096]; uint64_t sum_ft = 0, sum_t = 0;
        snprintf(p, sizeof p, "/sys/devices/system/cpu/cpufreq/policy%d/stats/time_in_state", g_pol[i].cpu);
        int ok = read_small(p, b, sizeof b) > 0;
        if (ok) { char *s = b; int k = 0;
            while (*s && k < NFREQ) { char *e; uint64_t f = strtoull(s, &e, 10); if (e == s) break; uint64_t t = strtoull(e, &s, 10);
                uint64_t prev = k < g_pol[i].nf && g_pol[i].f[k] == f ? g_pol[i].t[k] : 0, dt = t >= prev ? t - prev : 0;
                sum_ft += f / 1000 * dt; sum_t += dt; g_pol[i].f[k] = f; g_pol[i].t[k] = t; k++; while (*s == '\n' || *s == ' ') s++; }
            g_pol[i].nf = k; }
        snprintf(p, sizeof p, "/sys/devices/system/cpu/cpufreq/policy%d/scaling_max_freq", g_pol[i].cpu);
        long lim = read_small(p, b, 64) > 0 ? atol(b) / 1000 : -1;
        if (ok && sum_t) o += (size_t)snprintf(out + o, n - o, "%scpu%d+ %llu MHz (limit %ld)", i ? ", " : "", g_pol[i].cpu, (unsigned long long)(sum_ft / sum_t), lim);
        else o += (size_t)snprintf(out + o, n - o, "%scpu%d+ %s (limit %ld)", i ? ", " : "", g_pol[i].cpu, ok ? "idle" : "no time_in_state", lim);
    }
}
static int maps_lines(void) {
    int fd = open("/proc/self/maps", O_RDONLY | O_CLOEXEC); if (fd < 0) return -1;
    static char b[65536]; int n = 0; ssize_t k;
    while ((k = read(fd, b, sizeof b)) > 0) for (ssize_t i = 0; i < k; i++) n += b[i] == '\n';
    close(fd); return n;
}

// The once-a-second samples
static void *g_tm; static int (*g_tst)(void *); static float (*g_thr)(void *, int);
static struct { float h0, h0_low, h10, h10_high; int st, st_high; long q0, q_last; int nq; double cur_sum; int ncur; } W;
static void thermal_init(void) {
    void *h = dlopen("libandroid.so", RTLD_NOW); void *(*acq)(void) = h ? (void *(*)(void))dlsym(h, "AThermal_acquireManager") : NULL;
    g_tst = h ? (int (*)(void *))dlsym(h, "AThermal_getCurrentThermalStatus") : NULL; g_thr = h ? (float (*)(void *, int))dlsym(h, "AThermal_getThermalHeadroom") : NULL;
    g_tm = acq ? acq() : NULL;
}
static void window_reset(void) { W.h0_low = INFINITY; W.h10_high = -INFINITY; W.st_high = -1; W.q0 = W.q_last; W.nq = 0; W.cur_sum = 0; W.ncur = 0; }
// The battery's units and sign, decided once from readings (vk19.59c): vendors report CURRENT_NOW in uA or mA and CHARGE_COUNTER in uAh or mAh, and the sign while
// discharging differs. A current under 50,000 in magnitude is taken as mA (a tablet draws hundreds to thousands of mA); a charge counter under 200,000 as mAh. While
// unplugged the drain is the magnitude whatever the sign; the sign seen then is logged once
static int g_cur_ma = -1, g_q_mah = -1, g_plugged = -1, g_sign_logged;
static double cur_ua(long i) { return g_cur_ma == 1 ? i * 1000.0 : (double)i; }
static long q_uah(long q) { return g_q_mah == 1 ? q * 1000 : q; }
static void second_tick(uint64_t k) {
    if (g_tm && g_thr) { float v = g_thr(g_tm, k & 1 ? 10 : 0);   // one call a second (Android answers NaN to faster ones): now and the 10 s forecast on alternate seconds
        if (!isnan(v)) { if (k & 1) { W.h10 = v; if (v > W.h10_high) W.h10_high = v; } else { W.h0 = v; if (v < W.h0_low) W.h0_low = v; } } }
    if (g_tm && g_tst) { W.st = g_tst(g_tm); if (W.st > W.st_high) W.st_high = W.st; }
    if (dm_front_battery) { long q = 0, i = 0; int mv = -1, pl = -1;
        if (dm_front_battery(&q, &i, &mv, &pl, g_plugged < 0)) {   // the plug state with the first reading, then once a window
            if (pl >= 0) g_plugged = pl;
            if (g_cur_ma < 0 && i) g_cur_ma = labs(i) < 50000; if (g_q_mah < 0 && q > 0) g_q_mah = q < 200000;
            if (!g_sign_logged && g_plugged == 0 && i && g_cur_ma >= 0) { g_sign_logged = 1;
                LOG("vk: battery readings: current in %s, %s while discharging (%ld); charge counter in %s (%ld)", g_cur_ma ? "mA" : "uA", i < 0 ? "negative" : "positive", i, g_q_mah == 1 ? "mAh" : "uAh", q); }
            q = q_uah(q); if (!W.nq++ && W.q0 == 0) W.q0 = q; W.q_last = q;
            double c = cur_ua(i); W.cur_sum += g_plugged == 0 ? fabs(c) : c; W.ncur++; } }   // unplugged: the drain, whatever the sign
}
// Thread groups (vk19.59c): the recording thread (RHIThread, the critical path), the host's own threads (dm-*, dm hitch…), and the game's (every other thread of the process)
static void groups(uint64_t g[3]) {
    g[0] = g[1] = g[2] = 0;
    for (int i = 0; i < g_nth; i++) { const char *c = g_th[i].comm; int k = !strcmp(c, "RHIThread") ? 0 : !strncmp(c, "dm-", 3) || !strncmp(c, "dm ", 3) ? 1 : 2; g[k] += g_th[i].d; }
}
static void window_log(uint64_t frames, uint64_t dns) {
    double fr = frames ? (double)frames : 1, secs = dns / 1e9, ps = secs > 0 ? secs : 1;
    uint64_t sum = cpu_window(), g[3]; groups(g); char top[1024]; busiest(top, sizeof top, fr);
    LOG("vk: CPU per frame over the last %llu frames (%.1f s): %.2f ms in all threads (%.0f ms a second, %.2f cores busy); RHIThread %.2f (%.0f a second), the host's dm threads %.2f (%.0f), the game's %.2f (%.0f); the busiest: %s",
        (unsigned long long)frames, secs, sum / 1e6 / fr, sum / 1e6 / ps, sum / 1e9 / ps, g[0] / 1e6 / fr, g[0] / 1e6 / ps, g[1] / 1e6 / fr, g[1] / 1e6 / ps, g[2] / 1e6 / fr, g[2] / 1e6 / ps, top);
    char cl[512]; clusters(cl, sizeof cl);
    int mv = -1, pl = -1; long q = 0, cu = 0; if (dm_front_battery && dm_front_battery(&q, &cu, &mv, &pl, 1) && pl >= 0) g_plugged = pl;
    long dq = W.nq ? W.q_last - W.q0 : 0; double mean_ma = W.ncur ? W.cur_sum / W.ncur / 1000.0 : 0;
    LOG("vk: heat and energy over the last %llu frames (%.1f s): clusters %s; thermal status %d (highest %d), headroom now %.3f (lowest %.3f), in 10 s %.3f (highest %.3f); battery %+ld uAh (%.0f mAh an hour), %s %.0f mA, %d mV (%.0f mW), plugged %d%s; panel %.2f Hz; memory map %d lines",
        (unsigned long long)frames, secs, cl, W.st, W.st_high, (double)W.h0, (double)(W.h0_low == INFINITY ? NAN : W.h0_low), (double)W.h10, (double)(W.h10_high == -INFINITY ? NAN : W.h10_high),
        dq, dq / 1000.0 / ps * 3600.0, g_plugged == 0 ? "drain mean" : "current mean (as reported)", mean_ma, mv, mv > 0 ? mean_ma * mv / 1000.0 : 0, pl,
        dm_front_battery ? (W.ncur ? "" : " (no battery readings)") : " (no front end)", dm_front_panel_mhz_now ? dm_front_panel_mhz_now() / 1000.0 : 0.0, maps_lines());
    window_reset();
}
// A CPU's cluster, as its cpufreq policy's first CPU (related_cpus), read once: the recording thread's cluster in the present-to-present line (A/B windows on one cluster only)
int vk_stats_cluster_of(int cpu) {
    static int first[16], done;
    if (!done) { done = 1; for (int c = 0; c < 16; c++) { char p[96], b[64]; first[c] = c; snprintf(p, sizeof p, "/sys/devices/system/cpu/cpu%d/cpufreq/related_cpus", c); if (read_small(p, b, sizeof b) > 0) first[c] = atoi(b); } }
    return cpu >= 0 && cpu < 16 ? first[cpu] : cpu;
}
static void *stats_thread(void *a) {
    (void)a; pthread_setname_np("dm-stats");
    thermal_init(); window_reset();
    uint64_t k = 0, last_frames = g_start_frames, t_win = mono_ns(), t_sec = t_win;
    cpu_window(); clusters((char[512]){0}, 512);   // the baselines: every thread's time, every cluster's time_in_state
    for (;;) {
        pthread_mutex_lock(&g_mu);
        struct timespec dl; clock_gettime(CLOCK_REALTIME, &dl); dl.tv_sec += 1;
        while (!g_req_frames) if (pthread_cond_timedwait(&g_cv, &g_mu, &dl)) break;
        uint64_t req = g_req_frames; g_req_frames = 0;
        pthread_mutex_unlock(&g_mu);
        uint64_t now = mono_ns();
        if (now - t_sec >= 900000000ull) { second_tick(k++); t_sec = now; }
        if (req) { window_log(req > last_frames ? req - last_frames : 0, now - t_win); last_frames = req; t_win = now; }
    }
    return NULL;
}
// The present: the first call starts the thread (its baselines at frame `frames`), each later one ends a window at frame `frames` (every 600)
void vk_stats_window(uint64_t frames) {
    static int started; if (!started) { started = 1; g_start_frames = frames; pthread_t th; if (pthread_create(&th, NULL, stats_thread, NULL)) LOG("[RED] vk: the heat-and-energy thread was not started"); else pthread_detach(th); return; }
    pthread_mutex_lock(&g_mu); g_req_frames = frames; pthread_cond_signal(&g_cv); pthread_mutex_unlock(&g_mu);
}
