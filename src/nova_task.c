/**
 * @file nova_task.c
 * @brief Nova Language - NINI Task Runner
 *
 * NINI-based universal build orchestrator. Reads taskfile.nini
 * from the current or parent directory, decodes [task:*] sections,
 * resolves dependencies, and executes shell commands.
 *
 * Usage:
 *   nova task                 List available tasks
 *   nova task build           Run the 'build' task
 *   nova task build test      Run 'build' then 'test'
 *   nova task --dry build     Dry run (print commands only)
 *
 * Task properties:
 *   command         = <shell command>
 *   cmds[]          = <cmd>   (multi-line alternative to command)
 *   description     = <human-readable text>
 *   depends         = [dep1, dep2]  (run before this task)
 *   dir             = <working directory>
 *   shell           = <shell path>  (default: /bin/sh)
 *   silent          = true|false
 *   ignore_error    = true|false
 *   platforms       = [linux, macos, windows]
 *   timeout         = <seconds>
 *   pre             = <task name>   (run before)
 *   post            = <task name>   (run after)
 *   env.KEY         = <value>       (set environment variable)
 *   dotenv          = <file>        (load .env file)
 *
 * Global sections:
 *   [env]              Global env vars inherited by all tasks
 *   [options]          Runner configuration
 *     default = <task>   Default task when none specified
 *     shell   = <path>   Default shell for all tasks
 *     dotenv  = <file>   Global .env file
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

#include "nova/nova_task.h"
#include "nova/nova_conf.h"
#include "nova/nova_vm.h"
#include "nova/nova_nini.h"
#include "nova/nova_error.h"

#include <zorya/pal.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#ifdef _WIN32
    #include <direct.h>
    #define novai_chdir(p)      _chdir(p)
    #define novai_getcwd(b,n)   _getcwd((b),(int)(n))
#else
    #include <unistd.h>
    #include <sys/wait.h>
    #define novai_chdir(p)      chdir(p)
    #define novai_getcwd(b,n)   getcwd((b),(n))
#endif

/* ============================================================
 * CONSTANTS
 * ============================================================ */

/** Default taskfile name */
#define NOVAI_TASKFILE_NAME  "taskfile.nini"

/** Maximum dependency chain depth (cycle protection) */
#define NOVAI_TASK_MAX_DEPTH 32

/** Maximum unique tasks that can run in one invocation */
#define NOVAI_TASK_MAX_VISITED 128

/** Maximum parent directories to search for taskfile */
#define NOVAI_TASK_MAX_SEARCH 16

/** Path buffer size */
#define NOVAI_PATH_MAX 4096

/* Color helpers: respect --no-color / NO_COLOR via the diagnostic system.
 * Uses the extended 256-color palette for the Nova brand aesthetic.     */
#define TC_RED    (nova_diag_color_enabled() ? "\033[1;38;5;196m" : "")
#define TC_GREEN  (nova_diag_color_enabled() ? "\033[1;38;5;40m"  : "")
#define TC_YELLOW (nova_diag_color_enabled() ? "\033[1;38;5;214m" : "")
#define TC_CYAN   (nova_diag_color_enabled() ? "\033[1;38;5;75m"  : "")
#define TC_HUNTER (nova_diag_color_enabled() ? "\033[1;38;5;22m"  : "")
#define TC_BOLD   (nova_diag_color_enabled() ? "\033[1m"          : "")
#define TC_DIM    (nova_diag_color_enabled() ? "\033[38;5;245m"   : "")
#define TC_RESET  (nova_diag_color_enabled() ? "\033[0m"          : "")

/** Environment variable backup for task env save/restore */
typedef struct {
    char key[128];
    char old_val[512];
    int  had_old;
} NovaiEnvBackup;

#define NOVAI_ENV_BACKUP_MAX 32

/* ============================================================
 * INTERNAL HELPERS
 * ============================================================ */

/**
 * @brief Read an entire file into a heap buffer.
 *
 * @param path     File path
 * @param out_len  Output: bytes read
 * @return Heap buffer (caller frees), or NULL on error
 */
static char *novai_task_read_file(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (f == NULL) { return NULL; }

    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return NULL; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }

    char *buf = (char *)malloc((size_t)sz + 1);
    if (buf == NULL) { fclose(f); return NULL; }

    size_t nread = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[nread] = '\0';
    if (out_len != NULL) { *out_len = nread; }
    return buf;
}

