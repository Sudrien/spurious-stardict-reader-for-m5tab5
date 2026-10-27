/*
 * A StarDict dictionary: the .ifo, the .idx (held in memory), and the
 * .dict or dictzip'd .dict.dz (read a definition at a time).
 *
 * And the .syn, if there is one, plain or gzipped. Plain C over stdio,
 * no ESP-IDF, so test/stardict/ builds it on a host against a real
 * dictionary.
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

/*
 * Rows: the headwords and the .syn's synonyms, merged in the .idx's
 * order, which is what a list shows. A synonym row leads to another
 * headword's entry. Without a .syn, rows are the headwords.
 */
uint32_t sd_rows(const sd_dict_t *d);
const char *sd_row_word(const sd_dict_t *d, uint32_t r);
uint32_t sd_row_entry(const sd_dict_t *d, uint32_t r);    /* for sd_word(), sd_define() */
int sd_row_is_syn(const sd_dict_t *d, uint32_t r);
uint32_t sd_syn_count(const sd_dict_t *d);

/* Whether the .idx or .syn was out of StarDict's order, so the rows had
 * to be sorted at open (pyGlossary has written such files). */
int sd_resorted(const sd_dict_t *d);

/* The first row at or after `key` (ASCII case folded, as StarDict
 * sorts); sd_rows() when there is none. A prefix search is this, then
 * rows while they start with the prefix. */
uint32_t sd_lower_bound(const sd_dict_t *d, const char *key);

/*
 * Rows that sound like `key`: whose Double Metaphone keys share one with
 * key's, nearest in spelling first, at most `max` into `out`; returns how
 * many (of the first 4096 that share a key). The
 * first call works out every row's keys (a few seconds for a big
 * dictionary), calling progress(done, total) along the way if it is not
 * NULL; later calls only compare. -1 when there is no memory for them.
 */
int sd_sounds_like(sd_dict_t *d, const char *key, uint32_t *out, int max,
                   void (*progress)(uint32_t done, uint32_t total));

/* Whether `word` starts with `prefix`, ASCII case folded. */
int sd_has_prefix(const char *word, const char *prefix);

/* Headword i's definition as plain text: HTML is reduced to text with
 * line breaks. malloc'd and NUL terminated, for the caller to free; NULL
 * on a read error. */
char *sd_define(sd_dict_t *d, uint32_t i);
