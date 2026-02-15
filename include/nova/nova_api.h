/**
 * @file nova_api.h
 * @brief Nova Language - Internal API Helpers
 *
 * Internal functions used by the C API implementation (nova_api.c).
 * These are not part of the public API -- embedders should use nova.h.
 *
 * @author Anthony Taliento
 * @date 2026-02-05
 * @version 0.1.0
 *
 * @copyright Copyright (c) 2026 Zorya Corporation
 * @license MIT
 *
 * ZORYA-C COMPLIANCE: v2.0.0
 *
 * DEPENDENCIES:
 *   - nova.h (public types)
 *   - nova_vm.h (NovaState internals)
 *   - nova_object.h (value representation)
 */

#ifndef NOVA_API_H
#define NOVA_API_H

#include "nova.h"
#include "nova_vm.h"
#include "nova_object.h"

/* ============================================================
 * STACK INDEX CONVERSION
 *
 * Nova uses two indexing schemes:
 *   Positive: 1, 2, 3, ... from stack base (bottom)
 *   Negative: -1, -2, -3, ... from stack top
 *
 * These helpers convert API indices to internal pointers.
 * ============================================================ */

/**
 * @brief Convert API stack index to NovaValue pointer
 *
 * @param N   Nova state
 * @param idx Stack index (positive = from base, negative = from top)
 * @return Pointer to value, or NULL if index invalid
 *
 * HOT PATH: Called on every API value access
 */
static inline NovaValue *novai_index2val(NovaState *N, int idx) {
    NovaCallFrame *frame = &N->frames[N->frame_count - 1];
    if (idx > 0) {
        NovaValue *val = frame->base + idx - 1;
        return (val < N->stack_top) ? val : NULL;
    } else if (idx < 0) {
        NovaValue *val = N->stack_top + idx;
        return (val >= frame->base) ? val : NULL;
    }
    return NULL; /* idx == 0 is invalid */
}

/**
 * @brief Ensure there's room to push n values
 *
 * @param N Nova state
 * @param n Number of values to push
 */
static inline void novai_ensure_stack(NovaState *N, int n) {
    if (N->stack_top + n > N->stack_end) {
        novai_stack_grow(N, (size_t)n);
    }
}

/**
 * @brief Push a value onto the stack
 *
 * @param N     Nova state
 * @param val   Value to push
 */
static inline void novai_push(NovaState *N, NovaValue val) {
    *N->stack_top = val;
    N->stack_top++;
}

/* ============================================================
 * STRING OPERATIONS (INTERNAL)
 * ============================================================ */

/**
 * @brief Create or find an interned string
 *
 * Short strings are always interned. Long strings are cached
 * and interned lazily.
 *
 * @param N     Nova state
 * @param str   String data (not null-terminated required)
 * @param len   String length
 * @return Interned NovaString, or NULL on allocation failure
 */
NovaString *novai_string_new(NovaState *N, const char *str, size_t len);

/**
 * @brief Create a formatted string (printf-style)
 *
 * @param N   Nova state
 * @param fmt Format string
 * @param ... Format arguments
 * @return NovaString, or NULL on failure
 */
NovaString *novai_string_fmt(NovaState *N, const char *fmt, ...);

/**
 * @brief Concatenate two strings
 *
 * @param N Nova state
 * @param a First string
 * @param b Second string
 * @return Concatenated string
 */
NovaString *novai_string_concat(NovaState *N, const NovaString *a,
                                const NovaString *b);

/* ============================================================
 * TABLE OPERATIONS (INTERNAL)
 * ============================================================ */

/**
 * @brief Create a new table
 *
 * @param N     Nova state
 * @param narr  Array part hint
 * @param nhash Hash part hint
 * @return New table, or NULL on allocation failure
 */
NovaTable *novai_table_new(NovaState *N, int narr, int nhash);

/**
 * @brief Get a value from a table by key
 *
 * @param N     Nova state
 * @param table Table to look up in
 * @param key   Key value
 * @return Pointer to value (may be nil), or NULL if not found
 */
NovaValue *novai_table_get(NovaState *N, NovaTable *table, NovaValue key);

/**
 * @brief Set a value in a table
 *
 * @param N     Nova state
 * @param table Table to modify
 * @param key   Key value
 * @param value Value to store
 */
void novai_table_set(NovaState *N, NovaTable *table, NovaValue key,
                     NovaValue value);

/**
 * @brief Get a value by string key (optimized path)
 *
 * @param N     Nova state
 * @param table Table
 * @param key   String key
 * @return Pointer to value, or NULL
 */
NovaValue *novai_table_get_str(NovaState *N, NovaTable *table,
                               const NovaString *key);

/**
 * @brief Set a value by string key (optimized path)
 */
void novai_table_set_str(NovaState *N, NovaTable *table,
                         const NovaString *key, NovaValue value);

/* ============================================================
 * GC OPERATIONS (INTERNAL)
 * ============================================================ */

/**
 * @brief Allocate a new GC-managed object
 *
 * @param N     Nova state
 * @param size  Object size in bytes (including GC header)
 * @param type  Object type tag
 * @return New object, or NULL on failure (triggers GC first)
 */
NovaGCObject *novai_gc_alloc(NovaState *N, size_t size, uint8_t type);

/**
 * @brief Run a GC step (incremental)
 *
 * @param N Nova state
 */
void novai_gc_step(NovaState *N);

/**
 * @brief Run a full GC collection cycle
 *
 * @param N Nova state
 */
void novai_gc_collect(NovaState *N);

/**
 * @brief Write barrier: mark object as needing re-scan
 *
 * Must be called after modifying a black object to point to a white object.
 *
 * @param N     Nova state
 * @param obj   The modified (container) object
 * @param child The new child reference
 */
void novai_gc_barrier(NovaState *N, NovaGCObject *obj, NovaGCObject *child);

/* ============================================================
 * ERROR HANDLING (INTERNAL)
 * ============================================================ */

/**
 * @brief Raise a runtime error (longjmp)
 *
 * @param N   Nova state
 * @param fmt Format string
 * @param ... Arguments
 */
void novai_error(NovaState *N, const char *fmt, ...);

/**
 * @brief Type error helper
 *
 * @param N        Nova state
 * @param idx      Stack index of offending value
 * @param expected Expected type name
 */
void novai_type_error(NovaState *N, int idx, const char *expected);

#endif /* NOVA_API_H */
