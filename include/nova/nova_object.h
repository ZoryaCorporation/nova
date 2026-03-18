/**
 * @file nova_object.h
 * @brief Nova Language - Object System and Value Representation
 *
 * Internal header defining Nova's value representation, object types,
 * and GC object header. Uses NaN-boxing on 64-bit platforms.
 *
 * @author Anthony Taliento
 * @date 2026-02-05
 * @version 0.2.0
 *
 * @copyright Copyright (c) 2026 Zorya Corporation
 * @license MIT
 *
 * ZORYA-C COMPLIANCE: v2.0.0
 *
 * DEPENDENCIES:
 *   - nova_conf.h (configuration)
 *   - zorya/nxh.h (string hashing)
 *   - zorya/pcm.h (performance macros)
 */

#ifndef NOVA_OBJECT_H
#define NOVA_OBJECT_H

#include "nova_conf.h"
#include "zorya/pcm.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* ============================================================
 * GC OBJECT HEADER
 *
 * Every heap-allocated Nova object begins with this header.
 * The GC uses it for tri-color marking and type identification.
 * ============================================================ */

/**
 * @brief GC object color (tri-color invariant)
 */
typedef enum {
    NOVA_GC_WHITE0 = 0,     /**< White (unreached), bit 0       */
    NOVA_GC_WHITE1 = 1,     /**< White (unreached), bit 1       */
    NOVA_GC_GRAY   = 2,     /**< Gray (reached, children unscanned) */
    NOVA_GC_BLACK  = 3      /**< Black (fully scanned)          */
} NovaGCColor;

/**
 * @brief Common header for all GC-managed objects
 *
 * Occupies 16 bytes on 64-bit platforms.
 * Linked list threading for GC traversal.
 */
typedef struct NovaGCObject {
    struct NovaGCObject *gc_next;   /**< Next object in GC list     */
    uint8_t              type;      /**< NovaType tag               */
    uint8_t              color;     /**< GC color (NovaGCColor)     */
    uint8_t              generation;/**< 0=young, 1=old             */
    uint8_t              flags;     /**< Object-specific flags      */
    uint32_t             _pad;      /**< Alignment padding          */
} NovaGCObject;

/* GC header macros */
#define NOVA_GC_HEADER  NovaGCObject gc_header

/**
 * @brief Extract GC header from any Nova object pointer
 */
#define nova_gc_obj(o)      (&(o)->gc_header)
#define nova_gc_type(o)     ((o)->gc_header.type)
#define nova_gc_color(o)    ((o)->gc_header.color)
#define nova_gc_next(o)     ((o)->gc_header.gc_next)

/* ============================================================
 * VALUE REPRESENTATION
 *
 * On 64-bit platforms with NOVA_NAN_BOXING=1:
 *   Uses NaN-boxing to pack all values into 64 bits.
 *
 * On other platforms (or NOVA_NAN_BOXING=0):
 *   Uses tagged union (type tag + 8-byte payload).
 * ============================================================ */

#if NOVA_NAN_BOXING

/* --------------------------------------------------------
 * NaN-Boxing Layout (64-bit)
 *
 * IEEE 754 double uses bits 62:52 = 0x7FF for NaN.
 * Quiet NaN has bit 51 set. We use the remaining 51 bits
 * plus the sign bit to encode non-double values.
 *
 * QNAN = 0x7FFC000000000000 (quiet NaN with our tag area)
 *
 * Encoding:
 *   Double:   Any value where bits 62:52 != 0x7FF (normal doubles)
 *             or a real NaN (we use a canonical NaN)
 *   Nil:      QNAN | TAG_NIL
 *   Bool:     QNAN | TAG_BOOL | (0 or 1)
 *   Integer:  SIGN_BIT | QNAN | (48-bit value)
 *   Pointer:  QNAN | TAG_POINTER | (48-bit address)
 * -------------------------------------------------------- */

/** Quiet NaN space we commandeer for type encoding */
#define NOVA_QNAN       ((uint64_t)0x7FFC000000000000ULL)

