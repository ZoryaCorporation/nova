/**
 * @file nova.c
 * @brief Nova Language CLI - Main Entry Point
 *
 * Nova is a register-based bytecode VM for a dynamically-typed scripting
 * language with Lua-like syntax, a powerful preprocessor, string methods,
 * NINI config format, NDP data processing, and built-in CLI tools.
 *
 * ── QUICK START ─────────────────────────────────────────────
 *
 *   nova                          Launch interactive tool shell
 *   nova script.n                 Execute a Nova source file
 *   nova script.no                Execute compiled bytecode
 *   nova -e 'echo("hello")'      Evaluate an expression string
 *
 * ── COMPILATION ─────────────────────────────────────────────
 *
 *   nova -c script.n              Compile to script.no bytecode
 *   nova -c script.n -o out.no   Compile with custom output path
 *   nova -c script.n -s          Compile and strip debug info
 *   nova -O0 -c script.n         Compile without optimizations
 *
 * ── TOOLS (built-in, zero subprocess overhead) ──────────────
 *
 *   nova cat file.txt             Print file contents
 *   nova ls [dir]                 List directory entries
 *   nova tree [dir] -d=3         Directory tree (depth-limited)
 *   nova find . -m="*.c"          Find files by glob pattern
 *   nova grep src/ -m=TODO -r    Search text recursively
 *   nova head file.txt -l=20     First 20 lines
 *   nova tail file.txt -l=10     Last 10 lines
 *   nova wc file.txt              Line / word / char counts
 *   nova task                     List NINI taskfile tasks
 *   nova task build test          Run tasks (deps resolved)
 *
 * ── TOOL SHELL (interactive, pipe-capable) ──────────────────
 *
 *   $ nova                        Enter the shell
 *   nova> find src/ -m="*.c" | grep lib | wc
 *   nova> cat README.md | head -l=5
 *   nova> cd src && ls -L
 *   nova> help                    Show all commands
 *   nova> exit                    Leave the shell
 *
 * ── SCRIPTING LANGUAGE ──────────────────────────────────────
 *
 *   Output:       echo("hello", 42)       -- tab-separated + newline
 *                 printf("n=%d\n", 42)     -- C-style formatted
 *   Strings:      s:upper()  s:find("x")  s:sub(0, 4)  s:gsub("a","b")
 *   Tables:       t = {10, 20, 30}        -- 0-based indexing
 *   Closures:     local f = function(x) return x*2 end
 *   Interpolation: `hello ${name}, you are ${age} years old`
 *   Varargs:      function log(...) echo(...) end
 *   Modules:      local m = require("mylib")
 *   Preprocessor: #import json   #import nini   #define FOO 42
 *   Error handling: pcall(fn)  xpcall(fn, handler)
 *   Coroutines:   coroutine.create(fn)  coroutine.resume(co)
 *   Async:        async function fetch() ... end   await(task)
 *
 * ── DATA PROCESSING (NDP) ───────────────────────────────────
 *
 *   #import json      json.decode(str)   json.encode(tbl)
 *   #import csv       csv.decode(str)    csv.encode(tbl)
 *   #import nini      nini.decode(str)   nini.encode(tbl)
 *
 * ── DEBUG / DIAGNOSTIC MODES ────────────────────────────────
 *
 *   nova --lex script.n           Tokenize and print tokens
 *   nova --parse script.n         Parse and report AST stats
 *   nova --ast script.n           Dump abstract syntax tree
 *   nova --dis script.n           Compile and disassemble bytecode
 *   nova --trace=vm,call script.n Runtime trace instrumentation
 *   nova --explain E2001          Explain an error code
 *
 * ── STANDARD LIBRARY ────────────────────────────────────────
 *
 *   math     abs ceil floor sqrt sin cos log exp random pi
 *   string   len sub upper lower rep find format gsub match byte char
 *   table    insert remove sort concat move pack unpack
 *   io       open close read write lines
 *   os       execute capture getenv clock time date sleep platform
 *   fs       read write exists isfile isdir list walk glob mkdir stat
 *   tools    cat ls tree find grep head tail wc pwd run
 *   debug    traceback getinfo getlocal sethook
 *   net      get post put delete patch head request (#import net)
 *   sql      open exec query close (#import sql)
 *   nlp      tokenize stem fuzzy freq tfidf ngrams
 *
 * @author Anthony Taliento
 * @date 2026-02-07
 * @version 0.2.0
 *
 * @copyright Copyright (c) 2026 Zorya Corporation
 * @license MIT
 */

#include "nova/nova_lex.h"
#include "nova/nova_pp.h"
#include "nova/nova_parse.h"
#include "nova/nova_ast.h"
#include "nova/nova_compile.h"
#include "nova/nova_opt.h"
#include "nova/nova_codegen.h"
#include "nova/nova_vm.h"
#include "nova/nova_proto.h"
#include "nova/nova_opcode.h"
#include "nova/nova_conf.h"
#include "nova/nova_lib.h"
#include "nova/nova_error.h"
#include "nova/nova_trace.h"
#include "nova/nova_tools.h"
#include "nova/nova_shell.h"
#include "nova/nova_suggest.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <inttypes.h>

/* ============================================================
 * VERSION, BRANDING, AND ASCII ART
 * ============================================================ */

/* CLI version tracks the core version from nova_conf.h */
#define NOVA_CLI_VERSION_STRING NOVA_VERSION_STRING

/* Nova ASCII art banner — retro terminal aesthetic.
 * Displayed in hunter green for that old-school CRT feel.      */
#define NOVA_ASCII_ART \
    "    _   __                 \n" \
    "   / | / /___ _   ______ _\n" \
    "  /  |/ / __ \\ | / / __ `/\n" \
    " / /|  / /_/ / |/ / /_/ / \n" \
    "/_/ |_/\\____/|___/\\__,_/  \n"

#define NOVA_TAGLINE  "Zorya Corporation  //  Engineering Excellence, Democratized"
#define NOVA_SEPARATOR "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

/* ============================================================
 * CLI OPTIONS
 * ============================================================ */

typedef enum {
    NOVA_MODE_EXECUTE,      /* Execute script file */
    NOVA_MODE_COMPILE,      /* Compile to .no only */
    NOVA_MODE_EVAL,         /* Evaluate string (-e) */
    NOVA_MODE_LEX,          /* Lex only (debug) */
    NOVA_MODE_PARSE,        /* Parse only (debug) */
    NOVA_MODE_AST,          /* Dump AST (debug) */
    NOVA_MODE_DIS,          /* Disassemble (debug) */
    NOVA_MODE_HELP,         /* Show help */
    NOVA_MODE_VERSION       /* Show version */
} NovaMode;

typedef struct {
    NovaMode mode;
    const char *input_file;
    const char *output_file;
    const char *eval_string;
    int verbose;
    int optimize;
    int strip_debug;
    int no_color;
    const char *explain_code;
    const char *trace_channels;
    int argc;
    char **argv;
    int script_argc;
    char **script_argv;
} NovaOptions;

/* ============================================================
 * FORWARD DECLARATIONS
 * ============================================================ */

static void nova_print_usage(const char *prog);
static void nova_print_version(void);
static int nova_parse_args(int argc, char **argv, NovaOptions *opts);
static int nova_run_file(NovaVM *vm, const char *path, const NovaOptions *opts);
static int nova_run_string(NovaVM *vm, const char *source, const char *name);
static int nova_compile_file(const char *input, const char *output, const NovaOptions *opts);
static char *nova_read_file(const char *path, size_t *out_size);
static int nova_is_bytecode_file(const char *path);
static void nova_register_args(NovaVM *vm, int argc, char **argv);

/* Debug modes */
static int nova_debug_lex(const char *path);
static int nova_debug_parse(const char *path);
static int nova_debug_ast(const char *path);
static int nova_debug_dis(const char *path);

