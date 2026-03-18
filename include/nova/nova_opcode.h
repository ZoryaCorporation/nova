/**
 * @file nova_opcode.h
 * @brief Nova Language - Opcode Definitions and Instruction Encoding
 *
 * This header is the bytecode contract. Every component that touches
 * instructions -- compiler, optimizer, codegen, VM, disassembler,
 * debugger, error system -- includes this file and agrees on exactly
 * what each opcode number means.
 *
 * Opcodes are a typedef enum with explicit numeric assignments.
 * These values are STABLE: once assigned, an opcode number never
 * changes meaning. New opcodes are appended, never inserted.
 * This is critical for:
 *   - .no binary format compatibility across versions
 *   - ECC (error correction) on bytecode streams
 *   - Deterministic computed-goto dispatch tables
 *   - Machine-readable error diagnostics (NOVA-E03xx)
 *
 * Instruction format: 32-bit fixed-width, little-endian
 *
 *   31       24 23       16 15        8 7         0
 *   +----------+----------+----------+----------+
 *   | Opcode   |    A     |    B     |    C     |  ABC
 *   +----------+----------+----------+----------+
 *   | Opcode   |    A     |       Bx (16-bit)   |  ABx
 *   +----------+----------+----------+----------+
 *   | Opcode   |    A     |       sBx (signed)  |  AsBx
 *   +----------+----------+----------+----------+
 *   | Opcode   |          Ax (24-bit)           |  Ax
 *   +----------+----------+----------+----------+
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

#ifndef NOVA_OPCODE_H
#define NOVA_OPCODE_H

#include <stdint.h>

/* ============================================================
 * INSTRUCTION TYPE
 * ============================================================ */

/** A single Nova bytecode instruction (32-bit) */
typedef uint32_t NovaInstruction;

/* ============================================================
 * FIELD SIZES AND POSITIONS
 * ============================================================ */

#define NOVA_OPCODE_BITS    8
#define NOVA_FIELD_A_BITS   8
#define NOVA_FIELD_B_BITS   8
#define NOVA_FIELD_C_BITS   8
#define NOVA_FIELD_BX_BITS  16   /* B + C combined */
#define NOVA_FIELD_AX_BITS  24   /* A + B + C combined */

#define NOVA_OPCODE_SHIFT   24
#define NOVA_FIELD_A_SHIFT  16
#define NOVA_FIELD_B_SHIFT  8
#define NOVA_FIELD_C_SHIFT  0
#define NOVA_FIELD_BX_SHIFT 0
#define NOVA_FIELD_AX_SHIFT 0

#define NOVA_OPCODE_MASK    0xFFu
#define NOVA_FIELD_A_MASK   0xFFu
#define NOVA_FIELD_B_MASK   0xFFu
#define NOVA_FIELD_C_MASK   0xFFu
#define NOVA_FIELD_BX_MASK  0xFFFFu
#define NOVA_FIELD_AX_MASK  0xFFFFFFu

/** Maximum unsigned value for Bx field */
#define NOVA_MAX_BX         0xFFFF

/** Bias for signed sBx: sBx = Bx - NOVA_SBX_BIAS */
#define NOVA_SBX_BIAS       0x7FFF

/** Maximum positive sBx value */
#define NOVA_MAX_SBX        ((int)NOVA_SBX_BIAS)

/** Minimum negative sBx value */
#define NOVA_MIN_SBX        (-(int)NOVA_SBX_BIAS)

/** Maximum unsigned value for Ax field */
#define NOVA_MAX_AX         0xFFFFFF

/* ============================================================
 * INSTRUCTION DECODING MACROS
 * ============================================================ */

/**
 * PCM: NOVA_GET_OPCODE
 * Purpose: Extract opcode byte from instruction word
 * Rationale: Called on every VM dispatch cycle (hottest path)
 * Performance Impact: Must be single shift+mask, no branches
 * Audit Date: 2026-02-06
 */
