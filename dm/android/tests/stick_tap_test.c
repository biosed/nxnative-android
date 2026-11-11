// Checks app/stick_tap.h (double tap on an on-screen stick = L3 / R3) on a computer:  cc -I app -o /tmp/stick_tap_test tests/stick_tap_test.c && /tmp/stick_tap_test
#include "stick_tap.h"
#include <stdio.h>
static int fails;
static void check(int ok, const char *what) { printf("%s %s\n", ok ? "ok  " : "FAIL", what); if (!ok) fails++; }
#define MS 1000000ull
int main(void) {
    stick_tap s = {0};
    check(!stick_tap_down(&s, 1000 * MS), "a first touch is no click");
    stick_tap_up(&s, 1100 * MS, 0.05f, 0);
    check(stick_tap_down(&s, 1250 * MS), "a short still tap, then a touch 250 ms after the first came down: click");
    stick_tap_up(&s, 1900 * MS, 0.6f, 1);
    check(!stick_tap_down(&s, 2000 * MS), "the touch after a double tap is a first touch again (no triple)");
    stick_tap_up(&s, 2080 * MS, 0.0f, 0);
    check(!stick_tap_down(&s, 2400 * MS), "a second touch 400 ms after the first: too late, no click");
    stick_tap_up(&s, 2700 * MS, 0.0f, 0);   // held 300 ms: no tap
    check(!stick_tap_down(&s, 2800 * MS), "after a touch held 300 ms (longer than a tap): no click");
    stick_tap_up(&s, 2850 * MS, 0.3f, 0);   // moved 30 % of the radius: a flick, no tap
    check(!stick_tap_down(&s, 2950 * MS), "after a touch that moved the stick 30 % of its radius: no click");
    stick_tap_up(&s, 3000 * MS, 0.1f, 0);   // 50 ms, 10 %: a tap
    check(stick_tap_down(&s, 3200 * MS), "a 50 ms tap moving 10 % of the radius, then a touch 250 ms after it came down: click");
    stick_tap_up(&s, 3300 * MS, 1.0f, 1);
    stick_tap t = {0}; stick_tap_down(&t, 5000 * MS); stick_tap_up(&t, 5100 * MS, 1.0f, 0);   // cancelled
    check(!stick_tap_down(&t, 5200 * MS), "after a cancelled touch: no click");
    printf(fails ? "%d failed\n" : "all passed\n", fails); return fails != 0;
}
