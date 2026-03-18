/**
 * @file nova_lib_fs.c
 * @brief Nova Language - Filesystem Standard Library
 *
 * Cross-platform filesystem operations. Works identically on
 * Linux, macOS, and Windows — no shell commands required.
 *
 * Functions:
 *   fs.exists(path)           Check if path exists
 *   fs.isfile(path)           Check if path is a regular file
 *   fs.isdir(path)            Check if path is a directory
 *   fs.islink(path)           Check if path is a symbolic link
 *   fs.size(path)             File size in bytes
 *   fs.mtime(path)            Modification time (epoch seconds)
 *   fs.stat(path)             Full file metadata table
 *   fs.read(path)             Read entire file to string
 *   fs.write(path, data)      Write string to file (overwrite)
 *   fs.append(path, data)     Append string to file
 *   fs.lines(path)            Read file into table of lines
 *   fs.mkdir(path)            Create directory
 *   fs.mkdirs(path)           Create directory tree (recursive)
 *   fs.rmdir(path)            Remove empty directory
 *   fs.remove(path)           Remove file or empty directory
 *   fs.list(path)             List directory contents
 *   fs.walk(path)             Recursive directory listing
 *   fs.copy(src, dst)         Copy a file
 *   fs.move(src, dst)         Move/rename a file or directory
 *   fs.glob(pattern)          Glob pattern matching
 *   fs.find(dir, pattern)     Recursive search by name pattern
 *   fs.abspath(path)          Resolve to absolute path
 *   fs.realpath(path)         Resolve symlinks
 *   fs.join(...)              Join path segments
 *   fs.basename(path)         Filename component
 *   fs.dirname(path)          Directory component
 *   fs.ext(path)              File extension (with dot)
 *   fs.stem(path)             Filename without extension
 *   fs.normalize(path)        Clean up path separators
 *   fs.chmod(path, mode)      Change permissions (Unix)
 *   fs.touch(path)            Create empty file or update mtime
 *   fs.tempdir()              System temp directory
 *
 * Constants:
 *   fs.sep                    Path separator ("/" or "\")
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

/* Feature test macros — must precede all system headers.
 * _DEFAULT_SOURCE exposes realpath, lstat, etc. on glibc.
 * _POSIX_C_SOURCE is set globally in the Makefile.       */
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include "nova/nova_lib.h"
#include "nova/nova_vm.h"

#include <zorya/pal.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>

/* ============================================================
 * CROSS-PLATFORM HEADERS
 *
 * Portable calls (mkdir, rmdir, getcwd, chmod, path separator)
 * go through <zorya/pal.h>. The remaining platform-specific
 * items (stat types, directory iteration, link detection) stay
 * in the #ifdef until PAL covers them fully.
 * ============================================================ */

#ifdef _WIN32
    #define WIN32_LEAN_AND_MEAN
    #include <windows.h>
    #include <direct.h>
    #include <io.h>
    #include <sys/stat.h>
    #include <sys/types.h>
    #define nova_fs_stat      _stat
    #define nova_fs_stat_t    struct _stat
    #define S_ISREG(m) (((m) & _S_IFMT) == _S_IFREG)
    #define S_ISDIR(m) (((m) & _S_IFMT) == _S_IFDIR)
#else
    #include <unistd.h>
    #include <sys/stat.h>
    #include <sys/types.h>
    #include <dirent.h>
    #include <glob.h>
    #include <libgen.h>
    #include <utime.h>
    #define nova_fs_stat      stat
    #define nova_fs_stat_t    struct stat
#endif

/* ============================================================
 * INTERNAL: PATH SEPARATOR CHECK
 * ============================================================ */

static int novai_is_sep(char c) {
    return c == '/' || c == '\\';
}

/* ============================================================
 * FS.EXISTS
 * ============================================================ */

/**
 * @brief fs.exists(path) - Check if a path exists.
 */
static int nova_fs_exists(NovaVM *vm) {
    if (nova_lib_check_args(vm, 1) != 0) return -1;
    const char *path = nova_lib_check_string(vm, 0);
    if (path == NULL) return -1;

    nova_fs_stat_t st;
    nova_vm_push_bool(vm, nova_fs_stat(path, &st) == 0);
    return 1;
}

/* ============================================================
 * FS.ISFILE / FS.ISDIR / FS.ISLINK
 * ============================================================ */

static int nova_fs_isfile(NovaVM *vm) {
    if (nova_lib_check_args(vm, 1) != 0) return -1;
    const char *path = nova_lib_check_string(vm, 0);
    if (path == NULL) return -1;

    nova_fs_stat_t st;
    if (nova_fs_stat(path, &st) != 0) {
        nova_vm_push_bool(vm, 0);
        return 1;
    }
    nova_vm_push_bool(vm, S_ISREG(st.st_mode));
    return 1;
}

static int nova_fs_isdir(NovaVM *vm) {
    if (nova_lib_check_args(vm, 1) != 0) return -1;
    const char *path = nova_lib_check_string(vm, 0);
    if (path == NULL) return -1;

    nova_fs_stat_t st;
    if (nova_fs_stat(path, &st) != 0) {
        nova_vm_push_bool(vm, 0);
        return 1;
    }
    nova_vm_push_bool(vm, S_ISDIR(st.st_mode));
    return 1;
}

static int nova_fs_islink(NovaVM *vm) {
    if (nova_lib_check_args(vm, 1) != 0) return -1;
    const char *path = nova_lib_check_string(vm, 0);
    if (path == NULL) return -1;

#ifdef _WIN32
    DWORD attr = GetFileAttributesA(path);
    if (attr == INVALID_FILE_ATTRIBUTES) {
        nova_vm_push_bool(vm, 0);
    } else {
        nova_vm_push_bool(vm, (attr & FILE_ATTRIBUTE_REPARSE_POINT) != 0);
    }
#else
    struct stat st;
    if (lstat(path, &st) != 0) {
        nova_vm_push_bool(vm, 0);
    } else {
        nova_vm_push_bool(vm, S_ISLNK(st.st_mode));
    }
#endif
    return 1;
}

/* ============================================================
 * FS.SIZE / FS.MTIME / FS.STAT
 * ============================================================ */

