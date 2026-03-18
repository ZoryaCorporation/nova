/**
 * @file    nova_shell.c
 * @brief   Nova Interactive Tool Shell — Discovery-Based Dispatch
 *
 * Extracted from the monolithic nova_tools.c. Implements:
 *   - Tool discovery: scans directories for n-prefixed binaries
 *   - Shell loop: interactive prompt with built-in commands
 *   - Pipeline engine: pipe chaining via fork+exec+pipe()
 *   - Tool dispatch: exec discovered binaries or fall back to
 *     in-process functions for the task runner
 *
 * @author  Anthony Taliento
 * @date    2026-03-16
 * @version 0.2.0
 *
 * @copyright Copyright (c) 2026 Zorya Corporation
 * @license MIT
 *
 * ZORYA-C COMPLIANCE: v2.0.0
 */

#include "nova/nova_shell.h"
#include "nova/nova_conf.h"
#include "nova/nova_error.h"
#include "nova/nova_tools.h"
#include "tools/shared/ntool_common.h"

#include <zorya/pal.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>

/* POSIX process control for pipeline execution */
#ifdef _WIN32
    #include <process.h>
    #include <io.h>
    #define novasi_dup(fd)        _dup(fd)
    #define novasi_dup2(fd1,fd2)  _dup2((fd1),(fd2))
    #define novasi_fdclose(fd)    _close(fd)
    #define novasi_fileno(fp)     _fileno(fp)
    #define novasi_isatty(fd)     _isatty(fd)
    #define novasi_getcwd(b,sz)   (_getcwd((b),(int)(sz)) != NULL ? 0 : -1)
    #define novasi_chdir(d)       _chdir(d)
#else
    #include <unistd.h>
    #include <sys/types.h>
    #include <sys/wait.h>
    #define novasi_dup(fd)        dup(fd)
    #define novasi_dup2(fd1,fd2)  dup2((fd1),(fd2))
    #define novasi_fdclose(fd)    close(fd)
    #define novasi_fileno(fp)     fileno(fp)
    #define novasi_isatty(fd)     isatty(fd)
    #define novasi_getcwd(b,sz)   (getcwd((b),(sz)) != NULL ? 0 : -1)
    #define novasi_chdir(d)       chdir(d)
#endif

#define NOVASI_STDIN_FD   0
#define NOVASI_STDOUT_FD  1

/* ============================================================
 * TOOL DISCOVERY
 * ============================================================ */

/**
 * @brief Scan a single directory for tool binaries.
 *
 * Finds executables starting with 'n', strips the prefix to get
 * the unprefixed name. Skips duplicates already in the registry.
 */
static void novasi_scan_dir(const char *dir, NovaShellToolRegistry *reg) {
    if (dir == NULL || reg == NULL) return;
    if (reg->count >= NOVA_SHELL_MAX_TOOLS) return;

    ZoryaDirEntry entries[NTOOL_DIR_MAX];
    int n = ntool_read_dir(dir, entries, NTOOL_DIR_MAX, 0);

    for (int i = 0; i < n && reg->count < NOVA_SHELL_MAX_TOOLS; i++) {
        /* Skip directories and dotfiles */
        if (entries[i].is_dir) continue;
        if (entries[i].name[0] == '.') continue;

        const char *name = entries[i].name;
        size_t nlen = strlen(name);

        /* Must start with 'n' and have at least 2 chars */
        if (nlen < 2 || name[0] != 'n') continue;

        /* The unprefixed name is everything after 'n' */
        const char *unprefixed = name + 1;

        /* Skip if already registered (earlier directories win) */
        int dup = 0;
        for (int j = 0; j < reg->count; j++) {
            if (strcmp(reg->tools[j].name, unprefixed) == 0) {
                dup = 1;
                break;
            }
        }
        if (dup) continue;

        /* Build full path and verify it exists */
        char full_path[NTOOL_PATH_MAX];
        ntool_join_path(full_path, sizeof(full_path), dir, name);

        /* Register the tool */
        NovaShellTool *t = &reg->tools[reg->count];
        snprintf(t->name, sizeof(t->name), "%s", unprefixed);
        snprintf(t->path, sizeof(t->path), "%s", full_path);
        reg->count++;
    }
}

