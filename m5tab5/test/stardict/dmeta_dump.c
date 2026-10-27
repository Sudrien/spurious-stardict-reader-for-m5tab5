/*
 * Host harness for main/dmeta.c: one word per line in, "word<TAB>primary
 * <TAB>alternate" out. dmeta_check.py drives it.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <string.h>

#include "dmeta.h"

int main(void)
{
    char line[1024], p[DMETA_LEN + 1], a[DMETA_LEN + 1];
    while (fgets(line, sizeof line, stdin)) {
        line[strcspn(line, "\n")] = 0;
        dmeta(line, p, a);
        printf("%s\t%s\t%s\n", line, p, a);
    }
    return 0;
}
