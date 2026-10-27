# SPDX-License-Identifier: MIT
# Host build of main/stardict.c and the vendored inflater. See lookup.c.
TINFL := ../../components/tinfl
CFLAGS ?= -O2 -g -Wall -Wextra -fsanitize=address,undefined
CPPFLAGS += -I../../main -I$(TINFL)

lookup: lookup.c ../../main/stardict.c $(TINFL)/miniz_tinfl.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $^

clean:
	rm -f lookup
.PHONY: clean
