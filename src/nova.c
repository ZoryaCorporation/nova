/**
 * @file nova.c
 * @brief Nova Language CLI - Main Entry Point
 *
 * The Nova command-line interface provides:
 *   - Script execution (.n, .m, .lua files)
 *   - Bytecode compilation (.no output)
 *   - Interactive REPL
 *   - String evaluation
 *   - Debug and diagnostic modes
 *
 * Usage:
 *   nova                     Interactive REPL
 *   nova script.n            Execute Nova script
 *   nova script.no           Execute bytecode
 *   nova -c script.n         Compile to .no
 *   nova -e "expr"           Evaluate expression
 *   nova -i script.n         Execute then REPL
 *   nova --lex script.n      Lex only (debug)
 *   nova --parse script.n    Parse only (debug)
 *   nova --ast script.n      Dump AST (debug)
 *   nova --dis script.n      Disassemble (debug)
 *
 * @author Anthony Taliento
 * @date 2026-02-07
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

/* ============================================================
 * VERSION AND BRANDING
 * ============================================================ */

#define NOVA_CLI_VERSION_MAJOR 0
#define NOVA_CLI_VERSION_MINOR 1
#define NOVA_CLI_VERSION_PATCH 0
#define NOVA_CLI_VERSION_STRING "0.1.0"

#define NOVA_BANNER \
    "Nova " NOVA_CLI_VERSION_STRING " -- Zorya Corporation\n" \
    "A Lua-inspired scripting language with modern features.\n" \
    "Type 'help' for commands, 'exit' to quit.\n"

/* ============================================================
 * CLI OPTIONS
 * ============================================================ */

typedef enum {
    NOVA_MODE_REPL,         /* Interactive REPL */
    NOVA_MODE_EXECUTE,      /* Execute script file */
    NOVA_MODE_COMPILE,      /* Compile to .no only */
    NOVA_MODE_EVAL,         /* Evaluate string (-e) */
    NOVA_MODE_INTERACT,     /* Execute then REPL (-i) */
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
static int nova_run_repl(NovaVM *vm, const NovaOptions *opts);
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
    opts.mode = NOVA_MODE_REPL;
    opts.optimize = 1;  /* Optimization on by default */
    opts.argc = argc;
    opts.argv = argv;

    /* Initialize diagnostic system (auto-detects TTY) */
    nova_diag_init();

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
        case NOVA_MODE_REPL:
            result = nova_run_repl(vm, &opts);
            break;

        case NOVA_MODE_EXECUTE:
            nova_package_set_script_dir(vm, opts.input_file);
            result = nova_run_file(vm, opts.input_file, &opts);
            break;

        case NOVA_MODE_EVAL:
            result = nova_run_string(vm, opts.eval_string, "=(eval)");
            break;

        case NOVA_MODE_INTERACT:
            nova_package_set_script_dir(vm, opts.input_file);
            result = nova_run_file(vm, opts.input_file, &opts);
            if (result == 0) {
                result = nova_run_repl(vm, &opts);
            }
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
    printf("Usage: %s [options] [script [args]]\n\n", prog);
    printf("Options:\n");
    printf("  -e <expr>       Execute expression string\n");
    printf("  -c <file>       Compile script to bytecode (.no)\n");
    printf("  -o <file>       Output file for -c (default: input.no)\n");
    printf("  -i <file>       Execute script then enter REPL\n");
    printf("  -O0             Disable optimizations\n");
    printf("  -O1, -O         Enable optimizations (default)\n");
    printf("  -s              Strip debug info from compiled output\n");
    printf("  -v, --verbose   Verbose output\n");
    printf("  --no-color      Disable colored error output\n");
    printf("  --explain <code> Show detailed explanation for error code\n");
    printf("  -h, --help      Show this help message\n");
    printf("  -V, --version   Show version information\n");
    printf("\n");
    printf("Debug options:\n");
    printf("  --lex <file>    Lex file and print tokens\n");
    printf("  --parse <file>  Parse file (check syntax)\n");
    printf("  --ast <file>    Parse and dump AST\n");
    printf("  --dis <file>    Compile and disassemble bytecode\n");
    printf("  --trace=<ch>    Enable trace channels (vm,call,stack,gc,module,all)\n");
    printf("\n");
    printf("Examples:\n");
    printf("  %s                    Start interactive REPL\n", prog);
    printf("  %s script.n           Execute Nova script\n", prog);
    printf("  %s script.no          Execute compiled bytecode\n", prog);
    printf("  %s -e 'print(42)'     Evaluate expression\n", prog);
    printf("  %s -c script.n        Compile to script.no\n", prog);
    printf("  %s -c script.n -o out.no  Compile with custom output\n", prog);
    printf("\n");
}