/**
 * @brief Get a string value from a task table.
 */
static const char *novai_task_get_str(NovaTable *t, const char *key) {
    if (t == NULL || key == NULL) { return NULL; }
    NovaValue v = nova_table_get_cstr(t, key, (uint32_t)strlen(key));
    if (nova_is_string(v)) {
        return nova_str_data(nova_as_string(v));
    }
    return NULL;
}

/**
 * @brief Get a boolean value from a task table.
 */
static int novai_task_get_bool(NovaTable *t, const char *key,
                               int default_val) {
    if (t == NULL || key == NULL) { return default_val; }
    NovaValue v = nova_table_get_cstr(t, key, (uint32_t)strlen(key));
    if (nova_is_bool(v)) { return nova_as_bool(v); }
    return default_val;
}

/**
 * @brief Get an integer value from a task table.
 */
static nova_int_t novai_task_get_int(NovaTable *t, const char *key,
                                     nova_int_t default_val) {
    if (t == NULL || key == NULL) { return default_val; }
    NovaValue v = nova_table_get_cstr(t, key, (uint32_t)strlen(key));
    if (nova_is_integer(v)) { return nova_as_integer(v); }
    return default_val;
}

/**
 * @brief Detect the current platform name.
 *
 * Returns one of: "linux", "macos", "windows", "freebsd", "unknown".
 */
static const char *novai_task_platform(void) {
#if defined(_WIN32) || defined(_WIN64)
    return "windows";
#elif defined(__APPLE__) && defined(__MACH__)
    return "macos";
#elif defined(__FreeBSD__)
    return "freebsd";
#elif defined(__linux__)
    return "linux";
#else
    return "unknown";
#endif
}

/**
 * @brief Check if a task should run on the current platform.
 *
 * If the task has no `platforms` key, it runs on all platforms.
 * Otherwise, the current platform must be in the array.
 */
static int novai_task_platform_ok(NovaTable *task_tbl) {
    NovaValue pv = nova_table_get_cstr(task_tbl, "platforms", 9);
    if (nova_is_nil(pv)) { return 1; } /* No filter → always run */

    if (nova_is_table(pv)) {
        const char *cur = novai_task_platform();
        NovaTable *parr = nova_as_table(pv);
        for (nova_int_t i = 0; ; i++) {
            NovaValue elem = nova_table_get_int(parr, i);
            if (nova_is_nil(elem)) { break; }
            if (nova_is_string(elem)) {
                if (strcmp(nova_str_data(nova_as_string(elem)), cur) == 0) {
                    return 1;
                }
            }
        }
        return 0; /* Platform not in list */
    }
    return 1;
}

/**
 * @brief Load a .env file and set environment variables.
 *
 * Format: KEY=VALUE (one per line, # comments, blank lines skip).
 * Supports optional quoting: KEY="value" or KEY='value'.
 */
static void novai_task_load_dotenv(const char *path) {
    if (path == NULL) { return; }
    FILE *f = fopen(path, "r");
    if (f == NULL) { return; } /* Missing .env is not an error */

    char line[1024];
    while (fgets(line, (int)sizeof(line), f) != NULL) {
        /* Strip newline */
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
            line[--len] = '\0';
        }
        /* Skip blanks + comments */
        if (len == 0 || line[0] == '#') { continue; }

        char *eq = strchr(line, '=');
        if (eq == NULL) { continue; }
        *eq = '\0';
        char *key = line;
        char *val = eq + 1;

        /* Strip optional quotes from value */
        size_t vlen = strlen(val);
        if (vlen >= 2 && ((val[0] == '"' && val[vlen - 1] == '"') ||
                          (val[0] == '\'' && val[vlen - 1] == '\''))) {
            val[vlen - 1] = '\0';
            val++;
        }

        zorya_setenv(key, val);
    }
    fclose(f);
}

/**
 * @brief Save current env vars set by a task's env.* block.
 *
 * Stores the old values so they can be restored after execution.
 */
