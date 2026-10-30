/*
 * sd_io_stdio.c -- main/sd_io.h on a host, with fseeko(). The Tab5's is
 * main/sd_io_fatfs.c.
 *
 * SPDX-License-Identifier: MIT
 */
#define _FILE_OFFSET_BITS 64
#define _POSIX_C_SOURCE 200809L
#include "sd_io.h"

#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>

struct sd_file {
    FILE *f;
    uint64_t size;
};

sd_file_t *sd_file_open(const char *path)
{
    sd_file_t *f = calloc(1, sizeof *f);
    if (!f || !(f->f = fopen(path, "rb")) || fseeko(f->f, 0, SEEK_END)) goto bad;
    off_t n = ftello(f->f);
    if (n < 0) goto bad;
    f->size = (uint64_t)n;
    return f;
bad:
    if (f && f->f) fclose(f->f);
    free(f);
    return NULL;
}

void sd_file_close(sd_file_t *f)
{
    if (!f) return;
    fclose(f->f);
    free(f);
}

uint64_t sd_file_size(const sd_file_t *f) { return f ? f->size : 0; }

size_t sd_file_read(sd_file_t *f, uint64_t off, size_t len, void *dst)
{
    if (!f || off >= f->size || fseeko(f->f, (off_t)off, SEEK_SET)) return 0;
    return fread(dst, 1, len, f->f);
}
