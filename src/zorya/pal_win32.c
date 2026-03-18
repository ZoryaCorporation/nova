/**
 * @file pal_win32.c
 * @brief ZORYA PAL - Windows Implementation
 *
 * Full implementation of the ZORYA PAL API for Windows.
 * Uses Win32 API and MSVC CRT functions.
 *
 * This file is only compiled when targeting Windows.
 * The Makefile / build system selects this file automatically.
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

#ifdef _WIN32  /* Guard: only compile on Windows */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <direct.h>
#include <io.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <process.h>

#include <zorya/pal.h>

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>

/* ============================================================
 * TERMINAL / FILE DESCRIPTORS
 * ============================================================ */

int zorya_isatty(int fd) {
    return _isatty(fd) ? 1 : 0;
}

int zorya_fileno(FILE *fp) {
    if (fp == NULL) return -1;
    return _fileno(fp);
}

int zorya_dup(int fd) {
    return _dup(fd);
}

int zorya_dup2(int oldfd, int newfd) {
    return _dup2(oldfd, newfd);
}

int zorya_fdclose(int fd) {
    return _close(fd);
}

/* ============================================================
 * FILESYSTEM
 * ============================================================ */

int zorya_file_exists(const char *path) {
    if (path == NULL) return 0;
    DWORD attrs = GetFileAttributesA(path);
    return (attrs != INVALID_FILE_ATTRIBUTES) ? 1 : 0;
}

int zorya_stat(const char *path, ZoryaStat *st) {
    if (path == NULL || st == NULL) return -1;

    WIN32_FILE_ATTRIBUTE_DATA data;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &data)) {
        return -1;
    }

    /* Combine high/low into 64-bit size */
    st->size = ((uint64_t)data.nFileSizeHigh << 32) |
               (uint64_t)data.nFileSizeLow;

    /* Convert FILETIME to Unix epoch seconds */
    /* FILETIME = 100ns intervals since 1601-01-01 */
    /* Unix epoch offset: 11644473600 seconds */
    uint64_t ft = ((uint64_t)data.ftLastWriteTime.dwHighDateTime << 32) |
                  (uint64_t)data.ftLastWriteTime.dwLowDateTime;
    st->mtime = (ft / 10000000ULL) - 11644473600ULL;

    st->is_file = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? 0 : 1;
    st->is_dir  = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? 1 : 0;
    st->is_link = (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ? 1 : 0;
    st->mode    = 0644;  /* Windows doesn't have Unix perms; approximate */
    if (st->is_dir) st->mode = 0755;

    return 0;
}

int zorya_mkdir(const char *path) {
    if (path == NULL) return -1;
    return _mkdir(path);
}

int zorya_rmdir(const char *path) {
    if (path == NULL) return -1;
    return _rmdir(path);
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
    return _chmod(path, mode);
}

int zorya_is_file(const char *path) {
    if (path == NULL) return 0;
    DWORD attrs = GetFileAttributesA(path);
    if (attrs == INVALID_FILE_ATTRIBUTES) return 0;
    return (attrs & FILE_ATTRIBUTE_DIRECTORY) ? 0 : 1;
}

int zorya_is_dir(const char *path) {
    if (path == NULL) return 0;
    DWORD attrs = GetFileAttributesA(path);
    if (attrs == INVALID_FILE_ATTRIBUTES) return 0;
    return (attrs & FILE_ATTRIBUTE_DIRECTORY) ? 1 : 0;
}

/* ============================================================
 * PATH / WORKING DIRECTORY
 * ============================================================ */

int zorya_getcwd(char *buf, size_t len) {
    if (buf == NULL || len == 0) return -1;
    return (_getcwd(buf, (int)len) != NULL) ? 0 : -1;
}

int zorya_chdir(const char *path) {
    if (path == NULL) return -1;
    return _chdir(path);
}

char zorya_path_sep(void) {
    return '\\';
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
    /* _putenv_s is the safe MSVC function */
    return _putenv_s(key, value) == 0 ? 0 : -1;
}

int zorya_unsetenv(const char *key) {
    if (key == NULL) return -1;
    /* Setting to empty string removes the variable on Windows */
    return _putenv_s(key, "") == 0 ? 0 : -1;
}

/* ============================================================
 * TEMPORARY FILES
 * ============================================================ */

