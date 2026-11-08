/* SPDX-License-Identifier: MIT
 *
 * 0018: the .bin's app_main(). The reader is the ELF that ../elf/ built
 * from main.c and the rest, embedded in this firmware, and this loads it
 * the way a launcher would load the published copy: through the
 * feckless ABI, with its storage section (feckless_abi_storage), which
 * this firmware links.
 *
 * What a launcher brings up before it runs an app is brought up here,
 * in Avoidant's order: NVS; the bus and expanders, which hold the USB
 * host's power; the USB host before storage, which registers mass
 * storage as a class with it; storage; then the port, which powers USB-A.
 * The reader finds the card and the drive mounted. Its own board inits
 * (tab5io_init(), lcd_init() and the rest) return ESP_OK when run again.
 *
 * u-spurious-0001: the reader is built for a box, so it runs in one here as
 * on Avoidant's umode branch -- RISC-V user mode, in one block of PSRAM the
 * PMP grants it and nothing else, every call outside it answered by
 * feckless_box_calls -- and not through feckless_abi_run(), which would
 * refuse it by the name feckless_abi_box_v1.
 */
#include <stdint.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "feckless_abi.h"
#include "feckless_box_calls.h"
#include "feckless_umode_idf.h"
#include "feckless_umode_load.h"
#include "nvs_flash.h"
#include "storage.h"
#include "storage_io.h"
#include "tab5io.h"
#include "usbhost.h"

extern const uint8_t elf_start[] asm("_binary_spurious_reader_app_elf_start");
extern const uint8_t elf_end[]   asm("_binary_spurious_reader_app_elf_end");

void app_main(void)
{
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    ESP_ERROR_CHECK(tab5io_init());
    ESP_ERROR_CHECK(usbhost_init(tab5io_exp2()));
    /* storage polls the slot every second, and each poll with no card
     * logs three errors from IDF's sdmmc and FAT layers and a line from
     * SD_HOST. tab5_storage logs the mount when there is one; that says
     * enough. */
    esp_log_level_set("sdmmc_common", ESP_LOG_NONE);
    esp_log_level_set("vfs_fat_sdmmc", ESP_LOG_NONE);
    esp_log_level_set("SD_HOST", ESP_LOG_WARN);
    storage_io_init();
    static const storage_usb_t usb = {
        .register_class = usbhost_register_class,
        .set_power      = usbhost_set_power,
        .powered        = usbhost_powered,
    };
    ESP_ERROR_CHECK(storage_init(&usb));
    usbhost_start();

    /* The box: the largest PSRAM block less 1 MB for this firmware, at most
     * 24 MB; 64 KB of it the reader's stack. The stack guard's interrupt
     * out of every core's routing first, which a window needs. */
    feckless_umode_guard_unroute();
    size_t big = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    size_t box = big > (1u << 20) ? big - (1u << 20) : 0;
    if (box > (24u << 20)) box = 24u << 20;
    box &= ~(size_t)63;
    uint32_t n;
    const feckless_umode_call_t *calls = feckless_box_calls_v1(&n);
    feckless_umode_app_t *app = NULL;
    e = feckless_umode_load(elf_start, (size_t)(elf_end - elf_start), calls, n,
                            (uint32_t)box, 64 * 1024, &app);
    if (e != ESP_OK) {
        ESP_LOGE("standalone", "the reader would not load in a box: %s", esp_err_to_name(e));
        return;
    }
    ESP_LOGI("standalone", "the reader in a box of %u KB", (unsigned)(box / 1024));
    const feckless_umode_end_t end = feckless_umode_app_run(app);
    ESP_LOGE("standalone", "the reader ended: reason %d, %s at 0x%08x", (int)end.reason,
             end.reason == FECKLESS_UMODE_END_FAULT ? feckless_umode_cause_name(end.mcause) : "-",
             (unsigned)end.pc);
    feckless_umode_unload(app);
    feckless_box_calls_end();
}
