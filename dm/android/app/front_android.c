// Android app front end (front.h; Mac: front_mac.m, iOS: dm/ios): the native half of the app, a NativeActivity in a process of its own (":game"). The start-up menu
// (MenuActivity.java, in the app's main process) writes the player's choices to <files>/settings.env and starts this activity; here the environment is set from it, the
// host runs on a big-stack thread, the window goes to the Vulkan backend (front_native_window), and controllers -- the console's own and any other, an Xbox one over
// USB or Bluetooth -- reach the game through dm_pad_set, by position as on the Mac and iOS (bottom B, right A, top X, left Y).
// Phones without a controller get on-screen controls (front_overlay, drawn by the Vulkan backend over the picture): they show at the first touch and hide again
// when a controller is used. Holding − and + together for two seconds goes back to the menu: the game process ends (progress the game has saved is kept) and the menu opens with the options shown.
// Game files live in the app's external folder (<external>/romfs, dm, vk, font), the saves in its internal one (<files>/save); the log goes to <external>/log.txt.
#include "dm.h"
#include "dm_input.h"
#include "front.h"
#include <android/input.h>
#include <android/keycodes.h>
#include <android/looper.h>
#include <android/native_activity.h>
#include <android/native_window.h>
#include <jni.h>
#include <math.h>
#include <stdatomic.h>
#include <sys/stat.h>
#include <unistd.h>

int dm_main(int argc, char **argv);   // dm/host/main.c, compiled with -Dmain=dm_main
void dm_focus_set(int st);
void dm_audio_suspend(int on);
void dm_gfx_suspend(int on);
void dm_host_shutdown(const char *why);

static ANativeActivity *g_act;
static pthread_mutex_t g_wmu = PTHREAD_MUTEX_INITIALIZER;
static ANativeWindow *g_win;
static _Atomic int g_win_gen;   // bumped whenever the window changes: the backend makes a new swapchain
// The window the backend presents to (NULL while there is none), and its generation; the backend holds it with front_window_lock while it uses it
void *front_native_window(int *gen) { if (gen) *gen = atomic_load(&g_win_gen); return g_win; }
void front_window_lock(int on) { if (on) pthread_mutex_lock(&g_wmu); else pthread_mutex_unlock(&g_wmu); }
void *front_metal_layer(void) { return NULL; }
int front_screen_max_fps(void) { return 0; }
int front_text_input(const char *title, const char *sub, const char *guide, const char *initial, int max_chars, char *out, int out_cap) {
    (void)title; (void)sub; (void)guide; (void)initial; (void)max_chars; (void)out; (void)out_cap; return -1;
}
void front_run(void (*guest)(void)) { guest(); }   // the cheats are loaded by the host itself (main.c dm_cheats_init)

