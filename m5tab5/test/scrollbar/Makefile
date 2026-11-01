# SPDX-License-Identifier: MIT
# 0020: main.c's scrollbar section, lifted and run on a host (sbtest.c).
MAIN := ../../main/main.c
CFLAGS ?= -std=c11 -O1 -g -Wall -Wextra -Werror -Wno-unused-function \
          -fsanitize=address,undefined -fno-sanitize-recover=all

all: run

# Everything from the section's marker to the next section's, that line
# excluded: the test runs the file's text, not a copy of it.
lifted.inc: $(MAIN)
	sed -n '/^\/\* ---- the scrollbar (0020)/,/^\/\* ---- finding dictionaries/p' $(MAIN) | sed '$$d' > $@
	grep -q 'wait_touch' $@

sbtest: sbtest.c lifted.inc
	$(CC) $(CFLAGS) -o $@ sbtest.c

run: sbtest
	./sbtest

clean:
	rm -f sbtest lifted.inc
.PHONY: all run clean