static int novai_task_set_env(NovaTable *task_tbl,
                              NovaiEnvBackup *backup,
                              int *backup_count) {
    NovaValue env_val = nova_table_get_cstr(task_tbl, "env", 3);
    if (!nova_is_table(env_val)) { return 0; }

    NovaTable *env_tbl = nova_as_table(env_val);
    uint32_t iter = 0;
    NovaValue ekey, eval;
    while (nova_table_next(env_tbl, &iter, &ekey, &eval)) {
        if (!nova_is_string(ekey) || !nova_is_string(eval)) { continue; }

        const char *k = nova_str_data(nova_as_string(ekey));
        const char *v = nova_str_data(nova_as_string(eval));

        /* Backup old value */
        if (*backup_count < NOVAI_ENV_BACKUP_MAX) {
            NovaiEnvBackup *b = &backup[*backup_count];
            snprintf(b->key, sizeof(b->key), "%s", k);
            const char *old = getenv(k);
            if (old != NULL) {
                snprintf(b->old_val, sizeof(b->old_val), "%s", old);
                b->had_old = 1;
            } else {
                b->old_val[0] = '\0';
                b->had_old = 0;
            }
            (*backup_count)++;
        }

        zorya_setenv(k, v);
    }
    return 0;
}

/**
 * @brief Restore environment variables from backup.
 */
static void novai_task_restore_env(const NovaiEnvBackup *backup,
                                   int backup_count) {
    for (int i = 0; i < backup_count; i++) {
        if (backup[i].had_old) {
            zorya_setenv(backup[i].key, backup[i].old_val);
        } else {
            zorya_unsetenv(backup[i].key);
        }
    }
}

/**
 * @brief Set global [env] section vars into the process environment.
 */
static void novai_task_set_global_env(NovaTable *root) {
    NovaValue env_val = nova_table_get_cstr(root, "env", 3);
    if (!nova_is_table(env_val)) { return; }

    NovaTable *env_tbl = nova_as_table(env_val);
    uint32_t iter = 0;
    NovaValue ekey, eval;
    while (nova_table_next(env_tbl, &iter, &ekey, &eval)) {
        if (nova_is_string(ekey) && nova_is_string(eval)) {
            /* Only set if not already present (don't overwrite existing) */
            const char *existing = zorya_getenv(nova_str_data(nova_as_string(ekey)));
            if (existing == NULL) {
                zorya_setenv(nova_str_data(nova_as_string(ekey)),
                             nova_str_data(nova_as_string(eval)));
            }
        }
    }
}

/* Forward declaration for dependency recursion */
static int novai_task_run_one(NovaVM *vm, NovaTable *root, NovaTable *tasks,
                              const char *name, int depth,
                              const char *visited[], int *visited_count,
                              int dry_run, const char *default_shell);

/**
 * @brief Run dependencies for a task.
 */
static int novai_task_run_deps(NovaVM *vm, NovaTable *root, NovaTable *tasks,
                               NovaTable *task_tbl, int depth,
                               const char *visited[],
                               int *visited_count, int dry_run,
                               const char *default_shell) {
    NovaValue deps = nova_table_get_cstr(task_tbl, "depends", 7);
    if (nova_is_nil(deps)) { return 0; }

    if (nova_is_table(deps)) {
        NovaTable *dep_arr = nova_as_table(deps);
        for (nova_int_t i = 0; ; i++) {
            NovaValue dep_val = nova_table_get_int(dep_arr, i);
            if (nova_is_nil(dep_val)) { break; }
            if (!nova_is_string(dep_val)) { continue; }

            const char *dep_name = nova_str_data(nova_as_string(dep_val));
            int rc = novai_task_run_one(vm, root, tasks, dep_name, depth + 1,
                                        visited, visited_count,
                                        dry_run, default_shell);
            if (rc != 0) { return rc; }
        }
    }
    return 0;
}

/**
 * @brief Execute a shell command, optionally with a custom shell.
 *
 * @param command  The command string
 * @param shell    Shell to use (NULL → system() default /bin/sh)
 * @param timeout  Timeout in seconds (0 → no timeout)
 * @return Exit code
 */
static int novai_task_exec(const char *command, const char *shell,
                           nova_int_t timeout) {
    int status;

    if (shell != NULL && strcmp(shell, "/bin/sh") != 0) {
        /* Custom shell: fork/exec shell -c "command" */
        char cmd_buf[4096];
        snprintf(cmd_buf, sizeof(cmd_buf), "%s -c '%s'", shell, command);
        status = system(cmd_buf);
    } else {
        status = system(command);
    }

    (void)timeout; /* TODO: implement alarm-based timeout in future */

#ifdef _WIN32
    return status;
#else
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
#endif
}