/** Sign bit (used for integer tagging) */
#define NOVA_SIGN_BIT   ((uint64_t)0x8000000000000000ULL)

/** Type tags within the NaN payload */
#define NOVA_TAG_NIL    ((uint64_t)0x0001000000000000ULL)
#define NOVA_TAG_BOOL   ((uint64_t)0x0002000000000000ULL)
#define NOVA_TAG_INT    (NOVA_SIGN_BIT)
#define NOVA_TAG_OBJ    ((uint64_t)0x0003000000000000ULL)

/** 48-bit pointer/payload mask */
#define NOVA_PAYLOAD_MASK ((uint64_t)0x0000FFFFFFFFFFFFULL)

/**
 * @brief Nova value type (NaN-boxed 64-bit)
 */
typedef uint64_t NovaValue;

/*
** PCM: NOVA_VALUE_NUMBER
** Purpose: Create a NovaValue from a double, zero-cost
** Rationale: Hot path - every number literal, every arithmetic result
** Performance Impact: Single memory reinterpret (union punning)
** Audit Date: 2026-02-05
*/
static inline NovaValue nova_value_number(nova_number_t n) {
    NovaValue v = 0;
    memcpy(&v, &n, sizeof(nova_number_t));
    return v;
}

static inline nova_number_t nova_as_number(NovaValue v) {
    nova_number_t n = 0.0;
    memcpy(&n, &v, sizeof(nova_number_t));
    return n;
}

/*
** PCM: NOVA_VALUE_NIL / NOVA_VALUE_TRUE / NOVA_VALUE_FALSE
** Purpose: Constant Nova values
** Rationale: Used in every nil/bool comparison and assignment
** Performance Impact: Compile-time constant
** Audit Date: 2026-02-05
*/
#define NOVA_VALUE_NIL      (NOVA_QNAN | NOVA_TAG_NIL)
#define NOVA_VALUE_TRUE     (NOVA_QNAN | NOVA_TAG_BOOL | 1)
#define NOVA_VALUE_FALSE    (NOVA_QNAN | NOVA_TAG_BOOL | 0)

static inline NovaValue nova_value_bool(int b) {
    return b ? NOVA_VALUE_TRUE : NOVA_VALUE_FALSE;
}

static inline NovaValue nova_value_int(nova_int_t i) {
    return NOVA_SIGN_BIT | NOVA_QNAN | ((uint64_t)i & NOVA_PAYLOAD_MASK);
}

static inline NovaValue nova_value_obj(NovaGCObject *obj) {
    return NOVA_QNAN | NOVA_TAG_OBJ | ((uint64_t)(uintptr_t)obj & NOVA_PAYLOAD_MASK);
}

/* Type checks */
#define nova_is_number_val(v)  (((v) & NOVA_QNAN) != NOVA_QNAN)
#define nova_is_nil_val(v)     ((v) == NOVA_VALUE_NIL)
#define nova_is_bool_val(v)    (((v) & (NOVA_QNAN | NOVA_TAG_BOOL)) == (NOVA_QNAN | NOVA_TAG_BOOL))
#define nova_is_int_val(v)     (((v) & (NOVA_SIGN_BIT | NOVA_QNAN)) == (NOVA_SIGN_BIT | NOVA_QNAN))
#define nova_is_obj_val(v)     (((v) & (NOVA_QNAN | NOVA_TAG_OBJ)) == (NOVA_QNAN | NOVA_TAG_OBJ) && \
                                !nova_is_nil_val(v) && !nova_is_bool_val(v))

/* Value extraction */
#define nova_as_bool(v)    ((int)((v) & 1))
#define nova_as_int(v)     ((nova_int_t)((v) & NOVA_PAYLOAD_MASK))
#define nova_as_obj(v)     ((NovaGCObject *)(uintptr_t)((v) & NOVA_PAYLOAD_MASK))

#else /* !NOVA_NAN_BOXING -- Tagged union representation */

/**
 * @brief Nova value type (tagged union, portable)
 */
