# SPDX-License-Identifier: MIT
idf_component_register(
    SRCS "main.c" "stardict.c" "dmeta.c"
    INCLUDE_DIRS "."
    REQUIRES esp_app_format nvs_flash fatfs vfs esp_lcd esp_timer tinfl
             feckless_graphics_handler feckless_drivers feckless_storage_handler
)
