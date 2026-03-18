/**
 * @file nova_meta.h
 * @brief Centralized metamethod pipeline for the Nova VM
 *
 * @author Anthony Taliento
 * @date 2026-02-08
 * @version 1.0.0
 *
 * @copyright Copyright (c) 2026 Zorya Corporation
 * @license MIT
 *
 * DESCRIPTION:
 *   Provides a single, clean entry point for ALL metamethod dispatch
 *   in the Nova VM.  No metamethod logic lives in the VM dispatch
 *   loop itself -- every opcode that needs meta-awareness calls into
 *   this module instead.
 *
 *   Architecture:
 *
 *     VM opcode  --->  nova_meta_xxx()  --->  raw op  OR  metamethod call
 *                          |
 *                     resolution
 *                     invocation
 *                     recursion guard
 *
 *   Supported metamethod categories:
 *
 *     INDEX       __index       table/userdata read fallback
 *     NEWINDEX    __newindex    table/userdata write intercept
 *     CALL        __call        callable tables/userdata
 *     ARITH       __add __sub __mul __div __mod __pow __unm __idiv
 *     BITWISE     __band __bor __bxor __bnot __shl __shr
 *     COMPARE     __eq __lt __le
 *     CONCAT      __concat
 *     LENGTH      __len
 *     TOSTRING    __tostring
 *     GC          __gc          destructor (future, for GC)
 *     METATABLE   __metatable   protection from getmetatable
 *
 * DEPENDENCIES:
 *   - nova_vm.h (NovaVM, NovaValue, NovaTable)
 *
 * THREAD SAFETY:
 *   Not thread-safe.  Each VM instance is single-threaded.
 */

#ifndef NOVA_META_H
#define NOVA_META_H

#include "nova_vm.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 * CONSTANTS
 * ============================================================ */

/** Maximum metamethod chain depth (prevents infinite __index loops) */
#define NOVA_META_MAX_DEPTH  16

/** Metamethod tag identifiers (fast enum for internal dispatch) */
typedef enum {
    NOVA_TM_INDEX = 0,
    NOVA_TM_NEWINDEX,
    NOVA_TM_CALL,
    NOVA_TM_ADD,
    NOVA_TM_SUB,
    NOVA_TM_MUL,
    NOVA_TM_DIV,
    NOVA_TM_MOD,
    NOVA_TM_POW,
    NOVA_TM_UNM,
    NOVA_TM_IDIV,
    NOVA_TM_BAND,
    NOVA_TM_BOR,
    NOVA_TM_BXOR,
    NOVA_TM_BNOT,
    NOVA_TM_SHL,
    NOVA_TM_SHR,
    NOVA_TM_EQ,
    NOVA_TM_LT,
    NOVA_TM_LE,
    NOVA_TM_CONCAT,
    NOVA_TM_LEN,
    NOVA_TM_TOSTRING,
    NOVA_TM_GC,
    NOVA_TM_METATABLE,
    NOVA_TM__COUNT         /* sentinel -- total number of tags */
} NovaMetaTag;

/* ============================================================
 * RESOLUTION  (lowest level)
 * ============================================================ */

/**
 * @brief Get the metatable for any NovaValue.
 *
 * Returns the metatable pointer if the value is a table or
 * userdata with a metatable set, otherwise returns NULL.
 *
 * @param v  The value to inspect
 * @return   Metatable pointer, or NULL
 */
NovaTable *nova_meta_get_mt(NovaValue v);

/**
 * @brief Set the shared string metatable.
 *
 * When set, all string values will appear to have this metatable,
 * enabling method-call syntax: s:find(), s:upper(), s:sub(), etc.
 * The metatable should have __index pointing to the string module.
 *
 * @param vm  VM instance (for GC rooting)
 * @param mt  The string metatable (NULL to clear)
 */
void nova_meta_set_string_mt(NovaVM *vm, NovaTable *mt);

/**
 * @brief Look up a metamethod by tag in a metatable.
 *
 * Searches the given metatable for the string key corresponding
 * to the tag (e.g., NOVA_TM_INDEX -> "__index").
 *
 * @param mt   The metatable to search (may be NULL)
 * @param tag  Which metamethod to look for
 * @return     The metamethod value, or nil if not found
 */