// ---------- Controllers: one state per input device; buttons are ORed, each stick comes from the device that moves it furthest ----------
#define NDEV 8
static struct { int32_t id, used; uint64_t btn; float ax[10]; } g_dev[NDEV];   // id: Android's device ID (adb's injected events come from -1)
enum { AX_LX, AX_LY, AX_RX, AX_RY, AX_HX, AX_HY, AX_LT, AX_RT, AX_BRAKE, AX_GAS };
static int dev_slot(int32_t id) {
    for (int i = 0; i < NDEV; i++) if (g_dev[i].used && g_dev[i].id == id) return i;
    for (int i = 0; i < NDEV; i++) if (!g_dev[i].used) { g_dev[i].id = id; g_dev[i].used = 1; return i; }
    return 0;
}
static uint64_t key_bit(int32_t k) {
    switch (k) {
        case AKEYCODE_BUTTON_B: return DM_BTN_A;  case AKEYCODE_BUTTON_A: return DM_BTN_B;   // by position: right = A, bottom = B
        case AKEYCODE_BUTTON_Y: return DM_BTN_X;  case AKEYCODE_BUTTON_X: return DM_BTN_Y;   // top = X, left = Y
        case AKEYCODE_BUTTON_L1: return DM_BTN_L; case AKEYCODE_BUTTON_R1: return DM_BTN_R;
        case AKEYCODE_BUTTON_L2: return DM_BTN_ZL; case AKEYCODE_BUTTON_R2: return DM_BTN_ZR;
        case AKEYCODE_BUTTON_THUMBL: return DM_BTN_LS; case AKEYCODE_BUTTON_THUMBR: return DM_BTN_RS;
        case AKEYCODE_BUTTON_START: return DM_BTN_PLUS; case AKEYCODE_BUTTON_SELECT: case AKEYCODE_BACK: return DM_BTN_MINUS;
        case AKEYCODE_DPAD_LEFT: return DM_BTN_LEFT; case AKEYCODE_DPAD_UP: return DM_BTN_UP; case AKEYCODE_DPAD_RIGHT: return DM_BTN_RIGHT; case AKEYCODE_DPAD_DOWN: return DM_BTN_DOWN;
    }
    return 0;
}
static uint64_t g_combo_since;
static int g_touch_slot = -1;   // the device slot of the on-screen controls
static void back_to_menu(void);
// A press is held at least 50 ms (three frames; latch_thread lets it go, and times the − + hold): the game reads the controller many times a frame (about 40 for Hollow Knight) and acts on one
// of those reads, so a shorter tap could fall between them
static uint64_t g_latch, g_prev; static uint64_t g_press_at[16];
static pthread_mutex_t g_pmu = PTHREAD_MUTEX_INITIALIZER;
static void pads_push_(void);
static void pads_push(void) { pthread_mutex_lock(&g_pmu); pads_push_(); pthread_mutex_unlock(&g_pmu); }
static void *latch_thread(void *a) {
    (void)a;
    for (;;) { usleep(4000); pthread_mutex_lock(&g_pmu); if (g_latch || g_combo_since) pads_push_(); pthread_mutex_unlock(&g_pmu); }   // also times − and + held with no new events
    return NULL;
}
static void pads_push_(void) {
    uint64_t btn = 0; float best[4] = {0}; float bm[2] = {0};
    for (int i = 0; i < NDEV; i++) {
        if (!g_dev[i].used) continue;
        const float *a = g_dev[i].ax; uint64_t b = g_dev[i].btn;
        if (a[AX_HX] < -0.5f) b |= DM_BTN_LEFT; if (a[AX_HX] > 0.5f) b |= DM_BTN_RIGHT; if (a[AX_HY] < -0.5f) b |= DM_BTN_UP; if (a[AX_HY] > 0.5f) b |= DM_BTN_DOWN;
        if (a[AX_LT] > 0.5f || a[AX_BRAKE] > 0.5f) b |= DM_BTN_ZL; if (a[AX_RT] > 0.5f || a[AX_GAS] > 0.5f) b |= DM_BTN_ZR;
        btn |= b;
        float m0 = hypotf(a[AX_LX], a[AX_LY]), m1 = hypotf(a[AX_RX], a[AX_RY]);
        if (m0 > bm[0]) { bm[0] = m0; best[0] = a[AX_LX]; best[1] = a[AX_LY]; }
        if (m1 > bm[1]) { bm[1] = m1; best[2] = a[AX_RX]; best[3] = a[AX_RY]; }
    }
    for (int k = 0; k < 4; k++) if (fabsf(best[k]) < 0.12f) best[k] = 0;   // dead zone
    uint64_t t = now_ns();
    for (int b = 0; b < 16; b++) {
        uint64_t m = 1ull << b;
        if ((btn & m) && !(g_prev & m)) g_press_at[b] = t;
        if (!(btn & m) && (g_prev & m) && t - g_press_at[b] < 50000000ull) g_latch |= m;   // released too soon: held on
        if ((g_latch & m) && (btn & m || t - g_press_at[b] >= 50000000ull)) g_latch &= ~m;
    }
    g_prev = btn; btn |= g_latch;
    #define AXV(v) (int32_t)(fmaxf(-1.f, fminf(1.f, (v))) * DM_STICK_MAX)
    dm_pad_set(btn, AXV(best[0]), AXV(-best[1]), AXV(best[2]), AXV(-best[3]));   // Android's y grows downwards, the Switch's upwards
    #undef AXV
    int combo = (btn & (DM_BTN_PLUS | DM_BTN_MINUS)) == (DM_BTN_PLUS | DM_BTN_MINUS);
    if (!combo) g_combo_since = 0;
    else if (!g_combo_since) g_combo_since = now_ns();
    else if (now_ns() - g_combo_since > 2000000000ull) back_to_menu();
}
// ---------- On-screen controls ----------
// Laid out from the window's height h: the D-pad bottom left, A B X Y bottom right (by position, as on the Switch), L / ZL and R / ZR in the top corners, − and + top centre
typedef struct { float x, y, w, h; int on; char label[3]; } dm_ovl;
enum { NVB = 15 };
static struct { const char *label; uint64_t bit; float cx, cy; } g_vb[NVB];
static _Atomic int g_ovl_show; static _Atomic uint64_t g_ovl_on; static float g_vw, g_vh, g_vr, g_dx, g_dy;
static void vb_layout(void) {
    ANativeWindow *w = front_native_window(NULL); if (!w) return;
    float W_ = (float)ANativeWindow_getWidth(w), H = (float)ANativeWindow_getHeight(w);
    if (W_ == g_vw && H == g_vh) return;
    g_vw = W_; g_vh = H; g_vr = 0.065f * H; float o = 0.12f * H, fx = W_ - 0.24f * H, fy = H - 0.25f * H;
    g_dx = 0.24f * H; g_dy = H - 0.25f * H;
    int i = 0;
    #define VB(l, b, x, y) g_vb[i++] = (typeof(g_vb[0])){l, b, x, y}
    VB("A", DM_BTN_A, fx + o, fy); VB("B", DM_BTN_B, fx, fy + o); VB("X", DM_BTN_X, fx, fy - o); VB("Y", DM_BTN_Y, fx - o, fy);
    VB("L", DM_BTN_L, 0.11f * H, 0.11f * H); VB("ZL", DM_BTN_ZL, 0.11f * H, 0.28f * H); VB("R", DM_BTN_R, W_ - 0.11f * H, 0.11f * H); VB("ZR", DM_BTN_ZR, W_ - 0.11f * H, 0.28f * H);
    VB("-", DM_BTN_MINUS, W_ / 2 - 0.1f * H, 0.08f * H); VB("+", DM_BTN_PLUS, W_ / 2 + 0.1f * H, 0.08f * H);
    VB("", DM_BTN_UP, g_dx, g_dy - o); VB("", DM_BTN_DOWN, g_dx, g_dy + o); VB("", DM_BTN_LEFT, g_dx - o, g_dy); VB("", DM_BTN_RIGHT, g_dx + o, g_dy);
    VB("", 0, g_dx, g_dy);
    #undef VB
}
// What the backend draws over the picture: one box per control, in window pixels (0 = hidden)
int front_overlay(dm_ovl *out, int max) {
    if (!atomic_load(&g_ovl_show)) return 0;
    vb_layout(); uint64_t on = atomic_load(&g_ovl_on); int n = 0;
    for (int i = 0; i < NVB && n < max; i++) {
        dm_ovl *b = &out[n++]; float r = g_vb[i].bit ? g_vr : g_vr * 0.6f;
        *b = (dm_ovl){g_vb[i].cx - r, g_vb[i].cy - r, 2 * r, 2 * r, g_vb[i].bit && (on & g_vb[i].bit) != 0, ""};
        snprintf(b->label, sizeof b->label, "%s", g_vb[i].label);
    }
    return n;
}
// Every finger on the screen: the buttons under it; on the D-pad its direction (eight ways) and the left stick, from the finger's offset to the pad's centre
static void touch_event(AInputEvent *e) {
    vb_layout();
    int32_t act = AMotionEvent_getAction(e), kind = act & AMOTION_EVENT_ACTION_MASK, up_idx = -1;
    if (kind == AMOTION_EVENT_ACTION_UP || kind == AMOTION_EVENT_ACTION_CANCEL) up_idx = -2;
    else if (kind == AMOTION_EVENT_ACTION_POINTER_UP) up_idx = (act & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK) >> AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT;
    uint64_t btn = 0; float sx = 0, sy = 0;
    if (up_idx != -2) for (size_t p = 0; p < AMotionEvent_getPointerCount(e); p++) {
        if ((int32_t)p == up_idx) continue;
        float x = AMotionEvent_getX(e, p), y = AMotionEvent_getY(e, p), ddx = x - g_dx, ddy = y - g_dy;
        if (fabsf(ddx) < 0.24f * g_vh && fabsf(ddy) < 0.24f * g_vh) {
            float m = hypotf(ddx, ddy), dz = 0.03f * g_vh;
            if (m > dz) { if (ddx < -0.4f * m) btn |= DM_BTN_LEFT; if (ddx > 0.4f * m) btn |= DM_BTN_RIGHT; if (ddy < -0.4f * m) btn |= DM_BTN_UP; if (ddy > 0.4f * m) btn |= DM_BTN_DOWN;
                          float k = fminf(1.f, m / (0.12f * g_vh)); sx = ddx / m * k; sy = ddy / m * k; }
            continue;
        }
        for (int i = 0; i < 10; i++) if (fabsf(x - g_vb[i].cx) < 1.3f * g_vr && fabsf(y - g_vb[i].cy) < 1.3f * g_vr) btn |= g_vb[i].bit;
    }
    if (g_touch_slot < 0) g_touch_slot = dev_slot(-1000000);
    g_dev[g_touch_slot].btn = btn & ~(uint64_t)(DM_BTN_LEFT | DM_BTN_RIGHT | DM_BTN_UP | DM_BTN_DOWN) ;
    g_dev[g_touch_slot].btn |= btn & (DM_BTN_LEFT | DM_BTN_RIGHT | DM_BTN_UP | DM_BTN_DOWN);
    g_dev[g_touch_slot].ax[AX_LX] = sx; g_dev[g_touch_slot].ax[AX_LY] = sy;
    atomic_store(&g_ovl_on, btn); atomic_store(&g_ovl_show, 1);
    pads_push();
}
static int on_input(AInputEvent *e) {
    int32_t type = AInputEvent_getType(e), src = AInputEvent_getSource(e);
    static int seen; if (!seen++) LOG("front end: first input event (type %d, source %#x)", type, src);
    int s = dev_slot(AInputEvent_getDeviceId(e));
    if (type == AINPUT_EVENT_TYPE_MOTION && (src & AINPUT_SOURCE_TOUCHSCREEN) == AINPUT_SOURCE_TOUCHSCREEN) { touch_event(e); return 1; }
    if (src & (AINPUT_SOURCE_GAMEPAD | AINPUT_SOURCE_JOYSTICK | AINPUT_SOURCE_DPAD)) atomic_store(&g_ovl_show, 0);   // a controller in use: the on-screen controls go
    if (type == AINPUT_EVENT_TYPE_KEY) {
        uint64_t b = key_bit(AKeyEvent_getKeyCode(e)); if (!b) return AKeyEvent_getKeyCode(e) == AKEYCODE_BACK;
        if (AKeyEvent_getAction(e) == AKEY_EVENT_ACTION_DOWN) g_dev[s].btn |= b; else if (AKeyEvent_getAction(e) == AKEY_EVENT_ACTION_UP) g_dev[s].btn &= ~b;
        pads_push(); return 1;
    }
    if (type == AINPUT_EVENT_TYPE_MOTION && (src & AINPUT_SOURCE_JOYSTICK) == AINPUT_SOURCE_JOYSTICK) {
        static const int32_t axes[10] = {AMOTION_EVENT_AXIS_X, AMOTION_EVENT_AXIS_Y, AMOTION_EVENT_AXIS_Z, AMOTION_EVENT_AXIS_RZ, AMOTION_EVENT_AXIS_HAT_X, AMOTION_EVENT_AXIS_HAT_Y,
                                         AMOTION_EVENT_AXIS_LTRIGGER, AMOTION_EVENT_AXIS_RTRIGGER, AMOTION_EVENT_AXIS_BRAKE, AMOTION_EVENT_AXIS_GAS};
        for (int k = 0; k < 10; k++) g_dev[s].ax[k] = AMotionEvent_getAxisValue(e, axes[k], 0);
        pads_push(); return 1;
    }
    return 0;
}
static AInputQueue *g_iq;
static int input_cb(int fd, int events, void *data) {
    (void)fd; (void)events; (void)data; AInputEvent *e;
    while (g_iq && AInputQueue_getEvent(g_iq, &e) >= 0) {
        if (AInputQueue_preDispatchEvent(g_iq, e)) continue;
        AInputQueue_finishEvent(g_iq, e, on_input(e));
    }
    return 1;
}

