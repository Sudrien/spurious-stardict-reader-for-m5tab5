/*
 * Spurious -- a StarDict reader for the M5Stack Tab5.
 *
 * Finds dictionaries (.ifo with its .idx and .dict or .dict.dz) under
 * /dict on the microSD card or a USB drive, one level of folders deep,
 * as tools/zim2stardict.py writes them. With one, it opens; with more,
 * a list. Then: type on the keyboard, the list shows the headwords (and
 * the .syn's synonyms) from there on, tap one for its definition. With
 * "Sounds like" on -- behind the gear at the top right since 0021 -- the
 * list is instead the words that sound like what was typed (Double
 * Metaphone, main/dmeta.c).
 *
 * The board bring-up is Avoidant's (avoidant-launcher-for-m5tab5): the
 * feckless components for the panel, touch, USB host and storage.
 *
 * 0018: the reader is an ELF built against the feckless ABI (../elf/),
 * with the card and the drive from its storage section. This app_main()
 * is what a launcher such as Avoidant runs, and finds storage up;
 * standalone.c is the .bin's, which brings NVS, the USB host and storage
 * up as a launcher does, then loads it.
 *
 * SPDX-License-Identifier: MIT
 */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "feckless_abi.h"            /* 0018 */
#include "feckless_abi_storage.h"
#include "feckless_box.h"            /* u-spurious-0001 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "storage.h"
#include "storage_io.h"

#include "feckless_keyboard.h"     /* 0019 */
#include "gfx.h"
#include "lcd.h"
#include "tab5io.h"
#include "touch.h"

#include "stardict.h"
#include "libversions.h"         /* 0024: generated, feckless_libversions() in elf/ */

static const char *TAG = "spurious";

#define BRIGHTNESS  60

/* Layout, portrait 720 x 1280. */
#define W           LCD_H_RES
#define H           LCD_V_RES
#define PAD         24
#define SCALE       3                   /* interface text */
#define DSCALE      2                   /* definitions */
#define TEXT_H      GFX_GLYPH_H(SCALE)
#define DTEXT_H     GFX_GLYPH_H(DSCALE)
#define HEAD_H      150
#define ROW_H       76
#define KEY_H       88
#define KB_Y        (H - (KBD_ROWS + 1) * KEY_H)   /* 0023: the keyboard, and our row */
#define ROWS        ((KB_Y - HEAD_H) / ROW_H)
#define FOOT_H      120
#define DLINE       (DTEXT_H + 10)
#define DROWS       ((H - HEAD_H - FOOT_H - 20) / DLINE)

#define C_BG        RGB(16, 18, 24)
#define C_HEAD      RGB(32, 36, 48)
#define C_TEXT      RGB(235, 235, 235)
#define C_DIM       RGB(140, 145, 160)
#define C_HIT       RGB(255, 210, 120)
#define C_RULE      RGB(44, 48, 60)
#define C_KEY       RGB(52, 58, 76)
#define C_HIT_KEY   RGB(120, 96, 40)
#define C_BAD       RGB(220, 90, 80)

/* ---- touch, polled ------------------------------------------------------ */

/* Wait up to ms for a finger to touch and lift; the tap is where it
 * touched. False on timeout, so a caller can check the volumes between. */
static bool wait_tap(int *x, int *y, int ms)
{
    int64_t until = esp_timer_get_time() + (int64_t)ms * 1000;
    bool down = false;
    for (;;) {
        int tx, ty;
        bool now = touch_get(&tx, &ty);
        if (now && !down) { down = true; *x = tx; *y = ty; }
        if (!now && down) return true;
        if (!down && esp_timer_get_time() > until) return false;
        vTaskDelay(pdMS_TO_TICKS(15));
    }
}

/* ---- drawing ------------------------------------------------------------ */

static void show(void) { gfx_blit(0, H); }

static void key(int x, int y, int w, int h, const char *label, uint16_t bg)
{
    gfx_fill_rect(x + 2, y + 2, w - 4, h - 4, bg);
    int tw = gfx_text_w(label, SCALE);
    gfx_draw_text(x + (w - tw) / 2, y + (h - TEXT_H) / 2, label, SCALE, w, C_TEXT);
}

/* 0021: its text w wide, so the search screen can keep the gear clear. */
static void header_w(const char *small, const char *big, int w)
{
    gfx_fill_rect(0, 0, W, HEAD_H, C_HEAD);
    if (small) gfx_draw_text(PAD, 20, small, DSCALE, w, C_DIM);
    if (big) gfx_draw_text(PAD, 20 + DTEXT_H + 20, big, SCALE, w, C_TEXT);
}

static void header(const char *small, const char *big)
{
    header_w(small, big, W - 2 * PAD);
}

static void message(const char *title, const char *body, uint16_t c)
{
    gfx_fill_rect(0, 0, W, H, C_BG);
    header("Spurious", title);
    gfx_draw_para(PAD, HEAD_H + 40, body, DSCALE, W - 2 * PAD, DLINE, 40, c);
    show();
}

/* ---- the scrollbar (0020) ---------------------------------------------- */