static int nova_fs_size_func(NovaVM *vm) {
    if (nova_lib_check_args(vm, 1) != 0) return -1;
    const char *path = nova_lib_check_string(vm, 0);
    if (path == NULL) return -1;

    nova_fs_stat_t st;
    if (nova_fs_stat(path, &st) != 0) {
        nova_vm_push_nil(vm);
        nova_vm_push_string(vm, strerror(errno), strlen(strerror(errno)));
        return 2;
    }
    nova_vm_push_integer(vm, (nova_int_t)st.st_size);
    return 1;
}

static int nova_fs_mtime(NovaVM *vm) {
    if (nova_lib_check_args(vm, 1) != 0) return -1;
    const char *path = nova_lib_check_string(vm, 0);
    if (path == NULL) return -1;

    nova_fs_stat_t st;
    if (nova_fs_stat(path, &st) != 0) {
        nova_vm_push_nil(vm);
        nova_vm_push_string(vm, strerror(errno), strlen(strerror(errno)));
        return 2;
    }
    nova_vm_push_integer(vm, (nova_int_t)st.st_mtime);
    return 1;
}

/**
 * @brief fs.stat(path) - Full file metadata.
 *
 * Returns table: {size, mtime, atime, type, mode}
 * type is "file", "directory", "link", or "other".
 */
static int nova_fs_stat_func(NovaVM *vm) {
    if (nova_lib_check_args(vm, 1) != 0) return -1;
    const char *path = nova_lib_check_string(vm, 0);
    if (path == NULL) return -1;

    nova_fs_stat_t st;
    if (nova_fs_stat(path, &st) != 0) {
        nova_vm_push_nil(vm);
        nova_vm_push_string(vm, strerror(errno), strlen(strerror(errno)));
        return 2;
    }

    nova_vm_push_table(vm);
    int tidx = nova_vm_get_top(vm) - 1;

    nova_vm_push_integer(vm, (nova_int_t)st.st_size);
    nova_vm_set_field(vm, tidx, "size");

    nova_vm_push_integer(vm, (nova_int_t)st.st_mtime);
    nova_vm_set_field(vm, tidx, "mtime");

    nova_vm_push_integer(vm, (nova_int_t)st.st_atime);
    nova_vm_set_field(vm, tidx, "atime");

    nova_vm_push_integer(vm, (nova_int_t)st.st_mode);
    nova_vm_set_field(vm, tidx, "mode");

    const char *type_str = "other";
    if (S_ISREG(st.st_mode)) type_str = "file";
    else if (S_ISDIR(st.st_mode)) type_str = "directory";
#ifndef _WIN32
    else if (S_ISLNK(st.st_mode)) type_str = "link";
#endif
    nova_vm_push_string(vm, type_str, strlen(type_str));
    nova_vm_set_field(vm, tidx, "type");

    return 1;
}

/* ============================================================
 * FS.READ / FS.WRITE / FS.APPEND
 * ============================================================ */

/**
 * @brief fs.read(path) - Read entire file to string.
 *
 * Returns file contents as string, or nil + error.
 */
static int nova_fs_read(NovaVM *vm) {
    if (nova_lib_check_args(vm, 1) != 0) return -1;
    const char *path = nova_lib_check_string(vm, 0);
    if (path == NULL) return -1;

    FILE *fp = fopen(path, "rb");
    if (fp == NULL) {
        nova_vm_push_nil(vm);
        nova_vm_push_string(vm, strerror(errno), strlen(strerror(errno)));
        return 2;
    }

    /* Get file size */
    fseek(fp, 0, SEEK_END);
    long fsize = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (fsize < 0) {
        fclose(fp);
        nova_vm_push_nil(vm);
        const char *msg = "cannot determine file size";
        nova_vm_push_string(vm, msg, strlen(msg));
        return 2;
    }

    char *buf = (char *)malloc((size_t)fsize + 1);
    if (buf == NULL) {
        fclose(fp);
        nova_vm_push_nil(vm);
        const char *msg = "out of memory";
        nova_vm_push_string(vm, msg, strlen(msg));
        return 2;
    }

    size_t got = fread(buf, 1, (size_t)fsize, fp);
    fclose(fp);

    buf[got] = '\0';
    nova_vm_push_string(vm, buf, got);
    free(buf);
    return 1;
}

/**
 * @brief fs.write(path, data) - Write string to file (overwrite).
 */
static int nova_fs_write(NovaVM *vm) {
    if (nova_lib_check_args(vm, 2) != 0) return -1;
    const char *path = nova_lib_check_string(vm, 0);
    if (path == NULL) return -1;

    NovaValue dv = nova_vm_get(vm, 1);
    if (!nova_is_string(dv)) {
        nova_vm_raise_error(vm, "fs.write: expected string data");
        return -1;
    }
    NovaString *s = nova_as_string(dv);

    FILE *fp = fopen(path, "wb");
    if (fp == NULL) {
        nova_vm_push_nil(vm);
        nova_vm_push_string(vm, strerror(errno), strlen(strerror(errno)));
        return 2;
    }

    size_t written = fwrite(nova_str_data(s), 1, nova_str_len(s), fp);
    fclose(fp);

    if (written != nova_str_len(s)) {
        nova_vm_push_nil(vm);
        const char *msg = "write incomplete";
        nova_vm_push_string(vm, msg, strlen(msg));
        return 2;
    }

    nova_vm_push_bool(vm, 1);
    return 1;
}

/**
 * @brief fs.append(path, data) - Append string to file.
 */
static int nova_fs_append_func(NovaVM *vm) {
    if (nova_lib_check_args(vm, 2) != 0) return -1;
    const char *path = nova_lib_check_string(vm, 0);
    if (path == NULL) return -1;

    NovaValue dv = nova_vm_get(vm, 1);
    if (!nova_is_string(dv)) {
        nova_vm_raise_error(vm, "fs.append: expected string data");
        return -1;
    }
    NovaString *s = nova_as_string(dv);

    FILE *fp = fopen(path, "ab");
    if (fp == NULL) {
        nova_vm_push_nil(vm);
        nova_vm_push_string(vm, strerror(errno), strlen(strerror(errno)));
        return 2;
    }

    size_t written = fwrite(nova_str_data(s), 1, nova_str_len(s), fp);
    fclose(fp);

    if (written != nova_str_len(s)) {
        nova_vm_push_nil(vm);
        const char *msg = "append incomplete";
        nova_vm_push_string(vm, msg, strlen(msg));
        return 2;
    }

    nova_vm_push_bool(vm, 1);
    return 1;
}

