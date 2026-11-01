/* SPDX-License-Identifier: MIT
 *
 * 0020: main.c's scrollbar section, run as it is in the file. The Makefile
 * copies it out between its marker line and the next section's into
 * lifted.inc, and this supplies what it calls: touch_get() from a script
 * of (until, down, x, y), a clock that vTaskDelay() advances, and a
 * gfx_fill_rect() that records. Checks the geometry against Spurious's
 * list (and three million rows), that the thumb is drawn centred where a
 * finger asks, and wait_touch() through a tap, a tap in the zone that is
 * not a grab, a drag with a finger held still, a drop, and a timeout.
 *
 *   make -C m5tab5/test/scrollbar
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define W 720
#define C_RULE 1
#define C_DIM  2
#define C_TEXT 3
#define pdMS_TO_TICKS(ms) (ms)

/* clock: vTaskDelay advances it */
static int64_t now_us;
static int64_t esp_timer_get_time(void) { return now_us; }

/* touch: a script of (until_us, down, x, y) */
typedef struct { int64_t until; bool down; int x, y; } step_t;
static const step_t *script; static int nsteps;
static bool touch_get(int *x, int *y)
{
    for (int i = 0; i < nsteps; i++)
        if (now_us < script[i].until) { if (script[i].down) { *x = script[i].x; *y = script[i].y; } return script[i].down; }
    return false;
}
static void vTaskDelay(int ms) { now_us += (int64_t)ms * 1000; }

/* gfx: the last two rectangles */
typedef struct { int x, y, w, h, c; } rect_t;
static rect_t rects[8]; static int nrects;
static void gfx_fill_rect(int x, int y, int w, int h, uint16_t c) { if (nrects < 8) rects[nrects++] = (rect_t){ x, y, w, h, c }; }

#include "lifted.inc"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static touch_ev_t run(const step_t *s, int n, const sbar_t *sb, int *x, int *y, int ms)
{
    script = s; nsteps = n;
    return wait_touch(sb, x, y, ms);
}

