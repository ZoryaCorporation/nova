/**
 * @file ntool_common.c
 * @brief Nova Tools - Shared Utility Library Implementation
 *
 * Cross-platform implementations of common tool utilities.
 * Built on the Zorya PAL for freestanding portability.
 *
 * @author Anthony Taliento
 * @date 2026-03-16
 * @version 0.2.0
 *
 * @copyright Copyright (c) 2026 Zorya Corporation
 * @license MIT
 *
 * ZORYA-C COMPLIANCE: v2.0.0
 */

#include "ntool_common.h"

#include <string.h>
#include <ctype.h>
#include <time.h>

/* ============================================================
 * FLAG PARSER
 * ============================================================ */

int ntool_parse_flags(int argc, char **argv, NToolFlags *flags) {
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
                fprintf(stderr, "ntool: unknown flag: %s\n", arg);
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
                fprintf(stderr, "ntool: unknown flag: %s\n", arg);
                return -1;
            }

        } else {
            /* ---- Positional argument ---- */
            if (flags->subject_count < NTOOL_MAX_SUBJECTS) {
                flags->subjects[flags->subject_count++] = arg;
            }
        }
    }

    return 0;
}

/* ============================================================
 * DIRECTORY READING (PAL-backed)
 * ============================================================ */

int ntool_read_dir(const char *path, NToolDirEntry *entries,
                   int max_entries, int show_hidden) {
    if (path == NULL || entries == NULL) return 0;
    int count = 0;

    ZoryaDir dir;
    if (zorya_dir_open(path, &dir) != 0) return 0;

    ZoryaDirEntry entry;
    while (zorya_dir_next(&dir, &entry) == 0) {
        if (strcmp(entry.name, ".") == 0 ||
            strcmp(entry.name, "..") == 0) {
            continue;
        }
        if (!show_hidden && entry.name[0] == '.') {
            continue;
        }
        if (count >= max_entries) break;

        memcpy(&entries[count], &entry, sizeof(NToolDirEntry));
        count++;
    }

    zorya_dir_close(&dir);
    return count;
}

int ntool_entry_cmp(const void *a, const void *b) {
    const NToolDirEntry *ea = (const NToolDirEntry *)a;
    const NToolDirEntry *eb = (const NToolDirEntry *)b;

    /* Directories before files */
    if (ea->is_dir && !eb->is_dir) return -1;
    if (!ea->is_dir && eb->is_dir) return 1;

    /* Alphabetical within category */
    return strcmp(ea->name, eb->name);
}

/* ============================================================
 * PATH / FILESYSTEM QUERIES (PAL-backed)
 * ============================================================ */

int ntool_is_directory(const char *path) {
    if (path == NULL) return 0;
    return zorya_is_dir(path);
}

long ntool_file_size(const char *path) {
    if (path == NULL) return -1;
    ZoryaStat st;
    if (zorya_stat(path, &st) != 0) return -1;
    return (long)st.size;
}

long ntool_file_mtime(const char *path) {
    if (path == NULL) return 0;
    ZoryaStat st;
    if (zorya_stat(path, &st) != 0) return 0;
    return (long)st.mtime;
}

void ntool_join_path(char *buf, size_t bufsz,
                     const char *base, const char *name) {
    if (base == NULL || name == NULL || buf == NULL) return;
    size_t blen = strlen(base);
    if (blen > 0 && (base[blen - 1] == '/' || base[blen - 1] == '\\')) {
        snprintf(buf, bufsz, "%s%s", base, name);
    } else {
        snprintf(buf, bufsz, "%s/%s", base, name);
    }
}

/* ============================================================
 * PATTERN MATCHING
 * ============================================================ */

int ntool_glob_match(const char *pattern, const char *str) {
    const char *p = pattern;
    const char *s = str;
    const char *star_p = NULL;
    const char *star_s = NULL;

    while (*s != '\0') {
        if (*p == '*') {
            star_p = p++;
            star_s = s;
        } else if (*p == '?' || *p == *s) {
            p++;
            s++;
        } else if (star_p != NULL) {
            p = star_p + 1;
            s = ++star_s;
        } else {
            return 0;
        }
    }
    while (*p == '*') p++;
    return (*p == '\0');
}

int ntool_glob_match_ci(const char *pattern, const char *str) {
    char p_low[256];
    char s_low[256];
    size_t pi = 0, si = 0;

    while (pattern[pi] != '\0' && pi < sizeof(p_low) - 1) {
        p_low[pi] = (char)tolower((unsigned char)pattern[pi]);
        pi++;
    }
    p_low[pi] = '\0';

    while (str[si] != '\0' && si < sizeof(s_low) - 1) {
        s_low[si] = (char)tolower((unsigned char)str[si]);
        si++;
    }
    s_low[si] = '\0';

    return ntool_glob_match(p_low, s_low);
}

const char *ntool_strcasestr(const char *haystack, const char *needle) {
    if (haystack == NULL || needle == NULL) return NULL;
    if (*needle == '\0') return haystack;

    size_t nlen = strlen(needle);
    for (; *haystack != '\0'; haystack++) {
        if ((size_t)strlen(haystack) < nlen) return NULL;
        int found = 1;
        for (size_t i = 0; i < nlen; i++) {
            if (tolower((unsigned char)haystack[i]) !=
                tolower((unsigned char)needle[i])) {
                found = 0;
                break;
            }
        }
        if (found) return haystack;
    }
    return NULL;
}

/* ============================================================
 * FORMATTING
 * ============================================================ */

void ntool_format_size(long size, char *buf, size_t bufsz) {
    if (buf == NULL) return;
    if (size < 0) {
        snprintf(buf, bufsz, "?");
    } else if (size < 1024L) {
        snprintf(buf, bufsz, "%ldB", size);
    } else if (size < 1024L * 1024L) {
        snprintf(buf, bufsz, "%.1fK", (double)size / 1024.0);
    } else if (size < 1024L * 1024L * 1024L) {
        snprintf(buf, bufsz, "%.1fM", (double)size / (1024.0 * 1024.0));
    } else {
        snprintf(buf, bufsz, "%.1fG",
                 (double)size / (1024.0 * 1024.0 * 1024.0));
    }
}

void ntool_format_time(long mtime, char *buf, size_t bufsz) {
    if (buf == NULL) return;
    time_t t = (time_t)mtime;
    struct tm *tm = localtime(&t);
    if (tm != NULL) {
        strftime(buf, bufsz, "%Y-%m-%d %H:%M", tm);
    } else {
        snprintf(buf, bufsz, "%s", "----/--/-- --:--");
    }
}

/* ============================================================
 * ENVIRONMENT DETECTION (PAL-backed)
 * ============================================================ */

int ntool_stdin_has_data(void) {
    return !zorya_isatty(zorya_fileno(stdin));
}

int ntool_color_enabled(void) {
    /* Respect NO_COLOR convention (https://no-color.org/) */
    const char *nc = zorya_getenv("NO_COLOR");
    if (nc != NULL && nc[0] != '\0') return 0;

    /* Check if stdout is a TTY */
    return zorya_isatty(zorya_fileno(stdout));
}
