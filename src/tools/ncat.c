/**
 * @file    ncat.c
 * @brief   ncat — Concatenate and print files
 * @author  Anthony Taliento
 * @date    2026-03-16
 * @copyright (c) 2026 Zorya Corporation. MIT License.
 *
 * Standalone tool binary. Links against libnova_toolutil.a only.
 * Usage: ncat [file...] [-n] [-o output] [-A]
 */

#include "shared/ntool_common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

/* ---- Internal helpers ---- */

static int ntooli_cat_file(const char *path, FILE *out,
                           int show_numbers, int *line_num) {
    FILE *fp = fopen(path, "r");
    if (fp == NULL) {
        fprintf(stderr, "ncat: cannot open '%s': %s\n",
                path, strerror(errno));
        return -1;
    }

    char line[NTOOL_LINE_MAX];
    while (fgets(line, (int)sizeof(line), fp) != NULL) {
        if (show_numbers) {
            fprintf(out, "%6d  %s", *line_num, line);
        } else {
            fputs(line, out);
        }
        (*line_num)++;
    }

    fclose(fp);
    return 0;
}

/* ---- Core entry point ---- */

int ntool_cat(const NToolFlags *flags) {
    if (flags == NULL) return -1;

    FILE *out = stdout;
    if (flags->output != NULL) {
        out = fopen(flags->output,
                    flags->append_mode ? "a" : "w");
        if (out == NULL) {
            fprintf(stderr, "ncat: cannot open output '%s': %s\n",
                    flags->output, strerror(errno));
            return -1;
        }
    }

    int result = 0;
    int line_num = 1;

    if (flags->subject_count == 0) {
        if (ntool_stdin_has_data()) {
            char line[NTOOL_LINE_MAX];
            while (fgets(line, (int)sizeof(line), stdin) != NULL) {
                if (flags->show_numbers) {
                    fprintf(out, "%6d  %s", line_num, line);
                } else {
                    fputs(line, out);
                }
                line_num++;
            }
        } else {
            fprintf(stderr, "ncat: no files specified\n");
            result = -1;
        }
    } else {
        for (int i = 0; i < flags->subject_count; i++) {
            if (ntooli_cat_file(flags->subjects[i], out,
                                flags->show_numbers, &line_num) != 0) {
                result = -1;
            }
        }
    }

    if (flags->output != NULL && out != stdout) {
        fclose(out);
    }
    return result;
}

/* ---- Usage ---- */

static void ntooli_usage(void) {
    fprintf(stderr, "Usage: ncat [file...] [-n] [-o output] [-A]\n");
    fprintf(stderr, "\nConcatenate and print files.\n\n");
    fprintf(stderr, "Flags:\n");
    fprintf(stderr, "  -n, --number      Show line numbers\n");
    fprintf(stderr, "  -o, --output=FILE Write output to file\n");
    fprintf(stderr, "  -A, --append      Append to output file\n");
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

    return ntool_cat(&flags) != 0 ? 1 : 0;
}
