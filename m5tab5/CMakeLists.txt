# SPDX-License-Identifier: MIT
cmake_minimum_required(VERSION 3.16)

# exFAT is feckless-libs' fatfs component since 0017, named in
# main/idf_component.yml: it patches this IDF's FatFs at configure time.
# CONFIG_FECKLESS_FATFS_EXFAT=n builds FAT32 only.

# 0018: the reader itself is an ELF built against the feckless ABI, by
# elf/; this build is main/standalone.c, which brings the board's storage
# up, embeds that ELF and loads it. The same ELF runs on a launcher
# (Avoidant) with no install. On its own:
#   idf.py -C elf -B build-elf build

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(spurious)