NovaValue nova_meta_get_method(NovaTable *mt, NovaMetaTag tag);

/**
 * @brief Convenience: get a metamethod for a value in one step.
 *
 * Equivalent to nova_meta_get_method(nova_meta_get_mt(v), tag).
 *
 * @param v    The value whose metatable to search
 * @param tag  Which metamethod to look for
 * @return     The metamethod value, or nil if not found
 */
NovaValue nova_meta_get_tm(NovaValue v, NovaMetaTag tag);

/* ============================================================
 * INVOCATION  (calls a metamethod with args, returns result)
 * ============================================================ */

/**
 * @brief Call a metamethod with 1 argument and 1 result.
 *
 * Used for:  __len, __unm, __bnot, __tostring
 *
 * @param vm       VM instance
 * @param method   The metamethod value (closure or cfunc)
 * @param arg1     First argument
 * @param result   [out] Result value
 * @return         0 on success, -1 on error (VM status set)
 */
int nova_meta_call1(NovaVM *vm, NovaValue method,
                    NovaValue arg1, NovaValue *result);

/**
 * @brief Call a metamethod with 2 arguments and 1 result.
 *
 * Used for:  __add, __sub, __mul, __div, __mod, __pow, __idiv,
 *            __band, __bor, __bxor, __shl, __shr,
 *            __eq, __lt, __le, __concat
 *
 * @param vm       VM instance
 * @param method   The metamethod value (closure or cfunc)
 * @param arg1     First argument  (left operand or table)
 * @param arg2     Second argument (right operand or key)
 * @param result   [out] Result value
 * @return         0 on success, -1 on error (VM status set)
 */
int nova_meta_call2(NovaVM *vm, NovaValue method,
                    NovaValue arg1, NovaValue arg2,
                    NovaValue *result);

/**
 * @brief Call a metamethod with 3 arguments and 0 results.
 *
 * Used for:  __newindex(table, key, value)
 *
 * @param vm       VM instance
 * @param method   The metamethod value (closure or cfunc)
 * @param arg1     First argument  (table)
 * @param arg2     Second argument (key)
 * @param arg3     Third argument  (value)
 * @return         0 on success, -1 on error (VM status set)
 */
int nova_meta_call3(NovaVM *vm, NovaValue method,
                    NovaValue arg1, NovaValue arg2, NovaValue arg3);

/* ============================================================
 * HIGH-LEVEL PIPELINE  (what VM opcodes call)
 * ============================================================
 *
 * These functions encapsulate the entire
 * "try raw op -> check metamethod -> invoke" pipeline.
 * The VM dispatch loop calls ONLY these functions.
 */

/**
 * @brief Meta-aware table GET.
 *
 * Pipeline:
 *   1. If obj is a table, try raw get
 *   2. If raw result is nil and __index exists:
 *      a. If __index is a table, recurse into it (up to MAX_DEPTH)
 *      b. If __index is a function, call __index(obj, key)
 *   3. If obj is not a table but has __index, use it
 *   4. Otherwise, error
 *
 * @param vm       VM instance
 * @param obj      The object being indexed (R(B) in GETTABLE)
 * @param key      The key (RK(C) in GETTABLE)
 * @param result   [out] The result value
 * @return         0 on success, -1 on error
 */
int nova_meta_index(NovaVM *vm, NovaValue obj, NovaValue key,
                    NovaValue *result);

/**
 * @brief Meta-aware table SET.
 *
 * Pipeline:
 *   1. If obj is a table:
 *      a. If key exists in table, raw set
 *      b. If __newindex exists, call __newindex(obj, key, val)
 *      c. Otherwise, raw set (new key)
 *   2. If obj is not a table but has __newindex, call it
 *   3. Otherwise, error
 *
 * @param vm       VM instance
 * @param obj      The table being written (R(A) in SETTABLE)
 * @param key      The key
 * @param val      The value to set
 * @return         0 on success, -1 on error
 */
int nova_meta_newindex(NovaVM *vm, NovaValue obj, NovaValue key,
                       NovaValue val);

