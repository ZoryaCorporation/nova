/**
 * @file pal.h
 * @brief ZORYA PAL - Platform Abstraction Layer API Contract
 *
 * Pure API header. No platform headers. No #ifdef _WIN32.
 * No conditional compilation. Just function declarations.
 *
 * Every function here has exactly ONE implementation per platform:
 *   - pal_posix.c   (Linux, macOS, *BSD, Raspberry Pi)
 *   - pal_win32.c   (Windows)
 *   - pal_stub.c    (Freestanding / unknown — everything fails cleanly)
 *
 * The Makefile (or build system) selects which .c file to compile.
 * Application code #includes this header and calls the functions.
 * That's it. Zero conditional compilation in the caller.
 *
 * RETURN CONVENTION:
 *   - int-returning functions: 0 = success, -1 = error
 *   - Pointer-returning functions: NULL = error
 *   - Boolean queries (zorya_file_exists, zorya_isatty): 1 = true, 0 = false
 *
 * @author Anthony Taliento
 * @date 2026-06-28
 * @version 1.0.0
 *
 * @copyright Copyright (c) 2026 Zorya Corporation
 * @license Apache-2.0
 *
 * ZORYA-C COMPLIANCE: v2.0.0 (Strict Mode)
 *
 * ============================================================================
 * USAGE
 * ============================================================================
 *
 *   #include <zorya/pal.h>
 *
 *   if (zorya_file_exists("config.ini")) {
 *       ZoryaStat st;
 *       zorya_stat("config.ini", &st);
 *       printf("Size: %llu bytes\n", (unsigned long long)st.size);
 *   }
 *
 *   zorya_setenv("NOVA_PATH", "/usr/lib/nova");
 *
 *   ZoryaDir dir;
 *   ZoryaDirEntry entry;
 *   if (zorya_dir_open(".", &dir) == 0) {
 *       while (zorya_dir_next(&dir, &entry) == 0) {
 *           printf("%s%s\n", entry.name, entry.is_dir ? "/" : "");
 *       }
 *       zorya_dir_close(&dir);
 *   }
 *
 */

#ifndef ZORYA_PAL_H
#define ZORYA_PAL_H

#include <zorya/pal_detect.h>

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>      /* FILE* for zorya_popen, zorya_fileno */

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 * TYPES
 * ============================================================ */

/** Maximum filename length in directory entries */
#define ZORYA_MAX_NAME  256

/** Maximum path length */
#define ZORYA_MAX_PATH  4096

/**
 * @brief Platform-independent file status.
 *
 * Subset of POSIX struct stat, normalized across platforms.
 * All times are seconds since Unix epoch (UTC).
 */
typedef struct {
    uint64_t size;       /**< File size in bytes                    */
    uint64_t mtime;      /**< Last modification time (epoch secs)  */
    int      is_file;    /**< 1 if regular file                    */
    int      is_dir;     /**< 1 if directory                       */
    int      is_link;    /**< 1 if symbolic link                   */
    int      mode;       /**< Permission bits (POSIX-style)        */
} ZoryaStat;

/**
 * @brief Opaque directory handle for zorya_dir_* functions.
 */
typedef struct {
    void *handle;        /**< Platform-specific handle (DIR*, HANDLE) */
    char  path[ZORYA_MAX_PATH]; /**< Original path for error reporting */
} ZoryaDir;

/**
 * @brief Directory entry returned by zorya_dir_next().
 */
typedef struct {
    char name[ZORYA_MAX_NAME]; /**< Entry filename (no path prefix) */
    int  is_dir;               /**< 1 if this entry is a directory  */
} ZoryaDirEntry;

/* ============================================================
 * TERMINAL / FILE DESCRIPTORS
 * ============================================================ */

/**
 * @brief Check if a file descriptor refers to a terminal.
 * @param fd  File descriptor (0=stdin, 1=stdout, 2=stderr)
 * @return 1 if terminal, 0 if not (or on error)
 */
int zorya_isatty(int fd);

/**
 * @brief Get the file descriptor for a FILE stream.
 * @param fp  Open FILE pointer
 * @return File descriptor, or -1 on error
 */
int zorya_fileno(FILE *fp);

/**
 * @brief Duplicate a file descriptor.
 * @param fd  File descriptor to duplicate
 * @return New file descriptor, or -1 on error
 */
int zorya_dup(int fd);

/**
 * @brief Duplicate a file descriptor to a specific number.
 * @param oldfd  Source file descriptor
 * @param newfd  Target file descriptor number
 * @return newfd on success, or -1 on error
 */
int zorya_dup2(int oldfd, int newfd);

/**
 * @brief Close a raw file descriptor.
 * @param fd  File descriptor to close
 * @return 0 on success, -1 on error
 */
int zorya_fdclose(int fd);

/* ============================================================
 * FILESYSTEM
 * ============================================================ */

/**
 * @brief Check if a path exists (file, directory, or link).
 * @param path  Path to check (must not be NULL)
 * @return 1 if exists, 0 if not
 */
int zorya_file_exists(const char *path);

/**
 * @brief Get file/directory status.
 * @param path  Path to stat
 * @param st    Output stat structure
 * @return 0 on success, -1 on error
 */
int zorya_stat(const char *path, ZoryaStat *st);

/**
 * @brief Create a directory.
 * @param path  Directory path to create
 * @return 0 on success, -1 on error
 */
int zorya_mkdir(const char *path);

/**
 * @brief Remove an empty directory.
 * @param path  Directory path to remove
 * @return 0 on success, -1 on error
 */
int zorya_rmdir(const char *path);

/**
 * @brief Delete a file.
 * @param path  File path to delete
 * @return 0 on success, -1 on error
 */