int main(void)
{
    /* Spurious's list: track from HEAD_H 150, 10 rows of 76 */
    sbar_t big = { 150, 760, 54189, 10 };
    int bh; int32_t mt;
    CHECK(sbar_geom(&big, &bh, &mt) && bh == 24 && mt == 54179, "big geom %d %ld", bh, (long)mt);
    sbar_t fits = { 150, 760, 10, 10 };
    CHECK(!sbar_geom(&fits, &bh, &mt), "10 of 10 fits");
    nrects = 0; sbar_draw(&fits, 0, false);
    CHECK(nrects == 0, "nothing drawn when it fits");
    CHECK(!sbar_grab(&fits, 710, 300), "no grab when it fits");
    CHECK(!sbar_grab(NULL, 710, 300), "no grab without a bar");

    /* draw: ends, and colours */
    nrects = 0; sbar_draw(&big, 0, false);
    CHECK(nrects == 2 && rects[0].x == W - 16 && rects[0].y == 150 && rects[0].h == 760 && rects[0].c == C_RULE, "track");
    CHECK(rects[1].y == 150 && rects[1].h == 24 && rects[1].c == C_DIM, "thumb at top, resting %d %d", rects[1].y, rects[1].c);
    nrects = 0; sbar_draw(&big, 54179, true);
    CHECK(rects[1].y == 150 + 760 - 24 && rects[1].c == C_TEXT, "thumb at bottom, held: y %d", rects[1].y);
    nrects = 0; sbar_draw(&big, 999999, false);
    CHECK(rects[1].y == 150 + 760 - 24, "past the end is the end");

    /* finger to row: ends, centring, monotonic, and drawn where it asks */
    CHECK(sbar_top(&big, 150) == 0, "top of track");
    CHECK(sbar_top(&big, 150 + 12) == 0, "thumb centred on the finger at the top");
    CHECK(sbar_top(&big, 150 + 759) == 54179, "bottom of track: %ld", (long)sbar_top(&big, 909));
    int32_t prev = -1; bool mono = true;
    for (int y = 150; y < 910; y++) { int32_t t = sbar_top(&big, y); if (t < prev) mono = false; prev = t; }
    CHECK(mono, "monotonic down the track");
    for (int y = 162; y < 898; y++) {
        int32_t t = sbar_top(&big, y);
        nrects = 0; sbar_draw(&big, t, true);
        int mid = rects[1].y + rects[1].h / 2;
        if (mid < y - 1 || mid > y + 1) { CHECK(0, "finger %d, thumb centre %d", y, mid); break; }
    }
    sbar_t huge = { 150, 760, 3000000, 10 };   /* past what int32 products hold */
    CHECK(sbar_top(&huge, 909) == 2999990, "3M rows, bottom: %ld", (long)sbar_top(&huge, 909));

    int x = 0, y = 0;
    /* a) nothing */
    now_us = 0; { step_t s[] = { { 1000000, false, 0, 0 } };
      CHECK(run(s, 1, &big, &x, &y, 250) == T_NONE && now_us >= 250000 && now_us < 300000, "timeout"); }
    /* b) a tap on a row: where it went down, after it lifts */
    now_us = 0; { step_t s[] = { { 30000, true, 100, 300 }, { 60000, true, 120, 340 }, { 1e9, false, 0, 0 } };
      touch_ev_t e = run(s, 3, &big, &x, &y, 250);
      CHECK(e == T_TAP && x == 100 && y == 300 && !s_drag, "tap: %d at %d,%d", e, x, y); }
    /* c) a tap in the zone on a list that fits is a tap */
    now_us = 0; { step_t s[] = { { 30000, true, 710, 300 }, { 1e9, false, 0, 0 } };
      CHECK(run(s, 2, &fits, &x, &y, 250) == T_TAP && x == 710, "zone on a short list"); }
    /* d) a tap in the zone below the track (the keyboard) is a tap */
    now_us = 0; { step_t s[] = { { 30000, true, 710, 1000 }, { 1e9, false, 0, 0 } };
      CHECK(run(s, 2, &big, &x, &y, 250) == T_TAP && y == 1000, "zone below the track"); }
    /* e) a drag: grab, two moves, a still finger reports nothing, lift */
    now_us = 0; { step_t s[] = { { 30000, true, 700, 300 }, { 60000, true, 702, 350 }, { 300000, true, 702, 350 },
                                  { 330000, true, 690, 400 }, { 1e9, false, 0, 0 } };
      script = s; nsteps = 5;
      touch_ev_t e1 = wait_touch(&big, &x, &y, 250); int y1 = y;
      touch_ev_t e2 = wait_touch(&big, &x, &y, 250); int y2 = y;
      touch_ev_t e3 = wait_touch(&big, &x, &y, 250); int y3 = y; int64_t t3 = now_us;
      touch_ev_t e4 = wait_touch(&big, &x, &y, 250);
      CHECK(e1 == T_DRAG && y1 == 300, "grab %d at %d", e1, y1);
      CHECK(e2 == T_DRAG && y2 == 350, "move %d to %d", e2, y2);
      CHECK(e3 == T_DRAG && y3 == 400 && t3 >= 300000, "still finger silent, then %d to %d at %lld", e3, y3, (long long)t3);
      CHECK(e4 == T_DROP && !s_drag, "drop %d", e4);
      /* and the next tap after it is an ordinary tap */
      step_t s2[] = { { now_us + 30000, true, 50, 200 }, { (int64_t)1e12, false, 0, 0 } };
      CHECK(run(s2, 2, &big, &x, &y, 250) == T_TAP && x == 50, "tap after a drag"); }
    /* f) no bar given: wait_tap()'s behaviour */
    now_us = 0; { step_t s[] = { { 30000, true, 710, 300 }, { 1e9, false, 0, 0 } };
      CHECK(run(s, 2, NULL, &x, &y, 250) == T_TAP, "NULL bar"); }
    /* g) a finger held past the timeout still comes back a tap */
    now_us = 0; { step_t s[] = { { 900000, true, 100, 300 }, { 1e9, false, 0, 0 } };
      CHECK(run(s, 2, &big, &x, &y, 250) == T_TAP && now_us >= 900000, "held long"); }

    if (fails) { printf("sbtest: %d failed\n", fails); return 1; }
    printf("sbtest: all passed\n");
    return 0;
}