typedef struct {
    uint8_t type;       /**< NovaType tag */
    union {
        int            boolean;
        nova_int_t     integer;
        nova_number_t  number;
        NovaGCObject  *object;
        void          *pointer;
    } as;
} NovaValue;

#define NOVA_VALUE_NIL      ((NovaValue){ .type = NOVA_TYPE_NIL, .as.integer = 0 })
#define NOVA_VALUE_TRUE     ((NovaValue){ .type = NOVA_TYPE_BOOL, .as.boolean = 1 })
#define NOVA_VALUE_FALSE    ((NovaValue){ .type = NOVA_TYPE_BOOL, .as.boolean = 0 })

static inline NovaValue nova_value_number(nova_number_t n) {
    NovaValue v = { .type = NOVA_TYPE_NUMBER, .as.number = n };
    return v;
}

static inline NovaValue nova_value_bool(int b) {
    NovaValue v = { .type = NOVA_TYPE_BOOL, .as.boolean = b ? 1 : 0 };
    return v;
}

static inline NovaValue nova_value_int(nova_int_t i) {
    NovaValue v = { .type = NOVA_TYPE_INTEGER, .as.integer = i };
    return v;
}

static inline NovaValue nova_value_obj(NovaGCObject *obj) {
    NovaValue v = { .type = obj->type, .as.object = obj };
    return v;
}

#define nova_is_number_val(v)  ((v).type == NOVA_TYPE_NUMBER)
#define nova_is_nil_val(v)     ((v).type == NOVA_TYPE_NIL)
#define nova_is_bool_val(v)    ((v).type == NOVA_TYPE_BOOL)
#define nova_is_int_val(v)     ((v).type == NOVA_TYPE_INTEGER)
#define nova_is_obj_val(v)     ((v).type >= NOVA_TYPE_STRING)

#define nova_as_number(v)  ((v).as.number)
#define nova_as_bool(v)    ((v).as.boolean)
#define nova_as_int(v)     ((v).as.integer)
#define nova_as_obj(v)     ((v).as.object)

#endif /* NOVA_NAN_BOXING */

/* ============================================================
 * CONCRETE OBJECT TYPES
 * ============================================================ */

/**
 * @brief Nova string object
 *
 * Strings are immutable once created. Short strings are interned
 * via Weave's Tablet for O(1) equality comparison.
 */
typedef struct NovaString {
    NOVA_GC_HEADER;
    uint64_t    hash;       /**< NXH64 hash (cached)            */
    uint32_t    len;        /**< String length (bytes)           */
    uint8_t     interned;   /**< 1 if string is interned         */
    uint8_t     _reserved[3];
    char        data[];     /**< Flexible array: null-terminated */
} NovaString;

/**
 * @brief Nova upvalue (closed-over variable)
 */
typedef struct NovaUpvalue {
    NOVA_GC_HEADER;
    NovaValue          *location;  /**< Points to stack or closed  */
    NovaValue           closed;    /**< Storage when closed        */
    struct NovaUpvalue  *next;     /**< Linked list of open upvals */
} NovaUpvalue;

/**
 * @brief Function prototype (compiled bytecode chunk)
 */
typedef struct NovaProto {
    NOVA_GC_HEADER;
    uint8_t         num_params;     /**< Number of fixed parameters */
    uint8_t         num_upvalues;   /**< Number of upvalues         */
    uint8_t         max_registers;  /**< Registers needed           */
    uint8_t         is_vararg;      /**< Has ... parameter?         */

    int             code_count;     /**< Number of instructions     */
    uint32_t       *code;           /**< Instruction array          */

    int             constant_count; /**< Number of constants        */
    NovaValue      *constants;      /**< Constant pool              */

    int             proto_count;    /**< Number of nested prototypes*/
    struct NovaProto **protos;      /**< Nested function prototypes */

    /* Debug information (strippable) */
    int            *lineinfo;       /**< Line number per instruction*/
    NovaString     *source;         /**< Source file name           */

    /* Upvalue descriptions */
    struct {
        uint8_t in_stack;   /**< 1 = in enclosing stack, 0 = in enclosing upvalue */
        uint8_t index;      /**< Index in stack or upvalue list */
    } *upvalue_desc;
} NovaProto;