#define NOVA_GET_OPCODE(i) \
    ((NovaOpcode)(((i) >> NOVA_OPCODE_SHIFT) & NOVA_OPCODE_MASK))

/**
 * PCM: NOVA_GET_A
 * Purpose: Extract A field (register index) from instruction
 * Rationale: Used in ~95% of instruction handlers
 * Performance Impact: Single shift+mask
 * Audit Date: 2026-02-06
 */
#define NOVA_GET_A(i) \
    ((uint8_t)(((i) >> NOVA_FIELD_A_SHIFT) & NOVA_FIELD_A_MASK))

/**
 * PCM: NOVA_GET_B
 * Purpose: Extract B field from ABC-format instruction
 * Rationale: Used in arithmetic, table, comparison ops
 * Performance Impact: Single shift+mask
 * Audit Date: 2026-02-06
 */
#define NOVA_GET_B(i) \
    ((uint8_t)(((i) >> NOVA_FIELD_B_SHIFT) & NOVA_FIELD_B_MASK))

/**
 * PCM: NOVA_GET_C
 * Purpose: Extract C field from ABC-format instruction
 * Rationale: Used in arithmetic, table, comparison ops
 * Performance Impact: Single shift+mask
 * Audit Date: 2026-02-06
 */
#define NOVA_GET_C(i) \
    ((uint8_t)(((i) >> NOVA_FIELD_C_SHIFT) & NOVA_FIELD_C_MASK))

/**
 * PCM: NOVA_GET_BX
 * Purpose: Extract unsigned Bx field (16-bit) from ABx-format
 * Rationale: Used for constant pool indices (LOADK, GETGLOBAL)
 * Performance Impact: Single mask, no shift
 * Audit Date: 2026-02-06
 */
#define NOVA_GET_BX(i) \
    ((uint16_t)(((i) >> NOVA_FIELD_BX_SHIFT) & NOVA_FIELD_BX_MASK))

/**
 * PCM: NOVA_GET_SBX
 * Purpose: Extract signed sBx field from AsBx-format
 * Rationale: Used for jump offsets (JMP, FORLOOP, FORPREP)
 * Performance Impact: Single mask + subtract bias
 * Audit Date: 2026-02-06
 */
#define NOVA_GET_SBX(i) \
    ((int)(NOVA_GET_BX(i)) - NOVA_SBX_BIAS)

/**
 * PCM: NOVA_GET_AX
 * Purpose: Extract unsigned Ax field (24-bit) from Ax-format
 * Rationale: Used for extra-arg instructions
 * Performance Impact: Single mask, no shift
 * Audit Date: 2026-02-06
 */
#define NOVA_GET_AX(i) \
    ((uint32_t)(((i) >> NOVA_FIELD_AX_SHIFT) & NOVA_FIELD_AX_MASK))

/* ============================================================
 * INSTRUCTION ENCODING MACROS
 * ============================================================ */

/**
 * PCM: NOVA_ENCODE_ABC
 * Purpose: Build an ABC-format instruction from parts
 * Rationale: Used by compiler for all 3-operand instructions
 * Performance Impact: 3 shifts + 3 ORs, no branches
 * Audit Date: 2026-02-06
 */
#define NOVA_ENCODE_ABC(op, a, b, c) \
    ((NovaInstruction)( \
        (((uint32_t)(op) & NOVA_OPCODE_MASK) << NOVA_OPCODE_SHIFT) | \
        (((uint32_t)(a)  & NOVA_FIELD_A_MASK) << NOVA_FIELD_A_SHIFT) | \
        (((uint32_t)(b)  & NOVA_FIELD_B_MASK) << NOVA_FIELD_B_SHIFT) | \
        (((uint32_t)(c)  & NOVA_FIELD_C_MASK) << NOVA_FIELD_C_SHIFT) \
    ))