/**
 * @brief fs.lines(path) - Read file into table of lines.
 *
 * Returns a 0-indexed table of strings (one per line).
 * Strips trailing newline/carriage-return from each line.
 */
static int nova_fs_lines(NovaVM *vm) {
    if (nova_lib_check_args(vm, 1) != 0) return -1;
    const char *path = nova_lib_check_string(vm, 0);
    if (path == NULL) return -1;

    FILE *fp = fopen(path, "r");
    if (fp == NULL) {
        nova_vm_push_nil(vm);
        nova_vm_push_string(vm, strerror(errno), strlen(strerror(errno)));
        return 2;
    }

    nova_vm_push_table(vm);
    NovaValue tval = nova_vm_get(vm, -1);
    if (!nova_is_table(tval)) {
        fclose(fp);
        return 1;
    }
    NovaTable *t = nova_as_table(tval);

    char buf[8192];
    nova_int_t idx = 0;

    while (fgets(buf, (int)sizeof(buf), fp) != NULL) {
        size_t len = strlen(buf);
        /* Strip trailing newline */
        while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r')) {
            buf[--len] = '\0';
        }
        NovaString *ns = nova_vm_intern_string(vm, buf, len);
        if (ns != NULL) {
            nova_table_raw_set_int(vm, t, idx, nova_value_string(ns));
        }
        idx++;
    }

    fclose(fp);
    return 1;
}

/* ============================================================
 * FS.MKDIR / FS.MKDIRS / FS.RMDIR / FS.REMOVE
 * ============================================================ */

static int nova_fs_mkdir_func(NovaVM *vm) {
    if (nova_lib_check_args(vm, 1) != 0) return -1;
    const char *path = nova_lib_check_string(vm, 0);
    if (path == NULL) return -1;

    if (zorya_mkdir(path) == 0) {
        nova_vm_push_bool(vm, 1);
        return 1;
    }
    nova_vm_push_nil(vm);
    nova_vm_push_string(vm, strerror(errno), strlen(strerror(errno)));
    return 2;
}

/**
 * @brief fs.mkdirs(path) - Create directory tree recursively.
 *
 * Like `mkdir -p`: creates all intermediate directories as needed.
 */
static int nova_fs_mkdirs(NovaVM *vm) {
    if (nova_lib_check_args(vm, 1) != 0) return -1;
    const char *path = nova_lib_check_string(vm, 0);
    if (path == NULL) return -1;

    char tmp[4096];
    size_t len = strlen(path);
    if (len >= sizeof(tmp)) {
        nova_vm_push_nil(vm);
        const char *msg = "path too long";
        nova_vm_push_string(vm, msg, strlen(msg));
        return 2;
    }
    memcpy(tmp, path, len + 1);

    /* Walk the path, creating directories as we go */
    for (size_t i = 1; i < len; i++) {
        if (novai_is_sep(tmp[i])) {
            tmp[i] = '\0';
            nova_fs_stat_t st;
            if (nova_fs_stat(tmp, &st) != 0) {
                if (zorya_mkdir(tmp) != 0 && errno != EEXIST) {
                    nova_vm_push_nil(vm);
                    nova_vm_push_string(vm, strerror(errno),
                                        strlen(strerror(errno)));
                    return 2;
                }
            }
            tmp[i] = zorya_path_sep();
        }
    }
    /* Create the final directory */
    nova_fs_stat_t st;
    if (nova_fs_stat(tmp, &st) != 0) {
        if (zorya_mkdir(tmp) != 0 && errno != EEXIST) {
            nova_vm_push_nil(vm);
            nova_vm_push_string(vm, strerror(errno),
                                strlen(strerror(errno)));
            return 2;
        }
    }

    nova_vm_push_bool(vm, 1);
    return 1;
}

static int nova_fs_rmdir_func(NovaVM *vm) {
    if (nova_lib_check_args(vm, 1) != 0) return -1;
    const char *path = nova_lib_check_string(vm, 0);
    if (path == NULL) return -1;

    if (zorya_rmdir(path) == 0) {
        nova_vm_push_bool(vm, 1);
        return 1;
    }
    nova_vm_push_nil(vm);
    nova_vm_push_string(vm, strerror(errno), strlen(strerror(errno)));
    return 2;
}

static int nova_fs_remove_func(NovaVM *vm) {
    if (nova_lib_check_args(vm, 1) != 0) return -1;
    const char *path = nova_lib_check_string(vm, 0);
    if (path == NULL) return -1;

    if (remove(path) == 0) {
        nova_vm_push_bool(vm, 1);
        return 1;
    }
    nova_vm_push_nil(vm);
    nova_vm_push_string(vm, strerror(errno), strlen(strerror(errno)));
    return 2;
}

/* ============================================================
 * FS.LIST (directory listing)
 * ============================================================ */

/**
 * @brief fs.list(path) - List directory contents.
 *
 * Returns a 0-indexed table of filenames (excludes "." and "..").
 * Directories have a trailing "/" appended.
 */
