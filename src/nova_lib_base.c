/**
 * @file nova_lib_base.c
 * @brief Nova Language - Base Standard Library
 *
 * The base library provides fundamental functions that are
 * registered as globals (not inside a module table).
 *
 * Functions:
 *   print(...)        Print values separated by tabs, newline at end
 *   type(v)           Return type name as string
 *   tostring(v)       Convert value to string representation
 *   tonumber(v [,b])  Convert value to number (optional base)
 *   tointeger(v)      Convert value to integer
 *   error(msg)        Raise a runtime error
 *   assert(v [,msg])  Assert value is truthy
 *   pcall(f, ...)     Protected call (catches errors)
 *   select(idx, ...)  Select arguments by index or '#' for count
 *   rawget(t, k)      Raw table access (no metamethods)
 *   rawset(t, k, v)   Raw table assignment (no metamethods)
 *   rawlen(t)         Raw length (array part size)
 *   rawequal(a, b)    Equality without metamethods
 *   ipairs(t)         Integer iterator factory
 *   pairs(t)          General iterator factory
 *   next(t [,k])      Next key/value after k in table
 *   unpack(t [,i,j])  Unpack table elements as multiple returns
 *   collectgarbage()   GC control interface
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
 *   - nova_vm.h
 *
 * ZORYA-C COMPLIANCE: v2.0.0
 */

#include "nova/nova_lib.h"
#include "nova/nova_vm.h"
#include "nova/nova_meta.h"
#include "nova/nova_pp.h"
#include "nova/nova_parse.h"
#include "nova/nova_compile.h"
#include "nova/nova_opt.h"
#include "nova/nova_trace.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <errno.h>

/* ============================================================
 * INTERNAL: VALUE-TO-STRING CONVERSION
 * ============================================================ */

/**
 * @brief Format a single NovaValue into a static buffer.
 *
 * Returns a pointer to a NUL-terminated string representation.
 * The returned pointer is valid until the next call (uses TLS
 * buffer or a static buffer).
 *
 * @param v  Value to format
 * @return Formatted string (static storage)
 */
static const char *novai_format_value(NovaValue v) {
    static char buf[128];

    switch (nova_typeof(v)) {
        case NOVA_TYPE_NIL:
            return "nil";

        case NOVA_TYPE_BOOL:
            return nova_as_bool(v) ? "true" : "false";

        case NOVA_TYPE_INTEGER:
            snprintf(buf, sizeof(buf), "%lld", (long long)nova_as_integer(v));
            return buf;

        case NOVA_TYPE_NUMBER: {
            /* Use %.14g like Lua for clean output */
            snprintf(buf, sizeof(buf), "%.14g", nova_as_number(v));
            return buf;
        }

        case NOVA_TYPE_STRING:
            return nova_str_data(nova_as_string(v));

        case NOVA_TYPE_TABLE:
            snprintf(buf, sizeof(buf), "table: %p", (void *)nova_as_table(v));
            return buf;

        case NOVA_TYPE_FUNCTION:
            snprintf(buf, sizeof(buf), "function: %p", (void *)nova_as_closure(v));
            return buf;

        case NOVA_TYPE_CFUNCTION:
            snprintf(buf, sizeof(buf), "cfunction: %p",
                     (void *)(uintptr_t)nova_as_cfunction(v));
            return buf;

        case NOVA_TYPE_USERDATA:
            snprintf(buf, sizeof(buf), "userdata: %p", nova_as_userdata(v));
            return buf;

        default:
            return "unknown";
    }
}

/* ============================================================
 * PART 1: PRINT
 * ============================================================ */

/**
 * @brief print(...) - Print values to stdout.
 *
 * Variadic. Values separated by tab, newline at end.
 * Uses tostring-style conversion for each value.
 *
 * @return 0 results.
 */
static int nova_base_print(NovaVM *vm) {
    int nargs = nova_vm_get_top(vm);

    for (int i = 0; i < nargs; i++) {
        if (i > 0) {
            putchar('\t');
        }
        NovaValue v = nova_vm_get(vm, i);
        const char *s = novai_format_value(v);
        fputs(s, stdout);
    }
    putchar('\n');
    fflush(stdout);

    return 0;  /* No results */
}

/* ============================================================
 * PART 2: TYPE
 * ============================================================ */

/**
 * @brief type(v) - Return the type name as a string.
 *
 * Returns "nil", "boolean", "integer", "number", "string",
 * "table", "function", or "userdata".
 * Note: cfunction returns "function" (like Lua).
 *
 * @return 1 result: string.
 */
static int nova_base_type(NovaVM *vm) {
    if (nova_lib_check_args(vm, 1) != 0) {
        return -1;
    }

    NovaValue v = nova_vm_get(vm, 0);
    const char *name = NULL;

    /* Map cfunction to "function" for user-facing API.
     * Use nova_typeof() for correct type priority (integers
     * before cfunctions in NaN-boxing). */
    NovaValueType t = nova_typeof(v);
    if (t == NOVA_TYPE_CFUNCTION) {
        name = "function";
    } else {
        name = nova_vm_typename(t);
    }

    nova_vm_push_string(vm, name, strlen(name));
    return 1;
}

/* ============================================================
 * PART 3: TOSTRING
 * ============================================================ */

/**
 * @brief tostring(v) - Convert a value to its string representation.
 *
 * @return 1 result: string.
 */
static int nova_base_tostring(NovaVM *vm) {
    if (nova_lib_check_args(vm, 1) != 0) {
        return -1;
    }

    NovaValue v = nova_vm_get(vm, 0);

    /* If already a string, return it directly */
    if (nova_is_string(v)) {
        nova_vm_push_string(vm, nova_str_data(nova_as_string(v)), nova_str_len(nova_as_string(v)));
        return 1;
    }

    /* Check for __tostring metamethod on tables */
    if (nova_is_table(v)) {
        NovaValue method = nova_meta_get_tm(v, NOVA_TM_TOSTRING);
        if (!nova_is_nil(method)) {
            NovaValue result = nova_value_nil();
            if (nova_meta_call1(vm, method, v, &result) == 0) {
                if (nova_is_string(result)) {
                    nova_vm_push_string(vm, nova_str_data(nova_as_string(result)),
                                        nova_str_len(nova_as_string(result)));
                    return 1;
                }
            }
        }
    }

    const char *s = novai_format_value(v);
    nova_vm_push_string(vm, s, strlen(s));
    return 1;
}

