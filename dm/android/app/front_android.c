// Android app front end (front.h; Mac: front_mac.m, iOS: dm/ios): the native half of the app, a NativeActivity in a process of its own (":game"). The start-up menu
// (MenuActivity.java, in the app's main process) writes the player's choices to <files>/settings.env and starts this activity; here the environment is set from it, the
// host runs on a big-stack thread, the window goes to the Vulkan backend (front_native_window), and controllers -- the console's own and any other, an Xbox one over
// USB or Bluetooth -- reach the game through dm_pad_set, by position as on the Mac and iOS (bottom B, right A, top X, left Y).
// Phones without a controller get on-screen controls (front_overlay, drawn by the Vulkan backend over the picture from the APK's sprite atlas: buttons and sticks, laid
// out by the launcher's editor): they show at the first touch and hide again when a controller is used. Holding − and + together for two seconds goes back to the menu: the game process ends (progress the game has saved is kept) and the menu opens with the options shown.
// Game files live in the app's external folder (<external>/romfs, dm, vk, font), the saves in its internal one (<files>/save); the log goes to <external>/log.txt.
#include "dm.h"
#include "dm_input.h"
#include "front.h"
#include "stick_tap.h"
#include "touch_map.h"
#include <android/input.h>
#include <android/log.h>
#include <android/keycodes.h>
#include <android/looper.h>
#include <android/native_activity.h>
#include <android/native_window.h>
#include <android/asset_manager.h>
#include <android/imagedecoder.h>
#include <jni.h>
#include <malloc.h>
#include <math.h>
#include <stdatomic.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