static void nova_print_version(void) {
    printf("Nova %s\n", NOVA_VERSION_STRING);
    printf("%s\n", NOVA_COPYRIGHT);
    printf("Built: %s %s\n", __DATE__, __TIME__);
#ifdef __GNUC__
    printf("Compiler: GCC %d.%d.%d\n", __GNUC__, __GNUC_MINOR__, __GNUC_PATCHLEVEL__);
#endif
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
            if (strcmp(arg, "-i") == 0) {
                if (i + 1 >= argc) {
                    fprintf(stderr, "nova: -i requires a file argument\n");
                    return -1;
                }
                opts->mode = NOVA_MODE_INTERACT;
                opts->input_file = argv[++i];
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
            const char *err = nova_vm_error(vm);
            if (err != NULL) {
                nova_diag_report(NOVA_DIAG_ERROR, NOVA_E2012,
                                 path, 0, 0,
                                 "runtime error: %s", err);
            }
            /* Print stack traceback for unhandled errors */
            char *tb = nova_vm_traceback(vm, NULL, 0);
            if (tb != NULL) {
                fprintf(stderr, "%s\n", tb);
                free(tb);
            }
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
        const char *err = nova_vm_error(vm);
        if (err != NULL) {
            nova_diag_report(NOVA_DIAG_ERROR, NOVA_E2012,
                             name, 0, 0,
                             "runtime error: %s", err);
        }
        /* Print stack traceback for unhandled errors */
        char *tb = nova_vm_traceback(vm, NULL, 0);
        if (tb != NULL) {
            fprintf(stderr, "%s\n", tb);
            free(tb);
        }
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
 * EXECUTION: INTERACTIVE REPL
 * ============================================================ */

static int nova_run_repl(NovaVM *vm, const NovaOptions *opts) {
    (void)opts;

    printf("%s", NOVA_BANNER);
    printf("\n");

    char line[4096];
    int running = 1;

    while (running) {
        printf("nova> ");
        fflush(stdout);

        if (fgets(line, sizeof(line), stdin) == NULL) {
            printf("\n");
            break;
        }

        /* Remove trailing newline */
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n') {
            line[len - 1] = '\0';
            len--;
        }

        /* Skip empty lines */
        if (len == 0) {
            continue;
        }

        /* Built-in REPL commands */
        if (strcmp(line, "exit") == 0 || strcmp(line, "quit") == 0) {
            printf("Goodbye!\n");
            break;
        }

        if (strcmp(line, "help") == 0) {
            printf("\nREPL Commands:\n");
            printf("  help          Show this help\n");
            printf("  exit, quit    Exit the REPL\n");
            printf("  version       Show version information\n");
            printf("  clear         Clear screen (if supported)\n");
            printf("\nType Nova code to execute it.\n\n");
            continue;
        }

        if (strcmp(line, "version") == 0) {
            nova_print_version();
            continue;
        }

        if (strcmp(line, "clear") == 0) {
            printf("\033[2J\033[H");  /* ANSI clear screen */
            continue;
        }

        /* Execute as Nova code */
        /* For REPL, wrap print around expressions for convenience */
        /* (Similar to Lua's = prefix behavior) */

        int result = nova_run_string(vm, line, "=repl");
        (void)result;  /* Errors already printed */
    }

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

    printf("=== AST: %s ===\n\n", path);
    /* AST pretty-print not yet implemented */
    printf("  Expressions: %u\n", parser.table.expr_count);
    printf("  Statements:  %u\n", parser.table.stmt_count);
    printf("  Blocks:      %u\n", parser.table.block_count);
    printf("\n  (Detailed AST dump not yet implemented)\n");
    printf("\n");

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