/* ============================================================
 * MAIN ENTRY POINT
 * ============================================================ */

int main(int argc, char **argv) {
    NovaOptions opts = {0};
    opts.mode = NOVA_MODE_HELP;  /* Default: show help */
    opts.optimize = 1;  /* Optimization on by default */
    opts.argc = argc;
    opts.argv = argv;

    /* Initialize diagnostic system (auto-detects TTY) */
    nova_diag_init();

    /* Enter tool shell if no arguments */
    if (argc == 1) {
        return nova_shell_run();
    }

    /* Check for tool subcommand FIRST (before regular arg parsing).
     * Use discovery-based dispatch: scans tool directories for
     * n-prefixed binaries, falls back to in-process for "task".   */
    if (argc >= 2) {
        NovaShellToolRegistry reg;
        nova_shell_discover_tools(&reg);
        if (nova_shell_is_tool(&reg, argv[1])) {
            return nova_shell_dispatch(&reg, argv[1],
                                       argc - 2, &argv[2]);
        }
    }

    /* Parse command-line arguments */
    int parse_result = nova_parse_args(argc, argv, &opts);
    if (parse_result != 0) {
        return (parse_result < 0) ? 1 : 0;
    }

    /* Apply --no-color flag */
    if (opts.no_color) {
        nova_diag_set_color(0);
    }

    /* Initialize trace system (reads --trace or NOVA_TRACE env) */
#ifdef NOVA_TRACE
    nova_trace_init(opts.trace_channels);
#endif

    /* Handle --explain */
    if (opts.explain_code != NULL) {
        NovaErrorCode ecode = nova_error_parse_code(opts.explain_code);
        if (ecode == NOVA_E0000) {
            fprintf(stderr, "nova: unknown error code: %s\n", opts.explain_code);
            return 1;
        }
        const char *explanation = nova_error_explain(ecode);
        if (explanation != NULL) {
            printf("%s\n", explanation);
        } else {
            printf("No detailed explanation for %s.\n", opts.explain_code);
        }
        return 0;
    }

    /* Handle special modes that don't need VM */
    switch (opts.mode) {
        case NOVA_MODE_HELP:
            nova_print_usage(argv[0]);
            return 0;

        case NOVA_MODE_VERSION:
            nova_print_version();
            return 0;

        case NOVA_MODE_LEX:
            return nova_debug_lex(opts.input_file);

        case NOVA_MODE_PARSE:
            return nova_debug_parse(opts.input_file);

        case NOVA_MODE_AST:
            return nova_debug_ast(opts.input_file);

        case NOVA_MODE_DIS:
            return nova_debug_dis(opts.input_file);

        case NOVA_MODE_COMPILE:
            return nova_compile_file(opts.input_file, opts.output_file, &opts);

        default:
            break;
    }

    /* Create VM for execution modes */
    NovaVM *vm = nova_vm_create();
    if (vm == NULL) {
        nova_diag_report(NOVA_DIAG_ERROR, NOVA_E2003,
                         NULL, 0, 0,
                         "failed to create VM (out of memory)");
        return 1;
    }

    /* Register command-line args as 'arg' table */
    nova_register_args(vm, opts.script_argc, opts.script_argv);

    /* Register standard libraries (base, math, string, table, io, os) */
    nova_open_libs(vm);

    int result = 0;

    switch (opts.mode) {
        case NOVA_MODE_EXECUTE:
            nova_package_set_script_dir(vm, opts.input_file);
            result = nova_run_file(vm, opts.input_file, &opts);
            break;

        case NOVA_MODE_EVAL:
            result = nova_run_string(vm, opts.eval_string, "=(eval)");
            break;

        default:
            fprintf(stderr, "nova: internal error: unknown mode\n");
            result = 1;
    }

    nova_vm_destroy(vm);
    return result;
}

/* ============================================================
 * ARGUMENT PARSING
 * ============================================================ */

