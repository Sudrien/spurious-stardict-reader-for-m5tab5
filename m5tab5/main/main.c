/*
 * Spurious -- a StarDict reader for the M5Stack Tab5.
 *
 * Finds dictionaries (.ifo with its .idx and .dict or .dict.dz) under
 * /dict on the microSD card or a USB drive, one level of folders deep,
 * as tools/zim2stardict.py writes them. With one, it opens; with more,
 * a list. Then: type on the keyboard, the list shows the headwords (and
 * the .syn's synonyms) from there on, tap one for its definition. With
 * "sounds" on, the list is instead the words that sound like what was
 * typed (Double Metaphone, main/dmeta.c).
 *
 * The board bring-up is Duplicitous's (duplicitous-for-m5tab5): the
 * feckless components for the panel, touch, USB host and storage.
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
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "storage.h"
#include "storage_io.h"
#include "usbhost.h"

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

static void header(const char *small, const char *big)
{
    gfx_fill_rect(0, 0, W, HEAD_H, C_HEAD);
    if (small) gfx_draw_text(PAD, 20, small, DSCALE, W - 2 * PAD, C_DIM);
    if (big) gfx_draw_text(PAD, 20 + DTEXT_H + 20, big, SCALE, W - 2 * PAD, C_TEXT);
}

static void message(const char *title, const char *body, uint16_t c)
{
    gfx_fill_rect(0, 0, W, H, C_BG);
    header("Spurious", title);
    gfx_draw_para(PAD, HEAD_H + 40, body, DSCALE, W - 2 * PAD, DLINE, 40, c);
    show();
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
        int fy = H - FOOT_H, bw = W / 3;
        key(0, fy, bw, FOOT_H, "Back", C_KEY);
        key(bw, fy, bw, FOOT_H, "Up", top > 0 ? C_KEY : C_RULE);
        key(2 * bw, fy, W - 2 * bw, FOOT_H, "Down", top + DROWS < (int)n ? C_KEY : C_RULE);
        show();
        int x, y;
        while (!wait_tap(&x, &y, 250))
            if (!storage_present(vol)) { ok = false; goto out; }
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

static const char *const kb[3][10] = {
    { "q", "w", "e", "r", "t", "y", "u", "i", "o", "p" },
    { "a", "s", "d", "f", "g", "h", "j", "k", "l", "'" },
    { "z", "x", "c", "v", "b", "n", "m", "-", ".", "del" },
};

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

static void draw_keyboard(bool sounds)
{
    int kw = W / 10;
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 10; c++)
            key(c * kw, KB_Y + r * KEY_H, kw, KEY_H, kb[r][c], C_KEY);
    int y = KB_Y + 3 * KEY_H;
    key(0, y, 2 * kw, KEY_H, "Up", C_KEY);
    key(2 * kw, y, 2 * kw, KEY_H, "Down", C_KEY);
    key(4 * kw, y, 2 * kw, KEY_H, "space", C_KEY);
    key(6 * kw, y, 2 * kw, KEY_H, "sounds", sounds ? C_HIT_KEY : C_KEY);
    key(8 * kw, y, W - 8 * kw, KEY_H, "clear", C_KEY);
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
        gfx_fill_rect(0, 0, W, H, C_BG);
        if (sounds) {
            snprintf(big, sizeof big, "~%s_", q);
            header(sd_name(d), big);
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
            if (q[0] && !nhits)
                gfx_draw_text(PAD, HEAD_H + 30, "Nothing sounds like that.", DSCALE, W - 2 * PAD, C_DIM);
        } else {
            snprintf(big, sizeof big, "%s_", q);
            header(sd_name(d), big);
            int64_t f = (int64_t)sd_lower_bound(d, q) + scroll;
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
        }
        draw_keyboard(sounds);
        show();

        int x, y;
        while (!wait_tap(&x, &y, 250))
            if (!storage_present(vol)) return;
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
            const char *k = kb[(y - KB_Y) / KEY_H][x / kw < 10 ? x / kw : 9];
            if (!strcmp(k, "del")) { if (l) q[l - 1] = 0; }
            else if (l + 1 < sizeof q) { q[l] = k[0]; q[l + 1] = 0; }
            scroll = 0;
            stale = true;
        } else if (y >= KB_Y + 3 * KEY_H) {
            if (x < 2 * kw) scroll -= ROWS;
            else if (x < 4 * kw) scroll += ROWS;
            else if (x < 6 * kw) {
                if (l && l + 1 < sizeof q && q[l - 1] != ' ') { q[l] = ' '; q[l + 1] = 0; }
                scroll = 0;
                stale = true;
            } else if (x < 8 * kw) {
                sounds = !sounds;
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
    /* Which build this is: the version is the tag or ref CI wrote to
     * version.txt, else git describe. */
    const esp_app_desc_t *app = esp_app_get_description();
    ESP_LOGI(TAG, "=== Spurious %s, built %s %s, ESP-IDF %s ===",
             app->version, app->date, app->time, app->idf_ver);

    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    ESP_ERROR_CHECK(tab5io_init());     /* bus, expanders, LCD_RST/TP_RST released */
    esp_lcd_panel_handle_t panel;
    ESP_ERROR_CHECK(lcd_init(&panel));
    ESP_ERROR_CHECK(gfx_init(panel, LCD_H_RES, LCD_V_RES));
    if (touch_init(tab5io_bus(), LCD_H_RES, LCD_V_RES) != ESP_OK)
        ESP_LOGE(TAG, "no touch controller found");
    gfx_fill_rect(0, 0, W, H, C_BG);
    show();
    lcd_backlight_set(BRIGHTNESS);

    /* As Duplicitous: the USB host before storage, which registers mass
     * storage as a class with it; then the port, which powers USB-A. */
    ESP_ERROR_CHECK(usbhost_init(tab5io_exp2()));
    /* storage polls the slot every second, and each poll with no card
     * logs three errors from IDF's sdmmc and FAT layers and a line from
     * SD_HOST. tab5_storage logs the mount when there is one; that says
     * enough. */
    esp_log_level_set("sdmmc_common", ESP_LOG_NONE);
    esp_log_level_set("vfs_fat_sdmmc", ESP_LOG_NONE);
    esp_log_level_set("SD_HOST", ESP_LOG_WARN);
    storage_io_init();
    static const storage_usb_t usb = {
        .register_class = usbhost_register_class,
        .set_power      = usbhost_set_power,
        .powered        = usbhost_powered,
    };
    ESP_ERROR_CHECK(storage_init(&usb));
    usbhost_start();

    message("Looking for dictionaries", "", C_DIM);
    vTaskDelay(pdMS_TO_TICKS(1500));    /* the card mounts on storage's poll task */

    for (;;) {
        const char *ifo = pick();
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
        search(d, vol);
        sd_close(d);
        storage_hold(STORAGE_COUNT);
    }
}