/**
 * @brief Nova closure (function + upvalues)
 */
typedef struct NovaClosure {
    NOVA_GC_HEADER;
    uint8_t     is_c;           /**< 1 = C function, 0 = Nova     */
    uint8_t     num_upvalues;   /**< Number of upvalues            */
    uint16_t    _pad;
    union {
        struct {
            NovaProto    *proto;
            NovaUpvalue  *upvalues[1]; /**< Flexible: [num_upvalues] */
        } nova;
        struct {
            nova_cfunc_t  func;
            NovaValue     upvalues[1]; /**< Flexible: [num_upvalues] */
        } c;
    } u;
} NovaClosure;

/**
 * @brief Nova table (array + hash map)
 *
 * The array part stores integer-keyed values [0..array_size-1].
 * The hash part is backed by DAGGER for string/mixed keys.
 * This split matches Lua's proven hybrid approach.
 */
typedef struct NovaTable {
    NOVA_GC_HEADER;
    NovaValue      *array;          /**< Array part                  */
    uint32_t        array_size;     /**< Array part capacity         */
    uint32_t        array_count;    /**< Used array slots            */
    void           *hash;           /**< DaggerTable* for hash part  */
    struct NovaTable *metatable;    /**< Metatable (NULL if none)    */
} NovaTable;

/**
 * @brief Nova userdata (user-managed GC object)
 */
typedef struct NovaUserdata {
    NOVA_GC_HEADER;
    size_t              size;       /**< Data size in bytes          */
    struct NovaTable   *metatable;  /**< Metatable (NULL if none)    */
    uint8_t             data[];     /**< User data (flexible array)  */
} NovaUserdata;

/* ============================================================
 * OBJECT TYPE CASTING
 *
 * These macros safely cast a NovaGCObject* to a concrete type.
 * In debug builds, they assert the type tag matches.
 * ============================================================ */

#define nova_as_string(o)   ((NovaString *)(o))
#define nova_as_closure(o)  ((NovaClosure *)(o))
#define nova_as_table(o)    ((NovaTable *)(o))
#define nova_as_userdata(o) ((NovaUserdata *)(o))
#define nova_as_upvalue(o)  ((NovaUpvalue *)(o))
#define nova_as_proto(o)    ((NovaProto *)(o))

/* Type checks on GC objects */
#define nova_obj_is_string(o)   (nova_gc_type(o) == NOVA_TYPE_STRING)
#define nova_obj_is_table(o)    (nova_gc_type(o) == NOVA_TYPE_TABLE)
#define nova_obj_is_function(o) (nova_gc_type(o) == NOVA_TYPE_FUNCTION)

/* ============================================================
 * STRING UTILITIES
 * ============================================================ */

/**
 * @brief Get C string from NovaString
 * HOT PATH: Called constantly in VM dispatch
 */
static inline const char *nova_str_data(const NovaString *s) {
    return s != NULL ? s->data : "";
}

/**
 * @brief Get string length
 */
static inline uint32_t nova_str_len(const NovaString *s) {
    return s != NULL ? s->len : 0;
}

/**
 * @brief Get string hash
 */
static inline uint64_t nova_str_hash(const NovaString *s) {
    return s != NULL ? s->hash : 0;
}

/**
 * @brief Compare two NovaStrings for equality
 * Short interned strings: pointer comparison (O(1))
 * Long strings: hash comparison, then memcmp fallback
 */
static inline int nova_str_equal(const NovaString *a, const NovaString *b) {
    if (a == b) return 1;
    if (a == NULL || b == NULL) return 0;
    if (a->len != b->len) return 0;
    if (a->hash != b->hash) return 0;
    /* If both interned and hashes match but pointers differ, not equal */
    if (a->interned && b->interned) return 0;
    return memcmp(a->data, b->data, a->len) == 0;
}

#endif /* NOVA_OBJECT_H */
