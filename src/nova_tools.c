/**
 * @file nova_tools.c
 * @brief Nova Language - CLI Tools Implementation
 *
 * Cross-platform C implementations of common CLI tools.
 * Built on portable C99 primitives — no POSIX-only headers
 * like glob.h, fnmatch.h, or regex.h.
 *
 * Platform-specific code is limited to directory iteration
 * and file metadata, guarded by #ifdef _WIN32.
 *
 * Tools: cat, ls, tree, find, grep, head, tail, wc, write
 *
 * @author Anthony Taliento
 * @date 2026-02-17
 * @version 0.2.0
 *
 * @copyright Copyright (c) 2026 Zorya Corporation
 * @license MIT
 *
 * ZORYA-C COMPLIANCE: v2.0.0
 */

/* Feature test macros — must precede all system headers */
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include "nova/nova_tools.h"
#include "nova/nova_task.h"
#include "nova/nova_conf.h"
#include "nova/nova_error.h"

#include "tools/shared/ntool_common.h"

#include <zorya/pal.h>     /* zorya_setenv/unsetenv — portable env manipulation */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <time.h>

/* ============================================================
 * CROSS-PLATFORM HEADERS
 *
 * The shared tool utility library (ntool_common) handles most
 * platform abstraction via the Zorya PAL. These remaining
 * platform macros are used by the shell pipeline engine and
 * task runner which stay in the core binary.
 * ============================================================ */

#ifdef _WIN32
    #define WIN32_LEAN_AND_MEAN
    #include <windows.h>
    #include <direct.h>
    #include <io.h>
    #include <sys/stat.h>
    #include <sys/types.h>
    #define novai_getcwd(b,n)   _getcwd((b),(int)(n))
#else
    #include <unistd.h>
    #include <sys/stat.h>
    #include <sys/types.h>
    #include <dirent.h>
    #define novai_getcwd(b,n)   getcwd((b),(n))
#endif

/* ============================================================
 * COMPATIBILITY ALIASES
 *
 * Map legacy NOVAI_ constants to the shared ntool_ library.
 * Map pipeline engine I/O to Zorya PAL.
 * ============================================================ */

#define NOVAI_PATH_MAX   NTOOL_PATH_MAX
#define NOVAI_LINE_MAX   NTOOL_LINE_MAX
#define NOVAI_DIR_MAX    NTOOL_DIR_MAX

/** Tree drawing characters — forwarded from ntool_common.h */
#define TREE_BRANCH  NTOOL_TREE_BRANCH
#define TREE_CORNER  NTOOL_TREE_CORNER
#define TREE_PIPE    NTOOL_TREE_PIPE
#define TREE_SPACE   NTOOL_TREE_SPACE

/** NovaiDirEntry → NToolDirEntry (= ZoryaDirEntry) */
typedef NToolDirEntry NovaiDirEntry;

/* ============================================================
 * SHARED HELPER ALIASES
 *
 * All platform helpers now live in ntool_common (libnova_toolutil.a).
 * These inline wrappers keep existing call sites unchanged while
 * delegating to the shared library.
 * ============================================================ */

static inline int novai_is_directory(const char *p)     { return ntool_is_directory(p);  }
static inline long novai_file_size(const char *p)       { return ntool_file_size(p);     }
static inline long novai_file_mtime(const char *p)      { return ntool_file_mtime(p);    }
static inline int novai_glob_match(const char *pat, const char *s)
    { return ntool_glob_match(pat, s); }
static inline int novai_glob_match_ci(const char *pat, const char *s)
    { return ntool_glob_match_ci(pat, s); }
static inline const char *novai_strcasestr(const char *h, const char *n)
    { return ntool_strcasestr(h, n); }
static inline int novai_stdin_has_data(void)
    { return ntool_stdin_has_data(); }

static inline void novai_join_path(char *buf, size_t sz,
                                   const char *base, const char *name)
    { ntool_join_path(buf, sz, base, name); }
static inline void novai_format_size(long size, char *buf, size_t sz)
    { ntool_format_size(size, buf, sz); }
static inline void novai_format_time(long mtime, char *buf, size_t sz)
    { ntool_format_time(mtime, buf, sz); }

static inline int novai_read_dir(const char *path, NovaiDirEntry *entries,
                                 int max_entries, int show_hidden)
    { return ntool_read_dir(path, entries, max_entries, show_hidden); }