int dm_main(int argc, char **argv);   // dm/host/main.c, compiled with -Dmain=dm_main
void dm_focus_set(int st);
void dm_audio_suspend(int on);
void dm_gfx_suspend(int on);
void dm_host_shutdown(const char *why);
void guest_clock_freeze_for(int reason, int on);
void dm_vk_window_lost(void);   // src_vk/nvn_vk.c

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
        case AKEYCODE_BUTTON_C: return DM_BTN_ZR; case AKEYCODE_BUTTON_Z: return DM_BTN_ZL;   // six-button pads (Sega layout)
    }
    return 0;
}
// A physical keyboard (USB, Bluetooth, a keyboard cover), mapped as the Mac's (dm/host/front_mac.m): WASD the left stick, the arrows the D-pad or, with Shift held, the right
// stick, J K U I = A B X Y, Q E = L R, Z C = ZL ZR, F G = press the left / right stick, Return = +, Tab = −, Space = shake the controller. It is one more device slot
static struct { int w, a, s, d, left, up, right, down, shift; uint64_t btn; } g_kbd;
static int g_kbd_slot = -1;
static uint64_t kbd_bit(int32_t k) {
    switch (k) {
        case AKEYCODE_J: return DM_BTN_A; case AKEYCODE_K: return DM_BTN_B; case AKEYCODE_U: return DM_BTN_X; case AKEYCODE_I: return DM_BTN_Y;
        case AKEYCODE_F: return DM_BTN_LS; case AKEYCODE_G: return DM_BTN_RS;
        case AKEYCODE_Q: return DM_BTN_L; case AKEYCODE_E: return DM_BTN_R; case AKEYCODE_Z: return DM_BTN_ZL; case AKEYCODE_C: return DM_BTN_ZR;
        case AKEYCODE_ENTER: case AKEYCODE_NUMPAD_ENTER: return DM_BTN_PLUS; case AKEYCODE_TAB: return DM_BTN_MINUS; case AKEYCODE_SPACE: return DM_BTN_SHAKE;
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
// DM_LATCH_BLOCK=1 (vk19.64, off by default): the thread sleeps on a condition while nothing is latched and − + is not held, woken by pads_push_ when either starts,
// instead of waking every 4 ms whatever happens (profile P1: 93 ms of nanosleep in 750 frames, 250 wake-ups a second for nothing)
static pthread_cond_t g_latch_cv = PTHREAD_COND_INITIALIZER; static int g_latch_block = -1;
static void *latch_thread(void *a) {
    (void)a;
    if (g_latch_block < 0) { const char *e = getenv("DM_LATCH_BLOCK"); g_latch_block = e && atoi(e) != 0; if (g_latch_block) LOG("front end: DM_LATCH_BLOCK=1: the input latch thread sleeps while nothing is held"); }
    for (;;) {
        if (g_latch_block) { pthread_mutex_lock(&g_pmu); while (!g_latch && !g_combo_since) pthread_cond_wait(&g_latch_cv, &g_pmu); pthread_mutex_unlock(&g_pmu); }
        usleep(4000); pthread_mutex_lock(&g_pmu); if (g_latch || g_combo_since) pads_push_(); pthread_mutex_unlock(&g_pmu);   // also times − and + held with no new events
    }
    return NULL;
}
static void pads_push_(void) {
    uint64_t btn = 0; float best[4] = {0}; float bm[2] = {0};
    for (int i = 0; i < NDEV; i++) {
        if (!g_dev[i].used) continue;
        const float *a = g_dev[i].ax; uint64_t b = g_dev[i].btn;
        if (a[AX_HX] < -0.5f) b |= DM_BTN_LEFT; if (a[AX_HX] > 0.5f) b |= DM_BTN_RIGHT; if (a[AX_HY] < -0.5f) b |= DM_BTN_UP; if (a[AX_HY] > 0.5f) b |= DM_BTN_DOWN;
        if (a[AX_LT] > 0.35f || a[AX_BRAKE] > 0.35f) b |= DM_BTN_ZL; if (a[AX_RT] > 0.35f || a[AX_GAS] > 0.35f) b |= DM_BTN_ZR;   // analog triggers as the digital ZL / ZR
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
    if (g_latch || g_combo_since) pthread_cond_signal(&g_latch_cv);   // DM_LATCH_BLOCK: the latch thread has something to time (g_pmu is held)
}
// ---------- On-screen controls ----------
// Groups of buttons -- A B X Y bottom right (by position, as on the Switch), the D-pad bottom left, L / ZL and R / ZR in the top corners, − and + top centre -- and two
// sticks. A stick floats, as in mobile games: a finger that comes down in its zone (by default the left or the right half of the screen), on no button, is that stick,
// where it came down the stick's centre and its offset from there the tilt (full at the stick's radius, a small dead zone); lifting it lets the stick go back. Or it
// sits at a fixed place, shown all the time, and a finger that comes down on it moves it. Each finger keeps to what it started on, so sticks and buttons work together.
// Where everything goes comes from the launcher's layout editor (MenuActivity → ControlsActivity, ControlsLayout.java): <files>/controls_active.txt, the active profile's
// groups (centre as fractions of the screen, a scale), the sticks' modes and zones, the hidden buttons and the opacity; without it, the layout as it always was.
// Drawn by the Vulkan backend from the sprite atlas the APK carries (assets/controls, made from the SVG masters in app/res_svg)
typedef struct { float x, y, w, h; int on; char label[3]; int sprite; float alpha; } dm_ovl;
enum { NVB = 14, NPTR = 10, NG = 7, G_FACE = 0, G_DPAD, G_LZL, G_RZR, G_MP, G_LS, G_RS };
static const char *const GNAME[NG] = {"face", "dpad", "lzl", "rzr", "mp", "ls", "rs"};
// The same table as ControlsLayout.BUTTONS: offset from the group's centre and radius in units of the screen's height at scale 1
static const struct { const char *name, *sprite, *label; uint64_t bit; int group; float dx, dy, r; } BTN[NVB] = {
    {"A", "btn_a", "A", DM_BTN_A, G_FACE, 0.12f, 0, 0.065f}, {"B", "btn_b", "B", DM_BTN_B, G_FACE, 0, 0.12f, 0.065f}, {"X", "btn_x", "X", DM_BTN_X, G_FACE, 0, -0.12f, 0.065f},
    {"Y", "btn_y", "Y", DM_BTN_Y, G_FACE, -0.12f, 0, 0.065f},
    {"UP", "dpad_up", "", DM_BTN_UP, G_DPAD, 0, -0.1f, 0.05f}, {"DOWN", "dpad_down", "", DM_BTN_DOWN, G_DPAD, 0, 0.1f, 0.05f}, {"LEFT", "dpad_left", "", DM_BTN_LEFT, G_DPAD, -0.1f, 0, 0.05f},
    {"RIGHT", "dpad_right", "", DM_BTN_RIGHT, G_DPAD, 0.1f, 0, 0.05f},
    {"L", "btn_l", "L", DM_BTN_L, G_LZL, 0, -0.085f, 0.065f}, {"ZL", "btn_zl", "ZL", DM_BTN_ZL, G_LZL, 0, 0.085f, 0.065f}, {"R", "btn_r", "R", DM_BTN_R, G_RZR, 0, -0.085f, 0.065f},
    {"ZR", "btn_zr", "ZR", DM_BTN_ZR, G_RZR, 0, 0.085f, 0.065f}, {"MINUS", "btn_minus", "-", DM_BTN_MINUS, G_MP, -0.1f, 0, 0.065f}, {"PLUS", "btn_plus", "+", DM_BTN_PLUS, G_MP, 0.1f, 0, 0.065f}};
#define STICK_R 0.12f   // a stick's travel (its base's radius) and its knob's radius, in units of the screen's height at scale 1 (ControlsLayout.STICK_R / KNOB_R)
#define KNOB_R 0.055f
static struct { float x[NG], y[NG], s[NG]; int hidden[NG], fixed[2]; float zone[2][4]; float opacity; uint64_t hide; int stick_click; } g_ctl;   // the layout, normalised (ctl_layout); stick_click: double tap on a stick = L3 / R3
static struct { float cx, cy, r; int vis; } g_vb[NVB];   // the buttons in window pixels
static _Atomic int g_ovl_show; static _Atomic uint64_t g_ovl_on; static float g_vw, g_vh;
// Fingers down (by pointer ID): kind 1 buttons, 2 left stick, 3 right stick, 4 the game's touch screen (gid its finger there, 0-15), 5 nothing (came down on a black bar beside
// the picture, or on a stick another finger holds); x0 / y0 the stick's centre (where it came down, or the fixed place), x / y where it is
enum { PK_NONE = 0, PK_BTN = 1, PK_LS = 2, PK_RS = 3, PK_TOUCH = 4, PK_DEAD = 5 };
static struct { int32_t id; int kind; float x0, y0, x, y; float fx, fy, maxd; int click, gid; } g_ptr[NPTR]; static int g_nptr;   // fx / fy where it came down, maxd how far it went; click: a stick's double tap (L3 / R3)
static stick_tap g_tap[2];   // each stick's last touch (stick_tap.h)
static struct { int on; float x0, y0, x, y; } g_stick[2];   // what front_overlay draws (copied under g_smu)
static pthread_mutex_t g_smu = PTHREAD_MUTEX_INITIALIZER;
static void ctl_defaults(float W_, float H) {   // the layout as it always was (ControlsLayout.defaults)
    float d[NG][2] = {{(W_ - 0.24f * H) / W_, 0.75f}, {0.22f * H / W_, 0.75f}, {0.11f * H / W_, 0.195f}, {(W_ - 0.11f * H) / W_, 0.195f}, {0.5f, 0.08f}, {0.18f, 0.45f}, {0.82f, 0.45f}};
    for (int g = 0; g < NG; g++) { g_ctl.x[g] = d[g][0]; g_ctl.y[g] = d[g][1]; g_ctl.s[g] = 1; g_ctl.hidden[g] = 0; }
    g_ctl.fixed[0] = g_ctl.fixed[1] = 0; g_ctl.opacity = 0.75f; g_ctl.hide = 0; g_ctl.stick_click = 1;
    float z[2][4] = {{0, 0, 0.5f, 1}, {0.5f, 0, 0.5f, 1}}; memcpy(g_ctl.zone, z, sizeof z);
}
static void ctl_load(void) {   // the launcher's active profile over the defaults
    char p[1100]; if (!g_act || !g_act->internalDataPath) return;
    snprintf(p, sizeof p, "%s/controls_active.txt", g_act->internalDataPath);
    FILE *f = fopen(p, "re"); if (!f) return;
    char ln[256], nm[32]; int n = 0;
    while (fgets(ln, sizeof ln, f)) {
        float a, b, c, d; int h, fx;
        if (sscanf(ln, "opacity %f", &a) == 1) { g_ctl.opacity = a < 0.1f ? 0.1f : a > 1 ? 1 : a; n++; }
        else if (sscanf(ln, "group %31s %f %f %f %d", nm, &a, &b, &c, &h) == 5) { for (int g = 0; g < NG; g++) if (!strcmp(nm, GNAME[g])) { g_ctl.x[g] = a; g_ctl.y[g] = b; g_ctl.s[g] = c < 0.3f ? 0.3f : c > 3 ? 3 : c; g_ctl.hidden[g] = h; n++; } }
        else if (sscanf(ln, "stick %31s %d %f %f %f %f", nm, &fx, &a, &b, &c, &d) == 6) { for (int k = 0; k < 2; k++) if (!strcmp(nm, GNAME[G_LS + k])) { g_ctl.fixed[k] = fx; g_ctl.zone[k][0] = a; g_ctl.zone[k][1] = b; g_ctl.zone[k][2] = c; g_ctl.zone[k][3] = d; n++; } }
        else if (sscanf(ln, "stickclick %d", &fx) == 1) { g_ctl.stick_click = fx != 0; n++; }
        else if (sscanf(ln, "hide %31s", nm) == 1) { for (int i = 0; i < NVB; i++) if (!strcmp(nm, BTN[i].name)) { g_ctl.hide |= BTN[i].bit; n++; } }
    }
    fclose(f);
    static int logged; if (!logged++) LOG("front end: on-screen controls from %s (%d settings)", p, n);
}
static void vb_layout(void) {
    ANativeWindow *w = front_native_window(NULL); if (!w) return;
    float W_ = (float)ANativeWindow_getWidth(w), H = (float)ANativeWindow_getHeight(w);
    if (W_ == g_vw && H == g_vh) return;
    g_vw = W_; g_vh = H; ctl_defaults(W_, H); ctl_load();
    for (int i = 0; i < NVB; i++) { int g = BTN[i].group; float s = g_ctl.s[g];
        g_vb[i].cx = g_ctl.x[g] * W_ + BTN[i].dx * H * s; g_vb[i].cy = g_ctl.y[g] * H + BTN[i].dy * H * s; g_vb[i].r = BTN[i].r * H * s;
        g_vb[i].vis = !g_ctl.hidden[g] && !(g_ctl.hide & BTN[i].bit); }
}
static uint64_t vb_hit(float x, float y) {   // the buttons a finger at (x, y) is over
    uint64_t b = 0; for (int i = 0; i < NVB; i++) if (g_vb[i].vis && fabsf(x - g_vb[i].cx) < 1.3f * g_vb[i].r && fabsf(y - g_vb[i].cy) < 1.3f * g_vb[i].r) b |= BTN[i].bit;
    return b;
}
static float stick_r(int k) { return STICK_R * g_vh * g_ctl.s[G_LS + k]; }
static int stick_at(float x, float y, float *x0, float *y0, int floating) {   // the stick a finger coming down at (x, y) takes (2 left, 3 right), else 0; floating 0: fixed sticks only
    for (int k = 0; k < 2; k++) {
        if (g_ctl.hidden[G_LS + k] || (!floating && !g_ctl.fixed[k])) continue;
        if (g_ctl.fixed[k]) { float cx = g_ctl.x[G_LS + k] * g_vw, cy = g_ctl.y[G_LS + k] * g_vh;
                              if (hypotf(x - cx, y - cy) < 1.35f * stick_r(k)) { *x0 = cx; *y0 = cy; return 2 + k; } continue; }
        const float *z = g_ctl.zone[k];
        if (x >= z[0] * g_vw && x < (z[0] + z[2]) * g_vw && y >= z[1] * g_vh && y < (z[1] + z[3]) * g_vh) { *x0 = x; *y0 = y; return 2 + k; }
    }
    return 0;
}
// The sprite atlas (assets/controls/atlas.png and atlas.txt), decoded once for the backend (premultiplied RGBA), freed after it has made its texture
static void *g_atlas; static int g_aw, g_ah, g_acell, g_acols, g_spr[NVB], g_spr_base = -1, g_spr_knob = -1, g_atlas_tried;
static void atlas_names(void) {
    for (int i = 0; i < NVB; i++) g_spr[i] = -1;
    AAsset *a = g_act ? AAssetManager_open(g_act->assetManager, "controls/atlas.txt", AASSET_MODE_BUFFER) : NULL; if (!a) return;
    size_t n = (size_t)AAsset_getLength(a); char *t = malloc(n + 1); if (!t) { AAsset_close(a); return; }
    memcpy(t, AAsset_getBuffer(a), n); t[n] = 0; AAsset_close(a);
    for (char *ln = strtok(t, "\n"); ln; ln = strtok(NULL, "\n")) {
        char nm[48]; int idx, w, h;
        if (sscanf(ln, "cell %d %d %d %d", &g_acell, &g_acols, &w, &h) == 4) continue;
        if (sscanf(ln, "%47s %d", nm, &idx) != 2 || nm[0] == '#') continue;
        for (int i = 0; i < NVB; i++) if (!strcmp(nm, BTN[i].sprite)) g_spr[i] = idx;
        if (!strcmp(nm, "stick_base")) g_spr_base = idx; else if (!strcmp(nm, "stick_knob")) g_spr_knob = idx;
    }
    free(t);
}
int front_overlay_atlas(const void **rgba, int *w, int *h, int *cell, int *cols) {
    if (!g_atlas_tried) {
        g_atlas_tried = 1; atlas_names();
        AAsset *a = g_act ? AAssetManager_open(g_act->assetManager, "controls/atlas.png", AASSET_MODE_BUFFER) : NULL;
        AImageDecoder *d = NULL;
        if (a && AImageDecoder_createFromAAsset(a, &d) == ANDROID_IMAGE_DECODER_SUCCESS) {
            const AImageDecoderHeaderInfo *hi = AImageDecoder_getHeaderInfo(d);
            int aw = AImageDecoderHeaderInfo_getWidth(hi), ah = AImageDecoderHeaderInfo_getHeight(hi);
            AImageDecoder_setAndroidBitmapFormat(d, ANDROID_BITMAP_FORMAT_RGBA_8888);   // premultiplied: the decoder's default
            size_t st = AImageDecoder_getMinimumStride(d); void *px = st == (size_t)aw * 4 ? malloc(st * (size_t)ah) : NULL;
            if (px && AImageDecoder_decodeImage(d, px, st, st * (size_t)ah) == ANDROID_IMAGE_DECODER_SUCCESS) { g_atlas = px; g_aw = aw; g_ah = ah; } else free(px);
            AImageDecoder_delete(d);
        }
        if (a) AAsset_close(a);
        if (!g_atlas || g_acell <= 0 || g_acols <= 0) { LOG("front end: no on-screen controls atlas in the APK (assets/controls): outlines instead"); free(g_atlas); g_atlas = NULL; }
    }
    if (!g_atlas) return 0;
    *rgba = g_atlas; *w = g_aw; *h = g_ah; *cell = g_acell; *cols = g_acols; return 1;
}
void front_overlay_atlas_release(void) { free(g_atlas); g_atlas = NULL; }
// What the backend draws over the picture, in window pixels (0 = hidden): a sprite per button; per stick its base and its knob (a fixed one always, a floating one while held).
// Without the atlas's names (an APK without it) the outlines and labels of before, and a ring of dots per stick
static int touch_mode(void);
int front_overlay(dm_ovl *out, int max) {
    if (!atomic_load(&g_ovl_show) || touch_mode() == 2) return 0;
    vb_layout(); uint64_t on = atomic_load(&g_ovl_on); int n = 0; float al = g_ctl.opacity;
    for (int i = 0; i < NVB && n < max; i++) {
        if (!g_vb[i].vis) continue;
        dm_ovl *b = &out[n++]; float r = g_vb[i].r;
        *b = (dm_ovl){g_vb[i].cx - r, g_vb[i].cy - r, 2 * r, 2 * r, (on & BTN[i].bit) != 0, "", g_spr[i], al};
        if (g_spr[i] < 0) snprintf(b->label, sizeof b->label, "%s", BTN[i].label);
    }
    typeof(g_stick[0]) st[2]; pthread_mutex_lock(&g_smu); memcpy(st, g_stick, sizeof st); pthread_mutex_unlock(&g_smu);
    for (int k = 0; k < 2; k++) {
        if (g_ctl.hidden[G_LS + k] || (touch_mode() && !g_ctl.fixed[k])) continue;   // touch mode: no floating sticks
        float R = stick_r(k), kr = KNOB_R * g_vh * g_ctl.s[G_LS + k], x0 = st[k].x0, y0 = st[k].y0, ddx = 0, ddy = 0;
        if (g_ctl.fixed[k]) { x0 = g_ctl.x[G_LS + k] * g_vw; y0 = g_ctl.y[G_LS + k] * g_vh; } else if (!st[k].on) continue;
        if (st[k].on) { ddx = st[k].x - x0; ddy = st[k].y - y0; float m = hypotf(ddx, ddy); if (m > R) { ddx *= R / m; ddy *= R / m; } }
        if (g_spr_base >= 0 && g_spr_knob >= 0) {
            if (n < max) out[n++] = (dm_ovl){x0 - R, y0 - R, 2 * R, 2 * R, 0, "", g_spr_base, al};
            if (n < max) out[n++] = (dm_ovl){x0 + ddx - kr, y0 + ddy - kr, 2 * kr, 2 * kr, st[k].on, "", g_spr_knob, al};
            continue;
        }
        enum { RING_DOTS = 16 }; float dot = 0.008f * g_vh;
        for (int j = 0; j < RING_DOTS && n < max; j++) { float a = 6.2831853f * j / RING_DOTS; out[n++] = (dm_ovl){x0 + R * cosf(a) - dot, y0 + R * sinf(a) - dot, 2 * dot, 2 * dot, 1, "", -1, al}; }
        if (n < max) out[n++] = (dm_ovl){x0 + ddx - kr, y0 + ddy - kr, 2 * kr, 2 * kr, 1, "", -1, al};
    }
    return n;
}
// ---------- The game's touch screen ----------
// DM_TOUCH_MODE (the menu's "Touch screen"; default the profile's touch_mode, game.py): 0 = a finger on no button moves the floating stick of its half, as before (a finger outside
// every stick zone touches the game); 1 = the buttons and fixed sticks stay, every other finger touches the game; 2 = no on-screen controls, every finger touches the game.
// A finger keeps to what it came down on: one that started on a control never becomes a touch, and a touch never takes over a control. The window's pixels are mapped onto
// the picture as the backend letterboxes it (touch_map.h, dm_vk_picture); a finger that comes down on a black bar is ignored, one that slides onto it stays at the edge.
// The touch screen reaches the game in handheld mode only, as on the Switch (hle_hid.c touch_read)
int dm_vk_picture(int *w, int *h) __attribute__((weak));
#ifndef DM_GAME_TOUCH_MODE
#define DM_GAME_TOUCH_MODE 0
#endif
static int touch_mode(void) {
    static int m = -1;
    if (m < 0) { const char *e = getenv("DM_TOUCH_MODE"); int v = e && *e ? atoi(e) : DM_GAME_TOUCH_MODE; m = v < 0 || v > 2 ? 0 : v;
                 LOG("front end: touch mode %d (%s; %s)", m, m == 2 ? "every finger touches the game" : m ? "buttons, fixed sticks, the rest touches the game" : "floating sticks, as before",
                     e && *e ? "DM_TOUCH_MODE" : "the profile's touch_mode"); }
    return m;
}
static int g_ntouch_prev;
static int touch_map_win(float x, float y, int clamp, float *u, float *v) {
    int pw = 0, ph = 0, st = dm_vk_picture ? dm_vk_picture(&pw, &ph) : 0;
    return touch_map(x, y, g_vw, g_vh, pw, ph, st, clamp, u, v);
}
static void touch_log(const char *what, int32_t id, float x, float y, float u, float v, int gid) {   // one line a finger down: the first 40, then every 100th
    static int n; n++; if (n > 40 && n % 100) return;
    if (gid >= 0) LOG("touch #%d: finger %d down at %.0f,%.0f → the game's screen %.3f,%.3f (touch id %d)", n, (int)id, x, y, u, v, gid);
    else LOG("touch #%d: finger %d down at %.0f,%.0f: %s", n, (int)id, x, y, what);
}
static void touch_push(void) {   // every game finger to the host (dm_touch_set takes the whole set each time); nothing sent while none is down or lifted
    dm_touch t[NPTR]; int n = 0;
    for (int i = 0; i < g_nptr; i++) if (g_ptr[i].kind == PK_TOUCH) { float u, v; touch_map_win(g_ptr[i].x, g_ptr[i].y, 1, &u, &v); t[n++] = (dm_touch){(uint32_t)g_ptr[i].gid, u, v}; }
    if (n || g_ntouch_prev) dm_touch_set(n ? t : NULL, n);
    g_ntouch_prev = n;
}
static void touch_event(AInputEvent *e) {
    vb_layout();
    int tm = touch_mode();
    int32_t act = AMotionEvent_getAction(e), kind = act & AMOTION_EVENT_ACTION_MASK;
    size_t ai = (size_t)((act & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK) >> AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT);
    uint64_t tnow = now_ns();
    if (kind == AMOTION_EVENT_ACTION_UP || kind == AMOTION_EVENT_ACTION_CANCEL) {   // the last finger up: nothing is held; a stick's touch may have been the first of a double tap
        for (int i = 0; i < g_nptr; i++) if (g_ptr[i].kind == PK_LS || g_ptr[i].kind == PK_RS) { int k = g_ptr[i].kind - 2;
            stick_tap_up(&g_tap[k], tnow, kind == AMOTION_EVENT_ACTION_CANCEL ? 1.f : g_ptr[i].maxd / stick_r(k), g_ptr[i].click); }
        g_nptr = 0;
    }
    else {
        if (kind == AMOTION_EVENT_ACTION_DOWN || kind == AMOTION_EVENT_ACTION_POINTER_DOWN) {   // a new finger: a button, else the stick whose zone or place it is in (one finger per stick)
            int32_t id = AMotionEvent_getPointerId(e, ai); float x = AMotionEvent_getX(e, ai), y = AMotionEvent_getY(e, ai), x0 = x, y0 = y;
            if (kind == AMOTION_EVENT_ACTION_DOWN) g_nptr = 0;
            int k = tm == 2 ? PK_NONE : vb_hit(x, y) ? PK_BTN : stick_at(x, y, &x0, &y0, tm == 0);
            for (int i = 0; i < g_nptr && (k == PK_LS || k == PK_RS); i++) if (g_ptr[i].kind == k) k = PK_DEAD;   // that stick is held already: this finger does nothing
            int gid = -1; float u = 0, v = 0;
            if (k == PK_NONE) {   // on no control: the game's touch screen, when it is on the picture
                if (!touch_map_win(x, y, 0, &u, &v)) { k = PK_DEAD; touch_log("on a black bar beside the picture: ignored", id, x, y, 0, 0, -1); }
                else { int used = 0; for (int i = 0; i < g_nptr; i++) if (g_ptr[i].kind == PK_TOUCH) used |= 1 << g_ptr[i].gid;
                       for (gid = 0; gid < 16 && (used >> gid & 1); gid++) {}
                       if (gid < 16) { k = PK_TOUCH; touch_log(NULL, id, x, y, u, v, gid); } else { k = PK_DEAD; gid = -1; } }
            } else touch_log(k == PK_BTN ? "on a button" : k == PK_DEAD ? "on a stick another finger holds" : "on a stick", id, x, y, 0, 0, -1);
            int click = (k == PK_LS || k == PK_RS) && stick_tap_down(&g_tap[k - 2], tnow) && g_ctl.stick_click;   // the second touch of a double tap on that stick: its click while held
            if (g_nptr < NPTR) g_ptr[g_nptr++] = (typeof(g_ptr[0])){id, k, x0, y0, x, y, x, y, 0, click, gid};
        }
        if (kind == AMOTION_EVENT_ACTION_POINTER_UP) {
            int32_t id = AMotionEvent_getPointerId(e, ai);
            for (int i = 0; i < g_nptr; i++) if (g_ptr[i].id == id) {
                if (g_ptr[i].kind == PK_LS || g_ptr[i].kind == PK_RS) { int k = g_ptr[i].kind - 2; stick_tap_up(&g_tap[k], tnow, g_ptr[i].maxd / stick_r(k), g_ptr[i].click); }
                g_ptr[i] = g_ptr[--g_nptr]; break; }
        }
        for (size_t p = 0; p < AMotionEvent_getPointerCount(e); p++) {   // where every finger is now
            int32_t id = AMotionEvent_getPointerId(e, p);
            for (int i = 0; i < g_nptr; i++) if (g_ptr[i].id == id) { g_ptr[i].x = AMotionEvent_getX(e, p); g_ptr[i].y = AMotionEvent_getY(e, p);
                float d = hypotf(g_ptr[i].x - g_ptr[i].fx, g_ptr[i].y - g_ptr[i].fy); if (d > g_ptr[i].maxd) g_ptr[i].maxd = d; }
        }
    }
    uint64_t btn = 0; float ax[4] = {0}; typeof(g_stick[0]) st[2] = {{0}};
    for (int i = 0; i < g_nptr; i++) {
        if (g_ptr[i].kind == PK_BTN) { btn |= vb_hit(g_ptr[i].x, g_ptr[i].y); continue; }
        if (g_ptr[i].kind != PK_LS && g_ptr[i].kind != PK_RS) continue;
        int k = g_ptr[i].kind - 2; float ddx = g_ptr[i].x - g_ptr[i].x0, ddy = g_ptr[i].y - g_ptr[i].y0, m = hypotf(ddx, ddy), R = stick_r(k);
        st[k] = (typeof(st[0])){1, g_ptr[i].x0, g_ptr[i].y0, g_ptr[i].x, g_ptr[i].y};
        if (g_ptr[i].click) btn |= k ? DM_BTN_RS : DM_BTN_LS;   // a double tap held: the stick's click (it still moves the stick)
        if (m > R / 6) { float t = fminf(1.f, m / R); ax[2 * k] = ddx / m * t; ax[2 * k + 1] = ddy / m * t; }   // the dead zone: a sixth of the travel (0.02 h at scale 1, as before)
    }
    pthread_mutex_lock(&g_smu); memcpy(g_stick, st, sizeof st); pthread_mutex_unlock(&g_smu);
    if (g_touch_slot < 0) g_touch_slot = dev_slot(-1000000);
    g_dev[g_touch_slot].btn = btn;
    g_dev[g_touch_slot].ax[AX_LX] = ax[0]; g_dev[g_touch_slot].ax[AX_LY] = ax[1]; g_dev[g_touch_slot].ax[AX_RX] = ax[2]; g_dev[g_touch_slot].ax[AX_RY] = ax[3];
    atomic_store(&g_ovl_on, btn); if (tm != 2) atomic_store(&g_ovl_show, 1);
    touch_push();
    pads_push();
}
static void kbd_apply(void) {   // the keyboard's slot from its keys
    if (g_kbd_slot < 0) g_kbd_slot = dev_slot(-2000000);
    float *ax = g_dev[g_kbd_slot].ax; memset(ax, 0, sizeof g_dev[0].ax);
    ax[AX_LX] = (float)(g_kbd.d - g_kbd.a); ax[AX_LY] = (float)(g_kbd.s - g_kbd.w);   // full tilt; diagonals combine (the host clamps the vector)
    uint64_t b = g_kbd.btn;
    if (g_kbd.shift) { ax[AX_RX] = (float)(g_kbd.right - g_kbd.left); ax[AX_RY] = (float)(g_kbd.down - g_kbd.up); }
    else b |= (g_kbd.left ? DM_BTN_LEFT : 0) | (g_kbd.up ? DM_BTN_UP : 0) | (g_kbd.right ? DM_BTN_RIGHT : 0) | (g_kbd.down ? DM_BTN_DOWN : 0);
    g_dev[g_kbd_slot].btn = b;
}
static int kbd_event(AInputEvent *e) {   // 1: a key of the keyboard's map
    int32_t k = AKeyEvent_getKeyCode(e), act = AKeyEvent_getAction(e), down = act == AKEY_EVENT_ACTION_DOWN;
    if (act != AKEY_EVENT_ACTION_DOWN && act != AKEY_EVENT_ACTION_UP) return 0;
    int *f = NULL;
    switch (k) {
        case AKEYCODE_W: f = &g_kbd.w; break; case AKEYCODE_A: f = &g_kbd.a; break; case AKEYCODE_S: f = &g_kbd.s; break; case AKEYCODE_D: f = &g_kbd.d; break;
        case AKEYCODE_DPAD_LEFT: f = &g_kbd.left; break; case AKEYCODE_DPAD_UP: f = &g_kbd.up; break; case AKEYCODE_DPAD_RIGHT: f = &g_kbd.right; break; case AKEYCODE_DPAD_DOWN: f = &g_kbd.down; break;
        case AKEYCODE_SHIFT_LEFT: case AKEYCODE_SHIFT_RIGHT: f = &g_kbd.shift; break;
    }
    uint64_t m = f ? 0 : kbd_bit(k);
    if (!f && !m) return 0;
    if (down && AKeyEvent_getRepeatCount(e) > 0) return 1;   // auto-repeat: the key is held already
    if (f) *f = down; else g_kbd.btn = down ? g_kbd.btn | m : g_kbd.btn & ~m;
    if (f != &g_kbd.shift) g_kbd.shift = (AKeyEvent_getMetaState(e) & AMETA_SHIFT_ON) != 0;   // Shift released while the window had no focus
    kbd_apply(); pads_push(); return 1;
}
// Everything let go (the game suspends, or the window loses focus): a key or button released meanwhile sends this window no event, and would stay held
static void input_release_all(void) {
    pthread_mutex_lock(&g_pmu);
    for (int i = 0; i < NDEV; i++) { g_dev[i].btn = 0; memset(g_dev[i].ax, 0, sizeof g_dev[i].ax); }
    memset(&g_kbd, 0, sizeof g_kbd); g_latch = 0; g_combo_since = 0; g_nptr = 0;
    if (g_ntouch_prev) { g_ntouch_prev = 0; dm_touch_set(NULL, 0); }   // the game's touches lifted too
    pthread_mutex_lock(&g_smu); memset(g_stick, 0, sizeof g_stick); pthread_mutex_unlock(&g_smu);
    pads_push_();
    pthread_mutex_unlock(&g_pmu);
}
// The shader pre-warm (src_vk/nvn_vk.c dm_vk_prewarm): the pipelines earlier runs made are made again before the game starts, behind a progress screen; a tap skips it
int dm_vk_prewarm(void) __attribute__((weak)); void dm_vk_prewarm_cancel(void) __attribute__((weak)); void dm_vk_message(const char *t1, const char *t2) __attribute__((weak));
static _Atomic int g_prewarming;
static int on_input(AInputEvent *e) {
    int32_t type = AInputEvent_getType(e), src = AInputEvent_getSource(e);
    static int seen; if (!seen++) LOG("front end: first input event (type %d, source %#x)", type, src);
    if (atomic_load(&g_prewarming)) {   // the shader pre-warm screen: the player's tap (or a controller button) skips it
        if ((type == AINPUT_EVENT_TYPE_MOTION && (AMotionEvent_getAction(e) & AMOTION_EVENT_ACTION_MASK) == AMOTION_EVENT_ACTION_DOWN) || (type == AINPUT_EVENT_TYPE_KEY && AKeyEvent_getAction(e) == AKEY_EVENT_ACTION_DOWN))
            if (dm_vk_prewarm_cancel) dm_vk_prewarm_cancel();
        return 1;
    }
    int s = dev_slot(AInputEvent_getDeviceId(e));
    if (type == AINPUT_EVENT_TYPE_MOTION && (src & AINPUT_SOURCE_TOUCHSCREEN) == AINPUT_SOURCE_TOUCHSCREEN) { touch_event(e); return 1; }
    // A controller's events carry the gamepad, joystick or D-pad source bits; a keyboard's only the keyboard's (its arrows send the same DPAD keycodes as a pad)
    int pad = (src & (AINPUT_SOURCE_GAMEPAD | AINPUT_SOURCE_JOYSTICK)) == AINPUT_SOURCE_GAMEPAD || (src & AINPUT_SOURCE_JOYSTICK) == AINPUT_SOURCE_JOYSTICK || (src & AINPUT_SOURCE_DPAD) == AINPUT_SOURCE_DPAD;
    int kbd = !pad && type == AINPUT_EVENT_TYPE_KEY && (src & AINPUT_SOURCE_KEYBOARD) == AINPUT_SOURCE_KEYBOARD && AInputEvent_getDeviceId(e) > 0;   // not the on-screen keyboard (-1: injected)
    if (pad || kbd) atomic_store(&g_ovl_show, 0);   // a controller or keyboard in use: the on-screen controls go until the next touch
    if (kbd && kbd_event(e)) return 1;
    if (type == AINPUT_EVENT_TYPE_KEY) {
        int32_t k = AKeyEvent_getKeyCode(e);
        if (k == AKEYCODE_BUTTON_MODE || k == AKEYCODE_HOME) return 0;   // the system's own (home)
        uint64_t b = key_bit(k); if (!b) return k == AKEYCODE_BACK;
        if (AKeyEvent_getAction(e) == AKEY_EVENT_ACTION_DOWN) { if (AKeyEvent_getRepeatCount(e) > 0) return 1; g_dev[s].btn |= b; }
        else if (AKeyEvent_getAction(e) == AKEY_EVENT_ACTION_UP) g_dev[s].btn &= ~b;
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
// <files>/game_running is made when the host boots and removed when the player leaves (− and +, or the activity finished). A game process that dies any other way
// (a FATAL boot error, a crash: the crash handler ends with _exit, which Android takes for a plain process death) leaves it behind, and Android then starts the activity
// that was on top again in a new process at once -- the host failed again, a new process every 0.15 s. The new process finds the marker, does not boot, renames it to
// <files>/game_failed and finishes: the menu (MenuActivity) shows the error from log.txt. The menu's Play removes both before it starts the game
static char g_files[512];
static int g_booted;
static void marker_path(char *p, size_t n, const char *name) { snprintf(p, n, "%s/%s", g_files, name); }
static void game_left(void) { char p[600]; marker_path(p, sizeof p, "game_running"); unlink(p); }   // the player left: not a failure
static void game_failed(void) { char a[600], b[600]; marker_path(a, sizeof a, "game_running"); marker_path(b, sizeof b, "game_failed"); if (rename(a, b)) { int fd = open(b, O_WRONLY | O_CREAT, 0600); if (fd >= 0) close(fd); } }
static void back_to_menu(void) {
    static _Atomic int once; if (atomic_exchange(&once, 1)) return;
    if (dm_hib_forget_hook) dm_hib_forget_hook();   // the game ends here: no hibernation snapshot to resume (dm/host/hle_hibsnap.c)
    char p[600]; snprintf(p, sizeof p, "%s/show_menu", g_files); FILE *f = fopen(p, "w"); if (f) fclose(f);
    game_left();
    LOG("front end: − and + held: back to the menu");
    ANativeActivity_finish(g_act);
    dm_host_shutdown("back to the menu");
}

// The hibernation snapshot the menu resumed the game for was refused (removed by the host): back to the menu, quietly (show_menu keeps it from starting the game again)
static void hib_refused(void) {
    char p[600]; snprintf(p, sizeof p, "%s/show_menu", g_files); FILE *f = fopen(p, "w"); if (f) fclose(f);
    LOG("front end: the hibernation snapshot was refused: back to the menu");
    game_left();   // not a failure: the menu shows, not the error
    ANativeActivity_finish(g_act);
    dm_host_shutdown("hibernation snapshot refused");
}
// ---------- Boot: environment from the menu's settings, log, then the host on its own thread ----------
static char *g_argv[4];
static void boot_env(const char *files, const char *ext) {
    char p[1024];
    snprintf(p, sizeof p, "%s/log.txt", ext);
    // the last runs' logs: log.prev.txt (the run before), log.prev2.txt, log.prev3.txt -- a long play session is not pushed out by a couple of short test starts after it
    char prev[1024], older[1024];
    for (int k = 3; k > 1; k--) {   // log.prev2 -> log.prev3, log.prev -> log.prev2
        if (k == 2) snprintf(prev, sizeof prev, "%s/log.prev.txt", ext); else snprintf(prev, sizeof prev, "%s/log.prev%d.txt", ext, k - 1);
        snprintf(older, sizeof older, "%s/log.prev%d.txt", ext, k); rename(prev, older);
    }
    snprintf(prev, sizeof prev, "%s/log.prev.txt", ext); rename(p, prev);
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
    // DM_HIBERNATE: a snapshot the last process wrote and did not come back from (dm/host/hle_hibsnap.c keeps it in <files>/hibernate) is where the game resumes
    snprintf(p, sizeof p, "%s/hibernate/snap.bin", files);
    int hib_on = getenv("DM_HIBERNATE") && atoi(getenv("DM_HIBERNATE")) > 0, snap = !access(p, R_OK);
    if (hib_on && snap) {
        setenv("DM_HIB_RESTORE", p, 0); dm_hib_refused_hook = hib_refused;   // a snapshot the host refuses (another build, damaged) is removed, and the menu shows
        fprintf(stderr, "boot: hibernate: snapshot %s found, DM_HIBERNATE on: restoring it\n", p);
    } else fprintf(stderr, "boot: hibernate: %s, %s: no restore\n", hib_on ? "DM_HIBERNATE on" : "DM_HIBERNATE off", snap ? "a snapshot exists (left as it is)" : "no snapshot");
    // With hibernation, <external>/hib_ctl.txt takes DM_CTL's "hib" commands from adb (dm/android/tools/hib_install.sh writes "hib freeze" before an update installs over the
    // game): made by the app, so that the shell's appends reach it (a file adb makes there is the shell's, and the app cannot read it). Other DM_CTL commands are not taken from it
    if (hib_on && !getenv("DM_CTL")) {
        snprintf(p, sizeof p, "%s/hib_ctl.txt", ext); FILE *c = fopen(p, "w");
        if (c) { fclose(c); chmod(p, 0666); setenv("DM_CTL", p, 1); setenv("DM_CTL_HIB_ONLY", "1", 1); fprintf(stderr, "boot: hibernate: control file %s (hib commands only)\n", p); }
    }
    chdir(files);
    g_argv[0] = strdup("dmhost"); snprintf(p, sizeof p, "%s/dm", ext); g_argv[1] = strdup(p);
}
// The game files as this process sees them (its own view of Android/data: a bind mount of the lower file system or the FUSE one), so a file the host cannot open shows
// why in the log: owner, group, mode, and the error of an open as the app
static void boot_files_check(const char *ext) {
    gid_t g[32]; int ng = getgroups(32, g); char gs[256] = ""; size_t o = 0;
    for (int i = 0; i < ng && o < sizeof gs - 12; i++) o += snprintf(gs + o, sizeof gs - o, " %u", (unsigned)g[i]);
    fprintf(stderr, "boot: files as uid %u gid %u groups%s\n", (unsigned)getuid(), (unsigned)getgid(), gs);
    const char *rel[] = {"", "/dm", "/dm/main.fixups.tsv", "/dm/main.patched.bin", "/romfs", "/vk", "/font"}; int bad = 0;
    for (size_t i = 0; i < sizeof rel / sizeof *rel; i++) {
        char p[1024]; snprintf(p, sizeof p, "%s%s", ext, rel[i]); struct stat st;
        if (stat(p, &st)) { fprintf(stderr, "boot: [RED] %s: stat: %s\n", p, strerror(errno)); bad = 1; continue; }
        int fd = open(p, O_RDONLY | (S_ISDIR(st.st_mode) ? O_DIRECTORY : 0)), e = errno;
        if (fd >= 0) close(fd); else bad = 1;
        int n = -1; if (S_ISDIR(st.st_mode)) { DIR *d = opendir(p); if (d) { n = 0; while (readdir(d)) n++; closedir(d); } }
        fprintf(stderr, "boot: %s%s: uid %u gid %u mode %o size %lld entries %d, open %s\n", fd < 0 ? "[RED] " : "", p, (unsigned)st.st_uid, (unsigned)st.st_gid,
                (unsigned)(st.st_mode & 07777), (long long)st.st_size, n, fd >= 0 ? "ok" : strerror(e));
    }
    if (!bad) return;
    FILE *m = fopen("/proc/self/mountinfo", "r"); char ln[1024];   // which file system serves the external folder
    if (m) { while (fgets(ln, sizeof ln, m)) if (strstr(ln, "/storage/emulated") || strstr(ln, "Android/data")) fprintf(stderr, "boot: mount %s", ln); fclose(m); }
}
// The shader pre-warm (src_vk/nvn_vk.c dm_vk_prewarm): the pipelines earlier runs made are made again before the game starts, behind a progress screen; a tap skips it
static void *boot_thread(void *a) {
    (void)a;
    static char alt[65536]; stack_t ss = {.ss_sp = alt, .ss_size = sizeof alt}; sigaltstack(&ss, 0);
    if (dm_vk_prewarm) { atomic_store(&g_prewarming, 1); dm_vk_prewarm(); atomic_store(&g_prewarming, 0); }
    if (getenv("DM_HIB_RESTORE") && dm_vk_message) dm_vk_message("MELANJUTKAN...", "DARI HIBERNASI");   // the snapshot takes seconds to read: the player sees why the screen waits
    int rc = dm_main(2, g_argv);
    fprintf(stderr, "boot: dm_main returned %d\n", rc);
    game_failed(); ANativeActivity_finish(g_act);   // the host is over: back to the menu, which shows the log's error
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
static void panel_read_activity(ANativeActivity *a);
static void on_window_created(ANativeActivity *a, ANativeWindow *w) { panel_read_activity(a); pthread_mutex_lock(&g_wmu); g_win = w; atomic_fetch_add(&g_win_gen, 1); pthread_mutex_unlock(&g_wmu); LOG("front end: window %dx%d", ANativeWindow_getWidth(w), ANativeWindow_getHeight(w)); }
// The window goes when the activity stops (screen off, another app): the swapchain and surface are released with it, before Android takes the window back
static void on_window_destroyed(ANativeActivity *a, ANativeWindow *w) { (void)a; (void)w; pthread_mutex_lock(&g_wmu); g_win = NULL; atomic_fetch_add(&g_win_gen, 1); dm_vk_window_lost(); pthread_mutex_unlock(&g_wmu); }
static void on_window_resized(ANativeActivity *a, ANativeWindow *w) { (void)a; (void)w; atomic_fetch_add(&g_win_gen, 1); }
static void on_input_created(ANativeActivity *a, AInputQueue *q) { (void)a; g_iq = q; AInputQueue_attachLooper(q, ALooper_forThread(), 1, input_cb, NULL); }
static void on_input_destroyed(ANativeActivity *a, AInputQueue *q) { (void)a; AInputQueue_detachLooper(q); g_iq = NULL; }
// ---------- Suspend and resume, as a Switch going to sleep ----------
// The activity stopping (the screen off, home, another app) suspends the game the way the iOS front end does for the background and Horizon for sleep: the game gets
// OutOfFocus then Background (oe focus notifications), audio stops (AAudio), the guest clock is frozen (no time passes for its timers), and the graphics gate closes
// (dm_gfx_suspend: the game's next submit or present blocks there, so its threads stall at their next frame, and in-flight GPU work is drained). The process stays, kept
// by GameService (a foreground service with an ongoing notification), with every game resource in memory. The activity starting again reverses it in the opposite order;
// focus comes back with the window focus. Before this the game ran on unseen in the background, its frames dropped for want of a window
static pthread_mutex_t g_lmu = PTHREAD_MUTEX_INITIALIZER; static int g_suspended;
static void game_suspend(int on) {
    pthread_mutex_lock(&g_lmu);
    if (on != g_suspended) {
        g_suspended = on;
        if (on) { input_release_all(); dm_focus_set(2); dm_focus_set(3); dm_audio_suspend(1); guest_clock_freeze_for(1, 1); dm_gfx_suspend(1); dm_hib_suspend(1); LOG("front end: suspended (the activity stopped)"); }   // dm_hib_suspend: DM_HIBERNATE's snapshot, written on a thread of its own (dm/host/hle_hibsnap.c)
        else { dm_hib_suspend(0); dm_gfx_suspend(0); guest_clock_freeze_for(1, 0); dm_audio_suspend(0); dm_focus_set(2); LOG("front end: resumed (the activity started)"); }
    }
    pthread_mutex_unlock(&g_lmu);
}
// GameService's static methods (app/java/com/nxnative/GameService.java), through the activity's class loader (FindClass on this thread sees the system classes only)
static void game_service(ANativeActivity *a, const char *method, int arg) {
    JNIEnv *env = a->env;
    jobject ld = (*env)->CallObjectMethod(env, a->clazz, (*env)->GetMethodID(env, (*env)->GetObjectClass(env, a->clazz), "getClassLoader", "()Ljava/lang/ClassLoader;"));
    jstring nm = (*env)->NewStringUTF(env, "com.nxnative.GameService");
    jclass cl = ld ? (jclass)(*env)->CallObjectMethod(env, ld, (*env)->GetMethodID(env, (*env)->GetObjectClass(env, ld), "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;"), nm) : NULL;
    if (cl && !(*env)->ExceptionCheck(env)) {
        int set = !strcmp(method, "setSuspended");
        jmethodID m = (*env)->GetStaticMethodID(env, cl, method, set ? "(Landroid/content/Context;Z)V" : "(Landroid/content/Context;)V");
        if (m && !(*env)->ExceptionCheck(env)) { if (set) (*env)->CallStaticVoidMethod(env, cl, m, a->clazz, (jboolean)(arg != 0)); else (*env)->CallStaticVoidMethod(env, cl, m, a->clazz); }
    }
    if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); LOG("[RED] front end: GameService.%s failed", method); }
    (*env)->DeleteLocalRef(env, nm);
}
static void on_start(ANativeActivity *a) { game_suspend(0); game_service(a, "setSuspended", 0); }
static void on_stop(ANativeActivity *a) { game_suspend(1); game_service(a, "setSuspended", 1); }
static void game_mode_read(ANativeActivity *a);
static void on_resume(ANativeActivity *a) { game_mode_read(a); }
// Android is short of memory (other apps are being killed): free pages go back to the system now, and the graphics backend's rings shrink to their minimum (dm_vk_trim)
void dm_vk_trim(void) __attribute__((weak));
static void on_low_memory(ANativeActivity *a) { (void)a; LOG("front end: low memory: freeing what can be freed"); mallopt(M_PURGE, 0); if (dm_vk_trim) dm_vk_trim(); }
// onTrimMemory (TrimWatch.java): RUNNING_LOW (10) and up purge as onLowMemory does; RUNNING_MODERATE (5) is only logged
static void JNICALL trim_native(JNIEnv *env, jclass cl, jint level) {
    (void)env; (void)cl; static int nl;
    if (nl++ < 30) LOG("front end: onTrimMemory level %d%s", (int)level, level >= 10 ? ": freeing what can be freed" : "");
    if (level >= 10) { mallopt(M_PURGE, 0); if (dm_vk_trim) dm_vk_trim(); }
}
static void trim_watch(ANativeActivity *a) {   // TrimWatch.register(activity), through the activity's class loader, with nativeTrim bound to trim_native
    JNIEnv *env = a->env;
    jobject ld = (*env)->CallObjectMethod(env, a->clazz, (*env)->GetMethodID(env, (*env)->GetObjectClass(env, a->clazz), "getClassLoader", "()Ljava/lang/ClassLoader;"));
    jstring nm = (*env)->NewStringUTF(env, "com.nxnative.TrimWatch");
    jclass cl = ld ? (jclass)(*env)->CallObjectMethod(env, ld, (*env)->GetMethodID(env, (*env)->GetObjectClass(env, ld), "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;"), nm) : NULL;
    if (cl && !(*env)->ExceptionCheck(env)) {
        JNINativeMethod m = {"nativeTrim", "(I)V", (void *)trim_native};
        if ((*env)->RegisterNatives(env, cl, &m, 1) == 0) {
            jmethodID r = (*env)->GetStaticMethodID(env, cl, "register", "(Landroid/content/Context;)V");
            if (r && !(*env)->ExceptionCheck(env)) (*env)->CallStaticVoidMethod(env, cl, r, a->clazz);
        }
    }
    if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); LOG("[RED] front end: TrimWatch not registered: only onLowMemory reaches the host"); }
    else LOG("front end: onTrimMemory watched (TrimWatch)");
    (*env)->DeleteLocalRef(env, nm);
}
// The Android game mode (GameManager.getGameMode, API 31: 0 unsupported, 1 standard, 2 performance, 3 battery, 4 custom), read at start and on each resume into dm_game_mode for the
// backend's clock line. The manifest declares the performance and battery modes (game_mode_config) so the system's game settings can choose them for this app
extern int dm_game_mode __attribute__((weak));
static void game_mode_read(ANativeActivity *a) {
    if (!&dm_game_mode) return;
    JNIEnv *env = a->env; jstring nm = (*env)->NewStringUTF(env, "game");
    jobject gm = (*env)->CallObjectMethod(env, a->clazz, (*env)->GetMethodID(env, (*env)->GetObjectClass(env, a->clazz), "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;"), nm);
    int mode = -1;
    if (gm && !(*env)->ExceptionCheck(env)) { jmethodID m = (*env)->GetMethodID(env, (*env)->GetObjectClass(env, gm), "getGameMode", "()I"); if (m && !(*env)->ExceptionCheck(env)) mode = (*env)->CallIntMethod(env, gm, m); }
    if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); mode = -1; }
    (*env)->DeleteLocalRef(env, nm);
    if (mode != dm_game_mode) LOG("front end: Android game mode %d (0 unsupported, 1 standard, 2 performance, 3 battery, 4 custom)", mode);
    dm_game_mode = mode;
}
// The battery for the backend's heat-and-energy line (vk19.59, src_vk/vk_stats.c), from its sampling thread dm-stats only (never a game, guest or RHI thread; at most once a
// second): BatteryManager's CHARGE_COUNTER and CURRENT_NOW (binder calls into the health HAL, which can block for milliseconds: hence that thread), and the sticky
// ACTION_BATTERY_CHANGED intent's voltage (mV) and plugged state when want_v. It uses the JavaVM and a global reference to the application context, both taken once on the
// activity thread at onCreate (battery_init_activity): never g_act, whose ANativeActivity is freed when Android destroys the activity while the process stays (vk19.59c).
// The thread attaches once, as a daemon. Every JNI call is followed by an exception check; a failed setup is not tried again. Returns 0 when there is nothing to read
static JavaVM *g_vm; static jobject g_appctx;
static JNIEnv *stats_env(void) {   // dm-stats only: attached once as a daemon (0 when there is no VM)
    static JNIEnv *env; static int tried;
    if (!env && !tried) { tried = 1; JavaVMAttachArgs aa = {JNI_VERSION_1_6, "dm-stats", NULL}; if (!g_vm || (*g_vm)->AttachCurrentThreadAsDaemon(g_vm, &env, &aa) != JNI_OK) env = NULL; }
    return env;
}
static void battery_init_activity(ANativeActivity *a) {   // the activity thread, first onCreate of the process
    JNIEnv *env = a->env; g_vm = a->vm;
    jmethodID m = (*env)->GetMethodID(env, (*env)->GetObjectClass(env, a->clazz), "getApplicationContext", "()Landroid/content/Context;");
    jobject c = !(*env)->ExceptionCheck(env) && m ? (*env)->CallObjectMethod(env, a->clazz, m) : NULL;
    if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); c = NULL; }
    if (c) { g_appctx = (*env)->NewGlobalRef(env, c); (*env)->DeleteLocalRef(env, c); }
}
#define JX(env) ((*(env))->ExceptionCheck(env) ? ((*(env))->ExceptionClear(env), 1) : 0)
// The panel's refresh rate as reported (vk19.63, DM_VK_PRESENT_PACE; only with the switch on): Display.getRefreshRate through DisplayManager, in millihertz. The activity thread reads it when a window is
// made (panel_read_activity: the backend's swapchain chooses FIFO from it); dm-stats reads it again once a window (dm_front_panel_mhz_now: the rate actually granted after
// the backend's setFrameRate request, for the heat-and-energy line). No game, guest or RHI thread calls into Java
static _Atomic int g_panel_mhz;
static int pace_on(void) { const char *e = getenv("DM_VK_PRESENT_PACE"); return e && atoi(e) != 0; }   // off: no panel query at all (vk19.63c)
// Display.getRefreshRate of the default display through DisplayManager.getDisplay(DEFAULT_DISPLAY) (not the window manager of a non-visual context: StrictMode), inside a
// local frame so a long-lived thread keeps no local references (vk19.63c). The rate as reported: it can be the app's own requested rate rather than the panel's mode
static int panel_query(JNIEnv *env, jobject ctx, int log_modes) {
    if ((*env)->PushLocalFrame(env, 16) < 0) { (*env)->ExceptionClear(env); return 0; }
    int mhz = 0;
#define PQ_BAD() ((*env)->ExceptionCheck(env) ? ((*env)->ExceptionClear(env), 1) : 0)
    jstring nm = (*env)->NewStringUTF(env, "display"); if (PQ_BAD() || !nm) goto out;
    jmethodID gss = (*env)->GetMethodID(env, (*env)->GetObjectClass(env, ctx), "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;"); if (PQ_BAD() || !gss) goto out;
    jobject dm = (*env)->CallObjectMethod(env, ctx, gss, nm); if (PQ_BAD() || !dm) goto out;
    jmethodID gd = (*env)->GetMethodID(env, (*env)->GetObjectClass(env, dm), "getDisplay", "(I)Landroid/view/Display;"); if (PQ_BAD() || !gd) goto out;
    jobject d = (*env)->CallObjectMethod(env, dm, gd, 0); if (PQ_BAD() || !d) goto out;   // Display.DEFAULT_DISPLAY
    jclass dc = (*env)->GetObjectClass(env, d); jmethodID gr = (*env)->GetMethodID(env, dc, "getRefreshRate", "()F"); if (PQ_BAD() || !gr) goto out;
    { jfloat r = (*env)->CallFloatMethod(env, d, gr); if (!PQ_BAD()) mhz = (int)(r * 1000.0f + 0.5f); }
    if (log_modes) {   // the panel's modes, once (Display.getSupportedModes, API 23)
        jmethodID gm = (*env)->GetMethodID(env, dc, "getSupportedModes", "()[Landroid/view/Display$Mode;"); jobjectArray ms = !PQ_BAD() && gm ? (*env)->CallObjectMethod(env, d, gm) : NULL;
        if (PQ_BAD()) ms = NULL;
        char b[256] = ""; size_t o = 0; jsize n = ms ? (*env)->GetArrayLength(env, ms) : 0;
        for (jsize i = 0; i < n && i < 24 && o < sizeof b - 12; i++) { jobject m = (*env)->GetObjectArrayElement(env, ms, i); if (PQ_BAD() || !m) continue;
            jmethodID mr = (*env)->GetMethodID(env, (*env)->GetObjectClass(env, m), "getRefreshRate", "()F"); jfloat r = !PQ_BAD() && mr ? (*env)->CallFloatMethod(env, m, mr) : 0;
            if (PQ_BAD()) r = 0;
            o += (size_t)snprintf(b + o, sizeof b - o, "%s%.0f", o ? " " : "", (double)r); (*env)->DeleteLocalRef(env, m); }
        LOG("front end: panel refresh rate %.2f Hz reported now; modes %s Hz", mhz / 1000.0, o ? b : "?");
    }
out:
#undef PQ_BAD
    (*env)->PopLocalFrame(env, NULL);
    return mhz;
}
static void panel_read_activity(ANativeActivity *a) { if (!pace_on()) return; static int logged; int m = panel_query(a->env, a->clazz, !logged++); if (m) atomic_store(&g_panel_mhz, m); }
__attribute__((visibility("default"))) int dm_front_panel_mhz(void) { return atomic_load(&g_panel_mhz); }   // the activity thread's last reading (any thread may read it)
__attribute__((visibility("default"))) int dm_front_panel_mhz_now(void) { if (!pace_on()) return 0; JNIEnv *env = g_appctx ? stats_env() : NULL; return env ? panel_query(env, g_appctx, 0) : 0; }   // dm-stats only
__attribute__((visibility("default"))) int dm_front_battery(long *charge_uah, long *current_ua, int *mv, int *plugged, int want_v) {
    static int state; static JNIEnv *env; static jobject bm, s_act, s_volt, s_plug; static jclass ifc; static jmethodID get_int, if_new, reg, get_extra;
    if (state < 0) return 0;
    if (!state) {
        state = -1;   // until the whole setup has worked: one try only
        if (!g_vm || !g_appctx || !(env = stats_env())) return 0;
        jclass cc = (*env)->GetObjectClass(env, g_appctx); if (JX(env) || !cc) return 0;
        jmethodID gss = (*env)->GetMethodID(env, cc, "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;"); if (JX(env) || !gss) return 0;
        reg = (*env)->GetMethodID(env, cc, "registerReceiver", "(Landroid/content/BroadcastReceiver;Landroid/content/IntentFilter;)Landroid/content/Intent;"); if (JX(env) || !reg) return 0;
        jstring nm = (*env)->NewStringUTF(env, "batterymanager"); if (JX(env) || !nm) return 0;
        jobject o = (*env)->CallObjectMethod(env, g_appctx, gss, nm); (*env)->DeleteLocalRef(env, nm); if (JX(env) || !o) return 0;
        get_int = (*env)->GetMethodID(env, (*env)->GetObjectClass(env, o), "getIntProperty", "(I)I"); if (JX(env) || !get_int) return 0;
        jclass fc = (*env)->FindClass(env, "android/content/IntentFilter"); if (JX(env) || !fc) return 0;
        if_new = (*env)->GetMethodID(env, fc, "<init>", "(Ljava/lang/String;)V"); if (JX(env) || !if_new) return 0;
        jclass ic = (*env)->FindClass(env, "android/content/Intent"); if (JX(env) || !ic) return 0;
        get_extra = (*env)->GetMethodID(env, ic, "getIntExtra", "(Ljava/lang/String;I)I"); if (JX(env) || !get_extra) return 0;
        jstring a1 = (*env)->NewStringUTF(env, "android.intent.action.BATTERY_CHANGED"), a2 = (*env)->NewStringUTF(env, "voltage"), a3 = (*env)->NewStringUTF(env, "plugged");
        if (JX(env) || !a1 || !a2 || !a3) return 0;
        // every lookup worked: the global references, made once
        bm = (*env)->NewGlobalRef(env, o); ifc = (*env)->NewGlobalRef(env, fc); s_act = (*env)->NewGlobalRef(env, a1); s_volt = (*env)->NewGlobalRef(env, a2); s_plug = (*env)->NewGlobalRef(env, a3);
        (*env)->DeleteLocalRef(env, o); (*env)->DeleteLocalRef(env, fc); (*env)->DeleteLocalRef(env, ic); (*env)->DeleteLocalRef(env, a1); (*env)->DeleteLocalRef(env, a2); (*env)->DeleteLocalRef(env, a3);
        if (!bm || !ifc || !s_act || !s_volt || !s_plug) return 0;
        state = 1;
    }
    jint q = (*env)->CallIntMethod(env, bm, get_int, 1); if (JX(env)) return 0;   // BATTERY_PROPERTY_CHARGE_COUNTER
    jint i = (*env)->CallIntMethod(env, bm, get_int, 2); if (JX(env)) return 0;   // BATTERY_PROPERTY_CURRENT_NOW
    *charge_uah = q; *current_ua = i;
    if (want_v) {
        jobject f = (*env)->NewObject(env, ifc, if_new, s_act); if (JX(env)) f = NULL;
        jobject in = f ? (*env)->CallObjectMethod(env, g_appctx, reg, NULL, f) : NULL; if (JX(env)) in = NULL;
        if (in) { jint v = (*env)->CallIntMethod(env, in, get_extra, s_volt, -1); if (!JX(env)) *mv = v;
                  jint p = (*env)->CallIntMethod(env, in, get_extra, s_plug, -1); if (!JX(env)) *plugged = p; (*env)->DeleteLocalRef(env, in); }
        if (f) (*env)->DeleteLocalRef(env, f);
    }
    return 1;
}
#undef JX
static void on_pause(ANativeActivity *a) { (void)a; dm_focus_set(2); }   // still visible behind something (a dialog, a split screen): out of focus, running
static void on_focus(ANativeActivity *a, int f) { if (f) immersive(a); else input_release_all(); dm_focus_set(f ? 1 : 2); }
// Destroyed by the player leaving (− and + held, back_to_menu, finish) the game ends; destroyed by Android while the process stays (to reclaim memory, its activity
// only), the game stays suspended and a new activity takes over its window (ANativeActivity_onCreate: the host runs once per process)
static void on_destroy(ANativeActivity *a) {
    if (!g_booted) return;   // an activity that did not boot (relaunched by Android after the game process died): nothing to end
    JNIEnv *env = a->env;
    jboolean fin = (*env)->CallBooleanMethod(env, a->clazz, (*env)->GetMethodID(env, (*env)->GetObjectClass(env, a->clazz), "isFinishing", "()Z"));
    if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); fin = 1; }
    // finished without the − and + hold (a recents swipe, the task removed): the hibernation snapshot the stop began is written to the end and kept; only back_to_menu drops it
    if (fin) { if (dm_hib_flush_hook) dm_hib_flush_hook(); game_left(); dm_host_shutdown("activity destroyed"); }
    else LOG("front end: the activity was destroyed by Android, not finished: the game stays suspended");
}

