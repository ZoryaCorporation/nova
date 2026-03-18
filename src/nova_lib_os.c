/**
 * @file nova_lib_os.c
 * @brief Nova Language - OS Standard Library
 *
 * Provides operating system functions as the "os" module.
 * Cross-platform: POSIX (Linux/macOS) and Windows.
 *
 * Functions:
 *   os.clock()              CPU time used (seconds)
 *   os.time([t])            Current time as integer
 *   os.date([fmt [,t]])     Formatted date string
 *   os.difftime(t2, t1)     Difference in seconds
 *   os.getenv(name)         Get environment variable
 *   os.setenv(name, value)  Set environment variable
 *   os.unsetenv(name)       Remove environment variable
 *   os.env()                All env vars as table
 *   os.execute(cmd)         Execute shell command
 *   os.capture(cmd)         Execute and capture stdout
 *   os.exit([code])         Exit the process
 *   os.remove(filename)     Remove a file
 *   os.rename(old, new)     Rename a file
 *   os.tmpname()            Generate temporary file name
 *   os.setlocale(l [,c])    Set locale
 *   os.platform()           "linux", "darwin", "windows"
 *   os.arch()               "x86_64", "aarch64", etc.
 *   os.hostname()           Machine hostname
 *   os.homedir()            User home directory
 *   os.tmpdir()             System temp directory
 *   os.cwd()                Current working directory
 *   os.chdir(path)          Change working directory
 *   os.pid()                Current process ID
 *   os.sleep(seconds)       Sleep (fractional seconds OK)
 *   os.which(cmd)           Find executable in PATH
 *
 * @author Anthony Taliento
 * @date 2026-02-08
 * @version 0.2.0
 *
 * @copyright Copyright (c) 2026 Zorya Corporation
 * @license MIT
 *
 * ZORYA-C COMPLIANCE: v2.0.0
 */

#include "nova/nova_lib.h"
#include "nova/nova_vm.h"

#include <zorya/pal.h>     /* zorya_tmpname() — safe temp file generation */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <locale.h>

/* ============================================================
 * CROSS-PLATFORM HEADERS & COMPAT
 *
 * Platform-portable calls (getcwd, chdir, popen, pclose, setenv,
 * unsetenv, sleep, path separator) go through <zorya/pal.h>.
 * Only items without a PAL equivalent remain in the #ifdef.
 * ============================================================ */

#ifdef _WIN32
    #define WIN32_LEAN_AND_MEAN
    #include <windows.h>       /* GetComputerNameA, GetTempPathA, MAX_PATH */
    #include <process.h>       /* _getpid */
    #include <io.h>            /* _access */
    #define nova_getpid    _getpid
    #define NOVA_PATH_DELIM ';'
    /* _environ is a macro provided by <stdlib.h> on MSYS2/UCRT */
    #define nova_environ   _environ
#else
    #include <unistd.h>
    #include <sys/stat.h>
    #include <sys/types.h>
    #include <sys/wait.h>
    #define nova_getpid    getpid
    #define NOVA_PATH_DELIM ':'
    extern char **environ;
    #define nova_environ   environ
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
#ifdef _WIN32
    (void)code;
    code = status;
#elif defined(__linux__) || defined(__APPLE__)
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
    char buf[ZORYA_MAX_PATH];
    if (zorya_tmpname(buf, sizeof(buf)) != 0) {
        nova_vm_push_nil(vm);
        return 1;
    }
    nova_vm_push_string(vm, buf, strlen(buf));
    return 1;
}

/* ============================================================
 * SETLOCALE
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
 * PLATFORM / ARCH / HOSTNAME
 * ============================================================ */

/**
 * @brief os.platform() - Returns the OS name.
 * @return "linux", "darwin", "windows", or "unknown".
 */
static int nova_os_platform(NovaVM *vm) {
    const char *p;
#if defined(_WIN32)
    p = "windows";
#elif defined(__APPLE__)
    p = "darwin";
#elif defined(__linux__)
    p = "linux";
#elif defined(__FreeBSD__)
    p = "freebsd";
#elif defined(__OpenBSD__)
    p = "openbsd";
#elif defined(__NetBSD__)
    p = "netbsd";
#else
    p = "unknown";
#endif
    nova_vm_push_string(vm, p, strlen(p));
    return 1;
}

/**
 * @brief os.arch() - Returns the CPU architecture.
 * @return "x86_64", "aarch64", "i386", "arm", or "unknown".
 */
