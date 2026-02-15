/**
 * @file nova_lib_os.c
 * @brief Nova Language - OS Standard Library
 *
 * Provides operating system functions as the "os" module.
 *
 * Functions:
 *   os.clock()           CPU time used (seconds)
 *   os.time([t])         Current time as integer
 *   os.date([fmt [,t]])  Formatted date string
 *   os.difftime(t2, t1)  Difference in seconds
 *   os.getenv(name)      Get environment variable
 *   os.execute(cmd)      Execute shell command
 *   os.exit([code])      Exit the process
 *   os.remove(filename)  Remove a file
 *   os.rename(old, new)  Rename a file
 *   os.tmpname()         Generate temporary file name
 *   os.setlocale(l [,c]) Set locale (stub)
 *
 * @author Anthony Taliento
 * @date 2026-02-08
 * @version 0.1.0
 *
 * @copyright Copyright (c) 2026 Zorya Corporation
 * @license MIT
 *
 * DEPENDENCIES:
 *   - nova_lib.h
 *   - <time.h>, <stdlib.h>, <stdio.h>
 *
 * FUTURE: zorya_fileops.h integration for:
 *   - zfo_remove() / zfo_move() for robust file operations
 *   - zfo_stat() for file metadata
 *   - zfo_mkdir() / zfo_opendir() for directory operations
 *
 * ZORYA-C COMPLIANCE: v2.0.0
 */

#include "nova/nova_lib.h"
#include "nova/nova_vm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <locale.h>

#ifdef __linux__
#include <sys/wait.h>
#endif

/* ============================================================
 * CLOCK
 * ============================================================ */

/**
 * @brief os.clock() - CPU time used by the program.
 * @return 1 result: number (seconds).
 */
static int nova_os_clock(NovaVM *vm) {
    nova_vm_push_number(vm, (nova_number_t)clock() / (nova_number_t)CLOCKS_PER_SEC);
    return 1;
}

/* ============================================================
 * TIME
 * ============================================================ */

/**
 * @brief os.time([table]) - Current time as integer.
 *
 * Without arguments, returns current time.
 * With a table argument, returns time from table fields
 * (year, month, day, hour, min, sec) - Phase 9.
 *
 * @return 1 result: integer (seconds since epoch).
 */
static int nova_os_time(NovaVM *vm) {
    (void)vm;
    time_t t = time(NULL);
    nova_vm_push_integer(vm, (nova_int_t)t);
    return 1;
}

/* ============================================================
 * DATE
 * ============================================================ */

/**
 * @brief os.date([format [, time]]) - Formatted date string.
 *
 * Default format: "%c" (locale-appropriate date/time).
 * If format starts with "!", uses UTC.
 *
 * @return 1 result: string.
 */
/*
** PCM: format-nonliteral suppression
** Purpose: os.date() passes user-provided strftime format strings
** Rationale: Format string is validated by strftime itself
** Audit Date: 2026-02-08
*/
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-nonliteral"

static int nova_os_date(NovaVM *vm) {
    int nargs = nova_vm_get_top(vm);
    const char *fmt = "%c";
    time_t t = time(NULL);

    if (nargs >= 1) {
        NovaValue fv = nova_vm_get(vm, 0);
        if (nova_is_string(fv)) {
            fmt = nova_str_data(nova_as_string(fv));
        }
    }
    if (nargs >= 2) {
        NovaValue tv = nova_vm_get(vm, 1);
        if (nova_is_integer(tv)) {
            t = (time_t)nova_as_integer(tv);
        }
    }

    /* Check for UTC prefix */
    int utc = 0;
    if (fmt[0] == '!') {
        utc = 1;
        fmt++;
    }

    struct tm result;
    struct tm *tm_ptr = NULL;
    if (utc) {
        tm_ptr = gmtime_r(&t, &result);
    } else {
        tm_ptr = localtime_r(&t, &result);
    }

    if (tm_ptr == NULL) {
        nova_vm_push_nil(vm);
        return 1;
    }

    char buf[256];
    size_t len = strftime(buf, sizeof(buf), fmt, tm_ptr);
    if (len == 0) {
        /* strftime failed or format produced empty string */
        nova_vm_push_string(vm, "", 0);
        return 1;
    }

    nova_vm_push_string(vm, buf, len);
    return 1;
}

#pragma GCC diagnostic pop

/* ============================================================
 * DIFFTIME
 * ============================================================ */

static int nova_os_difftime(NovaVM *vm) {
    if (nova_lib_check_args(vm, 2) != 0) {
        return -1;
    }

    nova_int_t t2 = 0, t1 = 0;
    if (!nova_lib_check_integer(vm, 0, &t2)) return -1;
    if (!nova_lib_check_integer(vm, 1, &t1)) return -1;

    nova_vm_push_number(vm, difftime((time_t)t2, (time_t)t1));
    return 1;
}

/* ============================================================
 * GETENV
 * ============================================================ */

static int nova_os_getenv(NovaVM *vm) {
    if (nova_lib_check_args(vm, 1) != 0) {
        return -1;
    }

    const char *name = nova_lib_check_string(vm, 0);
    if (name == NULL) {
        return -1;
    }

    const char *val = getenv(name);
    if (val == NULL) {
        nova_vm_push_nil(vm);
    } else {
        nova_vm_push_string(vm, val, strlen(val));
    }
    return 1;
}