/* Defeatist's (its browser.c): a bar down the right edge, thumb the share
 * of the list that is showing, and a control as well as a readout. Drawn
 * SCROLL_W wide; a finger is caught anywhere in the outer SCROLL_HIT_W of
 * the list, which is the margin rows leave there anyway. The thumb centres
 * on the finger rather than keeping where it was grabbed -- a fingertip
 * covers it, so a press on the track and a drag are one gesture. */
#define SCROLL_W        16
#define SCROLL_HIT_W    72

typedef struct {
    int y, h;               /* the track */
    int32_t count;          /* rows in the list */
    int32_t rows;           /* rows showing */
} sbar_t;

/* False when the list fits, which is also when there is nothing to drag. */
static bool sbar_geom(const sbar_t *s, int *bar_h, int32_t *max_top)
{
    if (s->count <= s->rows || s->h <= 0) return false;
    *bar_h = (int)((int64_t)s->h * s->rows / s->count);
    if (*bar_h < 24) *bar_h = 24;
    if (*bar_h > s->h) *bar_h = s->h;
    *max_top = s->count - s->rows;
    return true;
}

static void sbar_draw(const sbar_t *s, int32_t top, bool held)
{
    int bar_h;
    int32_t max_top;
    if (!sbar_geom(s, &bar_h, &max_top)) return;
    if (top < 0) top = 0;
    if (top > max_top) top = max_top;
    int bar_y = s->y + (int)((int64_t)(s->h - bar_h) * top / max_top);
    gfx_fill_rect(W - SCROLL_W, s->y, SCROLL_W, s->h, C_RULE);
    gfx_fill_rect(W - SCROLL_W, bar_y, SCROLL_W, bar_h, held ? C_TEXT : C_DIM);
}

/* Is a finger going down at (x, y) a grab: in the zone, on the track, of
 * a bar that is drawn. */
static bool sbar_grab(const sbar_t *s, int x, int y)
{
    int bar_h;
    int32_t max_top;
    return s && x >= W - SCROLL_HIT_W && y >= s->y && y < s->y + s->h &&
           sbar_geom(s, &bar_h, &max_top);
}

/* The first row a finger at y asks for, the thumb centred on it. */
static int32_t sbar_top(const sbar_t *s, int y)
{
    int bar_h;
    int32_t max_top;
    if (!sbar_geom(s, &bar_h, &max_top)) return 0;
    int span = s->h - bar_h;
    int pos = y - s->y - bar_h / 2;
    if (pos < 0) pos = 0;
    if (pos > span) pos = span;
    return span > 0 ? (int32_t)(((int64_t)pos * max_top + span / 2) / span) : 0;
}

typedef enum {
    T_NONE,                 /* nothing within the time */
    T_TAP,                  /* down and up, at where it went down: wait_tap()'s */
    T_DRAG,                 /* on the scrollbar, at (x, y) now */
    T_DROP,                 /* off it again, or the end of a pan */
    T_PAN,                  /* 0026: a finger dragging the content, s_pan_dy since the last */
} touch_ev_t;

static bool s_drag;         /* a finger on the scrollbar, between calls */

/* 0026: dragging the content itself. A finger that goes down between
 * s_pan_top and s_pan_bot (set by the screen; both 0, the default, is
 * nowhere) and moves PAN_SLOP or more up or down is a pan, not a tap:
 * reported at each position it moves to with s_pan_dy, the pixels since
 * the last report (down the screen positive), and T_DROP when it lifts.
 * Less than PAN_SLOP and it is still a tap where it went down. */
#define PAN_SLOP    16
static int s_pan_top, s_pan_bot;
static bool s_pan;          /* a finger panning, between calls */
static int s_pan_dy;

/* wait_tap(), and the scrollbar: a finger that goes down on sb's grab
 * zone is a drag, reported at each position it moves to and once more
 * when it lifts, instead of a tap. sb may be NULL. 0026: and the pan. */
static touch_ev_t wait_touch(const sbar_t *sb, int *x, int *y, int ms)
{
    static int drag_y, down_y;
    int64_t until = esp_timer_get_time() + (int64_t)ms * 1000;
    bool down = false;
    for (;;) {
        int tx, ty;
        bool now = touch_get(&tx, &ty);
        if (s_drag || s_pan) {
            if (!now) { s_drag = s_pan = false; return T_DROP; }
            if (ty != drag_y) {
                s_pan_dy = ty - drag_y;
                drag_y = ty; *x = tx; *y = ty;
                return s_drag ? T_DRAG : T_PAN;
            }
        } else {
            if (now && !down) {
                down = true; *x = tx; *y = ty; down_y = ty;
                if (sbar_grab(sb, tx, ty)) { s_drag = true; drag_y = ty; return T_DRAG; }
            }
            int moved = ty > down_y ? ty - down_y : down_y - ty;
            if (now && down && moved >= PAN_SLOP && down_y >= s_pan_top && down_y < s_pan_bot) {
                s_pan = true;
                s_pan_dy = ty - down_y;
                drag_y = ty; *x = tx; *y = ty;
                return T_PAN;
            }
            if (!now && down) return T_TAP;
            if (!down && esp_timer_get_time() > until) return T_NONE;
        }
        vTaskDelay(pdMS_TO_TICKS(15));
    }
}

