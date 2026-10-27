# miniz 3.0.2's inflater (MIT, see LICENSE), for .dict.dz: the ESP32-P4's
# ROM has no tinfl, unlike the older chips'. Only miniz_tinfl.c and the
# two headers it needs, with its #include "miniz.h" changed to
# "miniz_tinfl.h" -- IDF's esp_rom ships a miniz.h of its own -- and
# miniz_export.h, which miniz's CMake would generate, written by hand.
idf_component_register(
    SRCS "miniz_tinfl.c"
    INCLUDE_DIRS "."
)
target_compile_definitions(${COMPONENT_LIB} PUBLIC MINIZ_NO_STDIO MINIZ_NO_TIME)
