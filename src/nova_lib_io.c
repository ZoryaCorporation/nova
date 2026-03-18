/**
 * @file nova_lib_io.c
 * @brief Nova Language - I/O Standard Library
 *
 * Provides file I/O functions as the "io" module.
 * Phase 7 uses standard C <stdio.h> for portability.
 * Phase 9 will integrate zorya_fileops for POSIX optimizations
 * (mmap, inotify file watching, atomic writes, etc.).
 *
 * Functions:
 *   io.open(filename [, mode])  Open a file
 *   io.close([file])            Close a file
 *   io.read([fmt])              Read from stdin or file
 *   io.write(...)               Write to stdout
 *   io.lines([filename])        Line iterator (basic)
 *   io.type(obj)                Check if obj is a file
 *   io.input([file])            Set/get default input
 *   io.output([file])           Set/get default output
 *   io.tmpfile()                Create and open temp file
 *   io.flush()                  Flush default output
 *
 * File methods (via file handle):
 *   file:read(fmt)
 *   file:write(...)
 *   file:close()
 *   file:seek([whence [, offset]])
 *   file:flush()
 *   file:lines()
 *
 * NOTE: File handles are stored as userdata containing FILE*.
 * Full handle-as-object with methods requires metatables (Phase 9).
 * For now, we provide module-level functions.
 *
 * @author Anthony Taliento
 * @date 2026-02-08
 * @version 0.2.0
 *
 * @copyright Copyright (c) 2026 Zorya Corporation
 * @license MIT
 *
 * DEPENDENCIES:
 *   - nova_lib.h
 *   - <stdio.h>
 *
 * FUTURE: zorya_fileops.h integration for:
 *   - zfo_read_text() / zfo_write_text() for convenience
 *   - zfo_mmap_read() for large file performance
 *   - zfo_watch() for file change monitoring
 *   - zfo_atomic_write() for safe file updates
 *
 * ZORYA-C COMPLIANCE: v2.0.0
 */

#include "nova/nova_lib.h"
#include "nova/nova_vm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

/* ============================================================
 * INTERNAL: FILE HANDLE MANAGEMENT
 *
 * We store FILE* in a simple structure. Without metatables,
 * we can't attach methods to the handle. For Phase 7,
 * file operations use module-level functions.
 * ============================================================ */

#define NOVA_IO_MAX_FILES 64

typedef struct {
    FILE *fp;
    int   is_pipe;
    int   closable;  /* Can be closed (not stdin/stdout/stderr) */
} NovaFileHandle;

static NovaFileHandle nova_io_files[NOVA_IO_MAX_FILES];
static int nova_io_initialized = 0;

static void novai_io_init(void) {
    if (nova_io_initialized) {
        return;
    }
    memset(nova_io_files, 0, sizeof(nova_io_files));
    /* Reserve slots 0-2 for stdin/stdout/stderr */
    nova_io_files[0].fp = stdin;
    nova_io_files[0].closable = 0;
    nova_io_files[1].fp = stdout;
    nova_io_files[1].closable = 0;
    nova_io_files[2].fp = stderr;
    nova_io_files[2].closable = 0;
    nova_io_initialized = 1;
}

static int novai_io_alloc(FILE *fp, int is_pipe) {
    novai_io_init();
    for (int i = 3; i < NOVA_IO_MAX_FILES; i++) {
        if (nova_io_files[i].fp == NULL) {
            nova_io_files[i].fp = fp;
            nova_io_files[i].is_pipe = is_pipe;
            nova_io_files[i].closable = 1;
            return i;
        }
    }
    return -1;  /* No free slots */
}

static void novai_io_free(int slot) {
    if (slot >= 3 && slot < NOVA_IO_MAX_FILES) {
        nova_io_files[slot].fp = NULL;
        nova_io_files[slot].is_pipe = 0;
        nova_io_files[slot].closable = 0;
    }
}

static FILE *novai_io_get(int slot) {
    novai_io_init();
    if (slot >= 0 && slot < NOVA_IO_MAX_FILES) {
        return nova_io_files[slot].fp;
    }
    return NULL;
}

/* ============================================================
 * IO.OPEN
 * ============================================================ */

