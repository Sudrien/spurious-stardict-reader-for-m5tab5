/*
 * StarDict reading. See stardict.h.
 *
 * The .idx is read whole -- a million-word English Wiktionary is around
 * 20 MB of it, which PSRAM holds; the Simple English one is under 1 MB --
 * with an array of where each entry starts, so a lookup is a binary
 * search and no card reads.
 *
 * A .dict.dz is gzip with dictzip's random-access table in the header's
 * extra field: the data is deflated in chunks of a fixed uncompressed
 * size, each flushed so it inflates on its own, and the table gives
 * each chunk's compressed size. A definition is read by inflating the
 * chunks it spans; the last one inflated is kept, as consecutive
 * lookups are often near each other.
 *
 * SPDX-License-Identifier: MIT
 */
#include "stardict.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "miniz_tinfl.h"

/* ---- dictzip ------------------------------------------------------------ */

typedef struct {
    FILE *f;
    uint32_t chlen;         /* uncompressed bytes per chunk */
    uint32_t chcnt;
    uint64_t *at;           /* chcnt + 1 file offsets: chunk i is at[i]..at[i+1] */
    uint8_t *in;            /* largest compressed chunk */
    uint8_t *out;           /* chlen */
    uint32_t cached;        /* chunk in out, or UINT32_MAX */
    size_t cached_len;
    tinfl_decompressor inf;
} dz_t;

static void dz_free(dz_t *z)
{
    if (!z) return;
    free(z->at);
    free(z->in);
    free(z->out);
    free(z);
}

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

/* Parse the gzip header and its RA table; NULL if it is not dictzip. */
static dz_t *dz_open(FILE *f, char *err, size_t err_len)
{
    uint8_t h[12];
    if (fseek(f, 0, SEEK_SET) || fread(h, 1, 12, f) != 12 || h[0] != 0x1f || h[1] != 0x8b || h[2] != 8) {
        snprintf(err, err_len, ".dict.dz is not gzip");
        return NULL;
    }
    uint8_t flg = h[3];
    if (!(flg & 4)) {
        snprintf(err, err_len, ".dict.dz is gzip but not dictzip (no random-access table)");
        return NULL;
    }
    uint16_t xlen = le16(h + 10);
    uint8_t *x = malloc(xlen);
    if (!x || fread(x, 1, xlen, f) != xlen) {
        free(x);
        snprintf(err, err_len, ".dict.dz header is short");
        return NULL;
    }
    dz_t *z = NULL;
    for (size_t p = 0; p + 4 <= xlen;) {
        uint16_t sl = le16(x + p + 2);
        if (x[p] == 'R' && x[p + 1] == 'A' && sl >= 6 && p + 4 + sl <= xlen) {
            const uint8_t *ra = x + p + 4;
            uint32_t chcnt = le16(ra + 4);
            if (le16(ra) != 1 || 6 + 2 * chcnt > sl) break;
            z = calloc(1, sizeof *z);
            if (!z) break;
            z->chlen = le16(ra + 2);
            z->chcnt = chcnt;
            z->at = malloc((chcnt + 1) * sizeof *z->at);
            if (!z->at) { dz_free(z); z = NULL; break; }
            uint32_t biggest = 0;
            z->at[0] = 0;   /* made absolute below */
            for (uint32_t i = 0; i < chcnt; i++) {
                uint16_t c = le16(ra + 6 + 2 * i);
                z->at[i + 1] = z->at[i] + c;
                if (c > biggest) biggest = c;
            }
            z->in = malloc(biggest ? biggest : 1);
            z->out = malloc(z->chlen ? z->chlen : 1);
            if (!z->in || !z->out) { dz_free(z); z = NULL; }
            break;
        }
        p += 4 + sl;
    }
    free(x);
    if (!z) {
        snprintf(err, err_len, ".dict.dz has no usable dictzip table");
        return NULL;
    }
    /* FNAME, FCOMMENT, FHCRC: then the data. */
    if (flg & 8) { int c; while ((c = fgetc(f)) > 0) {} }
    if (flg & 16) { int c; while ((c = fgetc(f)) > 0) {} }
    if (flg & 2) { fgetc(f); fgetc(f); }
    long data = ftell(f);
    for (uint32_t i = 0; i <= z->chcnt; i++) z->at[i] += (uint64_t)data;
    z->f = f;
    z->cached = UINT32_MAX;
    return z;
}

