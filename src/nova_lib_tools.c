/**
 * @file nova_lib_tools.c
 * @brief Nova Language - Tools Standard Library (In-Process)
 *
 * Exposes Nova's CLI tools as in-process functions callable from
 * Nova scripts. Returns structured data (strings, tables) instead
 * of printing to stdout. Zero subprocess overhead.
 *
 * Module: tools
 *   tools.cat(path)              Read file → string
 *   tools.ls([dir])              List dir  → table of entries
 *   tools.tree([dir [, depth]])  Dir tree  → string
 *   tools.find(dir, pattern)     Find      → table of paths
 *   tools.grep(pattern, path...) Search    → table of matches
 *   tools.head(path [, n])       First N   → string
 *   tools.tail(path [, n])       Last N    → string
 *   tools.wc(path)               Count     → table {lines,words,chars}
 *   tools.pwd()                  Cwd       → string
 *   tools.run(command)           Execute   → string (captured stdout)
 *
 * @author Anthony Taliento
 * @date 2026-02-18
 * @version 0.2.0
 *
 * @copyright Copyright (c) 2026 Zorya Corporation
 * @license MIT
 *
 * ZORYA-C COMPLIANCE: v2.0.0
 */

#include "nova/nova_lib.h"
#include "nova/nova_vm.h"
#include "tools/shared/ntool_common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>
#include <errno.h>

#include <zorya/pal.h>

#ifdef _WIN32
    #include <windows.h>
    #include <sys/stat.h>
    #include <sys/types.h>
#else
    #include <unistd.h>
    #include <sys/stat.h>
    #include <sys/types.h>
    #include <dirent.h>
#endif

/* ============================================================
 * CONSTANTS
 * ============================================================ */

#define NOVAI_TL_PATH_MAX   4096
#define NOVAI_TL_LINE_MAX   8192
#define NOVAI_TL_BUF_INIT   4096
#define NOVAI_TL_DEPTH_MAX  64

/* ============================================================
 * DYNAMIC STRING BUFFER
 * ============================================================ */

typedef struct {
    char   *data;
    size_t  len;
    size_t  cap;
} NovaiStrBuf;

static void novai_sb_init(NovaiStrBuf *sb) {
    sb->data = NULL;
    sb->len  = 0;
    sb->cap  = 0;
}

static void novai_sb_free(NovaiStrBuf *sb) {
    free(sb->data);
    sb->data = NULL;
    sb->len  = 0;
    sb->cap  = 0;
}

static int novai_sb_ensure(NovaiStrBuf *sb, size_t extra) {
    size_t need = sb->len + extra + 1;
    if (need <= sb->cap) { return 0; }
    size_t newcap = (sb->cap == 0) ? NOVAI_TL_BUF_INIT : sb->cap;
    while (newcap < need) { newcap *= 2; }
    char *p = (char *)realloc(sb->data, newcap);
    if (p == NULL) { return -1; }
    sb->data = p;
    sb->cap  = newcap;
    return 0;
}

static int novai_sb_append(NovaiStrBuf *sb, const char *s, size_t n) {
    if (novai_sb_ensure(sb, n) != 0) { return -1; }
    memcpy(sb->data + sb->len, s, n);
    sb->len += n;
    sb->data[sb->len] = '\0';
    return 0;
}

static int novai_sb_appendf(NovaiStrBuf *sb, const char *fmt, ...) {
    char tmp[NOVAI_TL_LINE_MAX];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n < 0) { return -1; }
    return novai_sb_append(sb, tmp, (size_t)n);
}

/* ============================================================
 * TOOL: cat — Read file contents
 * ============================================================ */

/**
 * tools.cat(path) → string
 *
 * Reads the entire file and returns it as a string.
 */