int nova_shell_discover_tools(NovaShellToolRegistry *reg) {
    if (reg == NULL) return 0;
    memset(reg, 0, sizeof(*reg));

    /* 1. Environment variable override */
    const char *env_dir = getenv(NOVA_TOOLS_DIR_ENV);
    if (env_dir != NULL && env_dir[0] != '\0') {
        novasi_scan_dir(env_dir, reg);
    }

    /* 2. Local project tools */
    novasi_scan_dir(NOVA_TOOLS_DIR_LOCAL, reg);

    /* 3. User tools (~/.nova/tools/) */
    const char *home = getenv("HOME");
    if (home == NULL) home = getenv("USERPROFILE");
    if (home != NULL) {
        char user_dir[NTOOL_PATH_MAX];
        ntool_join_path(user_dir, sizeof(user_dir), home,
                        NOVA_TOOLS_DIR_USER);
        novasi_scan_dir(user_dir, reg);
    }

    /* 4. System-installed tools */
    novasi_scan_dir(NOVA_TOOLS_DIR_SYSTEM, reg);

    return reg->count;
}

const NovaShellTool *nova_shell_find_tool(
    const NovaShellToolRegistry *reg, const char *name
) {
    if (reg == NULL || name == NULL) return NULL;
    for (int i = 0; i < reg->count; i++) {
        if (strcmp(reg->tools[i].name, name) == 0) {
            return &reg->tools[i];
        }
    }
    return NULL;
}

int nova_shell_is_tool(const NovaShellToolRegistry *reg, const char *name) {
    if (name == NULL) return 0;

    /* Check discovered tools */
    if (reg != NULL && nova_shell_find_tool(reg, name) != NULL) {
        return 1;
    }

    /* "task" is always a built-in tool */
    if (strcmp(name, "task") == 0) return 1;

    return 0;
}

/* ============================================================
 * TOOL DISPATCH
 * ============================================================ */

/**
 * @brief Execute a discovered tool binary as a child process.
 *
 * Uses fork+exec on POSIX. The child inherits the current
 * stdin/stdout (which may be redirected by the pipeline engine).
 *
 * @param tool_path  Full path to the tool binary
 * @param argc       Argument count
 * @param argv       Arguments (tool-specific, NOT including tool name)
 * @return Exit status of the tool
 */
static int novasi_exec_tool(const char *tool_path, int argc, char **argv) {
#ifdef _WIN32
    /* On Windows, use _spawnvp (no fork) */
    (void)argc;
    char *spawn_argv[NOVA_SHELL_MAX_ARGS + 2];
    spawn_argv[0] = (char *)tool_path;
    for (int i = 0; i < argc && i < NOVA_SHELL_MAX_ARGS; i++) {
        spawn_argv[i + 1] = argv[i];
    }
    spawn_argv[argc + 1] = NULL;
    return (int)_spawnvp(_P_WAIT, tool_path, (const char *const *)spawn_argv);
#else
    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "nova: fork failed: %s\n", strerror(errno));
        return -1;
    }

    if (pid == 0) {
        /* Child: build argv and exec */
        char *exec_argv[NOVA_SHELL_MAX_ARGS + 2];
        exec_argv[0] = (char *)tool_path;
        for (int i = 0; i < argc && i < NOVA_SHELL_MAX_ARGS; i++) {
            exec_argv[i + 1] = argv[i];
        }
        exec_argv[argc + 1] = NULL;

        execv(tool_path, exec_argv);
        /* If we get here, exec failed */
        fprintf(stderr, "nova: exec %s: %s\n", tool_path, strerror(errno));
        _exit(127);
    }

    /* Parent: wait for child */
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        fprintf(stderr, "nova: waitpid: %s\n", strerror(errno));
        return -1;
    }

    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    return -1;
#endif
}

int nova_shell_dispatch(const NovaShellToolRegistry *reg,
                        const char *name, int argc, char **argv) {
    if (name == NULL) return -1;

    /* 1. Check discovered tools → exec binary */
    const NovaShellTool *tool = nova_shell_find_tool(reg, name);
    if (tool != NULL) {
        return novasi_exec_tool(tool->path, argc, argv);
    }

    /* 2. "task" is always handled in-process (core VM feature) */
    if (strcmp(name, "task") == 0) {
        return nova_tool_dispatch("task", argc, argv);
    }

    /* 3. Unknown tool */
    fprintf(stderr, "nova: unknown command: %s\n", name);
    return 1;
}

/* ============================================================
 * TOKENIZER
 * ============================================================ */

/**
 * @brief Tokenize a command line into argv-style tokens.
 *
 * Handles double and single quotes, including mid-token quotes
 * like -m="*.c". Modifies the input string in-place.
 */