/**
 * PCM: NOVA_ENCODE_ABX
 * Purpose: Build an ABx-format instruction from parts
 * Rationale: Used by compiler for constant-load and global-access ops
 * Performance Impact: 2 shifts + 2 ORs, no branches
 * Audit Date: 2026-02-06
 */
#define NOVA_ENCODE_ABX(op, a, bx) \
    ((NovaInstruction)( \
        (((uint32_t)(op) & NOVA_OPCODE_MASK)  << NOVA_OPCODE_SHIFT) | \
        (((uint32_t)(a)  & NOVA_FIELD_A_MASK)  << NOVA_FIELD_A_SHIFT) | \
        (((uint32_t)(bx) & NOVA_FIELD_BX_MASK) << NOVA_FIELD_BX_SHIFT) \
    ))

/**
 * PCM: NOVA_ENCODE_ASBX
 * Purpose: Build an AsBx-format instruction (signed offset)
 * Rationale: Used by compiler for jumps and loop ops
 * Performance Impact: 2 shifts + 2 ORs + 1 add (bias), no branches
 * Audit Date: 2026-02-06
 */
#define NOVA_ENCODE_ASBX(op, a, sbx) \
    NOVA_ENCODE_ABX((op), (a), (uint16_t)((sbx) + NOVA_SBX_BIAS))

/**
 * PCM: NOVA_ENCODE_AX
 * Purpose: Build an Ax-format instruction (24-bit payload)
 * Rationale: Used for extra-arg instructions
 * Performance Impact: 1 shift + 1 OR, no branches
 * Audit Date: 2026-02-06
 */
#define NOVA_ENCODE_AX(op, ax) \
    ((NovaInstruction)( \
        (((uint32_t)(op) & NOVA_OPCODE_MASK)  << NOVA_OPCODE_SHIFT) | \
        (((uint32_t)(ax) & NOVA_FIELD_AX_MASK) << NOVA_FIELD_AX_SHIFT) \
    ))

/**
 * PCM: NOVA_ENCODE_SJ
 * Purpose: Build a signed-jump instruction (sJ format)
 * Rationale: JMP opcode uses 16-bit signed offset in B+C fields
 * Performance Impact: 2 shifts + 3 ORs + 1 add (bias), no branches
 * Audit Date: 2026-02-07
 *
 * The sJ format stores a signed offset in B[7:0] and C[7:0] combined,
 * biased by NOVA_SJ_BIAS/2 so the raw value is unsigned.
 */
#define NOVA_SJ_BIAS 131071
#define NOVA_ENCODE_SJ(op, sj) \
    ((NovaInstruction)( \
        (((uint32_t)(op) & NOVA_OPCODE_MASK) << NOVA_OPCODE_SHIFT) | \
        (((uint32_t)((sj) + (NOVA_SJ_BIAS >> 1)) >> 8) << NOVA_FIELD_B_SHIFT) | \
        (((uint32_t)((sj) + (NOVA_SJ_BIAS >> 1)) & 0xFF) << NOVA_FIELD_C_SHIFT) \
    ))

/* ============================================================
 * INSTRUCTION PATCHING MACROS
 *
 * Used by the compiler to back-patch jump targets and by
 * the optimizer to rewrite operands in-place.
 * ============================================================ */

/** Replace the opcode byte of an instruction */
#define NOVA_SET_OPCODE(i, op) \
    ((i) = ((i) & ~((uint32_t)NOVA_OPCODE_MASK << NOVA_OPCODE_SHIFT)) | \
           (((uint32_t)(op) & NOVA_OPCODE_MASK) << NOVA_OPCODE_SHIFT))

/** Replace the A field of an instruction */
#define NOVA_SET_A(i, a) \
    ((i) = ((i) & ~((uint32_t)NOVA_FIELD_A_MASK << NOVA_FIELD_A_SHIFT)) | \
           (((uint32_t)(a) & NOVA_FIELD_A_MASK) << NOVA_FIELD_A_SHIFT))