// ---------- Back to the menu: a flag for the menu, then this process ends ----------
static char g_files[512];
static void back_to_menu(void) {
    static _Atomic int once; if (atomic_exchange(&once, 1)) return;
    char p[600]; snprintf(p, sizeof p, "%s/show_menu", g_files); FILE *f = fopen(p, "w"); if (f) fclose(f);
    LOG("front end: − and + held: back to the menu");
    ANativeActivity_finish(g_act);
    dm_host_shutdown("back to the menu");
}

// ---------- Boot: environment from the menu's settings, log, then the host on its own thread ----------
static char *g_argv[4];
static void boot_env(const char *files, const char *ext) {
    char p[1024];
    snprintf(p, sizeof p, "%s/log.txt", ext);
    char prev[1024]; snprintf(prev, sizeof prev, "%s/log.prev.txt", ext); rename(p, prev);
    if (freopen(p, "w", stderr)) setvbuf(stderr, NULL, _IOLBF, 0);
    snprintf(p, sizeof p, "%s/settings.env", files);   // KEY=VALUE lines from MenuActivity
    FILE *f = fopen(p, "r");
    if (f) { char ln[1024]; while (fgets(ln, sizeof ln, f)) { ln[strcspn(ln, "\r\n")] = 0; char *eq = strchr(ln, '='); if (!eq || ln[0] == '#') continue; *eq = 0; setenv(ln, eq + 1, 1); fprintf(stderr, "boot: %s=%s\n", ln, eq + 1); } fclose(f); }
    else fprintf(stderr, "boot: no settings.env, the defaults apply\n");
    snprintf(p, sizeof p, "%s/env.txt", ext);   // diagnostics: switches in <external>/env.txt (one KEY=VALUE per line) win
    if ((f = fopen(p, "r"))) { char ln[1024]; while (fgets(ln, sizeof ln, f)) { ln[strcspn(ln, "\r\n")] = 0; char *eq = strchr(ln, '='); if (!eq || ln[0] == '#') continue; *eq = 0; setenv(ln, eq + 1, 1); fprintf(stderr, "boot: env %s=%s\n", ln, eq + 1); } fclose(f); }
    const char *sub[][2] = {{"DM_ROMFS", "romfs"}, {"DM_VK_DIR", "vk"}, {"DM_FONT_DIR", "font"}};
    for (int i = 0; i < 3; i++) { snprintf(p, sizeof p, "%s/%s", ext, sub[i][1]); setenv(sub[i][0], p, 0); }
    snprintf(p, sizeof p, "%s/save", files); mkdir(p, 0755); setenv("DM_SAVE", p, 0);
    snprintf(p, sizeof p, "%s/temp", files); mkdir(p, 0755); setenv("DM_TEMP", p, 0);
    snprintf(p, sizeof p, "%s/cheats", files); if (!access(p, R_OK)) setenv("DM_CHEATS", p, 0);   // the cheats folder (<build>.txt files; hle_cheats.c)
    chdir(files);
    g_argv[0] = strdup("dmhost"); snprintf(p, sizeof p, "%s/dm", ext); g_argv[1] = strdup(p);
}
static void *boot_thread(void *a) {
    (void)a;
    static char alt[65536]; stack_t ss = {.ss_sp = alt, .ss_size = sizeof alt}; sigaltstack(&ss, 0);
    int rc = dm_main(2, g_argv);
    fprintf(stderr, "boot: dm_main returned %d\n", rc);
    return NULL;
}