int zorya_tmpname(char *buf, size_t len) {
    if (buf == NULL || len < MAX_PATH) return -1;

    char tmpdir[MAX_PATH];
    DWORD n = GetTempPathA(MAX_PATH, tmpdir);
    if (n == 0 || n > MAX_PATH) return -1;

    char tmpfile[MAX_PATH];
    if (GetTempFileNameA(tmpdir, "zry", 0, tmpfile) == 0) {
        return -1;
    }

    size_t flen = strlen(tmpfile);
    if (flen >= len) return -1;
    memcpy(buf, tmpfile, flen + 1);

    return 0;
}

/* ============================================================
 * PROCESS / COMMAND EXECUTION
 * ============================================================ */

FILE *zorya_popen(const char *cmd, const char *mode) {
    if (cmd == NULL || mode == NULL) return NULL;
    return _popen(cmd, mode);
}

int zorya_pclose(FILE *fp) {
    if (fp == NULL) return -1;
    return _pclose(fp);
}

int zorya_system(const char *cmd) {
    return system(cmd);
}

/* ============================================================
 * DIRECTORY LISTING
 * ============================================================ */

/** Internal: Win32 dir handle wraps HANDLE + WIN32_FIND_DATAA */
typedef struct {
    HANDLE           hfind;
    WIN32_FIND_DATAA fdata;
    int              first;  /* 1 = first entry not yet consumed */
} Win32DirState;

int zorya_dir_open(const char *path, ZoryaDir *dir) {
    if (path == NULL || dir == NULL) return -1;

    /* Build search pattern: path\* */
    char pattern[ZORYA_MAX_PATH];
    int n = snprintf(pattern, sizeof(pattern), "%s\\*", path);
    if (n < 0 || (size_t)n >= sizeof(pattern)) return -1;

    Win32DirState *state = (Win32DirState *)malloc(sizeof(Win32DirState));
    if (state == NULL) return -1;

    state->hfind = FindFirstFileA(pattern, &state->fdata);
    if (state->hfind == INVALID_HANDLE_VALUE) {
        free(state);
        return -1;
    }
    state->first = 1;

    dir->handle = state;
    strncpy(dir->path, path, ZORYA_MAX_PATH - 1);
    dir->path[ZORYA_MAX_PATH - 1] = '\0';
    return 0;
}

int zorya_dir_next(ZoryaDir *dir, ZoryaDirEntry *entry) {
    if (dir == NULL || dir->handle == NULL || entry == NULL) return -1;

    Win32DirState *state = (Win32DirState *)dir->handle;

    for (;;) {
        if (!state->first) {
            if (!FindNextFileA(state->hfind, &state->fdata)) {
                return 1;  /* End of directory */
            }
        }
        state->first = 0;

        const char *name = state->fdata.cFileName;

        /* Skip . and .. */
        if (name[0] == '.') {
            if (name[1] == '\0') continue;
            if (name[1] == '.' && name[2] == '\0') continue;
        }
        break;
    }

    strncpy(entry->name, state->fdata.cFileName, ZORYA_MAX_NAME - 1);
    entry->name[ZORYA_MAX_NAME - 1] = '\0';
    entry->is_dir = (state->fdata.dwFileAttributes &
                     FILE_ATTRIBUTE_DIRECTORY) ? 1 : 0;

    return 0;
}

void zorya_dir_close(ZoryaDir *dir) {
    if (dir == NULL || dir->handle == NULL) return;
    Win32DirState *state = (Win32DirState *)dir->handle;
    if (state->hfind != INVALID_HANDLE_VALUE) {
        FindClose(state->hfind);
    }
    free(state);
    dir->handle = NULL;
}

/* ============================================================
 * DYNAMIC LOADING
 * ============================================================ */

void *zorya_dlopen(const char *path) {
    HMODULE h = LoadLibraryA(path);
    return (void *)h;
}

void *zorya_dlsym(void *handle, const char *name) {
    if (handle == NULL || name == NULL) return NULL;
    FARPROC p = GetProcAddress((HMODULE)handle, name);
    return (void *)(uintptr_t)p;
}

int zorya_dlclose(void *handle) {
    if (handle == NULL) return -1;
    return FreeLibrary((HMODULE)handle) ? 0 : -1;
}

/* ============================================================
 * TIME
 * ============================================================ */

uint64_t zorya_monotonic_usec(void) {
    static LARGE_INTEGER freq = {0};
    if (freq.QuadPart == 0) {
        QueryPerformanceFrequency(&freq);
    }
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    return (uint64_t)(counter.QuadPart * 1000000LL / freq.QuadPart);
}

void zorya_sleep_ms(unsigned int ms) {
    Sleep((DWORD)ms);
}

#endif /* _WIN32 */
