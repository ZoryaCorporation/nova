/**
 * @file nova.h
 * @brief Nova Language - Master Public Header (Umbrella)
 *
 * Single-include header for embedding Nova in C applications.
 * Drop this one header and you have the entire Nova API:
 *
 *   #include <nova/nova.h>
 *
 * This is modeled after sqlite3.h — one include gives you
 * everything Nova offers: VM lifecycle, value manipulation,
 * the full compilation pipeline (preprocessor → parser →
 * compiler → optimizer → codegen), the standard library
 * registration infrastructure, diagnostics, data processing,
 * and CLI tools.
 *
 * QUICK START (embedding):
 *
 *   #include <nova/nova.h>
 *
 *   int main(void) {
 *       // 1. Create VM
 *       NovaVM *vm = nova_vm_create();
 *       nova_open_libs(vm);
 *
 *       // 2. Preprocess source
 *       NovaPP *pp = nova_pp_create();
 *       nova_pp_process_file(pp, "script.n");
 *
 *       // 3. Parse
 *       NovaParser parser;
 *       nova_parser_init(&parser, pp);
 *       nova_parse_row(&parser, "script.n");
 *
 *       // 4. Compile
 *       NovaProto *proto = nova_compile(&parser.table, "script.n");
 *
 *       // 5. Execute
 *       nova_vm_execute(vm, proto);
 *
 *       // 6. Cleanup
 *       nova_proto_destroy(proto);
 *       nova_parser_free(&parser);
 *       nova_pp_destroy(pp);
 *       nova_vm_destroy(vm);
 *   }
 *
 * @author Anthony Taliento
 * @date 2026-02-05
 * @version 0.2.0
 *
 * @copyright Copyright (c) 2026 Zorya Corporation
 * @license MIT
 *
 * ZORYA-C COMPLIANCE: v2.0.0
 */

#ifndef NOVA_H
#define NOVA_H

/* ============================================================
 * LAYER 0 — Build Configuration
 *
 * Compile-time knobs: stack sizes, GC tuning, NaN-boxing
 * toggle, numeric types (nova_int_t, nova_number_t).
 * Override before including this header or via -D flags.
 * ============================================================ */

#include "nova_conf.h"

/* ============================================================
 * LAYER 1 — Instruction Set & Prototypes
 *
 * Bytecode encoding (iABC, iABx, iAsBx, iAx), opcode enum,
 * function prototypes (NovaProto), and instruction macros.
 * ============================================================ */

#include "nova_opcode.h"
#include "nova_proto.h"

/* ============================================================
 * LAYER 2 — VM Core
 *
 * The heart of Nova. Provides:
 *
 *   Lifecycle:     nova_vm_create(), nova_vm_destroy()
 *   Execution:     nova_vm_execute(), nova_vm_call(), nova_vm_pcall()
 *   Stack ops:     nova_vm_push_*(), nova_vm_get(), nova_vm_pop()
 *   Globals:       nova_vm_set_global(), nova_vm_get_global()
 *   Tables:        nova_vm_push_table(), nova_vm_set_field()
 *   Strings:       nova_vm_intern_string(), nova_string_new()
 *   Errors:        nova_vm_raise_error(), nova_vm_traceback()
 *
 *   Value Abstraction Layer (VAL):
 *     Constructors: nova_value_nil/bool/integer/number/string/table/...()
 *     Type queries:  nova_is_nil/bool/integer/number/string/table/...()
 *     Extractors:   nova_as_bool/integer/number/string/table/...()
 *     Truthiness:   nova_value_is_truthy()
 *
 *   GC:            NovaGCHeader, tri-color marking
 *   Closures:      nova_closure_new()
 *   Coroutines:    NovaCoroutine, NovaCoStatus
 *
 * All 900+ internal call sites use the VAL macros exclusively,
 * decoupling consumer code from the NaN-boxed/tagged-union IVR.
 * ============================================================ */

#include "nova_vm.h"