// Immersive full screen: Android's navigation bar and gesture handle hidden (View.setSystemUiVisibility, on the UI thread)
static void immersive(ANativeActivity *a) {
    JNIEnv *env = a->env;
    jclass ac = (*env)->GetObjectClass(env, a->clazz);
    jobject win = (*env)->CallObjectMethod(env, a->clazz, (*env)->GetMethodID(env, ac, "getWindow", "()Landroid/view/Window;"));
    jobject dv = (*env)->CallObjectMethod(env, win, (*env)->GetMethodID(env, (*env)->GetObjectClass(env, win), "getDecorView", "()Landroid/view/View;"));
    (*env)->CallVoidMethod(env, dv, (*env)->GetMethodID(env, (*env)->GetObjectClass(env, dv), "setSystemUiVisibility", "(I)V"), 0x1706);   // IMMERSIVE_STICKY | LAYOUT_* | HIDE_NAVIGATION | FULLSCREEN
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
}

// ---------- Activity callbacks ----------
static void on_window_created(ANativeActivity *a, ANativeWindow *w) { (void)a; pthread_mutex_lock(&g_wmu); g_win = w; atomic_fetch_add(&g_win_gen, 1); pthread_mutex_unlock(&g_wmu); LOG("front end: window %dx%d", ANativeWindow_getWidth(w), ANativeWindow_getHeight(w)); }
static void on_window_destroyed(ANativeActivity *a, ANativeWindow *w) { (void)a; (void)w; pthread_mutex_lock(&g_wmu); g_win = NULL; atomic_fetch_add(&g_win_gen, 1); pthread_mutex_unlock(&g_wmu); }
static void on_window_resized(ANativeActivity *a, ANativeWindow *w) { (void)a; (void)w; atomic_fetch_add(&g_win_gen, 1); }
static void on_input_created(ANativeActivity *a, AInputQueue *q) { (void)a; g_iq = q; AInputQueue_attachLooper(q, ALooper_forThread(), 1, input_cb, NULL); }
static void on_input_destroyed(ANativeActivity *a, AInputQueue *q) { (void)a; AInputQueue_detachLooper(q); g_iq = NULL; }
static void on_resume(ANativeActivity *a) { (void)a; dm_focus_set(1); dm_audio_suspend(0); }
static void on_pause(ANativeActivity *a) { (void)a; dm_audio_suspend(1); dm_focus_set(3); }
static void on_focus(ANativeActivity *a, int f) { if (f) immersive(a); dm_focus_set(f ? 1 : 2); }
static void on_destroy(ANativeActivity *a) { (void)a; dm_host_shutdown("activity destroyed"); }

