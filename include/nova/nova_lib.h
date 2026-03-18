/**
 * @file nova_lib.h
 * @brief Nova Language - Standard Library Registration Infrastructure
 *
 * Provides the types, macros, and function declarations needed to
 * register C-implemented standard library functions with the Nova VM.
 *
 * Library registration pattern:
 *   1. Each module defines a static array of NovaLibReg entries
 *   2. Each module provides an opener: nova_open_<module>(vm)
 *   3. nova_open_libs(vm) calls all module openers
 *
 * C function calling convention (Lua-style):
 *   - nova_vm_get_top(vm) returns the number of arguments
 *   - nova_vm_get(vm, 0) returns the first argument
 *   - nova_vm_get(vm, n-1) returns the nth argument
 *   - nova_vm_push_*(vm, ...) pushes return values
 *   - The C function returns the number of results pushed
 *   - Return -1 after nova_vm_raise_error() to propagate errors
 *
 * @author Anthony Taliento
 * @date 2026-02-08
 * @version 0.2.0
 *
 * @copyright Copyright (c) 2026 Zorya Corporation
 * @license MIT
 *
 * DEPENDENCIES:
 *   - nova_vm.h
 *
 * ZORYA-C COMPLIANCE: v2.0.0
 */

#ifndef NOVA_LIB_H
#define NOVA_LIB_H

#include "nova_vm.h"

/* ============================================================
 * LIBRARY REGISTRATION
 * ============================================================ */

/**
 * @brief A single library function registration entry.
 *
 * An array of these describes a module. The last entry must
 * have name == NULL to mark the sentinel.
 */
typedef struct {
    const char    *name;    /** Function name (NULL = end sentinel) */
    nova_cfunc_t   func;    /** C implementation                   */
} NovaLibReg;

/**
 * @brief Register a list of C functions as globals.
 *
 * Iterates the list until a sentinel (name == NULL) is found,
 * calling nova_vm_set_global() for each entry.
 *
 * @param vm    VM instance (must not be NULL)
 * @param lib   Array of NovaLibReg (must end with {NULL, NULL})
 */
void nova_lib_register(NovaVM *vm, const NovaLibReg *lib);

/**
 * @brief Register a module as a table of functions.
 *
 * Creates a new table, populates it with the functions from lib,
 * and sets it as a global with the given module name.
 *
 * @param vm     VM instance (must not be NULL)
 * @param name   Module name (e.g. "math", "string")
 * @param lib    Array of NovaLibReg (must end with {NULL, NULL})
 */
void nova_lib_register_module(NovaVM *vm, const char *name,
                              const NovaLibReg *lib);

/* ============================================================
 * ARGUMENT CHECKING HELPERS
 * ============================================================ */

/**
 * @brief Check minimum argument count, raise error if insufficient.
 * @return 0 if OK, -1 if error was raised.
 */
static inline int nova_lib_check_args(NovaVM *vm, int min_args) {
    int got = nova_vm_get_top(vm);
    if (got < min_args) {
        nova_vm_raise_error(vm, "expected at least %d argument(s), got %d",
                            min_args, got);
        return -1;
    }
    return 0;
}

/**
 * @brief Check that argument at idx is a string, raise error if not.
 * @return Pointer to NovaString data, or NULL if error was raised.
 */
static inline const char *nova_lib_check_string(NovaVM *vm, int idx) {
    NovaValue v = nova_vm_get(vm, idx);
    if (!nova_is_string(v)) {
        nova_vm_raise_error(vm, "expected string for argument %d, got %s",
                            idx + 1, nova_vm_typename(nova_typeof(v)));
        return NULL;
    }
    return nova_str_data(nova_as_string(v));
}

/**
 * @brief Check that argument at idx is a number or integer.
 * @return 1 if OK and *out is filled, 0 if error was raised.
 */
static inline int nova_lib_check_number(NovaVM *vm, int idx,
                                        nova_number_t *out) {
    NovaValue v = nova_vm_get(vm, idx);
    if (nova_is_number(v)) {
        *out = nova_as_number(v);
        return 1;
    }
    if (nova_is_integer(v)) {
        *out = (nova_number_t)nova_as_integer(v);
        return 1;
    }
    nova_vm_raise_error(vm, "expected number for argument %d, got %s",
                        idx + 1, nova_vm_typename(nova_typeof(v)));
    return 0;
}

/**
 * @brief Check that argument at idx is an integer.
 * @return 1 if OK and *out is filled, 0 if error was raised.
 */
