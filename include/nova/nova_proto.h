/**
 * @file nova_proto.h
 * @brief Nova Language - Function Prototype and Compilation State
 *
 * NovaProto is the central data structure of the compiler pipeline.
 * The compiler builds it, the optimizer transforms it, the codegen
 * serializes it, and the VM executes from it. Every compiled function
 * (including the top-level chunk) becomes a NovaProto.
 *
 * A NovaProto contains:
 *   - Instruction array (the actual bytecode)
 *   - Constant pool (numbers, strings, booleans, nil)
 *   - Sub-prototype array (nested functions / closures)
 *   - Upvalue descriptors (how closures capture variables)
 *   - Local variable info (for debug / error reporting)
 *   - Line number mapping (instruction → source line)
 *
 * Memory: All dynamic arrays within a proto are allocated via
 * standard malloc/realloc (not the arena). The arena is for AST
 * nodes only; protos outlive the parse phase and are owned by the
 * VM until GC collects them.
 *
 * @author Anthony Taliento
 * @date 2026-02-06
 * @version 0.2.0
 *
 * @copyright Copyright (c) 2026 Zorya Corporation
 * @license MIT
 *
 * ZORYA-C COMPLIANCE: v2.0.0
 */

#ifndef NOVA_PROTO_H
#define NOVA_PROTO_H

#include "nova_conf.h"
#include "nova_opcode.h"

#include <stdint.h>
#include <stddef.h>

/* ============================================================
 * CONSTANT VALUE (tagged union)
 *
 * Each entry in the constant pool is one of these.
 * The tag byte matches the .no binary format encoding.
 * ============================================================ */

/** Constant type tags (stable values for .no format) */
typedef enum {
    NOVA_CONST_NIL     = 0,
    NOVA_CONST_BOOL    = 1,
    NOVA_CONST_INTEGER = 2,
    NOVA_CONST_NUMBER  = 3,
    NOVA_CONST_STRING  = 4
} NovaConstTag;

/** A compile-time constant value */
typedef struct {
    NovaConstTag tag;
    union {
        int           boolean;   /* NOVA_CONST_BOOL    */
        nova_int_t    integer;   /* NOVA_CONST_INTEGER  */
        nova_number_t number;    /* NOVA_CONST_NUMBER   */
        struct {
            const char *data;    /* NOT owned -- points into source or interned pool */
            uint32_t    length;
        } string;                /* NOVA_CONST_STRING   */
    } as;
} NovaConstant;

/* ============================================================
 * UPVALUE DESCRIPTOR
 *
 * Describes how a closure captures one variable from an
 * enclosing scope. Used at compile time to emit GETUPVAL /
 * SETUPVAL and at runtime to build the upvalue chain.
 * ============================================================ */

typedef struct {
    uint8_t  index;     /* Register index (local) or upvalue index (parent) */
    uint8_t  in_stack;  /* 1 = captured from immediate parent's locals      */
                        /* 0 = captured from parent's upvalue list          */
    uint8_t  kind;      /* 0 = normal, 1 = const (for future optimization)  */
    uint8_t  pad_;      /* Padding for alignment                            */
    const char *name;   /* Variable name (debug only, may be NULL)          */
} NovaUpvalDesc;

/* ============================================================
 * LOCAL VARIABLE DESCRIPTOR (debug info)
 *
 * Maps a register to a variable name and the instruction range
 * where it is live. Used for error messages and debugger.
 * ============================================================ */

typedef struct {
    const char *name;       /* Variable name                          */
    uint32_t    start_pc;   /* First instruction where var is active  */
    uint32_t    end_pc;     /* Last instruction where var is active   */
    uint8_t     reg;        /* Register holding this variable         */
    uint8_t     pad_[3];    /* Padding for alignment                  */
} NovaLocalInfo;

/* ============================================================
 * LINE INFO
 *
 * Maps instruction indices to source line numbers. Uses a
 * compact delta encoding: most consecutive instructions share
 * the same line, so we store (delta_pc, delta_line) pairs.
 *
 * For simplicity, we also keep a flat line_numbers[] array
 * that maps instruction index → line number directly. The
 * delta encoding is used only in the .no serialization.
 * ============================================================ */

typedef struct {
    uint32_t *line_numbers;     /* line_numbers[pc] = source line    */
    uint32_t  count;            /* Number of entries (== code_count)  */
} NovaLineInfo;

/* ============================================================
 * FUNCTION PROTOTYPE
 *
 * The compiled representation of a single function.
 * Top-level chunks are also protos (the "main" function).
 * ============================================================ */

#ifndef NOVA_PROTO_TYPEDEF
#define NOVA_PROTO_TYPEDEF
typedef struct NovaProto NovaProto;
#endif

struct NovaProto {
    /* == Bytecode ============================================ */
    NovaInstruction *code;          /* Instruction array              */
    uint32_t         code_count;    /* Number of instructions         */
    uint32_t         code_capacity; /* Allocated slots                */

    /* == Constants ============================================ */
    NovaConstant    *constants;     /* Constant pool                  */
    uint32_t         const_count;   /* Number of constants            */
    uint32_t         const_capacity;/* Allocated slots                */

