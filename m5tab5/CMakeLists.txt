# SPDX-License-Identifier: MIT
cmake_minimum_required(VERSION 3.16)

# exFAT: a patched copy of this IDF's fatfs into components/fatfs, made
# at configure time and before project.cmake scans components/. From
# duplicitous-for-m5tab5 (and defeatist-music-player-for-m5tab5 before
# it); see the file. TAB5_NO_EXFAT in the environment opts out.
include(${CMAKE_CURRENT_LIST_DIR}/cmake/idfcopy.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/cmake/exfat.cmake)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(spurious)