static void nova_print_usage(const char *prog) {
    int c = nova_diag_color_enabled();

    /* Color shorthand macros for readability */
    const char *B   = c ? NOVA_COLOR_BANNER  : "";   /* nova green bold    */
    const char *H   = c ? NOVA_COLOR_HUNTERB : "";   /* hunter green bold  */
    const char *U   = c ? NOVA_COLOR_UNDERLINE : ""; /* underline          */
    const char *E   = c ? NOVA_COLOR_EMERALD : "";   /* emerald            */
    const char *M   = c ? NOVA_COLOR_MUTED   : "";   /* muted gray         */
    const char *W   = c ? NOVA_COLOR_PATH    : "";   /* bright white bold  */
    const char *N   = c ? NOVA_COLOR_NOVA    : "";   /* nova green         */
    const char *A   = c ? NOVA_COLOR_ACCENT  : "";   /* emerald accent     */
    const char *L   = c ? NOVA_COLOR_LIME    : "";   /* lime highlight     */
    const char *D   = c ? NOVA_COLOR_DIM     : "";   /* dim                */
    const char *R   = c ? NOVA_COLOR_RESET   : "";   /* reset              */

    /* ── Banner ── */
    printf("%s%s%s",  B, NOVA_ASCII_ART, R);
    printf(" %sv%s%s  %s%s%s\n",
            N, NOVA_CLI_VERSION_STRING, R, M, NOVA_TAGLINE, R);
    printf(" %s%s%s\n\n", A, NOVA_SEPARATOR, R);

    /* ── USAGE ── */
    printf("%s%sUSAGE%s\n", H, U, R);
    printf("  %s%s%s [options] [script [args]]\n", W, prog, R);
    printf("  %s%s%s <tool> [subject...] [flags]\n", W, prog, R);
    printf("  %s%s%s %s(no args)%s                        %sLaunch interactive tool shell%s\n\n",
           W, prog, R, D, R, M, R);

    /* ── QUICK START ── */
    printf("%s%sQUICK START%s\n", H, U, R);
    printf("  %snova%s script.n               %sExecute a Nova source file%s\n", N, R, M, R);
    printf("  %snova%s script.no              %sExecute compiled bytecode%s\n", N, R, M, R);
    printf("  %snova%s %s-e%s %s'echo(\"hello\")'%s      %sEvaluate an inline expression%s\n",
           N, R, E, R, L, R, M, R);
    printf("  %snova%s %s-c%s script.n            %sCompile to bytecode (.no)%s\n",
           N, R, E, R, M, R);
    printf("  %snova%s                         %sInteractive tool shell with pipes%s\n\n",
           N, R, M, R);

    /* ── SCRIPT EXECUTION ── */
    printf("%s%sSCRIPT EXECUTION%s\n", H, U, R);
    printf("  %s-e%s <expr>              %sExecute expression string%s\n", E, R, M, R);
    printf("  %s-c%s <file>              %sCompile script to bytecode (.no)%s\n", E, R, M, R);
    printf("  %s-o%s <file>              %sOutput file for -c (default: input.no)%s\n", E, R, M, R);
    printf("  %s-O0%s                    %sDisable optimizations%s\n", E, R, M, R);
    printf("  %s-O1%s, %s-O%s                %sEnable optimizations (default)%s\n", E, R, E, R, M, R);
    printf("  %s-s%s                     %sStrip debug info from compiled output%s\n\n", E, R, M, R);

    /* ── OPTIONS ── */
    printf("%s%sOPTIONS%s\n", H, U, R);
    printf("  %s-v%s, %s--verbose%s          %sVerbose output%s\n", E, R, E, R, M, R);
    printf("  %s--no-color%s             %sDisable colored terminal output%s\n", E, R, M, R);
    printf("  %s--explain%s <code>       %sShow detailed explanation for error code%s\n", E, R, M, R);
    printf("  %s-h%s, %s--help%s             %sShow this help message%s\n", E, R, E, R, M, R);
    printf("  %s-V%s, %s--version%s          %sShow version information%s\n\n", E, R, E, R, M, R);

    /* ── DEBUG & DIAGNOSTICS ── */
    printf("%s%sDEBUG & DIAGNOSTICS%s\n", H, U, R);
    printf("  %s--lex%s <file>           %sTokenize and print tokens%s\n", E, R, M, R);
    printf("  %s--parse%s <file>         %sParse file and report AST statistics%s\n", E, R, M, R);
    printf("  %s--ast%s <file>           %sDump abstract syntax tree%s\n", E, R, M, R);
    printf("  %s--dis%s <file>           %sCompile and disassemble bytecode%s\n", E, R, M, R);
    printf("  %s--trace%s=%sCHANNELS%s       %sRuntime trace (vm,call,stack,gc,module,all)%s\n\n",
           E, R, L, R, M, R);

    /* ── TOOLS ── */
    nova_tool_print_help();

    /* ── SCRIPTING LANGUAGE ── */
    printf("%s%sLANGUAGE QUICK REFERENCE%s\n", H, U, R);
    printf("  %secho%s(%s\"hello\"%s, 42)           %sPreferred output (tab-separated + newline)%s\n",
           E, R, L, R, M, R);
    printf("  %sprintf%s(%s\"n=%%d\\n\"%s, 42)        %sC-style formatted output%s\n",
           E, R, L, R, M, R);
    printf("  %ss%s:%supper%s()  %ss%s:%sfind%s(%s\"x\"%s)   %sString methods via colon syntax%s\n",
           E, R, E, R, E, R, E, R, L, R, M, R);
    printf("  %st%s = {10, 20, 30}            %sTables are 0-indexed%s\n", E, R, M, R);
    printf("  %s`hello ${name}`%s              %sBacktick string interpolation%s\n", L, R, M, R);
    printf("  %s#import%s json                 %sPreprocessor module imports%s\n", E, R, M, R);
    printf("  %spcall%s(fn)  %sxpcall%s(fn, h)    %sProtected calls with error handling%s\n\n",
           E, R, E, R, M, R);

    /* ── DATA PROCESSING ── */
    printf("%s%sDATA PROCESSING%s\n", H, U, R);
    printf("  %s#import json%s   json.decode(s)   json.encode(t)   json.load(f)%s\n", E, R, R);
    printf("  %s#import csv%s    csv.decode(s)    csv.encode(t)    csv.load(f)%s\n", E, R, R);
    printf("  %s#import nini%s   nini.decode(s)   nini.encode(t)   nini.load(f)%s\n\n", E, R, R);

    /* ── EXAMPLES ── */
    printf("%s%sEXAMPLES%s\n", H, U, R);
    printf("  %s$%s %snova%s script.n                 %sRun a script%s\n",
           M, R, N, R, M, R);
    printf("  %s$%s %snova%s %s-e%s %s'echo(math.pi)'%s       %sQuick expression%s\n",
           M, R, N, R, E, R, L, R, M, R);
    printf("  %s$%s %snova%s %s-c%s app.n %s-s%s               %sCompile + strip for deployment%s\n",
           M, R, N, R, E, R, E, R, M, R);
    printf("  %s$%s %snova%s tree src/ %s-d%s=2            %sTree view of source directory%s\n",
           M, R, N, R, E, R, M, R);
    printf("  %s$%s %snova%s grep src/ %s-m%s=TODO %s-r%s       %sFind all TODOs recursively%s\n",
           M, R, N, R, E, R, E, R, M, R);
    printf("  %s$%s %snova%s find . %s-m%s=%s\"*.c\"%s            %sLocate all C source files%s\n",
           M, R, N, R, E, R, L, R, M, R);
    printf("  %s$%s %snova%s cat src/nova.c %s-n%s         %sPrint source with line numbers%s\n",
           M, R, N, R, E, R, M, R);
    printf("  %s$%s %snova%s task build test          %sRun build tasks from taskfile.nini%s\n",
           M, R, N, R, M, R);
    printf("  %s$%s %snova%s %s--explain%s E2001          %sExplain a compiler error code%s\n\n",
           M, R, N, R, E, R, M, R);
}

static void nova_print_version(void) {
    int c = nova_diag_color_enabled();

    const char *B  = c ? NOVA_COLOR_BANNER  : "";
    const char *N  = c ? NOVA_COLOR_NOVA    : "";
    const char *M  = c ? NOVA_COLOR_MUTED   : "";
    const char *W  = c ? NOVA_COLOR_PATH    : "";
    const char *A  = c ? NOVA_COLOR_ACCENT  : "";
    const char *E  = c ? NOVA_COLOR_EMERALD : "";
    const char *EC = c ? NOVA_COLOR_ERRCODE : "";
    const char *R  = c ? NOVA_COLOR_RESET   : "";

    printf("%s%s%s", B, NOVA_ASCII_ART, R);
    printf(" %sv%s%s\n", N, NOVA_CLI_VERSION_STRING, R);
    printf(" %s%s%s\n", M, NOVA_TAGLINE, R);
    printf(" %s%s%s\n\n", A, NOVA_SEPARATOR, R);

    printf("  %sVersion%s     %s%s%s\n", M, R, N, NOVA_VERSION_STRING, R);
    printf("  %sCopyright%s   %s%s%s\n", M, R, W, NOVA_COPYRIGHT, R);
    printf("  %sBuilt%s       %s%s %s%s\n", M, R, W, __DATE__, __TIME__, R);
#ifdef __GNUC__
    printf("  %sCompiler%s    %sGCC %d.%d.%d%s\n", M, R, W,
           __GNUC__, __GNUC_MINOR__, __GNUC_PATCHLEVEL__, R);
#endif
    printf("  %sStandard%s    %sZORYA-C v2.0.0%s\n", M, R, EC, R);
    printf("  %sPlatform%s    %sLinux x86_64 / C99%s\n", M, R, W, R);
    printf("\n");

    printf("  %sFeatures%s    %sNaN-boxing%s, %sregister-based VM%s, %scomputed-goto dispatch%s\n",
           M, R, E, R, E, R, E, R);
    printf("              %sDagger hash tables%s, %sWeave string interning%s\n",
           E, R, E, R);
    printf("              %stri-color mark-sweep GC%s, %sclosures & upvalues%s\n",
           E, R, E, R);
    printf("              %scoroutines%s, %sasync/await%s, %smetatables%s, %sNINI config%s\n",
           E, R, E, R, E, R, E, R);
    printf("              %sNDP data processing%s, %sCLI tools%s, %stask runner%s\n\n",
           E, R, E, R, E, R);
}

