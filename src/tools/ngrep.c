/**
 * @file    ngrep.c
 * @brief   ngrep — Search text in files
 * @author  Anthony Taliento
 * @date    2026-03-16
 * @copyright (c) 2026 Zorya Corporation. MIT License.
 *
 * Standalone tool binary. Links against libnova_toolutil.a only.
 * Usage: ngrep [file|dir...] -m=pattern [-r] [-n] [-i] [-v] [-l=N]
 */

#include "shared/ntool_common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

/* ---- Internal: grep a single file ---- */

static int ntooli_grep_file(const char *path, const char *show_name,
                            const NToolFlags *flags, FILE *out,
                            int *total_found) {
    FILE *fp = fopen(path, "r");
    if (fp == NULL) {
        fprintf(stderr, "ngrep: cannot open '%s': %s\n",
                path, strerror(errno));
        return -1;
    }

    const char *pattern = flags->match;
    if (pattern == NULL) {
        fclose(fp);
        return 0;
    }

    char line[NTOOL_LINE_MAX];
    int line_num = 0;
    int file_matches = 0;

    while (fgets(line, (int)sizeof(line), fp) != NULL) {
        line_num++;

        if (flags->limit > 0 && *total_found >= flags->limit) break;

        int found;
        if (flags->ignore_case) {
            found = (ntool_strcasestr(line, pattern) != NULL);
        } else {
            found = (strstr(line, pattern) != NULL);
        }

        if (flags->invert) found = !found;

        if (found) {
            size_t llen = strlen(line);
            if (llen > 0 && line[llen - 1] == '\n') {
                line[llen - 1] = '\0';
            }

            if (show_name != NULL) {
                if (flags->show_numbers) {
                    fprintf(out, "%s:%d:%s\n", show_name, line_num, line);
                } else {
                    fprintf(out, "%s:%s\n", show_name, line);
                }
            } else {
                if (flags->show_numbers) {
                    fprintf(out, "%d:%s\n", line_num, line);
                } else {
                    fprintf(out, "%s\n", line);
                }
            }

            file_matches++;
            (*total_found)++;
        }
    }

    fclose(fp);
    return file_matches;
}

/* ---- Internal: recursive directory grep ---- */

static void ntooli_grep_dir(const char *dir, const NToolFlags *flags,
                            FILE *out, int *total_found) {
    NToolDirEntry *entries = (NToolDirEntry *)malloc(
        (size_t)NTOOL_DIR_MAX * sizeof(NToolDirEntry));
    if (entries == NULL) return;

    int count = ntool_read_dir(dir, entries, NTOOL_DIR_MAX,
                               flags->show_all);

    for (int i = 0; i < count; i++) {
        if (flags->limit > 0 && *total_found >= flags->limit) break;

        char fullpath[NTOOL_PATH_MAX];
        ntool_join_path(fullpath, sizeof(fullpath), dir, entries[i].name);

        if (entries[i].is_dir) {
            ntooli_grep_dir(fullpath, flags, out, total_found);
        } else {
            ntooli_grep_file(fullpath, fullpath, flags, out, total_found);
        }
    }

    free(entries);
}

/* ---- Core entry point ---- */

int ntool_grep(const NToolFlags *flags) {
    if (flags == NULL) return -1;

    if (flags->match == NULL) {
        fprintf(stderr, "ngrep: --match/-m pattern required\n");
        fprintf(stderr, "Usage: ngrep [file...] -m=PATTERN\n");
        return -1;
    }

    FILE *out = stdout;
    if (flags->output != NULL) {
        out = fopen(flags->output,
                    flags->append_mode ? "a" : "w");
        if (out == NULL) {
            fprintf(stderr, "ngrep: cannot open output '%s': %s\n",
                    flags->output, strerror(errno));
            return -1;
        }
    }

    int total_found = 0;
    int result = 0;

    if (flags->subject_count == 0) {
        if (ntool_stdin_has_data()) {
            char line[NTOOL_LINE_MAX];
            int line_num = 0;

            while (fgets(line, (int)sizeof(line), stdin) != NULL) {
                line_num++;
                if (flags->limit > 0 && total_found >= flags->limit) break;

                int fnd;
                if (flags->ignore_case) {
                    fnd = (ntool_strcasestr(line, flags->match) != NULL);
                } else {
                    fnd = (strstr(line, flags->match) != NULL);
                }
                if (flags->invert) fnd = !fnd;

                if (fnd) {
                    size_t llen = strlen(line);
                    if (llen > 0 && line[llen - 1] == '\n') {
                        line[llen - 1] = '\0';
                    }
                    if (flags->show_numbers) {
                        fprintf(out, "%d:%s\n", line_num, line);
                    } else {
                        fprintf(out, "%s\n", line);
                    }
                    total_found++;
                }
            }
        } else {
            fprintf(stderr, "ngrep: no files specified\n");
            result = -1;
        }
    } else {
        int multi_file = (flags->subject_count > 1 || flags->recursive);

        for (int i = 0; i < flags->subject_count; i++) {
            const char *subj = flags->subjects[i];

            if (ntool_is_directory(subj)) {
                if (flags->recursive) {
                    ntooli_grep_dir(subj, flags, out, &total_found);
                } else {
                    fprintf(stderr,
                            "ngrep: '%s' is a directory "
                            "(use -r/--recursive)\n", subj);
                }
            } else {
                const char *display = multi_file ? subj : NULL;
                ntooli_grep_file(subj, display, flags, out, &total_found);
            }
        }
    }

    if (flags->verbose) {
        fprintf(stderr, "%d match(es) found\n", total_found);
    }

    if (flags->output != NULL && out != stdout) {
        fclose(out);
    }
    return result;
}

/* ---- Usage ---- */

static void ntooli_usage(void) {
    fprintf(stderr, "Usage: ngrep [file|dir...] -m=pattern [-r] [-n] [-i] [-v] [-l=N]\n");
    fprintf(stderr, "\nSearch files for lines matching a text pattern.\n\n");
    fprintf(stderr, "Flags:\n");
    fprintf(stderr, "  -m, --match=PAT    Search pattern (required)\n");
    fprintf(stderr, "  -i, --ignore-case  Case-insensitive matching\n");
    fprintf(stderr, "  -v, --invert       Invert match (show non-matching)\n");
    fprintf(stderr, "  -n, --number       Show line numbers\n");
    fprintf(stderr, "  -r, --recursive    Recurse into directories\n");
    fprintf(stderr, "  -a, --all          Include hidden files\n");
    fprintf(stderr, "  -l, --limit=N      Maximum matches\n");
    fprintf(stderr, "  -o, --output=FILE  Write results to file\n");
    fprintf(stderr, "  -V, --verbose      Print match count to stderr\n");
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

    return ntool_grep(&flags) != 0 ? 1 : 0;
}
