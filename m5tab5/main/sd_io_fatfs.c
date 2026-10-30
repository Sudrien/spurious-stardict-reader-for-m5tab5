/*
 * sd_io_fatfs.c -- see sd_io.h. From aimless-moving-map's main/ffread.c
 * (itself its original/bigfile.cpp), on FAT32 or exFAT.
 *
 * The VFS path is turned into FatFs's: the volume from
 * storage_of_path(), its drive from storage_ff_drive(), and the rest of
 * the path after the mount point. Fast seek (CONFIG_FATFS_USE_FASTSEEK)
 * builds a cluster map at open; without it every seek walks the FAT
 * chain, which aimless measured at ~500 ms on a large file.
 *
 * Each read holds feckless-storage's I/O lease a STORAGE_IO_CHUNK at a
 * time, as the VFS path's reads did.
 *
 * SPDX-License-Identifier: MIT
 */
#include "sd_io.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "ff.h"

#include "storage.h"
#include "storage_io.h"

static const char *TAG = "sd_io";

struct sd_file {
    FIL fp;
    uint64_t size;
    char path[300];
    DWORD *clmt;
};

/* The cluster map for fast seek: 256 items first, then whatever FatFs
 * says it needs, up to 16384. */
static void clmt_build(sd_file_t *f)
{
#if FF_USE_FASTSEEK
    uint32_t items = 256;
    for (int attempt = 0; attempt < 2; attempt++) {
        DWORD *tbl = heap_caps_malloc(items * sizeof(DWORD), MALLOC_CAP_SPIRAM);
        if (!tbl) tbl = malloc(items * sizeof(DWORD));
        if (!tbl) break;
        f->fp.cltbl = tbl;
        tbl[0] = items;
        const FRESULT r = f_lseek(&f->fp, CREATE_LINKMAP);
        if (r == FR_OK) {
            f->clmt = tbl;
            f_lseek(&f->fp, 0);
            ESP_LOGI(TAG, "%s: fast seek, %u of %u table items", f->path,
                     (unsigned)tbl[0], (unsigned)items);
            return;
        }
        const uint32_t need = (r == FR_NOT_ENOUGH_CORE) ? (uint32_t)tbl[0] : 0;
        f->fp.cltbl = NULL;
        free(tbl);
        if (!need || need > 16384 || attempt == 1) break;
        items = need;
    }
    ESP_LOGW(TAG, "%s: no fast seek; every seek walks the FAT chain", f->path);
#else
    ESP_LOGW(TAG, "%s: built without CONFIG_FATFS_USE_FASTSEEK; seeks walk the FAT chain", f->path);
#endif
}

sd_file_t *sd_file_open(const char *path)
{
    const storage_id_t id = storage_of_path(path);
    if (id >= STORAGE_COUNT) return NULL;
    const int drive = storage_ff_drive(id);
    const char *mount = storage_mount_path(id);
    const size_t ml = strlen(mount);
    if (drive < 0 || strncmp(path, mount, ml)) return NULL;
    const char *rest = path + ml;
    sd_file_t *f = calloc(1, sizeof *f);
    if (!f) return NULL;
    const int n = snprintf(f->path, sizeof f->path, "%d:%s%s", drive, rest[0] == '/' ? "" : "/", rest);
    if (n < 0 || (size_t)n >= sizeof f->path) { free(f); return NULL; }
    storage_io_acquire(STORAGE_IO_PLAYBACK);
    const FRESULT r = f_open(&f->fp, f->path, FA_READ);
    if (r == FR_OK) {
        f->size = (uint64_t)f_size(&f->fp);
        clmt_build(f);
    }
    storage_io_release();
    if (r != FR_OK) { free(f); return NULL; }
    return f;
}

void sd_file_close(sd_file_t *f)
{
    if (!f) return;
    f_close(&f->fp);
#if FF_USE_FASTSEEK
    f->fp.cltbl = NULL;
#endif
    free(f->clmt);
    free(f);
}

uint64_t sd_file_size(const sd_file_t *f) { return f ? f->size : 0; }

size_t sd_file_read(sd_file_t *f, uint64_t off, size_t len, void *dst)
{
    if (!f || off >= f->size) return 0;
    if (len > f->size - off) len = (size_t)(f->size - off);
    size_t done = 0;
    while (done < len) {
        const UINT want = (UINT)(len - done > STORAGE_IO_CHUNK ? STORAGE_IO_CHUNK : len - done);
        UINT got = 0;
        storage_io_acquire(STORAGE_IO_PLAYBACK);
        bool ok = f_lseek(&f->fp, (FSIZE_t)(off + done)) == FR_OK &&
                  f_read(&f->fp, (uint8_t *)dst + done, want, &got) == FR_OK;
        storage_io_release();
        if (!ok || !got) return 0;
        done += got;
    }
    return done;
}
