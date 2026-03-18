/**
 * @file    nhead.c
 * @brief   nhead — First N lines of a file
 * @author  Anthony Taliento
 * @date    2026-03-16
 * @copyright (c) 2026 Zorya Corporation. MIT License.
 *
 * Standalone tool binary. Links against libnova_toolutil.a only.
 * Usage: nhead [file...] [-l=N] [-n]
 */

#include "shared/ntool_common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

/* ---- Core entry point ---- */

int ntool_head(const NToolFlags *flags) {
    if (flags == NULL) return -1;

    int max_lines = flags->limit > 0 ? flags->limit : 10;
    FILE *out = stdout;

    if (flags->subject_count == 0) {
        if (ntool_stdin_has_data()) {
            char line[NTOOL_LINE_MAX];
            int ln = 0;
            while (ln < max_lines &&
                   fgets(line, (int)sizeof(line), stdin) != NULL) {
                if (flags->show_numbers) {
                    fprintf(out, "%6d  %s", ln + 1, line);
                } else {
                    fputs(line, out);
                }
                ln++;
            }
        } else {
            fprintf(stderr, "nhead: no files specified\n");
            return -1;
        }
        return 0;
    }

    int multi = (flags->subject_count > 1);

    for (int fi = 0; fi < flags->subject_count; fi++) {
        const char *path = flags->subjects[fi];
        FILE *fp = fopen(path, "r");
        if (fp == NULL) {
            fprintf(stderr, "nhead: cannot open '%s': %s\n",
                    path, strerror(errno));
            continue;
        }

        if (multi) {
            if (fi > 0) printf("\n");
            printf("==> %s <==\n", path);
        }

        char line[NTOOL_LINE_MAX];
        int ln = 0;
        while (ln < max_lines &&
               fgets(line, (int)sizeof(line), fp) != NULL) {
            if (flags->show_numbers) {
                fprintf(out, "%6d  %s", ln + 1, line);
            } else {
                fputs(line, out);
            }
            ln++;
        }

        fclose(fp);
    }

    return 0;
}

/* ---- Usage ---- */

static void ntooli_usage(void) {
    fprintf(stderr, "Usage: nhead [file...] [-l=N] [-n]\n");
    fprintf(stderr, "\nShow the first N lines of a file.\n\n");
    fprintf(stderr, "Flags:\n");
    fprintf(stderr, "  -l, --limit=N   Number of lines (default: 10)\n");
    fprintf(stderr, "  -n, --number    Show line numbers\n");
}

/* ---- Standalone entry point ---- */

int main(int argc, char **argv) {
    if (argc > 1 && (strcmp(argv[1], "--help") == 0 ||
                     strcmp(argv[1], "-h") == 0)) {
        ntooli_usage();
        return 0;
    }

    NToolFlags flags;
    if (ntool_parse_flags(argc - 1, argv + 1, &flags) != 0) {
        return 1;
    }

    return ntool_head(&flags) != 0 ? 1 : 0;
}