static bool dz_chunk(dz_t *z, uint32_t i)
{
    if (z->cached == i) return true;
    size_t clen = (size_t)(z->at[i + 1] - z->at[i]);
    if (fseek(z->f, (long)z->at[i], SEEK_SET) || fread(z->in, 1, clen, z->f) != clen) return false;
    tinfl_init(&z->inf);
    size_t in_sz = clen, out_sz = z->chlen;
    tinfl_status st = tinfl_decompress(&z->inf, z->in, &in_sz, z->out, z->out, &out_sz,
                                       TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF |
                                       TINFL_FLAG_HAS_MORE_INPUT);
    /* A chunk ends at a flush, not at the end of the stream -- the last
     * one too: the final, empty block lies outside the table -- so
     * inflate asks for more input there, and that is success. */
    if (st != TINFL_STATUS_DONE && st != TINFL_STATUS_NEEDS_MORE_INPUT) {
        z->cached = UINT32_MAX;
        return false;
    }
    z->cached = i;
    z->cached_len = out_sz;
    return true;
}

static bool dz_read(dz_t *z, uint64_t off, size_t len, uint8_t *dst)
{
    while (len) {
        uint32_t i = (uint32_t)(off / z->chlen);
        if (i >= z->chcnt || !dz_chunk(z, i)) return false;
        size_t in = (size_t)(off % z->chlen);
        if (in >= z->cached_len) return false;
        size_t n = z->cached_len - in;
        if (n > len) n = len;
        memcpy(dst, z->out + in, n);
        dst += n;
        off += n;
        len -= n;
    }
    return true;
}

/* ---- the dictionary ----------------------------------------------------- */

struct sd_dict {
    char name[128];
    char types[8];          /* sametypesequence, or "" */
    uint32_t count;
    bool off64;
    uint8_t *idx;
    uint32_t *at;           /* where each .idx entry starts */
    FILE *dict;
    dz_t *dz;               /* NULL for a plain .dict */
};

static void fail(char *err, size_t n, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, n, fmt, ap);
    va_end(ap);
}

/* The path with its ".ifo" replaced by ext. */
static bool sibling(char *out, size_t n, const char *ifo, const char *ext)
{
    size_t l = strlen(ifo);
    if (l < 4 || strcmp(ifo + l - 4, ".ifo")) return false;
    return snprintf(out, n, "%.*s%s", (int)(l - 4), ifo, ext) < (int)n;
}

static void trim(char *s)
{
    size_t l = strlen(s);
    while (l && (s[l - 1] == '\n' || s[l - 1] == '\r' || s[l - 1] == ' ')) s[--l] = 0;
}

sd_dict_t *sd_open(const char *ifo, char *err, size_t err_len)
{
    sd_dict_t *d = calloc(1, sizeof *d);
    char path[300];
    uint32_t idxsize = 0;
    FILE *f = NULL;
    if (!d) { fail(err, err_len, "out of memory"); return NULL; }

    if (!(f = fopen(ifo, "r"))) { fail(err, err_len, "cannot open %s", ifo); goto bad; }
    char line[512];
    if (!fgets(line, sizeof line, f) || strncmp(line, "StarDict's dict ifo file", 24)) {
        fail(err, err_len, "%s is not a StarDict .ifo", ifo);
        goto bad;
    }
    while (fgets(line, sizeof line, f)) {
        trim(line);
        char *v = strchr(line, '=');
        if (!v) continue;
        *v++ = 0;
        if (!strcmp(line, "bookname")) snprintf(d->name, sizeof d->name, "%s", v);
        else if (!strcmp(line, "wordcount")) d->count = (uint32_t)strtoul(v, NULL, 10);
        else if (!strcmp(line, "idxfilesize")) idxsize = (uint32_t)strtoul(v, NULL, 10);
        else if (!strcmp(line, "idxoffsetbits")) d->off64 = atoi(v) == 64;
        else if (!strcmp(line, "sametypesequence")) snprintf(d->types, sizeof d->types, "%s", v);
    }
    fclose(f);
    f = NULL;
    if (!d->count || !idxsize) { fail(err, err_len, "%s has no wordcount or idxfilesize", ifo); goto bad; }

    /* The .idx, whole. A .idx.gz is not handled. */
    if (!sibling(path, sizeof path, ifo, ".idx") || !(f = fopen(path, "rb"))) {
        fail(err, err_len, "no .idx next to %s", ifo);
        goto bad;
    }
    d->idx = malloc(idxsize);
    d->at = malloc(d->count * sizeof *d->at);
    if (!d->idx || !d->at) { fail(err, err_len, "not enough memory for a %u-byte .idx", (unsigned)idxsize); goto bad; }
    if (fread(d->idx, 1, idxsize, f) != idxsize) { fail(err, err_len, ".idx is shorter than the .ifo says"); goto bad; }
    fclose(f);
    f = NULL;
    size_t tail = d->off64 ? 12 : 8;
    uint32_t p = 0, n = 0;
    while (p < idxsize && n < d->count) {
        const uint8_t *z = memchr(d->idx + p, 0, idxsize - p);
        if (!z || (size_t)(z - d->idx) + 1 + tail > idxsize) break;
        d->at[n++] = p;
        p = (uint32_t)(z - d->idx) + 1 + (uint32_t)tail;
    }
    if (n != d->count) { fail(err, err_len, ".idx has %u entries, the .ifo says %u", (unsigned)n, (unsigned)d->count); goto bad; }

    /* The .dict.dz, else the .dict. */
    if (sibling(path, sizeof path, ifo, ".dict.dz") && (d->dict = fopen(path, "rb"))) {
        if (!(d->dz = dz_open(d->dict, err, err_len))) goto bad;
    } else if (!sibling(path, sizeof path, ifo, ".dict") || !(d->dict = fopen(path, "rb"))) {
        fail(err, err_len, "no .dict or .dict.dz next to %s", ifo);
        goto bad;
    }
    if (!d->name[0]) snprintf(d->name, sizeof d->name, "%s", ifo);
    return d;

bad:
    if (f) fclose(f);
    sd_close(d);
    return NULL;
}