static int nova_fs_list(NovaVM *vm) {
    if (nova_lib_check_args(vm, 1) != 0) return -1;
    const char *path = nova_lib_check_string(vm, 0);
    if (path == NULL) return -1;

    nova_vm_push_table(vm);
    NovaValue tval = nova_vm_get(vm, -1);
    if (!nova_is_table(tval)) return 1;
    NovaTable *t = nova_as_table(tval);
    nova_int_t idx = 0;

#ifdef _WIN32
    char pattern[4096];
    snprintf(pattern, sizeof(pattern), "%s\\*", path);

    WIN32_FIND_DATAA fdata;
    HANDLE hFind = FindFirstFileA(pattern, &fdata);
    if (hFind == INVALID_HANDLE_VALUE) {
        return 1;
    }

    do {
        if (strcmp(fdata.cFileName, ".") == 0 ||
            strcmp(fdata.cFileName, "..") == 0) {
            continue;
        }

        char entry[4096];
        if (fdata.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            snprintf(entry, sizeof(entry), "%s/", fdata.cFileName);
        } else {
            snprintf(entry, sizeof(entry), "%s", fdata.cFileName);
        }

        NovaString *ns = nova_vm_intern_string(vm, entry, strlen(entry));
        if (ns != NULL) {
            nova_table_raw_set_int(vm, t, idx, nova_value_string(ns));
        }
        idx++;
    } while (FindNextFileA(hFind, &fdata));

    FindClose(hFind);
#else
    DIR *dp = opendir(path);
    if (dp == NULL) {
        return 1;
    }

    struct dirent *de;
    while ((de = readdir(dp)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 ||
            strcmp(de->d_name, "..") == 0) {
            continue;
        }

        char entry[4096];
        char fullpath[4096];
        snprintf(fullpath, sizeof(fullpath), "%s/%s", path, de->d_name);
        nova_fs_stat_t st;
        if (nova_fs_stat(fullpath, &st) == 0 && S_ISDIR(st.st_mode)) {
            snprintf(entry, sizeof(entry), "%s/", de->d_name);
        } else {
            snprintf(entry, sizeof(entry), "%s", de->d_name);
        }

        NovaString *ns = nova_vm_intern_string(vm, entry, strlen(entry));
        if (ns != NULL) {
            nova_table_raw_set_int(vm, t, idx, nova_value_string(ns));
        }
        idx++;
    }

    closedir(dp);
#endif

    return 1;
}

/* ============================================================
 * FS.WALK (recursive directory listing)
 * ============================================================ */

/**
 * @brief Internal recursive walker.
 *
 * Appends relative paths to the result table on the VM stack.
 * Uses VM stack for GC safety — the result table is at tidx.
 */
static void novai_fs_walk_recurse(NovaVM *vm, NovaTable *t,
                                   const char *base,
                                   const char *prefix,
                                   nova_int_t *idx) {
#ifdef _WIN32
    char pattern[4096];
    snprintf(pattern, sizeof(pattern), "%s\\*", base);

    WIN32_FIND_DATAA fdata;
    HANDLE hFind = FindFirstFileA(pattern, &fdata);
    if (hFind == INVALID_HANDLE_VALUE) return;

    do {
        if (strcmp(fdata.cFileName, ".") == 0 ||
            strcmp(fdata.cFileName, "..") == 0) {
            continue;
        }

        char relpath[4096];
        if (prefix[0] != '\0') {
            snprintf(relpath, sizeof(relpath), "%s/%s",
                     prefix, fdata.cFileName);
        } else {
            snprintf(relpath, sizeof(relpath), "%s", fdata.cFileName);
        }

        NovaString *ns = nova_vm_intern_string(vm, relpath, strlen(relpath));
        if (ns != NULL) {
            nova_table_raw_set_int(vm, t, *idx, nova_value_string(ns));
        }
        (*idx)++;

        if (fdata.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            char subdir[4096];
            snprintf(subdir, sizeof(subdir), "%s\\%s",
                     base, fdata.cFileName);
            novai_fs_walk_recurse(vm, t, subdir, relpath, idx);
        }
    } while (FindNextFileA(hFind, &fdata));

    FindClose(hFind);
#else
    DIR *dp = opendir(base);
    if (dp == NULL) return;

    struct dirent *de;
    while ((de = readdir(dp)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 ||
            strcmp(de->d_name, "..") == 0) {
            continue;
        }

        char relpath[4096];
        if (prefix[0] != '\0') {
            snprintf(relpath, sizeof(relpath), "%s/%s",
                     prefix, de->d_name);
        } else {
            snprintf(relpath, sizeof(relpath), "%s", de->d_name);
        }

        NovaString *ns = nova_vm_intern_string(vm, relpath, strlen(relpath));
        if (ns != NULL) {
            nova_table_raw_set_int(vm, t, *idx, nova_value_string(ns));
        }
        (*idx)++;

        /* Recurse into subdirectories */
        char fullpath[4096];
        snprintf(fullpath, sizeof(fullpath), "%s/%s",
                 base, de->d_name);
        nova_fs_stat_t st;
        if (nova_fs_stat(fullpath, &st) == 0 && S_ISDIR(st.st_mode)) {
            novai_fs_walk_recurse(vm, t, fullpath, relpath, idx);
        }
    }

    closedir(dp);
#endif
}

/**
 * @brief fs.walk(path) - Recursive directory listing.
 *
 * Returns a 0-indexed table of all files/dirs under path.
 * Paths are relative to the given root directory.
 */
static int nova_fs_walk_func(NovaVM *vm) {
    if (nova_lib_check_args(vm, 1) != 0) return -1;
    const char *path = nova_lib_check_string(vm, 0);
    if (path == NULL) return -1;

    nova_vm_push_table(vm);
    NovaValue tval = nova_vm_get(vm, -1);
    if (!nova_is_table(tval)) return 1;
    NovaTable *t = nova_as_table(tval);
    nova_int_t idx = 0;

    novai_fs_walk_recurse(vm, t, path, "", &idx);

    return 1;
}

/* ============================================================
 * FS.COPY
 * ============================================================ */

/**
 * @brief fs.copy(src, dst) - Copy a file.
 *
 * Reads src and writes to dst. Binary-safe.
 */
static int nova_fs_copy(NovaVM *vm) {
    if (nova_lib_check_args(vm, 2) != 0) return -1;
    const char *src = nova_lib_check_string(vm, 0);
    const char *dst = nova_lib_check_string(vm, 1);
    if (src == NULL || dst == NULL) return -1;

    FILE *fin = fopen(src, "rb");
    if (fin == NULL) {
        nova_vm_push_nil(vm);
        nova_vm_push_string(vm, strerror(errno), strlen(strerror(errno)));
        return 2;
    }

    FILE *fout = fopen(dst, "wb");
    if (fout == NULL) {
        fclose(fin);
        nova_vm_push_nil(vm);
        nova_vm_push_string(vm, strerror(errno), strlen(strerror(errno)));
        return 2;
    }

    char buf[8192];
    size_t n;
    int ok = 1;

    while ((n = fread(buf, 1, sizeof(buf), fin)) > 0) {
        if (fwrite(buf, 1, n, fout) != n) {
            ok = 0;
            break;
        }
    }

    fclose(fin);
    fclose(fout);

    if (!ok) {
        nova_vm_push_nil(vm);
        const char *msg = "copy failed during write";
        nova_vm_push_string(vm, msg, strlen(msg));
        return 2;
    }

    nova_vm_push_bool(vm, 1);
    return 1;
}