    /* == Sub-prototypes (nested functions) ==================== */
    struct NovaProto **protos;      /* Child proto array              */
    uint32_t          proto_count;  /* Number of child prototypes     */
    uint32_t          proto_capacity;

    /* == Upvalues ============================================= */
    NovaUpvalDesc   *upvalues;      /* Upvalue descriptors            */
    uint8_t          upvalue_count; /* Number of upvalues (max 255)   */

    /* == Debug info =========================================== */
    NovaLocalInfo   *locals;        /* Local variable descriptors     */
    uint32_t         local_count;   /* Number of local descriptors    */
    uint32_t         local_capacity;/* Allocated local descriptor slots */
    NovaLineInfo     lines;         /* Instruction → line mapping     */
    const char      *source;        /* Source file name (not owned)    */

    /* == Function metadata ==================================== */
    uint8_t          num_params;    /* Number of fixed parameters     */
    uint8_t          is_vararg;     /* 1 if function uses ...         */
    uint8_t          is_async;      /* 1 if async function            */
    uint8_t          max_stack;     /* Maximum register used + 1      */

    /* == Linkage ============================================== */
    uint32_t         line_defined;  /* Source line where func starts  */
    uint32_t         last_line;     /* Source line where func ends    */

};

/* ============================================================
 * PROTO LIFECYCLE API
 * ============================================================ */

/**
 * @brief Create a new empty function prototype.
 *
 * All arrays start at NULL with zero capacity. They grow on
 * first use via nova_proto_emit / nova_proto_add_constant / etc.
 *
 * @return Newly allocated proto, or NULL on allocation failure.
 *
 * COMPLEXITY: O(1)
 * THREAD SAFETY: Not thread-safe
 */
NovaProto *nova_proto_create(void);

/**
 * @brief Recursively destroy a prototype and all sub-prototypes.
 *
 * Frees the instruction array, constant pool (but NOT string
 * data -- strings are interned and outlive protos), upvalue
 * descriptors, local info, line info, sub-protos, and the
 * proto struct itself.
 *
 * @param proto  Proto to destroy (NULL is safe, no-op).
 *
 * COMPLEXITY: O(n) where n = total nodes in proto tree
 * THREAD SAFETY: Not thread-safe
 */
void nova_proto_destroy(NovaProto *proto);

/* ============================================================
 * INSTRUCTION EMISSION
 * ============================================================ */

/**
 * @brief Append one instruction to the proto's code array.
 *
 * Grows the array geometrically (2x) when full.
 *
 * @param proto  Target proto (must not be NULL)
 * @param instr  The instruction to append
 * @param line   Source line number for this instruction
 *
 * @return Index of the emitted instruction (for back-patching),
 *         or UINT32_MAX on allocation failure.
 *
 * @pre proto != NULL
 *
 * COMPLEXITY: Amortized O(1)
 * THREAD SAFETY: Not thread-safe
 */
uint32_t nova_proto_emit(NovaProto *proto, NovaInstruction instr,
                         uint32_t line);

/**
 * @brief Emit an ABC-format instruction.
 *
 * Convenience wrapper around nova_proto_emit.
 *
 * @param proto  Target proto
 * @param op     Opcode
 * @param a      A field (register)
 * @param b      B field
 * @param c      C field
 * @param line   Source line number
 *
 * @return Instruction index, or UINT32_MAX on failure.
 */
uint32_t nova_proto_emit_abc(NovaProto *proto, NovaOpcode op,
                             uint8_t a, uint8_t b, uint8_t c,
                             uint32_t line);

/**
 * @brief Emit an ABx-format instruction.
 *
 * @param proto  Target proto
 * @param op     Opcode
 * @param a      A field (register)
 * @param bx     Bx field (16-bit unsigned)
 * @param line   Source line number
 *
 * @return Instruction index, or UINT32_MAX on failure.
 */
uint32_t nova_proto_emit_abx(NovaProto *proto, NovaOpcode op,
                             uint8_t a, uint16_t bx,
                             uint32_t line);

/**
 * @brief Emit an AsBx-format instruction (signed offset).
 *
 * @param proto  Target proto
 * @param op     Opcode
 * @param a      A field (register)
 * @param sbx    Signed offset
 * @param line   Source line number
 *
 * @return Instruction index, or UINT32_MAX on failure.
 */
uint32_t nova_proto_emit_asbx(NovaProto *proto, NovaOpcode op,
                              uint8_t a, int sbx,
                              uint32_t line);

/* ============================================================
 * BACK-PATCHING
 *
 * The compiler emits forward jumps before knowing the target.
 * It records the instruction index, then patches the offset
 * once the target is known.
 * ============================================================ */

/**
 * @brief Patch the sBx field of an instruction (for jumps).
 *
 * @param proto  Target proto
 * @param pc     Instruction index to patch
 * @param sbx    New signed offset
 *
 * @pre pc < proto->code_count
 */
void nova_proto_patch_sbx(NovaProto *proto, uint32_t pc, int sbx);