static int novasi_tokenize(char *line, char **av, int max_args) {
    int ac = 0;
    char *p = line;

    while (*p && ac < max_args) {
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0') break;

        char *start = p;
        char *out = p;

        while (*p && *p != ' ' && *p != '\t') {
            if (*p == '"') {
                p++;
                while (*p && *p != '"') *out++ = *p++;
                if (*p == '"') p++;
            } else if (*p == '\'') {
                p++;
                while (*p && *p != '\'') *out++ = *p++;
                if (*p == '\'') p++;
            } else {
                *out++ = *p++;
            }
        }

        if (*p) *p++ = '\0';
        *out = '\0';
        av[ac++] = start;
    }

    return ac;
}

/* ============================================================
 * PIPELINE ENGINE
 * ============================================================ */

/**
 * @brief Split a command line by unquoted '|' into pipeline stages.
 *
 * Respects double and single quotes. Modifies the input string.
 */
static int novasi_split_pipeline(char *line, char **stages,
                                 int max_stages) {
    int count = 0;
    char *p = line;

    stages[count++] = p;

    while (*p && count < max_stages) {
        if (*p == '"') {
            p++;
            while (*p && *p != '"') p++;
            if (*p) p++;
        } else if (*p == '\'') {
            p++;
            while (*p && *p != '\'') p++;
            if (*p) p++;
        } else if (*p == '|') {
            *p++ = '\0';
            while (*p == ' ' || *p == '\t') p++;
            stages[count++] = p;
        } else {
            p++;
        }
    }

    return count;
}

/**
 * @brief Execute a pipeline of tool commands.
 *
 * For a single command (no pipe), dispatches directly.
 * For multi-stage pipelines, uses tmpfile + dup2 redirection
 * to pipe stdout of one stage to stdin of the next.
 */
static int novasi_run_pipeline(const NovaShellToolRegistry *reg,
                               char *line) {
    char *stages[NOVA_SHELL_PIPE_MAX];
    int stage_count = novasi_split_pipeline(line, stages,
                                            NOVA_SHELL_PIPE_MAX);
    if (stage_count == 0) return 0;

    FILE *prev_tmp = NULL;
    int result = 0;

    for (int si = 0; si < stage_count; si++) {
        int is_first = (si == 0);
        int is_last  = (si == stage_count - 1);

        int saved_stdin  = -1;
        int saved_stdout = -1;
        FILE *cur_tmp = NULL;

        /* Redirect stdin from previous stage output */
        if (!is_first && prev_tmp != NULL) {
            fflush(prev_tmp);
            rewind(prev_tmp);
            saved_stdin = novasi_dup(NOVASI_STDIN_FD);
            novasi_dup2(novasi_fileno(prev_tmp), NOVASI_STDIN_FD);
        }

        /* Redirect stdout to temp file (unless last stage) */
        if (!is_last) {
            cur_tmp = tmpfile();
            if (cur_tmp != NULL) {
                fflush(stdout);
                saved_stdout = novasi_dup(NOVASI_STDOUT_FD);
                novasi_dup2(novasi_fileno(cur_tmp), NOVASI_STDOUT_FD);
            }
        }

        /* Tokenize and dispatch this stage */
        char *av[NOVA_SHELL_MAX_ARGS];
        char *cmd = stages[si];
        while (*cmd == ' ' || *cmd == '\t') cmd++;

        int ac = novasi_tokenize(cmd, av, NOVA_SHELL_MAX_ARGS);

        if (ac > 0) {
            if (nova_shell_is_tool(reg, av[0])) {
                result = nova_shell_dispatch(reg, av[0], ac - 1, &av[1]);
            } else {
                fprintf(stderr, "nova: unknown command: %s\n", av[0]);
                result = -1;
            }
        }

        /* Restore stdout */
        if (saved_stdout >= 0) {
            fflush(stdout);
            novasi_dup2(saved_stdout, NOVASI_STDOUT_FD);
            novasi_fdclose(saved_stdout);
        }

        /* Drain stdin buffer and restore */
        if (saved_stdin >= 0) {
            {
                char drain[1024];
                while (fread(drain, 1, sizeof(drain), stdin) > 0) {
                    /* discard */
                }
            }
            novasi_dup2(saved_stdin, NOVASI_STDIN_FD);
            novasi_fdclose(saved_stdin);
            clearerr(stdin);
        }

        /* Close previous temp file */
        if (prev_tmp != NULL) {
            fclose(prev_tmp);
        }

        prev_tmp = cur_tmp;
    }

    /* Close last temp file */
    if (prev_tmp != NULL) {
        fclose(prev_tmp);
    }

    return result;
}