static inline int novai_entry_cmp(const void *a, const void *b)
    { return ntool_entry_cmp(a, b); }

/* ============================================================
 * FLAG PARSER
 * ============================================================ */

int nova_tool_parse_flags(int argc, char **argv, NovaToolFlags *flags) {
    if (flags == NULL) return -1;
    memset(flags, 0, sizeof(*flags));
    flags->depth = -1;  /* Unlimited by default */

    for (int i = 0; i < argc; i++) {
        const char *arg = argv[i];
        if (arg == NULL) continue;

        if (strncmp(arg, "--", 2) == 0) {
            /* ---- Long flag ---- */
            const char *name = arg + 2;
            const char *eq = strchr(name, '=');
            char flag_name[64] = {0};
            const char *value = NULL;

            if (eq != NULL) {
                size_t len = (size_t)(eq - name);
                if (len >= sizeof(flag_name)) len = sizeof(flag_name) - 1;
                memcpy(flag_name, name, len);
                flag_name[len] = '\0';
                value = eq + 1;
            } else {
                snprintf(flag_name, sizeof(flag_name), "%s", name);
            }

            /* Universal flags */
            if (strcmp(flag_name, "match") == 0) {
                if (value == NULL && i + 1 < argc) value = argv[++i];
                flags->match = value;
            } else if (strcmp(flag_name, "ignore-case") == 0) {
                flags->ignore_case = 1;
            } else if (strcmp(flag_name, "invert") == 0) {
                flags->invert = 1;
            } else if (strcmp(flag_name, "recursive") == 0) {
                flags->recursive = 1;
            } else if (strcmp(flag_name, "output") == 0) {
                if (value == NULL && i + 1 < argc) value = argv[++i];
                flags->output = value;
            } else if (strcmp(flag_name, "limit") == 0) {
                if (value == NULL && i + 1 < argc) value = argv[++i];
                if (value != NULL) flags->limit = atoi(value);
            } else if (strcmp(flag_name, "depth") == 0) {
                if (value == NULL && i + 1 < argc) value = argv[++i];
                if (value != NULL) flags->depth = atoi(value);
            } else if (strcmp(flag_name, "verbose") == 0) {
                flags->verbose = 1;
            }
            /* Tool-specific long flags */
            else if (strcmp(flag_name, "all") == 0) {
                flags->show_all = 1;
            } else if (strcmp(flag_name, "number") == 0) {
                flags->show_numbers = 1;
            } else if (strcmp(flag_name, "long") == 0) {
                flags->show_long = 1;
            } else if (strcmp(flag_name, "append") == 0) {
                flags->append_mode = 1;
            } else if (strcmp(flag_name, "lines") == 0) {
                flags->count_lines = 1;
            } else if (strcmp(flag_name, "words") == 0) {
                flags->count_words = 1;
            } else if (strcmp(flag_name, "chars") == 0) {
                flags->count_chars = 1;
            } else if (strcmp(flag_name, "dry") == 0) {
                flags->dry_run = 1;
            } else {
                fprintf(stderr, "nova: unknown flag: %s\n", arg);
                return -1;
            }

        } else if (arg[0] == '-' && arg[1] != '\0') {
            /* ---- Short flag ---- */
            const char *eq = strchr(arg + 1, '=');
            char flag_str[16] = {0};
            const char *value = NULL;

            if (eq != NULL) {
                size_t len = (size_t)(eq - (arg + 1));
                if (len >= sizeof(flag_str)) len = sizeof(flag_str) - 1;
                memcpy(flag_str, arg + 1, len);
                flag_str[len] = '\0';
                value = eq + 1;
            } else {
                snprintf(flag_str, sizeof(flag_str), "%s", arg + 1);
            }

            /* Universal short flags */
            if (strcmp(flag_str, "m") == 0) {
                if (value == NULL && i + 1 < argc) value = argv[++i];
                flags->match = value;
            } else if (strcmp(flag_str, "i") == 0) {
                flags->ignore_case = 1;
            } else if (strcmp(flag_str, "v") == 0) {
                flags->invert = 1;
            } else if (strcmp(flag_str, "r") == 0) {
                flags->recursive = 1;
            } else if (strcmp(flag_str, "o") == 0) {
                if (value == NULL && i + 1 < argc) value = argv[++i];
                flags->output = value;
            } else if (strcmp(flag_str, "l") == 0) {
                if (value == NULL && i + 1 < argc) value = argv[++i];
                if (value != NULL) flags->limit = atoi(value);
            } else if (strcmp(flag_str, "d") == 0) {
                if (value == NULL && i + 1 < argc) value = argv[++i];
                if (value != NULL) flags->depth = atoi(value);
            } else if (strcmp(flag_str, "V") == 0) {
                flags->verbose = 1;
            }
            /* Tool-specific short flags */
            else if (strcmp(flag_str, "a") == 0) {
                flags->show_all = 1;
            } else if (strcmp(flag_str, "n") == 0) {
                flags->show_numbers = 1;
            } else if (strcmp(flag_str, "L") == 0) {
                flags->show_long = 1;
            } else if (strcmp(flag_str, "A") == 0) {
                flags->append_mode = 1;
            } else {
                fprintf(stderr, "nova: unknown flag: %s\n", arg);
                return -1;
            }

        } else {
            /* ---- Positional argument ---- */
            if (flags->subject_count < NOVA_TOOL_MAX_SUBJECTS) {
                flags->subjects[flags->subject_count++] = arg;
            }
        }
    }

    return 0;
}