/* ============================================================
 * EXECUTE
 * ============================================================ */

/**
 * @brief os.execute([command]) - Execute a shell command.
 *
 * Returns true/nil, exit-type ("exit" or "signal"), exit-code.
 * Without args, returns whether a shell is available.
 */
static int nova_os_execute(NovaVM *vm) {
    int nargs = nova_vm_get_top(vm);

    if (nargs == 0) {
        /* Check if shell is available */
        nova_vm_push_bool(vm, system(NULL) != 0);
        return 1;
    }

    const char *cmd = nova_lib_check_string(vm, 0);
    if (cmd == NULL) {
        return -1;
    }

    int status = system(cmd);

    if (status == -1) {
        nova_vm_push_nil(vm);
        const char *exit_type = "exit";
        nova_vm_push_string(vm, exit_type, strlen(exit_type));
        nova_vm_push_integer(vm, -1);
        return 3;
    }

    /* On POSIX, use WEXITSTATUS etc. */
    int code = status;
#ifdef __linux__
    if (WIFEXITED(status)) {
        code = WEXITSTATUS(status);
    }
#endif

    if (code == 0) {
        nova_vm_push_bool(vm, 1);
    } else {
        nova_vm_push_nil(vm);
    }
    const char *exit_type = "exit";
    nova_vm_push_string(vm, exit_type, strlen(exit_type));
    nova_vm_push_integer(vm, (nova_int_t)code);
    return 3;
}

/* ============================================================
 * EXIT
 * ============================================================ */

static int nova_os_exit(NovaVM *vm) {
    int nargs = nova_vm_get_top(vm);
    int code = 0;

    if (nargs >= 1) {
        NovaValue v = nova_vm_get(vm, 0);
        if (nova_is_bool(v)) {
            code = nova_as_bool(v) ? 0 : 1;
        } else if (nova_is_integer(v)) {
            code = (int)nova_as_integer(v);
        }
    }

    exit(code);
    return 0;  /* Unreachable */
}

/* ============================================================
 * REMOVE / RENAME
 * ============================================================ */

static int nova_os_remove(NovaVM *vm) {
    if (nova_lib_check_args(vm, 1) != 0) {
        return -1;
    }

    const char *path = nova_lib_check_string(vm, 0);
    if (path == NULL) return -1;

    if (remove(path) != 0) {
        nova_vm_push_nil(vm);
        nova_vm_push_string(vm, strerror(errno), strlen(strerror(errno)));
        return 2;
    }

    nova_vm_push_bool(vm, 1);
    return 1;
}

static int nova_os_rename(NovaVM *vm) {
    if (nova_lib_check_args(vm, 2) != 0) {
        return -1;
    }

    const char *old_name = nova_lib_check_string(vm, 0);
    const char *new_name = nova_lib_check_string(vm, 1);
    if (old_name == NULL || new_name == NULL) return -1;

    if (rename(old_name, new_name) != 0) {
        nova_vm_push_nil(vm);
        nova_vm_push_string(vm, strerror(errno), strlen(strerror(errno)));
        return 2;
    }

    nova_vm_push_bool(vm, 1);
    return 1;
}

/* ============================================================
 * TMPNAME
 * ============================================================ */

static int nova_os_tmpname(NovaVM *vm) {
    char buf[L_tmpnam + 1];
    char *name = tmpnam(buf);
    if (name == NULL) {
        nova_vm_push_nil(vm);
        return 1;
    }
    nova_vm_push_string(vm, name, strlen(name));
    return 1;
}

/* ============================================================
 * SETLOCALE (stub)
 * ============================================================ */

static int nova_os_setlocale(NovaVM *vm) {
    int nargs = nova_vm_get_top(vm);
    const char *locale = "";
    int category = 0;  /* LC_ALL */

    if (nargs >= 1) {
        NovaValue lv = nova_vm_get(vm, 0);
        if (nova_is_string(lv)) {
            locale = nova_str_data(nova_as_string(lv));
        }
    }

    (void)category;  /* TODO: Parse category string */

    const char *result = setlocale(0, locale);
    if (result == NULL) {
        nova_vm_push_nil(vm);
    } else {
        nova_vm_push_string(vm, result, strlen(result));
    }
    return 1;
}

/* ============================================================
 * REGISTRATION TABLE
 * ============================================================ */

static const NovaLibReg nova_os_lib[] = {
    {"clock",      nova_os_clock},
    {"time",       nova_os_time},
    {"date",       nova_os_date},
    {"difftime",   nova_os_difftime},
    {"getenv",     nova_os_getenv},
    {"execute",    nova_os_execute},
    {"exit",       nova_os_exit},
    {"remove",     nova_os_remove},
    {"rename",     nova_os_rename},
    {"tmpname",    nova_os_tmpname},
    {"setlocale",  nova_os_setlocale},
    {NULL,         NULL}
};

/* ============================================================
 * MODULE OPENER
 * ============================================================ */

int nova_open_os(NovaVM *vm) {
    if (vm == NULL) {
        return -1;
    }

    nova_lib_register_module(vm, "os", nova_os_lib);
    return 0;
}
