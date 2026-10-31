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
 */
#include <stdint.h>

#include "esp_log.h"
#include "feckless_abi.h"
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

    int ret = feckless_abi_run(elf_start, (size_t)(elf_end - elf_start));
    ESP_LOGE("standalone", "the reader did not start: %d", ret);
}