/* ---- finding dictionaries ----------------------------------------------- */

#define MAX_DICTS   16
#define PATH_LEN    300

static char dicts[MAX_DICTS][PATH_LEN];
static int ndicts;

static void add_ifos(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) && ndicts < MAX_DICTS) {
        if (e->d_name[0] == '.') continue;
        size_t l = strlen(e->d_name);
        if (l > 4 && !strcasecmp(e->d_name + l - 4, ".ifo") &&
            storage_join_path(dicts[ndicts], PATH_LEN, dir, e->d_name))
            ndicts++;
    }
    closedir(d);
}

/* Each .ifo in <vol>/dict and in each folder directly inside it. */
static void scan(void)
{
    ndicts = 0;
    for (int v = 0; v < STORAGE_COUNT; v++) {
        if (!storage_present((storage_id_t)v)) continue;
        char root[PATH_LEN];
        if (!storage_join_path(root, sizeof root, storage_mount_path((storage_id_t)v), "dict")) continue;
        add_ifos(root);
        DIR *d = opendir(root);
        if (!d) continue;
        struct dirent *e;
        char sub[PATH_LEN];
        while ((e = readdir(d)) && ndicts < MAX_DICTS) {
            if (e->d_name[0] == '.' || e->d_type != DT_DIR) continue;
            if (storage_join_path(sub, sizeof sub, root, e->d_name)) add_ifos(sub);
        }
        closedir(d);
    }
}

