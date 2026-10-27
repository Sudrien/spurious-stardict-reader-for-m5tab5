/*
 * Host test of main/stardict.c: open a dictionary and look words up.
 *
 *   make -C test/stardict
 *   test/stardict/lookup ../out/<name>/<name>.ifo cat run '!'
 *
 * For each word: the headwords it is a prefix of (the first few), then
 * the exact match's definition as the reader would show it. With no
 * words, every definition is read once, which exercises each dictzip
 * chunk.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    printf("%s: %u words\n", sd_name(d), (unsigned)sd_count(d));
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
    }
    for (int a = 2; a < argc; a++) {
        uint32_t i = sd_lower_bound(d, argv[a]);
        printf("== %s: from #%u:", argv[a], (unsigned)i);
        for (uint32_t j = i; j < sd_count(d) && j < i + 6 && sd_has_prefix(sd_word(d, j), argv[a]); j++)
            printf(" [%s]", sd_word(d, j));
        printf("\n");
        if (i < sd_count(d) && !strcmp(sd_word(d, i), argv[a])) {
            char *t = sd_define(d, i);
            printf("%s\n", t ? t : "(read failed)");
            free(t);
        }
    }
    sd_close(d);
    return 0;
}