/* ============================================================
 * PART 4: TONUMBER
 * ============================================================ */

/**
 * @brief tonumber(v [, base]) - Convert to number.
 *
 * If v is already a number/integer, returns it as number.
 * If v is a string, attempts to parse it.
 * Optional base (2-36) for integer parsing from string.
 * Returns nil on failure.
 *
 * @return 1 result: number or nil.
 */
static int nova_base_tonumber(NovaVM *vm) {
    if (nova_lib_check_args(vm, 1) != 0) {
        return -1;
    }

    NovaValue v = nova_vm_get(vm, 0);
    int nargs = nova_vm_get_top(vm);

    /* Already a number */
    if (nova_is_number(v)) {
        nova_vm_push_number(vm, nova_as_number(v));
        return 1;
    }

    /* Integer -> number */
    if (nova_is_integer(v)) {
        nova_vm_push_number(vm, (nova_number_t)nova_as_integer(v));
        return 1;
    }

    /* String -> number */
    if (nova_is_string(v)) {
        const char *s = nova_str_data(nova_as_string(v));
        char *end = NULL;

        if (nargs >= 2) {
            /* Parse with explicit base */
            nova_int_t base = 10;
            NovaValue bval = nova_vm_get(vm, 1);
            if (nova_is_integer(bval)) {
                base = nova_as_integer(bval);
            } else if (nova_is_number(bval)) {
                base = (nova_int_t)nova_as_number(bval);
            }

            if (base < 2 || base > 36) {
                nova_vm_raise_error(vm, "invalid base %lld (must be 2-36)",
                                    (long long)base);
                return -1;
            }

            errno = 0;
            long long val = strtoll(s, &end, (int)base);
            if (end == s || errno != 0) {
                nova_vm_push_nil(vm);
                return 1;
            }
            /* Skip trailing whitespace */
            while (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r') {
                end++;
            }
            if (*end != '\0') {
                nova_vm_push_nil(vm);
                return 1;
            }
            nova_vm_push_number(vm, (nova_number_t)val);
            return 1;
        }

        /* Default: parse as floating point or integer */
        errno = 0;
        double d = strtod(s, &end);
        if (end == s) {
            nova_vm_push_nil(vm);
            return 1;
        }
        /* Skip trailing whitespace */
        while (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r') {
            end++;
        }
        if (*end != '\0') {
            nova_vm_push_nil(vm);
            return 1;
        }
        nova_vm_push_number(vm, d);
        return 1;
    }

    /* Other types: nil */
    nova_vm_push_nil(vm);
    return 1;
}

/* ============================================================
 * PART 5: TOINTEGER
 * ============================================================ */

/**
 * @brief tointeger(v) - Convert to integer.
 *
 * Returns integer directly if already integer.
 * Truncates number to integer.
 * Parses string as integer.
 * Returns nil on failure.
 *
 * @return 1 result: integer or nil.
 */
static int nova_base_tointeger(NovaVM *vm) {
    if (nova_lib_check_args(vm, 1) != 0) {
        return -1;
    }

    NovaValue v = nova_vm_get(vm, 0);

    if (nova_is_integer(v)) {
        nova_vm_push_integer(vm, nova_as_integer(v));
        return 1;
    }

    if (nova_is_number(v)) {
        /* Check if it has an exact integer representation */
        nova_number_t n = nova_as_number(v);
        nova_number_t floored = floor(n);
        if (n == floored) {
            nova_vm_push_integer(vm, (nova_int_t)n);
            return 1;
        }
        nova_vm_push_nil(vm);
        return 1;
    }

    if (nova_is_string(v)) {
        const char *s = nova_str_data(nova_as_string(v));
        char *end = NULL;
        errno = 0;
        long long val = strtoll(s, &end, 10);
        if (end != s && *end == '\0' && errno == 0) {
            nova_vm_push_integer(vm, (nova_int_t)val);
            return 1;
        }
    }

    nova_vm_push_nil(vm);
    return 1;
}

/* ============================================================
 * PART 6: ERROR
 * ============================================================ */

/**
 * @brief error(msg) - Raise a runtime error.
 *
 * @return Does not return normally (returns -1 to abort).
 */
static int nova_base_error(NovaVM *vm) {
    int nargs = nova_vm_get_top(vm);

    if (nargs >= 1) {
        NovaValue v = nova_vm_get(vm, 0);
        const char *msg = novai_format_value(v);
        nova_vm_raise_error(vm, "%s", msg);
    } else {
        nova_vm_raise_error(vm, "(error object is nil)");
    }

    return -1;  /* Abort execution */
}

/* ============================================================
 * PART 7: ASSERT
 * ============================================================ */

/**
 * @brief assert(v [, msg]) - Assert that v is truthy.
 *
 * If v is truthy, returns all arguments.
 * If v is falsy, raises an error with msg (default: "assertion failed").
 *
 * @return All arguments if truthy, or -1 on error.
 */
static int nova_base_assert(NovaVM *vm) {
    if (nova_lib_check_args(vm, 1) != 0) {
        return -1;
    }

    int nargs = nova_vm_get_top(vm);
    NovaValue v = nova_vm_get(vm, 0);

    if (!nova_value_is_truthy(v)) {
        if (nargs >= 2) {
            NovaValue msg = nova_vm_get(vm, 1);
            nova_vm_raise_error(vm, "%s", novai_format_value(msg));
        } else {
            nova_vm_raise_error(vm, "assertion failed!");
        }
        return -1;
    }

    /* Return all arguments */
    for (int i = 0; i < nargs; i++) {
        NovaValue arg = nova_vm_get(vm, i);
        nova_vm_push_value(vm, arg);
    }
    return nargs;
}

/* ============================================================
 * PART 8: SELECT
 * ============================================================ */