/* ============================================================
 * FS.MOVE
 * ============================================================ */

/**
 * @brief fs.move(src, dst) - Move/rename file or directory.
 *
 * Tries rename() first (atomic on same filesystem).
 * Falls back to copy + remove for cross-filesystem moves.
 */
static int nova_fs_move(NovaVM *vm) {
    if (nova_lib_check_args(vm, 2) != 0) return -1;
    const char *src = nova_lib_check_string(vm, 0);
    const char *dst = nova_lib_check_string(vm, 1);
    if (src == NULL || dst == NULL) return -1;

    if (rename(src, dst) == 0) {
        nova_vm_push_bool(vm, 1);
        return 1;
    }

    /* If rename failed (cross-device), try copy + remove */
    if (errno == EXDEV) {
        /* Copy the file */
        FILE *fin = fopen(src, "rb");
        if (fin == NULL) {
            nova_vm_push_nil(vm);
            nova_vm_push_string(vm, strerror(errno),
                                strlen(strerror(errno)));
            return 2;
        }
        FILE *fout = fopen(dst, "wb");
        if (fout == NULL) {
            fclose(fin);
            nova_vm_push_nil(vm);
            nova_vm_push_string(vm, strerror(errno),
                                strlen(strerror(errno)));
            return 2;
        }

        char buf[8192];
        size_t n;
        int ok = 1;
        while ((n = fread(buf, 1, sizeof(buf), fin)) > 0) {
            if (fwrite(buf, 1, n, fout) != n) {
                ok = 0;
                break;
            }
        }
        fclose(fin);
        fclose(fout);

        if (!ok) {
            remove(dst);
            nova_vm_push_nil(vm);
            const char *msg = "move failed during copy";
            nova_vm_push_string(vm, msg, strlen(msg));
            return 2;
        }

        if (remove(src) != 0) {
            nova_vm_push_nil(vm);
            nova_vm_push_string(vm, strerror(errno),
                                strlen(strerror(errno)));
            return 2;
        }

        nova_vm_push_bool(vm, 1);
        return 1;
    }

    nova_vm_push_nil(vm);
    nova_vm_push_string(vm, strerror(errno), strlen(strerror(errno)));
    return 2;
}

/* ============================================================
 * FS.GLOB (pattern matching on filesystem)
 * ============================================================ */

/**
 * @brief Internal: check if name matches a simple glob pattern.
 *
 * Supports: * (any chars), ? (single char).
 * Does NOT support character classes [abc] or {a,b} for simplicity.
 * This is used on Windows where POSIX glob() is unavailable,
 * and also used by fs.find for name-only matching.
 */
static int novai_glob_match(const char *pattern, const char *name) {
    const char *p = pattern;
    const char *n = name;
    const char *star_p = NULL;
    const char *star_n = NULL;

    while (*n != '\0') {
        if (*p == '*') {
            star_p = p++;
            star_n = n;
        } else if (*p == '?' || *p == *n) {
            p++;
            n++;
        } else if (star_p != NULL) {
            p = star_p + 1;
            n = ++star_n;
        } else {
            return 0;
        }
    }
    while (*p == '*') p++;
    return *p == '\0';
}

/**
 * @brief fs.glob(pattern) - Find files matching a glob pattern.
 *
 * Pattern can include directory components: "src/ *.c", "** / *.n"
 * On POSIX, uses the system glob() function.
 * On Windows, uses FindFirstFile with manual pattern matching.
 *
 * Returns a 0-indexed table of matching paths.
 */
static int nova_fs_glob_func(NovaVM *vm) {
    if (nova_lib_check_args(vm, 1) != 0) return -1;
    const char *pattern = nova_lib_check_string(vm, 0);
    if (pattern == NULL) return -1;

    nova_vm_push_table(vm);
    NovaValue tval = nova_vm_get(vm, -1);
    if (!nova_is_table(tval)) return 1;
    NovaTable *t = nova_as_table(tval);
    nova_int_t idx = 0;

#ifdef _WIN32
    /* On Windows: extract directory and file pattern */
    char dir[4096] = ".";
    char fpat[256] = "*";

    /* Find last separator */
    const char *last_sep = NULL;
    for (const char *c = pattern; *c; c++) {
        if (*c == '/' || *c == '\\') last_sep = c;
    }

    if (last_sep != NULL) {
        size_t dlen = (size_t)(last_sep - pattern);
        if (dlen >= sizeof(dir)) dlen = sizeof(dir) - 1;
        memcpy(dir, pattern, dlen);
        dir[dlen] = '\0';
        snprintf(fpat, sizeof(fpat), "%s", last_sep + 1);
    } else {
        snprintf(fpat, sizeof(fpat), "%s", pattern);
    }

    char search[4096];
    snprintf(search, sizeof(search), "%s\\*", dir);

    WIN32_FIND_DATAA fdata;
    HANDLE hFind = FindFirstFileA(search, &fdata);
    if (hFind == INVALID_HANDLE_VALUE) return 1;

    do {
        if (strcmp(fdata.cFileName, ".") == 0 ||
            strcmp(fdata.cFileName, "..") == 0) {
            continue;
        }
        if (novai_glob_match(fpat, fdata.cFileName)) {
            char full[4096];
            snprintf(full, sizeof(full), "%s/%s", dir, fdata.cFileName);
            NovaString *ns = nova_vm_intern_string(vm, full, strlen(full));
            if (ns != NULL) {
                nova_table_raw_set_int(vm, t, idx, nova_value_string(ns));
            }
            idx++;
        }
    } while (FindNextFileA(hFind, &fdata));

    FindClose(hFind);
#else
    glob_t globbuf;
    int flags = GLOB_NOSORT;
#ifdef GLOB_TILDE
    flags |= GLOB_TILDE;
#endif

    int rc = glob(pattern, flags, NULL, &globbuf);
    if (rc == 0) {
        for (size_t i = 0; i < globbuf.gl_pathc; i++) {
            NovaString *ns = nova_vm_intern_string(vm,
                globbuf.gl_pathv[i],
                strlen(globbuf.gl_pathv[i]));
            if (ns != NULL) {
                nova_table_raw_set_int(vm, t, idx, nova_value_string(ns));
            }
            idx++;
        }
        globfree(&globbuf);
    }
#endif

    return 1;
}