static int nova_parse_args(int argc, char **argv, NovaOptions *opts) {
    int i = 1;

    while (i < argc) {
        const char *arg = argv[i];

        /* Check for options */
        if (arg[0] == '-') {
            /* Long options */
            if (strcmp(arg, "--help") == 0) {
                opts->mode = NOVA_MODE_HELP;
                return 0;  /* Continue to switch in main */
            }
            if (strcmp(arg, "--version") == 0) {
                opts->mode = NOVA_MODE_VERSION;
                return 0;  /* Continue to switch in main */
            }
            if (strcmp(arg, "--verbose") == 0) {
                opts->verbose = 1;
                i++;
                continue;
            }
            if (strcmp(arg, "--no-color") == 0) {
                opts->no_color = 1;
                i++;
                continue;
            }
            if (strncmp(arg, "--trace=", 8) == 0) {
                opts->trace_channels = arg + 8;
                i++;
                continue;
            }
            if (strcmp(arg, "--trace") == 0) {
                if (i + 1 >= argc) {
                    fprintf(stderr,
                            "nova: --trace requires channels "
                            "(e.g. --trace=vm,call,stack)\n");
                    return -1;
                }
                opts->trace_channels = argv[++i];
                i++;
                continue;
            }
            if (strcmp(arg, "--explain") == 0) {
                if (i + 1 >= argc) {
                    fprintf(stderr, "nova: --explain requires an error code argument\n");
                    return -1;
                }
                opts->explain_code = argv[++i];
                i++;
                continue;
            }
            if (strcmp(arg, "--lex") == 0) {
                if (i + 1 >= argc) {
                    fprintf(stderr, "nova: --lex requires a file argument\n");
                    return -1;
                }
                opts->mode = NOVA_MODE_LEX;
                opts->input_file = argv[++i];
                i++;
                continue;
            }
            if (strcmp(arg, "--parse") == 0) {
                if (i + 1 >= argc) {
                    fprintf(stderr, "nova: --parse requires a file argument\n");
                    return -1;
                }
                opts->mode = NOVA_MODE_PARSE;
                opts->input_file = argv[++i];
                i++;
                continue;
            }
            if (strcmp(arg, "--ast") == 0) {
                if (i + 1 >= argc) {
                    fprintf(stderr, "nova: --ast requires a file argument\n");
                    return -1;
                }
                opts->mode = NOVA_MODE_AST;
                opts->input_file = argv[++i];
                i++;
                continue;
            }
            if (strcmp(arg, "--dis") == 0) {
                if (i + 1 >= argc) {
                    fprintf(stderr, "nova: --dis requires a file argument\n");
                    return -1;
                }
                opts->mode = NOVA_MODE_DIS;
                opts->input_file = argv[++i];
                i++;
                continue;
            }

            /* Short options */
            if (strcmp(arg, "-h") == 0) {
                opts->mode = NOVA_MODE_HELP;
                return 0;  /* Continue to switch in main */
            }
            if (strcmp(arg, "-V") == 0) {
                opts->mode = NOVA_MODE_VERSION;
                return 0;  /* Continue to switch in main */
            }
            if (strcmp(arg, "-v") == 0) {
                opts->verbose = 1;
                i++;
                continue;
            }
            if (strcmp(arg, "-e") == 0) {
                if (i + 1 >= argc) {
                    fprintf(stderr, "nova: -e requires an expression argument\n");
                    return -1;
                }
                opts->mode = NOVA_MODE_EVAL;
                opts->eval_string = argv[++i];
                i++;
                continue;
            }
            if (strcmp(arg, "-c") == 0) {
                if (i + 1 >= argc) {
                    fprintf(stderr, "nova: -c requires a file argument\n");
                    return -1;
                }
                opts->mode = NOVA_MODE_COMPILE;
                opts->input_file = argv[++i];
                i++;
                continue;
            }
            if (strcmp(arg, "-o") == 0) {
                if (i + 1 >= argc) {
                    fprintf(stderr, "nova: -o requires a file argument\n");
                    return -1;
                }
                opts->output_file = argv[++i];
                i++;
                continue;
            }
            if (strcmp(arg, "-O0") == 0) {
                opts->optimize = 0;
                i++;
                continue;
            }
            if (strcmp(arg, "-O1") == 0 || strcmp(arg, "-O") == 0) {
                opts->optimize = 1;
                i++;
                continue;
            }
            if (strcmp(arg, "-s") == 0) {
                opts->strip_debug = 1;
                i++;
                continue;
            }

            /* Unknown option */
            fprintf(stderr, "nova: unknown option: %s\n", arg);
            fprintf(stderr, "Try 'nova --help' for usage information.\n");
            return -1;
        }

        /* Not an option - must be script file */
        opts->mode = NOVA_MODE_EXECUTE;
        opts->input_file = arg;

        /* Remaining args are script arguments */
        opts->script_argc = argc - i;
        opts->script_argv = &argv[i];
        break;
    }

    return 0;
}

/* ============================================================
 * FILE I/O UTILITIES
 * ============================================================ */

static char *nova_read_file(const char *path, size_t *out_size) {
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        fprintf(stderr, "nova: cannot open '%s': %s\n", path, strerror(errno));
        return NULL;
    }

    /* Get file size */
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size < 0) {
        fprintf(stderr, "nova: cannot read '%s': %s\n", path, strerror(errno));
        fclose(f);
        return NULL;
    }

    /* Allocate buffer with null terminator */
    char *buffer = (char *)malloc((size_t)size + 1);
    if (buffer == NULL) {
        fprintf(stderr, "nova: out of memory reading '%s'\n", path);
        fclose(f);
        return NULL;
    }

    /* Read file */
    size_t read_size = fread(buffer, 1, (size_t)size, f);
    fclose(f);

    if (read_size != (size_t)size) {
        fprintf(stderr, "nova: error reading '%s'\n", path);
        free(buffer);
        return NULL;
    }

    buffer[size] = '\0';

    if (out_size != NULL) {
        *out_size = (size_t)size;
    }

    return buffer;
}

static int nova_is_bytecode_file(const char *path) {
    size_t len = strlen(path);
    if (len < 3) return 0;
    return strcmp(path + len - 3, ".no") == 0;
}

/* ============================================================
 * SCRIPT ARGUMENT REGISTRATION
 * ============================================================ */

static void nova_register_args(NovaVM *vm, int argc, char **argv) {
    /* Create 'arg' table in globals:
     *   arg[-1] = "nova" (interpreter)
     *   arg[0]  = script name
     *   arg[1..n] = script arguments
     */

    if (vm == NULL || argv == NULL) {
        return;
    }

    nova_vm_push_table(vm);
    NovaValue arg_table = nova_vm_get(vm, -1);
    vm->stack_top--;  /* Pop table but keep reference */

    if (!nova_is_table(arg_table)) {
        return;
    }

    /* Set arg[0] = script name (or "nova" if interactive) */
    /* We set integer keys via the array part */
    NovaTable *tbl = nova_as_table(arg_table);

    /* Pre-allocate array for argc entries */
    if (argc > 0) {
        uint32_t needed = (uint32_t)argc;
        if (tbl->array_size < needed) {
            uint32_t sz = 4;
            while (sz < needed) {
                sz *= 2;
            }
            tbl->array = (NovaValue *)calloc(sz, sizeof(NovaValue));
            if (tbl->array != NULL) {
                for (uint32_t i = 0; i < sz; i++) {
                    tbl->array[i] = nova_value_nil();
                }
                tbl->array_size = sz;
            }
        }
    }

    /* Fill arg[0], arg[1], ... = argv[0], argv[1], ... (0-based) */
    for (int i = 0; i < argc; i++) {
        NovaString *s = NULL;
        size_t slen = strlen(argv[i]);

        /* Push then pop to create the string through the VM */
        nova_vm_push_string(vm, argv[i], slen);
        NovaValue sv = nova_vm_get(vm, -1);
        vm->stack_top--;

        /* Store as arg[i] (0-based) */
        uint32_t idx = (uint32_t)i;
        if (idx < tbl->array_size) {
            tbl->array[idx] = sv;
            if (idx + 1 > tbl->array_used) {
                tbl->array_used = idx + 1;
            }
        }
        (void)s;
    }

    nova_vm_set_global(vm, "arg", arg_table);
}

/* ============================================================
 * RUNTIME ERROR DISPLAY HELPER
 *
 * Parses the "source:line: msg" format from novai_error() in
 * nova_vm.c to extract a line number for caret-style display.
 * Also maps VM status codes to specific NovaErrorCode values.
 * ============================================================ */

/**
 * @brief Map a VM status code to a NovaErrorCode.
 */