/* ============================================================
 * HELP DISPLAY
 * ============================================================ */

/**
 * @brief Print available tools based on discovery results.
 */
static void novasi_print_help(const NovaShellToolRegistry *reg) {
    int c = nova_diag_color_enabled();
    const char *h = c ? "\033[1;38;5;22m\033[4m" : ""; /* hunter+uline */
    const char *e = c ? "\033[38;5;35m" : "";           /* emerald      */
    const char *m = c ? "\033[38;5;245m" : "";          /* muted        */
    const char *r = c ? "\033[0m" : "";                 /* reset        */

    /* Known tool descriptions */
    static const struct {
        const char *name;
        const char *desc;
        const char *usage;
    } descs[] = {
        {"cat",   "Concatenate and print files",       "[file...]"         },
        {"ls",    "List directory contents",            "[dir]"             },
        {"tree",  "Directory tree visualization",       "[dir]"             },
        {"find",  "Find files by name pattern",         "[dir] -m=PATTERN"  },
        {"grep",  "Search text in files",               "[file...] -m=PAT"  },
        {"head",  "First N lines of a file",            "[file...]"         },
        {"tail",  "Last N lines of a file",             "[file...]"         },
        {"wc",    "Word, line, and character count",    "[file...]"         },
        {"write", "Write stdin to file",                "<file>"            },
        {"echo",  "Print text to stdout",               "[text...]"         },
        {"pwd",   "Print working directory",            ""                  },
        {NULL, NULL, NULL}
    };

    printf("%sTOOLS%s\n", h, r);

    /* Print discovered tools with descriptions */
    for (int i = 0; i < reg->count; i++) {
        const char *name = reg->tools[i].name;
        const char *desc = "User tool";
        const char *usage = "";

        /* Look up description */
        for (int d = 0; descs[d].name != NULL; d++) {
            if (strcmp(name, descs[d].name) == 0) {
                desc = descs[d].desc;
                usage = descs[d].usage;
                break;
            }
        }

        printf("  %s%-5s%s %-20s %s%s%s\n", e, name, r, usage, m, desc, r);
    }

    /* Task is always available (in-process) */
    printf("  %s%-5s%s %-20s %s%s%s\n",
           e, "task", r, "[name...]", m, "Run NINI taskfile tasks", r);

    printf("\n%sFLAGS%s\n", h, r);
    printf("  %s-m%s, %s--match%s=PATTERN      Filter/search pattern\n",
           e, r, e, r);
    printf("  %s-i%s, %s--ignore-case%s        Case-insensitive matching\n",
           e, r, e, r);
    printf("  %s-v%s, %s--invert%s             Invert match (exclude)\n",
           e, r, e, r);
    printf("  %s-r%s, %s--recursive%s          Recurse into directories\n",
           e, r, e, r);
    printf("  %s-o%s, %s--output%s=FILE        Redirect output to file\n",
           e, r, e, r);
    printf("  %s-l%s, %s--limit%s=N            Limit results\n",
           e, r, e, r);
    printf("  %s-d%s, %s--depth%s=N            Maximum directory depth\n",
           e, r, e, r);
    printf("  %s-V%s, %s--verbose%s            Verbose output\n",
           e, r, e, r);
    printf("  %s-a%s, %s--all%s                Show hidden files\n",
           e, r, e, r);
    printf("  %s-n%s, %s--number%s             Show line numbers\n",
           e, r, e, r);
    printf("  %s-L%s, %s--long%s               Detailed listing\n",
           e, r, e, r);
    printf("  %s-A%s, %s--append%s             Append to output file\n",
           e, r, e, r);
    printf("      %s--dry%s                Dry run (print commands only)\n\n",
           e, r);
}

/* ============================================================
 * SHELL LOOP
 * ============================================================ */