static int nova_os_arch(NovaVM *vm) {
    const char *a;
#if defined(__x86_64__) || defined(_M_X64)
    a = "x86_64";
#elif defined(__aarch64__) || defined(_M_ARM64)
    a = "aarch64";
#elif defined(__i386__) || defined(_M_IX86)
    a = "i386";
#elif defined(__arm__) || defined(_M_ARM)
    a = "arm";
#elif defined(__riscv)
    a = "riscv64";
#elif defined(__ppc64__)
    a = "ppc64";
#else
    a = "unknown";
#endif
    nova_vm_push_string(vm, a, strlen(a));
    return 1;
}

/**
 * @brief os.hostname() - Returns the machine hostname.
 */
static int nova_os_hostname(NovaVM *vm) {
    char buf[256];
#ifdef _WIN32
    DWORD size = (DWORD)sizeof(buf);
    if (GetComputerNameA(buf, &size)) {
        nova_vm_push_string(vm, buf, (size_t)size);
    } else {
        nova_vm_push_nil(vm);
    }
#else
    if (gethostname(buf, sizeof(buf)) == 0) {
        buf[sizeof(buf) - 1] = '\0';
        nova_vm_push_string(vm, buf, strlen(buf));
    } else {
        nova_vm_push_nil(vm);
    }
#endif
    return 1;
}

/* ============================================================
 * CWD / CHDIR
 * ============================================================ */

/**
 * @brief os.cwd() - Get the current working directory.
 */
static int nova_os_cwd(NovaVM *vm) {
    char buf[4096];
    if (zorya_getcwd(buf, sizeof(buf)) == 0) {
        nova_vm_push_string(vm, buf, strlen(buf));
    } else {
        nova_vm_push_nil(vm);
        nova_vm_push_string(vm, strerror(errno), strlen(strerror(errno)));
        return 2;
    }
    return 1;
}

/**
 * @brief os.chdir(path) - Change the current working directory.
 */
static int nova_os_chdir(NovaVM *vm) {
    if (nova_lib_check_args(vm, 1) != 0) {
        return -1;
    }

    const char *path = nova_lib_check_string(vm, 0);
    if (path == NULL) return -1;

    if (zorya_chdir(path) == 0) {
        nova_vm_push_bool(vm, 1);
        return 1;
    }

    nova_vm_push_nil(vm);
    nova_vm_push_string(vm, strerror(errno), strlen(strerror(errno)));
    return 2;
}

/* ============================================================
 * HOMEDIR / TMPDIR
 * ============================================================ */

/**
 * @brief os.homedir() - Get the user's home directory.
 */
static int nova_os_homedir(NovaVM *vm) {
#ifdef _WIN32
    const char *home = getenv("USERPROFILE");
    if (home == NULL) {
        const char *drive = getenv("HOMEDRIVE");
        const char *hpath = getenv("HOMEPATH");
        if (drive != NULL && hpath != NULL) {
            char buf[4096];
            snprintf(buf, sizeof(buf), "%s%s", drive, hpath);
            nova_vm_push_string(vm, buf, strlen(buf));
            return 1;
        }
        nova_vm_push_nil(vm);
        return 1;
    }
#else
    const char *home = getenv("HOME");
    if (home == NULL) {
        nova_vm_push_nil(vm);
        return 1;
    }
#endif
    nova_vm_push_string(vm, home, strlen(home));
    return 1;
}

/**
 * @brief os.tmpdir() - Get the system temporary directory.
 */
