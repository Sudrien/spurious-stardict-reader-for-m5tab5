# SPDX-License-Identifier: MIT
# Host builds of main/stardict.c with the vendored inflater and stdio
# in place of main/sd_io_fatfs.c (lookup.c),
# and of main/dmeta.c (dmeta_dump.c, driven by dmeta_check.py).
TINFL := ../../components/tinfl
CFLAGS ?= -O2 -g -Wall -Wextra -fsanitize=address,undefined
CPPFLAGS += -I../../main -I$(TINFL)

all: lookup dmeta_dump

lookup: lookup.c sd_io_stdio.c ../../main/stardict.c ../../main/dmeta.c $(TINFL)/miniz_tinfl.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $^

dmeta_dump: dmeta_dump.c ../../main/dmeta.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $^

clean:
	rm -f lookup dmeta_dump
.PHONY: all clean