/**
 * @brief select(idx, ...) - Select arguments.
 *
 * If idx is "#", returns the number of remaining arguments.
 * If idx is a number, returns all arguments from that index on.
 *
 * @return Variable results.
 */
static int nova_base_select(NovaVM *vm) {
    if (nova_lib_check_args(vm, 1) != 0) {
        return -1;
    }

    int nargs = nova_vm_get_top(vm);
    NovaValue idx_val = nova_vm_get(vm, 0);

    /* select("#", ...) -> count of varargs */
    if (nova_is_string(idx_val) &&
        nova_str_len(nova_as_string(idx_val)) == 1 &&
        nova_str_data(nova_as_string(idx_val))[0] == '#') {
        nova_vm_push_integer(vm, (nova_int_t)(nargs - 1));
        return 1;
    }

    /* select(n, ...) -> args from index n onwards (0-based) */
    if (!nova_is_integer(idx_val) && !nova_is_number(idx_val)) {
        nova_vm_raise_error(vm, "bad argument #1 to 'select' "
                           "(number or string expected)");
        return -1;
    }

    nova_int_t idx = 0;
    if (nova_is_integer(idx_val)) {
        idx = nova_as_integer(idx_val);
    } else {
        idx = (nova_int_t)nova_as_number(idx_val);
    }

    /* 0-based: idx 0 means first vararg, idx must be in [0, nargs-2] */
    nova_int_t first_arg = idx + 1;  /* offset in VM stack (arg 0 = select itself) */
    if (first_arg < 1 || first_arg > nargs - 1) {
        nova_vm_raise_error(vm, "bad argument #1 to 'select' "
                           "(index out of range)");
        return -1;
    }

    int count = 0;
    for (nova_int_t i = first_arg; i < nargs; i++) {
        NovaValue v = nova_vm_get(vm, (int)i);
        nova_vm_push_value(vm, v);
        count++;
    }
    return count;
}

/* ============================================================
 * PART 9: PCALL (Protected Call)
 * ============================================================ */

/**
 * @brief pcall(f, ...) - Call f in protected mode.
 *
 * Returns true + results on success.
 * Returns false + error message on failure.
 *
 * NOTE: This is a simplified implementation. Full pcall with
 * longjmp-based error recovery will come in Phase 8.
 * For now, we just call the function and catch VM errors.
 *
 * @return Variable results.
 */
static int nova_base_pcall(NovaVM *vm) {
    if (nova_lib_check_args(vm, 1) != 0) {
        return -1;
    }

    /* pcall(f, arg1, arg2, ...) -> true, results... OR false, errmsg */
    int total = nova_vm_get_top(vm);
    int nargs = total - 1;

    /* Save the args from cfunc_base (stack may move during push) */
    NovaValue func_val = nova_vm_get(vm, 0);
    NovaValue saved_args[256];
    int safe_nargs = nargs < 256 ? nargs : 255;
    for (int i = 0; i < safe_nargs; i++) {
        saved_args[i] = nova_vm_get(vm, 1 + i);
    }

    /* Mark where cfunc results should go, using an offset from
     * vm->stack so we survive stack reallocation inside
     * nova_vm_pcall / nova_vm_call. */
    ptrdiff_t result_area_off = vm->stack_top - vm->stack;

    /* Push func + args at stack_top for nova_vm_pcall */
    nova_vm_push_value(vm, func_val);
    for (int i = 0; i < safe_nargs; i++) {
        nova_vm_push_value(vm, saved_args[i]);
    }

    /* nova_vm_pcall: on success, results at result_area[0..n-1].
     * On error, error msg at result_area[0]. */
    int rc = nova_vm_pcall(vm, safe_nargs, -1);

    /* Re-derive result_area after possible stack reallocation */
    NovaValue *result_area = vm->stack + result_area_off;

    /* Count how many values pcall left on the stack */
    int nvalues = (int)(vm->stack_top - result_area);

    if (rc == NOVA_VM_OK) {
        /* Success: shift results right by 1 and prepend true */
        for (int i = nvalues - 1; i >= 0; i--) {
            result_area[i + 1] = result_area[i];
        }
        result_area[0] = nova_value_bool(1);
        vm->stack_top = result_area + 1 + nvalues;
        return 1 + nvalues;
    }

    /* Error: result_area[0] = error msg string.
     * Shift right by 1 and prepend false */
    for (int i = nvalues - 1; i >= 0; i--) {
        result_area[i + 1] = result_area[i];
    }
    result_area[0] = nova_value_bool(0);
    vm->stack_top = result_area + 1 + nvalues;
    return 1 + nvalues;
}

/* ============================================================
 * PART 9b: XPCALL (Extended Protected Call)
 *
 * xpcall(f, handler, ...) -> true, results...  (on success)
 *                         -> false, handler(err) (on error)
 *
 * Like pcall, but if f raises an error, the handler function
 * is called with the error message BEFORE stack unwinding,
 * allowing it to add a traceback or transform the error.
 * ============================================================ */

/**
 * @brief xpcall(f, handler, ...) - Protected call with error handler.
 *
 * @param vm  VM instance
 * @return Variable results: 1 + nresults on success, 2 on error.
 *
 * @pre Arg 0 = callable, Arg 1 = error handler function
 *
 * ZORYA-C COMPLIANCE: v2.0.0
 */