static NovaErrorCode novai_map_vm_status(int status) {
    switch (status) {
        case NOVA_VM_ERR_MEMORY:        return NOVA_E2003;
        case NOVA_VM_ERR_STACKOVERFLOW: return NOVA_E2002;
        case NOVA_VM_ERR_TYPE:          return NOVA_E2001;
        case NOVA_VM_ERR_DIVZERO:       return NOVA_E2005;
        case NOVA_VM_ERR_NULLPTR:       return NOVA_E2004;
        default:                        return NOVA_E2012;
    }
}

/**
 * @brief Parse "source:line: msg" to extract the line number.
 *
 * The VM error format is "filename:NNN: actual message".
 * We find the last colon-delimited number to get the line.
 *
 * @param err     Full error string from nova_vm_error()
 * @param out_msg Set to pointer within err where the clean message starts
 * @return Line number, or 0 if parsing fails
 */
static int novai_parse_error_line(const char *err, const char **out_msg) {
    if (err == NULL) {
        *out_msg = err;
        return 0;
    }

    /* Find the pattern ":NNN: " — a colon, digits, colon, space.
     * We scan from the start because format is "file:line: msg". */
    const char *p = err;
    int line = 0;

    /* Skip past the filename (find first colon followed by digits) */
    while (*p != '\0') {
        if (*p == ':') {
            const char *numstart = p + 1;
            int n = 0;
            int has_digit = 0;
            while (*numstart >= '0' && *numstart <= '9') {
                n = n * 10 + (*numstart - '0');
                numstart++;
                has_digit = 1;
            }
            if (has_digit && *numstart == ':') {
                line = n;
                /* Skip ": " to get the clean message */
                numstart++;  /* skip ':' */
                if (*numstart == ' ') numstart++;
                *out_msg = numstart;
                return line;
            }
        }
        p++;
    }

    *out_msg = err;
    return 0;
}

/**
 * @brief Report a runtime error with rich diagnostics.
 *
 * Extracts line number from the error string, maps the VM status
 * to an error code, and emits a caret-style diagnostic.
 *
 * @param vm      VM with error state
 * @param name    Source filename
 */
static void novai_report_runtime_error(NovaVM *vm, const char *name) {
    const char *err = nova_vm_error(vm);
    if (err == NULL) { return; }

    const char *clean_msg = err;
    int line = novai_parse_error_line(err, &clean_msg);

    /* Prefer fine-grained diag_code when set by the error site */
    NovaErrorCode code = (vm->diag_code != 0)
                       ? (NovaErrorCode)vm->diag_code
                       : novai_map_vm_status(vm->status);

    /* Create primary diagnostic, then attach NLP-driven hints */
    NovaDiagnostic diag;
    nova_diag_create(&diag, NOVA_DIAG_ERROR, code,
                     name, line, 0, "%s", clean_msg);

    NovaDiagnostic hints[4];
    int nhints = nova_suggest_runtime_hints(
        hints, 4, (int)code, clean_msg,
        diag.source_line, diag.source_line_len);

    for (int h = 0; h < nhints; h++) {
        nova_diag_attach(&diag, &hints[h]);
    }

    nova_diag_emit(&diag);

    /* Print stack traceback for unhandled errors */
    char *tb = nova_vm_traceback(vm, NULL, 0);
    if (tb != NULL) {
        fprintf(stderr, "%s\n", tb);
        free(tb);
    }
}

/* ============================================================
 * EXECUTION: RUN SOURCE FILE
 * ============================================================ */

static int nova_run_file(NovaVM *vm, const char *path, const NovaOptions *opts) {
    (void)opts;  /* TODO: Use for verbose output */

    /* Check for bytecode file */
    if (nova_is_bytecode_file(path)) {
        int load_err = 0;
        NovaProto *proto = nova_codegen_load(path, &load_err);
        if (proto == NULL) {
            fprintf(stderr, "nova: failed to load '%s': %s\n",
                    path, nova_codegen_strerror(load_err));
            return 1;
        }

        int status = nova_vm_execute(vm, proto);
        int result = (status == NOVA_VM_OK) ? 0 : 1;

        if (status != NOVA_VM_OK) {
            novai_report_runtime_error(vm, path);
        }

        nova_proto_destroy(proto);
        return result;
    }

    /* Read source file */
    size_t source_size = 0;
    char *source = nova_read_file(path, &source_size);
    if (source == NULL) {
        return 1;
    }

    /* Run the source */
    int result = nova_run_string(vm, source, path);

    free(source);
    return result;
}

/* ============================================================
 * EXECUTION: RUN SOURCE STRING
 * ============================================================ */

static int nova_run_string(NovaVM *vm, const char *source, const char *name) {
    /* Set source context for caret-style diagnostics */
    NovaDiagSource diag_src;
    diag_src.filename   = name;
    diag_src.source     = source;
    diag_src.source_len = strlen(source);
    nova_diag_set_source(&diag_src);

    /* === STAGE 1: Preprocessor === */
    NovaPP *pp = nova_pp_create();
    if (pp == NULL) {
        nova_diag_report(NOVA_DIAG_ERROR, NOVA_E3004,
                         name, 0, 0,
                         "failed to create preprocessor");
        return 1;
    }

    if (nova_pp_process_string(pp, source, strlen(source), name) != 0) {
        nova_diag_report(NOVA_DIAG_ERROR, NOVA_E3003,
                         name, 0, 0,
                         "preprocessor error: %s", nova_pp_error(pp));
        nova_pp_destroy(pp);
        nova_diag_set_source(NULL);
        return 1;
    }

    /* === STAGE 1.5: Open #import-ed data modules === */
    {
        uint32_t imports = nova_pp_get_imports(pp);
        if (imports != 0) {
            (void)nova_open_data_imports(vm, imports);
        }
    }

    /* === STAGE 2: Parser (row-based) === */
    NovaParser parser;
    if (nova_parser_init(&parser, pp) != 0) {
        nova_diag_report(NOVA_DIAG_ERROR, NOVA_E3004,
                         name, 0, 0,
                         "failed to initialize parser");
        nova_pp_destroy(pp);
        nova_diag_set_source(NULL);
        return 1;
    }

    /* Use row-based parser which populates parser.table */
    if (nova_parse_row(&parser, name) != 0) {
        /* Parser already emitted colored diagnostic via novai_error */
        nova_parser_free(&parser);
        nova_pp_destroy(pp);
        nova_diag_set_source(NULL);
        return 1;
    }

    /* === STAGE 3: Compiler === */
    NovaProto *proto = nova_compile(&parser.table, name);
    if (proto == NULL) {
        /* Compiler already emitted colored diagnostic via novai_error */
        nova_parser_free(&parser);
        nova_pp_destroy(pp);
        nova_diag_set_source(NULL);
        return 1;
    }

    /* === STAGE 4: Optimizer (optional) === */
    nova_optimize(proto, 1);  /* Level 1 optimization */

    /* === STAGE 5: VM Execution === */
    int status = nova_vm_execute(vm, proto);
    int result = (status == NOVA_VM_OK) ? 0 : 1;

    if (status != NOVA_VM_OK) {
        novai_report_runtime_error(vm, name);
    }

    /* Cleanup */
    nova_proto_destroy(proto);
    nova_parser_free(&parser);
    nova_pp_destroy(pp);
    nova_diag_set_source(NULL);

    return result;
}

/* ============================================================
 * EXECUTION: COMPILE TO BYTECODE
 * ============================================================ */