static int nova_tools_cat(NovaVM *vm) {
    if (nova_lib_check_args(vm, 1) != 0) { return -1; }
    const char *path = nova_lib_check_string(vm, 0);
    if (path == NULL) { return -1; }

    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        nova_vm_raise_error(vm, "tools.cat: cannot open '%s': %s",
                            path, strerror(errno));
        return -1;
    }

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        nova_vm_raise_error(vm, "tools.cat: cannot seek '%s'", path);
        return -1;
    }
    long sz = ftell(f);
    if (sz < 0) { fclose(f); nova_vm_push_string(vm, "", 0); return 1; }
    fseek(f, 0, SEEK_SET);

    char *buf = (char *)malloc((size_t)sz + 1);
    if (buf == NULL) {
        fclose(f);
        nova_vm_raise_error(vm, "tools.cat: out of memory");
        return -1;
    }

    size_t nread = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[nread] = '\0';

    nova_vm_push_string(vm, buf, nread);
    free(buf);
    return 1;
}

/* ============================================================
 * TOOL: ls — List directory
 * ============================================================ */

/**
 * tools.ls([dir]) → table of {name, type, size}
 *
 * Returns a 0-indexed array table of entry tables.
 * Each entry: {name = "foo", type = "file"|"dir", size = N}
 */
static int nova_tools_ls(NovaVM *vm) {
    const char *dir = nova_lib_opt_string(vm, 0, ".");

#ifdef _WIN32
    /* Windows directory listing */
    (void)dir;
    nova_vm_push_table(vm);
    /* TODO: Windows implementation */
    return 1;
#else
    DIR *d = opendir(dir);
    if (d == NULL) {
        nova_vm_raise_error(vm, "tools.ls: cannot open '%s': %s",
                            dir, strerror(errno));
        return -1;
    }

    nova_vm_push_table(vm);
    NovaValue result = nova_vm_get(vm, nova_vm_get_top(vm) - 1);
    NovaTable *tbl = nova_as_table(result);

    struct dirent *ent;
    nova_int_t idx = 0;
    while ((ent = readdir(d)) != NULL) {
        /* Skip . and .. */
        if (strcmp(ent->d_name, ".") == 0 ||
            strcmp(ent->d_name, "..") == 0) {
            continue;
        }

        /* Build full path for stat */
        char fullpath[NOVAI_TL_PATH_MAX];
        snprintf(fullpath, sizeof(fullpath), "%s/%s", dir, ent->d_name);

        /* Create entry table */
        nova_vm_push_table(vm);
        NovaValue entry_val = nova_vm_get(vm,
                                          nova_vm_get_top(vm) - 1);
        NovaTable *entry = nova_as_table(entry_val);

        /* name */
        NovaString *name_key = nova_string_new(vm, "name", 4);
        if (name_key != NULL) {
            NovaValue name_val;
            NovaString *ns = nova_string_new(vm, ent->d_name,
                                             strlen(ent->d_name));
            if (ns != NULL) {
                name_val = nova_value_string(ns);
                nova_table_set_str(vm, entry, name_key, name_val);
            }
        }

        /* type + size */
        struct stat st;
        const char *type_str = "file";
        nova_int_t fsize = 0;
        if (stat(fullpath, &st) == 0) {
            if (S_ISDIR(st.st_mode)) {
                type_str = "dir";
            } else if (S_ISLNK(st.st_mode)) {
                type_str = "link";
            }
            fsize = (nova_int_t)st.st_size;
        }

        NovaString *type_key = nova_string_new(vm, "type", 4);
        if (type_key != NULL) {
            NovaString *ts = nova_string_new(vm, type_str,
                                             strlen(type_str));
            if (ts != NULL) {
                nova_table_set_str(vm, entry, type_key,
                                   nova_value_string(ts));
            }
        }

        NovaString *size_key = nova_string_new(vm, "size", 4);
        if (size_key != NULL) {
            nova_table_set_str(vm, entry, size_key,
                               nova_value_integer(fsize));
        }

        /* Pop entry from helper position, set in result array */
        vm->stack_top--;
        nova_table_set_int(vm, tbl, idx, entry_val);
        idx++;
    }
    closedir(d);

    return 1;
#endif
}

/* ============================================================
 * TOOL: tree — Directory tree visualization
 * ============================================================ */

/**
 * @brief Recursive tree builder.
 */