static int nova_base_xpcall(NovaVM *vm) {
    if (nova_lib_check_args(vm, 2) != 0) {
        return -1;
    }

    /* xpcall(f, handler, arg1, arg2, ...) */
    int total = nova_vm_get_top(vm);
    int nargs = total - 2;  /* Args after f and handler */

    NovaValue func_val = nova_vm_get(vm, 0);
    NovaValue handler_val = nova_vm_get(vm, 1);

    /* Validate handler is callable */
    if (!nova_is_function(handler_val) &&
        !nova_is_cfunction(handler_val)) {
        nova_vm_raise_error(vm,
            "bad argument #2 to 'xpcall' (function expected, got %s)",
            nova_vm_typename(nova_typeof(handler_val)));
        return -1;
    }

    /* Save extra args (stack may move during push) */
    NovaValue saved_args[256];
    int safe_nargs = nargs < 256 ? nargs : 255;
    for (int i = 0; i < safe_nargs; i++) {
        saved_args[i] = nova_vm_get(vm, 2 + i);
    }

    /* Mark where results should go (offset, not pointer, to
     * survive stack reallocation inside nova_vm_pcall). */
    ptrdiff_t result_area_off = vm->stack_top - vm->stack;

    /* Push func + args at stack_top for nova_vm_pcall */
    nova_vm_push_value(vm, func_val);
    for (int i = 0; i < safe_nargs; i++) {
        nova_vm_push_value(vm, saved_args[i]);
    }

    int rc = nova_vm_pcall(vm, safe_nargs, -1);

    /* Re-derive result_area after possible stack reallocation */
    NovaValue *result_area = vm->stack + result_area_off;

    int nvalues = (int)(vm->stack_top - result_area);

    if (rc == NOVA_VM_OK) {
        /* Success: shift results right by 1 and prepend true */
        for (int i = nvalues - 1; i >= 0; i--) {
            result_area[i + 1] = result_area[i];
        }
        result_area[0] = nova_value_bool(1);
        vm->stack_top = result_area + 1 + nvalues;
        return 1 + nvalues;
    }

    /* Error path: result_area[0] has the raw error message.
     * Call the handler with it to get the transformed error. */
    NovaValue raw_err = (nvalues > 0) ? result_area[0] : nova_value_nil();

    /* Reset stack to result_area and call handler(raw_err) */
    vm->stack_top = result_area;
    nova_vm_push_value(vm, handler_val);
    nova_vm_push_value(vm, raw_err);

    int hrc = nova_vm_pcall(vm, 1, 1);

    /* Re-derive result_area after possible stack reallocation */
    result_area = vm->stack + result_area_off;

    if (hrc == NOVA_VM_OK) {
        /* Handler succeeded: result_area[0] = handler's return value */
        NovaValue handled_err = result_area[0];
        result_area[0] = nova_value_bool(0);
        result_area[1] = handled_err;
        vm->stack_top = result_area + 2;
        return 2;
    }

    /* Handler itself failed: use the original error message */
    result_area[0] = nova_value_bool(0);
    result_area[1] = raw_err;
    vm->stack_top = result_area + 2;
    return 2;
}

/* ============================================================
 * PART 10: RAW TABLE OPERATIONS
 * ============================================================ */

/**
 * @brief rawget(table, key) - Get without metamethods.
 */
static int nova_base_rawget(NovaVM *vm) {
    if (nova_lib_check_args(vm, 2) != 0) {
        return -1;
    }

    NovaValue t = nova_vm_get(vm, 0);
    if (!nova_is_table(t)) {
        nova_vm_raise_error(vm, "bad argument #1 to 'rawget' (table expected)");
        return -1;
    }

    NovaValue key = nova_vm_get(vm, 1);

    /* Integer key -> array access */
    if (nova_is_integer(key)) {
        nova_int_t idx = nova_as_integer(key);
        NovaTable *tbl = nova_as_table(t);
        if (idx >= 0 && (uint32_t)idx < nova_table_array_len(tbl)) {
            nova_vm_push_value(vm, nova_table_get_int(tbl, idx));
        } else {
            nova_vm_push_nil(vm);
        }
        return 1;
    }

    /* String key -> hash access */
    if (nova_is_string(key)) {
        NovaTable *tbl = nova_as_table(t);
        NovaValue found = nova_table_get_str(tbl, nova_as_string(key));
        nova_vm_push_value(vm, found);
        return 1;
    }

    nova_vm_push_nil(vm);
    return 1;
}

/**
 * @brief rawset(table, key, value) - Set without metamethods.
 */
static int nova_base_rawset(NovaVM *vm) {
    if (nova_lib_check_args(vm, 3) != 0) {
        return -1;
    }

    NovaValue t = nova_vm_get(vm, 0);
    if (!nova_is_table(t)) {
        nova_vm_raise_error(vm, "bad argument #1 to 'rawset' (table expected)");
        return -1;
    }

    NovaValue key = nova_vm_get(vm, 1);
    NovaValue val = nova_vm_get(vm, 2);

    /* Integer key -> array access */
    if (nova_is_integer(key)) {
        nova_int_t idx = nova_as_integer(key);
        if (idx >= 0) {
            NovaTable *tbl = nova_as_table(t);
            nova_table_set_int(vm, tbl, idx, val);
        }
    } else if (nova_is_string(key)) {
        /* Use the VM's internal table set */
        nova_vm_push_value(vm, val);
        nova_vm_set_field(vm, 0, nova_str_data(nova_as_string(key)));
    }

    /* Return the table (for chaining) */
    nova_vm_push_value(vm, t);
    return 1;
}

/**
 * @brief rawlen(t) - Get the raw length of a table or string.
 */
static int nova_base_rawlen(NovaVM *vm) {
    if (nova_lib_check_args(vm, 1) != 0) {
        return -1;
    }

    NovaValue v = nova_vm_get(vm, 0);

    if (nova_is_table(v)) {
        nova_vm_push_integer(vm,
            (nova_int_t)nova_table_array_len(nova_as_table(v)));
        return 1;
    }

    if (nova_is_string(v)) {
        nova_vm_push_integer(vm, (nova_int_t)nova_str_len(nova_as_string(v)));
        return 1;
    }

    nova_vm_raise_error(vm, "bad argument #1 to 'rawlen' "
                       "(table or string expected)");
    return -1;
}

/**
 * @brief rawequal(a, b) - Equality without metamethods.
 */