/* ============================================================
 * FS.FIND (recursive search by name pattern)
 * ============================================================ */

/**
 * @brief Internal: recursive find with glob name matching.
 */
static void novai_fs_find_recurse(NovaVM *vm, NovaTable *t,
                                   const char *base,
                                   const char *prefix,
                                   const char *pattern,
                                   nova_int_t *idx) {
#ifdef _WIN32
    char search[4096];
    snprintf(search, sizeof(search), "%s\\*", base);

    WIN32_FIND_DATAA fdata;
    HANDLE hFind = FindFirstFileA(search, &fdata);
    if (hFind == INVALID_HANDLE_VALUE) return;

    do {
        if (strcmp(fdata.cFileName, ".") == 0 ||
            strcmp(fdata.cFileName, "..") == 0) {
            continue;
        }

        char relpath[4096];
        if (prefix[0] != '\0') {
            snprintf(relpath, sizeof(relpath), "%s/%s",
                     prefix, fdata.cFileName);
        } else {
            snprintf(relpath, sizeof(relpath), "%s", fdata.cFileName);
        }

        /* Check if name matches pattern */
        if (novai_glob_match(pattern, fdata.cFileName)) {
            NovaString *ns = nova_vm_intern_string(vm, relpath,
                                                    strlen(relpath));
            if (ns != NULL) {
                nova_table_raw_set_int(vm, t, *idx,
                                       nova_value_string(ns));
            }
            (*idx)++;
        }

        /* Recurse into subdirectories */
        if (fdata.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            char subdir[4096];
            snprintf(subdir, sizeof(subdir), "%s\\%s",
                     base, fdata.cFileName);
            novai_fs_find_recurse(vm, t, subdir, relpath,
                                  pattern, idx);
        }
    } while (FindNextFileA(hFind, &fdata));

    FindClose(hFind);
#else
    DIR *dp = opendir(base);
    if (dp == NULL) return;

    struct dirent *de;
    while ((de = readdir(dp)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 ||
            strcmp(de->d_name, "..") == 0) {
            continue;
        }

        char relpath[4096];
        if (prefix[0] != '\0') {
            snprintf(relpath, sizeof(relpath), "%s/%s",
                     prefix, de->d_name);
        } else {
            snprintf(relpath, sizeof(relpath), "%s", de->d_name);
        }

        /* Check if name matches pattern */
        if (novai_glob_match(pattern, de->d_name)) {
            NovaString *ns = nova_vm_intern_string(vm, relpath,
                                                    strlen(relpath));
            if (ns != NULL) {
                nova_table_raw_set_int(vm, t, *idx,
                                       nova_value_string(ns));
            }
            (*idx)++;
        }

        /* Recurse into subdirectories */
        char fullpath[4096];
        snprintf(fullpath, sizeof(fullpath), "%s/%s",
                 base, de->d_name);
        nova_fs_stat_t st;
        if (nova_fs_stat(fullpath, &st) == 0 && S_ISDIR(st.st_mode)) {
            novai_fs_find_recurse(vm, t, fullpath, relpath,
                                  pattern, idx);
        }
    }

    closedir(dp);
#endif
}

/**
 * @brief fs.find(dir, pattern) - Recursive search by filename pattern.
 *
 * Searches directory tree for files matching the glob pattern.
 * Pattern matches against the filename only (not the full path).
 * Returns a 0-indexed table of relative paths.
 *
 * Example: fs.find("src", "*.c") → {"nova_vm.c", "zorya/nxh.c", ...}
 */
static int nova_fs_find_func(NovaVM *vm) {
    if (nova_lib_check_args(vm, 2) != 0) return -1;
    const char *dir = nova_lib_check_string(vm, 0);
    const char *pattern = nova_lib_check_string(vm, 1);
    if (dir == NULL || pattern == NULL) return -1;

    nova_vm_push_table(vm);
    NovaValue tval = nova_vm_get(vm, -1);
    if (!nova_is_table(tval)) return 1;
    NovaTable *t = nova_as_table(tval);
    nova_int_t idx = 0;

    novai_fs_find_recurse(vm, t, dir, "", pattern, &idx);

    return 1;
}

/* ============================================================
 * PATH UTILITIES: join, basename, dirname, ext, stem, abspath,
 *                 realpath, normalize
 * ============================================================ */

/**
 * @brief fs.join(...) - Join path segments with separator.
 *
 * fs.join("src", "nova", "vm.c") → "src/nova/vm.c"
 */
static int nova_fs_join(NovaVM *vm) {
    int nargs = nova_vm_get_top(vm);
    if (nargs == 0) {
        nova_vm_push_string(vm, "", 0);
        return 1;
    }

    char result[4096];
    size_t pos = 0;

    for (int i = 0; i < nargs; i++) {
        NovaValue v = nova_vm_get(vm, i);
        if (!nova_is_string(v)) continue;

        const char *seg = nova_str_data(nova_as_string(v));
        size_t slen = nova_str_len(nova_as_string(v));

        if (slen == 0) continue;

        /* If segment is absolute, it replaces everything */
        if (novai_is_sep(seg[0])
#ifdef _WIN32
            || (slen >= 2 && seg[1] == ':')
#endif
        ) {
            memcpy(result, seg, slen);
            pos = slen;
            continue;
        }

        /* Add separator if needed */
        if (pos > 0 && !novai_is_sep(result[pos - 1])) {
            if (pos < sizeof(result) - 1) {
                result[pos++] = zorya_path_sep();
            }
        }

        /* Append segment */
        size_t copy = slen;
        if (pos + copy >= sizeof(result)) {
            copy = sizeof(result) - pos - 1;
        }
        memcpy(result + pos, seg, copy);
        pos += copy;
    }

    result[pos] = '\0';
    nova_vm_push_string(vm, result, pos);
    return 1;
}

/**
 * @brief fs.basename(path) - Get filename component.
 *
 * fs.basename("/home/user/file.txt") → "file.txt"
 */
