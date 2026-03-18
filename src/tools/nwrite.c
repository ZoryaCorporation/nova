/**
 * @file    nwrite.c
 * @brief   nwrite — Write stdin to file (pipe endpoint)
 * @author  Anthony Taliento
 * @date    2026-03-16
 * @copyright (c) 2026 Zorya Corporation. MIT License.
 *
 * Standalone tool binary. Links against libnova_toolutil.a only.
 * Usage: nwrite <file> [--append] [text...]
 */

#include "shared/ntool_common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

/* ---- Core entry point ---- */

int ntool_write(const NToolFlags *flags) {
    if (flags == NULL) return -1;

    if (flags->subject_count == 0) {
        fprintf(stderr, "nwrite: output file required\n");
        fprintf(stderr, "Usage: nwrite <file> [--append]\n");
        return -1;
    }

    const char *path = flags->subjects[0];
    const char *mode = flags->append_mode ? "a" : "w";

    FILE *out = fopen(path, mode);
    if (out == NULL) {
        fprintf(stderr, "nwrite: cannot open '%s': %s\n",
                path, strerror(errno));
        return -1;
    }

    if (ntool_stdin_has_data()) {
        char buf[NTOOL_LINE_MAX];
        while (fgets(buf, (int)sizeof(buf), stdin) != NULL) {
            fputs(buf, out);
        }
    } else if (flags->subject_count > 1) {
        for (int i = 1; i < flags->subject_count; i++) {
            fputs(flags->subjects[i], out);
            fputc('\n', out);
        }
    } else {
        fprintf(stderr, "nwrite: no input (pipe data or provide text)\n");
        fclose(out);
        return -1;
    }

    fclose(out);

    if (flags->verbose) {
        fprintf(stderr, "Wrote to '%s'\n", path);
    }

    return 0;
}

/* ---- Usage ---- */

static void ntooli_usage(void) {
    fprintf(stderr, "Usage: nwrite <file> [--append] [text...]\n");
    fprintf(stderr, "\nWrite stdin or text to a file.\n\n");
    fprintf(stderr, "Flags:\n");
    fprintf(stderr, "  -A, --append   Append to file instead of overwriting\n");
    fprintf(stderr, "  -V, --verbose  Print confirmation to stderr\n");
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

    return ntool_write(&flags) != 0 ? 1 : 0;
}