static int nova_base_rawequal(NovaVM *vm) {
    if (nova_lib_check_args(vm, 2) != 0) {
        return -1;
    }

    NovaValue a = nova_vm_get(vm, 0);
    NovaValue b = nova_vm_get(vm, 1);

    if (nova_typeof(a) != nova_typeof(b)) {
        nova_vm_push_bool(vm, 0);
        return 1;
    }

    int equal = 0;
    switch (nova_typeof(a)) {
        case NOVA_TYPE_NIL:
            equal = 1;
            break;
        case NOVA_TYPE_BOOL:
            equal = (nova_as_bool(a) == nova_as_bool(b));
            break;
        case NOVA_TYPE_INTEGER:
            equal = (nova_as_integer(a) == nova_as_integer(b));
            break;
        case NOVA_TYPE_NUMBER:
            equal = (nova_as_number(a) == nova_as_number(b));
            break;
        case NOVA_TYPE_STRING:
            equal = (nova_str_len(nova_as_string(a)) == nova_str_len(nova_as_string(b)) &&
                     nova_str_hash(nova_as_string(a)) == nova_str_hash(nova_as_string(b)) &&
                     memcmp(nova_str_data(nova_as_string(a)), nova_str_data(nova_as_string(b)),
                            nova_str_len(nova_as_string(a))) == 0);
            break;
        case NOVA_TYPE_TABLE:
            equal = (nova_as_table(a) == nova_as_table(b));
            break;
        case NOVA_TYPE_FUNCTION:
            equal = (nova_as_closure(a) == nova_as_closure(b));
            break;
        case NOVA_TYPE_CFUNCTION:
            equal = (nova_as_cfunction(a) == nova_as_cfunction(b));
            break;
        case NOVA_TYPE_USERDATA:
            equal = (nova_as_userdata(a) == nova_as_userdata(b));
            break;
        default:
            equal = 0;
            break;
    }

    nova_vm_push_bool(vm, equal);
    return 1;
}

/* ============================================================
 * PART 11: TABLE ITERATION
 * ============================================================ */

/* Internal state for ipairs iterator */
static int nova_base_ipairs_iter(NovaVM *vm) {
    if (nova_lib_check_args(vm, 2) != 0) {
        return -1;
    }

    NovaValue t = nova_vm_get(vm, 0);
    NovaValue idx_val = nova_vm_get(vm, 1);

    if (!nova_is_table(t)) {
        return 0;
    }

    nova_int_t idx = 0;
    if (nova_is_integer(idx_val)) {
        idx = nova_as_integer(idx_val) + 1;
    }

    NovaTable *tbl = nova_as_table(t);
    if (idx < 0 || (uint32_t)idx >= nova_table_array_len(tbl)) {
        return 0;  /* End of iteration */
    }

    NovaValue elem = nova_table_get_int(tbl, idx);
    if (nova_is_nil(elem)) {
        return 0;  /* Stop at nil */
    }

    nova_vm_push_integer(vm, idx);
    nova_vm_push_value(vm, elem);
    return 2;
}

/**
 * @brief ipairs(t) - Return iterator function, table, and 0.
 */
static int nova_base_ipairs(NovaVM *vm) {
    if (nova_lib_check_args(vm, 1) != 0) {
        return -1;
    }

    NovaValue t = nova_vm_get(vm, 0);
    if (!nova_is_table(t)) {
        nova_vm_raise_error(vm, "bad argument #1 to 'ipairs' (table expected)");
        return -1;
    }

    nova_vm_push_cfunction(vm, nova_base_ipairs_iter);
    nova_vm_push_value(vm, t);     /* Push the table reference */
    nova_vm_push_integer(vm, -1);
    return 3;
}

/**
 * @brief next(table [, key]) - Get next key/value pair.
 *
 * Iterates both array and hash parts of a table.
 * Pass nil key to get the first entry.
 * Returns key, value for the next entry, or nothing when done.
 */
static int nova_base_next(NovaVM *vm) {
    if (nova_lib_check_args(vm, 1) != 0) {
        return -1;
    }

    NovaValue tv = nova_vm_get(vm, 0);
    if (!nova_is_table(tv)) {
        nova_vm_raise_error(vm, "bad argument #1 to 'next' (table expected)");
        return -1;
    }

    NovaTable *tbl = nova_as_table(tv);
    int nargs = nova_vm_get_top(vm);
    NovaValue key = (nargs >= 2) ? nova_vm_get(vm, 1) : nova_value_nil();

    /* Find the iterator cursor for the current key */
    uint32_t iter_idx;
    if (nova_is_nil(key)) {
        iter_idx = 0;
    } else {
        iter_idx = nova_table_find_iter_idx(tbl, key);
        if (iter_idx == (uint32_t)-1) {
            /* Key was not found — end of iteration */
            return 0;
        }
    }

    /* Get the next entry */
    NovaValue out_key, out_val;
    if (nova_table_next(tbl, &iter_idx, &out_key, &out_val)) {
        nova_vm_push_value(vm, out_key);
        nova_vm_push_value(vm, out_val);
        return 2;
    }

    /* No more entries */
    return 0;
}

/**
 * @brief pairs(t) - Return next, table, nil.
 *
 * Returns the next() function, the table, and nil as
 * initial control value for generic-for iteration.
 */
static int nova_base_pairs(NovaVM *vm) {
    if (nova_lib_check_args(vm, 1) != 0) {
        return -1;
    }

    NovaValue t = nova_vm_get(vm, 0);
    if (!nova_is_table(t)) {
        nova_vm_raise_error(vm, "bad argument #1 to 'pairs' (table expected)");
        return -1;
    }

    nova_vm_push_cfunction(vm, nova_base_next);
    nova_vm_push_value(vm, t);     /* Push the table reference */
    nova_vm_push_nil(vm);          /* Initial control = nil    */
    return 3;
}

/* ============================================================
 * PART 12: UNPACK
 * ============================================================ */

/**
 * @brief unpack(list [, i [, j]]) - Unpack table elements.
 *
 * Returns list[i], list[i+1], ..., list[j].
 * Default i=1, j=#list.
 */
