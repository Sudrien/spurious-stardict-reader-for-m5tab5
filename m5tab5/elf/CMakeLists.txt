# 0018: the reader as an ELF for esp_elf -- main/'s reader sources,
# linked position-independent with tinfl linked in and every other call
# left undefined: the feckless ABI's, with its storage section
# (feckless_abi, feckless_abi_storage from feckless-libs-for-m5tab5).
# The build checks the ELF against them. The shape is Lothesome Audio
# Analyzer's and Unprofitable Mahjong's elf/.
#
# The top-level build runs this one (main/CMakeLists.txt) and embeds
# what it makes. On its own:
#   idf.py -C elf -B build-elf build
# writes build-elf/spurious_reader.app.elf.
#
# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 3.16)

# The configuration is the .bin's, copied whole when the top-level
# build passes it: struct layouts and inline functions in the headers
# depend on it -- FatFs's FIL above all -- and the ELF must agree with the
# host it runs on. On its own, the defaults.
#
# Except partitions.csv, which is relative to the project and so would
# be looked for in elf/: made absolute, here and in the defaults (the
# later file wins).
get_filename_component(_m5 "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(_partitions_line "CONFIG_PARTITION_TABLE_CUSTOM_FILENAME=\"${_m5}/partitions.csv\"")
if(SPURIOUS_HOST_SDKCONFIG AND SDKCONFIG)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${SPURIOUS_HOST_SDKCONFIG}")
    file(READ "${SPURIOUS_HOST_SDKCONFIG}" _cfg)
    string(REGEX REPLACE "CONFIG_PARTITION_TABLE_CUSTOM_FILENAME=\"[^\"\n]*\""
           "${_partitions_line}" _cfg "${_cfg}")
    file(WRITE "${SDKCONFIG}.host" "${_cfg}")
    configure_file("${SDKCONFIG}.host" "${SDKCONFIG}" COPYONLY)
endif()
file(WRITE "${CMAKE_BINARY_DIR}/sdkconfig.elf.defaults" "${_partitions_line}\n")
set(SDKCONFIG_DEFAULTS "${_m5}/sdkconfig.defaults"
                       "${CMAKE_BINARY_DIR}/sdkconfig.elf.defaults")

# main is ../main, tinfl is ../components; only what main needs is built.
list(APPEND EXTRA_COMPONENT_DIRS "${_m5}/main" "${_m5}/components")
set(COMPONENTS main)

# The .bin's version, which CI takes from the release tag (version.txt
# beside the .bin's CMakeLists.txt, not here), so the ELF's own
# esp_app_desc_t says the same. On its own, IDF's usual: git describe.
if(SPURIOUS_PROJECT_VER)
    set(PROJECT_VER "${SPURIOUS_PROJECT_VER}")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Tells main/CMakeLists.txt which of its two builds this is.
idf_build_set_property(SPURIOUS_ELF 1)
# One manifest, so one lock: the top level's.
idf_build_set_property(DEPENDENCIES_LOCK "${_m5}/dependencies.lock")

project(spurious_reader)

# Into the ELF, not the ABI: miniz's inflater, renamed so the P4 ROM's
# older miniz cannot bind its symbols (components/tinfl, 0011).
set(ELF_COMPONENTS tinfl)
feckless_abi_elf(spurious_reader)