/**
 * @brief Run a single task with full feature support.
 *
 * Features: dependency resolution, platform filter, env save/restore,
 * dir change, shell selection, multi-command (cmds[]), ignore_error,
 * pre/post hooks, dry-run, and timeout.
 */
static int novai_task_run_one(NovaVM *vm, NovaTable *root, NovaTable *tasks,
                              const char *name, int depth,
                              const char *visited[], int *visited_count,
                              int dry_run, const char *default_shell) {
    if (name == NULL) { return 1; }

    if (depth > NOVAI_TASK_MAX_DEPTH) {
        fprintf(stderr, "%serror:%s task dependency chain "
                "too deep (max %d)\n", TC_RED, TC_RESET, NOVAI_TASK_MAX_DEPTH);
        return 1;
    }

    /* Skip if already executed (handles diamonds + cycles) */
    for (int i = 0; i < *visited_count; i++) {
        if (strcmp(visited[i], name) == 0) {
            return 0;
        }
    }

    /* Look up task in __tasks table */
    NovaValue task_val = nova_table_get_cstr(tasks, name,
                                             (uint32_t)strlen(name));
    if (nova_is_nil(task_val) || !nova_is_table(task_val)) {
        fprintf(stderr, "%serror:%s unknown task: "
                "%s%s%s\n", TC_RED, TC_RESET, TC_BOLD, name, TC_RESET);
        return 1;
    }
    NovaTable *task_tbl = nova_as_table(task_val);

    /* Platform filter */
    if (!novai_task_platform_ok(task_tbl)) {
        printf("%s\xe2\x97\x8b task:%s %s%s%s  %sskipped (platform: %s)%s\n",
               TC_DIM, TC_RESET, TC_BOLD, name, TC_RESET,
               TC_DIM, novai_task_platform(), TC_RESET);
        /* Mark visited so deps don't re-try */
        if (*visited_count < NOVAI_TASK_MAX_VISITED) {
            visited[*visited_count] = name;
            (*visited_count)++;
        }
        return 0;
    }

    /* Run 'pre' hook task */
    const char *pre_task = novai_task_get_str(task_tbl, "pre");
    if (pre_task != NULL) {
        int rc = novai_task_run_one(vm, root, tasks, pre_task, depth + 1,
                                    visited, visited_count,
                                    dry_run, default_shell);
        if (rc != 0) { return rc; }
    }

    /* Run dependencies */
    int rc = novai_task_run_deps(vm, root, tasks, task_tbl, depth,
                                 visited, visited_count,
                                 dry_run, default_shell);
    if (rc != 0) { return rc; }

    /* Mark as visited */
    if (*visited_count < NOVAI_TASK_MAX_VISITED) {
        visited[*visited_count] = name;
        (*visited_count)++;
    }

    /* Extract task properties */
    const char *command = novai_task_get_str(task_tbl, "command");
    const char *dir     = novai_task_get_str(task_tbl, "dir");
    const char *desc    = novai_task_get_str(task_tbl, "description");
    const char *shell   = novai_task_get_str(task_tbl, "shell");
    const char *dotenv  = novai_task_get_str(task_tbl, "dotenv");
    int silent          = novai_task_get_bool(task_tbl, "silent", 0);
    int ignore_error    = novai_task_get_bool(task_tbl, "ignore_error", 0);
    nova_int_t timeout  = novai_task_get_int(task_tbl, "timeout", 0);

    if (shell == NULL) { shell = default_shell; }

    /* Check for multi-command (cmds array) */
    NovaValue cmds_val = nova_table_get_cstr(task_tbl, "cmds", 4);
    int has_cmds = nova_is_table(cmds_val);

    if (command == NULL && !has_cmds) {
        if (!silent) {
            fprintf(stderr, "%swarning:%s task '%s' "
                    "has no command\n", TC_YELLOW, TC_RESET, name);
        }
        return 0;
    }

    /* Load task-level .env file */
    if (dotenv != NULL) {
        novai_task_load_dotenv(dotenv);
    }

    /* Set environment variables (with backup for restore) */
    NovaiEnvBackup env_backup[NOVAI_ENV_BACKUP_MAX];
    int env_backup_count = 0;
    novai_task_set_env(task_tbl, env_backup, &env_backup_count);

    /* Change directory if specified */
    char saved_cwd[NOVAI_PATH_MAX];
    saved_cwd[0] = '\0';
    if (dir != NULL) {
        if (novai_getcwd(saved_cwd, sizeof(saved_cwd)) == NULL) {
            saved_cwd[0] = '\0';
        }
        if (novai_chdir(dir) != 0) {
            fprintf(stderr, "%serror:%s cannot chdir "
                    "to '%s': %s\n", TC_RED, TC_RESET, dir, strerror(errno));
            novai_task_restore_env(env_backup, env_backup_count);
            return 1;
        }
    }

    /* Print task header */
    if (!silent) {
        printf("%s\xe2\x96\xb8 task:%s %s%s%s",
               TC_HUNTER, TC_RESET, TC_BOLD, name, TC_RESET);
        if (desc != NULL) {
            printf("  %s\xe2\x80\x94 %s%s", TC_DIM, desc, TC_RESET);
        }
        printf("\n");
    }

    /* Execute commands */
    int exit_code = 0;

    if (has_cmds) {
        /* Multi-command mode (cmds[]) */
        NovaTable *cmds_arr = nova_as_table(cmds_val);
        for (nova_int_t i = 0; ; i++) {
            NovaValue cv = nova_table_get_int(cmds_arr, i);
            if (nova_is_nil(cv)) { break; }
            if (!nova_is_string(cv)) { continue; }

            const char *cmd = nova_str_data(nova_as_string(cv));
            if (!silent) {
                printf("  %s$ %s%s\n", TC_DIM, cmd, TC_RESET);
            }

            if (dry_run) { continue; }

            exit_code = novai_task_exec(cmd, shell, timeout);
            if (exit_code != 0) {
                if (ignore_error) {
                    if (!silent) {
                        printf("  %s\xe2\x9a\xa0 command failed "
                               "(exit %d, ignored)%s\n",
                               TC_YELLOW, exit_code, TC_RESET);
                    }
                    exit_code = 0;
                } else {
                    break;
                }
            }
        }
    } else {
        /* Single command mode */
        if (!silent) {
            printf("  %s$ %s%s\n", TC_DIM, command, TC_RESET);
        }

        if (!dry_run) {
            exit_code = novai_task_exec(command, shell, timeout);
            if (exit_code != 0 && ignore_error) {
                if (!silent) {
                    printf("  %s\xe2\x9a\xa0 command failed "
                           "(exit %d, ignored)%s\n",
                           TC_YELLOW, exit_code, TC_RESET);
                }
                exit_code = 0;
            }
        }
    }

    /* Restore directory */
    if (dir != NULL && saved_cwd[0] != '\0') {
        (void)novai_chdir(saved_cwd);
    }

    /* Restore env vars */
    novai_task_restore_env(env_backup, env_backup_count);

    if (exit_code != 0) {
        fprintf(stderr, "%s\xe2\x9c\x97 task '%s' failed%s "
                "(exit code %d)\n", TC_RED, name, TC_RESET, exit_code);
        return exit_code;
    }

    if (!silent && !dry_run) {
        printf("%s\xe2\x9c\x93 task '%s' complete%s\n\n",
               TC_GREEN, name, TC_RESET);
    } else if (dry_run && !silent) {
        printf("%s\xe2\x9c\x93 task '%s' (dry run)%s\n\n",
               TC_CYAN, name, TC_RESET);
    }

    /* Run 'post' hook task */
    const char *post_task = novai_task_get_str(task_tbl, "post");
    if (post_task != NULL && exit_code == 0 && !dry_run) {
        rc = novai_task_run_one(vm, root, tasks, post_task, depth + 1,
                                visited, visited_count,
                                dry_run, default_shell);
        if (rc != 0) { return rc; }
    }

    return 0;
}

