/*
 * Double Metaphone (Lawrence Philips, C/C++ Users Journal, June 2000):
 * a primary and an alternate phonetic key for an English word, so that
 * words which sound alike -- "nife" and "knife", "fonetic" and
 * "phonetic" -- get the same key.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#define DMETA_LEN   4   /* key length, as Philips': longer keys match too little */

/* Keys for `word` (UTF-8; letters outside A-Z are skipped, a few accented
 * Latin ones folded first). Each out is DMETA_LEN + 1 bytes, NUL
 * terminated; alt equals primary when the word has one reading. */
void dmeta(const char *word, char primary[DMETA_LEN + 1], char alt[DMETA_LEN + 1]);