static int nova_compile_file(const char *input, const char *output, const NovaOptions *opts) {
    /* Determine output filename */
    char output_path[1024];
    if (output != NULL) {
        snprintf(output_path, sizeof(output_path), "%s", output);
    } else {
        /* Replace extension with .no */
        size_t len = strlen(input);
        if (len > 2 && input[len - 2] == '.') {
            snprintf(output_path, sizeof(output_path), "%.*s.no", (int)(len - 2), input);
        } else {
            snprintf(output_path, sizeof(output_path), "%s.no", input);
        }
    }

    /* Read source */
    size_t source_size = 0;
    char *source = nova_read_file(input, &source_size);
    if (source == NULL) {
        return 1;
    }

    /* Preprocessor */
    NovaPP *pp = nova_pp_create();
    if (pp == NULL) {
        fprintf(stderr, "nova: failed to create preprocessor\n");
        free(source);
        return 1;
    }

    if (nova_pp_process_string(pp, source, source_size, input) != 0) {
        fprintf(stderr, "nova: preprocessor error: %s\n", nova_pp_error(pp));
        nova_pp_destroy(pp);
        free(source);
        return 1;
    }

    /* Parser (row-based) */
    NovaParser parser;
    if (nova_parser_init(&parser, pp) != 0) {
        nova_pp_destroy(pp);
        free(source);
        return 1;
    }

    if (nova_parse_row(&parser, input) != 0) {
        fprintf(stderr, "nova: parse error in '%s': %s\n", input, nova_parser_error(&parser));
        nova_parser_free(&parser);
        nova_pp_destroy(pp);
        free(source);
        return 1;
    }

    /* Compiler */
    NovaProto *proto = nova_compile(&parser.table, input);
    if (proto == NULL) {
        fprintf(stderr, "nova: compilation error in '%s'\n", input);
        nova_parser_free(&parser);
        nova_pp_destroy(pp);
        free(source);
        return 1;
    }

    /* Optimizer (if enabled) */
    int opt_level = opts->optimize ? 1 : 0;
    nova_optimize(proto, opt_level);

    /* Save bytecode - use strip_debug flag */
    int flags = opts->strip_debug ? NOVA_CODEGEN_FLAG_STRIP : 0;
    int save_result = nova_codegen_save(proto, output_path, flags);

    if (save_result != 0) {
        fprintf(stderr, "nova: failed to write '%s': %s\n",
                output_path, nova_codegen_strerror(save_result));
        nova_proto_destroy(proto);
        nova_parser_free(&parser);
        nova_pp_destroy(pp);
        free(source);
        return 1;
    }

    if (opts->verbose) {
        printf("Compiled '%s' -> '%s'\n", input, output_path);
    }

    /* Cleanup */
    nova_proto_destroy(proto);
    nova_parser_free(&parser);
    nova_pp_destroy(pp);
    free(source);

    return 0;
}

/* ============================================================
 * DEBUG MODES
 * ============================================================ */

static int nova_debug_lex(const char *path) {
    size_t source_size = 0;
    char *source = nova_read_file(path, &source_size);
    if (source == NULL) {
        return 1;
    }

    NovaLexer lex;
    if (nova_lex_init(&lex, source, source_size, path) != 0) {
        fprintf(stderr, "nova: failed to initialize lexer\n");
        free(source);
        return 1;
    }

    printf("=== TOKENS: %s ===\n\n", path);

    int count = 0;
    NovaTokenType type;
    do {
        type = nova_lex_next(&lex);
        const NovaToken *tok = nova_lex_current(&lex);

        printf("%4d:%-3d  %-16s", tok->loc.line, tok->loc.column,
               nova_token_name(tok->type));

        /* Print token value for identifiers, strings, numbers */
        if (tok->type == NOVA_TOKEN_NAME ||
            tok->type == NOVA_TOKEN_STRING ||
            tok->type == NOVA_TOKEN_NUMBER ||
            tok->type == NOVA_TOKEN_INTEGER) {
            printf("  '%.*s'", (int)tok->value.string.len, tok->value.string.data);
        }
        printf("\n");
        count++;
    } while (type != NOVA_TOKEN_EOF && type != NOVA_TOKEN_ERROR);

    printf("\n=== %d tokens ===\n", count);

    nova_lex_free(&lex);
    free(source);
    return 0;
}

static int nova_debug_parse(const char *path) {
    size_t source_size = 0;
    char *source = nova_read_file(path, &source_size);
    if (source == NULL) {
        return 1;
    }

    NovaPP *pp = nova_pp_create();
    if (pp == NULL) {
        free(source);
        return 1;
    }

    if (nova_pp_process_string(pp, source, source_size, path) != 0) {
        fprintf(stderr, "nova: preprocessor error: %s\n", nova_pp_error(pp));
        nova_pp_destroy(pp);
        free(source);
        return 1;
    }

    NovaParser parser;
    if (nova_parser_init(&parser, pp) != 0) {
        nova_pp_destroy(pp);
        free(source);
        return 1;
    }

    printf("=== PARSING: %s ===\n\n", path);

    /* Use row-based parser */
    if (nova_parse_row(&parser, path) != 0) {
        printf("PARSE FAILED: %s\n", nova_parser_error(&parser));
        nova_parser_free(&parser);
        nova_pp_destroy(pp);
        free(source);
        return 1;
    }

    printf("PARSE SUCCESS\n");
    printf("  Expressions: %u\n", parser.table.expr_count);
    printf("  Statements:  %u\n", parser.table.stmt_count);
    printf("  Blocks:      %u\n", parser.table.block_count);

    nova_parser_free(&parser);
    nova_pp_destroy(pp);
    free(source);
    return 0;
}

/* ============================================================
 * AST DUMP HELPERS
 * ============================================================ */

static void novai_indent(int depth) {
    for (int i = 0; i < depth; i++) {
        printf("  ");
    }
}

static const char *novai_binop_str(NovaBinOp op) {
    static const char *names[] = {
        "+", "-", "*", "/", "//", "%%", "^",
        "..",
        "==", "~=", "<", "<=", ">", ">=",
        "and", "or",
        "&", "|", "~", "<<", ">>"
    };
    if (op < NOVA_BINOP_COUNT) return names[op];
    return "?";
}

static const char *novai_unop_str(NovaUnOp op) {
    static const char *names[] = { "-", "not", "#", "~" };
    if (op < NOVA_UNOP_COUNT) return names[op];
    return "?";
}

/* Forward declarations for mutual recursion */
static void novai_dump_expr(const NovaASTTable *t, NovaExprIdx idx, int depth);
static void novai_dump_stmt(const NovaASTTable *t, NovaStmtIdx idx, int depth);
static void novai_dump_block(const NovaASTTable *t, NovaBlockIdx idx, int depth);