static void novai_tl_tree_recurse(const char *dir, const char *prefix,
                                   int depth, int max_depth,
                                   NovaiStrBuf *sb) {
    if (max_depth >= 0 && depth > max_depth) { return; }

#ifndef _WIN32
    DIR *d = opendir(dir);
    if (d == NULL) { return; }

    /* Collect entries (skip . and ..) */
    struct dirent *entries[1024];
    int count = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL && count < 1024) {
        if (strcmp(ent->d_name, ".") == 0 ||
            strcmp(ent->d_name, "..") == 0) {
            continue;
        }
        entries[count] = ent;
        count++;
    }

    for (int i = 0; i < count; i++) {
        int last = (i == count - 1);
        const char *connector = last ? "\xe2\x94\x94\xe2\x94\x80\xe2\x94\x80 "
                                     : "\xe2\x94\x9c\xe2\x94\x80\xe2\x94\x80 ";
        novai_sb_appendf(sb, "%s%s%s\n", prefix, connector,
                         entries[i]->d_name);

        /* Recurse into directories */
        char fullpath[NOVAI_TL_PATH_MAX];
        snprintf(fullpath, sizeof(fullpath), "%s/%s",
                 dir, entries[i]->d_name);
        if (ntool_is_directory(fullpath)) {
            char new_prefix[NOVAI_TL_PATH_MAX];
            const char *ext = last ? "    " : "\xe2\x94\x82   ";
            snprintf(new_prefix, sizeof(new_prefix), "%s%s",
                     prefix, ext);
            novai_tl_tree_recurse(fullpath, new_prefix,
                                   depth + 1, max_depth, sb);
        }
    }
    closedir(d);
#endif
    (void)dir; (void)prefix; (void)depth; (void)max_depth; (void)sb;
}

/**
 * tools.tree([dir [, depth]]) → string
 *
 * Returns a visual directory tree using Unicode box characters.
 */
static int nova_tools_tree(NovaVM *vm) {
    const char *dir = nova_lib_opt_string(vm, 0, ".");
    int max_depth = -1;
    if (nova_vm_get_top(vm) >= 2) {
        nova_int_t d;
        if (nova_lib_check_integer(vm, 1, &d)) {
            max_depth = (int)d;
        }
    }

    NovaiStrBuf sb;
    novai_sb_init(&sb);

    novai_sb_appendf(&sb, "%s\n", dir);
    novai_tl_tree_recurse(dir, "", 0, max_depth, &sb);

    if (sb.data != NULL) {
        nova_vm_push_string(vm, sb.data, sb.len);
    } else {
        nova_vm_push_string(vm, "", 0);
    }
    novai_sb_free(&sb);
    return 1;
}

/* ============================================================
 * TOOL: find — Find files by name pattern
 * ============================================================ */

/**
 * @brief Recursive find helper.
 */
static void novai_tl_find_recurse(NovaVM *vm, NovaTable *results,
                                   nova_int_t *idx,
                                   const char *dir, const char *pat,
                                   int depth, int max_depth) {
    if (max_depth >= 0 && depth > max_depth) { return; }

#ifndef _WIN32
    DIR *d = opendir(dir);
    if (d == NULL) { return; }

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 ||
            strcmp(ent->d_name, "..") == 0) {
            continue;
        }

        char fullpath[NOVAI_TL_PATH_MAX];
        snprintf(fullpath, sizeof(fullpath), "%s/%s",
                 dir, ent->d_name);

        /* Check name against pattern */
        if (ntool_glob_match(pat, ent->d_name)) {
            NovaString *s = nova_string_new(vm, fullpath,
                                            strlen(fullpath));
            if (s != NULL) {
                nova_table_set_int(vm, results, *idx,
                                   nova_value_string(s));
                (*idx)++;
            }
        }

        /* Recurse into directories */
        if (ntool_is_directory(fullpath)) {
            novai_tl_find_recurse(vm, results, idx,
                                   fullpath, pat,
                                   depth + 1, max_depth);
        }
    }
    closedir(d);
#endif
    (void)vm; (void)results; (void)idx;
    (void)dir; (void)pat; (void)depth; (void)max_depth;
}

/**
 * tools.find(dir, pattern [, depth]) → table of paths
 *
 * Recursively finds files matching the glob pattern.
 * Returns a 0-indexed array of path strings.
 */
