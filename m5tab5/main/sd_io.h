/*
 * sd_io.h -- random reads from a dictionary's .dict or .dict.dz.
 *
 * Offsets are 64 bits. On the Tab5 (sd_io_fatfs.c) the file is read
 * through FatFs itself, as aimless-moving-map's ffread.c does: off_t is
 * 32 bits on ESP-IDF, so fseek() through the VFS stops at 2 GiB, while
 * f_lseek() takes FSIZE_t, 64 bits with exFAT. On a host
 * (test/stardict/sd_io_stdio.c) it is plain fseeko().
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

typedef struct sd_file sd_file_t;

/* A path as fopen() takes it ("/sd/dict/x.dict"). NULL if it will not open. */
sd_file_t *sd_file_open(const char *path);
void sd_file_close(sd_file_t *f);
uint64_t sd_file_size(const sd_file_t *f);

/* Up to len bytes at off into dst; how many were read (short at the end
 * of the file), or 0 on an error. */
size_t sd_file_read(sd_file_t *f, uint64_t off, size_t len, void *dst);