/* ============================================================
 * LAYER 3 — Compilation Pipeline
 *
 * Source → Tokens → AST → Bytecode → Optimized Bytecode
 *
 *   Lexer:         NovaToken, NovaTokenType, NovaSourceLoc
 *   Preprocessor:  nova_pp_create(), nova_pp_process_file/string()
 *                  #import flags (NOVA_IMPORT_JSON..NOVA_IMPORT_ALL)
 *   Parser:        nova_parser_init(), nova_parse_row()
 *                  NovaParser (with NovaASTTable row-based AST)
 *   AST:           NovaAST, NovaASTTable, NovaExprIdx, NovaStmtIdx
 *   Compiler:      nova_compile() — AST table → NovaProto
 *   Optimizer:     nova_optimize() — in-place bytecode transforms
 *   Codegen:       nova_codegen_save/load() — .no binary format
 * ============================================================ */

#include "nova_lex.h"
#include "nova_pp.h"
#include "nova_ast.h"
#include "nova_ast_row.h"
#include "nova_parse.h"
#include "nova_compile.h"
#include "nova_opt.h"
#include "nova_codegen.h"

/* ============================================================
 * LAYER 4 — Standard Library Registration
 *
 *   NovaLibReg:    {name, func} pairs for C function registration
 *   nova_open_libs():  Open all standard modules at once
 *   nova_open_*():     Open individual modules (base, string,
 *                      table, math, io, os, fs, tools, coroutine,
 *                      async, debug, package, net, sql, nlp, data)
 *
 * C function calling convention:
 *   - nova_vm_get_top(vm) → argument count
 *   - nova_vm_get(vm, 0..n-1) → read arguments
 *   - nova_vm_push_*(vm, ...) → push return values
 *   - Return count of values pushed (or -1 after raise_error)
 * ============================================================ */

#include "nova_lib.h"

/* ============================================================
 * LAYER 5 — Metamethods
 *
 * Operator overloading and prototype inheritance:
 *   __add, __sub, __mul, __div, __mod, __pow, __unm,
 *   __eq, __lt, __le, __index, __newindex, __call,
 *   __tostring, __len, __concat, __gc
 *
 * String metatable dispatch via OP_SELF enables s:upper() etc.
 * ============================================================ */

#include "nova_meta.h"

/* ============================================================
 * LAYER 6 — Diagnostics
 *
 * Rich error reporting with ANSI coloring:
 *   NovaDiagSeverity:  ERROR, WARNING, NOTE, HELP
 *   NovaErrorCode:     E1xxx (compile), E2xxx (runtime),
 *                      E3xxx (I/O), W1xxx (warnings)
 *   nova_diag_emit():  Caret-style source context display
 *   nova_error_name/explain():  Error catalog lookup
 * ============================================================ */

#include "nova_error.h"

/* ============================================================
 * LAYER 7 — Data Processing & Tools
 *
 *   NINI:   Nova's native config format (standalone codec)
 *   NDP:    Multi-format data processing
 *           (JSON, CSV, TSV, INI, TOML, HTML, YAML, NINI)
 *   Tools:  Built-in CLI tools (cat, ls, tree, find, grep,
 *           head, tail, wc, pwd) — both CLI and in-process
 * ============================================================ */

#include "nova_nini.h"
#include "nova_ndp.h"
#include "nova_tools.h"

/* ============================================================
 * CONVENIENCE ALIASES
 *
 * Shorter names for the most common operations. These map
 * directly to nova_vm_* functions with zero overhead.
 * ============================================================ */

/** @brief Alias: NovaVM is the Nova state handle */
typedef NovaVM NovaState;

/**
 * @brief Packed version number (major*10000 + minor*100 + patch)
 *
 * NOVA_VERSION_STRING is already defined in nova_conf.h.
 * Use NOVA_VERSION_STRING for display, NOVA_VERSION_NUM for
 * compile-time version checks:
 *
 *   #if NOVA_VERSION_NUM >= 2000
 *     // Use 0.2.0+ features
 *   #endif
 */
#define NOVA_VERSION_NUM \
    (NOVA_VERSION_MAJOR * 10000 + NOVA_VERSION_MINOR * 100 + NOVA_VERSION_PATCH)

#endif /* NOVA_H */