__attribute__((visibility("default"))) void ANativeActivity_onCreate(ANativeActivity *a, void *saved, size_t saved_n) {
    (void)saved; (void)saved_n;
    g_act = a;
    a->callbacks->onNativeWindowCreated = on_window_created; a->callbacks->onNativeWindowDestroyed = on_window_destroyed;
    a->callbacks->onNativeWindowResized = on_window_resized;
    a->callbacks->onInputQueueCreated = on_input_created; a->callbacks->onInputQueueDestroyed = on_input_destroyed;
    a->callbacks->onResume = on_resume; a->callbacks->onPause = on_pause; a->callbacks->onWindowFocusChanged = on_focus; a->callbacks->onDestroy = on_destroy;
    ANativeActivity_setWindowFlags(a, 0x80 /* KEEP_SCREEN_ON */ | 0x400 /* FULLSCREEN */, 0);
    immersive(a);
    static int booted; if (booted++) return;   // the activity can be made again (rotation, a return): the host runs once per process
    snprintf(g_files, sizeof g_files, "%s", a->internalDataPath);
    boot_env(a->internalDataPath, a->externalDataPath);
    pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setstacksize(&at, 16 << 20);
    pthread_t th; if (pthread_create(&th, &at, boot_thread, NULL)) die("front end: cannot start the host");
    pthread_detach(th);
    if (!pthread_create(&th, NULL, latch_thread, NULL)) pthread_detach(th);
}
