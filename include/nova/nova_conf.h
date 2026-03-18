/**
 * @file nova_conf.h
 * @brief Nova Language - Build Configuration and Tuning Parameters
 *
 * All compile-time configuration knobs for Nova live here.
 * Override any value by defining it before including this header,
 * or pass -DNOVA_OPTION=value to the compiler.
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

#ifndef NOVA_CONF_H
#define NOVA_CONF_H

#include <stdint.h>

/* ============================================================
 * VERSION
 * ============================================================ */

#define NOVA_VERSION_MAJOR  0
#define NOVA_VERSION_MINOR  2
#define NOVA_VERSION_PATCH  0
#define NOVA_VERSION_STRING "0.2.0"

#define NOVA_VERSION_NUMBER \
    (NOVA_VERSION_MAJOR * 10000 + NOVA_VERSION_MINOR * 100 + NOVA_VERSION_PATCH)

#define NOVA_COPYRIGHT "Copyright (c) 2026 Zorya Corporation"
#define NOVA_AUTHORS   "Anthony Taliento"

/* ============================================================
 * NUMERIC TYPES
 * ============================================================ */

/**
 * Nova's number type. Default is double (IEEE 754).
 * Override to float for embedded systems with limited FPU.
 */
#ifndef NOVA_NUMBER_TYPE
    #define NOVA_NUMBER_TYPE double
#endif

/**
 * Nova's integer type. 64-bit by default.
 * Override to int32_t for 32-bit embedded targets.
 */
#ifndef NOVA_INTEGER_TYPE
    #define NOVA_INTEGER_TYPE int64_t
#endif

/**
 * Unsigned integer type matching NOVA_INTEGER_TYPE.
 */
#ifndef NOVA_UNSIGNED_TYPE
    #define NOVA_UNSIGNED_TYPE uint64_t
#endif

/** Concrete typedefs derived from the macros above */
typedef NOVA_NUMBER_TYPE   nova_number_t;
typedef NOVA_INTEGER_TYPE  nova_int_t;
typedef NOVA_UNSIGNED_TYPE nova_uint_t;

/* ============================================================
 * VM LIMITS
 * ============================================================ */

/** Maximum number of registers per call frame (RK encoding reserves top 128 for constants) */
#ifndef NOVA_MAX_REGISTERS
    #define NOVA_MAX_REGISTERS 125
#endif

/** Maximum call stack depth (prevents infinite recursion) */
#ifndef NOVA_MAX_CALL_DEPTH
    #define NOVA_MAX_CALL_DEPTH 200
#endif

/** Maximum number of upvalues per closure */
#ifndef NOVA_MAX_UPVALUES
    #define NOVA_MAX_UPVALUES 64
#endif

/** Maximum number of constants per function prototype */
#ifndef NOVA_MAX_CONSTANTS
    #define NOVA_MAX_CONSTANTS 65536
#endif

/** Maximum number of local variables per function */
#ifndef NOVA_MAX_LOCALS
    #define NOVA_MAX_LOCALS 120
#endif

/** Initial stack size (registers) */
#ifndef NOVA_INITIAL_STACK_SIZE
    #define NOVA_INITIAL_STACK_SIZE 1024
#endif

/** Maximum stack size */
#ifndef NOVA_MAX_STACK_SIZE
    #define NOVA_MAX_STACK_SIZE 1000000
#endif

/* ============================================================
 * STRING LIMITS
 * ============================================================ */

/** Short string threshold - strings <= this are always interned */
#ifndef NOVA_SHORT_STRING_LIMIT
    #define NOVA_SHORT_STRING_LIMIT 40
#endif

/** Maximum string length (256 MB default) */
#ifndef NOVA_MAX_STRING_LENGTH
    #define NOVA_MAX_STRING_LENGTH (256 * 1024 * 1024)
#endif

/* ============================================================
 * TABLE LIMITS
 * ============================================================ */

/** Initial array part size for tables */
#ifndef NOVA_TABLE_INITIAL_ARRAY
    #define NOVA_TABLE_INITIAL_ARRAY 4
#endif

/** Initial hash part capacity for tables */
#ifndef NOVA_TABLE_INITIAL_HASH
    #define NOVA_TABLE_INITIAL_HASH 4
#endif

/* ============================================================
 * GARBAGE COLLECTOR
 * ============================================================ */

/** GC pause: percentage of heap growth before next cycle (200 = 2x) */
#ifndef NOVA_GC_PAUSE
    #define NOVA_GC_PAUSE 200
#endif

/** GC step multiplier: speed of collection relative to allocation */
#ifndef NOVA_GC_STEP_MULTIPLIER
    #define NOVA_GC_STEP_MULTIPLIER 400
#endif

/** Minor collection ratio for generational GC */
#ifndef NOVA_GC_MINOR_RATIO
    #define NOVA_GC_MINOR_RATIO 20
#endif

/** Major collection ratio for generational GC */
#ifndef NOVA_GC_MAJOR_RATIO
    #define NOVA_GC_MAJOR_RATIO 100
#endif