__attribute__((visibility("default"))) void ANativeActivity_onCreate(ANativeActivity *a, void *saved, size_t saved_n) {
    (void)saved; (void)saved_n;
    g_act = a;
    a->callbacks->onNativeWindowCreated = on_window_created; a->callbacks->onNativeWindowDestroyed = on_window_destroyed;
    a->callbacks->onNativeWindowResized = on_window_resized;
    a->callbacks->onInputQueueCreated = on_input_created; a->callbacks->onInputQueueDestroyed = on_input_destroyed;
    a->callbacks->onResume = on_resume; a->callbacks->onPause = on_pause; a->callbacks->onWindowFocusChanged = on_focus; a->callbacks->onDestroy = on_destroy;
    a->callbacks->onStart = on_start; a->callbacks->onStop = on_stop; a->callbacks->onLowMemory = on_low_memory;
    ANativeActivity_setWindowFlags(a, 0x80 /* KEEP_SCREEN_ON */ | 0x400 /* FULLSCREEN */, 0);
    immersive(a);
    if (g_booted) return;   // the activity can be made again (rotation, a return): the host runs once per process
    trim_watch(a); game_mode_read(a); battery_init_activity(a);
    snprintf(g_files, sizeof g_files, "%s", a->internalDataPath);
    mallopt(M_DECAY_TIME, 1);   // freed memory (Unreal's heap runs on host malloc) goes back to the system after about a second instead of being kept
    char mk[600]; marker_path(mk, sizeof mk, "game_running");
    if (!access(mk, F_OK)) {   // the last game process died without the player leaving and Android started this activity again by itself: no boot, back to the menu
        game_failed(); __android_log_print(ANDROID_LOG_WARN, "nxnative", "the game process died at its last run: not booting again, back to the menu");
        ANativeActivity_finish(a); return;
    }
    g_booted = 1;
    { int fd = open(mk, O_WRONLY | O_CREAT | O_TRUNC, 0600); if (fd >= 0) close(fd); }
    game_service(a, "start", 0);   // in the foreground now, as starting a foreground service requires
    boot_env(a->internalDataPath, a->externalDataPath);
    boot_files_check(a->externalDataPath);
    {   // the menu's save backup for this start (MenuActivity.play writes <files>/play_token after SaveBackup): said in the log, consumed
        char tp[600], ln[512] = ""; marker_path(tp, sizeof tp, "play_token"); FILE *t = fopen(tp, "r");
        if (t) { if (!fgets(ln, sizeof ln, t)) ln[0] = 0; fclose(t); unlink(tp); ln[strcspn(ln, "\r\n")] = 0; fprintf(stderr, "boot: save backup before this start: %s\n", ln); }
        else fprintf(stderr, "boot: [RED] started without the menu's Play (no play_token): no save backup made for this start\n");
    }
    pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setstacksize(&at, 16 << 20);
    pthread_t th; if (pthread_create(&th, &at, boot_thread, NULL)) die("front end: cannot start the host");
    pthread_detach(th);
    if (!pthread_create(&th, NULL, latch_thread, NULL)) pthread_detach(th);
}
