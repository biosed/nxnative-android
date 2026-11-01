// Android app front end (front.h; Mac: front_mac.m, iOS: dm/ios): the native half of the app, a NativeActivity in a process of its own (":game"). The start-up menu
// (MenuActivity.java, in the app's main process) writes the player's choices to <files>/settings.env and starts this activity; here the environment is set from it, the
// host runs on a big-stack thread, the window goes to the Vulkan backend (front_native_window), and controllers -- the console's own and any other, an Xbox one over
// USB or Bluetooth -- reach the game through dm_pad_set, by position as on the Mac and iOS (bottom B, right A, top X, left Y).
// Phones without a controller get on-screen controls (front_overlay, drawn by the Vulkan backend over the picture: buttons and floating sticks): they show at the first
// touch and hide again when a controller is used. Holding − and + together for two seconds goes back to the menu: the game process ends (progress the game has saved is kept) and the menu opens with the options shown.
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
}
// ---------- On-screen controls ----------
// Laid out from the window's height h: A B X Y bottom right (by position, as on the Switch), the D-pad as four buttons bottom left, L / ZL and R / ZR in the top corners,
// − and + top centre. The sticks float, as in mobile games: a finger that comes down anywhere else is a stick, the left one in the left half of the screen and the right one
// (FINAL FANTASY XII THE ZODIAC AGE's camera) in the right half. Where it came down is the stick's centre, its offset from there the tilt (full at 0.12 h, a small dead
// zone), and lifting it lets the stick go back; a faint ring and a knob show under the finger while it is down. Each finger keeps to what it started on (a stick, or the
// buttons it is over), so both sticks and buttons work together
typedef struct { float x, y, w, h; int on; char label[3]; } dm_ovl;
enum { NVB = 14, NPTR = 10, RING_DOTS = 16 };
static struct { const char *label; uint64_t bit; float cx, cy; } g_vb[NVB];
static _Atomic int g_ovl_show; static _Atomic uint64_t g_ovl_on; static float g_vw, g_vh, g_vr, g_dr;   // g_dr: the D-pad buttons' radius
// Fingers down (by pointer ID): kind 1 buttons, 2 left stick, 3 right stick; x0 / y0 where it came down, x / y where it is
static struct { int32_t id; int kind; float x0, y0, x, y; } g_ptr[NPTR]; static int g_nptr;
static struct { int on; float x0, y0, x, y; } g_stick[2];   // what front_overlay draws (copied under g_smu)
static pthread_mutex_t g_smu = PTHREAD_MUTEX_INITIALIZER;
static void vb_layout(void) {
    ANativeWindow *w = front_native_window(NULL); if (!w) return;
    float W_ = (float)ANativeWindow_getWidth(w), H = (float)ANativeWindow_getHeight(w);
    if (W_ == g_vw && H == g_vh) return;
    g_vw = W_; g_vh = H; g_vr = 0.065f * H; g_dr = 0.05f * H; float o = 0.12f * H, fx = W_ - 0.24f * H, fy = H - 0.25f * H, dx = 0.22f * H, dy = H - 0.25f * H, d = 0.1f * H;
    int i = 0;
    #define VB(l, b, x, y) g_vb[i++] = (typeof(g_vb[0])){l, b, x, y}
    VB("A", DM_BTN_A, fx + o, fy); VB("B", DM_BTN_B, fx, fy + o); VB("X", DM_BTN_X, fx, fy - o); VB("Y", DM_BTN_Y, fx - o, fy);
    VB("L", DM_BTN_L, 0.11f * H, 0.11f * H); VB("ZL", DM_BTN_ZL, 0.11f * H, 0.28f * H); VB("R", DM_BTN_R, W_ - 0.11f * H, 0.11f * H); VB("ZR", DM_BTN_ZR, W_ - 0.11f * H, 0.28f * H);
    VB("-", DM_BTN_MINUS, W_ / 2 - 0.1f * H, 0.08f * H); VB("+", DM_BTN_PLUS, W_ / 2 + 0.1f * H, 0.08f * H);
    VB("", DM_BTN_UP, dx, dy - d); VB("", DM_BTN_DOWN, dx, dy + d); VB("", DM_BTN_LEFT, dx - d, dy); VB("", DM_BTN_RIGHT, dx + d, dy);
    #undef VB
}
static float vb_r(int i) { return g_vb[i].bit & (DM_BTN_UP | DM_BTN_DOWN | DM_BTN_LEFT | DM_BTN_RIGHT) ? g_dr : g_vr; }
static uint64_t vb_hit(float x, float y) {   // the buttons a finger at (x, y) is over
    uint64_t b = 0; for (int i = 0; i < NVB; i++) if (fabsf(x - g_vb[i].cx) < 1.3f * vb_r(i) && fabsf(y - g_vb[i].cy) < 1.3f * vb_r(i)) b |= g_vb[i].bit;
    return b;
}
// What the backend draws over the picture, in window pixels (0 = hidden): a box per button; a ring of dots and a knob per stick held
int front_overlay(dm_ovl *out, int max) {
    if (!atomic_load(&g_ovl_show)) return 0;
    vb_layout(); uint64_t on = atomic_load(&g_ovl_on); int n = 0;
    for (int i = 0; i < NVB && n < max; i++) {
        dm_ovl *b = &out[n++]; float r = vb_r(i);
        *b = (dm_ovl){g_vb[i].cx - r, g_vb[i].cy - r, 2 * r, 2 * r, (on & g_vb[i].bit) != 0, ""};
        snprintf(b->label, sizeof b->label, "%s", g_vb[i].label);
    }
    typeof(g_stick[0]) st[2]; pthread_mutex_lock(&g_smu); memcpy(st, g_stick, sizeof st); pthread_mutex_unlock(&g_smu);
    float R = 0.12f * g_vh, dot = 0.008f * g_vh, kr = 0.045f * g_vh;
    for (int k = 0; k < 2; k++) {
        if (!st[k].on) continue;
        for (int j = 0; j < RING_DOTS && n < max; j++) { float a = 6.2831853f * j / RING_DOTS; out[n++] = (dm_ovl){st[k].x0 + R * cosf(a) - dot, st[k].y0 + R * sinf(a) - dot, 2 * dot, 2 * dot, 1, ""}; }
        float ddx = st[k].x - st[k].x0, ddy = st[k].y - st[k].y0, m = hypotf(ddx, ddy); if (m > R) { ddx *= R / m; ddy *= R / m; }
        if (n < max) out[n++] = (dm_ovl){st[k].x0 + ddx - kr, st[k].y0 + ddy - kr, 2 * kr, 2 * kr, 1, ""};
    }
    return n;
}
static void touch_event(AInputEvent *e) {
    vb_layout();
    int32_t act = AMotionEvent_getAction(e), kind = act & AMOTION_EVENT_ACTION_MASK;
    size_t ai = (size_t)((act & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK) >> AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT);
    if (kind == AMOTION_EVENT_ACTION_UP || kind == AMOTION_EVENT_ACTION_CANCEL) g_nptr = 0;   // the last finger up: nothing is held
    else {
        if (kind == AMOTION_EVENT_ACTION_DOWN || kind == AMOTION_EVENT_ACTION_POINTER_DOWN) {   // a new finger: a button, else the stick of its half (one finger per stick)
            int32_t id = AMotionEvent_getPointerId(e, ai); float x = AMotionEvent_getX(e, ai), y = AMotionEvent_getY(e, ai);
            int k = vb_hit(x, y) ? 1 : x < g_vw / 2 ? 2 : 3;
            for (int i = 0; i < g_nptr && k > 1; i++) if (g_ptr[i].kind == k) k = 0;   // that stick is held already: this finger does nothing
            if (kind == AMOTION_EVENT_ACTION_DOWN) g_nptr = 0;
            if (g_nptr < NPTR) g_ptr[g_nptr++] = (typeof(g_ptr[0])){id, k, x, y, x, y};
        }
        if (kind == AMOTION_EVENT_ACTION_POINTER_UP) {
            int32_t id = AMotionEvent_getPointerId(e, ai);
            for (int i = 0; i < g_nptr; i++) if (g_ptr[i].id == id) { g_ptr[i] = g_ptr[--g_nptr]; break; }
        }
        for (size_t p = 0; p < AMotionEvent_getPointerCount(e); p++) {   // where every finger is now
            int32_t id = AMotionEvent_getPointerId(e, p);
            for (int i = 0; i < g_nptr; i++) if (g_ptr[i].id == id) { g_ptr[i].x = AMotionEvent_getX(e, p); g_ptr[i].y = AMotionEvent_getY(e, p); }
        }
    }
    uint64_t btn = 0; float ax[4] = {0}; typeof(g_stick[0]) st[2] = {{0}};
    for (int i = 0; i < g_nptr; i++) {
        if (g_ptr[i].kind == 1) { btn |= vb_hit(g_ptr[i].x, g_ptr[i].y); continue; }
        if (g_ptr[i].kind < 2) continue;
        int k = g_ptr[i].kind - 2; float ddx = g_ptr[i].x - g_ptr[i].x0, ddy = g_ptr[i].y - g_ptr[i].y0, m = hypotf(ddx, ddy);
        st[k] = (typeof(st[0])){1, g_ptr[i].x0, g_ptr[i].y0, g_ptr[i].x, g_ptr[i].y};
        if (m > 0.02f * g_vh) { float t = fminf(1.f, m / (0.12f * g_vh)); ax[2 * k] = ddx / m * t; ax[2 * k + 1] = ddy / m * t; }
    }
    pthread_mutex_lock(&g_smu); memcpy(g_stick, st, sizeof st); pthread_mutex_unlock(&g_smu);
    if (g_touch_slot < 0) g_touch_slot = dev_slot(-1000000);
    g_dev[g_touch_slot].btn = btn;
    g_dev[g_touch_slot].ax[AX_LX] = ax[0]; g_dev[g_touch_slot].ax[AX_LY] = ax[1]; g_dev[g_touch_slot].ax[AX_RX] = ax[2]; g_dev[g_touch_slot].ax[AX_RY] = ax[3];
    atomic_store(&g_ovl_on, btn); atomic_store(&g_ovl_show, 1);
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
    pthread_mutex_lock(&g_smu); memset(g_stick, 0, sizeof g_stick); pthread_mutex_unlock(&g_smu);
    pads_push_();
    pthread_mutex_unlock(&g_pmu);
}
static int on_input(AInputEvent *e) {
    int32_t type = AInputEvent_getType(e), src = AInputEvent_getSource(e);
    static int seen; if (!seen++) LOG("front end: first input event (type %d, source %#x)", type, src);
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
        if (on) { input_release_all(); dm_focus_set(2); dm_focus_set(3); dm_audio_suspend(1); guest_clock_freeze_for(1, 1); dm_gfx_suspend(1); LOG("front end: suspended (the activity stopped)"); }
        else { dm_gfx_suspend(0); guest_clock_freeze_for(1, 0); dm_audio_suspend(0); dm_focus_set(2); LOG("front end: resumed (the activity started)"); }
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
static void on_resume(ANativeActivity *a) { (void)a; }
static void on_pause(ANativeActivity *a) { (void)a; dm_focus_set(2); }   // still visible behind something (a dialog, a split screen): out of focus, running
static void on_focus(ANativeActivity *a, int f) { if (f) immersive(a); else input_release_all(); dm_focus_set(f ? 1 : 2); }
// Destroyed by the player leaving (− and + held, back_to_menu, finish) the game ends; destroyed by Android while the process stays (to reclaim memory, its activity
// only), the game stays suspended and a new activity takes over its window (ANativeActivity_onCreate: the host runs once per process)
static void on_destroy(ANativeActivity *a) {
    JNIEnv *env = a->env;
    jboolean fin = (*env)->CallBooleanMethod(env, a->clazz, (*env)->GetMethodID(env, (*env)->GetObjectClass(env, a->clazz), "isFinishing", "()Z"));
    if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); fin = 1; }
    if (fin) dm_host_shutdown("activity destroyed");
    else LOG("front end: the activity was destroyed by Android, not finished: the game stays suspended");
}