/** Minimum bytes to allocate before first GC cycle */
#ifndef NOVA_GC_MIN_THRESHOLD
    #define NOVA_GC_MIN_THRESHOLD (64 * 1024)
#endif

/* ============================================================
 * PREPROCESSOR
 * ============================================================ */

/** Maximum #include nesting depth */
#ifndef NOVA_PP_MAX_INCLUDE_DEPTH
    #define NOVA_PP_MAX_INCLUDE_DEPTH 32
#endif

/** Maximum macro expansion depth (prevents recursive macros) */
#ifndef NOVA_PP_MAX_MACRO_DEPTH
    #define NOVA_PP_MAX_MACRO_DEPTH 64
#endif

/** Maximum number of macro parameters */
#ifndef NOVA_PP_MAX_MACRO_PARAMS
    #define NOVA_PP_MAX_MACRO_PARAMS 32
#endif

/** Maximum conditional nesting depth (#if/#ifdef) */
#ifndef NOVA_PP_MAX_IF_DEPTH
    #define NOVA_PP_MAX_IF_DEPTH 64
#endif

/* ============================================================
 * BYTECODE (.no) FORMAT
 * ============================================================ */

/** Magic number: "NOVA" in ASCII */
#define NOVA_BYTECODE_MAGIC     0x4E4F5641

/** Current bytecode format version */
#define NOVA_BYTECODE_VERSION   ((NOVA_VERSION_MAJOR << 8) | NOVA_VERSION_MINOR)

/** EOF marker */
#define NOVA_BYTECODE_EOF       0xDEAD0A00

/* ============================================================
 * PARSER
 * ============================================================ */

/** Maximum expression nesting depth */
#ifndef NOVA_MAX_EXPR_DEPTH
    #define NOVA_MAX_EXPR_DEPTH 200
#endif

/** Maximum number of function parameters */
#ifndef NOVA_MAX_PARAMS
    #define NOVA_MAX_PARAMS 255
#endif

/** Maximum number of return values */
#ifndef NOVA_MAX_RETURNS
    #define NOVA_MAX_RETURNS 255
#endif

/* ============================================================
 * ARRAY INDEXING
 * ============================================================ */

/**
 * Base index for arrays. Default is 0 (unlike Lua's 1).
 * Set to 1 for Lua compatibility mode.
 */
#ifndef NOVA_ARRAY_BASE
    #define NOVA_ARRAY_BASE 0
#endif

/* ============================================================
 * PLATFORM DETECTION
 *
 * Delegates to ZORYA PAL for the canonical detection macros.
 * Nova-specific aliases are provided for backward compatibility.
 * ============================================================ */

#include <zorya/pal_detect.h>

/* Map ZORYA_OS_* → NOVA_PLATFORM_* for existing Nova code */
#if ZORYA_OS_LINUX
    #define NOVA_PLATFORM_LINUX 1
#elif ZORYA_OS_MACOS
    #define NOVA_PLATFORM_MACOS 1
#elif ZORYA_OS_WINDOWS
    #define NOVA_PLATFORM_WINDOWS 1
#elif ZORYA_OS_FREEBSD
    #define NOVA_PLATFORM_FREEBSD 1
#else
    #define NOVA_PLATFORM_UNKNOWN 1
#endif

/* Map ZORYA_ARCH_* → NOVA_ARCH_* for existing Nova code */
#if ZORYA_ARCH_X64
    #define NOVA_ARCH_X86_64 1
#elif ZORYA_ARCH_ARM64
    #define NOVA_ARCH_ARM64 1
#elif ZORYA_ARCH_X86
    #define NOVA_ARCH_X86 1
#elif ZORYA_ARCH_ARM32
    #define NOVA_ARCH_ARM32 1
#else
    #define NOVA_ARCH_UNKNOWN 1
#endif

/* ============================================================
 * FEATURE FLAGS
 * ============================================================ */

/** Enable NaN-boxing for compact value representation */
#ifndef NOVA_NAN_BOXING
    #if defined(NOVA_ARCH_X86_64) || defined(NOVA_ARCH_ARM64)
        #define NOVA_NAN_BOXING 1
    #else
        #define NOVA_NAN_BOXING 0
    #endif
#endif

/** Enable computed goto dispatch — delegates to PAL detection */
#ifndef NOVA_COMPUTED_GOTO
    #define NOVA_COMPUTED_GOTO ZORYA_HAS_COMPUTED_GOTO
#endif

/** Enable string interpolation with backtick syntax */
#ifndef NOVA_STRING_INTERPOLATION
    #define NOVA_STRING_INTERPOLATION 1
#endif

/** Enable preprocessor support */
#ifndef NOVA_PREPROCESSOR
    #define NOVA_PREPROCESSOR 1
#endif

/** Enable debug hooks and facilities */
#ifndef NOVA_DEBUG_HOOKS
    #ifdef NOVA_DEBUG
        #define NOVA_DEBUG_HOOKS 1
    #else
        #define NOVA_DEBUG_HOOKS 0
    #endif
#endif

#endif /* NOVA_CONF_H */