int nova_shell_run(void) {
    /* Discover available tools */
    NovaShellToolRegistry reg;
    nova_shell_discover_tools(&reg);

    int c = nova_diag_color_enabled();
    const char *hg = c ? "\033[1;38;5;22m" : "";  /* hunter green  */
    const char *ng = c ? "\033[1;38;5;40m" : "";   /* nova green    */
    const char *em = c ? "\033[38;5;35m" : "";      /* emerald       */
    const char *mu = c ? "\033[38;5;245m" : "";     /* muted         */
    const char *rs = c ? "\033[0m" : "";             /* reset         */

    /* ASCII art banner */
    fprintf(stderr,
        "%s"
        "    _   __                 \n"
        "   / | / /___ _   ______ _\n"
        "  /  |/ / __ \\ | / / __ `/\n"
        " / /|  / /_/ / |/ / /_/ / \n"
        "/_/ |_/\\____/|___/\\__,_/  \n"
        "%s", hg, rs);
    fprintf(stderr, " %sv%s%s  %sTool Shell%s\n",
            ng, NOVA_VERSION_STRING, rs, mu, rs);
    fprintf(stderr, " %sZorya Corporation  //  Engineering Excellence, Democratized%s\n",
            mu, rs);
    fprintf(stderr, " %s\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80%s\n",
            em, rs);
    fprintf(stderr, " %sType '%shelp%s%s' for commands, '%sexit%s%s' to quit.%s\n",
            mu, em, rs, mu, em, rs, mu, rs);
    fprintf(stderr, " %s%d tools discovered.%s\n\n",
            mu, reg.count, rs);

    /* Disable stdin buffering for pipeline dup2 redirects */
    setvbuf(stdin, NULL, _IONBF, 0);

    char line[NOVA_SHELL_LINE_MAX];

    for (;;) {
        /* Colored prompt: nova$ */
        fprintf(stdout, "%snova%s%s$%s ",
                ng, rs, em, rs);
        fflush(stdout);

        if (fgets(line, (int)sizeof(line), stdin) == NULL) {
            printf("\n");
            break;
        }

        /* Strip trailing newline/CR */
        size_t len = strlen(line);
        while (len > 0 &&
               (line[len - 1] == '\n' || line[len - 1] == '\r')) {
            line[--len] = '\0';
        }

        /* Skip empty lines and comments */
        if (len == 0) continue;
        if (line[0] == '#') continue;

        /* Trim leading whitespace */
        char *cmd = line;
        while (*cmd == ' ' || *cmd == '\t') cmd++;
        if (*cmd == '\0') continue;

        /* ---- Shell built-in commands ---- */

        if (strcmp(cmd, "exit") == 0 || strcmp(cmd, "quit") == 0) {
            break;
        }

        if (strcmp(cmd, "help") == 0) {
            novasi_print_help(&reg);
            printf("Shell commands:\n");
            printf("  cd [dir]     Change working directory\n");
            printf("  exit, quit   Leave the shell\n");
            printf("  help         Show this help\n");
            printf("  clear        Clear the screen\n");
            printf("  version      Show version\n");
            printf("\nUse | to pipe output between tools:\n");
            printf("  find src/ -m=\"*.c\" | grep lib | wc\n");
            continue;
        }

        if (strcmp(cmd, "clear") == 0) {
            printf("\033[2J\033[H");
            fflush(stdout);
            continue;
        }

        if (strcmp(cmd, "version") == 0) {
            printf("Nova %s -- %s\n",
                   NOVA_VERSION_STRING, NOVA_COPYRIGHT);
            continue;
        }

        /* cd: change directory */
        if (strncmp(cmd, "cd", 2) == 0 &&
            (cmd[2] == ' ' || cmd[2] == '\t' || cmd[2] == '\0')) {
            const char *dir = cmd + 2;
            while (*dir == ' ' || *dir == '\t') dir++;

            if (*dir == '\0') {
                dir = getenv("HOME");
                if (dir == NULL) dir = getenv("USERPROFILE");
                if (dir == NULL) {
                    fprintf(stderr, "nova: cd: HOME not set\n");
                    continue;
                }
            } else if (dir[0] == '~' &&
                       (dir[1] == '/' || dir[1] == '\0')) {
                static char expanded[NTOOL_PATH_MAX];
                const char *home = getenv("HOME");
                if (home == NULL) home = getenv("USERPROFILE");
                if (home == NULL) {
                    fprintf(stderr, "nova: cd: HOME not set\n");
                    continue;
                }
                snprintf(expanded, sizeof(expanded), "%s%s",
                         home, dir + 1);
                dir = expanded;
            }

            if (novasi_chdir(dir) != 0) {
                fprintf(stderr, "nova: cd: %s: %s\n",
                        dir, strerror(errno));
            }
            continue;
        }

        /* ---- Dispatch as tool pipeline ---- */
        novasi_run_pipeline(&reg, cmd);
    }

    return 0;
}