static void novai_dump_expr(const NovaASTTable *t, NovaExprIdx idx, int depth) {
    if (idx == NOVA_IDX_NONE) {
        novai_indent(depth);
        printf("(none)\n");
        return;
    }
    const NovaRowExpr *e = &t->exprs[idx];
    novai_indent(depth);

    switch (e->kind) {
    case NOVA_EXPR_NIL:    printf("Nil\n"); break;
    case NOVA_EXPR_TRUE:   printf("True\n"); break;
    case NOVA_EXPR_FALSE:  printf("False\n"); break;
    case NOVA_EXPR_VARARG: printf("Vararg\n"); break;

    case NOVA_EXPR_INTEGER:
        printf("Integer: %" PRId64 "\n", (int64_t)e->as.integer.value);
        break;

    case NOVA_EXPR_NUMBER:
        printf("Number: %g\n", e->as.number.value);
        break;

    case NOVA_EXPR_STRING:
        printf("String: \"%.*s\"\n", (int)e->as.string.len, e->as.string.data);
        break;

    case NOVA_EXPR_NAME:
        printf("Name: %.*s\n", (int)e->as.string.len, e->as.string.data);
        break;

    case NOVA_EXPR_INDEX:
        printf("Index\n");
        novai_dump_expr(t, e->as.index.object, depth + 1);
        novai_dump_expr(t, e->as.index.index, depth + 1);
        break;

    case NOVA_EXPR_FIELD:
        printf("Field: .%.*s\n", (int)e->as.field.name_len, e->as.field.name);
        novai_dump_expr(t, e->as.field.object, depth + 1);
        break;

    case NOVA_EXPR_METHOD:
        printf("Method: :%.*s\n", (int)e->as.field.name_len, e->as.field.name);
        novai_dump_expr(t, e->as.field.object, depth + 1);
        break;

    case NOVA_EXPR_UNARY:
        printf("Unary: %s\n", novai_unop_str(e->as.unary.op));
        novai_dump_expr(t, e->as.unary.operand, depth + 1);
        break;

    case NOVA_EXPR_BINARY:
        printf("Binary: %s\n", novai_binop_str(e->as.binary.op));
        novai_dump_expr(t, e->as.binary.left, depth + 1);
        novai_dump_expr(t, e->as.binary.right, depth + 1);
        break;

    case NOVA_EXPR_CALL:
        printf("Call (%d args)\n", e->as.call.arg_count);
        novai_dump_expr(t, e->as.call.callee, depth + 1);
        for (int i = 0; i < e->as.call.arg_count; i++) {
            novai_dump_expr(t, t->expr_extra[e->as.call.args_start + (uint32_t)i], depth + 1);
        }
        break;

    case NOVA_EXPR_METHOD_CALL:
        printf("MethodCall (%d args)\n", e->as.call.arg_count);
        novai_dump_expr(t, e->as.call.callee, depth + 1);
        for (int i = 0; i < e->as.call.arg_count; i++) {
            novai_dump_expr(t, t->expr_extra[e->as.call.args_start + (uint32_t)i], depth + 1);
        }
        break;

    case NOVA_EXPR_TABLE:
        printf("Table (%d fields)\n", e->as.table.field_count);
        for (int i = 0; i < e->as.table.field_count; i++) {
            const NovaRowTableField *f = &t->fields[e->as.table.field_start + (uint32_t)i];
            novai_indent(depth + 1);
            if (f->kind == NOVA_FIELD_LIST) {
                printf("ListField:\n");
            } else if (f->kind == NOVA_FIELD_RECORD) {
                printf("RecordField:\n");
            } else {
                printf("BracketField:\n");
            }
            if (f->key != NOVA_IDX_NONE) {
                novai_dump_expr(t, f->key, depth + 2);
            }
            novai_dump_expr(t, f->value, depth + 2);
        }
        break;

    case NOVA_EXPR_FUNCTION:
        printf("Function");
        if (e->as.function.is_async) printf(" async");
        printf("(");
        for (int i = 0; i < e->as.function.param_count; i++) {
            const NovaRowParam *p = &t->params[e->as.function.param_start + (uint32_t)i];
            if (i > 0) printf(", ");
            printf("%.*s", (int)p->name_len, p->name);
        }
        if (e->as.function.is_variadic) {
            if (e->as.function.param_count > 0) printf(", ");
            printf("...");
        }
        printf(")\n");
        novai_dump_block(t, e->as.function.body, depth + 1);
        break;

    case NOVA_EXPR_INTERP_STRING:
        printf("InterpString (%d parts)\n", e->as.interp.part_count);
        for (int i = 0; i < e->as.interp.part_count; i++) {
            novai_dump_expr(t, t->expr_extra[e->as.interp.parts_start + (uint32_t)i], depth + 1);
        }
        break;

    case NOVA_EXPR_AWAIT:
        printf("Await\n");
        novai_dump_expr(t, e->as.async_op.operand, depth + 1);
        break;

    case NOVA_EXPR_SPAWN:
        printf("Spawn\n");
        novai_dump_expr(t, e->as.async_op.operand, depth + 1);
        break;

    case NOVA_EXPR_YIELD:
        printf("Yield\n");
        novai_dump_expr(t, e->as.async_op.operand, depth + 1);
        break;

    default:
        printf("Expr(%d)\n", e->kind);
        break;
    }
}