/** Replace the B field of an instruction */
#define NOVA_SET_B(i, b) \
    ((i) = ((i) & ~((uint32_t)NOVA_FIELD_B_MASK << NOVA_FIELD_B_SHIFT)) | \
           (((uint32_t)(b) & NOVA_FIELD_B_MASK) << NOVA_FIELD_B_SHIFT))

/** Replace the C field of an instruction */
#define NOVA_SET_C(i, c) \
    ((i) = ((i) & ~((uint32_t)NOVA_FIELD_C_MASK << NOVA_FIELD_C_SHIFT)) | \
           (((uint32_t)(c) & NOVA_FIELD_C_MASK) << NOVA_FIELD_C_SHIFT))

/** Replace the Bx field of an instruction */
#define NOVA_SET_BX(i, bx) \
    ((i) = ((i) & ~((uint32_t)NOVA_FIELD_BX_MASK << NOVA_FIELD_BX_SHIFT)) | \
           (((uint32_t)(bx) & NOVA_FIELD_BX_MASK) << NOVA_FIELD_BX_SHIFT))

/** Replace the sBx field of an instruction (signed) */
#define NOVA_SET_SBX(i, sbx) \
    NOVA_SET_BX((i), (uint16_t)((sbx) + NOVA_SBX_BIAS))

/* ============================================================
 * OPCODE ENUMERATION
 *
 * STABLE CONTRACT: Numeric values are permanent.
 * Never reorder, never reuse a retired number.
 * New opcodes go at the end, before NOVA_OP_SENTINEL.
 *
 * The explicit numeric assignments serve as:
 *   1. Binary format compatibility (same .no runs on any version)
 *   2. ECC anchor points (error correction can validate opcodes)
 *   3. Computed-goto table indices (must be dense and stable)
 *   4. Machine-readable diagnostics (NOVA-E03xx can reference by number)
 * ============================================================ */