static inline int nova_lib_check_integer(NovaVM *vm, int idx,
                                         nova_int_t *out) {
    NovaValue v = nova_vm_get(vm, idx);
    if (nova_is_integer(v)) {
        *out = nova_as_integer(v);
        return 1;
    }
    if (nova_is_number(v)) {
        *out = (nova_int_t)nova_as_number(v);
        return 1;
    }
    nova_vm_raise_error(vm, "expected integer for argument %d, got %s",
                        idx + 1, nova_vm_typename(nova_typeof(v)));
    return 0;
}

/**
 * @brief Get optional argument string with default.
 * @return String data, or default_val if arg is nil/absent.
 */
static inline const char *nova_lib_opt_string(NovaVM *vm, int idx,
                                              const char *default_val) {
    if (idx >= nova_vm_get_top(vm)) {
        return default_val;
    }
    NovaValue v = nova_vm_get(vm, idx);
    if (nova_is_nil(v)) {
        return default_val;
    }
    if (nova_is_string(v)) {
        return nova_str_data(nova_as_string(v));
    }
    return default_val;
}

/* ============================================================
 * MODULE OPENERS
 *
 * Each function registers its module's functions into the VM.
 * Returns 0 on success, non-zero on failure.
 * ============================================================ */

/** Base library: print, type, tostring, tonumber, error, assert, etc. */
int nova_open_base(NovaVM *vm);

/** Math library: abs, ceil, floor, sqrt, sin, cos, etc. */
int nova_open_math(NovaVM *vm);

/** String library: len, sub, upper, lower, rep, find, format, etc. */
int nova_open_string(NovaVM *vm);

/** Table library: insert, remove, sort, concat, move, etc. */
int nova_open_table(NovaVM *vm);

/** I/O library: open, close, read, write, lines, etc. */
int nova_open_io(NovaVM *vm);

/** OS library: clock, date, time, getenv, execute, etc. */
int nova_open_os(NovaVM *vm);

/** Package/module library: require(), package.path, package.loaded */
int nova_open_package(NovaVM *vm);

/**
 * @brief Prepend a script's directory to the package search path.
 *
 * Extracts the directory component of the given script path and
 * prepends "dir/?.n;dir/?.no;dir/lib/?.n;dir/lib/?.no;" to the
 * current _package_path global so that require() can find modules
 * relative to the executing script.
 *
 * @param vm    VM instance (must not be NULL)
 * @param path  Path to the script file (must not be NULL)
 */
void nova_package_set_script_dir(NovaVM *vm, const char *path);

/** Coroutine library: create, resume, yield, wrap, status, etc. */
int nova_open_coroutine(NovaVM *vm);

/** Async library: run, spawn, sleep, status, wrap */
int nova_open_async(NovaVM *vm);

/** Data processing library: json, csv, ini, toml, html codecs */
int nova_open_data(NovaVM *vm);

/**
 * @brief Open data format modules based on #import flags.
 *
 * Called from the interpreter after preprocessing to selectively
 * register only the format modules requested via #import directives.
 *
 * @param vm            VM instance (must not be NULL)
 * @param import_flags  Bitfield of NOVA_IMPORT_* from nova_pp.h
 * @return 0 on success, -1 on failure
 */
int nova_open_data_imports(NovaVM *vm, uint32_t import_flags);

/** Data processing library: decode, encode, load, save, detect */
int nova_open_data(NovaVM *vm);

/** Debug library: traceback, getinfo, getlocal, sethook */
int nova_open_debug(NovaVM *vm);

/** Network library: get, post, put, delete, patch, head, request */
#ifndef NOVA_NO_NET
int nova_open_net(NovaVM *vm);
#endif

/** SQL library: open, exec, query, close (SQLite3 backend) */
int nova_open_sql(NovaVM *vm);

/** Filesystem library: exists, read, write, list, walk, glob, etc. */
int nova_open_fs(NovaVM *vm);

/** NLP library: tokenize, stem, fuzzy, freq, tfidf, ngrams, etc. */
int nova_open_nlp(NovaVM *vm);

/** Tools library: cat, ls, tree, find, grep, head, tail, wc, pwd, run */
int nova_open_tools(NovaVM *vm);

/* ============================================================
 * SHARED LIBRARY FUNCTIONS
 *
 * Functions exposed for cross-module use (e.g., printf/sprintf
 * in base library wrapping string.format).
 * ============================================================ */

/** string.format implementation — shared so printf/sprintf can reuse it */
int nova_string_format(NovaVM *vm);

/* ============================================================
 * CONVENIENCE: OPEN ALL STANDARD LIBRARIES
 * ============================================================ */

/**
 * @brief Open all standard libraries.
 *
 * Calls each module opener in order. Should be called once
 * after nova_vm_create() and before executing any code.
 *
 * @param vm  VM instance (must not be NULL)
 * @return 0 on success, non-zero on failure.
 */
int nova_open_libs(NovaVM *vm);

#endif /* NOVA_LIB_H */