static int nova_base_unpack(NovaVM *vm) {
    if (nova_lib_check_args(vm, 1) != 0) {
        return -1;
    }

    NovaValue t = nova_vm_get(vm, 0);
    if (!nova_is_table(t)) {
        nova_vm_raise_error(vm, "bad argument #1 to 'unpack' (table expected)");
        return -1;
    }

    int nargs = nova_vm_get_top(vm);
    NovaTable *tbl = nova_as_table(t);
    nova_int_t i = 0;
    nova_int_t j = (nova_int_t)nova_table_array_len(tbl) - 1;

    if (nargs >= 2) {
        NovaValue vi = nova_vm_get(vm, 1);
        if (nova_is_integer(vi)) {
            i = nova_as_integer(vi);
        }
    }
    if (nargs >= 3) {
        NovaValue vj = nova_vm_get(vm, 2);
        if (nova_is_integer(vj)) {
            j = nova_as_integer(vj);
        }
    }

    int count = 0;
    for (nova_int_t k = i; k <= j; k++) {
        if (k < 0 || (uint32_t)k >= nova_table_array_len(tbl)) {
            nova_vm_push_nil(vm);
        } else {
            nova_vm_push_value(vm, nova_table_get_int(tbl, k));
        }
        count++;
    }
    return count;
}

/* ============================================================
 * PART 13: MISCELLANEOUS
 * ============================================================ */

/**
 * @brief collectgarbage([opt [, arg]]) - Control the garbage collector.
 *
 * Options:
 *   "collect"      - Force a full GC cycle (default)
 *   "stop"         - Stop the GC
 *   "restart"      - Restart the GC
 *   "count"        - Return memory in use (KB + bytes)
 *   "step"         - Single incremental step; arg = step size
 *   "setpause"     - Set GC pause %; arg = new value. Returns old.
 *   "setstepmul"   - Set GC step multiplier; arg = new value. Returns old.
 *   "isrunning"    - Return true if GC is active
 *
 * @return 0-2 results depending on option.
 */
static int nova_base_collectgarbage(NovaVM *vm) {
    int nargs = (int)(vm->stack_top - vm->cfunc_base);
    const char *opt = "collect";

    if (nargs >= 1) {
        NovaValue v0 = nova_vm_get(vm, 0);
        if (nova_is_string(v0) && nova_as_string(v0) != NULL) {
            opt = nova_str_data(nova_as_string(v0));
        }
    }

    nova_int_t arg = 0;
    if (nargs >= 2) {
        NovaValue v1 = nova_vm_get(vm, 1);
        if (nova_is_integer(v1)) {
            arg = nova_as_integer(v1);
        } else if (nova_is_number(v1)) {
            arg = (nova_int_t)nova_as_number(v1);
        }
    }

    if (strcmp(opt, "collect") == 0) {
        nova_gc_full_collect(vm);
        nova_vm_push_integer(vm, 0);
        return 1;
    }

    if (strcmp(opt, "stop") == 0) {
        vm->gc_running = 0;
        nova_vm_push_integer(vm, 0);
        return 1;
    }

    if (strcmp(opt, "restart") == 0) {
        vm->gc_running = 1;
        nova_vm_push_integer(vm, 0);
        return 1;
    }

    if (strcmp(opt, "count") == 0) {
        /* Return KB and byte remainder, like Lua */
        size_t bytes = vm->bytes_allocated;
        nova_vm_push_number(vm, (nova_number_t)bytes / 1024.0);
        nova_vm_push_integer(vm, (nova_int_t)(bytes % 1024));
        return 2;
    }

    if (strcmp(opt, "step") == 0) {
        int finished = 0;
        if (arg > 0) {
            vm->gc_debt = (size_t)(arg * 1024);
        }
        nova_gc_step(vm);
        finished = (vm->gc_phase == NOVA_GC_PHASE_PAUSE) ? 1 : 0;
        nova_vm_push_bool(vm, finished);
        return 1;
    }

    if (strcmp(opt, "setpause") == 0) {
        int old = vm->gc_pause;
        if (arg > 0) {
            vm->gc_pause = (int)arg;
        }
        nova_vm_push_integer(vm, (nova_int_t)old);
        return 1;
    }

    if (strcmp(opt, "setstepmul") == 0) {
        int old = vm->gc_step_mul;
        if (arg > 0) {
            vm->gc_step_mul = (int)arg;
        }
        nova_vm_push_integer(vm, (nova_int_t)old);
        return 1;
    }

    if (strcmp(opt, "isrunning") == 0) {
        nova_vm_push_bool(vm, vm->gc_running);
        return 1;
    }

    /* Unknown option - default to collect */
    nova_gc_full_collect(vm);
    nova_vm_push_integer(vm, 0);
    return 1;
}

/* ============================================================
 * PART 13b: PRINTF / SPRINTF
 * ============================================================ */

/**
 * @brief printf(fmt, ...) - Formatted print to stdout.
 *
 * Like string.format(), but writes the result to stdout
 * instead of returning it. Returns no values.
 *
 * This is a first-class function per the Nova design spec.
 */
static int nova_base_printf(NovaVM *vm) {
    /* Call string.format to build the formatted string */
    int got = nova_string_format(vm);
    if (got < 0) {
        return -1;
    }
    if (got >= 1) {
        NovaValue result = nova_vm_get(vm, -1);
        if (nova_is_string(result)) {
            fwrite(nova_str_data(nova_as_string(result)), 1, nova_str_len(nova_as_string(result)), stdout);
            fflush(stdout);
        }
        vm->stack_top--;  /* Pop the format result */
    }
    return 0;  /* printf returns nothing */
}

/**
 * @brief sprintf(fmt, ...) - Formatted string (alias for string.format).
 *
 * Returns a formatted string. Identical to string.format() but
 * available as a global for convenience.
 */
static int nova_base_sprintf(NovaVM *vm) {
    return nova_string_format(vm);
}

/**
 * @brief fprintf(file, fmt, ...) - Formatted print to file handle.
 *
 * Placeholder: for now, just prints to stdout or stderr.
 * arg 0 = file handle or string ("stdout"/"stderr")
 * arg 1+ = format string and arguments
 *
 * TODO: Integrate with io library file handles.
 */
