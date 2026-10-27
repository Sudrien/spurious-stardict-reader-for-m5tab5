# miniz 3.0.2's inflater (MIT, see LICENSE), for .dict.dz and .syn.dz.
# The ESP32-P4's ROM before rev 3 has an older miniz, whose symbols the
# ROM linker script binds first; miniz_export.h renames these so they
# cannot be (0011). Only miniz_tinfl.c and the two headers it needs, with
# its #include "miniz.h" changed to "miniz_tinfl.h" -- IDF's esp_rom ships
# a miniz.h of its own -- and miniz_export.h written by hand.
idf_component_register(
    SRCS "miniz_tinfl.c"
    INCLUDE_DIRS "."
)
target_compile_definitions(${COMPONENT_LIB} PUBLIC MINIZ_NO_STDIO MINIZ_NO_TIME)
