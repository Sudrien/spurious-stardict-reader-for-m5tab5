# SPDX-License-Identifier: MIT
cmake_minimum_required(VERSION 3.16)

# exFAT is feckless-libs' fatfs component since 0017, named in
# main/idf_component.yml: it patches this IDF's FatFs at configure time.
# CONFIG_FECKLESS_FATFS_EXFAT=n builds FAT32 only.

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(spurious)
