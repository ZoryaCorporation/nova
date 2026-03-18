/**
 * @file pal_posix.c
 * @brief ZORYA PAL - POSIX Implementation
 *
 * Full implementation of the ZORYA PAL API for POSIX-compliant systems:
 * Linux, macOS, FreeBSD, OpenBSD, NetBSD, Raspberry Pi OS, etc.
 *
 * This file freely uses POSIX headers and APIs. It is the ONLY place
 * in the codebase that should include <unistd.h>, <sys/stat.h>,
 * <dirent.h>, etc. Everything else calls the zorya_* wrappers.
 *
 * @author Anthony Taliento
 * @date 2026-06-28
 * @version 1.0.0
 *
 * @copyright Copyright (c) 2026 Zorya Corporation
 * @license Apache-2.0
 *
 * ZORYA-C COMPLIANCE: v2.0.0 (Strict Mode)
 */

/* Must come before ANY includes to enable POSIX.1-2008 */
#define _POSIX_C_SOURCE 200809L

#include <zorya/pal.h>

#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>

#ifdef __APPLE__
    #include <mach/mach_time.h>
#endif

/* dlopen/dlsym/dlclose */
#include <dlfcn.h>

/* ============================================================
 * TERMINAL / FILE DESCRIPTORS
 * ============================================================ */

int zorya_isatty(int fd) {
    return isatty(fd) ? 1 : 0;
}

int zorya_fileno(FILE *fp) {
    if (fp == NULL) return -1;
    return fileno(fp);
}

int zorya_dup(int fd) {
    return dup(fd);
}

int zorya_dup2(int oldfd, int newfd) {
    return dup2(oldfd, newfd);
}

int zorya_fdclose(int fd) {
    return close(fd);
}

/* ============================================================
 * FILESYSTEM
 * ============================================================ */

int zorya_file_exists(const char *path) {
    if (path == NULL) return 0;
    struct stat st;
    return (stat(path, &st) == 0) ? 1 : 0;
}

int zorya_stat(const char *path, ZoryaStat *st) {
    if (path == NULL || st == NULL) return -1;

    struct stat ps;
    if (stat(path, &ps) != 0) return -1;

    st->size    = (uint64_t)ps.st_size;
    st->mtime   = (uint64_t)ps.st_mtime;
    st->is_file = S_ISREG(ps.st_mode) ? 1 : 0;
    st->is_dir  = S_ISDIR(ps.st_mode) ? 1 : 0;
    st->is_link = S_ISLNK(ps.st_mode) ? 1 : 0;
    st->mode    = (int)(ps.st_mode & 07777);
    return 0;
}

int zorya_mkdir(const char *path) {
    if (path == NULL) return -1;
    return mkdir(path, 0755);
}

int zorya_rmdir(const char *path) {
    if (path == NULL) return -1;
    return rmdir(path);
}

int zorya_remove(const char *path) {
    if (path == NULL) return -1;
    return remove(path);
}

int zorya_rename(const char *src, const char *dst) {
    if (src == NULL || dst == NULL) return -1;
    return rename(src, dst);
}

int zorya_chmod(const char *path, int mode) {
    if (path == NULL) return -1;
    return chmod(path, (mode_t)mode);
}

int zorya_is_file(const char *path) {
    if (path == NULL) return 0;
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    return S_ISREG(st.st_mode) ? 1 : 0;
}

int zorya_is_dir(const char *path) {
    if (path == NULL) return 0;
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    return S_ISDIR(st.st_mode) ? 1 : 0;
}

/* ============================================================
 * PATH / WORKING DIRECTORY
 * ============================================================ */

int zorya_getcwd(char *buf, size_t len) {
    if (buf == NULL || len == 0) return -1;
    return (getcwd(buf, len) != NULL) ? 0 : -1;
}

int zorya_chdir(const char *path) {
    if (path == NULL) return -1;
    return chdir(path);
}

char zorya_path_sep(void) {
    return '/';
}

/* ============================================================
 * ENVIRONMENT VARIABLES
 * ============================================================ */

const char *zorya_getenv(const char *key) {
    if (key == NULL) return NULL;
    return getenv(key);
}

int zorya_setenv(const char *key, const char *value) {
    if (key == NULL || value == NULL) return -1;
    return setenv(key, value, 1);  /* 1 = overwrite */
}

int zorya_unsetenv(const char *key) {
    if (key == NULL) return -1;
    return unsetenv(key);
}

/* ============================================================
 * TEMPORARY FILES
 * ============================================================ */