static int nova_tools_find(NovaVM *vm) {
    if (nova_lib_check_args(vm, 2) != 0) { return -1; }
    const char *dir = nova_lib_check_string(vm, 0);
    if (dir == NULL) { return -1; }
    const char *pat = nova_lib_check_string(vm, 1);
    if (pat == NULL) { return -1; }

    int max_depth = -1;
    if (nova_vm_get_top(vm) >= 3) {
        nova_int_t d;
        if (nova_lib_check_integer(vm, 2, &d)) {
            max_depth = (int)d;
        }
    }

    nova_vm_push_table(vm);
    NovaValue result = nova_vm_get(vm, nova_vm_get_top(vm) - 1);
    NovaTable *tbl = nova_as_table(result);
    nova_int_t idx = 0;

    novai_tl_find_recurse(vm, tbl, &idx, dir, pat, 0, max_depth);

    return 1;
}

/* ============================================================
 * TOOL: grep — Search text in files
 * ============================================================ */

/**
 * @brief Grep a single file, appending matches to results table.
 */
static void novai_tl_grep_file(NovaVM *vm, NovaTable *results,
                                nova_int_t *idx,
                                const char *pattern, const char *path,
                                int ignore_case) {
    FILE *f = fopen(path, "r");
    if (f == NULL) { return; }

    char line[NOVAI_TL_LINE_MAX];
    nova_int_t line_num = 1;

    while (fgets(line, (int)sizeof(line), f) != NULL) {
        /* Remove trailing newline */
        size_t ln = strlen(line);
        if (ln > 0 && line[ln - 1] == '\n') { line[--ln] = '\0'; }
        if (ln > 0 && line[ln - 1] == '\r') { line[--ln] = '\0'; }

        /* Match */
        int matched = 0;
        if (ignore_case) {
            matched = (ntool_strcasestr(line, pattern) != NULL);
        } else {
            matched = (strstr(line, pattern) != NULL);
        }

        if (matched) {
            /* Create match entry: {file, line, num, text} */
            nova_vm_push_table(vm);
            NovaValue entry_val = nova_vm_get(vm,
                                              nova_vm_get_top(vm) - 1);
            NovaTable *entry = nova_as_table(entry_val);

            NovaString *k;

            k = nova_string_new(vm, "file", 4);
            if (k != NULL) {
                NovaString *v = nova_string_new(vm, path, strlen(path));
                if (v != NULL) {
                    nova_table_set_str(vm, entry, k,
                                       nova_value_string(v));
                }
            }

            k = nova_string_new(vm, "num", 3);
            if (k != NULL) {
                nova_table_set_str(vm, entry, k,
                                   nova_value_integer(line_num));
            }

            k = nova_string_new(vm, "text", 4);
            if (k != NULL) {
                NovaString *v = nova_string_new(vm, line, ln);
                if (v != NULL) {
                    nova_table_set_str(vm, entry, k,
                                       nova_value_string(v));
                }
            }

            vm->stack_top--;
            nova_table_set_int(vm, results, *idx, entry_val);
            (*idx)++;
        }
        line_num++;
    }
    fclose(f);
}

/**
 * tools.grep(pattern, path...) → table of matches
 *
 * Each match: {file = "path", num = N, text = "line contents"}
 * Pass multiple file paths or a single path.
 */
static int nova_tools_grep(NovaVM *vm) {
    if (nova_lib_check_args(vm, 2) != 0) { return -1; }
    const char *pattern = nova_lib_check_string(vm, 0);
    if (pattern == NULL) { return -1; }

    nova_vm_push_table(vm);
    NovaValue result = nova_vm_get(vm, nova_vm_get_top(vm) - 1);
    NovaTable *tbl = nova_as_table(result);
    nova_int_t idx = 0;

    int nargs = nova_vm_get_top(vm) - 1; /* -1 for result table */
    for (int i = 1; i < nargs; i++) {
        NovaValue arg = nova_vm_get(vm, i);
        if (nova_is_string(arg)) {
            novai_tl_grep_file(vm, tbl, &idx, pattern,
                               nova_str_data(nova_as_string(arg)), 0);
        }
    }

    return 1;
}