/* ============================================================
 * TOOL: cat — Concatenate and print files
 * ============================================================ */

/**
 * @brief Print a single file to the given output stream.
 */
static int novai_cat_file(const char *path, FILE *out,
                          int show_numbers, int *line_num) {
    FILE *fp = fopen(path, "r");
    if (fp == NULL) {
        fprintf(stderr, "nova cat: cannot open '%s': %s\n",
                path, strerror(errno));
        return -1;
    }

    char line[NOVAI_LINE_MAX];
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

int nova_tool_cat(const NovaToolFlags *flags) {
    if (flags == NULL) return -1;

    FILE *out = stdout;
    if (flags->output != NULL) {
        out = fopen(flags->output,
                    flags->append_mode ? "a" : "w");
        if (out == NULL) {
            fprintf(stderr, "nova cat: cannot open output '%s': %s\n",
                    flags->output, strerror(errno));
            return -1;
        }
    }

    int result = 0;
    int line_num = 1;

    if (flags->subject_count == 0) {
        /* Read from stdin */
        if (novai_stdin_has_data()) {
            char line[NOVAI_LINE_MAX];
            while (fgets(line, (int)sizeof(line), stdin) != NULL) {
                if (flags->show_numbers) {
                    fprintf(out, "%6d  %s", line_num, line);
                } else {
                    fputs(line, out);
                }
                line_num++;
            }
        } else {
            fprintf(stderr, "nova cat: no files specified\n");
            result = -1;
        }
    } else {
        for (int i = 0; i < flags->subject_count; i++) {
            if (novai_cat_file(flags->subjects[i], out,
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

/* ============================================================
 * TOOL: ls — List directory contents
 * ============================================================ */

int nova_tool_ls(const NovaToolFlags *flags) {
    if (flags == NULL) return -1;

    const char *dir = ".";
    if (flags->subject_count > 0) {
        dir = flags->subjects[0];
    }

    if (!novai_is_directory(dir)) {
        fprintf(stderr, "nova ls: '%s' is not a directory\n", dir);
        return -1;
    }

    NovaiDirEntry *entries = (NovaiDirEntry *)malloc(
        (size_t)NOVAI_DIR_MAX * sizeof(NovaiDirEntry));
    if (entries == NULL) {
        fprintf(stderr, "nova ls: out of memory\n");
        return -1;
    }

    int count = novai_read_dir(dir, entries, NOVAI_DIR_MAX,
                               flags->show_all);
    qsort(entries, (size_t)count, sizeof(NovaiDirEntry), novai_entry_cmp);

    FILE *out = stdout;
    int limit = flags->limit > 0 ? flags->limit : count;
    int shown = 0;

    for (int i = 0; i < count && shown < limit; i++) {
        /* Apply match filter if present */
        if (flags->match != NULL) {
            int matches = flags->ignore_case
                ? novai_glob_match_ci(flags->match, entries[i].name)
                : novai_glob_match(flags->match, entries[i].name);
            if (flags->invert) matches = !matches;
            if (!matches) continue;
        }

        if (flags->show_long) {
            char fullpath[NOVAI_PATH_MAX];
            novai_join_path(fullpath, sizeof(fullpath),
                            dir, entries[i].name);

            char sizebuf[32];
            char timebuf[32];

            if (entries[i].is_dir) {
                snprintf(sizebuf, sizeof(sizebuf), "-");
            } else {
                novai_format_size(novai_file_size(fullpath),
                                  sizebuf, sizeof(sizebuf));
            }
            novai_format_time(novai_file_mtime(fullpath),
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

/* ============================================================
 * TOOL: tree — Directory tree visualization
 * ============================================================ */

/**
 * @brief Internal recursive tree printer.
 *
 * @param path     Current directory to list
 * @param prefix   Prefix string for nested indentation
 * @param depth    Current depth (0 = root)
 * @param flags    Tool flags
 * @param dirs     Counter for directories seen
 * @param files    Counter for files seen
 */
static void novai_tree_recurse(const char *path, const char *prefix,
                                int depth, const NovaToolFlags *flags,
                                int *dirs, int *files) {
    if (flags->depth >= 0 && depth >= flags->depth) return;

    NovaiDirEntry *entries = (NovaiDirEntry *)malloc(
        (size_t)NOVAI_DIR_MAX * sizeof(NovaiDirEntry));
    if (entries == NULL) return;

    int count = novai_read_dir(path, entries, NOVAI_DIR_MAX,
                               flags->show_all);
    qsort(entries, (size_t)count, sizeof(NovaiDirEntry), novai_entry_cmp);

    for (int i = 0; i < count; i++) {
        int is_last = (i == count - 1);

        /* Print this entry */
        printf("%s%s%s%s\n",
               prefix,
               is_last ? TREE_CORNER : TREE_BRANCH,
               entries[i].name,
               entries[i].is_dir ? "/" : "");

        if (entries[i].is_dir) {
            (*dirs)++;

            /* Build new prefix for children */
            char new_prefix[NOVAI_PATH_MAX];
            snprintf(new_prefix, sizeof(new_prefix), "%s%s",
                     prefix, is_last ? TREE_SPACE : TREE_PIPE);

            /* Build child path */
            char child_path[NOVAI_PATH_MAX];
            novai_join_path(child_path, sizeof(child_path),
                            path, entries[i].name);

            novai_tree_recurse(child_path, new_prefix,
                                depth + 1, flags, dirs, files);
        } else {
            (*files)++;
        }
    }

    free(entries);
}

int nova_tool_tree(const NovaToolFlags *flags) {
    if (flags == NULL) return -1;

    const char *dir = ".";
    if (flags->subject_count > 0) {
        dir = flags->subjects[0];
    }

    if (!novai_is_directory(dir)) {
        fprintf(stderr, "nova tree: '%s' is not a directory\n", dir);
        return -1;
    }

    printf("%s\n", dir);

    int dirs = 0, files = 0;
    novai_tree_recurse(dir, "", 0, flags, &dirs, &files);

    printf("\n%d directories, %d files\n", dirs, files);
    return 0;
}

/* ============================================================
 * TOOL: find — Find files by name pattern
 * ============================================================ */

/**
 * @brief Internal recursive find.
 */
static void novai_find_recurse(const char *base, const char *prefix,
                                int depth, const NovaToolFlags *flags,
                                int *found, FILE *out) {
    if (flags->depth >= 0 && depth > flags->depth) return;
    if (flags->limit > 0 && *found >= flags->limit) return;

    NovaiDirEntry *entries = (NovaiDirEntry *)malloc(
        (size_t)NOVAI_DIR_MAX * sizeof(NovaiDirEntry));
    if (entries == NULL) return;

    int count = novai_read_dir(base, entries, NOVAI_DIR_MAX,
                               flags->show_all);

    for (int i = 0; i < count; i++) {
        if (flags->limit > 0 && *found >= flags->limit) break;

        char relpath[NOVAI_PATH_MAX];
        if (prefix[0] != '\0') {
            snprintf(relpath, sizeof(relpath), "%s/%s",
                     prefix, entries[i].name);
        } else {
            snprintf(relpath, sizeof(relpath), "%s", entries[i].name);
        }

        /* Check if name matches pattern */
        if (flags->match != NULL) {
            int matches = flags->ignore_case
                ? novai_glob_match_ci(flags->match, entries[i].name)
                : novai_glob_match(flags->match, entries[i].name);
            if (flags->invert) matches = !matches;
            if (matches) {
                fprintf(out, "%s\n", relpath);
                (*found)++;
            }
        } else {
            /* No pattern: list everything */
            fprintf(out, "%s\n", relpath);
            (*found)++;
        }

        /* Recurse into subdirectories */
        if (entries[i].is_dir) {
            char child[NOVAI_PATH_MAX];
            novai_join_path(child, sizeof(child), base, entries[i].name);
            novai_find_recurse(child, relpath, depth + 1,
                                flags, found, out);
        }
    }

    free(entries);
}

int nova_tool_find(const NovaToolFlags *flags) {
    if (flags == NULL) return -1;

    const char *dir = ".";
    if (flags->subject_count > 0) {
        dir = flags->subjects[0];
    }

    if (!novai_is_directory(dir)) {
        fprintf(stderr, "nova find: '%s' is not a directory\n", dir);
        return -1;
    }

    FILE *out = stdout;
    if (flags->output != NULL) {
        out = fopen(flags->output,
                    flags->append_mode ? "a" : "w");
        if (out == NULL) {
            fprintf(stderr, "nova find: cannot open output '%s': %s\n",
                    flags->output, strerror(errno));
            return -1;
        }
    }

    int found = 0;
    novai_find_recurse(dir, "", 0, flags, &found, out);

    if (flags->verbose) {
        fprintf(stderr, "%d file(s) found\n", found);
    }

    if (flags->output != NULL && out != stdout) {
        fclose(out);
    }
    return 0;
}

/* ============================================================
 * TOOL: grep — Search text in files
 * ============================================================ */

/**
 * @brief Grep a single file, writing matches to out.
 *
 * @return Number of matches found in this file.
 */
static int novai_grep_file(const char *path, const char *show_name,
                            const NovaToolFlags *flags, FILE *out,
                            int *total_found) {
    FILE *fp = fopen(path, "r");
    if (fp == NULL) {
        fprintf(stderr, "nova grep: cannot open '%s': %s\n",
                path, strerror(errno));
        return -1;
    }

    const char *pattern = flags->match;
    if (pattern == NULL) {
        fclose(fp);
        return 0;
    }

    char line[NOVAI_LINE_MAX];
    int line_num = 0;
    int file_matches = 0;

    while (fgets(line, (int)sizeof(line), fp) != NULL) {
        line_num++;

        if (flags->limit > 0 && *total_found >= flags->limit) break;

        /* Check for match */
        int found;
        if (flags->ignore_case) {
            found = (novai_strcasestr(line, pattern) != NULL);
        } else {
            found = (strstr(line, pattern) != NULL);
        }

        if (flags->invert) found = !found;

        if (found) {
            /* Remove trailing newline for cleaner output */
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

/**
 * @brief Recursively grep a directory.
 */
static void novai_grep_dir(const char *dir, const NovaToolFlags *flags,
                            FILE *out, int *total_found) {
    NovaiDirEntry *entries = (NovaiDirEntry *)malloc(
        (size_t)NOVAI_DIR_MAX * sizeof(NovaiDirEntry));
    if (entries == NULL) return;

    int count = novai_read_dir(dir, entries, NOVAI_DIR_MAX,
                               flags->show_all);

    for (int i = 0; i < count; i++) {
        if (flags->limit > 0 && *total_found >= flags->limit) break;

        char fullpath[NOVAI_PATH_MAX];
        novai_join_path(fullpath, sizeof(fullpath), dir, entries[i].name);

        if (entries[i].is_dir) {
            novai_grep_dir(fullpath, flags, out, total_found);
        } else {
            novai_grep_file(fullpath, fullpath, flags, out, total_found);
        }
    }

    free(entries);
}

int nova_tool_grep(const NovaToolFlags *flags) {
    if (flags == NULL) return -1;

    if (flags->match == NULL) {
        fprintf(stderr, "nova grep: --match/-m pattern required\n");
        fprintf(stderr, "Usage: nova grep [file...] -m=PATTERN\n");
        return -1;
    }

    FILE *out = stdout;
    if (flags->output != NULL) {
        out = fopen(flags->output,
                    flags->append_mode ? "a" : "w");
        if (out == NULL) {
            fprintf(stderr, "nova grep: cannot open output '%s': %s\n",
                    flags->output, strerror(errno));
            return -1;
        }
    }

    int total_found = 0;
    int result = 0;

    if (flags->subject_count == 0) {
        /* Read from stdin */
        if (novai_stdin_has_data()) {
            char line[NOVAI_LINE_MAX];
            int line_num = 0;

            while (fgets(line, (int)sizeof(line), stdin) != NULL) {
                line_num++;
                if (flags->limit > 0 && total_found >= flags->limit) break;

                int fnd;
                if (flags->ignore_case) {
                    fnd = (novai_strcasestr(line, flags->match) != NULL);
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
            fprintf(stderr, "nova grep: no files specified\n");
            result = -1;
        }
    } else {
        int multi_file = (flags->subject_count > 1 || flags->recursive);

        for (int i = 0; i < flags->subject_count; i++) {
            const char *subj = flags->subjects[i];

            if (novai_is_directory(subj)) {
                if (flags->recursive) {
                    novai_grep_dir(subj, flags, out, &total_found);
                } else {
                    fprintf(stderr,
                            "nova grep: '%s' is a directory "
                            "(use -r/--recursive)\n", subj);
                }
            } else {
                const char *display =
                    multi_file ? subj : NULL;
                novai_grep_file(subj, display, flags, out,
                                &total_found);
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

/* ============================================================
 * TOOL: head — First N lines of a file
 * ============================================================ */

int nova_tool_head(const NovaToolFlags *flags) {
    if (flags == NULL) return -1;

    int max_lines = flags->limit > 0 ? flags->limit : 10;
    FILE *out = stdout;

    if (flags->subject_count == 0) {
        /* Read from stdin */
        if (novai_stdin_has_data()) {
            char line[NOVAI_LINE_MAX];
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
            fprintf(stderr, "nova head: no files specified\n");
            return -1;
        }
        return 0;
    }

    int multi = (flags->subject_count > 1);

    for (int fi = 0; fi < flags->subject_count; fi++) {
        const char *path = flags->subjects[fi];
        FILE *fp = fopen(path, "r");
        if (fp == NULL) {
            fprintf(stderr, "nova head: cannot open '%s': %s\n",
                    path, strerror(errno));
            continue;
        }

        if (multi) {
            if (fi > 0) printf("\n");
            printf("==> %s <==\n", path);
        }

        char line[NOVAI_LINE_MAX];
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

/* ============================================================
 * TOOL: tail — Last N lines of a file
 * ============================================================ */

int nova_tool_tail(const NovaToolFlags *flags) {
    if (flags == NULL) return -1;

    int max_lines = flags->limit > 0 ? flags->limit : 10;
    FILE *out = stdout;

    if (flags->subject_count == 0) {
        /* Read from stdin — must buffer all lines */
        if (novai_stdin_has_data()) {
            char **lines = NULL;
            int line_count = 0;
            int line_cap = 256;

            lines = (char **)malloc((size_t)line_cap * sizeof(char *));
            if (lines == NULL) {
                fprintf(stderr, "nova tail: out of memory\n");
                return -1;
            }

            char buf[NOVAI_LINE_MAX];
            while (fgets(buf, (int)sizeof(buf), stdin) != NULL) {
                if (line_count >= line_cap) {
                    line_cap *= 2;
                    char **tmp = (char **)realloc(
                        lines, (size_t)line_cap * sizeof(char *));
                    if (tmp == NULL) {
                        /* Free what we have and bail */
                        for (int j = 0; j < line_count; j++) free(lines[j]);
                        free(lines);
                        fprintf(stderr, "nova tail: out of memory\n");
                        return -1;
                    }
                    lines = tmp;
                }
                lines[line_count] = strdup(buf);
                if (lines[line_count] == NULL) {
                    for (int j = 0; j < line_count; j++) free(lines[j]);
                    free(lines);
                    fprintf(stderr, "nova tail: out of memory\n");
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
            fprintf(stderr, "nova tail: no files specified\n");
            return -1;
        }
        return 0;
    }

    int multi = (flags->subject_count > 1);

    for (int fi = 0; fi < flags->subject_count; fi++) {
        const char *path = flags->subjects[fi];
        FILE *fp = fopen(path, "r");
        if (fp == NULL) {
            fprintf(stderr, "nova tail: cannot open '%s': %s\n",
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
            fprintf(stderr, "nova tail: out of memory\n");
            continue;
        }

        char buf[NOVAI_LINE_MAX];
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

/* ============================================================
 * TOOL: wc — Word, line, and character count
 * ============================================================ */

/**
 * @brief Count lines, words, and chars in a stream.
 */
static void novai_wc_stream(FILE *fp, long *out_lines,
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

int nova_tool_wc(const NovaToolFlags *flags) {
    if (flags == NULL) return -1;

    int specific = (flags->count_lines || flags->count_words ||
                    flags->count_chars);

    if (flags->subject_count == 0) {
        /* Read from stdin */
        if (novai_stdin_has_data()) {
            long lines = 0, words = 0, chars = 0;
            novai_wc_stream(stdin, &lines, &words, &chars);

            if (!specific) {
                printf("%7ld %7ld %7ld\n", lines, words, chars);
            } else {
                if (flags->count_lines) printf("%7ld", lines);
                if (flags->count_words) printf("%7ld", words);
                if (flags->count_chars) printf("%7ld", chars);
                printf("\n");
            }
        } else {
            fprintf(stderr, "nova wc: no files specified\n");
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
            fprintf(stderr, "nova wc: cannot open '%s': %s\n",
                    path, strerror(errno));
            continue;
        }

        long lines = 0, words = 0, chars = 0;
        novai_wc_stream(fp, &lines, &words, &chars);
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

/* ============================================================
 * TOOL: write — Write stdin to file (pipe endpoint)
 * ============================================================ */

int nova_tool_write(const NovaToolFlags *flags) {
    if (flags == NULL) return -1;

    if (flags->subject_count == 0) {
        fprintf(stderr, "nova write: output file required\n");
        fprintf(stderr, "Usage: nova write <file> [--append]\n");
        return -1;
    }

    const char *path = flags->subjects[0];
    const char *mode = flags->append_mode ? "a" : "w";

    FILE *out = fopen(path, mode);
    if (out == NULL) {
        fprintf(stderr, "nova write: cannot open '%s': %s\n",
                path, strerror(errno));
        return -1;
    }

    /* Read from stdin (piped data or remaining subjects as lines) */
    if (novai_stdin_has_data()) {
        char buf[NOVAI_LINE_MAX];
        while (fgets(buf, (int)sizeof(buf), stdin) != NULL) {
            fputs(buf, out);
        }
    } else if (flags->subject_count > 1) {
        /* Write remaining subjects as content */
        for (int i = 1; i < flags->subject_count; i++) {
            fputs(flags->subjects[i], out);
            fputc('\n', out);
        }
    } else {
        fprintf(stderr, "nova write: no input (pipe data or provide text)\n");
        fclose(out);
        return -1;
    }

    fclose(out);

    if (flags->verbose) {
        fprintf(stderr, "Wrote to '%s'\n", path);
    }

    return 0;
}

/* ============================================================
 * ECHO AND PWD TOOLS
 * ============================================================ */

int nova_tool_echo(const NovaToolFlags *flags) {
    if (flags == NULL) return -1;
    for (int i = 0; i < flags->subject_count; i++) {
        if (i > 0) putchar(' ');
        fputs(flags->subjects[i], stdout);
    }
    putchar('\n');
    return 0;
}

int nova_tool_pwd(const NovaToolFlags *flags) {
    (void)flags;
    char cwd[NOVAI_PATH_MAX];
    if (novai_getcwd(cwd, sizeof(cwd)) != NULL) {
        printf("%s\n", cwd);
        return 0;
    }
    fprintf(stderr, "nova pwd: %s\n", strerror(errno));
    return -1;
}

/* ============================================================
 * TOOL REGISTRY AND DISPATCH
 * ============================================================ */

/** Names of all known tools */
static const char *novai_tool_names[] = {
    "cat", "ls", "tree", "find", "grep",
    "head", "tail", "wc", "write",
    "echo", "pwd", "task",
    NULL
};

int nova_tool_is_tool(const char *name) {
    if (name == NULL) return 0;
    for (int i = 0; novai_tool_names[i] != NULL; i++) {
        if (strcmp(name, novai_tool_names[i]) == 0) return 1;
    }
    return 0;
}

void nova_tool_print_help(void) {
    int c = nova_diag_color_enabled();
    const char *h  = c ? "\033[1;38;5;22m\033[4m" : ""; /* hunter+uline */
    const char *e  = c ? "\033[38;5;35m" : "";            /* emerald     */
    const char *m  = c ? "\033[38;5;245m" : "";           /* muted       */
    const char *r  = c ? "\033[0m" : "";                  /* reset       */

    printf("%sTOOLS%s\n", h, r);
    printf("  %scat%s   [file...]          %sConcatenate and print files%s\n", e, r, m, r);
    printf("  %sls%s    [dir]              %sList directory contents%s\n", e, r, m, r);
    printf("  %stree%s  [dir]              %sDirectory tree visualization%s\n", e, r, m, r);
    printf("  %sfind%s  [dir] -m=PATTERN   %sFind files by name pattern%s\n", e, r, m, r);
    printf("  %sgrep%s  [file...] -m=PAT   %sSearch text in files%s\n", e, r, m, r);
    printf("  %shead%s  [file...]          %sFirst N lines of a file%s\n", e, r, m, r);
    printf("  %stail%s  [file...]          %sLast N lines of a file%s\n", e, r, m, r);
    printf("  %swc%s    [file...]          %sWord, line, and character count%s\n", e, r, m, r);
    printf("  %swrite%s <file>             %sWrite stdin to file%s\n", e, r, m, r);
    printf("  %secho%s  [text...]          %sPrint text to stdout%s\n", e, r, m, r);
    printf("  %spwd%s                      %sPrint working directory%s\n", e, r, m, r);
    printf("  %stask%s  [name...]          %sRun NINI taskfile tasks%s\n\n", e, r, m, r);

    printf("%sFLAGS%s\n", h, r);
    printf("  %s-m%s, %s--match%s=PATTERN      Filter/search pattern\n", e, r, e, r);
    printf("  %s-i%s, %s--ignore-case%s        Case-insensitive matching\n", e, r, e, r);
    printf("  %s-v%s, %s--invert%s             Invert match (exclude)\n", e, r, e, r);
    printf("  %s-r%s, %s--recursive%s          Recurse into directories\n", e, r, e, r);
    printf("  %s-o%s, %s--output%s=FILE        Redirect output to file\n", e, r, e, r);
    printf("  %s-l%s, %s--limit%s=N            Limit results\n", e, r, e, r);
    printf("  %s-d%s, %s--depth%s=N            Maximum directory depth\n", e, r, e, r);
    printf("  %s-V%s, %s--verbose%s            Verbose output\n", e, r, e, r);
    printf("  %s-a%s, %s--all%s                Show hidden files\n", e, r, e, r);
    printf("  %s-n%s, %s--number%s             Show line numbers\n", e, r, e, r);
    printf("  %s-L%s, %s--long%s               Detailed listing\n", e, r, e, r);
    printf("  %s-A%s, %s--append%s             Append to output file\n", e, r, e, r);
    printf("      %s--dry%s                Dry run (print commands only)\n\n", e, r);
}

int nova_tool_dispatch(const char *tool, int argc, char **argv) {
    if (tool == NULL) return -1;

    NovaToolFlags flags;
    if (nova_tool_parse_flags(argc, argv, &flags) != 0) {
        return 1;
    }

    if (strcmp(tool, "cat") == 0)   return nova_tool_cat(&flags);
    if (strcmp(tool, "ls") == 0)    return nova_tool_ls(&flags);
    if (strcmp(tool, "tree") == 0)  return nova_tool_tree(&flags);
    if (strcmp(tool, "find") == 0)  return nova_tool_find(&flags);
    if (strcmp(tool, "grep") == 0)  return nova_tool_grep(&flags);
    if (strcmp(tool, "head") == 0)  return nova_tool_head(&flags);
    if (strcmp(tool, "tail") == 0)  return nova_tool_tail(&flags);
    if (strcmp(tool, "wc") == 0)    return nova_tool_wc(&flags);
    if (strcmp(tool, "write") == 0) return nova_tool_write(&flags);
    if (strcmp(tool, "echo") == 0)  return nova_tool_echo(&flags);
    if (strcmp(tool, "pwd") == 0)   return nova_tool_pwd(&flags);
    if (strcmp(tool, "task") == 0)  return nova_tool_task(&flags);

    fprintf(stderr, "nova: unknown tool: %s\n", tool);
    return 1;
}
