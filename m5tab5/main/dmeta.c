/*
 * Double Metaphone. See dmeta.h.
 *
 * Written for this project from Philips' published rules, in the order
 * and with the contexts of his reference implementation (as revised by
 * Kevin Atkinson for Aspell), so the keys agree with other Double
 * Metaphone implementations. test/stardict/dmeta_check.py compares it
 * with the Python Metaphone package over a dictionary's headwords; that
 * package departs from the reference in a few places, listed there.
 *
 * One deliberate difference: where the reference appends a space to the
 * alternate key (final -J, Spanish -LL-), nothing is appended. A key with
 * a space in it only ever matched another word with the same oddity.
 *
 * SPDX-License-Identifier: MIT
 */
#include "dmeta.h"

#include <stdarg.h>
#include <stdbool.h>
#include <string.h>

#define MAXW 64

typedef struct {
    char w[MAXW + 1];
    int len, last;
    bool slavo;
    char p[DMETA_LEN + 1], a[DMETA_LEN + 1];
    int np, na;
} dm_t;

static char at(const dm_t *d, int i) { return i < 0 || i >= d->len ? 0 : d->w[i]; }

static bool vowel(const dm_t *d, int i)
{
    char c = at(d, i);
    return c && strchr("AEIOUY", c);
}

/* Whether the n letters at i are one of the NULL-terminated list. */
static bool str_at(const dm_t *d, int i, int n, ...)
{
    if (i < 0 || i + n > d->len) return false;
    va_list ap;
    va_start(ap, n);
    bool hit = false;
    for (const char *s; (s = va_arg(ap, const char *));)
        if (!hit && !strncmp(d->w + i, s, (size_t)n)) hit = true;
    va_end(ap);
    return hit;
}
#define AT(i, n, ...) str_at(d, (i), (n), __VA_ARGS__, (const char *)0)

static void app(char *k, int *n, const char *s)
{
    for (; *s && *n < DMETA_LEN; s++) k[(*n)++] = *s;
    k[*n] = 0;
}

/* Primary p and alternate a; a NULL alternate is the same as p. */
static void add2(dm_t *d, const char *p, const char *a)
{
    app(d->p, &d->np, p);
    app(d->a, &d->na, a ? a : p);
}
#define ADD(p) add2(d, (p), NULL)

/* Fold a UTF-8 word to the upper-case Latin letters the rules are
 * written in: Latin-1 and Latin Extended-A letters lose their accents
 * (Ç is S, as in the reference; Œ is O, ß and ſ S); anything else
 * non-ASCII is dropped. */