/**
 * @brief Patch the A field of an instruction.
 *
 * Used for patching CLOSE instructions and register assignments.
 *
 * @param proto  Target proto
 * @param pc     Instruction index to patch
 * @param a      New A field value
 *
 * @pre pc < proto->code_count
 */
void nova_proto_patch_a(NovaProto *proto, uint32_t pc, uint8_t a);

/**
 * @brief Get current instruction count (next emission index).
 *
 * @param proto  Target proto
 * @return Number of instructions emitted so far
 */
static inline uint32_t nova_proto_pc(const NovaProto *proto) {
    return proto->code_count;
}

/* ============================================================
 * CONSTANT POOL
 * ============================================================ */

/**
 * @brief Add a nil constant. Returns existing index if duplicate.
 *
 * @param proto  Target proto
 * @return Constant pool index, or UINT32_MAX on failure.
 */
uint32_t nova_proto_add_nil(NovaProto *proto);

/**
 * @brief Add a boolean constant. Returns existing index if duplicate.
 *
 * @param proto  Target proto
 * @param value  Boolean value (0 or 1)
 * @return Constant pool index, or UINT32_MAX on failure.
 */
uint32_t nova_proto_add_bool(NovaProto *proto, int value);

/**
 * @brief Add an integer constant. Returns existing index if duplicate.
 *
 * @param proto  Target proto
 * @param value  Integer value
 * @return Constant pool index, or UINT32_MAX on failure.
 */
uint32_t nova_proto_add_integer(NovaProto *proto, nova_int_t value);

/**
 * @brief Add a number (float) constant. Returns existing index if duplicate.
 *
 * @param proto  Target proto
 * @param value  Number value
 * @return Constant pool index, or UINT32_MAX on failure.
 */
uint32_t nova_proto_add_number(NovaProto *proto, nova_number_t value);

/**
 * @brief Add a string constant. Returns existing index if duplicate.
 *
 * The string data is NOT copied -- the pointer must remain valid
 * for the lifetime of the proto (typically interned).
 *
 * @param proto   Target proto
 * @param str     String data (not necessarily null-terminated)
 * @param length  String length in bytes
 * @return Constant pool index, or UINT32_MAX on failure.
 */
uint32_t nova_proto_add_string(NovaProto *proto, const char *str,
                               uint32_t length);

/**
 * @brief Look up or add a string constant (deduplicating).
 *
 * Searches the existing constant pool for a matching string
 * before adding. This is the primary entry point for the
 * compiler's string constant handling.
 *
 * @param proto   Target proto
 * @param str     String data
 * @param length  String length in bytes
 * @return Constant pool index, or UINT32_MAX on failure.
 */
uint32_t nova_proto_find_or_add_string(NovaProto *proto, const char *str,
                                       uint32_t length);

/* ============================================================
 * SUB-PROTOTYPES
 * ============================================================ */

/**
 * @brief Add a child prototype (for nested function/closure).
 *
 * The child proto is owned by the parent -- nova_proto_destroy
 * on the parent will recursively destroy all children.
 *
 * @param proto  Parent proto
 * @param child  Child proto to add (ownership transferred)
 * @return Index of the child proto, or UINT32_MAX on failure.
 */
uint32_t nova_proto_add_child(NovaProto *proto, NovaProto *child);

/* ============================================================
 * UPVALUE MANAGEMENT
 * ============================================================ */

/**
 * @brief Add an upvalue descriptor.
 *
 * @param proto     Target proto
 * @param index     Register or upvalue index in parent
 * @param in_stack  1 if direct local, 0 if parent upvalue
 * @param name      Variable name (for debug, may be NULL)
 *
 * @return Upvalue index (0-based), or 255 on failure/overflow.
 */
uint8_t nova_proto_add_upvalue(NovaProto *proto, uint8_t index,
                               uint8_t in_stack, const char *name);

/* ============================================================
 * LOCAL VARIABLE INFO (debug)
 * ============================================================ */

/**
 * @brief Register a local variable for debug info.
 *
 * @param proto     Target proto
 * @param name      Variable name
 * @param reg       Register assigned to this local
 * @param start_pc  First instruction where var is active
 *
 * @return Local index, or UINT32_MAX on failure.
 */
uint32_t nova_proto_add_local(NovaProto *proto, const char *name,
                              uint8_t reg, uint32_t start_pc);

/**
 * @brief Close a local variable's live range.
 *
 * Sets end_pc for the most recent local with the given register.
 *
 * @param proto   Target proto
 * @param reg     Register to close
 * @param end_pc  Last instruction where var is active
 */
void nova_proto_close_local(NovaProto *proto, uint8_t reg,
                            uint32_t end_pc);

/* ============================================================
 * DISASSEMBLY (debug utility)
 * ============================================================ */

/**
 * @brief Print a human-readable disassembly of a prototype.
 *
 * Outputs to stderr: function header, instruction listing
 * with opcode names, operands, constant values, and line numbers.
 * Recurses into sub-prototypes.
 *
 * @param proto  Proto to disassemble
 * @param indent Indentation level (0 for top-level)
 */
void nova_proto_dump(const NovaProto *proto, int indent);

#endif /* NOVA_PROTO_H */
