# SPDX-License-Identifier: MIT
#
# 0018: one directory, two builds. ../elf/ sets SPURIOUS_ELF and builds
# the reader into an ELF for esp_elf, checked against the feckless ABI;
# the top level builds standalone.c, runs ../elf/ and embeds what it
# made. The shape is Lothesome's and Unprofitable Mahjong's.
idf_build_get_property(_spurious_elf SPURIOUS_ELF)

if(_spurious_elf)
    idf_component_register(
        SRCS "main.c" "stardict.c" "sd_io_fatfs.c" "dmeta.c"
        INCLUDE_DIRS "."
        REQUIRES esp_app_format fatfs vfs esp_lcd esp_timer
                 tinfl                       # linked into the ELF
                 feckless_abi feckless_abi_storage
                 feckless_graphics_handler feckless_drivers feckless_storage_handler
    )
    return()
endif()

idf_component_register(
    SRCS "standalone.c"
    PRIV_REQUIRES feckless_abi feckless_abi_storage nvs_flash
                  feckless_drivers feckless_storage_handler
)

if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    include(ExternalProject)
    idf_build_get_property(_idf_path IDF_PATH)
    idf_build_get_property(_target IDF_TARGET)
    idf_build_get_property(_build_dir BUILD_DIR)
    idf_build_get_property(_sdkconfig SDKCONFIG)
    idf_build_get_property(_project_dir PROJECT_DIR)
    idf_build_get_property(_project_ver PROJECT_VER)

    set(_elf_dir "${_build_dir}/elf")
    set(_elf "${_elf_dir}/spurious_reader.app.elf")

    externalproject_add(spurious_elf
        SOURCE_DIR "${_project_dir}/elf"
        BINARY_DIR "${_elf_dir}"
        CMAKE_ARGS -DIDF_PATH=${_idf_path}
                   -DIDF_TARGET=${_target}
                   -DSDKCONFIG=${_elf_dir}/sdkconfig
                   -DSPURIOUS_HOST_SDKCONFIG=${_sdkconfig}
                   -DSPURIOUS_PROJECT_VER=${_project_ver}
        CMAKE_GENERATOR "Unix Makefiles"
        BUILD_COMMAND make elf
        BUILD_ALWAYS 1
        BUILD_BYPRODUCTS "${_elf}"
        INSTALL_COMMAND ""
    )

    # elf/ checked it against the ABI as it linked it; embedding waits
    # for that.
    add_dependencies(${COMPONENT_LIB} spurious_elf)

    # elf_loader's: 4-byte aligned, as esp_elf_relocate() wants it.
    elf_embed_aligned_binary(${COMPONENT_LIB} "${_elf}" BINARY)
endif()
