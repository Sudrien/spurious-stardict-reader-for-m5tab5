#pragma once
/*
 * Written by hand for this component (miniz's CMake would generate it).
 *
 * The renames are the point of it. The ESP32-P4's ROM before rev 3
 * (esp32p4.rom.eco0_4.ld) has its own, older miniz, and its linker
 * script defines tinfl_decompress and friends at ROM addresses -- which
 * win over these at link time. The ROM's tinfl_decompressor is not this
 * one's, so the ROM inflater wrote past the struct this header sizes:
 * heap corruption on the Tab5 (0011). Every name this component exports
 * is prefixed, so nothing can bind to the ROM's.
 */
#define MINIZ_EXPORT

#define tinfl_decompress                 spurious_tinfl_decompress
#define tinfl_decompress_mem_to_heap     spurious_tinfl_decompress_mem_to_heap
#define tinfl_decompress_mem_to_mem      spurious_tinfl_decompress_mem_to_mem
#define tinfl_decompress_mem_to_callback spurious_tinfl_decompress_mem_to_callback
#define tinfl_decompressor_alloc         spurious_tinfl_decompressor_alloc
#define tinfl_decompressor_free          spurious_tinfl_decompressor_free
#define miniz_def_alloc_func             spurious_miniz_def_alloc_func
#define miniz_def_free_func              spurious_miniz_def_free_func
#define miniz_def_realloc_func           spurious_miniz_def_realloc_func