int zorya_remove(const char *path);

/**
 * @brief Rename/move a file or directory.
 * @param src   Source path
 * @param dst   Destination path
 * @return 0 on success, -1 on error
 */
int zorya_rename(const char *src, const char *dst);

/**
 * @brief Change file permissions.
 * @param path  File path
 * @param mode  Permission bits (POSIX-style: 0755, 0644, etc.)
 * @return 0 on success, -1 on error
 */
int zorya_chmod(const char *path, int mode);

/**
 * @brief Check if a path is a regular file.
 * @param path  Path to check
 * @return 1 if regular file, 0 otherwise
 */
int zorya_is_file(const char *path);

/**
 * @brief Check if a path is a directory.
 * @param path  Path to check
 * @return 1 if directory, 0 otherwise
 */
int zorya_is_dir(const char *path);

/* ============================================================
 * PATH / WORKING DIRECTORY
 * ============================================================ */

/**
 * @brief Get the current working directory.
 * @param buf   Buffer to receive the path
 * @param len   Buffer size in bytes
 * @return 0 on success, -1 on error
 */
int zorya_getcwd(char *buf, size_t len);

/**
 * @brief Change the current working directory.
 * @param path  New working directory
 * @return 0 on success, -1 on error
 */
int zorya_chdir(const char *path);

/**
 * @brief Get the platform path separator character.
 * @return '/' on Unix, '\\' on Windows
 */
char zorya_path_sep(void);

/* ============================================================
 * ENVIRONMENT VARIABLES
 * ============================================================ */

/**
 * @brief Get an environment variable.
 * @param key   Variable name
 * @return Pointer to the value (do NOT free), or NULL if not set
 */
const char *zorya_getenv(const char *key);

/**
 * @brief Set an environment variable (overwrites existing).
 * @param key    Variable name
 * @param value  Variable value
 * @return 0 on success, -1 on error
 */
int zorya_setenv(const char *key, const char *value);

/**
 * @brief Unset (remove) an environment variable.
 * @param key   Variable name to remove
 * @return 0 on success, -1 on error
 */
int zorya_unsetenv(const char *key);

/* ============================================================
 * TEMPORARY FILES
 * ============================================================ */

/**
 * @brief Generate a unique temporary file path (safe).
 *
 * Uses mkstemp() on POSIX, GetTempFileName() on Windows.
 * This is the safe replacement for the deprecated tmpnam().
 *
 * Note: on POSIX, creates the file (to avoid TOCTOU races)
 * and immediately closes the fd. The caller gets the path.
 *
 * @param buf   Buffer to receive the path (min ZORYA_MAX_PATH)
 * @param len   Buffer size
 * @return 0 on success, -1 on error
 */
int zorya_tmpname(char *buf, size_t len);

/* ============================================================
 * PROCESS / COMMAND EXECUTION
 * ============================================================ */

/**
 * @brief Open a pipe to/from a command (like popen).
 * @param cmd   Shell command string
 * @param mode  "r" for reading, "w" for writing
 * @return FILE* on success, NULL on error
 */
FILE *zorya_popen(const char *cmd, const char *mode);

/**
 * @brief Close a pipe opened with zorya_popen().
 * @param fp  FILE* returned by zorya_popen()
 * @return Exit status of the command, or -1 on error
 */
int zorya_pclose(FILE *fp);

/**
 * @brief Execute a command via the system shell.
 * @param cmd  Command string (NULL to check shell availability)
 * @return Command exit status, or -1 on error
 */
int zorya_system(const char *cmd);

/* ============================================================
 * DIRECTORY LISTING
 * ============================================================ */

/**
 * @brief Open a directory for iteration.
 * @param path  Directory path
 * @param dir   Output directory handle
 * @return 0 on success, -1 on error
 */
int zorya_dir_open(const char *path, ZoryaDir *dir);

/**
 * @brief Read the next directory entry.
 *
 * Automatically skips "." and ".." entries.
 *
 * @param dir    Open directory handle
 * @param entry  Output entry
 * @return 0 on success, 1 when no more entries, -1 on error
 */
int zorya_dir_next(ZoryaDir *dir, ZoryaDirEntry *entry);

/**
 * @brief Close a directory handle.
 * @param dir  Directory handle to close
 */
void zorya_dir_close(ZoryaDir *dir);

/* ============================================================
 * DYNAMIC LOADING
 * ============================================================ */

/**
 * @brief Load a shared library / DLL.
 * @param path  Library path (NULL for the main executable)
 * @return Opaque handle, or NULL on error
 */
void *zorya_dlopen(const char *path);

/**
 * @brief Look up a symbol in a loaded library.
 * @param handle  Handle from zorya_dlopen()
 * @param name    Symbol name
 * @return Pointer to the symbol, or NULL on error
 */
void *zorya_dlsym(void *handle, const char *name);

/**
 * @brief Close a loaded library.
 * @param handle  Handle from zorya_dlopen()
 * @return 0 on success, -1 on error
 */
int zorya_dlclose(void *handle);

/* ============================================================
 * TIME
 * ============================================================ */

/**
 * @brief Get monotonic time in microseconds.
 *
 * Uses clock_gettime(CLOCK_MONOTONIC) on POSIX,
 * QueryPerformanceCounter on Windows. Not wall-clock time;
 * suitable for measuring elapsed intervals.
 *
 * @return Microseconds since an arbitrary epoch, or 0 on error
 */
uint64_t zorya_monotonic_usec(void);

/**
 * @brief Sleep for the specified number of milliseconds.
 * @param ms  Milliseconds to sleep
 */
void zorya_sleep_ms(unsigned int ms);

#ifdef __cplusplus
}
#endif

#endif /* ZORYA_PAL_H */