/**
 * @brief List all available tasks from a taskfile.
 */
static void novai_task_list(NovaTable *tasks) {
    printf("%sAvailable tasks:%s\n\n", TC_HUNTER, TC_RESET);

    uint32_t iter = 0;
    NovaValue key, val;
    while (nova_table_next(tasks, &iter, &key, &val)) {
        if (!nova_is_string(key) || !nova_is_table(val)) { continue; }

        const char *name = nova_str_data(nova_as_string(key));
        NovaTable *task_tbl = nova_as_table(val);
        const char *desc = novai_task_get_str(task_tbl, "description");
        const char *cmd  = novai_task_get_str(task_tbl, "command");

        /* Show platform badge if filtered */
        NovaValue pv = nova_table_get_cstr(task_tbl, "platforms", 9);
        int platform_filtered = !nova_is_nil(pv);

        printf("  %s%-16s%s", TC_GREEN, name, TC_RESET);
        if (desc != NULL) {
            printf("%s", desc);
        } else if (cmd != NULL) {
            printf("%s$ %s%s", TC_DIM, cmd, TC_RESET);
        }
        if (platform_filtered) {
            printf("  %s[", TC_DIM);
            if (nova_is_table(pv)) {
                NovaTable *parr = nova_as_table(pv);
                for (nova_int_t pi = 0; ; pi++) {
                    NovaValue pe = nova_table_get_int(parr, pi);
                    if (nova_is_nil(pe)) break;
                    if (pi > 0) printf(",");
                    if (nova_is_string(pe)) {
                        printf("%s", nova_str_data(nova_as_string(pe)));
                    }
                }
            }
            printf("]%s", TC_RESET);
        }
        printf("\n");
    }
    printf("\n%sUsage:%s nova task [--dry] <name> [name...]\n", TC_DIM, TC_RESET);
    printf("%sPlatform:%s %s\n", TC_DIM, TC_RESET, novai_task_platform());
}