static void fold(dm_t *d, const char *s)
{
    static const char c3[64 + 1] =
        /* U+00C0..U+00FF, by the second UTF-8 byte - 0x80 */
        "AAAAAAASEEEEIIII" "DNOOOOO.OUUUUY.S" "AAAAAAASEEEEIIII" "DNOOOOO.OUUUUY.Y";
    static const char c45[128 + 1] =
        /* U+0100..U+017F */
        "AAAAAACCCCCCCCDDDDEEEEEEEEEEGGGGGGGGHHHHIIIIIIIIIIIIJJKKK"
        "LLLLLLLLLLNNNNNNNNNOOOOOOOORRRRRRSSSSSSSSTTTTTTUUUUUUUUUUUUWWYYYZZZZZZS";
    int n = 0;
    for (const unsigned char *u = (const unsigned char *)s; *u && n < MAXW;) {
        if (*u < 0x80) {
            char c = (char)*u++;
            d->w[n++] = (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
        } else if (*u == 0xC3 && u[1] >= 0x80 && u[1] < 0xC0) {
            char c = c3[u[1] - 0x80];
            if (c != '.') d->w[n++] = c;
            u += 2;
        } else if ((*u == 0xC4 || *u == 0xC5) && u[1] >= 0x80 && u[1] < 0xC0) {
            d->w[n++] = c45[(*u - 0xC4) * 64 + u[1] - 0x80];
            u += 2;
        } else {
            u++;
            while ((*u & 0xC0) == 0x80) u++;
        }
    }
    d->w[n] = 0;
    d->len = n;
    d->last = n - 1;
    d->slavo = strchr(d->w, 'W') || strchr(d->w, 'K') || strstr(d->w, "CZ") || strstr(d->w, "WITZ");
}

void dmeta(const char *word, char primary[DMETA_LEN + 1], char alt[DMETA_LEN + 1])
{
    dm_t dd = { 0 }, *d = &dd;
    fold(d, word);
    int cur = 0;

    if (AT(0, 2, "GN", "KN", "PN", "WR", "PS")) cur++;
    if (at(d, 0) == 'X') { ADD("S"); cur++; }   /* Xavier */

    while ((d->np < DMETA_LEN || d->na < DMETA_LEN) && cur < d->len) {
        char c = at(d, cur);
        switch (c) {
        case 'A': case 'E': case 'I': case 'O': case 'U': case 'Y':
            if (cur == 0) ADD("A");
            cur++;
            break;

        case 'B':
            ADD("P");
            cur += at(d, cur + 1) == 'B' ? 2 : 1;
            break;

        case 'C':
            /* various Germanic */
            if (cur > 1 && !vowel(d, cur - 2) && AT(cur - 1, 3, "ACH") && at(d, cur + 2) != 'I' &&
                (at(d, cur + 2) != 'E' || AT(cur - 2, 6, "BACHER", "MACHER"))) {
                ADD("K"); cur += 2; break;
            }
            if (cur == 0 && AT(cur, 6, "CAESAR")) { ADD("S"); cur += 2; break; }
            if (AT(cur, 4, "CHIA")) { ADD("K"); cur += 2; break; }     /* chianti */
            if (AT(cur, 2, "CH")) {
                if (cur > 0 && AT(cur, 4, "CHAE")) { add2(d, "K", "X"); cur += 2; break; }  /* michael */
                if (cur == 0 && (AT(cur + 1, 5, "HARAC", "HARIS") || AT(cur + 1, 3, "HOR", "HYM", "HIA", "HEM")) &&
                    !AT(0, 5, "CHORE")) {
                    ADD("K"); cur += 2; break;                          /* Greek roots */
                }
                if (AT(0, 4, "VAN ", "VON ") || AT(0, 3, "SCH") || AT(cur - 2, 6, "ORCHES", "ARCHIT", "ORCHID") ||
                    AT(cur + 2, 1, "T", "S") ||
                    ((AT(cur - 1, 1, "A", "O", "U", "E") || cur == 0) &&
                     AT(cur + 2, 1, "L", "R", "N", "M", "B", "H", "F", "V", "W", " "))) {
                    ADD("K");
                } else if (cur > 0) {
                    if (AT(0, 2, "MC")) ADD("K");                       /* McHugh */
                    else add2(d, "X", "K");
                } else {
                    ADD("X");
                }
                cur += 2;
                break;
            }
            if (AT(cur, 2, "CZ") && !AT(cur - 2, 4, "WICZ")) { add2(d, "S", "X"); cur += 2; break; }  /* czerny */
            if (AT(cur + 1, 3, "CIA")) { ADD("X"); cur += 3; break; }   /* focaccia */
            if (AT(cur, 2, "CC") && !(cur == 1 && at(d, 0) == 'M')) {   /* not McClellan */
                if (AT(cur + 2, 1, "I", "E", "H") && !AT(cur + 2, 2, "HU")) {
                    if ((cur == 1 && at(d, cur - 1) == 'A') || AT(cur - 1, 5, "UCCEE", "UCCES")) ADD("KS");  /* accident, succeed */
                    else ADD("X");                                      /* bacci */
                    cur += 3;
                    break;
                }
                ADD("K"); cur += 2; break;
            }
            if (AT(cur, 2, "CK", "CG", "CQ")) { ADD("K"); cur += 2; break; }
            if (AT(cur, 2, "CI", "CE", "CY")) {
                if (AT(cur, 3, "CIO", "CIE", "CIA")) add2(d, "S", "X");
                else ADD("S");
                cur += 2;
                break;
            }
            ADD("K");
            if (AT(cur + 1, 2, " C", " Q", " G")) cur += 3;             /* Mac Caffrey */
            else if (AT(cur + 1, 1, "C", "K", "Q") && !AT(cur + 1, 2, "CE", "CI")) cur += 2;
            else cur++;
            break;

        case 'D':
            if (AT(cur, 2, "DG")) {
                if (AT(cur + 2, 1, "I", "E", "Y")) { ADD("J"); cur += 3; }   /* edge */
                else { ADD("TK"); cur += 2; }                           /* edgar */
                break;
            }
            ADD("T");
            cur += AT(cur, 2, "DT", "DD") ? 2 : 1;
            break;

        case 'F':
            ADD("F");
            cur += at(d, cur + 1) == 'F' ? 2 : 1;
            break;

        case 'G':
            if (at(d, cur + 1) == 'H') {
                if (cur > 0 && !vowel(d, cur - 1)) { ADD("K"); cur += 2; break; }
                if (cur == 0) {                                         /* ghislane, ghiradelli */
                    ADD(at(d, cur + 2) == 'I' ? "J" : "K");
                    cur += 2;
                    break;
                }
                /* Parker's rule: hugh, bough, broughton */
                if ((cur > 1 && AT(cur - 2, 1, "B", "H", "D")) || (cur > 2 && AT(cur - 3, 1, "B", "H", "D")) ||
                    (cur > 3 && AT(cur - 4, 1, "B", "H"))) {
                    cur += 2;
                    break;
                }
                if (cur > 2 && at(d, cur - 1) == 'U' && AT(cur - 3, 1, "C", "G", "L", "R", "T")) ADD("F");  /* laugh, tough */
                else if (cur > 0 && at(d, cur - 1) != 'I') ADD("K");
                cur += 2;
                break;
            }
            if (at(d, cur + 1) == 'N') {
                if (cur == 1 && vowel(d, 0) && !d->slavo) add2(d, "KN", "N");
                else if (!AT(cur + 2, 2, "EY") && at(d, cur + 1) != 'Y' && !d->slavo) add2(d, "N", "KN");  /* not cagney */
                else ADD("KN");
                cur += 2;
                break;
            }
            if (AT(cur + 1, 2, "LI") && !d->slavo) { add2(d, "KL", "L"); cur += 2; break; }  /* tagliaro */
            if (cur == 0 && (at(d, cur + 1) == 'Y' ||
                             AT(cur + 1, 2, "ES", "EP", "EB", "EL", "EY", "IB", "IL", "IN", "IE", "EI", "ER"))) {
                add2(d, "K", "J"); cur += 2; break;
            }
            if ((AT(cur + 1, 2, "ER") || at(d, cur + 1) == 'Y') && !AT(0, 6, "DANGER", "RANGER", "MANGER") &&
                !AT(cur - 1, 1, "E", "I") && !AT(cur - 1, 3, "RGY", "OGY")) {
                add2(d, "K", "J"); cur += 2; break;
            }
            if (AT(cur + 1, 1, "E", "I", "Y") || AT(cur - 1, 4, "AGGI", "OGGI")) {  /* Italian biaggi */
                if (AT(0, 4, "VAN ", "VON ") || AT(0, 3, "SCH") || AT(cur + 1, 2, "ET")) ADD("K");
                else if (AT(cur + 1, 4, "IER ")) ADD("J");
                else add2(d, "J", "K");
                cur += 2;
                break;
            }
            ADD("K");
            cur += at(d, cur + 1) == 'G' ? 2 : 1;
            break;

        case 'H':
            if ((cur == 0 || vowel(d, cur - 1)) && vowel(d, cur + 1)) { ADD("H"); cur += 2; }
            else cur++;
            break;

        case 'J':
            if (AT(cur, 4, "JOSE") || AT(0, 4, "SAN ")) {               /* Spanish */
                if ((cur == 0 && at(d, cur + 4) == ' ') || AT(0, 4, "SAN ")) ADD("H");
                else add2(d, "J", "H");
                cur++;
                break;
            }
            if (cur == 0) add2(d, "J", "A");                            /* Yankelovich/Jankelowicz */
            else if (vowel(d, cur - 1) && !d->slavo && (at(d, cur + 1) == 'A' || at(d, cur + 1) == 'O')) add2(d, "J", "H");
            else if (cur == d->last) add2(d, "J", "");
            else if (!AT(cur + 1, 1, "L", "T", "K", "S", "N", "M", "B", "Z") && !AT(cur - 1, 1, "S", "K", "L")) ADD("J");
            cur += at(d, cur + 1) == 'J' ? 2 : 1;
            break;

        case 'K':
            ADD("K");
            cur += at(d, cur + 1) == 'K' ? 2 : 1;
            break;

        case 'L':
            if (at(d, cur + 1) == 'L') {
                if ((cur == d->len - 3 && AT(cur - 1, 4, "ILLO", "ILLA", "ALLE")) ||
                    ((AT(d->last - 1, 2, "AS", "OS") || AT(d->last, 1, "A", "O")) && AT(cur - 1, 4, "ALLE"))) {
                    add2(d, "L", "");                                   /* Spanish cabrillo */
                    cur += 2;
                    break;
                }
                cur += 2;
            } else {
                cur++;
            }
            ADD("L");
            break;

        case 'M':
            if ((AT(cur - 1, 3, "UMB") && (cur + 1 == d->last || AT(cur + 2, 2, "ER"))) || at(d, cur + 1) == 'M') cur += 2;
            else cur++;
            ADD("M");
            break;

        case 'N':
            ADD("N");
            cur += at(d, cur + 1) == 'N' ? 2 : 1;
            break;

        case 'P':
            if (at(d, cur + 1) == 'H') { ADD("F"); cur += 2; break; }
            ADD("P");
            cur += AT(cur + 1, 1, "P", "B") ? 2 : 1;                    /* campbell */
            break;

        case 'Q':
            ADD("K");
            cur += at(d, cur + 1) == 'Q' ? 2 : 1;
            break;

        case 'R':
            if (cur == d->last && !d->slavo && AT(cur - 2, 2, "IE") && !AT(cur - 4, 2, "ME", "MA"))
                add2(d, "", "R");                                       /* French rogier */
            else
                ADD("R");
            cur += at(d, cur + 1) == 'R' ? 2 : 1;
            break;

        case 'S':
            if (AT(cur - 1, 3, "ISL", "YSL")) { cur++; break; }         /* island, carlisle */
            if (cur == 0 && AT(cur, 5, "SUGAR")) { add2(d, "X", "S"); cur++; break; }
            if (AT(cur, 2, "SH")) {
                ADD(AT(cur + 1, 4, "HEIM", "HOEK", "HOLM", "HOLZ") ? "S" : "X");
                cur += 2;
                break;
            }
            if (AT(cur, 3, "SIO", "SIA") || AT(cur, 4, "SIAN")) {      /* Italian, Armenian */
                if (!d->slavo) add2(d, "S", "X");
                else ADD("S");
                cur += 3;
                break;
            }
            if ((cur == 0 && AT(cur + 1, 1, "M", "N", "L", "W")) || AT(cur + 1, 1, "Z")) {  /* smith/schmidt */
                add2(d, "S", "X");
                cur += AT(cur + 1, 1, "Z") ? 2 : 1;
                break;
            }
            if (AT(cur, 2, "SC")) {
                if (at(d, cur + 2) == 'H') {                            /* Schlesinger's rule */
                    if (AT(cur + 3, 2, "OO", "ER", "EN", "UY", "ED", "EM")) {
                        if (AT(cur + 3, 2, "ER", "EN")) add2(d, "X", "SK");
                        else ADD("SK");
                    } else if (cur == 0 && !vowel(d, 3) && at(d, 3) != 'W') {
                        add2(d, "X", "S");
                    } else {
                        ADD("X");
                    }
                    cur += 3;
                    break;
                }
                ADD(AT(cur + 2, 1, "I", "E", "Y") ? "S" : "SK");
                cur += 3;
                break;
            }
            if (cur == d->last && AT(cur - 2, 2, "AI", "OI")) add2(d, "", "S");  /* French resnais */
            else ADD("S");
            cur += AT(cur + 1, 1, "S", "Z") ? 2 : 1;
            break;

        case 'T':
            if (AT(cur, 4, "TION") || AT(cur, 3, "TIA", "TCH")) { ADD("X"); cur += 3; break; }
            if (AT(cur, 2, "TH") || AT(cur, 3, "TTH")) {
                if (AT(cur + 2, 2, "OM", "AM") || AT(0, 4, "VAN ", "VON ") || AT(0, 3, "SCH")) ADD("T");  /* thomas */
                else add2(d, "0", "T");
                cur += 2;
                break;
            }
            ADD("T");
            cur += AT(cur + 1, 1, "T", "D") ? 2 : 1;
            break;

        case 'V':
            ADD("F");
            cur += at(d, cur + 1) == 'V' ? 2 : 1;
            break;

        case 'W':
            if (AT(cur, 2, "WR")) { ADD("R"); cur += 2; break; }
            if (cur == 0 && (vowel(d, cur + 1) || AT(cur, 2, "WH"))) {
                if (vowel(d, cur + 1)) add2(d, "A", "F");               /* Wasserman/Vasserman */
                else ADD("A");
            }
            if ((cur == d->last && vowel(d, cur - 1)) || AT(cur - 1, 5, "EWSKI", "EWSKY", "OWSKI", "OWSKY") ||
                AT(0, 3, "SCH")) {
                add2(d, "", "F");                                       /* Arnow/Arnoff */
                cur++;
                break;
            }
            if (AT(cur, 4, "WICZ", "WITZ")) { add2(d, "TS", "FX"); cur += 4; break; }
            cur++;
            break;

        case 'X':
            if (!(cur == d->last && (AT(cur - 3, 3, "IAU", "EAU") || AT(cur - 2, 2, "AU", "OU")))) ADD("KS");  /* not breaux */
            cur += AT(cur + 1, 1, "C", "X") ? 2 : 1;
            break;

        case 'Z':
            if (at(d, cur + 1) == 'H') { ADD("J"); cur += 2; break; }   /* Chinese zhao */
            if (AT(cur + 1, 2, "ZO", "ZI", "ZA") || (d->slavo && cur > 0 && at(d, cur - 1) != 'T')) add2(d, "S", "TS");
            else ADD("S");
            cur += at(d, cur + 1) == 'Z' ? 2 : 1;
            break;

        default:
            cur++;
            break;
        }
    }
    memcpy(primary, d->p, DMETA_LEN + 1);
    memcpy(alt, d->a, DMETA_LEN + 1);
}