static void novai_dump_stmt(const NovaASTTable *t, NovaStmtIdx idx, int depth) {
    if (idx == NOVA_IDX_NONE) return;
    const NovaRowStmt *s = &t->stmts[idx];
    novai_indent(depth);

    switch (s->kind) {
    case NOVA_STMT_EXPR:
        printf("ExprStmt\n");
        novai_dump_expr(t, s->as.expr.expr, depth + 1);
        break;

    case NOVA_STMT_LOCAL:
        printf("Local:");
        for (int i = 0; i < s->as.local.name_count; i++) {
            const NovaRowNameRef *n = &t->names[s->as.local.names_start + (uint32_t)i];
            printf(" %.*s", (int)n->len, n->data);
        }
        printf("\n");
        for (int i = 0; i < s->as.local.value_count; i++) {
            novai_dump_expr(t, t->expr_extra[s->as.local.values_start + (uint32_t)i], depth + 1);
        }
        break;

    case NOVA_STMT_ASSIGN:
        printf("Assign\n");
        novai_indent(depth + 1);
        printf("targets:\n");
        for (int i = 0; i < s->as.assign.target_count; i++) {
            novai_dump_expr(t, t->expr_extra[s->as.assign.targets_start + (uint32_t)i], depth + 2);
        }
        novai_indent(depth + 1);
        printf("values:\n");
        for (int i = 0; i < s->as.assign.value_count; i++) {
            novai_dump_expr(t, t->expr_extra[s->as.assign.values_start + (uint32_t)i], depth + 2);
        }
        break;

    case NOVA_STMT_IF:
        printf("If (%d branches)\n", s->as.if_stmt.branch_count);
        for (int i = 0; i < s->as.if_stmt.branch_count; i++) {
            const NovaRowIfBranch *b = &t->branches[s->as.if_stmt.branch_start + (uint32_t)i];
            novai_indent(depth + 1);
            if (b->condition == NOVA_IDX_NONE) {
                printf("Else:\n");
            } else if (i == 0) {
                printf("If:\n");
                novai_dump_expr(t, b->condition, depth + 2);
            } else {
                printf("ElseIf:\n");
                novai_dump_expr(t, b->condition, depth + 2);
            }
            novai_dump_block(t, b->body, depth + 2);
        }
        break;

    case NOVA_STMT_WHILE:
        printf("While\n");
        novai_dump_expr(t, s->as.while_stmt.condition, depth + 1);
        novai_dump_block(t, s->as.while_stmt.body, depth + 1);
        break;

    case NOVA_STMT_REPEAT:
        printf("Repeat\n");
        novai_dump_block(t, s->as.repeat_stmt.body, depth + 1);
        novai_indent(depth + 1);
        printf("Until:\n");
        novai_dump_expr(t, s->as.repeat_stmt.condition, depth + 2);
        break;

    case NOVA_STMT_FOR_NUMERIC:
        printf("ForNumeric: %.*s\n",
               (int)s->as.for_numeric.name_len, s->as.for_numeric.name);
        novai_indent(depth + 1); printf("start:\n");
        novai_dump_expr(t, s->as.for_numeric.start, depth + 2);
        novai_indent(depth + 1); printf("stop:\n");
        novai_dump_expr(t, s->as.for_numeric.stop, depth + 2);
        if (s->as.for_numeric.step != NOVA_IDX_NONE) {
            novai_indent(depth + 1); printf("step:\n");
            novai_dump_expr(t, s->as.for_numeric.step, depth + 2);
        }
        novai_dump_block(t, s->as.for_numeric.body, depth + 1);
        break;

    case NOVA_STMT_FOR_GENERIC:
        printf("ForGeneric:");
        for (int i = 0; i < s->as.for_generic.name_count; i++) {
            const NovaRowNameRef *n = &t->names[s->as.for_generic.names_start + (uint32_t)i];
            printf(" %.*s", (int)n->len, n->data);
        }
        printf("\n");
        novai_indent(depth + 1); printf("iterators:\n");
        for (int i = 0; i < s->as.for_generic.iter_count; i++) {
            novai_dump_expr(t, t->expr_extra[s->as.for_generic.iters_start + (uint32_t)i], depth + 2);
        }
        novai_dump_block(t, s->as.for_generic.body, depth + 1);
        break;

    case NOVA_STMT_DO:
        printf("Do\n");
        novai_dump_block(t, s->as.do_stmt.body, depth + 1);
        break;

    case NOVA_STMT_BREAK:
        printf("Break\n");
        break;

    case NOVA_STMT_CONTINUE:
        printf("Continue\n");
        break;

    case NOVA_STMT_GOTO:
        printf("Goto: %.*s\n",
               (int)s->as.goto_stmt.label_len, s->as.goto_stmt.label);
        break;

    case NOVA_STMT_LABEL:
        printf("Label: %.*s\n",
               (int)s->as.label_stmt.label_len, s->as.label_stmt.label);
        break;

    case NOVA_STMT_RETURN:
        printf("Return");
        if (s->as.return_stmt.value_count == 0) {
            printf("\n");
        } else {
            printf(" (%d values)\n", s->as.return_stmt.value_count);
            for (int i = 0; i < s->as.return_stmt.value_count; i++) {
                novai_dump_expr(t, t->expr_extra[s->as.return_stmt.values_start + (uint32_t)i], depth + 1);
            }
        }
        break;

    case NOVA_STMT_FUNCTION:
    case NOVA_STMT_LOCAL_FUNCTION: {
        printf("%s", s->kind == NOVA_STMT_LOCAL_FUNCTION
                     ? "LocalFunction" : "Function");
        if (s->as.func_stmt.name != NOVA_IDX_NONE) {
            const NovaRowExpr *ne = &t->exprs[s->as.func_stmt.name];
            if (ne->kind == NOVA_EXPR_NAME) {
                printf(": %.*s", (int)ne->as.string.len, ne->as.string.data);
            } else if (ne->kind == NOVA_EXPR_FIELD &&
                       ne->as.field.object != NOVA_IDX_NONE) {
                const NovaRowExpr *obj = &t->exprs[ne->as.field.object];
                if (obj->kind == NOVA_EXPR_NAME) {
                    printf(": %.*s.%.*s",
                           (int)obj->as.string.len, obj->as.string.data,
                           (int)ne->as.field.name_len, ne->as.field.name);
                }
            } else if (ne->kind == NOVA_EXPR_METHOD &&
                       ne->as.field.object != NOVA_IDX_NONE) {
                const NovaRowExpr *obj = &t->exprs[ne->as.field.object];
                if (obj->kind == NOVA_EXPR_NAME) {
                    printf(": %.*s:%.*s",
                           (int)obj->as.string.len, obj->as.string.data,
                           (int)ne->as.field.name_len, ne->as.field.name);
                }
            }
        }
        if (s->as.func_stmt.is_async) printf(" async");
        printf("(");
        for (int i = 0; i < s->as.func_stmt.param_count; i++) {
            const NovaRowParam *p = &t->params[s->as.func_stmt.param_start + (uint32_t)i];
            if (i > 0) printf(", ");
            printf("%.*s", (int)p->name_len, p->name);
        }
        if (s->as.func_stmt.is_variadic) {
            if (s->as.func_stmt.param_count > 0) printf(", ");
            printf("...");
        }
        printf(")\n");
        novai_dump_block(t, s->as.func_stmt.body, depth + 1);
        break;
    }

    case NOVA_STMT_IMPORT:
        printf("Import: \"%.*s\"",
               (int)s->as.import_stmt.module_len, s->as.import_stmt.module);
        if (s->as.import_stmt.alias != NULL) {
            printf(" as %.*s",
                   (int)s->as.import_stmt.alias_len, s->as.import_stmt.alias);
        }
        printf("\n");
        break;

    case NOVA_STMT_EXPORT:
        printf("Export\n");
        novai_dump_expr(t, s->as.export_stmt.value, depth + 1);
        break;

    case NOVA_STMT_CONST:
        printf("Const: %.*s\n",
               (int)s->as.const_stmt.name_len, s->as.const_stmt.name);
        novai_dump_expr(t, s->as.const_stmt.value, depth + 1);
        break;

    default:
        printf("Stmt(%d)\n", s->kind);
        break;
    }
}

static void novai_dump_block(const NovaASTTable *t, NovaBlockIdx idx, int depth) {
    if (idx == NOVA_IDX_NONE) {
        novai_indent(depth);
        printf("(empty block)\n");
        return;
    }
    const NovaRowBlock *b = &t->blocks[idx];
    novai_indent(depth);
    printf("Block (%d stmts)\n", b->stmt_count);
    NovaStmtIdx si = b->first;
    while (si != NOVA_IDX_NONE) {
        novai_dump_stmt(t, si, depth + 1);
        si = t->stmts[si].next;
    }
}

static int nova_debug_ast(const char *path) {
    size_t source_size = 0;
    char *source = nova_read_file(path, &source_size);
    if (source == NULL) {
        return 1;
    }

    NovaPP *pp = nova_pp_create();
    if (pp == NULL) {
        free(source);
        return 1;
    }

    if (nova_pp_process_string(pp, source, source_size, path) != 0) {
        fprintf(stderr, "nova: preprocessor error: %s\n", nova_pp_error(pp));
        nova_pp_destroy(pp);
        free(source);
        return 1;
    }

    NovaParser parser;
    if (nova_parser_init(&parser, pp) != 0) {
        nova_pp_destroy(pp);
        free(source);
        return 1;
    }

    /* Use row-based parser */
    if (nova_parse_row(&parser, path) != 0) {
        fprintf(stderr, "nova: parse error: %s\n", nova_parser_error(&parser));
        nova_parser_free(&parser);
        nova_pp_destroy(pp);
        free(source);
        return 1;
    }

    printf("=== AST: %s ===\n", path);
    printf("  Expressions: %u\n", parser.table.expr_count);
    printf("  Statements:  %u\n", parser.table.stmt_count);
    printf("  Blocks:      %u\n", parser.table.block_count);
    printf("\n");
    novai_dump_block(&parser.table, parser.table.root, 0);

    nova_parser_free(&parser);
    nova_pp_destroy(pp);
    free(source);
    return 0;
}

static int nova_debug_dis(const char *path) {
    size_t source_size = 0;
    char *source = nova_read_file(path, &source_size);
    if (source == NULL) {
        return 1;
    }

    NovaPP *pp = nova_pp_create();
    if (pp == NULL) {
        free(source);
        return 1;
    }

    if (nova_pp_process_string(pp, source, source_size, path) != 0) {
        fprintf(stderr, "nova: preprocessor error: %s\n", nova_pp_error(pp));
        nova_pp_destroy(pp);
        free(source);
        return 1;
    }

    NovaParser parser;
    if (nova_parser_init(&parser, pp) != 0) {
        nova_pp_destroy(pp);
        free(source);
        return 1;
    }

    /* Use row-based parser */
    if (nova_parse_row(&parser, path) != 0) {
        fprintf(stderr, "nova: parse error: %s\n", nova_parser_error(&parser));
        nova_parser_free(&parser);
        nova_pp_destroy(pp);
        free(source);
        return 1;
    }

    NovaProto *proto = nova_compile(&parser.table, path);
    if (proto == NULL) {
        fprintf(stderr, "nova: compilation error\n");
        nova_parser_free(&parser);
        nova_pp_destroy(pp);
        free(source);
        return 1;
    }

    printf("=== DISASSEMBLY: %s ===\n\n", path);
    nova_proto_dump(proto, 0);  /* indent = 0 */
    printf("\n");

    nova_proto_destroy(proto);
    nova_parser_free(&parser);
    nova_pp_destroy(pp);
    free(source);
    return 0;
}