/**
 * @brief Search for taskfile.nini in current and parent directories.
 *
 * Starts from CWD and walks up to NOVAI_TASK_MAX_SEARCH levels.
 * Stores the found path in out_path. Returns 1 if found, 0 if not.
 */
static int novai_task_find_taskfile(char *out_path, size_t out_size) {
    char cwd[NOVAI_PATH_MAX];
    if (novai_getcwd(cwd, sizeof(cwd)) == NULL) { return 0; }

    for (int i = 0; i < NOVAI_TASK_MAX_SEARCH; i++) {
        snprintf(out_path, out_size, "%s/%s", cwd, NOVAI_TASKFILE_NAME);

        FILE *test = fopen(out_path, "r");
        if (test != NULL) {
            fclose(test);
            return 1;
        }

        /* Go up one directory */
        char *last_slash = strrchr(cwd, '/');
        if (last_slash == NULL || last_slash == cwd) {
            break; /* Reached root */
        }
        *last_slash = '\0';
    }
    return 0;
}

/* ============================================================
 * PUBLIC API
 * ============================================================ */

/**
 * @brief Task runner entry point.
 *
 * Creates a temporary VM for NINI decoding, reads the taskfile,
 * applies global config, and dispatches to the named task(s).
 */
int nova_tool_task(const NovaToolFlags *flags) {
    if (flags == NULL) { return 1; }

    /* Search for taskfile in current + parent directories */
    char taskfile_path[NOVAI_PATH_MAX];
    if (!novai_task_find_taskfile(taskfile_path, sizeof(taskfile_path))) {
        fprintf(stderr, "%serror:%s cannot find '%s' in current "
                "or parent directories\n",
                TC_RED, TC_RESET, NOVAI_TASKFILE_NAME);
        return 1;
    }

    /* chdir to taskfile's directory for relative paths */
    char taskfile_dir[NOVAI_PATH_MAX];
    snprintf(taskfile_dir, sizeof(taskfile_dir), "%s", taskfile_path);
    char *dir_sep = strrchr(taskfile_dir, '/');
    char original_cwd[NOVAI_PATH_MAX];
    original_cwd[0] = '\0';
    if (dir_sep != NULL) {
        *dir_sep = '\0';
        if (novai_getcwd(original_cwd, sizeof(original_cwd)) != NULL) {
            (void)novai_chdir(taskfile_dir);
        }
    }

    /* Read taskfile from disk */
    size_t flen = 0;
    char *content = novai_task_read_file(NOVAI_TASKFILE_NAME, &flen);
    if (content == NULL) {
        fprintf(stderr, "%serror:%s cannot open '%s': %s\n",
                TC_RED, TC_RESET, taskfile_path, strerror(errno));
        if (original_cwd[0] != '\0') { (void)novai_chdir(original_cwd); }
        return 1;
    }

    /* Create a temporary VM for NINI parsing */
    NovaVM *vm = nova_vm_create();
    if (vm == NULL) {
        free(content);
        fprintf(stderr, "%serror:%s cannot create VM\n", TC_RED, TC_RESET);
        if (original_cwd[0] != '\0') { (void)novai_chdir(original_cwd); }
        return 1;
    }

    /* Decode NINI */
    NiniOptions nini_opts;
    nova_nini_options_init(&nini_opts);
    char errbuf[256];
    int rc = nova_nini_decode(vm, content, flen, &nini_opts,
                              errbuf, sizeof(errbuf));
    free(content);

    if (rc != 0) {
        fprintf(stderr, "%serror:%s %s\n", TC_RED, TC_RESET, errbuf);
        nova_vm_destroy(vm);
        if (original_cwd[0] != '\0') { (void)novai_chdir(original_cwd); }
        return 1;
    }

    /* Get decoded table from VM stack top */
    if (vm->stack_top <= vm->stack) {
        fprintf(stderr, "%serror:%s NINI decode produced "
                "no result\n", TC_RED, TC_RESET);
        nova_vm_destroy(vm);
        if (original_cwd[0] != '\0') { (void)novai_chdir(original_cwd); }
        return 1;
    }
    NovaValue root_val = vm->stack_top[-1];
    if (!nova_is_table(root_val)) {
        fprintf(stderr, "%serror:%s taskfile did not "
                "produce a table\n", TC_RED, TC_RESET);
        nova_vm_destroy(vm);
        if (original_cwd[0] != '\0') { (void)novai_chdir(original_cwd); }
        return 1;
    }
    NovaTable *root = nova_as_table(root_val);

    /* Look up __tasks subtable */
    NovaValue tasks_val = nova_table_get_cstr(root, "__tasks", 7);
    if (nova_is_nil(tasks_val) || !nova_is_table(tasks_val)) {
        fprintf(stderr, "%serror:%s no [task:*] sections "
                "in %s\n", TC_RED, TC_RESET, taskfile_path);
        nova_vm_destroy(vm);
        if (original_cwd[0] != '\0') { (void)novai_chdir(original_cwd); }
        return 1;
    }
    NovaTable *tasks = nova_as_table(tasks_val);

    /* Read [options] section for runner configuration */
    NovaTable *opts_tbl = NULL;
    NovaValue opts_val = nova_table_get_cstr(root, "options", 7);
    if (nova_is_table(opts_val)) {
        opts_tbl = nova_as_table(opts_val);
    }

    const char *default_task = NULL;
    const char *default_shell = NULL;
    const char *global_dotenv = NULL;
    if (opts_tbl != NULL) {
        default_task  = novai_task_get_str(opts_tbl, "default");
        default_shell = novai_task_get_str(opts_tbl, "shell");
        global_dotenv = novai_task_get_str(opts_tbl, "dotenv");
    }

    /* Load global .env file */
    if (global_dotenv != NULL) {
        novai_task_load_dotenv(global_dotenv);
    }

    /* Set global [env] section vars */
    novai_task_set_global_env(root);

    int dry = flags->dry_run;

    /* No task name → run default or list */
    if (flags->subject_count == 0) {
        if (default_task != NULL) {
            /* Run default task */
            const char *visited[NOVAI_TASK_MAX_VISITED];
            int visited_count = 0;
            int result = novai_task_run_one(vm, root, tasks, default_task, 0,
                                            visited, &visited_count,
                                            dry, default_shell);
            nova_vm_destroy(vm);
            if (original_cwd[0] != '\0') { (void)novai_chdir(original_cwd); }
            return result;
        }
        novai_task_list(tasks);
        nova_vm_destroy(vm);
        if (original_cwd[0] != '\0') { (void)novai_chdir(original_cwd); }
        return 0;
    }

    /* Run requested tasks in order */
    const char *visited[NOVAI_TASK_MAX_VISITED];
    int visited_count = 0;
    int result = 0;

    for (int i = 0; i < flags->subject_count; i++) {
        result = novai_task_run_one(vm, root, tasks, flags->subjects[i], 0,
                                    visited, &visited_count,
                                    dry, default_shell);
        if (result != 0) { break; }
    }

    nova_vm_destroy(vm);
    if (original_cwd[0] != '\0') { (void)novai_chdir(original_cwd); }
    return result;
}
