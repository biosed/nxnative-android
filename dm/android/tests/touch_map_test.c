// Checks app/touch_map.h (a finger on the window → the game's touch screen) on a computer:  cc -I app -o /tmp/touch_map_test tests/touch_map_test.c && /tmp/touch_map_test
#include "touch_map.h"
#include <math.h>
#include <stdio.h>
static int fails;
static void check(int ok, const char *what) { if (!ok) { printf("FAIL: %s\n", what); fails++; } }
static int near(float a, float b) { return fabsf(a - b) < 1e-3f; }
int main(void) {
    float u, v;
    // A 16:10 tablet (2560 x 1600) and a 16:9 picture: 2560 wide, 2560*9/16 = 1440 px tall, bars of 80 px above and below
    check(touch_map(1280, 800, 2560, 1600, 1280, 720, 0, 0, &u, &v) && near(u, 0.5f) && near(v, 0.5f), "the centre is the centre");
    check(touch_map(0, 80, 2560, 1600, 1280, 720, 0, 0, &u, &v) && near(u, 0) && near(v, 0), "the picture's top left corner");
    check(!touch_map(1280, 40, 2560, 1600, 1280, 720, 0, 0, &u, &v), "the bar above the picture is no touch");
    check(!touch_map(1280, 1570, 2560, 1600, 1280, 720, 0, 0, &u, &v), "the bar below the picture is no touch");
    check(touch_map(1280, 40, 2560, 1600, 1280, 720, 0, 1, &u, &v) && near(u, 0.5f) && near(v, 0), "a finger slid onto a bar stays at the edge");
    check(touch_map(2560, 1250, 2560, 1600, 1280, 720, 0, 1, &u, &v) && u < 1 && v < 1, "clamped below 1");
    // A 20:9 phone (2400 x 1080): bars left and right (1080*16/9 = 1920 px wide, 240 px each side)
    check(touch_map(240 + 480, 270, 2400, 1080, 1920, 1080, 0, 0, &u, &v) && near(u, 0.25f) && near(v, 0.25f), "a quarter in on a wide screen");
    check(!touch_map(100, 540, 2400, 1080, 1920, 1080, 0, 0, &u, &v), "the left bar is no touch");
    // Stretched: the whole window is the picture
    check(touch_map(100, 540, 2400, 1080, 1920, 1080, 1, 0, &u, &v) && near(u, 100.f / 2400) && near(v, 0.5f), "stretch maps the whole window");
    // No picture yet: taken as 16:9
    check(touch_map(1280, 800, 2560, 1600, 0, 0, 0, 0, &u, &v) && near(u, 0.5f) && near(v, 0.5f), "no picture yet: 16:9");
    // A crop that is not 16:9 (a game rendering 1280 x 960): bars left and right on a 16:10 window
    check(touch_map(1280, 800, 2560, 1600, 1280, 960, 0, 0, &u, &v) && near(u, 0.5f) && near(v, 0.5f), "a 4:3 picture's centre");
    check(!touch_map(100, 800, 2560, 1600, 1280, 960, 0, 0, &u, &v), "a 4:3 picture's side bar");
    printf(fails ? "%d failed\n" : "all passed\n", fails);
    return fails != 0;
}