static const char *base(const char *p)
{
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

/* A dictionary's .ifo path, once there is one to open. */
static const char *pick(void)
{
    for (;;) {
        uint32_t gen = storage_generation();
        scan();
        if (ndicts == 1) return dicts[0];
        if (ndicts == 0) {
            message("No dictionary",
                    "Put a StarDict dictionary -- its .ifo, .idx and .dict or .dict.dz -- "
                    "in a folder called dict on the microSD card or a USB drive, "
                    "or in a folder inside it.\n\n"
                    "tools/zim2stardict.py makes one from a Kiwix Wiktionary dump.", C_TEXT);
            while (storage_generation() == gen) vTaskDelay(pdMS_TO_TICKS(250));
            continue;
        }
        gfx_fill_rect(0, 0, W, H, C_BG);
        header("Spurious", "Choose a dictionary");
        for (int i = 0; i < ndicts; i++) {
            int y = HEAD_H + i * ROW_H;
            gfx_draw_text(PAD, y + (ROW_H - TEXT_H) / 2, base(dicts[i]), SCALE, W - 2 * PAD, C_TEXT);
            gfx_fill_rect(0, y + ROW_H - 2, W, 2, C_RULE);
        }
        show();
        for (;;) {
            int x, y;
            if (wait_tap(&x, &y, 250)) {
                int i = (y - HEAD_H) / ROW_H;
                if (y >= HEAD_H && i < ndicts) return dicts[i];
            }
            if (storage_generation() != gen) break;
        }
    }
}

/* ---- a definition ------------------------------------------------------- */

typedef struct { const char *s; uint16_t n; } line_t;

/* Returns false when the dictionary's volume went away. */
static bool define(sd_dict_t *d, uint32_t i, storage_id_t vol)
{
    char *text = sd_define(d, i);
    if (!text) {
        message(sd_word(d, i), "This definition could not be read.", C_BAD);
        vTaskDelay(pdMS_TO_TICKS(1500));
        return storage_present(vol);
    }
    /* Wrap it all once, then page through the lines. */
    size_t cap = 256, n = 0;
    line_t *lines = malloc(cap * sizeof *lines);
    for (const char *s = text; s && lines;) {
        const char *next;
        size_t len = gfx_wrap_line(s, DSCALE, W - 2 * PAD, &next);
        if (n == cap) {
            line_t *t = realloc(lines, (cap *= 2) * sizeof *lines);
            if (!t) break;
            lines = t;
        }
        lines[n++] = (line_t){ s, (uint16_t)(len > 0xffff ? 0xffff : len) };
        s = next;
    }
    /* 0026: scrolled by the pixel, so a finger dragging the text moves it
     * with the finger; Up and Down move a screen, the scrollbar anywhere.
     * The text is drawn first and the header and the keys over it, which
     * is the clipping. */
    const int vy = HEAD_H + 20, vh = DROWS * DLINE, fy = H - FOOT_H, bw = W / 3;
    const int ch = (int)n * DLINE, max = ch > vh ? ch - vh : 0;
    int top = 0;                /* pixels scrolled */
    bool ok = true;
    char buf[512];
    for (;;) {
        if (top > max) top = max;
        if (top < 0) top = 0;
        gfx_fill_rect(0, 0, W, H, C_BG);
        for (int r = top / DLINE; r < (int)n; r++) {
            int ly = vy + r * DLINE - top;
            if (ly >= vy + vh) break;
            const line_t *l = &lines[r];
            size_t m = l->n < sizeof buf - 1 ? l->n : sizeof buf - 1;
            memcpy(buf, l->s, m);
            buf[m] = 0;
            gfx_draw_text(PAD, ly, buf, DSCALE, W - 2 * PAD, C_TEXT);
        }
        gfx_fill_rect(0, 0, W, vy, C_BG);
        gfx_fill_rect(0, vy + vh, W, H - vy - vh, C_BG);
        char sub[96];
        int pages = (ch + vh - 1) / vh;
        snprintf(sub, sizeof sub, "%s  %d/%d", sd_name(d),
                 top >= max ? (pages ? pages : 1) : top / vh + 1, pages ? pages : 1);
        header(sub, sd_word(d, i));
        sbar_t sb = { vy, vh, ch, vh };     /* 0020; 0026: in pixels */
        sbar_draw(&sb, top, s_drag);
        key(0, fy, bw, FOOT_H, "Back", C_KEY);
        key(bw, fy, bw, FOOT_H, "Up", top > 0 ? C_KEY : C_RULE);
        key(2 * bw, fy, W - 2 * bw, FOOT_H, "Down", top < max ? C_KEY : C_RULE);
        show();
        int x, y;
        touch_ev_t ev;
        s_pan_top = vy;
        s_pan_bot = vy + vh;
        while ((ev = wait_touch(&sb, &x, &y, 250)) == T_NONE)
            if (!storage_present(vol)) { ok = false; goto out; }
        if (ev == T_DRAG) { top = (int)sbar_top(&sb, y); continue; }
        if (ev == T_PAN) { top -= s_pan_dy; continue; }
        if (ev == T_DROP) {
            ESP_LOGI(TAG, "scrolled to line %d of %d", top / DLINE, (int)n);
            continue;
        }
        if (y < fy) continue;
        if (x < bw) break;
        if (x < 2 * bw) top -= vh;
        else top += vh;
    }
out:
    s_pan_top = s_pan_bot = 0;
    free(lines);
    free(text);
    return ok;
}

/* ---- search ------------------------------------------------------------- */

/* 0019: the keys are feckless_keyboard's (feckless-libs' 8010, this
 * file's kb[] and key() as they were). 0023: v0.8.6's (its 8011), four
 * rows staggered as a real keyboard's, with shift and a symbol page; its
 * state is its own, so this is no longer const. The row under it is this
 * file's. */
static kbd_t KBD = { .x = 0, .y = KB_Y, .w = W, .key_h = KEY_H, .scale = SCALE,
                     .key = C_KEY, .text = C_TEXT, .on = C_HIT_KEY };

/* Our row's keys on the keyboard's pitch, eleven across: the n'th key's
 * left edge. */
static int kb_x(int n)
{
    return n * W / KBD_UNITS;
}

#define MAX_HITS    (4 * ROWS)

/* One row of the list: the word, and for a synonym the headword it
 * leads to, dimmed after it. */
static void draw_row(sd_dict_t *d, uint32_t r, int y, uint16_t c)
{
    const char *w = sd_row_word(d, r);
    int ty = y + (ROW_H - TEXT_H) / 2;
    gfx_draw_text(PAD, ty, w, SCALE, W - 2 * PAD, c);
    if (sd_row_is_syn(d, r)) {
        int x = PAD + gfx_text_w(w, SCALE) + GFX_GLYPH_W(SCALE);
        if (x < W - PAD - 4 * GFX_GLYPH_W(SCALE)) {
            char see[96];
            snprintf(see, sizeof see, "(%s)", sd_word(d, sd_row_entry(d, r)));
            gfx_draw_text(x, ty, see, SCALE, W - PAD - x, C_DIM);
        }
    }
    gfx_fill_rect(0, y + ROW_H - 2, W, 2, C_RULE);
}

/* 0021: sounds moved to the gear's panel, and space has its width. */
static void draw_keyboard(void)
{
    kbd_draw(&KBD);
    int y = KB_Y + kbd_h(&KBD);
    key(kb_x(0), y, kb_x(2) - kb_x(0), KEY_H, "Up", C_KEY);
    key(kb_x(2), y, kb_x(4) - kb_x(2), KEY_H, "Down", C_KEY);
    key(kb_x(4), y, kb_x(9) - kb_x(4), KEY_H, "space", C_KEY);
    key(kb_x(9), y, kb_x(11) - kb_x(9), KEY_H, "clear", C_KEY);
}

/* ---- the gear and its settings (0021) ----------------------------------- */

/* The search screen's settings, behind a gear at the right of its header:
 * the switches first -- for now one, "Sounds like", which was a key in the
 * row under the keyboard and read there as a key-click setting -- then
 * the dictionaries on the card and the drive, to change to another without
 * restarting, then (0024) the libraries this reader was built from, with
 * their versions and licences (libversions.h). 0026: a screen of its own,
 * not a panel over the word list: everything is on it, none cut off, and
 * it scrolls -- a finger dragging it, or the scrollbar. A tap on the
 * switch flips it, on a dictionary opens it; Back or the gear puts it
 * away. */
#define GEAR_W      HEAD_H              /* the square it is tapped in */
#define SET_ROW     (ROW_H + 12)        /* the switch's row */
#define SET_NOTE    (2 * DLINE + 24)    /* what the switch does */
#define SET_TITLE   (DLINE + 24)        /* "Dictionaries", "Libraries" */
#define SET_LEFT    PAD
#define SET_RIGHT   (W - SCROLL_HIT_W)  /* clear of the scrollbar's grab */
#define SWITCH_W    150
#define LIB_ROW     DLINE               /* a library's line: one, or two if its name is long */
#define LIB_GAP     24

/* The path of the dictionary that is open, and the one chosen from the
 * settings for app_main() to open next (empty: none). */
static char s_open_path[PATH_LEN];
static char s_chosen[PATH_LEN];

/* Defeatist's gear (its ui.c, draw_gear()): a disc with a hole and eight
 * teeth, from rectangles, the diagonals as blocks set out at 45 degrees. */
static void draw_gear(int cx, int cy, int o, uint16_t c, uint16_t bg)
{
    const int r = o - 4, t = 8, s = 13;
    gfx_fill_rect(cx - t, cy - o, 2 * t, o - r + 6, c);
    gfx_fill_rect(cx - t, cy + r - 6, 2 * t, o - r + 6, c);
    gfx_fill_rect(cx - o, cy - t, o - r + 6, 2 * t, c);
    gfx_fill_rect(cx + r - 6, cy - t, o - r + 6, 2 * t, c);
    const int d = (r * 7) / 10;
    gfx_fill_rect(cx + d - 6, cy - d - 6, s, s, c);
    gfx_fill_rect(cx - d - 7, cy - d - 6, s, s, c);
    gfx_fill_rect(cx + d - 6, cy + d - 7, s, s, c);
    gfx_fill_rect(cx - d - 7, cy + d - 7, s, s, c);
    gfx_fill_circle(cx, cy, r, c);
    gfx_fill_circle(cx, cy, 8, bg);
}

static bool in_gear(int x, int y)
{
    return y < HEAD_H && x >= W - GEAR_W;
}

/* 0024: the libraries, three columns -- module, checkout, licence -- the
 * last two as wide as their longest entry and against the right edge. A
 * module whose name does not fit before them has its own line, and its
 * checkout and licence go on the next. */
static int s_lib_x1, s_lib_x2;          /* the checkout's and licence's columns */

static void lib_columns(void)
{
    if (s_lib_x1) return;
    int cw1 = 0, cw2 = 0;
    for (int i = 0; i < LIBVERSIONS_COUNT; i++) {
        int w1 = gfx_text_w(k_libversions[i].checkout, DSCALE);
        int w2 = gfx_text_w(k_libversions[i].licence, DSCALE);
        if (w1 > cw1) cw1 = w1;
        if (w2 > cw2) cw2 = w2;
    }
    s_lib_x2 = SET_RIGHT - cw2;
    s_lib_x1 = s_lib_x2 - LIB_GAP - cw1;
    if (s_lib_x1 < SET_LEFT + 48) s_lib_x1 = SET_LEFT + 48;   /* indented under the name */
    if (s_lib_x2 < s_lib_x1 + cw1 + LIB_GAP) s_lib_x2 = s_lib_x1 + cw1 + LIB_GAP;
}

/* The lines library i takes: one if its name fits before the columns. */
static int lib_lines(int i)
{
    lib_columns();
    return SET_LEFT + gfx_text_w(k_libversions[i].module, DSCALE) + LIB_GAP <= s_lib_x1 ? 1 : 2;
}

/* Where each part starts, down the settings from their top (0), and how
 * tall they are. */
static int set_dicts_y(void) { return SET_ROW + SET_NOTE + SET_TITLE; }
static int set_libs_y(void)  { return set_dicts_y() + (ndicts ? ndicts : 1) * ROW_H; }  /* one for "None found" */

static int set_h(void)
{
    int lines = 0;
    for (int i = 0; i < LIBVERSIONS_COUNT; i++) lines += lib_lines(i);
    return set_libs_y() + SET_TITLE + lines * LIB_ROW + PAD;
}

/* Everything, from oy: the settings' top on the screen, above the screen
 * when they are scrolled. gfx clips at the screen's edges; the caller
 * draws the header and Back over what runs under them. */
static void draw_settings(int oy, bool sounds)
{
    const int sx = SET_RIGHT - SWITCH_W;
    int ty = oy + (SET_ROW - TEXT_H) / 2;
    gfx_draw_text(SET_LEFT, ty, "Sounds like", SCALE, sx - SET_LEFT - PAD, C_TEXT);
    gfx_fill_rect(sx, oy + 14, SWITCH_W, SET_ROW - 28, sounds ? C_HIT_KEY : C_KEY);
    const char *v = sounds ? "on" : "off";
    gfx_draw_text(sx + (SWITCH_W - gfx_text_w(v, SCALE)) / 2, ty, v, SCALE, SWITCH_W, C_TEXT);
    gfx_draw_para(SET_LEFT, oy + SET_ROW + 8,
                  "Words that sound like what you type, for one you can say "
                  "but not spell.",
                  DSCALE, SET_RIGHT - SET_LEFT, DLINE, 2, C_DIM);

    int y = oy + SET_ROW + SET_NOTE;
    gfx_fill_rect(0, y, W - SCROLL_W, 2, C_RULE);
    gfx_draw_text(SET_LEFT, y + 16, "Dictionaries", DSCALE, SET_RIGHT - SET_LEFT, C_DIM);
    for (int i = 0; i < ndicts; i++) {
        int ry = oy + set_dicts_y() + i * ROW_H;
        if (!strcmp(dicts[i], s_open_path)) gfx_fill_rect(0, ry, W - SCROLL_W, ROW_H - 2, C_HIT_KEY);
        gfx_draw_text(SET_LEFT, ry + (ROW_H - TEXT_H) / 2, base(dicts[i]), SCALE,
                      SET_RIGHT - SET_LEFT, C_TEXT);
        gfx_fill_rect(0, ry + ROW_H - 2, W - SCROLL_W, 2, C_RULE);
    }
    if (!ndicts)
        gfx_draw_text(SET_LEFT, oy + set_dicts_y() + (ROW_H - DTEXT_H) / 2,
                      "None found on the card or the drive.", DSCALE, SET_RIGHT - SET_LEFT, C_DIM);

    lib_columns();
    y = oy + set_libs_y();
    gfx_fill_rect(0, y, W - SCROLL_W, 2, C_RULE);
    gfx_draw_text(SET_LEFT, y + 16, "Libraries", DSCALE, SET_RIGHT - SET_LEFT, C_DIM);
    y += SET_TITLE;
    for (int i = 0; i < LIBVERSIONS_COUNT; i++) {
        const libv_t *e = &k_libversions[i];
        if (y > H) break;
        if (lib_lines(i) == 1) {
            gfx_draw_text(SET_LEFT, y, e->module, DSCALE, s_lib_x1 - LIB_GAP - SET_LEFT, C_TEXT);
        } else {
            gfx_draw_text(SET_LEFT, y, e->module, DSCALE, SET_RIGHT - SET_LEFT, C_TEXT);
            y += LIB_ROW;
        }
        gfx_draw_text(s_lib_x1, y, e->checkout, DSCALE, s_lib_x2 - LIB_GAP - s_lib_x1, C_DIM);
        gfx_draw_text(s_lib_x2, y, e->licence, DSCALE, SET_RIGHT - s_lib_x2, C_DIM);
        y += LIB_ROW;
    }
}

enum { SET_CLOSED, SET_CHOSEN, SET_GONE };

/* The settings screen, until Back or the gear (SET_CLOSED), a dictionary
 * chosen (SET_CHOSEN, in s_chosen), or the volume gone (SET_GONE).
 * *sounds is the switch. */
static int settings(sd_dict_t *d, storage_id_t vol, bool *sounds)
{
    const int vy = HEAD_H, vh = H - FOOT_H - HEAD_H, fy = H - FOOT_H;
    int top = 0;                /* pixels scrolled */
    int result = SET_CLOSED;
    for (;;) {
        const int max = set_h() > vh ? set_h() - vh : 0;
        if (top > max) top = max;
        if (top < 0) top = 0;
        gfx_fill_rect(0, 0, W, H, C_BG);
        draw_settings(vy - top, *sounds);
        gfx_fill_rect(0, 0, W, vy, C_BG);
        header_w(sd_name(d), "Settings", W - PAD - GEAR_W);
        draw_gear(W - GEAR_W / 2, HEAD_H / 2, 32, C_HIT, C_HEAD);
        sbar_t sb = { vy, vh, set_h(), vh };   /* in pixels */
        sbar_draw(&sb, top, s_drag);
        key(0, fy, W, FOOT_H, "Back", C_KEY);
        show();

        int x, y;
        touch_ev_t ev;
        s_pan_top = vy;
        s_pan_bot = vy + vh;
        while ((ev = wait_touch(&sb, &x, &y, 250)) == T_NONE)
            if (!storage_present(vol)) { result = SET_GONE; goto out; }
        if (ev == T_DRAG) { top = (int)sbar_top(&sb, y); continue; }
        if (ev == T_PAN) { top -= s_pan_dy; continue; }
        if (ev == T_DROP) continue;
        if (y >= fy || in_gear(x, y)) break;
        if (y < vy) continue;
        const int cy = y - vy + top;
        if (cy < SET_ROW) {
            *sounds = !*sounds;
            ESP_LOGI(TAG, "sounds like: %s", *sounds ? "on" : "off");
        } else if (cy >= set_dicts_y() && cy < set_libs_y()) {
            int i = (cy - set_dicts_y()) / ROW_H;
            if (i < ndicts && strcmp(dicts[i], s_open_path)) {
                snprintf(s_chosen, sizeof s_chosen, "%s", dicts[i]);
                ESP_LOGI(TAG, "changing to %s", base(s_chosen));
                result = SET_CHOSEN;
                break;
            }
        }
    }
out:
    s_pan_top = s_pan_bot = 0;
    return result;
}

/* app_main()'s next dictionary: the one chosen from the panel, once, in a
 * buffer of its own (the next scan() rewrites dicts[]), else the chooser's. */
static const char *pick_or_chosen(void)
{
    static char next[PATH_LEN];
    if (s_chosen[0]) {
        snprintf(next, sizeof next, "%s", s_chosen);
        s_chosen[0] = 0;
        return next;
    }
    return pick();
}

/* sd_sounds_like()'s first call works out every row's keys: say so. */
static void keys_progress(uint32_t done, uint32_t total)
{
    gfx_fill_rect(0, HEAD_H, W, KB_Y - HEAD_H, C_BG);
    gfx_draw_text(PAD, HEAD_H + 40, "Working out how words sound", DSCALE, W - 2 * PAD, C_DIM);
    gfx_fill_rect(PAD, HEAD_H + 100, W - 2 * PAD, 30, C_KEY);
    gfx_fill_rect(PAD, HEAD_H + 100, (int)((int64_t)(W - 2 * PAD) * done / (total ? total : 1)), 30, C_HIT);
    gfx_blit(HEAD_H, KB_Y);
}

/* Until the dictionary's volume goes away. Two lists: the rows from what
 * was typed on (prefix matches highlighted), or with "sounds" on, the
 * rows that sound like it, nearest spelling first. */
static void search(sd_dict_t *d, storage_id_t vol)
{
    char q[64] = "";
    int32_t scroll = 0;
    bool sounds = false;
    static uint32_t hits[MAX_HITS];
    int nhits = 0;
    bool stale = true;          /* hits are not for q */
    for (;;) {
        char big[96];
        uint32_t from = 0;
        uint32_t lb = 0;                            /* 0020: where q starts */
        sbar_t sb = { HEAD_H, ROWS * ROW_H, 0, ROWS };
        gfx_fill_rect(0, 0, W, H, C_BG);
        if (sounds) {
            snprintf(big, sizeof big, "~%s_", q);
            header_w(sd_name(d), big, W - PAD - GEAR_W);
            if (stale) {
                nhits = q[0] ? sd_sounds_like(d, q, hits, MAX_HITS, keys_progress) : 0;
                if (nhits < 0) {
                    message("Not enough memory", "There is not enough memory for the sounds-like keys "
                            "of a dictionary this size.", C_BAD);
                    vTaskDelay(pdMS_TO_TICKS(3000));
                    sounds = false;
                    nhits = 0;
                    continue;
                }
                stale = false;
                gfx_fill_rect(0, HEAD_H, W, KB_Y - HEAD_H, C_BG);
            }
            if (scroll < 0) scroll = 0;
            if (scroll >= nhits) scroll = nhits > ROWS ? (nhits - 1) / ROWS * ROWS : 0;
            for (int r = 0; r < ROWS && scroll + r < nhits; r++)
                draw_row(d, hits[scroll + r], HEAD_H + r * ROW_H, C_TEXT);
            sb.count = nhits;
            sbar_draw(&sb, scroll, s_drag);
            if (q[0] && !nhits)
                gfx_draw_text(PAD, HEAD_H + 30, "Nothing sounds like that.", DSCALE, W - 2 * PAD, C_DIM);
        } else {
            snprintf(big, sizeof big, "%s_", q);
            header_w(sd_name(d), big, W - PAD - GEAR_W);
            lb = sd_lower_bound(d, q);
            int64_t f = (int64_t)lb + scroll;
            if (f < 0) { scroll -= (int32_t)f; f = 0; }
            if (f >= sd_rows(d)) {
                f = sd_rows(d) > ROWS ? sd_rows(d) - ROWS : 0;
                scroll = (int32_t)(f - sd_lower_bound(d, q));
            }
            from = (uint32_t)f;
            for (int r = 0; r < ROWS && from + r < sd_rows(d); r++) {
                uint32_t i = from + (uint32_t)r;
                draw_row(d, i, HEAD_H + r * ROW_H,
                         q[0] && sd_has_prefix(sd_row_word(d, i), q) ? C_HIT : C_TEXT);
            }
            sb.count = (int32_t)sd_rows(d);
            sbar_draw(&sb, (int32_t)from, s_drag);
        }
        draw_keyboard();
        draw_gear(W - GEAR_W / 2, HEAD_H / 2, 32, C_TEXT, C_HEAD);
        show();

        int x, y;
        touch_ev_t ev;
        while ((ev = wait_touch(&sb, &x, &y, 250)) == T_NONE)
            if (!storage_present(vol)) return;
        /* 0020: the alphabetical list keeps its place as an offset from
         * where q starts, so a drag to a row is that row less q's start. */
        if (ev == T_DRAG) {
            int32_t want = sbar_top(&sb, y);
            scroll = sounds ? want : (int32_t)((int64_t)want - lb);
            continue;
        }
        if (ev == T_DROP) {
            ESP_LOGI(TAG, "scrolled to row %ld of %ld",
                     (long)(sounds ? scroll : (int32_t)from), (long)sb.count);
            continue;
        }
        /* 0021: the gear opens the settings; 0026: as a screen of their own. */
        if (in_gear(x, y)) {
            scan();             /* what is on the card and the drive now */
            const bool was = sounds;
            int how = settings(d, vol, &sounds);
            if (how == SET_GONE) return;
            if (how == SET_CHOSEN) return;
            if (sounds != was) {
                scroll = 0;
                stale = true;
            }
            continue;
        }
        size_t l = strlen(q);
        if (y >= HEAD_H && y < KB_Y) {
            int r = (y - HEAD_H) / ROW_H;
            if (r >= ROWS) continue;
            if (sounds) {
                if (scroll + r < nhits && !define(d, sd_row_entry(d, hits[scroll + r]), vol)) return;
            } else if (from + (uint32_t)r < sd_rows(d)) {
                if (!define(d, sd_row_entry(d, from + (uint32_t)r), vol)) return;
            }
        } else if (y >= KB_Y && y < KB_Y + kbd_h(&KBD)) {
            /* 0023: shift and #+ change only the keys' labels, which the
             * redraw shows; a gap in the stagger is nothing. */
            int k = kbd_hit(&KBD, x, y);
            if (k == KBD_STATE || k == KBD_NONE) continue;
            kbd_edit(q, sizeof q, k);
            scroll = 0;
            stale = true;
        } else if (y >= KB_Y + kbd_h(&KBD)) {
            if (x < kb_x(2)) scroll -= ROWS;
            else if (x < kb_x(4)) scroll += ROWS;
            else if (x < kb_x(9)) {
                if (l && l + 1 < sizeof q && q[l - 1] != ' ') { q[l] = ' '; q[l + 1] = 0; }
                scroll = 0;
                stale = true;
            } else {
                q[0] = 0;
                scroll = 0;
                stale = true;
            }
        }
    }
}

/* ---- main --------------------------------------------------------------- */

void app_main(void)
{
    /* 0018: tie the ELF to the launcher's ABI and its storage section; a
     * launcher without either refuses it before any of this runs. */
    feckless_abi_require();
    feckless_abi_require_storage();
    /* u-spurious-0001: and to a box -- a launcher that would run it trusted
     * refuses it by this name; Avoidant's umode branch and this firmware's
     * standalone.c run it in one. */
    feckless_abi_require_box();

    /* Which build this is: the version is the tag or ref CI wrote to
     * version.txt, else git describe. */
    const esp_app_desc_t *app = esp_app_get_description();
    /* 0025: a warning, so the banner stands out in the log (yellow). */
    ESP_LOGW(TAG, "=== Spurious %s, built %s %s, ESP-IDF %s ===",
             app->version, app->date, app->time, app->idf_ver);

    ESP_ERROR_CHECK(tab5io_init());     /* bus, expanders, LCD_RST/TP_RST released */
    esp_lcd_panel_handle_t panel;
    ESP_ERROR_CHECK(lcd_init(&panel));
    ESP_ERROR_CHECK(gfx_init(panel, LCD_H_RES, LCD_V_RES));
    if (touch_init(tab5io_bus(), LCD_H_RES, LCD_V_RES) != ESP_OK)
        ESP_LOGE(TAG, "no touch controller found");
    gfx_fill_rect(0, 0, W, H, C_BG);
    show();
    lcd_backlight_set(BRIGHTNESS);

    /* 0018: the USB host and storage are the launcher's, already up --
     * standalone.c's when this is the .bin. */

    message("Looking for dictionaries", "", C_DIM);
    vTaskDelay(pdMS_TO_TICKS(1500));    /* the card mounts on storage's poll task */

    for (;;) {
        /* 0021: a dictionary chosen from the gear's panel, else the chooser. */
        const char *ifo = pick_or_chosen();
        message("Opening", base(ifo), C_DIM);
        char err[200];
        int64_t t0 = esp_timer_get_time();
        sd_dict_t *d = sd_open(ifo, err, sizeof err);
        if (!d) {
            ESP_LOGE(TAG, "%s: %s", ifo, err);
            message("Cannot open it", err, C_BAD);
            vTaskDelay(pdMS_TO_TICKS(3000));
            continue;
        }
        ESP_LOGI(TAG, "%s: %u words, %u synonyms%s, opened in %lld ms", sd_name(d),
                 (unsigned)sd_count(d), (unsigned)sd_syn_count(d),
                 sd_resorted(d) ? " (out of order: sorted)" : "",
                 (long long)(esp_timer_get_time() - t0) / 1000);
        /* The .dict stays open: a held volume unmounts only after release. */
        storage_id_t vol = storage_of_path(ifo);
        storage_hold(vol);
        snprintf(s_open_path, sizeof s_open_path, "%s", ifo);      /* 0021 */
        search(d, vol);
        sd_close(d);
        storage_hold(STORAGE_COUNT);
    }
}