static int nova_fs_basename_func(NovaVM *vm) {
    if (nova_lib_check_args(vm, 1) != 0) return -1;
    const char *path = nova_lib_check_string(vm, 0);
    if (path == NULL) return -1;

    size_t len = strlen(path);
    /* Skip trailing separators */
    while (len > 1 && novai_is_sep(path[len - 1])) len--;

    /* Find last separator */
    size_t i = len;
    while (i > 0 && !novai_is_sep(path[i - 1])) i--;

    nova_vm_push_string(vm, path + i, len - i);
    return 1;
}

/**
 * @brief fs.dirname(path) - Get directory component.
 *
 * fs.dirname("/home/user/file.txt") → "/home/user"
 */
static int nova_fs_dirname_func(NovaVM *vm) {
    if (nova_lib_check_args(vm, 1) != 0) return -1;
    const char *path = nova_lib_check_string(vm, 0);
    if (path == NULL) return -1;

    size_t len = strlen(path);
    /* Skip trailing separators */
    while (len > 1 && novai_is_sep(path[len - 1])) len--;

    /* Find last separator */
    size_t i = len;
    while (i > 0 && !novai_is_sep(path[i - 1])) i--;

    if (i == 0) {
        nova_vm_push_string(vm, ".", 1);
    } else if (i == 1) {
        /* Root directory */
        char sep[2] = { path[0], '\0' };
        nova_vm_push_string(vm, sep, 1);
    } else {
        nova_vm_push_string(vm, path, i - 1);
    }
    return 1;
}

/**
 * @brief fs.ext(path) - Get file extension (with dot).
 *
 * fs.ext("file.tar.gz") → ".gz"
 * fs.ext("Makefile") → ""
 */
static int nova_fs_ext_func(NovaVM *vm) {
    if (nova_lib_check_args(vm, 1) != 0) return -1;
    const char *path = nova_lib_check_string(vm, 0);
    if (path == NULL) return -1;

    /* Find basename first */
    size_t len = strlen(path);
    size_t base = len;
    while (base > 0 && !novai_is_sep(path[base - 1])) base--;

    /* Find last dot in basename */
    const char *dot = NULL;
    for (size_t i = len; i > base; i--) {
        if (path[i - 1] == '.') {
            dot = path + i - 1;
            break;
        }
    }

    if (dot == NULL || dot == path + base) {
        nova_vm_push_string(vm, "", 0);
    } else {
        nova_vm_push_string(vm, dot, len - (size_t)(dot - path));
    }
    return 1;
}

/**
 * @brief fs.stem(path) - Filename without extension.
 *
 * fs.stem("/path/file.txt") → "file"
 * fs.stem("archive.tar.gz") → "archive.tar"
 */
static int nova_fs_stem_func(NovaVM *vm) {
    if (nova_lib_check_args(vm, 1) != 0) return -1;
    const char *path = nova_lib_check_string(vm, 0);
    if (path == NULL) return -1;

    /* Get basename range */
    size_t len = strlen(path);
    while (len > 1 && novai_is_sep(path[len - 1])) len--;
    size_t base = len;
    while (base > 0 && !novai_is_sep(path[base - 1])) base--;

    /* Find last dot in basename */
    size_t dot = len;
    for (size_t i = len; i > base; i--) {
        if (path[i - 1] == '.') {
            dot = i - 1;
            break;
        }
    }

    if (dot == base) dot = len;  /* No extension */
    nova_vm_push_string(vm, path + base, dot - base);
    return 1;
}

/**
 * @brief fs.abspath(path) - Resolve to absolute path.
 */
static int nova_fs_abspath(NovaVM *vm) {
    if (nova_lib_check_args(vm, 1) != 0) return -1;
    const char *path = nova_lib_check_string(vm, 0);
    if (path == NULL) return -1;

    /* If already absolute, return as-is */
    if (novai_is_sep(path[0])
#ifdef _WIN32
        || (strlen(path) >= 2 && path[1] == ':')
#endif
    ) {
        nova_vm_push_string(vm, path, strlen(path));
        return 1;
    }

    /* Prepend cwd */
    char cwd[4096];
    if (zorya_getcwd(cwd, sizeof(cwd)) != 0) {
        nova_vm_push_string(vm, path, strlen(path));
        return 1;
    }

    char result[4096 + 4096 + 2];
    snprintf(result, sizeof(result), "%s%c%s", cwd, zorya_path_sep(), path);
    nova_vm_push_string(vm, result, strlen(result));
    return 1;
}

/**
 * @brief fs.realpath(path) - Resolve symlinks to canonical path.
 */
static int nova_fs_realpath_func(NovaVM *vm) {
    if (nova_lib_check_args(vm, 1) != 0) return -1;
    const char *path = nova_lib_check_string(vm, 0);
    if (path == NULL) return -1;

#ifdef _WIN32
    char resolved[MAX_PATH];
    DWORD len = GetFullPathNameA(path, MAX_PATH, resolved, NULL);
    if (len == 0 || len >= MAX_PATH) {
        nova_vm_push_nil(vm);
        return 1;
    }
    nova_vm_push_string(vm, resolved, (size_t)len);
#else
    char *resolved = realpath(path, NULL);
    if (resolved == NULL) {
        nova_vm_push_nil(vm);
        nova_vm_push_string(vm, strerror(errno), strlen(strerror(errno)));
        return 2;
    }
    nova_vm_push_string(vm, resolved, strlen(resolved));
    free(resolved);
#endif
    return 1;
}

/**
 * @brief fs.normalize(path) - Clean up path separators.
 *
 * Converts all separators to the platform default.
 * Removes duplicate separators. Does NOT resolve . or ..
 */
static int nova_fs_normalize(NovaVM *vm) {
    if (nova_lib_check_args(vm, 1) != 0) return -1;
    const char *path = nova_lib_check_string(vm, 0);
    if (path == NULL) return -1;

    char result[4096];
    size_t pos = 0;
    int prev_sep = 0;

    for (size_t i = 0; path[i] != '\0' && pos < sizeof(result) - 1; i++) {
        if (novai_is_sep(path[i])) {
            if (!prev_sep) {
                result[pos++] = zorya_path_sep();
                prev_sep = 1;
            }
        } else {
            result[pos++] = path[i];
            prev_sep = 0;
        }
    }

    /* Remove trailing separator (unless root) */
    if (pos > 1 && result[pos - 1] == zorya_path_sep()) {
        pos--;
    }

    result[pos] = '\0';
    nova_vm_push_string(vm, result, pos);
    return 1;
}