/**
 * @brief io.open(filename [, mode]) - Open a file.
 *
 * Returns a file handle (integer slot) or nil + error.
 * Mode defaults to "r" (read text).
 */
static int nova_io_open(NovaVM *vm) {
    if (nova_lib_check_args(vm, 1) != 0) {
        return -1;
    }

    const char *filename = nova_lib_check_string(vm, 0);
    if (filename == NULL) {
        return -1;
    }

    const char *mode = nova_lib_opt_string(vm, 1, "r");

    FILE *fp = fopen(filename, mode);
    if (fp == NULL) {
        nova_vm_push_nil(vm);
        nova_vm_push_string(vm, strerror(errno), strlen(strerror(errno)));
        return 2;
    }

    int slot = novai_io_alloc(fp, 0);
    if (slot < 0) {
        fclose(fp);
        nova_vm_push_nil(vm);
        const char *msg = "too many open files";
        nova_vm_push_string(vm, msg, strlen(msg));
        return 2;
    }

    nova_vm_push_integer(vm, (nova_int_t)slot);
    return 1;
}

/* ============================================================
 * IO.CLOSE
 * ============================================================ */

/**
 * @brief io.close([file]) - Close a file handle.
 *
 * If no file given, closes default output (no-op for stdout).
 */
static int nova_io_close_func(NovaVM *vm) {
    int nargs = nova_vm_get_top(vm);

    if (nargs == 0) {
        /* Close default output - no-op for stdout */
        nova_vm_push_bool(vm, 1);
        return 1;
    }

    nova_int_t slot = 0;
    if (!nova_lib_check_integer(vm, 0, &slot)) {
        return -1;
    }

    if (slot < 0 || slot >= NOVA_IO_MAX_FILES) {
        nova_vm_push_nil(vm);
        const char *msg = "invalid file handle";
        nova_vm_push_string(vm, msg, strlen(msg));
        return 2;
    }

    NovaFileHandle *fh = &nova_io_files[slot];
    if (fh->fp == NULL) {
        nova_vm_push_nil(vm);
        const char *msg = "file already closed";
        nova_vm_push_string(vm, msg, strlen(msg));
        return 2;
    }

    if (!fh->closable) {
        nova_vm_push_nil(vm);
        const char *msg = "cannot close standard file";
        nova_vm_push_string(vm, msg, strlen(msg));
        return 2;
    }

    int rc = 0;
    if (fh->is_pipe) {
        rc = pclose(fh->fp);
    } else {
        rc = fclose(fh->fp);
    }

    novai_io_free((int)slot);

    if (rc != 0) {
        nova_vm_push_nil(vm);
        nova_vm_push_string(vm, strerror(errno), strlen(strerror(errno)));
        return 2;
    }

    nova_vm_push_bool(vm, 1);
    return 1;
}

/* ============================================================
 * IO.READ
 * ============================================================ */

/**
 * @brief io.read([file,] fmt) - Read from file or stdin.
 *
 * Formats:
 *   "*l" or "l" - Read a line (default)
 *   "*n" or "n" - Read a number
 *   "*a" or "a" - Read entire file
 *   number      - Read n bytes
 */
