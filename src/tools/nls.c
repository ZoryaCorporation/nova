/**
 * @file    nls.c
 * @brief   nls — List directory contents
 * @author  Anthony Taliento
 * @date    2026-03-16
 * @copyright (c) 2026 Zorya Corporation. MIT License.
 *
 * Standalone tool binary. Links against libnova_toolutil.a only.
 * Usage: nls [dir] [-a] [-L] [-m=pattern] [-l=N]
 */

#include "shared/ntool_common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- Core entry point ---- */

int ntool_ls(const NToolFlags *flags) {
    if (flags == NULL) return -1;

    const char *dir = ".";
    if (flags->subject_count > 0) {
        dir = flags->subjects[0];
    }

    if (!ntool_is_directory(dir)) {
        fprintf(stderr, "nls: '%s' is not a directory\n", dir);
        return -1;
    }

    NToolDirEntry *entries = (NToolDirEntry *)malloc(
        (size_t)NTOOL_DIR_MAX * sizeof(NToolDirEntry));
    if (entries == NULL) {
        fprintf(stderr, "nls: out of memory\n");
        return -1;
    }

    int count = ntool_read_dir(dir, entries, NTOOL_DIR_MAX,
                               flags->show_all);
    qsort(entries, (size_t)count, sizeof(NToolDirEntry), ntool_entry_cmp);

    FILE *out = stdout;
    int limit = flags->limit > 0 ? flags->limit : count;
    int shown = 0;

    for (int i = 0; i < count && shown < limit; i++) {
        /* Apply match filter if present */
        if (flags->match != NULL) {
            int matches = flags->ignore_case
                ? ntool_glob_match_ci(flags->match, entries[i].name)
                : ntool_glob_match(flags->match, entries[i].name);
            if (flags->invert) matches = !matches;
            if (!matches) continue;
        }

        if (flags->show_long) {
            char fullpath[NTOOL_PATH_MAX];
            ntool_join_path(fullpath, sizeof(fullpath),
                            dir, entries[i].name);

            char sizebuf[32];
            char timebuf[32];

            if (entries[i].is_dir) {
                snprintf(sizebuf, sizeof(sizebuf), "-");
            } else {
                ntool_format_size(ntool_file_size(fullpath),
                                  sizebuf, sizeof(sizebuf));
            }
            ntool_format_time(ntool_file_mtime(fullpath),
                              timebuf, sizeof(timebuf));

            fprintf(out, "%s  %8s  %s%s\n",
                    timebuf, sizebuf,
                    entries[i].name,
                    entries[i].is_dir ? "/" : "");
        } else {
            fprintf(out, "%s%s\n",
                    entries[i].name,
                    entries[i].is_dir ? "/" : "");
        }
        shown++;
    }

    free(entries);
    return 0;
}

/* ---- Usage ---- */

static void ntooli_usage(void) {
    fprintf(stderr, "Usage: nls [dir] [-a] [-L] [-m=pattern] [-l=N]\n");
    fprintf(stderr, "\nList directory contents.\n\n");
    fprintf(stderr, "Flags:\n");
    fprintf(stderr, "  -a, --all          Show hidden files\n");
    fprintf(stderr, "  -L, --long         Long listing format\n");
    fprintf(stderr, "  -m, --match=PAT    Filter by glob pattern\n");
    fprintf(stderr, "  -i, --ignore-case  Case-insensitive match\n");
    fprintf(stderr, "  -v, --invert       Invert match\n");
    fprintf(stderr, "  -l, --limit=N      Show at most N entries\n");
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

    return ntool_ls(&flags) != 0 ? 1 : 0;
}