/* ============================================================
 * FS.CHMOD (Unix permissions)
 * ============================================================ */

/**
 * @brief fs.chmod(path, mode) - Change file permissions.
 *
 * Mode is an integer (e.g., 0755 = 493 decimal).
 * On Windows, this is limited — only read-only flag.
 */
static int nova_fs_chmod_func(NovaVM *vm) {
    if (nova_lib_check_args(vm, 2) != 0) return -1;
    const char *path = nova_lib_check_string(vm, 0);
    if (path == NULL) return -1;

    nova_int_t mode = 0;
    if (!nova_lib_check_integer(vm, 1, &mode)) return -1;

    if (zorya_chmod(path, (int)mode) == 0) {
        nova_vm_push_bool(vm, 1);
    } else {
        nova_vm_push_nil(vm);
        nova_vm_push_string(vm, strerror(errno), strlen(strerror(errno)));
        return 2;
    }
    return 1;
}

/* ============================================================
 * FS.TOUCH
 * ============================================================ */

/**
 * @brief fs.touch(path) - Create empty file or update mtime.
 *
 * If the file doesn't exist, creates it.
 * If it does exist, updates the modification time to now.
 */
static int nova_fs_touch(NovaVM *vm) {
    if (nova_lib_check_args(vm, 1) != 0) return -1;
    const char *path = nova_lib_check_string(vm, 0);
    if (path == NULL) return -1;

    /* Try to update time first (file exists) */
#ifdef _WIN32
    HANDLE hFile = CreateFileA(path, GENERIC_WRITE,
                               FILE_SHARE_READ | FILE_SHARE_WRITE,
                               NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                               NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        nova_vm_push_nil(vm);
        const char *msg = "cannot touch file";
        nova_vm_push_string(vm, msg, strlen(msg));
        return 2;
    }
    FILETIME ft;
    SYSTEMTIME st;
    GetSystemTime(&st);
    SystemTimeToFileTime(&st, &ft);
    SetFileTime(hFile, NULL, NULL, &ft);
    CloseHandle(hFile);
#else
    /* Try utime first (updates mtime if file exists) */
    if (utime(path, NULL) != 0) {
        /* File doesn't exist — create it */
        FILE *fp = fopen(path, "a");
        if (fp == NULL) {
            nova_vm_push_nil(vm);
            nova_vm_push_string(vm, strerror(errno),
                                strlen(strerror(errno)));
            return 2;
        }
        fclose(fp);
    }
#endif

    nova_vm_push_bool(vm, 1);
    return 1;
}

/* ============================================================
 * FS.TEMPDIR
 * ============================================================ */

static int nova_fs_tempdir(NovaVM *vm) {
#ifdef _WIN32
    char buf[MAX_PATH + 1];
    DWORD len = GetTempPathA(MAX_PATH + 1, buf);
    if (len > 0 && len <= MAX_PATH) {
        if (len > 1 && buf[len - 1] == '\\') {
            buf[len - 1] = '\0';
            len--;
        }
        nova_vm_push_string(vm, buf, (size_t)len);
    } else {
        nova_vm_push_string(vm, "C:\\Temp", 7);
    }
#else
    const char *tmp = getenv("TMPDIR");
    if (tmp == NULL) tmp = getenv("TMP");
    if (tmp == NULL) tmp = getenv("TEMP");
    if (tmp == NULL) tmp = "/tmp";
    nova_vm_push_string(vm, tmp, strlen(tmp));
#endif
    return 1;
}

/* ============================================================
 * REGISTRATION TABLE
 * ============================================================ */

static const NovaLibReg nova_fs_lib[] = {
    /* Existence / type checks */
    {"exists",     nova_fs_exists},
    {"isfile",     nova_fs_isfile},
    {"isdir",      nova_fs_isdir},
    {"islink",     nova_fs_islink},

    /* File metadata */
    {"size",       nova_fs_size_func},
    {"mtime",      nova_fs_mtime},
    {"stat",       nova_fs_stat_func},

    /* Read/write */
    {"read",       nova_fs_read},
    {"write",      nova_fs_write},
    {"append",     nova_fs_append_func},
    {"lines",      nova_fs_lines},

    /* Directory operations */
    {"mkdir",      nova_fs_mkdir_func},
    {"mkdirs",     nova_fs_mkdirs},
    {"rmdir",      nova_fs_rmdir_func},
    {"remove",     nova_fs_remove_func},
    {"list",       nova_fs_list},
    {"walk",       nova_fs_walk_func},

    /* File operations */
    {"copy",       nova_fs_copy},
    {"move",       nova_fs_move},
    {"touch",      nova_fs_touch},
    {"chmod",      nova_fs_chmod_func},

    /* Search */
    {"glob",       nova_fs_glob_func},
    {"find",       nova_fs_find_func},

    /* Path utilities */
    {"join",       nova_fs_join},
    {"basename",   nova_fs_basename_func},
    {"dirname",    nova_fs_dirname_func},
    {"ext",        nova_fs_ext_func},
    {"stem",       nova_fs_stem_func},
    {"abspath",    nova_fs_abspath},
    {"realpath",   nova_fs_realpath_func},
    {"normalize",  nova_fs_normalize},
    {"tempdir",    nova_fs_tempdir},

    {NULL,         NULL}
};

/* ============================================================
 * MODULE OPENER
 * ============================================================ */

int nova_open_fs(NovaVM *vm) {
    if (vm == NULL) {
        return -1;
    }

    nova_lib_register_module(vm, "fs", nova_fs_lib);

    /* Set fs.sep — path separator constant */
    NovaValue fs_tbl = nova_vm_get_global(vm, "fs");
    if (nova_is_table(fs_tbl)) {
        nova_vm_push_value(vm, fs_tbl);
        int tidx = nova_vm_get_top(vm) - 1;

        char sep[2] = { zorya_path_sep(), '\0' };
        nova_vm_push_string(vm, sep, 1);
        nova_vm_set_field(vm, tidx, "sep");

        nova_vm_set_top(vm, tidx);
    }

    return 0;
}
