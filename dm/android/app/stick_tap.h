// Double tap on an on-screen stick = the stick's click (L3 / R3), as the launcher's layout editor allows per profile ("Ketuk 2x stik = L3/R3", ControlsLayout.stickDoubleTap).
// Plain C, no Android: app/front_android.c uses it, tests/stick_tap_test.c checks it on a computer.
// A finger that comes down on a stick within STICK_TAP_GAP of the previous touch-down on the same stick, when that previous touch was a short tap (lifted within
// STICK_TAP_MAX, moved less than STICK_TAP_MOVE of the stick's radius), presses the click for as long as it stays down; it still moves the stick (double tap, hold and drag).
// The first tap is an ordinary stick touch. A double tap does not start another one (a third quick tap is a first tap again)
#pragma once
#include <stdint.h>
#define STICK_TAP_GAP 300000000ull   // ns between the two touch-downs
#define STICK_TAP_MAX 250000000ull   // ns the first touch may last
#define STICK_TAP_MOVE 0.15f         // the first touch's largest movement, as a fraction of the stick's radius
typedef struct { uint64_t down_ns; int tap, have; } stick_tap;   // the stick's last touch: when it came down, whether it was a short tap
// A finger came down on the stick at `now`: 1 if it is the second touch of a double tap
static inline int stick_tap_down(stick_tap *s, uint64_t now) {
    int dbl = s->have && s->tap && now >= s->down_ns && now - s->down_ns < STICK_TAP_GAP;
    s->down_ns = now; s->have = 1; s->tap = 0;
    return dbl;
}
// It lifted at `now` (cancelled: pass a movement of 1 or more), having moved at most `move` of the stick's radius; was_double: it was the second touch of a double tap
static inline void stick_tap_up(stick_tap *s, uint64_t now, float move, int was_double) {
    s->tap = !was_double && now >= s->down_ns && now - s->down_ns < STICK_TAP_MAX && move < STICK_TAP_MOVE;
}