__attribute__((visibility("default"))) void ANativeActivity_onCreate(ANativeActivity *a, void *saved, size_t saved_n) {
    (void)saved; (void)saved_n;
    g_act = a;
    a->callbacks->onNativeWindowCreated = on_window_created; a->callbacks->onNativeWindowDestroyed = on_window_destroyed;
    a->callbacks->onNativeWindowResized = on_window_resized;
    a->callbacks->onInputQueueCreated = on_input_created; a->callbacks->onInputQueueDestroyed = on_input_destroyed;
    a->callbacks->onResume = on_resume; a->callbacks->onPause = on_pause; a->callbacks->onWindowFocusChanged = on_focus; a->callbacks->onDestroy = on_destroy;
    a->callbacks->onStart = on_start; a->callbacks->onStop = on_stop;
    ANativeActivity_setWindowFlags(a, 0x80 /* KEEP_SCREEN_ON */ | 0x400 /* FULLSCREEN */, 0);
    immersive(a);
    static int booted; if (booted++) return;   // the activity can be made again (rotation, a return): the host runs once per process
    game_service(a, "start", 0);   // in the foreground now, as starting a foreground service requires
    snprintf(g_files, sizeof g_files, "%s", a->internalDataPath);
    boot_env(a->internalDataPath, a->externalDataPath);
    pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setstacksize(&at, 16 << 20);
    pthread_t th; if (pthread_create(&th, &at, boot_thread, NULL)) die("front end: cannot start the host");
    pthread_detach(th);
    if (!pthread_create(&th, NULL, latch_thread, NULL)) pthread_detach(th);
}