typedef enum {
    /* ---- Data Movement (0x00-0x0F) ---- */
    NOVA_OP_MOVE       = 0x00,  /* ABC    R(A) = R(B)                            */
    NOVA_OP_LOADK      = 0x01,  /* ABx   R(A) = K(Bx)                           */
    NOVA_OP_LOADNIL    = 0x02,  /* ABC    R(A), R(A+1), ..., R(A+B) = nil        */
    NOVA_OP_LOADBOOL   = 0x03,  /* ABC    R(A) = (bool)B; if C then pc++         */
    NOVA_OP_LOADINT    = 0x04,  /* AsBx  R(A) = sBx (small integer literal)     */
    NOVA_OP_LOADKX     = 0x05,  /* A      R(A) = K(extra); next instr is EXTRAARG */

    /* ---- Table Operations (0x10-0x1F) ---- */
    NOVA_OP_NEWTABLE   = 0x10,  /* ABC    R(A) = {} (B=array hint, C=hash hint)  */
    NOVA_OP_GETTABLE   = 0x11,  /* ABC    R(A) = R(B)[RK(C)]                    */
    NOVA_OP_SETTABLE   = 0x12,  /* ABC    R(A)[RK(B)] = RK(C)                   */
    NOVA_OP_GETFIELD   = 0x13,  /* ABC    R(A) = R(B)[K(C)]  (string key)       */
    NOVA_OP_SETFIELD   = 0x14,  /* ABC    R(A)[K(B)] = RK(C) (string key)       */
    NOVA_OP_GETI       = 0x15,  /* ABC    R(A) = R(B)[C]     (integer key)      */
    NOVA_OP_SETI       = 0x16,  /* ABC    R(A)[B] = RK(C)    (integer key)      */
    NOVA_OP_SETLIST    = 0x17,  /* ABC    R(A)[(C-1)*FPF+i] = R(A+i), 1<=i<=B   */
    NOVA_OP_SELF       = 0x18,  /* ABC    R(A+1)=R(B); R(A)=R(B)[RK(C)]  (method)*/

    /* ---- Upvalue / Global / Environment (0x20-0x2F) ---- */
    NOVA_OP_GETUPVAL   = 0x20,  /* ABC    R(A) = U(B)                            */
    NOVA_OP_SETUPVAL   = 0x21,  /* ABC    U(B) = R(A)                            */
    NOVA_OP_GETGLOBAL  = 0x22,  /* ABx   R(A) = G[K(Bx)]                        */
    NOVA_OP_SETGLOBAL  = 0x23,  /* ABx   G[K(Bx)] = R(A)                        */
    NOVA_OP_GETTABUP   = 0x24,  /* ABC    R(A) = U(B)[RK(C)]  (upvalue table)   */
    NOVA_OP_SETTABUP   = 0x25,  /* ABC    U(A)[RK(B)] = RK(C) (upvalue table)   */

    /* ---- Arithmetic (0x30-0x3F) ---- */
    NOVA_OP_ADD        = 0x30,  /* ABC    R(A) = R(B) + R(C)                     */
    NOVA_OP_SUB        = 0x31,  /* ABC    R(A) = R(B) - R(C)                     */
    NOVA_OP_MUL        = 0x32,  /* ABC    R(A) = R(B) * R(C)                     */
    NOVA_OP_DIV        = 0x33,  /* ABC    R(A) = R(B) / R(C)     (float)        */
    NOVA_OP_IDIV       = 0x34,  /* ABC    R(A) = R(B) // R(C)    (floor div)    */
    NOVA_OP_MOD        = 0x35,  /* ABC    R(A) = R(B) % R(C)                     */
    NOVA_OP_POW        = 0x36,  /* ABC    R(A) = R(B) ^ R(C)                     */
    NOVA_OP_UNM        = 0x37,  /* ABC    R(A) = -R(B)                           */

    /* ---- Integer Arithmetic (0x38-0x3B) -- immediate operand ---- */
    NOVA_OP_ADDI       = 0x38,  /* AsBx  R(A) = R(A) + sBx                      */
    NOVA_OP_ADDK       = 0x39,  /* ABC    R(A) = R(B) + K(C)   (constant)       */
    NOVA_OP_SUBK       = 0x3A,  /* ABC    R(A) = R(B) - K(C)                     */
    NOVA_OP_MULK       = 0x3B,  /* ABC    R(A) = R(B) * K(C)                     */
    NOVA_OP_DIVK       = 0x3C,  /* ABC    R(A) = R(B) / K(C)                     */
    NOVA_OP_MODK       = 0x3D,  /* ABC    R(A) = R(B) % K(C)                     */

    /* ---- Bitwise (0x40-0x4F) ---- */
    NOVA_OP_BAND       = 0x40,  /* ABC    R(A) = R(B) & R(C)                     */
    NOVA_OP_BOR        = 0x41,  /* ABC    R(A) = R(B) | R(C)                     */
    NOVA_OP_BXOR       = 0x42,  /* ABC    R(A) = R(B) ~ R(C)                     */
    NOVA_OP_BNOT       = 0x43,  /* ABC    R(A) = ~R(B)                           */
    NOVA_OP_SHL        = 0x44,  /* ABC    R(A) = R(B) << R(C)                    */
    NOVA_OP_SHR        = 0x45,  /* ABC    R(A) = R(B) >> R(C)                    */

    /* ---- String (0x50-0x57) ---- */
    NOVA_OP_CONCAT     = 0x50,  /* ABC    R(A) = R(A) .. R(A+1) .. ... .. R(A+B) */
    NOVA_OP_STRLEN     = 0x51,  /* ABC    R(A) = #R(B)                           */

    /* ---- Comparison (0x58-0x5F) -- skip next if test fails ---- */
    NOVA_OP_EQ         = 0x58,  /* ABC    if (R(B) == R(C)) ~= A then pc++       */
    NOVA_OP_LT         = 0x59,  /* ABC    if (R(B) <  R(C)) ~= A then pc++       */
    NOVA_OP_LE         = 0x5A,  /* ABC    if (R(B) <= R(C)) ~= A then pc++       */
    NOVA_OP_EQK        = 0x5B,  /* ABC    if (R(B) == K(C)) ~= A then pc++       */
    NOVA_OP_EQI        = 0x5C,  /* AsBx  if (R(A) == sBx)   then pc++           */
    NOVA_OP_LTI        = 0x5D,  /* AsBx  if (R(A) <  sBx)   then pc++           */
    NOVA_OP_LEI        = 0x5E,  /* AsBx  if (R(A) <= sBx)   then pc++           */
    NOVA_OP_GTI        = 0x5F,  /* AsBx  if (R(A) >  sBx)   then pc++           */
    NOVA_OP_GEI        = 0x60,  /* AsBx  if (R(A) >= sBx)   then pc++           */

    /* ---- Logical (0x68-0x6F) ---- */
    NOVA_OP_NOT        = 0x68,  /* ABC    R(A) = not R(B)                        */
    NOVA_OP_TEST       = 0x69,  /* ABC    if (bool)R(A) ~= C then pc++           */
    NOVA_OP_TESTSET    = 0x6A,  /* ABC    if (bool)R(B) ~= C then R(A)=R(B) else pc++ */

    /* ---- Control Flow (0x70-0x7F) ---- */
    NOVA_OP_JMP        = 0x70,  /* AsBx  pc += sBx; close upvalues >= A          */
    NOVA_OP_CALL       = 0x71,  /* ABC    R(A),...,R(A+C-2) = R(A)(R(A+1),...,R(A+B-1)) */
    NOVA_OP_TAILCALL   = 0x72,  /* ABC    return R(A)(R(A+1), ..., R(A+B-1))     */
    NOVA_OP_RETURN     = 0x73,  /* ABC    return R(A), ..., R(A+B-2)             */
    NOVA_OP_RETURN0    = 0x74,  /* --     return (no values)                     */
    NOVA_OP_RETURN1    = 0x75,  /* A      return R(A) (single value, common)     */

    /* ---- Loops (0x80-0x8F) ---- */
    NOVA_OP_FORPREP    = 0x80,  /* AsBx  R(A) -= R(A+2); pc += sBx              */
    NOVA_OP_FORLOOP    = 0x81,  /* AsBx  R(A)+=R(A+2); if R(A)<=R(A+1): pc+=sBx; R(A+3)=R(A) */
    NOVA_OP_TFORCALL   = 0x82,  /* ABC    R(A+4),...=R(A)(R(A+1),R(A+2)); nresults=C */
    NOVA_OP_TFORLOOP   = 0x83,  /* AsBx  if R(A+2)~=nil then R(A)=R(A+2) else pc+=sBx */

    /* ---- Closures and Varargs (0x90-0x97) ---- */
    NOVA_OP_CLOSURE    = 0x90,  /* ABx   R(A) = closure(proto[Bx])               */
    NOVA_OP_VARARG     = 0x91,  /* ABC    R(A), ..., R(A+C-2) = vararg           */
    NOVA_OP_VARARGPREP = 0x92,  /* A      adjust vararg parameters               */
    NOVA_OP_CLOSE      = 0x93,  /* A      close upvalues >= R(A)                 */

    /* ---- Async / Concurrency (0xA0-0xAF) ---- */
    NOVA_OP_AWAIT      = 0xA0,  /* ABC    R(A) = await R(B)                      */
    NOVA_OP_SPAWN      = 0xA1,  /* ABC    spawn R(A)(R(A+1),...,R(A+B-1))        */
    NOVA_OP_YIELD      = 0xA2,  /* ABC    yield R(A), ..., R(A+B-1); results in R(A)..R(A+C-1) */

    /* ---- Module (0xB0-0xB7) ---- */
    NOVA_OP_IMPORT     = 0xB0,  /* ABx   R(A) = import(K(Bx))                    */
    NOVA_OP_EXPORT     = 0xB1,  /* ABx   export K(Bx) = R(A)                     */

    /* ---- Special (0xF0-0xFF) ---- */
    NOVA_OP_NOP        = 0xF0,  /* --     no operation (padding/alignment)       */
    NOVA_OP_DEBUG      = 0xF1,  /* A      debug breakpoint hook, level A         */
    NOVA_OP_EXTRAARG   = 0xFE,  /* Ax     extra argument for previous opcode     */

    /* ---- Sentinel (not a real opcode) ---- */
    NOVA_OP_SENTINEL   = 0xFF   /* End-of-enum sentinel -- NEVER encode this     */

} NovaOpcode;

