/*
 * A StarDict dictionary: the .ifo, the .idx (held in memory), and the
 * .dict or dictzip'd .dict.dz (read a definition at a time).
 *
 * Plain C over stdio, no ESP-IDF, so test/stardict/ builds it on a host
 * against a real dictionary.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

typedef struct sd_dict sd_dict_t;

/* Open from the path of its .ifo; the .idx and .dict[.dz] are found next
 * to it by the same stem. NULL on failure, with the reason in err. */
sd_dict_t *sd_open(const char *ifo_path, char *err, size_t err_len);
void sd_close(sd_dict_t *d);

const char *sd_name(const sd_dict_t *d);     /* the .ifo's bookname */
uint32_t sd_count(const sd_dict_t *d);       /* headwords in the .idx */
const char *sd_word(const sd_dict_t *d, uint32_t i);

/* The first headword at or after `key` in the .idx's own order (ASCII
 * case folded, as StarDict sorts). sd_count() when there is none. So a
 * prefix search is this, then words while they start with the prefix. */
uint32_t sd_lower_bound(const sd_dict_t *d, const char *key);

/* Whether `word` starts with `prefix`, ASCII case folded. */
int sd_has_prefix(const char *word, const char *prefix);

/* Headword i's definition as plain text: HTML is reduced to text with
 * line breaks. malloc'd and NUL terminated, for the caller to free; NULL
 * on a read error. */
char *sd_define(sd_dict_t *d, uint32_t i);