static int nova_os_tmpdir(NovaVM *vm) {
#ifdef _WIN32
    char buf[MAX_PATH + 1];
    DWORD len = GetTempPathA(MAX_PATH + 1, buf);
    if (len > 0 && len <= MAX_PATH) {
        /* Remove trailing backslash */
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
 * SETENV / UNSETENV / ENV
 * ============================================================ */

/**
 * @brief os.setenv(name, value) - Set an environment variable.
 */
static int nova_os_setenv(NovaVM *vm) {
    if (nova_lib_check_args(vm, 2) != 0) {
        return -1;
    }

    const char *name = nova_lib_check_string(vm, 0);
    const char *value = nova_lib_check_string(vm, 1);
    if (name == NULL || value == NULL) return -1;

    int rc = zorya_setenv(name, value);

    if (rc == 0) {
        nova_vm_push_bool(vm, 1);
    } else {
        nova_vm_push_nil(vm);
        nova_vm_push_string(vm, strerror(errno), strlen(strerror(errno)));
        return 2;
    }
    return 1;
}

/**
 * @brief os.unsetenv(name) - Remove an environment variable.
 */
static int nova_os_unsetenv(NovaVM *vm) {
    if (nova_lib_check_args(vm, 1) != 0) {
        return -1;
    }

    const char *name = nova_lib_check_string(vm, 0);
    if (name == NULL) return -1;

    int rc = zorya_unsetenv(name);

    if (rc == 0) {
        nova_vm_push_bool(vm, 1);
    } else {
        nova_vm_push_nil(vm);
        nova_vm_push_string(vm, strerror(errno), strlen(strerror(errno)));
        return 2;
    }
    return 1;
}

/**
 * @brief os.env() - Get all environment variables as a table.
 * @return Table with key=value pairs for every env var.
 */
static int nova_os_env_all(NovaVM *vm) {
    nova_vm_push_table(vm);
    int tidx = nova_vm_get_top(vm) - 1;

    if (nova_environ != NULL) {
        for (int i = 0; nova_environ[i] != NULL; i++) {
            const char *entry = nova_environ[i];
            const char *eq = strchr(entry, '=');
            if (eq != NULL) {
                size_t klen = (size_t)(eq - entry);
                const char *val = eq + 1;
                size_t vlen = strlen(val);

                /* Push key and value, set field */
                /* We need to create a temporary key string */
                char key[512];
                if (klen >= sizeof(key)) klen = sizeof(key) - 1;
                memcpy(key, entry, klen);
                key[klen] = '\0';

                nova_vm_push_string(vm, val, vlen);
                nova_vm_set_field(vm, tidx, key);
            }
        }
    }

    return 1;
}

/* ============================================================
 * PID / SLEEP
 * ============================================================ */

/**
 * @brief os.pid() - Get the current process ID.
 */
static int nova_os_pid(NovaVM *vm) {
    nova_vm_push_integer(vm, (nova_int_t)nova_getpid());
    return 1;
}

/**
 * @brief os.sleep(seconds) - Sleep for N seconds.
 *
 * Accepts fractional seconds (e.g., os.sleep(0.1) for 100ms).
 */
static int nova_os_sleep(NovaVM *vm) {
    if (nova_lib_check_args(vm, 1) != 0) {
        return -1;
    }

    NovaValue v = nova_vm_get(vm, 0);
    double seconds = 0.0;

    if (nova_is_number(v)) {
        seconds = nova_as_number(v);
    } else if (nova_is_integer(v)) {
        seconds = (double)nova_as_integer(v);
    } else {
        nova_vm_raise_error(vm, "os.sleep: expected number");
        return -1;
    }

    if (seconds <= 0.0) {
        nova_vm_push_bool(vm, 1);
        return 1;
    }

    zorya_sleep_ms((unsigned int)(seconds * 1000.0));

    nova_vm_push_bool(vm, 1);
    return 1;
}

/* ============================================================
 * WHICH (find executable in PATH)
 * ============================================================ */

/**
 * @brief os.which(cmd) - Find an executable in PATH.
 *
 * Searches the PATH environment variable for the given command.
 * Returns the full path if found, nil otherwise.
 * On Windows, also checks common extensions (.exe, .cmd, .bat).
 */
static int nova_os_which(NovaVM *vm) {
    if (nova_lib_check_args(vm, 1) != 0) {
        return -1;
    }

    const char *cmd = nova_lib_check_string(vm, 0);
    if (cmd == NULL) return -1;

    const char *path_env = getenv("PATH");
    if (path_env == NULL) {
        nova_vm_push_nil(vm);
        return 1;
    }

    /* Copy PATH so we can tokenize it */
    size_t plen = strlen(path_env);
    char *path_copy = (char *)malloc(plen + 1);
    if (path_copy == NULL) {
        nova_vm_push_nil(vm);
        return 1;
    }
    memcpy(path_copy, path_env, plen + 1);

    char full[4096];

#ifdef _WIN32
    /* Windows: check with common extensions */
    static const char *exts[] = { "", ".exe", ".cmd", ".bat", ".com", NULL };
#else
    static const char *exts[] = { "", NULL };
#endif

    char *saveptr = NULL;
    char *dir = NULL;

    /* Manual tokenization for cross-platform safety */
    dir = path_copy;
    for (;;) {
        /* Find next delimiter */
        char *delim = strchr(dir, NOVA_PATH_DELIM);
        if (delim != NULL) {
            *delim = '\0';
        }

        if (strlen(dir) > 0) {
            for (int e = 0; exts[e] != NULL; e++) {
                snprintf(full, sizeof(full), "%s%c%s%s",
                         dir, zorya_path_sep(), cmd, exts[e]);
#ifdef _WIN32
                if (_access(full, 0) == 0) {
#else
                if (access(full, X_OK) == 0) {
#endif
                    nova_vm_push_string(vm, full, strlen(full));
                    free(path_copy);
                    return 1;
                }
            }
        }

        if (delim == NULL) break;
        dir = delim + 1;
    }

    (void)saveptr;
    free(path_copy);
    nova_vm_push_nil(vm);
    return 1;
}

/* ============================================================
 * CAPTURE (run command, return stdout)
 * ============================================================ */

/**
 * @brief os.capture(cmd) - Execute command and capture stdout.
 *
 * Returns stdout_string, exit_code.
 * On failure, returns nil, error_string.
 */
static int nova_os_capture(NovaVM *vm) {
    if (nova_lib_check_args(vm, 1) != 0) {
        return -1;
    }

    const char *cmd = nova_lib_check_string(vm, 0);
    if (cmd == NULL) return -1;

    FILE *fp = zorya_popen(cmd, "r");
    if (fp == NULL) {
        nova_vm_push_nil(vm);
        nova_vm_push_string(vm, strerror(errno), strlen(strerror(errno)));
        return 2;
    }

    /* Read all output into a growable buffer */
    size_t cap = 4096;
    size_t len = 0;
    char *buf = (char *)malloc(cap);
    if (buf == NULL) {
        zorya_pclose(fp);
        nova_vm_push_nil(vm);
        const char *msg = "out of memory";
        nova_vm_push_string(vm, msg, strlen(msg));
        return 2;
    }

    for (;;) {
        if (len + 1024 > cap) {
            cap *= 2;
            char *nb = (char *)realloc(buf, cap);
            if (nb == NULL) {
                free(buf);
                zorya_pclose(fp);
                nova_vm_push_nil(vm);
                const char *msg = "out of memory";
                nova_vm_push_string(vm, msg, strlen(msg));
                return 2;
            }
            buf = nb;
        }
        size_t got = fread(buf + len, 1, 1024, fp);
        if (got == 0) break;
        len += got;
    }

    int status = zorya_pclose(fp);
    int code = status;
#if !defined(_WIN32) && (defined(__linux__) || defined(__APPLE__))
    if (WIFEXITED(status)) {
        code = WEXITSTATUS(status);
    }
#endif

    /* Trim trailing newline for convenience */
    while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r')) {
        len--;
    }

    nova_vm_push_string(vm, buf, len);
    nova_vm_push_integer(vm, (nova_int_t)code);
    free(buf);
    return 2;
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
    {"setenv",     nova_os_setenv},
    {"unsetenv",   nova_os_unsetenv},
    {"env",        nova_os_env_all},
    {"execute",    nova_os_execute},
    {"capture",    nova_os_capture},
    {"exit",       nova_os_exit},
    {"remove",     nova_os_remove},
    {"rename",     nova_os_rename},
    {"tmpname",    nova_os_tmpname},
    {"setlocale",  nova_os_setlocale},
    {"platform",   nova_os_platform},
    {"arch",       nova_os_arch},
    {"hostname",   nova_os_hostname},
    {"homedir",    nova_os_homedir},
    {"tmpdir",     nova_os_tmpdir},
    {"cwd",        nova_os_cwd},
    {"chdir",      nova_os_chdir},
    {"pid",        nova_os_pid},
    {"sleep",      nova_os_sleep},
    {"which",      nova_os_which},
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

    /* Set os.sep — path separator for the current platform */
    NovaValue os_tbl = nova_vm_get_global(vm, "os");
    if (nova_is_table(os_tbl)) {
        nova_vm_push_value(vm, os_tbl);
        int tidx = nova_vm_get_top(vm) - 1;

        char sep[2] = { zorya_path_sep(), '\0' };
        nova_vm_push_string(vm, sep, 1);
        nova_vm_set_field(vm, tidx, "sep");

        nova_vm_set_top(vm, tidx);
    }

    return 0;
}
