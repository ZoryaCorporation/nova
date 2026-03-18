/**
 * @file    necho.c
 * @brief   necho — Echo arguments to stdout
 * @author  Anthony Taliento
 * @date    2026-03-16
 * @copyright (c) 2026 Zorya Corporation. MIT License.
 *
 * Standalone tool binary. Links against libnova_toolutil.a only.
 * Usage: necho [text...]
 */

#include "shared/ntool_common.h"

#include <stdio.h>
#include <string.h>

/* ---- Core entry point ---- */

int ntool_echo(const NToolFlags *flags) {
    if (flags == NULL) return -1;
    for (int i = 0; i < flags->subject_count; i++) {
        if (i > 0) putchar(' ');
        fputs(flags->subjects[i], stdout);
    }
    putchar('\n');
    return 0;
}

/* ---- Standalone entry point ---- */

int main(int argc, char **argv) {
    if (argc > 1 && (strcmp(argv[1], "--help") == 0 ||
                     strcmp(argv[1], "-h") == 0)) {
        fprintf(stderr, "Usage: necho [text...]\n");
        fprintf(stderr, "\nEcho arguments to stdout.\n");
        return 0;
    }

    NToolFlags flags;
    if (ntool_parse_flags(argc - 1, argv + 1, &flags) != 0) {
        return 1;
    }

    return ntool_echo(&flags) != 0 ? 1 : 0;
}