static int nova_io_read(NovaVM *vm) {
    int nargs = nova_vm_get_top(vm);
    FILE *fp = stdin;
    int fmt_idx = 0;

    /* Check if first arg is a file handle (integer) */
    if (nargs >= 1) {
        NovaValue first = nova_vm_get(vm, 0);
        if (nova_is_integer(first)) {
            fp = novai_io_get((int)nova_as_integer(first));
            if (fp == NULL) {
                nova_vm_raise_error(vm, "invalid file handle");
                return -1;
            }
            fmt_idx = 1;
        }
    }

    /* Default format: read a line */
    const char *fmt = "*l";
    if (fmt_idx < nargs) {
        NovaValue fv = nova_vm_get(vm, fmt_idx);
        if (nova_is_string(fv)) {
            fmt = nova_str_data(nova_as_string(fv));
        } else if (nova_is_integer(fv)) {
            /* Read N bytes */
            nova_int_t nbytes = nova_as_integer(fv);
            if (nbytes <= 0) {
                nova_vm_push_string(vm, "", 0);
                return 1;
            }
            char *buf = (char *)malloc((size_t)nbytes + 1);
            if (buf == NULL) {
                nova_vm_push_nil(vm);
                return 1;
            }
            size_t got = fread(buf, 1, (size_t)nbytes, fp);
            if (got == 0) {
                free(buf);
                nova_vm_push_nil(vm);
                return 1;
            }
            buf[got] = '\0';
            nova_vm_push_string(vm, buf, got);
            free(buf);
            return 1;
        }
    }

    /* Handle format specifiers */
    if (strcmp(fmt, "*l") == 0 || strcmp(fmt, "l") == 0) {
        /* Read a line */
        char buf[4096];
        if (fgets(buf, (int)sizeof(buf), fp) == NULL) {
            nova_vm_push_nil(vm);
            return 1;
        }
        /* Remove trailing newline */
        size_t len = strlen(buf);
        if (len > 0 && buf[len - 1] == '\n') {
            buf[--len] = '\0';
            if (len > 0 && buf[len - 1] == '\r') {
                buf[--len] = '\0';
            }
        }
        nova_vm_push_string(vm, buf, len);
        return 1;
    }

    if (strcmp(fmt, "*n") == 0 || strcmp(fmt, "n") == 0) {
        /* Read a number */
        double d = 0.0;
        if (fscanf(fp, "%lf", &d) == 1) {
            nova_vm_push_number(vm, d);
        } else {
            nova_vm_push_nil(vm);
        }
        return 1;
    }

    if (strcmp(fmt, "*a") == 0 || strcmp(fmt, "a") == 0) {
        /* Read entire file */
        size_t cap = 4096;
        size_t len = 0;
        char *buf = (char *)malloc(cap);
        if (buf == NULL) {
            nova_vm_push_nil(vm);
            return 1;
        }

        while (!feof(fp)) {
            if (len + 1024 > cap) {
                cap *= 2;
                char *nb = (char *)realloc(buf, cap);
                if (nb == NULL) {
                    free(buf);
                    nova_vm_push_nil(vm);
                    return 1;
                }
                buf = nb;
            }
            size_t got = fread(buf + len, 1, 1024, fp);
            len += got;
            if (got == 0) break;
        }

        nova_vm_push_string(vm, buf, len);
        free(buf);
        return 1;
    }

    /* Unrecognized format */
    nova_vm_push_nil(vm);
    return 1;
}

/* ============================================================
 * IO.WRITE
 * ============================================================ */

/**
 * @brief io.write(...) - Write to stdout or file.
 *
 * If first arg is an integer file handle, writes to that file.
 * Otherwise writes all arguments to stdout.
 */
static int nova_io_write(NovaVM *vm) {
    int nargs = nova_vm_get_top(vm);
    FILE *fp = stdout;
    int start = 0;

    if (nargs >= 1) {
        NovaValue first = nova_vm_get(vm, 0);
        if (nova_is_integer(first)) {
            fp = novai_io_get((int)nova_as_integer(first));
            if (fp == NULL) {
                nova_vm_raise_error(vm, "invalid file handle");
                return -1;
            }
            start = 1;
        }
    }

    for (int i = start; i < nargs; i++) {
        NovaValue v = nova_vm_get(vm, i);
        if (nova_is_string(v)) {
            fwrite(nova_str_data(nova_as_string(v)), 1, nova_str_len(nova_as_string(v)), fp);
        } else if (nova_is_integer(v)) {
            fprintf(fp, "%lld", (long long)nova_as_integer(v));
        } else if (nova_is_number(v)) {
            fprintf(fp, "%.14g", nova_as_number(v));
        } else {
            const char *s = nova_vm_typename(nova_typeof(v));
            fputs(s, fp);
        }
    }

    /* Return the file handle (for chaining) */
    nova_vm_push_bool(vm, 1);
    return 1;
}

/* ============================================================
 * IO.LINES (simplified - reads from stdin or file)
 * ============================================================ */

static int nova_io_lines_iter(NovaVM *vm) {
    /* Read one line from stdin */
    char buf[4096];
    if (fgets(buf, (int)sizeof(buf), stdin) == NULL) {
        return 0;  /* End of iteration */
    }
    size_t len = strlen(buf);
    if (len > 0 && buf[len - 1] == '\n') {
        buf[--len] = '\0';
        if (len > 0 && buf[len - 1] == '\r') {
            buf[--len] = '\0';
        }
    }
    nova_vm_push_string(vm, buf, len);
    return 1;
}