/* ============================================================
 * OPCODE METADATA
 *
 * Static tables indexed by opcode for fast property queries.
 * These are declared extern here and defined in nova_opcode.c.
 * ============================================================ */

/** Instruction format for each opcode */
typedef enum {
    NOVA_FMT_ABC  = 0,   /* Three 8-bit fields: A, B, C            */
    NOVA_FMT_ABX  = 1,   /* A (8-bit) + Bx (16-bit unsigned)       */
    NOVA_FMT_ASBX = 2,   /* A (8-bit) + sBx (16-bit signed)        */
    NOVA_FMT_AX   = 3,   /* Ax (24-bit unsigned, no A field)        */
    NOVA_FMT_NONE = 4    /* No operands (e.g. RETURN0, NOP)         */
} NovaInstructionFormat;

/** Operand mode: how B and C fields are interpreted */
typedef enum {
    NOVA_ARGMODE_UNUSED = 0,  /* Field not used                     */
    NOVA_ARGMODE_REG    = 1,  /* Register index                     */
    NOVA_ARGMODE_CONST  = 2,  /* Constant pool index                */
    NOVA_ARGMODE_RK     = 3,  /* Register or constant (bit 8 flag)  */
    NOVA_ARGMODE_INT    = 4,  /* Raw integer value                  */
    NOVA_ARGMODE_JUMP   = 5   /* Jump offset (signed)               */
} NovaArgMode;