/* ============================================================
 * TOOL: head — First N lines
 * ============================================================ */

/**
 * tools.head(path [, n]) → string
 *
 * Returns the first N lines of a file (default 10).
 */
static int nova_tools_head(NovaVM *vm) {
    if (nova_lib_check_args(vm, 1) != 0) { return -1; }
    const char *path = nova_lib_check_string(vm, 0);
    if (path == NULL) { return -1; }

    nova_int_t n = 10;
    if (nova_vm_get_top(vm) >= 2) {
        (void)nova_lib_check_integer(vm, 1, &n);
    }

    FILE *f = fopen(path, "r");
    if (f == NULL) {
        nova_vm_raise_error(vm, "tools.head: cannot open '%s': %s",
                            path, strerror(errno));
        return -1;
    }

    NovaiStrBuf sb;
    novai_sb_init(&sb);

    char line[NOVAI_TL_LINE_MAX];
    nova_int_t count = 0;
    while (count < n && fgets(line, (int)sizeof(line), f) != NULL) {
        novai_sb_append(&sb, line, strlen(line));
        count++;
    }
    fclose(f);

    if (sb.data != NULL) {
        nova_vm_push_string(vm, sb.data, sb.len);
    } else {
        nova_vm_push_string(vm, "", 0);
    }
    novai_sb_free(&sb);
    return 1;
}

/* ============================================================
 * TOOL: tail — Last N lines
 * ============================================================ */

/**
 * tools.tail(path [, n]) → string
 *
 * Returns the last N lines of a file (default 10).
 */
static int nova_tools_tail(NovaVM *vm) {
    if (nova_lib_check_args(vm, 1) != 0) { return -1; }
    const char *path = nova_lib_check_string(vm, 0);
    if (path == NULL) { return -1; }

    nova_int_t n = 10;
    if (nova_vm_get_top(vm) >= 2) {
        (void)nova_lib_check_integer(vm, 1, &n);
    }

    FILE *f = fopen(path, "r");
    if (f == NULL) {
        nova_vm_raise_error(vm, "tools.tail: cannot open '%s': %s",
                            path, strerror(errno));
        return -1;
    }

    /* Collect all lines, then take last N */
    char **lines = NULL;
    nova_int_t total = 0;
    nova_int_t cap = 0;

    char buf[NOVAI_TL_LINE_MAX];
    while (fgets(buf, (int)sizeof(buf), f) != NULL) {
        if (total >= cap) {
            nova_int_t newcap = (cap == 0) ? 64 : cap * 2;
            char **p = (char **)realloc(lines,
                                        (size_t)newcap * sizeof(char *));
            if (p == NULL) { break; }
            lines = p;
            cap = newcap;
        }
        size_t blen = strlen(buf) + 1;
        lines[total] = (char *)malloc(blen);
        if (lines[total] != NULL) {
            memcpy(lines[total], buf, blen);
        }
        total++;
    }
    fclose(f);

    /* Build result from last N lines */
    NovaiStrBuf sb;
    novai_sb_init(&sb);

    nova_int_t start = (total > n) ? total - n : 0;
    for (nova_int_t i = start; i < total; i++) {
        if (lines[i] != NULL) {
            novai_sb_append(&sb, lines[i], strlen(lines[i]));
        }
    }

    /* Free lines */
    for (nova_int_t i = 0; i < total; i++) {
        free(lines[i]);
    }
    free(lines);

    if (sb.data != NULL) {
        nova_vm_push_string(vm, sb.data, sb.len);
    } else {
        nova_vm_push_string(vm, "", 0);
    }
    novai_sb_free(&sb);
    return 1;
}

/* ============================================================
 * TOOL: wc — Word, line, and character count
 * ============================================================ */

/**
 * tools.wc(path) → {lines = N, words = N, chars = N}
 *
 * Returns a table with line, word, and character counts.
 */
