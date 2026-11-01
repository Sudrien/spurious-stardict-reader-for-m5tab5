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
#define KB_Y        (H - 4 * KEY_H)
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
    T_DROP,                 /* off it again */
} touch_ev_t;

static bool s_drag;         /* a finger on the scrollbar, between calls */

/* wait_tap(), and the scrollbar: a finger that goes down on sb's grab
 * zone is a drag, reported at each position it moves to and once more
 * when it lifts, instead of a tap. sb may be NULL. */
static touch_ev_t wait_touch(const sbar_t *sb, int *x, int *y, int ms)
{
    static int drag_y;
    int64_t until = esp_timer_get_time() + (int64_t)ms * 1000;
    bool down = false;
    for (;;) {
        int tx, ty;
        bool now = touch_get(&tx, &ty);
        if (s_drag) {
            if (!now) { s_drag = false; return T_DROP; }
            if (ty != drag_y) { drag_y = ty; *x = tx; *y = ty; return T_DRAG; }
        } else {
            if (now && !down) {
                down = true; *x = tx; *y = ty;
                if (sbar_grab(sb, tx, ty)) { s_drag = true; drag_y = ty; return T_DRAG; }
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
    int top = 0;
    bool ok = true;
    char buf[512];
    for (;;) {
        gfx_fill_rect(0, 0, W, H, C_BG);
        char sub[96];
        snprintf(sub, sizeof sub, "%s  %d/%d", sd_name(d), n ? top / DROWS + 1 : 1,
                 (int)((n + DROWS - 1) / DROWS) ? (int)((n + DROWS - 1) / DROWS) : 1);
        header(sub, sd_word(d, i));
        for (int r = 0; r < DROWS && top + r < (int)n; r++) {
            const line_t *l = &lines[top + r];
            size_t m = l->n < sizeof buf - 1 ? l->n : sizeof buf - 1;
            memcpy(buf, l->s, m);
            buf[m] = 0;
            gfx_draw_text(PAD, HEAD_H + 20 + r * DLINE, buf, DSCALE, W - 2 * PAD, C_TEXT);
        }
        sbar_t sb = { HEAD_H + 20, DROWS * DLINE, (int32_t)n, DROWS };   /* 0020 */
        sbar_draw(&sb, top, s_drag);
        int fy = H - FOOT_H, bw = W / 3;
        key(0, fy, bw, FOOT_H, "Back", C_KEY);
        key(bw, fy, bw, FOOT_H, "Up", top > 0 ? C_KEY : C_RULE);
        key(2 * bw, fy, W - 2 * bw, FOOT_H, "Down", top + DROWS < (int)n ? C_KEY : C_RULE);
        show();
        int x, y;
        touch_ev_t ev;
        while ((ev = wait_touch(&sb, &x, &y, 250)) == T_NONE)
            if (!storage_present(vol)) { ok = false; goto out; }
        if (ev == T_DRAG) { top = (int)sbar_top(&sb, y); continue; }
        if (ev == T_DROP) {
            ESP_LOGI(TAG, "scrolled to line %d of %d", top, (int)n);
            continue;
        }
        if (y < fy) continue;
        if (x < bw) break;
        if (x < 2 * bw) { if (top > 0) top -= DROWS; }
        else if (top + DROWS < (int)n) top += DROWS;
        if (top < 0) top = 0;
    }
out:
    free(lines);
    free(text);
    return ok;
}

/* ---- search ------------------------------------------------------------- */

/* 0019: the three rows of keys are feckless_keyboard's (feckless-libs'
 * 8010), which is this file's kb[] and key() as they were; the row under
 * them is still this file's. */
static const kbd_t KBD = { .x = 0, .y = KB_Y, .w = W, .key_h = KEY_H,
                           .scale = SCALE, .key = C_KEY, .text = C_TEXT };

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
    int kw = W / 10;
    kbd_draw(&KBD);
    int y = KB_Y + 3 * KEY_H;
    key(0, y, 2 * kw, KEY_H, "Up", C_KEY);
    key(2 * kw, y, 2 * kw, KEY_H, "Down", C_KEY);
    key(4 * kw, y, 4 * kw, KEY_H, "space", C_KEY);
    key(8 * kw, y, W - 8 * kw, KEY_H, "clear", C_KEY);
}

/* ---- the gear and its panel (0021) -------------------------------------- */

/* The search screen's settings, behind a gear at the right of its header:
 * the switches first -- for now one, "Sounds like", which was a key in the
 * row under the keyboard and read there as a key-click setting -- and
 * under them the dictionaries on the card and the drive, to change to
 * another without restarting. The panel sits over the word list (and the
 * keyboard, with enough dictionaries); a tap on the switch flips it, on a
 * dictionary opens it, and anywhere else, the gear included, puts the
 * panel away. */
#define GEAR_W      HEAD_H              /* the square it is tapped in */
#define PANEL_X     PAD
#define PANEL_Y     (HEAD_H + 16)
#define PANEL_W     (W - 2 * PAD)
#define PANEL_ROW   (ROW_H + 12)        /* the switch's row */
#define PANEL_NOTE  (2 * DLINE + 24)    /* what the switch does */
#define PANEL_TITLE (DLINE + 24)        /* "Dictionaries" */
#define PANEL_MAX_Y (H - PAD)
#define SWITCH_W    150

/* The path of the dictionary that is open, and the one chosen from the
 * panel for app_main() to open next (empty: none). */
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

/* Where the dictionary rows start, and how many of them fit. */
static int panel_list_y(void)
{
    return PANEL_Y + PANEL_ROW + PANEL_NOTE + PANEL_TITLE;
}

static int panel_rows(void)
{
    int fit = (PANEL_MAX_Y - panel_list_y()) / ROW_H;
    return ndicts < fit ? ndicts : fit;
}

static int panel_h(void)
{
    return panel_list_y() + panel_rows() * ROW_H - PANEL_Y;
}

#define PANEL_SWITCH    (-1)
#define PANEL_INSIDE    (-2)        /* on the panel, on nothing */
#define PANEL_OUTSIDE   (-3)

/* What a tap at (x, y) is on: a dictionary's index, or one of the above. */
static int panel_hit(int x, int y)
{
    if (x < PANEL_X || x >= PANEL_X + PANEL_W || y < PANEL_Y || y >= PANEL_Y + panel_h())
        return PANEL_OUTSIDE;
    if (y < PANEL_Y + PANEL_ROW) return PANEL_SWITCH;
    if (y < panel_list_y()) return PANEL_INSIDE;
    int i = (y - panel_list_y()) / ROW_H;
    return i < panel_rows() ? i : PANEL_INSIDE;
}

static void draw_panel(bool sounds)
{
    int h = panel_h();
    gfx_fill_rect(PANEL_X - 2, PANEL_Y - 2, PANEL_W + 4, h + 4, C_RULE);
    gfx_fill_rect(PANEL_X, PANEL_Y, PANEL_W, h, C_HEAD);

    int ty = PANEL_Y + (PANEL_ROW - TEXT_H) / 2;
    gfx_draw_text(PANEL_X + PAD, ty, "Sounds like", SCALE, PANEL_W - SWITCH_W - 3 * PAD, C_TEXT);
    int sx = PANEL_X + PANEL_W - PAD - SWITCH_W;
    gfx_fill_rect(sx, PANEL_Y + 14, SWITCH_W, PANEL_ROW - 28, sounds ? C_HIT_KEY : C_KEY);
    const char *v = sounds ? "on" : "off";
    gfx_draw_text(sx + (SWITCH_W - gfx_text_w(v, SCALE)) / 2, ty, v, SCALE, SWITCH_W, C_TEXT);
    gfx_draw_para(PANEL_X + PAD, PANEL_Y + PANEL_ROW + 8,
                  "Words that sound like what you type, for one you can say "
                  "but not spell.",
                  DSCALE, PANEL_W - 2 * PAD, DLINE, 2, C_DIM);

    int y = PANEL_Y + PANEL_ROW + PANEL_NOTE;
    gfx_fill_rect(PANEL_X, y, PANEL_W, 2, C_RULE);
    gfx_draw_text(PANEL_X + PAD, y + 16, "Dictionaries", DSCALE, PANEL_W - 2 * PAD, C_DIM);
    for (int i = 0; i < panel_rows(); i++) {
        int ry = panel_list_y() + i * ROW_H;
        bool open = !strcmp(dicts[i], s_open_path);
        if (open) gfx_fill_rect(PANEL_X, ry, PANEL_W, ROW_H - 2, C_HIT_KEY);
        gfx_draw_text(PANEL_X + PAD, ry + (ROW_H - TEXT_H) / 2, base(dicts[i]), SCALE,
                      PANEL_W - 2 * PAD, C_TEXT);
        gfx_fill_rect(PANEL_X, ry + ROW_H - 2, PANEL_W, 2, C_RULE);
    }
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
    bool panel = false;         /* 0021: the gear's panel is up */
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
        draw_gear(W - GEAR_W / 2, HEAD_H / 2, 32, panel ? C_HIT : C_TEXT, C_HEAD);
        if (panel) draw_panel(sounds);
        show();

        int x, y;
        touch_ev_t ev;
        while ((ev = wait_touch(panel ? NULL : &sb, &x, &y, 250)) == T_NONE)
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
        /* 0021: with the panel up, a tap is the switch, a dictionary, or
         * puts the panel away. */
        if (panel) {
            int hit = panel_hit(x, y);
            if (hit == PANEL_SWITCH) {
                sounds = !sounds;
                scroll = 0;
                stale = true;
                ESP_LOGI(TAG, "sounds like: %s", sounds ? "on" : "off");
            } else if (hit >= 0 && strcmp(dicts[hit], s_open_path)) {
                snprintf(s_chosen, sizeof s_chosen, "%s", dicts[hit]);
                ESP_LOGI(TAG, "changing to %s", base(s_chosen));
                return;
            } else if (hit != PANEL_INSIDE) {
                panel = false;
            }
            continue;
        }
        if (in_gear(x, y)) {
            scan();             /* what is on the card and the drive now */
            panel = true;
            continue;
        }
        size_t l = strlen(q);
        int kw = W / 10;
        if (y >= HEAD_H && y < KB_Y) {
            int r = (y - HEAD_H) / ROW_H;
            if (r >= ROWS) continue;
            if (sounds) {
                if (scroll + r < nhits && !define(d, sd_row_entry(d, hits[scroll + r]), vol)) return;
            } else if (from + (uint32_t)r < sd_rows(d)) {
                if (!define(d, sd_row_entry(d, from + (uint32_t)r), vol)) return;
            }
        } else if (y >= KB_Y && y < KB_Y + 3 * KEY_H) {
            kbd_edit(q, sizeof q, kbd_hit(&KBD, x, y));
            scroll = 0;
            stale = true;
        } else if (y >= KB_Y + 3 * KEY_H) {
            if (x < 2 * kw) scroll -= ROWS;
            else if (x < 4 * kw) scroll += ROWS;
            else if (x < 8 * kw) {
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

    /* Which build this is: the version is the tag or ref CI wrote to
     * version.txt, else git describe. */
    const esp_app_desc_t *app = esp_app_get_description();
    ESP_LOGI(TAG, "=== Spurious %s, built %s %s, ESP-IDF %s ===",
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
