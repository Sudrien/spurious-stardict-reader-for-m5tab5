/*
 * Host test of main/stardict.c: open a dictionary and look words up.
 *
 *   make -C test/stardict
 *   test/stardict/lookup ../out/<name>/<name>.ifo cat run '!'
 *
 * For each word: the rows it is a prefix of (the first few; a synonym
 * shows its headword after "->"), then the exact match's definition as
 * the reader would show it. A word starting with ~ is a sounds-like
 * search instead. With no words, every definition is read once, which
 * exercises each dictzip chunk, and every synonym's target is checked.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "stardict.h"

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s <dict.ifo> [word...]\n", argv[0]);
        return 2;
    }
    char err[256];
    sd_dict_t *d = sd_open(argv[1], err, sizeof err);
    if (!d) {
        fprintf(stderr, "open: %s\n", err);
        return 1;
    }
    printf("%s: %u words, %u synonyms, %u rows%s\n", sd_name(d), (unsigned)sd_count(d),
           (unsigned)sd_syn_count(d), (unsigned)sd_rows(d),
           sd_resorted(d) ? " (files out of order: sorted at open)" : "");
    if (argc == 2) {
        size_t bytes = 0;
        for (uint32_t i = 0; i < sd_count(d); i++) {
            char *t = sd_define(d, i);
            if (!t) {
                printf("FAIL: definition %u (%s) did not read\n", (unsigned)i, sd_word(d, i));
                return 1;
            }
            bytes += strlen(t);
            free(t);
        }
        printf("read all %u definitions, %zu bytes of text\n", (unsigned)sd_count(d), bytes);
        /* rows in order, and each synonym leading to a headword */
        for (uint32_t r = 1; r < sd_rows(d); r++)
            if (strcasecmp(sd_row_word(d, r - 1), sd_row_word(d, r)) > 0 &&
                !((sd_row_word(d, r - 1)[0] | sd_row_word(d, r)[0]) & 0x80) &&
                strspn(sd_row_word(d, r - 1), "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ '-") == strlen(sd_row_word(d, r - 1))) {
                printf("FAIL: rows %u and %u out of order: %s, %s\n", (unsigned)r - 1, (unsigned)r,
                       sd_row_word(d, r - 1), sd_row_word(d, r));
                return 1;
            }
        uint32_t nsyn = 0;
        for (uint32_t r = 0; r < sd_rows(d); r++)
            if (sd_row_is_syn(d, r)) {
                nsyn++;
                if (sd_row_entry(d, r) >= sd_count(d)) { printf("FAIL: synonym row %u\n", (unsigned)r); return 1; }
            }
        printf("rows in order; %u synonym rows, each to a headword\n", (unsigned)nsyn);
        clock_t t = clock();
        uint32_t hits[8];
        sd_sounds_like(d, "x", hits, 8, NULL);
        printf("sounds-like keys for %u rows in %.0f ms\n", (unsigned)sd_rows(d),
               (double)(clock() - t) * 1000 / CLOCKS_PER_SEC);
    }
    for (int a = 2; a < argc; a++) {
        if (argv[a][0] == '~') {
            uint32_t hits[12];
            int n = sd_sounds_like(d, argv[a] + 1, hits, 12, NULL);
            printf("== sounds like %s: %d:", argv[a] + 1, n);
            for (int k = 0; k < n; k++) printf(" [%s]", sd_row_word(d, hits[k]));
            printf("\n");
            continue;
        }
        uint32_t i = sd_lower_bound(d, argv[a]);
        printf("== %s: from row %u:", argv[a], (unsigned)i);
        for (uint32_t j = i; j < sd_rows(d) && j < i + 6 && sd_has_prefix(sd_row_word(d, j), argv[a]); j++) {
            if (sd_row_is_syn(d, j)) printf(" [%s -> %s]", sd_row_word(d, j), sd_word(d, sd_row_entry(d, j)));
            else printf(" [%s]", sd_row_word(d, j));
        }
        printf("\n");
        if (i < sd_rows(d) && !strcmp(sd_row_word(d, i), argv[a])) {
            char *t = sd_define(d, sd_row_entry(d, i));
            printf("%.300s\n", t ? t : "(read failed)");
            free(t);
        }
    }
    sd_close(d);
    return 0;
}