static int nova_io_lines(NovaVM *vm) {
    int nargs = nova_vm_get_top(vm);

    if (nargs >= 1) {
        /* io.lines(filename) - open file and iterate */
        const char *filename = nova_lib_check_string(vm, 0);
        if (filename == NULL) {
            return -1;
        }
        /* For now, just return the stdin iterator.
         * Full file-based line iterator needs closures (Phase 8). */
        (void)filename;
    }

    nova_vm_push_cfunction(vm, nova_io_lines_iter);
    return 1;
}

/* ============================================================
 * IO.TYPE
 * ============================================================ */

static int nova_io_type_func(NovaVM *vm) {
    if (nova_lib_check_args(vm, 1) != 0) {
        return -1;
    }

    NovaValue v = nova_vm_get(vm, 0);

    if (nova_is_integer(v)) {
        int slot = (int)nova_as_integer(v);
        FILE *fp = novai_io_get(slot);
        if (fp != NULL) {
            const char *s = "file";
            nova_vm_push_string(vm, s, strlen(s));
            return 1;
        }
        const char *s = "closed file";
        nova_vm_push_string(vm, s, strlen(s));
        return 1;
    }

    nova_vm_push_bool(vm, 0);
    return 1;
}

/* ============================================================
 * IO.TMPFILE
 * ============================================================ */

static int nova_io_tmpfile(NovaVM *vm) {
    FILE *fp = tmpfile();
    if (fp == NULL) {
        nova_vm_push_nil(vm);
        nova_vm_push_string(vm, strerror(errno), strlen(strerror(errno)));
        return 2;
    }

    int slot = novai_io_alloc(fp, 0);
    if (slot < 0) {
        fclose(fp);
        nova_vm_push_nil(vm);
        const char *msg = "too many open files";
        nova_vm_push_string(vm, msg, strlen(msg));
        return 2;
    }

    nova_vm_push_integer(vm, (nova_int_t)slot);
    return 1;
}

/* ============================================================
 * IO.FLUSH
 * ============================================================ */

static int nova_io_flush(NovaVM *vm) {
    (void)vm;
    fflush(stdout);
    nova_vm_push_bool(vm, 1);
    return 1;
}

/* ============================================================
 * IO.POPEN (execute command, read output)
 * ============================================================ */

static int nova_io_popen(NovaVM *vm) {
    if (nova_lib_check_args(vm, 1) != 0) {
        return -1;
    }

    const char *cmd = nova_lib_check_string(vm, 0);
    if (cmd == NULL) {
        return -1;
    }

    const char *mode = nova_lib_opt_string(vm, 1, "r");

    FILE *fp = popen(cmd, mode);
    if (fp == NULL) {
        nova_vm_push_nil(vm);
        nova_vm_push_string(vm, strerror(errno), strlen(strerror(errno)));
        return 2;
    }

    int slot = novai_io_alloc(fp, 1);
    if (slot < 0) {
        pclose(fp);
        nova_vm_push_nil(vm);
        const char *msg = "too many open files";
        nova_vm_push_string(vm, msg, strlen(msg));
        return 2;
    }

    nova_vm_push_integer(vm, (nova_int_t)slot);
    return 1;
}

/* ============================================================
 * REGISTRATION TABLE
 * ============================================================ */

static const NovaLibReg nova_io_lib[] = {
    {"open",    nova_io_open},
    {"close",   nova_io_close_func},
    {"read",    nova_io_read},
    {"write",   nova_io_write},
    {"lines",   nova_io_lines},
    {"type",    nova_io_type_func},
    {"tmpfile",  nova_io_tmpfile},
    {"flush",   nova_io_flush},
    {"popen",   nova_io_popen},
    {NULL,      NULL}
};

/* ============================================================
 * MODULE OPENER
 * ============================================================ */

int nova_open_io(NovaVM *vm) {
    if (vm == NULL) {
        return -1;
    }

    novai_io_init();
    nova_lib_register_module(vm, "io", nova_io_lib);

    /* Also register io.stdin, io.stdout, io.stderr as integer handles */
    /* These will be accessible as io.stdin = 0, io.stdout = 1, etc. */

    return 0;
}
