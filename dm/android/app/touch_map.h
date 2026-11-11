// A finger on the Android window → a point on the Switch's touch screen (front_android.c, the game touches; checked by tests/touch_map_test.c on a computer).
// The Vulkan backend (src_vk/nvn_vk.c present) scales the game's picture onto the window keeping its aspect (black bars left and right, or above and below) unless
// DM_SCREEN_FIT=stretch; the touch screen covers the picture, origin at its top left, as dm_touch_set wants it (front.h: normalized [0, 1)). The window's coordinates are the
// ones MotionEvent gives, in the orientation the player sees: the letterbox is computed there from the picture's size, so a swapchain the compositor rotates (a phone whose
// screen is portrait by nature) or the backend turns half round (ROTATE_180, centred, the same bars) maps the same way.
#pragma once
// x, y: the finger in window pixels; ww, wh: the window; pw, ph: the picture's size (0: not presented yet, taken as 16:9); stretch: the picture fills the window.
// Returns 1 with *nx, *ny in [0, 1) when the finger is on the picture, 0 on a black bar. clamp: a finger that came down on the picture and slid onto a bar stays at the edge
static inline int touch_map(float x, float y, float ww, float wh, int pw, int ph, int stretch, int clamp, float *nx, float *ny) {
    if (ww <= 0 || wh <= 0) return 0;
    if (pw <= 0 || ph <= 0) { pw = 1280; ph = 720; }
    float dx = 0, dy = 0, dw = ww, dh = wh;
    if (!stretch) {   // the same arithmetic as the present's blit (integer there; a pixel either way)
        if (ww * (float)ph > wh * (float)pw) { dw = wh * (float)pw / (float)ph; dx = (ww - dw) / 2; }
        else { dh = ww * (float)ph / (float)pw; dy = (wh - dh) / 2; }
    }
    float u = (x - dx) / dw, v = (y - dy) / dh;
    const float top = 0.9999f;   // below 1: hle_touch.c scales by the panel's 1280 x 720 and clamps to its last pixel anyway
    if (u < 0 || v < 0 || u >= 1 || v >= 1) {
        if (!clamp) return 0;
        u = u < 0 ? 0 : u > top ? top : u; v = v < 0 ? 0 : v > top ? top : v;
    }
    *nx = u; *ny = v; return 1;
}