void sd_close(sd_dict_t *d)
{
    if (!d) return;
    dz_free(d->dz);
    if (d->dict) fclose(d->dict);
    free(d->idx);
    free(d->at);
    free(d);
}

const char *sd_name(const sd_dict_t *d) { return d->name; }
uint32_t sd_count(const sd_dict_t *d) { return d->count; }
const char *sd_word(const sd_dict_t *d, uint32_t i) { return (const char *)d->idx + d->at[i]; }

/* g_ascii_strcasecmp: StarDict's .idx order, before its strcmp tie-break. */
static int fold_cmp(const char *a, const char *b)
{
    for (;; a++, b++) {
        int ca = tolower((unsigned char)*a), cb = tolower((unsigned char)*b);
        if (*a & 0x80) ca = (unsigned char)*a;
        if (*b & 0x80) cb = (unsigned char)*b;
        if (ca != cb || !ca) return ca - cb;
    }
}

uint32_t sd_lower_bound(const sd_dict_t *d, const char *key)
{
    uint32_t lo = 0, hi = d->count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (fold_cmp(sd_word(d, mid), key) < 0) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

int sd_has_prefix(const char *w, const char *p)
{
    for (; *p; w++, p++) {
        int a = (*w & 0x80) ? (unsigned char)*w : tolower((unsigned char)*w);
        int b = (*p & 0x80) ? (unsigned char)*p : tolower((unsigned char)*p);
        if (a != b) return 0;
    }
    return 1;
}

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

/* ---- HTML to text ------------------------------------------------------- */

typedef struct { char *s; size_t n, cap; } buf_t;

static void put(buf_t *b, const char *s, size_t n)
{
    if (b->n + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 256;
        while (cap < b->n + n + 1) cap *= 2;
        char *t = realloc(b->s, cap);
        if (!t) return;
        b->s = t;
        b->cap = cap;
    }
    memcpy(b->s + b->n, s, n);
    b->n += n;
    b->s[b->n] = 0;
}

static void newline(buf_t *b)
{
    if (b->n && b->s[b->n - 1] != '\n') put(b, "\n", 1);
}

/* Enough HTML for a dictionary entry: block tags end a line, list items
 * get a dash, entities that matter are decoded, everything else is
 * dropped. Not a parser; malformed input comes out as text. */
static void html_text(buf_t *b, const char *s, size_t n)
{
    static const char *const blocks[] = { "p", "/p", "br", "br/", "div", "/div", "li", "/li",
                                          "ul", "/ul", "ol", "/ol", "dl", "/dl", "dd", "/dd",
                                          "dt", "/dt", "tr", "/tr", "table", "/table",
                                          "h1", "h2", "h3", "h4", "h5", "/h1", "/h2", "/h3",
                                          "/h4", "/h5", "blockquote", "/blockquote" };
    for (size_t i = 0; i < n;) {
        if (s[i] == '<') {
            const char *e = memchr(s + i, '>', n - i);
            if (!e) break;
            char tag[16] = "";
            size_t k = 0;
            for (const char *p = s + i + 1; p < e && k < sizeof tag - 1 && !isspace((unsigned char)*p); p++)
                tag[k++] = (char)tolower((unsigned char)*p);
            tag[k] = 0;
            for (size_t t = 0; t < sizeof blocks / sizeof *blocks; t++)
                if (!strcmp(tag, blocks[t])) {
                    newline(b);
                    if (!strcmp(tag, "li")) put(b, "- ", 2);
                    if (tag[0] == 'h' && tag[1] >= '1' && tag[1] <= '5') put(b, "\n", 1);
                    break;
                }
            i = (size_t)(e - s) + 1;
        } else if (s[i] == '&') {
            static const struct { const char *e, *t; } ents[] = {
                { "&amp;", "&" }, { "&lt;", "<" }, { "&gt;", ">" }, { "&quot;", "\"" },
                { "&#39;", "'" }, { "&apos;", "'" }, { "&nbsp;", " " },
            };
            size_t t;
            for (t = 0; t < sizeof ents / sizeof *ents; t++) {
                size_t l = strlen(ents[t].e);
                if (i + l <= n && !strncmp(s + i, ents[t].e, l)) {
                    put(b, ents[t].t, strlen(ents[t].t));
                    i += l;
                    break;
                }
            }
            if (t == sizeof ents / sizeof *ents) put(b, s + i++, 1);
        } else if (s[i] == '\n' || s[i] == '\r') {
            if (b->n && b->s[b->n - 1] != '\n' && b->s[b->n - 1] != ' ') put(b, " ", 1);
            i++;
        } else {
            put(b, s + i++, 1);
        }
    }
}

static void field(buf_t *b, char type, const char *s, size_t n)
{
    if (b->n) newline(b);
    if (type == 'h' || type == 'g' || type == 'x') html_text(b, s, n);
    else if (islower((unsigned char)type)) put(b, s, n);
    /* upper case types (images, sounds, ...) are binary: skipped */
}

char *sd_define(sd_dict_t *d, uint32_t i)
{
    const uint8_t *w = d->idx + d->at[i];
    const uint8_t *t = w + strlen((const char *)w) + 1;
    uint64_t off = d->off64 ? ((uint64_t)be32(t) << 32 | be32(t + 4)) : be32(t);
    uint32_t len = be32(t + (d->off64 ? 8 : 4));
    uint8_t *raw = malloc(len + 1);
    if (!raw) return NULL;
    bool ok = d->dz ? dz_read(d->dz, off, len, raw)
                    : !fseek(d->dict, (long)off, SEEK_SET) && fread(raw, 1, len, d->dict) == len;
    if (!ok) { free(raw); return NULL; }
    raw[len] = 0;

    buf_t b = { 0 };
    put(&b, "", 0);
    size_t p = 0;
    if (d->types[0]) {
        /* sametypesequence: no type bytes; every field but the last is
         * NUL terminated (lower case) or size prefixed (upper case). */
        for (const char *ty = d->types; *ty && p <= len; ty++) {
            bool lastf = !ty[1];
            if (islower((unsigned char)*ty)) {
                size_t n = lastf ? len - p : strnlen((char *)raw + p, len - p);
                field(&b, *ty, (char *)raw + p, n);
                p += n + 1;
            } else {
                size_t n = lastf ? len - p : (p + 4 <= len ? be32(raw + p) : 0);
                if (!lastf) p += 4;
                p += n;
            }
        }
    } else {
        while (p < len) {
            char ty = (char)raw[p++];
            if (islower((unsigned char)ty)) {
                size_t n = strnlen((char *)raw + p, len - p);
                field(&b, ty, (char *)raw + p, n);
                p += n + 1;
            } else {
                if (p + 4 > len) break;
                p += 4 + be32(raw + p);
            }
        }
    }
    free(raw);
    /* leading and trailing blank lines */
    while (b.n && (b.s[b.n - 1] == '\n' || b.s[b.n - 1] == ' ')) b.s[--b.n] = 0;
    size_t lead = strspn(b.s, "\n ");
    if (lead) memmove(b.s, b.s + lead, b.n - lead + 1);
    return b.s;
}