static int nova_tools_wc(NovaVM *vm) {
    if (nova_lib_check_args(vm, 1) != 0) { return -1; }
    const char *path = nova_lib_check_string(vm, 0);
    if (path == NULL) { return -1; }

    FILE *f = fopen(path, "r");
    if (f == NULL) {
        nova_vm_raise_error(vm, "tools.wc: cannot open '%s': %s",
                            path, strerror(errno));
        return -1;
    }

    nova_int_t lines = 0, words = 0, chars = 0;
    int in_word = 0;
    int c;

    while ((c = fgetc(f)) != EOF) {
        chars++;
        if (c == '\n') { lines++; }
        if (isspace(c)) {
            in_word = 0;
        } else if (!in_word) {
            in_word = 1;
            words++;
        }
    }
    fclose(f);

    /* Build result table */
    nova_vm_push_table(vm);
    NovaValue result = nova_vm_get(vm, nova_vm_get_top(vm) - 1);
    NovaTable *tbl = nova_as_table(result);

    NovaString *k;

    k = nova_string_new(vm, "lines", 5);
    if (k != NULL) {
        nova_table_set_str(vm, tbl, k, nova_value_integer(lines));
    }

    k = nova_string_new(vm, "words", 5);
    if (k != NULL) {
        nova_table_set_str(vm, tbl, k, nova_value_integer(words));
    }

    k = nova_string_new(vm, "chars", 5);
    if (k != NULL) {
        nova_table_set_str(vm, tbl, k, nova_value_integer(chars));
    }

    return 1;
}

/* ============================================================
 * TOOL: pwd — Print working directory
 * ============================================================ */

/**
 * tools.pwd() → string
 */
static int nova_tools_pwd(NovaVM *vm) {
    char cwd[NOVAI_TL_PATH_MAX];
    if (zorya_getcwd(cwd, sizeof(cwd)) == 0) {
        nova_vm_push_string(vm, cwd, strlen(cwd));
        return 1;
    }
    nova_vm_raise_error(vm, "tools.pwd: %s", strerror(errno));
    return -1;
}

/* ============================================================
 * TOOL: run — Execute command and capture stdout
 * ============================================================ */

/**
 * tools.run(command) → string, int
 *
 * Executes a shell command and returns its stdout output
 * and exit code.
 */
static int nova_tools_run(NovaVM *vm) {
    if (nova_lib_check_args(vm, 1) != 0) { return -1; }
    const char *cmd = nova_lib_check_string(vm, 0);
    if (cmd == NULL) { return -1; }

    FILE *fp = zorya_popen(cmd, "r");
    if (fp == NULL) {
        nova_vm_raise_error(vm, "tools.run: cannot execute '%s': %s",
                            cmd, strerror(errno));
        return -1;
    }

    NovaiStrBuf sb;
    novai_sb_init(&sb);

    char buf[NOVAI_TL_BUF_INIT];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) {
        novai_sb_append(&sb, buf, n);
    }

    int status = zorya_pclose(fp);
#ifdef _WIN32
    int exit_code = status;
#else
    int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
#endif

    /* Strip trailing newline */
    if (sb.data != NULL && sb.len > 0 && sb.data[sb.len - 1] == '\n') {
        sb.len--;
        sb.data[sb.len] = '\0';
    }

    if (sb.data != NULL) {
        nova_vm_push_string(vm, sb.data, sb.len);
    } else {
        nova_vm_push_string(vm, "", 0);
    }
    novai_sb_free(&sb);

    nova_vm_push_integer(vm, (nova_int_t)exit_code);
    return 2;
}

/* ============================================================
 * MODULE REGISTRATION
 * ============================================================ */

static const NovaLibReg nova_tools_lib[] = {
    {"cat",   nova_tools_cat},
    {"ls",    nova_tools_ls},
    {"tree",  nova_tools_tree},
    {"find",  nova_tools_find},
    {"grep",  nova_tools_grep},
    {"head",  nova_tools_head},
    {"tail",  nova_tools_tail},
    {"wc",    nova_tools_wc},
    {"pwd",   nova_tools_pwd},
    {"run",   nova_tools_run},
    {NULL,    NULL}
};

int nova_open_tools(NovaVM *vm) {
    nova_lib_register_module(vm, "tools", nova_tools_lib);
    return 0;
}