int zorya_tmpname(char *buf, size_t len) {
    if (buf == NULL || len < 32) return -1;

    const char *tmpdir = getenv("TMPDIR");
    if (tmpdir == NULL) tmpdir = "/tmp";

    int n = snprintf(buf, len, "%s/zorya_XXXXXX", tmpdir);
    if (n < 0 || (size_t)n >= len) return -1;

    int fd = mkstemp(buf);
    if (fd < 0) return -1;

    /* Close the fd — caller gets a safe, unique path that already exists */
    close(fd);
    return 0;
}

/* ============================================================
 * PROCESS / COMMAND EXECUTION
 * ============================================================ */

FILE *zorya_popen(const char *cmd, const char *mode) {
    if (cmd == NULL || mode == NULL) return NULL;
    return popen(cmd, mode);
}

int zorya_pclose(FILE *fp) {
    if (fp == NULL) return -1;
    return pclose(fp);
}

int zorya_system(const char *cmd) {
    return system(cmd);
}

/* ============================================================
 * DIRECTORY LISTING
 * ============================================================ */

int zorya_dir_open(const char *path, ZoryaDir *dir) {
    if (path == NULL || dir == NULL) return -1;

    DIR *d = opendir(path);
    if (d == NULL) return -1;

    dir->handle = d;
    strncpy(dir->path, path, ZORYA_MAX_PATH - 1);
    dir->path[ZORYA_MAX_PATH - 1] = '\0';
    return 0;
}

int zorya_dir_next(ZoryaDir *dir, ZoryaDirEntry *entry) {
    if (dir == NULL || dir->handle == NULL || entry == NULL) return -1;

    DIR *d = (DIR *)dir->handle;
    struct dirent *de;

    for (;;) {
        de = readdir(d);
        if (de == NULL) return 1;  /* End of directory */

        /* Skip . and .. */
        if (de->d_name[0] == '.') {
            if (de->d_name[1] == '\0') continue;
            if (de->d_name[1] == '.' && de->d_name[2] == '\0') continue;
        }
        break;
    }

    strncpy(entry->name, de->d_name, ZORYA_MAX_NAME - 1);
    entry->name[ZORYA_MAX_NAME - 1] = '\0';

    /* Determine if directory — use d_type if available, fall back to stat */
#ifdef DT_DIR
    if (de->d_type != DT_UNKNOWN) {
        entry->is_dir = (de->d_type == DT_DIR) ? 1 : 0;
    } else
#endif
    {
        /* Fallback: stat the full path */
        char fullpath[ZORYA_MAX_PATH];
        int n = snprintf(fullpath, sizeof(fullpath), "%s/%s",
                         dir->path, de->d_name);
        if (n > 0 && (size_t)n < sizeof(fullpath)) {
            struct stat st;
            entry->is_dir = (stat(fullpath, &st) == 0 &&
                             S_ISDIR(st.st_mode)) ? 1 : 0;
        } else {
            entry->is_dir = 0;
        }
    }

    return 0;
}

void zorya_dir_close(ZoryaDir *dir) {
    if (dir == NULL || dir->handle == NULL) return;
    closedir((DIR *)dir->handle);
    dir->handle = NULL;
}

/* ============================================================
 * DYNAMIC LOADING
 * ============================================================ */

void *zorya_dlopen(const char *path) {
    return dlopen(path, RTLD_LAZY);
}

void *zorya_dlsym(void *handle, const char *name) {
    if (handle == NULL || name == NULL) return NULL;
    return dlsym(handle, name);
}

int zorya_dlclose(void *handle) {
    if (handle == NULL) return -1;
    return dlclose(handle);
}

/* ============================================================
 * TIME
 * ============================================================ */

uint64_t zorya_monotonic_usec(void) {
#ifdef __APPLE__
    static mach_timebase_info_data_t timebase;
    if (timebase.denom == 0) {
        mach_timebase_info(&timebase);
    }
    uint64_t ticks = mach_absolute_time();
    /* Convert to nanoseconds, then to microseconds */
    return (ticks * timebase.numer / timebase.denom) / 1000;
#else
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000000ULL +
           (uint64_t)ts.tv_nsec / 1000ULL;
#endif
}

void zorya_sleep_ms(unsigned int ms) {
    struct timespec ts;
    ts.tv_sec  = (time_t)(ms / 1000);
    ts.tv_nsec = (long)((ms % 1000) * 1000000L);
    nanosleep(&ts, NULL);
}