/** Properties of a single opcode */
typedef struct {
    const char            *name;     /* Human-readable name ("MOVE")      */
    NovaInstructionFormat  format;   /* Instruction format                */
    NovaArgMode            arg_b;    /* How B field is used               */
    NovaArgMode            arg_c;    /* How C field is used               */
    uint8_t                testflag; /* 1 if this op is a test (sets pc)  */
    uint8_t                setsflag; /* 1 if this op sets register A      */
} NovaOpcodeInfo;

/**
 * @brief Opcode metadata table, indexed by opcode value.
 *
 * Defined in nova_opcode.c. Valid for all assigned opcodes.
 * Unassigned slots (gaps in the enum) have name == NULL.
 *
 * @example
 *   NovaOpcode op = NOVA_GET_OPCODE(instr);
 *   printf("Executing: %s\n", nova_opcode_info[op].name);
 */
extern const NovaOpcodeInfo nova_opcode_info[256];

/* ============================================================
 * RK ENCODING
 *
 * For opcodes that accept "register or constant" in B or C,
 * constants are flagged by setting the MSB (bit 7) of the field.
 * Since fields are 8 bits, this means:
 *   0-127  = register index
 *   128+   = constant index (subtract NOVA_RK_CONST_BASE)
 *
 * This allows 128 registers and 128 fast-path constants per
 * instruction, with LOADK handling larger constant indices.
 * ============================================================ */

#define NOVA_RK_CONST_BASE  128

/**
 * PCM: NOVA_IS_RK_CONST
 * Purpose: Test if an RK field refers to a constant
 * Rationale: Called in every RK-mode instruction handler
 * Performance Impact: Single compare, no branches in caller
 * Audit Date: 2026-02-06
 */
