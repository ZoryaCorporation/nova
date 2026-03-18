/**
 * @file    nfind.c
 * @brief   nfind — Find files by name pattern
 * @author  Anthony Taliento
 * @date    2026-03-16
 * @copyright (c) 2026 Zorya Corporation. MIT License.
 *
 * Standalone tool binary. Links against libnova_toolutil.a only.
 * Usage: nfind [dir] -m=pattern [-a] [-d=N] [-l=N] [-V]
 */

#include "shared/ntool_common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

/* ---- Internal recursive helper ---- */

static void ntooli_find_recurse(const char *base, const char *prefix,
                                int depth, const NToolFlags *flags,
                                int *found, FILE *out) {
    if (flags->depth >= 0 && depth > flags->depth) return;
    if (flags->limit > 0 && *found >= flags->limit) return;

    NToolDirEntry *entries = (NToolDirEntry *)malloc(
        (size_t)NTOOL_DIR_MAX * sizeof(NToolDirEntry));
    if (entries == NULL) return;

    int count = ntool_read_dir(base, entries, NTOOL_DIR_MAX,
                               flags->show_all);

    for (int i = 0; i < count; i++) {
        if (flags->limit > 0 && *found >= flags->limit) break;

        char relpath[NTOOL_PATH_MAX];
        if (prefix[0] != '\0') {
            snprintf(relpath, sizeof(relpath), "%s/%s",
                     prefix, entries[i].name);
        } else {
            snprintf(relpath, sizeof(relpath), "%s", entries[i].name);
        }

        /* Check if name matches pattern */
        if (flags->match != NULL) {
            int matches = flags->ignore_case
                ? ntool_glob_match_ci(flags->match, entries[i].name)
                : ntool_glob_match(flags->match, entries[i].name);
            if (flags->invert) matches = !matches;
            if (matches) {
                fprintf(out, "%s\n", relpath);
                (*found)++;
            }
        } else {
            fprintf(out, "%s\n", relpath);
            (*found)++;
        }

        /* Recurse into subdirectories */
        if (entries[i].is_dir) {
            char child[NTOOL_PATH_MAX];
            ntool_join_path(child, sizeof(child), base, entries[i].name);
            ntooli_find_recurse(child, relpath, depth + 1,
                                flags, found, out);
        }
    }

    free(entries);
}

/* ---- Core entry point ---- */

int ntool_find(const NToolFlags *flags) {
    if (flags == NULL) return -1;

    const char *dir = ".";
    if (flags->subject_count > 0) {
        dir = flags->subjects[0];
    }

    if (!ntool_is_directory(dir)) {
        fprintf(stderr, "nfind: '%s' is not a directory\n", dir);
        return -1;
    }

    FILE *out = stdout;
    if (flags->output != NULL) {
        out = fopen(flags->output,
                    flags->append_mode ? "a" : "w");
        if (out == NULL) {
            fprintf(stderr, "nfind: cannot open output '%s': %s\n",
                    flags->output, strerror(errno));
            return -1;
        }
    }

    int found = 0;
    ntooli_find_recurse(dir, "", 0, flags, &found, out);

    if (flags->verbose) {
        fprintf(stderr, "%d file(s) found\n", found);
    }

    if (flags->output != NULL && out != stdout) {
        fclose(out);
    }
    return 0;
}

/* ---- Usage ---- */

static void ntooli_usage(void) {
    fprintf(stderr, "Usage: nfind [dir] -m=pattern [-a] [-d=N] [-l=N] [-V]\n");
    fprintf(stderr, "\nFind files by name pattern.\n\n");
    fprintf(stderr, "Flags:\n");
    fprintf(stderr, "  -m, --match=PAT    Glob pattern to match\n");
    fprintf(stderr, "  -i, --ignore-case  Case-insensitive match\n");
    fprintf(stderr, "  -v, --invert       Invert match\n");
    fprintf(stderr, "  -a, --all          Include hidden files\n");
    fprintf(stderr, "  -d, --depth=N      Maximum search depth\n");
    fprintf(stderr, "  -l, --limit=N      Maximum results\n");
    fprintf(stderr, "  -V, --verbose      Print count to stderr\n");
    fprintf(stderr, "  -o, --output=FILE  Write results to file\n");
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

    return ntool_find(&flags) != 0 ? 1 : 0;
}
