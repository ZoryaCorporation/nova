/**
 * @file    ntree.c
 * @brief   ntree — Directory tree visualization
 * @author  Anthony Taliento
 * @date    2026-03-16
 * @copyright (c) 2026 Zorya Corporation. MIT License.
 *
 * Standalone tool binary. Links against libnova_toolutil.a only.
 * Usage: ntree [dir] [-a] [-d=N]
 */

#include "shared/ntool_common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- Internal recursive helper ---- */

static void ntooli_tree_recurse(const char *path, const char *prefix,
                                int depth, const NToolFlags *flags,
                                int *dirs, int *files) {
    if (flags->depth >= 0 && depth >= flags->depth) return;

    NToolDirEntry *entries = (NToolDirEntry *)malloc(
        (size_t)NTOOL_DIR_MAX * sizeof(NToolDirEntry));
    if (entries == NULL) return;

    int count = ntool_read_dir(path, entries, NTOOL_DIR_MAX,
                               flags->show_all);
    qsort(entries, (size_t)count, sizeof(NToolDirEntry), ntool_entry_cmp);

    for (int i = 0; i < count; i++) {
        int is_last = (i == count - 1);

        printf("%s%s%s%s\n",
               prefix,
               is_last ? NTOOL_TREE_CORNER : NTOOL_TREE_BRANCH,
               entries[i].name,
               entries[i].is_dir ? "/" : "");

        if (entries[i].is_dir) {
            (*dirs)++;

            char new_prefix[NTOOL_PATH_MAX];
            snprintf(new_prefix, sizeof(new_prefix), "%s%s",
                     prefix, is_last ? NTOOL_TREE_SPACE : NTOOL_TREE_PIPE);

            char child_path[NTOOL_PATH_MAX];
            ntool_join_path(child_path, sizeof(child_path),
                            path, entries[i].name);

            ntooli_tree_recurse(child_path, new_prefix,
                                depth + 1, flags, dirs, files);
        } else {
            (*files)++;
        }
    }

    free(entries);
}

/* ---- Core entry point ---- */

int ntool_tree(const NToolFlags *flags) {
    if (flags == NULL) return -1;

    const char *dir = ".";
    if (flags->subject_count > 0) {
        dir = flags->subjects[0];
    }

    if (!ntool_is_directory(dir)) {
        fprintf(stderr, "ntree: '%s' is not a directory\n", dir);
        return -1;
    }

    printf("%s\n", dir);

    int dirs = 0, files = 0;
    ntooli_tree_recurse(dir, "", 0, flags, &dirs, &files);

    printf("\n%d directories, %d files\n", dirs, files);
    return 0;
}

/* ---- Usage ---- */

static void ntooli_usage(void) {
    fprintf(stderr, "Usage: ntree [dir] [-a] [-d=N]\n");
    fprintf(stderr, "\nDirectory tree visualization.\n\n");
    fprintf(stderr, "Flags:\n");
    fprintf(stderr, "  -a, --all      Show hidden files\n");
    fprintf(stderr, "  -d, --depth=N  Maximum depth\n");
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

    return ntool_tree(&flags) != 0 ? 1 : 0;
}