#define NOVA_IS_RK_CONST(rk)    ((rk) >= NOVA_RK_CONST_BASE)

/**
 * PCM: NOVA_RK_TO_CONST
 * Purpose: Convert RK field value to constant pool index
 * Rationale: Pairs with NOVA_IS_RK_CONST in hot path
 * Performance Impact: Single subtract
 * Audit Date: 2026-02-06
 */
#define NOVA_RK_TO_CONST(rk)    ((rk) - NOVA_RK_CONST_BASE)

/**
 * PCM: NOVA_CONST_TO_RK
 * Purpose: Encode a constant pool index into an RK field value
 * Rationale: Used by compiler when emitting RK-mode instructions
 * Performance Impact: Single add
 * Audit Date: 2026-02-06
 */
#define NOVA_CONST_TO_RK(k)     ((k) + NOVA_RK_CONST_BASE)

/** Maximum constant index encodable in an RK field */
#define NOVA_MAX_RK_CONST       (255 - NOVA_RK_CONST_BASE)

/* ============================================================
 * FIELDS PER FLUSH (SETLIST batching)
 * ============================================================ */

/** Number of array elements set per SETLIST instruction */
#ifndef NOVA_FIELDS_PER_FLUSH
    #define NOVA_FIELDS_PER_FLUSH 50
#endif

/* ============================================================
 * CONVENIENCE: OPCODE CATEGORY QUERIES
 * ============================================================ */

/** Test if opcode is an arithmetic binary op (for constant folding) */
static inline int nova_opcode_is_arith(NovaOpcode op) {
    return op >= NOVA_OP_ADD && op <= NOVA_OP_MODK;
}

/** Test if opcode is a bitwise binary op */
static inline int nova_opcode_is_bitwise(NovaOpcode op) {
    return op >= NOVA_OP_BAND && op <= NOVA_OP_SHR;
}

/** Test if opcode is a comparison (test + conditional skip) */
static inline int nova_opcode_is_compare(NovaOpcode op) {
    return op >= NOVA_OP_EQ && op <= NOVA_OP_GEI;
}

/** Test if opcode is a jump or conditional branch */
static inline int nova_opcode_is_jump(NovaOpcode op) {
    return op == NOVA_OP_JMP
        || op == NOVA_OP_FORLOOP
        || op == NOVA_OP_FORPREP
        || op == NOVA_OP_TFORLOOP;
}

/** Test if opcode terminates a basic block (for optimizer) */
static inline int nova_opcode_is_block_end(NovaOpcode op) {
    return op == NOVA_OP_RETURN
        || op == NOVA_OP_RETURN0
        || op == NOVA_OP_RETURN1
        || op == NOVA_OP_TAILCALL
        || op == NOVA_OP_JMP;
}

/** Test if opcode is a return variant */
static inline int nova_opcode_is_return(NovaOpcode op) {
    return op == NOVA_OP_RETURN
        || op == NOVA_OP_RETURN0
        || op == NOVA_OP_RETURN1
        || op == NOVA_OP_TAILCALL;
}

/** Test if opcode is an async/concurrency operation */
static inline int nova_opcode_is_async(NovaOpcode op) {
    return op >= NOVA_OP_AWAIT && op <= NOVA_OP_YIELD;
}

/** Test if opcode uses the RK encoding for field B */
static inline int nova_opcode_b_is_rk(NovaOpcode op) {
    return nova_opcode_info[(uint8_t)op].arg_b == NOVA_ARGMODE_RK;
}

/** Test if opcode uses the RK encoding for field C */
static inline int nova_opcode_c_is_rk(NovaOpcode op) {
    return nova_opcode_info[(uint8_t)op].arg_c == NOVA_ARGMODE_RK;
}

#endif /* NOVA_OPCODE_H */