static int nova_base_fprintf(NovaVM *vm) {
    if (nova_lib_check_args(vm, 2) != 0) {
        return -1;
    }

    NovaValue handle = nova_vm_get(vm, 0);
    FILE *out = stdout;

    /* Check for string handle name */
    if (nova_is_string(handle)) {
        if (strcmp(nova_str_data(nova_as_string(handle)), "stderr") == 0) {
            out = stderr;
        }
    }

    /* Shift arguments: skip the file handle, format from arg 1+ */
    /* We need to adjust cfunc_base to skip the handle */
    NovaValue *saved_base = vm->cfunc_base;
    if (vm->cfunc_base != NULL) {
        vm->cfunc_base = vm->cfunc_base + 1;
    }

    int got = nova_string_format(vm);

    vm->cfunc_base = saved_base;

    if (got < 0) {
        return -1;
    }
    if (got >= 1) {
        NovaValue result = nova_vm_get(vm, -1);
        if (nova_is_string(result)) {
            fwrite(nova_str_data(nova_as_string(result)), 1, nova_str_len(nova_as_string(result)), out);
            fflush(out);
        }
        vm->stack_top--;
    }
    return 0;
}

/* ============================================================
 * PART 13b: METATABLE FUNCTIONS
 * ============================================================ */

/**
 * @brief setmetatable(table, metatable) - Set a table's metatable.
 *
 * @param vm  Virtual machine instance
 * @return Number of results (1: the table)
 */
static int nova_base_setmetatable(NovaVM *vm) {
    if (nova_lib_check_args(vm, 2) != 0) {
        return -1;
    }

    NovaValue t = nova_vm_get(vm, 0);
    if (!nova_is_table(t)) {
        nova_vm_raise_error(vm,
            "bad argument #1 to 'setmetatable' (table expected)");
        return -1;
    }

    NovaValue mt = nova_vm_get(vm, 1);
    if (nova_is_nil(mt)) {
        nova_table_set_metatable(nova_as_table(t), NULL);
    } else if (nova_is_table(mt)) {
        nova_table_set_metatable(nova_as_table(t), nova_as_table(mt));
        /* Write barrier: table now references the metatable */
        nova_gc_barrier(vm, NOVA_GC_HDR(nova_as_table(t)));
    } else {
        nova_vm_raise_error(vm,
            "bad argument #2 to 'setmetatable' (nil or table expected)");
        return -1;
    }

    nova_vm_push_value(vm, t);
    return 1;
}

/**
 * @brief getmetatable(object) - Get an object's metatable.
 *
 * @param vm  Virtual machine instance
 * @return Number of results (1: metatable or nil)
 */
static int nova_base_getmetatable(NovaVM *vm) {
    if (nova_lib_check_args(vm, 1) != 0) {
        return -1;
    }

    NovaValue obj = nova_vm_get(vm, 0);
    if (nova_is_table(obj) &&
        nova_table_get_metatable(nova_as_table(obj)) != NULL) {
        /* Check for __metatable field in the metatable */
        nova_vm_push_value(vm,
            nova_value_table(nova_table_get_metatable(nova_as_table(obj))));
    } else {
        nova_vm_push_nil(vm);
    }
    return 1;
}

/**
 * @brief _VERSION - returns Nova version string.
 *
 * (Registered as a global string, not a function.)
 */

/* ============================================================
 * PART 13b: load() — Runtime String Compilation
 * ============================================================ */

/**
 * @brief load(source [, chunkname]) — Compile a string into a callable.
 *
 * Compiles the source string through the full pipeline
 * (lex -> pp -> parse -> compile -> optimize) and returns
 * the resulting closure.
 *
 * On success: returns the compiled function.
 * On failure: returns nil, error_message.
 *
 * @param vm  VM instance (must not be NULL)
 * @return Number of results pushed (1 on success, 2 on error).
 *
 * @example
 *   local f = load("return 42")
 *   assert(f() == 42)
 *
 *   local f2, err = load("invalid !!!")
 *   assert(f2 == nil)
 *   assert(type(err) == "string")
 */
static int nova_base_load(NovaVM *vm) {
    if (nova_lib_check_args(vm, 1) != 0) {
        return -1;
    }

    NovaValue *result_slot = vm->stack_top;

    /* Arg 1: source string (required) */
    NovaValue srcv = nova_vm_get(vm, 0);
    if (!nova_is_string(srcv)) {
        nova_vm_raise_error(vm,
            "bad argument #1 to 'load' (string expected)");
        return -1;
    }
    const char *source = nova_str_data(nova_as_string(srcv));
    size_t source_len = nova_str_len(nova_as_string(srcv));

    /* Arg 2: chunk name (optional, defaults to "=(load)") */
    const char *chunkname = "=(load)";
    NovaValue namev = nova_vm_get(vm, 1);
    if (nova_is_string(namev)) {
        chunkname = nova_str_data(nova_as_string(namev));
    }

    NTRACE(COMPILE, "load() source=%zu bytes chunkname='%s'",
           source_len, chunkname);

    /* === STAGE 1: Preprocessor === */
    NovaPP *pp = nova_pp_create();
    if (pp == NULL) {
        vm->stack_top = result_slot;
        nova_vm_push_value(vm, nova_value_nil());
        nova_vm_push_string(vm, "out of memory", 13);
        return 2;
    }

    if (nova_pp_process_string(pp, source, source_len, chunkname) != 0) {
        const char *err = nova_pp_error(pp);
        vm->stack_top = result_slot;
        nova_vm_push_value(vm, nova_value_nil());
        if (err != NULL) {
            nova_vm_push_string(vm, err, strlen(err));
        } else {
            nova_vm_push_string(vm, "preprocessor error", 18);
        }
        nova_pp_destroy(pp);
        return 2;
    }

    /* === STAGE 2: Parser === */
    NovaParser parser;
    if (nova_parser_init(&parser, pp) != 0) {
        vm->stack_top = result_slot;
        nova_vm_push_value(vm, nova_value_nil());
        nova_vm_push_string(vm, "parser initialization failed", 28);
        nova_pp_destroy(pp);
        return 2;
    }

    if (nova_parse_row(&parser, chunkname) != 0) {
        vm->stack_top = result_slot;
        nova_vm_push_value(vm, nova_value_nil());
        nova_vm_push_string(vm, "syntax error in loaded string", 29);
        nova_parser_free(&parser);
        nova_pp_destroy(pp);
        return 2;
    }

    /* === STAGE 3: Compiler === */
    NovaProto *proto = nova_compile(&parser.table, chunkname);
    if (proto == NULL) {
        vm->stack_top = result_slot;
        nova_vm_push_value(vm, nova_value_nil());
        nova_vm_push_string(vm, "compilation error in loaded string", 34);
        nova_parser_free(&parser);
        nova_pp_destroy(pp);
        return 2;
    }

    /* === STAGE 4: Optimize === */
    nova_optimize(proto, 1);

    /* === STAGE 5: Wrap in closure === */
    NovaClosure *cl = nova_closure_new(vm, proto);
    if (cl == NULL) {
        nova_proto_destroy(proto);
        vm->stack_top = result_slot;
        nova_vm_push_value(vm, nova_value_nil());
        nova_vm_push_string(vm, "out of memory creating closure", 30);
        nova_parser_free(&parser);
        nova_pp_destroy(pp);
        return 2;
    }

    /* Clean up compiler temporaries (proto is now owned by closure) */
    nova_parser_free(&parser);
    nova_pp_destroy(pp);

    /* Push the callable closure as the result */
    vm->stack_top = result_slot;
    nova_vm_push_value(vm, nova_value_closure(cl));
    return 1;
}