/**
 * @brief Meta-aware binary arithmetic.
 *
 * Pipeline:
 *   1. Try raw numeric operation
 *   2. If type mismatch, look for __add (etc.) on left operand
 *   3. If not found, look for __add (etc.) on right operand
 *   4. Call the metamethod if found
 *   5. Error if no metamethod
 *
 * @param vm       VM instance
 * @param op       Opcode (NOVA_OP_ADD, etc.) for selecting metamethod
 * @param left     Left operand
 * @param right    Right operand
 * @param result   [out] Result value
 * @return         0 on success, -1 on error
 */
int nova_meta_arith(NovaVM *vm, NovaOpcode op,
                    NovaValue left, NovaValue right,
                    NovaValue *result);

/**
 * @brief Meta-aware unary operation.
 *
 * Pipeline:
 *   1. Try raw operation (UNM for numbers, LEN for strings/tables)
 *   2. If type mismatch, look for __unm / __len / __bnot
 *   3. Call metamethod if found
 *   4. Error if no metamethod
 *
 * @param vm       VM instance
 * @param tag      NOVA_TM_UNM, NOVA_TM_LEN, or NOVA_TM_BNOT
 * @param operand  The operand
 * @param result   [out] Result value
 * @return         0 on success, -1 on error
 */
int nova_meta_unary(NovaVM *vm, NovaMetaTag tag,
                    NovaValue operand, NovaValue *result);

/**
 * @brief Meta-aware comparison.
 *
 * Pipeline:
 *   1. If both values are numbers or both are strings, raw compare
 *   2. Otherwise, look for __eq / __lt / __le
 *   3. Call metamethod if found (returns boolean)
 *   4. For __eq: raw pointer equality as last resort
 *
 * @param vm       VM instance
 * @param tag      NOVA_TM_EQ, NOVA_TM_LT, or NOVA_TM_LE
 * @param left     Left operand
 * @param right    Right operand
 * @param result   [out] 1 = true, 0 = false
 * @return         0 on success, -1 on error
 */
int nova_meta_compare(NovaVM *vm, NovaMetaTag tag,
                      NovaValue left, NovaValue right,
                      int *result);

/**
 * @brief Meta-aware concatenation.
 *
 * Pipeline:
 *   1. If both are strings (or number-coercible), raw concat
 *   2. Otherwise, look for __concat on left then right
 *   3. Call metamethod if found
 *
 * @param vm       VM instance
 * @param left     Left operand
 * @param right    Right operand
 * @param result   [out] Concatenated value
 * @return         0 on success, -1 on error
 */
int nova_meta_concat(NovaVM *vm, NovaValue left, NovaValue right,
                     NovaValue *result);

/**
 * @brief Meta-aware call.
 *
 * Pipeline:
 *   1. If value is a function/closure/cfunc, return it as-is
 *   2. If value has __call metamethod, return that
 *   3. Error "attempt to call non-function"
 *
 * This is called by the CALL handler when the value isn't callable.
 * It returns the callable to invoke (the __call method), and the
 * caller prepends the original table as arg1.
 *
 * @param vm       VM instance
 * @param obj      The non-function being called
 * @param callable [out] The __call metamethod to invoke
 * @return         0 on success, -1 on error
 */
int nova_meta_call(NovaVM *vm, NovaValue obj, NovaValue *callable);

/**
 * @brief Meta-aware tostring.
 *
 * Pipeline:
 *   1. If value has __tostring, call it
 *   2. Otherwise, default string conversion
 *
 * @param vm       VM instance
 * @param v        The value to convert
 * @param result   [out] String result
 * @return         0 on success, -1 on error
 */
int nova_meta_tostring(NovaVM *vm, NovaValue v, NovaValue *result);

/* ============================================================
 * INITIALIZATION
 * ============================================================ */

/**
 * @brief Initialize the metamethod name cache.
 *
 * Pre-interns all metamethod name strings ("__index", "__add", etc.)
 * so that lookups are O(1) pointer comparisons instead of strcmp.
 * Called once during VM initialization.
 *
 * @param vm  VM instance
 */
void nova_meta_init(NovaVM *vm);

#ifdef __cplusplus
}
#endif

#endif /* NOVA_META_H */
