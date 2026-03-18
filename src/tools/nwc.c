/**
 * @file    nwc.c
 * @brief   nwc — Word, line, and character count
 * @author  Anthony Taliento
 * @date    2026-03-16
 * @copyright (c) 2026 Zorya Corporation. MIT License.
 *
 * Standalone tool binary. Links against libnova_toolutil.a only.
 * Usage: nwc [file...] [--lines] [--words] [--chars]
 */

#include "shared/ntool_common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>

/* ---- Internal: count lines/words/chars in a stream ---- */

static void ntooli_wc_stream(FILE *fp, long *out_lines,
                             long *out_words, long *out_chars) {
    long lines = 0, words = 0, chars = 0;
    int in_word = 0;
    int c;

    while ((c = fgetc(fp)) != EOF) {
        chars++;
        if (c == '\n') lines++;
        if (isspace((unsigned char)c)) {
            in_word = 0;
        } else {
            if (!in_word) {
                words++;
                in_word = 1;
            }
        }
    }

    *out_lines = lines;
    *out_words = words;
    *out_chars = chars;
}

/* ---- Core entry point ---- */

int ntool_wc(const NToolFlags *flags) {
    if (flags == NULL) return -1;

    int specific = (flags->count_lines || flags->count_words ||
                    flags->count_chars);

    if (flags->subject_count == 0) {
        if (ntool_stdin_has_data()) {
            long lines = 0, words = 0, chars = 0;
            ntooli_wc_stream(stdin, &lines, &words, &chars);

            if (!specific) {
                printf("%7ld %7ld %7ld\n", lines, words, chars);
            } else {
                if (flags->count_lines) printf("%7ld", lines);
                if (flags->count_words) printf("%7ld", words);
                if (flags->count_chars) printf("%7ld", chars);
                printf("\n");
            }
        } else {
            fprintf(stderr, "nwc: no files specified\n");
            return -1;
        }
        return 0;
    }

    long total_lines = 0, total_words = 0, total_chars = 0;
    int multi = (flags->subject_count > 1);

    for (int fi = 0; fi < flags->subject_count; fi++) {
        const char *path = flags->subjects[fi];
        FILE *fp = fopen(path, "r");
        if (fp == NULL) {
            fprintf(stderr, "nwc: cannot open '%s': %s\n",
                    path, strerror(errno));
            continue;
        }

        long lines = 0, words = 0, chars = 0;
        ntooli_wc_stream(fp, &lines, &words, &chars);
        fclose(fp);

        total_lines += lines;
        total_words += words;
        total_chars += chars;

        if (!specific) {
            printf("%7ld %7ld %7ld %s\n", lines, words, chars, path);
        } else {
            if (flags->count_lines) printf("%7ld", lines);
            if (flags->count_words) printf("%7ld", words);
            if (flags->count_chars) printf("%7ld", chars);
            printf(" %s\n", path);
        }
    }

    if (multi) {
        if (!specific) {
            printf("%7ld %7ld %7ld total\n",
                   total_lines, total_words, total_chars);
        } else {
            if (flags->count_lines) printf("%7ld", total_lines);
            if (flags->count_words) printf("%7ld", total_words);
            if (flags->count_chars) printf("%7ld", total_chars);
            printf(" total\n");
        }
    }

    return 0;
}

/* ---- Usage ---- */

static void ntooli_usage(void) {
    fprintf(stderr, "Usage: nwc [file...] [--lines] [--words] [--chars]\n");
    fprintf(stderr, "\nCount lines, words, and characters.\n\n");
    fprintf(stderr, "Flags:\n");
    fprintf(stderr, "  --lines   Count only lines\n");
    fprintf(stderr, "  --words   Count only words\n");
    fprintf(stderr, "  --chars   Count only characters\n");
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

    return ntool_wc(&flags) != 0 ? 1 : 0;
}
