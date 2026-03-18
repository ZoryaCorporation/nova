/**
 * @file    ntail.c
 * @brief   ntail — Last N lines of a file
 * @author  Anthony Taliento
 * @date    2026-03-16
 * @copyright (c) 2026 Zorya Corporation. MIT License.
 *
 * Standalone tool binary. Links against libnova_toolutil.a only.
 * Usage: ntail [file...] [-l=N] [-n]
 */

#include "shared/ntool_common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

/* ---- Core entry point ---- */

int ntool_tail(const NToolFlags *flags) {
    if (flags == NULL) return -1;

    int max_lines = flags->limit > 0 ? flags->limit : 10;
    FILE *out = stdout;

    if (flags->subject_count == 0) {
        /* Read from stdin — must buffer all lines */
        if (ntool_stdin_has_data()) {
            char **lines = NULL;
            int line_count = 0;
            int line_cap = 256;

            lines = (char **)malloc((size_t)line_cap * sizeof(char *));
            if (lines == NULL) {
                fprintf(stderr, "ntail: out of memory\n");
                return -1;
            }

            char buf[NTOOL_LINE_MAX];
            while (fgets(buf, (int)sizeof(buf), stdin) != NULL) {
                if (line_count >= line_cap) {
                    line_cap *= 2;
                    char **tmp = (char **)realloc(
                        lines, (size_t)line_cap * sizeof(char *));
                    if (tmp == NULL) {
                        for (int j = 0; j < line_count; j++) free(lines[j]);
                        free(lines);
                        fprintf(stderr, "ntail: out of memory\n");
                        return -1;
                    }
                    lines = tmp;
                }
                lines[line_count] = strdup(buf);
                if (lines[line_count] == NULL) {
                    for (int j = 0; j < line_count; j++) free(lines[j]);
                    free(lines);
                    fprintf(stderr, "ntail: out of memory\n");
                    return -1;
                }
                line_count++;
            }

            int start = line_count - max_lines;
            if (start < 0) start = 0;
            for (int j = start; j < line_count; j++) {
                if (flags->show_numbers) {
                    fprintf(out, "%6d  %s", j + 1, lines[j]);
                } else {
                    fputs(lines[j], out);
                }
            }

            for (int j = 0; j < line_count; j++) free(lines[j]);
            free(lines);
        } else {
            fprintf(stderr, "ntail: no files specified\n");
            return -1;
        }
        return 0;
    }

    int multi = (flags->subject_count > 1);

    for (int fi = 0; fi < flags->subject_count; fi++) {
        const char *path = flags->subjects[fi];
        FILE *fp = fopen(path, "r");
        if (fp == NULL) {
            fprintf(stderr, "ntail: cannot open '%s': %s\n",
                    path, strerror(errno));
            continue;
        }

        /* Buffer all lines from file */
        char **lines = NULL;
        int line_count = 0;
        int line_cap = 256;

        lines = (char **)malloc((size_t)line_cap * sizeof(char *));
        if (lines == NULL) {
            fclose(fp);
            fprintf(stderr, "ntail: out of memory\n");
            continue;
        }

        char buf[NTOOL_LINE_MAX];
        while (fgets(buf, (int)sizeof(buf), fp) != NULL) {
            if (line_count >= line_cap) {
                line_cap *= 2;
                char **tmp = (char **)realloc(
                    lines, (size_t)line_cap * sizeof(char *));
                if (tmp == NULL) {
                    for (int j = 0; j < line_count; j++) free(lines[j]);
                    free(lines);
                    lines = NULL;
                    break;
                }
                lines = tmp;
            }
            lines[line_count] = strdup(buf);
            if (lines[line_count] == NULL) {
                for (int j = 0; j < line_count; j++) free(lines[j]);
                free(lines);
                lines = NULL;
                break;
            }
            line_count++;
        }
        fclose(fp);

        if (lines == NULL) continue;

        if (multi) {
            if (fi > 0) printf("\n");
            printf("==> %s <==\n", path);
        }

        int start = line_count - max_lines;
        if (start < 0) start = 0;
        for (int j = start; j < line_count; j++) {
            if (flags->show_numbers) {
                fprintf(out, "%6d  %s", j + 1, lines[j]);
            } else {
                fputs(lines[j], out);
            }
        }

        for (int j = 0; j < line_count; j++) free(lines[j]);
        free(lines);
    }

    return 0;
}

/* ---- Usage ---- */

static void ntooli_usage(void) {
    fprintf(stderr, "Usage: ntail [file...] [-l=N] [-n]\n");
    fprintf(stderr, "\nShow the last N lines of a file.\n\n");
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

    return ntool_tail(&flags) != 0 ? 1 : 0;
}