/* ============================================================
 * PART 14: REGISTRATION
 * ============================================================ */

static const NovaLibReg nova_base_lib[] = {
    {"print",          nova_base_print},
    {"echo",           nova_base_print},  /* alias for print */
    {"printf",         nova_base_printf},
    {"sprintf",        nova_base_sprintf},
    {"fprintf",        nova_base_fprintf},
    {"type",           nova_base_type},
    {"tostring",       nova_base_tostring},
    {"tonumber",       nova_base_tonumber},
    {"tointeger",      nova_base_tointeger},
    {"error",          nova_base_error},
    {"assert",         nova_base_assert},
    {"pcall",          nova_base_pcall},
    {"xpcall",         nova_base_xpcall},
    {"select",         nova_base_select},
    {"rawget",         nova_base_rawget},
    {"rawset",         nova_base_rawset},
    {"rawlen",         nova_base_rawlen},
    {"rawequal",       nova_base_rawequal},
    {"setmetatable",   nova_base_setmetatable},
    {"getmetatable",   nova_base_getmetatable},
    {"ipairs",         nova_base_ipairs},
    {"pairs",          nova_base_pairs},
    {"next",           nova_base_next},
    {"unpack",         nova_base_unpack},
    {"collectgarbage", nova_base_collectgarbage},
    {"load",           nova_base_load},
    {NULL,             NULL}  /* Sentinel */
};

/* ============================================================
 * LIBRARY REGISTRATION IMPLEMENTATION
 * ============================================================ */

void nova_lib_register(NovaVM *vm, const NovaLibReg *lib) {
    if (vm == NULL || lib == NULL) {
        return;
    }
    for (const NovaLibReg *entry = lib; entry->name != NULL; entry++) {
        nova_vm_set_global(vm, entry->name,
                           nova_value_cfunction(entry->func));
    }
}

void nova_lib_register_module(NovaVM *vm, const char *name,
                              const NovaLibReg *lib) {
    if (vm == NULL || name == NULL || lib == NULL) {
        return;
    }

    /* Create module table and push it */
    nova_vm_push_table(vm);

    /* Get the table value we just pushed (at top of stack) */
    NovaValue tval = nova_vm_get(vm, -1);

    if (nova_is_table(tval)) {
        NovaTable *t = nova_as_table(tval);
        for (const NovaLibReg *entry = lib; entry->name != NULL; entry++) {
            NovaString *key = NULL;
            /* We need novai_string_new but it's static in nova_vm.c.
             * Use nova_vm_set_field which creates the string internally. */
            nova_vm_push_cfunction(vm, entry->func);
            nova_vm_set_field(vm, -2, entry->name);
            (void)key;
            (void)t;
        }
    }

    /* Pop the table and set it as a global */
    vm->stack_top--;
    nova_vm_set_global(vm, name, tval);
}

/* ============================================================
 * MODULE OPENER
 * ============================================================ */

int nova_open_base(NovaVM *vm) {
    if (vm == NULL) {
        return -1;
    }

    /* Register base functions as globals */
    nova_lib_register(vm, nova_base_lib);

    /* Register _VERSION as a global string */
    nova_vm_push_string(vm, NOVA_VERSION_STRING,
                       strlen(NOVA_VERSION_STRING));
    NovaValue ver = nova_vm_get(vm, -1);
    vm->stack_top--;
    nova_vm_set_global(vm, "_VERSION", ver);

    return 0;
}

/* -----------------------------------------------------------------------
 * Part 4: Master Library Opener
 * ----------------------------------------------------------------------- */

/**
 * @brief Open all standard libraries
 *
 * Calls each module opener in order: base, math, string, table, io, os.
 * Base is first because other modules may depend on globals it registers.
 *
 * @param vm  Virtual machine instance (must not be NULL)
 * @return 0 on success, -1 on failure
 */
int nova_open_libs(NovaVM *vm) {
    if (vm == NULL) {
        return -1;
    }

    /* Base library first — registers global functions */
    if (nova_open_base(vm) != 0) {
        return -1;
    }

    /* Module libraries — each creates a global table */
    if (nova_open_math(vm) != 0) {
        return -1;
    }
    if (nova_open_string(vm) != 0) {
        return -1;
    }
    if (nova_open_table(vm) != 0) {
        return -1;
    }
    if (nova_open_io(vm) != 0) {
        return -1;
    }
    if (nova_open_os(vm) != 0) {
        return -1;
    }
    if (nova_open_package(vm) != 0) {
        return -1;
    }
    if (nova_open_coroutine(vm) != 0) {
        return -1;
    }
    if (nova_open_async(vm) != 0) {
        return -1;
    }
    if (nova_open_debug(vm) != 0) {
        return -1;
    }
    if (nova_open_fs(vm) != 0) {
        return -1;
    }
    if (nova_open_nlp(vm) != 0) {
        return -1;
    }
    if (nova_open_tools(vm) != 0) {
        return -1;
    }
    /* Data modules (json, csv, ini, toml, html) are NOT auto-loaded.
     * They are activated on-demand via #import directives.
     * See nova_open_data_imports() in nova_lib_data.c.              */

    return 0;
}
