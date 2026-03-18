/**
 * @file pal_stub.c
 * @brief ZORYA PAL - Stub/Freestanding Implementation
 *
 * Fallback implementation for platforms without POSIX or Win32.
 * Every function returns a safe error value (-1, NULL, or 0).
 *
 * This allows Nova to COMPILE on any platform, even if platform
 * features are unavailable at runtime. Code that calls PAL
 * functions should handle errors gracefully.
 *
 * Use cases:
 *   - Embedded systems without an OS
 *   - Cross-compilation testing
 *   - WebAssembly targets (WASI extensions can override later)
 *   - Unknown/exotic platforms
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

#include <zorya/pal.h>

#include <string.h>

/* ============================================================
 * TERMINAL / FILE DESCRIPTORS
 * ============================================================ */

int zorya_isatty(int fd) {
    (void)fd;
    return 0;   /* Never a terminal in freestanding */
}

int zorya_fileno(FILE *fp) {
    (void)fp;
    return -1;
}

int zorya_dup(int fd) {
    (void)fd;
    return -1;
}

int zorya_dup2(int oldfd, int newfd) {
    (void)oldfd; (void)newfd;
    return -1;
}

int zorya_fdclose(int fd) {
    (void)fd;
    return -1;
}

/* ============================================================
 * FILESYSTEM
 * ============================================================ */

int zorya_file_exists(const char *path) {
    (void)path;
    return 0;   /* Nothing exists in freestanding */
}

int zorya_stat(const char *path, ZoryaStat *st) {
    (void)path;
    if (st != NULL) memset(st, 0, sizeof(*st));
    return -1;
}

int zorya_mkdir(const char *path) {
    (void)path;
    return -1;
}

int zorya_rmdir(const char *path) {
    (void)path;
    return -1;
}

int zorya_remove(const char *path) {
    (void)path;
    return -1;
}

int zorya_rename(const char *src, const char *dst) {
    (void)src; (void)dst;
    return -1;
}

int zorya_chmod(const char *path, int mode) {
    (void)path; (void)mode;
    return -1;
}

int zorya_is_file(const char *path) {
    (void)path;
    return 0;
}

int zorya_is_dir(const char *path) {
    (void)path;
    return 0;
}

/* ============================================================
 * PATH / WORKING DIRECTORY
 * ============================================================ */

int zorya_getcwd(char *buf, size_t len) {
    if (buf != NULL && len > 0) buf[0] = '\0';
    return -1;
}

int zorya_chdir(const char *path) {
    (void)path;
    return -1;
}

char zorya_path_sep(void) {
    return '/';   /* Default to Unix convention */
}

/* ============================================================
 * ENVIRONMENT VARIABLES
 * ============================================================ */

const char *zorya_getenv(const char *key) {
    (void)key;
    return NULL;
}

int zorya_setenv(const char *key, const char *value) {
    (void)key; (void)value;
    return -1;
}

int zorya_unsetenv(const char *key) {
    (void)key;
    return -1;
}

/* ============================================================
 * TEMPORARY FILES
 * ============================================================ */

int zorya_tmpname(char *buf, size_t len) {
    if (buf != NULL && len > 0) buf[0] = '\0';
    return -1;
}

/* ============================================================
 * PROCESS / COMMAND EXECUTION
 * ============================================================ */

FILE *zorya_popen(const char *cmd, const char *mode) {
    (void)cmd; (void)mode;
    return NULL;
}

int zorya_pclose(FILE *fp) {
    (void)fp;
    return -1;
}

int zorya_system(const char *cmd) {
    (void)cmd;
    return -1;
}

/* ============================================================
 * DIRECTORY LISTING
 * ============================================================ */

int zorya_dir_open(const char *path, ZoryaDir *dir) {
    (void)path;
    if (dir != NULL) dir->handle = NULL;
    return -1;
}

int zorya_dir_next(ZoryaDir *dir, ZoryaDirEntry *entry) {
    (void)dir;
    if (entry != NULL) {
        entry->name[0] = '\0';
        entry->is_dir = 0;
    }
    return -1;
}

void zorya_dir_close(ZoryaDir *dir) {
    if (dir != NULL) dir->handle = NULL;
}

/* ============================================================
 * DYNAMIC LOADING
 * ============================================================ */

void *zorya_dlopen(const char *path) {
    (void)path;
    return NULL;
}

void *zorya_dlsym(void *handle, const char *name) {
    (void)handle; (void)name;
    return NULL;
}

int zorya_dlclose(void *handle) {
    (void)handle;
    return -1;
}

/* ============================================================
 * TIME
 * ============================================================ */

uint64_t zorya_monotonic_usec(void) {
    return 0;   /* No monotonic clock in freestanding */
}

void zorya_sleep_ms(unsigned int ms) {
    (void)ms;
    /* Best effort: busy-wait or no-op */
}
