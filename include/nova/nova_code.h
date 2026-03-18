/**
 * @file nova_code.h
 * @brief Nova Language - Bytecode Format and Opcodes
 *
 * Defines the instruction set, encoding formats, and the
 * portable .no binary object format for Nova bytecode.
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
 *   - nova_conf.h (bytecode magic, version)
 */

#ifndef NOVA_CODE_H
#define NOVA_CODE_H

#include "nova_conf.h"
#include <stdint.h>

/* ============================================================
 * INSTRUCTION ENCODING
 *
 * All instructions are 32 bits wide (fixed-width).
 *
 *  31       24 23       16 15        8 7         0
 *  +----------+----------+----------+----------+
 *  | Opcode(8)|   A(8)   |   B(8)   |   C(8)   |  iABC
 *  +----------+----------+----------+----------+
 *  | Opcode(8)|   A(8)   |      Bx(16)         |  iABx
 *  +----------+----------+----------+----------+
 *  | Opcode(8)|   A(8)   |      sBx(16 signed) |  iAsBx
 *  +----------+----------+----------+----------+
 *  | Opcode(8)|         Ax(24)                  |  iAx
 *  +----------+----------+----------+----------+
 *
 * ============================================================ */

/** Instruction type */
typedef uint32_t NovaInstruction;

/* Field positions and sizes */
#define NOVA_OP_POS     24
#define NOVA_OP_SIZE    8
#define NOVA_A_POS      16
#define NOVA_A_SIZE     8
#define NOVA_B_POS      8
#define NOVA_B_SIZE     8
#define NOVA_C_POS      0
#define NOVA_C_SIZE     8
#define NOVA_BX_POS     0
#define NOVA_BX_SIZE    16
#define NOVA_AX_POS     0
#define NOVA_AX_SIZE    24

/* Maximum field values */
#define NOVA_MAX_A      255
#define NOVA_MAX_B      255
#define NOVA_MAX_C      255
#define NOVA_MAX_BX     65535
#define NOVA_MAX_SBX    32767
#define NOVA_MIN_SBX    (-32768)
#define NOVA_MAX_AX     16777215

/* sBx bias (offset encoding for signed values) */
#define NOVA_SBX_BIAS   32768

/* ============================================================
 * INSTRUCTION FIELD ACCESS MACROS
 * ============================================================ */

/*
** PCM: NOVA_GET_OP / NOVA_GET_A / NOVA_GET_B / NOVA_GET_C
** Purpose: Extract instruction fields
** Rationale: Called on EVERY instruction dispatch in the VM hot loop
** Performance Impact: Single shift + mask per field
** Audit Date: 2026-02-05
*/
#define NOVA_GET_OP(i)  ((uint8_t)(((i) >> NOVA_OP_POS) & 0xFF))
#define NOVA_GET_A(i)   ((uint8_t)(((i) >> NOVA_A_POS) & 0xFF))
#define NOVA_GET_B(i)   ((uint8_t)(((i) >> NOVA_B_POS) & 0xFF))
#define NOVA_GET_C(i)   ((uint8_t)(((i) >> NOVA_C_POS) & 0xFF))
#define NOVA_GET_BX(i)  ((uint16_t)((i) & 0xFFFF))
#define NOVA_GET_SBX(i) ((int16_t)(NOVA_GET_BX(i) - NOVA_SBX_BIAS))
#define NOVA_GET_AX(i)  ((uint32_t)((i) & 0xFFFFFF))

/* ============================================================
 * INSTRUCTION CREATION MACROS
 * ============================================================ */

/*
** PCM: NOVA_CREATE_ABC / NOVA_CREATE_ABX / NOVA_CREATE_ASBX / NOVA_CREATE_AX
** Purpose: Construct instruction words from fields
** Rationale: Called during compilation for every bytecode emission
** Performance Impact: Shift + OR, compiles to 2-3 instructions
** Audit Date: 2026-02-05
*/
#define NOVA_CREATE_ABC(op, a, b, c) \
    ((NovaInstruction)(((uint32_t)(op) << NOVA_OP_POS) | \
                       ((uint32_t)(a)  << NOVA_A_POS)  | \
                       ((uint32_t)(b)  << NOVA_B_POS)  | \
                       ((uint32_t)(c)  << NOVA_C_POS)))

#define NOVA_CREATE_ABX(op, a, bx) \
    ((NovaInstruction)(((uint32_t)(op) << NOVA_OP_POS) | \
                       ((uint32_t)(a)  << NOVA_A_POS)  | \
                       ((uint32_t)(bx) & 0xFFFF)))

#define NOVA_CREATE_ASBX(op, a, sbx) \
    NOVA_CREATE_ABX(op, a, (uint16_t)((sbx) + NOVA_SBX_BIAS))

#define NOVA_CREATE_AX(op, ax) \
    ((NovaInstruction)(((uint32_t)(op) << NOVA_OP_POS) | \
                       ((uint32_t)(ax) & 0xFFFFFF)))

/* ============================================================
 * OPCODES
 * ============================================================ */

/**
 * @brief Nova opcode enumeration
 *
 * Each opcode has a mnemonic, encoding format, and description.
 * The VM dispatch loop uses this as array index for computed goto.
 */
typedef enum {
    /* ---- Data Movement ---- */
    NOVA_OP_MOVE = 0,      /**< R(A) = R(B)                           iABC  */
    NOVA_OP_LOADK,         /**< R(A) = K(Bx)                          iABx  */
    NOVA_OP_LOADKX,        /**< R(A) = K(extra)  (next instr = Ax)    iABx  */
    NOVA_OP_LOADBOOL,      /**< R(A) = (bool)B; if C then pc++        iABC  */
    NOVA_OP_LOADNIL,       /**< R(A..A+B) = nil                       iABC  */
    NOVA_OP_LOADINT,       /**< R(A) = sBx                            iAsBx */

    /* ---- Upvalues and Globals ---- */
    NOVA_OP_GETUPVAL,      /**< R(A) = U(B)                           iABC  */
    NOVA_OP_SETUPVAL,      /**< U(B) = R(A)                           iABC  */
    NOVA_OP_GETGLOBAL,     /**< R(A) = G[K(Bx)]                       iABx  */
    NOVA_OP_SETGLOBAL,     /**< G[K(Bx)] = R(A)                       iABx  */

    /* ---- Table Operations ---- */
    NOVA_OP_NEWTABLE,      /**< R(A) = {} (B=arr hint, C=hash hint)   iABC  */
    NOVA_OP_GETTABLE,      /**< R(A) = R(B)[R(C)]                     iABC  */
    NOVA_OP_SETTABLE,      /**< R(A)[R(B)] = R(C)                     iABC  */
    NOVA_OP_GETFIELD,      /**< R(A) = R(B)[K(C)]  (string key opt)   iABC  */
    NOVA_OP_SETFIELD,      /**< R(A)[K(B)] = R(C)  (string key opt)   iABC  */
    NOVA_OP_GETINDEX,      /**< R(A) = R(B)[C]     (integer key opt)  iABC  */
    NOVA_OP_SETINDEX,      /**< R(A)[B] = R(C)     (integer key opt)  iABC  */
    NOVA_OP_SETLIST,       /**< R(A)[B..B+C] = R(A+1..A+C)            iABC  */
    NOVA_OP_SELF,          /**< R(A+1) = R(B); R(A) = R(B)[K(C)]      iABC  */

    /* ---- Arithmetic ---- */
    NOVA_OP_ADD,           /**< R(A) = R(B) + R(C)                    iABC  */
    NOVA_OP_SUB,           /**< R(A) = R(B) - R(C)                    iABC  */
    NOVA_OP_MUL,           /**< R(A) = R(B) * R(C)                    iABC  */
    NOVA_OP_DIV,           /**< R(A) = R(B) / R(C)                    iABC  */
    NOVA_OP_IDIV,          /**< R(A) = R(B) // R(C)                   iABC  */
    NOVA_OP_MOD,           /**< R(A) = R(B) % R(C)                    iABC  */
    NOVA_OP_POW,           /**< R(A) = R(B) ^ R(C)                    iABC  */
    NOVA_OP_UNM,           /**< R(A) = -R(B)                          iABC  */
    NOVA_OP_LEN,           /**< R(A) = #R(B)                          iABC  */

    /* ---- Bitwise ---- */
    NOVA_OP_BAND,          /**< R(A) = R(B) & R(C)                    iABC  */
    NOVA_OP_BOR,           /**< R(A) = R(B) | R(C)                    iABC  */
    NOVA_OP_BXOR,          /**< R(A) = R(B) ~ R(C)                    iABC  */
    NOVA_OP_BNOT,          /**< R(A) = ~R(B)                          iABC  */
    NOVA_OP_SHL,           /**< R(A) = R(B) << R(C)                   iABC  */
    NOVA_OP_SHR,           /**< R(A) = R(B) >> R(C)                   iABC  */

    /* ---- String ---- */
    NOVA_OP_CONCAT,        /**< R(A) = R(B) .. ... .. R(C)            iABC  */

    /* ---- Comparison ---- */
    NOVA_OP_EQ,            /**< if (R(B) == R(C)) ~= A then pc++      iABC  */
    NOVA_OP_LT,            /**< if (R(B) <  R(C)) ~= A then pc++      iABC  */
    NOVA_OP_LE,            /**< if (R(B) <= R(C)) ~= A then pc++      iABC  */

    /* ---- Logical / Test ---- */
    NOVA_OP_NOT,           /**< R(A) = not R(B)                       iABC  */
    NOVA_OP_TEST,          /**< if (bool)R(A) ~= C then pc++          iABC  */
    NOVA_OP_TESTSET,       /**< if (bool)R(B) ~= C then R(A)=R(B) else pc++ iABC */

    /* ---- Control Flow ---- */
    NOVA_OP_JMP,           /**< pc += sBx                             iAsBx */
    NOVA_OP_CALL,          /**< R(A)(R(A+1),...,R(A+B-1)); results -> R(A)..R(A+C-2) iABC */
    NOVA_OP_TAILCALL,      /**< return R(A)(R(A+1),...,R(A+B-1))      iABC  */
    NOVA_OP_RETURN,        /**< return R(A),...,R(A+B-2)               iABC  */

    /* ---- Loops ---- */
    NOVA_OP_FORPREP,       /**< R(A) -= R(A+2); pc += sBx             iAsBx */
    NOVA_OP_FORLOOP,       /**< R(A) += R(A+2); if R(A) <= R(A+1): pc += sBx, R(A+3) = R(A) iAsBx */
    NOVA_OP_TFORCALL,      /**< R(A+3),...= R(A)(R(A+1),R(A+2))       iABC  */
    NOVA_OP_TFORLOOP,      /**< if R(A+1) ~= nil: R(A)=R(A+1) else pc++ iAsBx */

    /* ---- Closures ---- */
    NOVA_OP_CLOSURE,       /**< R(A) = closure(proto[Bx])             iABx  */
    NOVA_OP_VARARG,        /**< R(A),...,R(A+B-2) = vararg            iABC  */
    NOVA_OP_CLOSE,         /**< close upvalues >= R(A)                iABC  */

    /* ---- Special ---- */
    NOVA_OP_NOP,           /**< No operation                          -     */
    NOVA_OP_EXTRAARG,      /**< Extra argument for previous opcode    iAx   */

    NOVA_OP_COUNT          /**< Total number of opcodes               */
} NovaOpCode;

/* ============================================================
 * OPCODE METADATA
 * ============================================================ */

/**
 * @brief Instruction format type
 */
typedef enum {
    NOVA_FMT_ABC,       /**< Three 8-bit fields: A, B, C       */
    NOVA_FMT_ABX,       /**< A (8-bit) + Bx (16-bit unsigned)  */
    NOVA_FMT_ASBX,      /**< A (8-bit) + sBx (16-bit signed)   */
    NOVA_FMT_AX         /**< Ax (24-bit unsigned)               */
} NovaInstrFormat;

/**
 * @brief Opcode metadata for disassembly and verification
 */
typedef struct {
    const char         *name;       /**< Opcode mnemonic             */
    NovaInstrFormat     format;     /**< Instruction format          */
    uint8_t             sets_a;     /**< Does this opcode write R(A)?*/
    uint8_t             uses_b;     /**< Does B refer to a register? */
    uint8_t             uses_c;     /**< Does C refer to a register? */
} NovaOpInfo;

/**
 * @brief Get opcode metadata
 *
 * @param op Opcode
 * @return Pointer to opcode info (static, do not free)
 */
const NovaOpInfo *nova_opcode_info(NovaOpCode op);

/**
 * @brief Get opcode name string
 *
 * @param op Opcode
 * @return Opcode name (e.g., "ADD", "LOADK")
 */
const char *nova_opcode_name(NovaOpCode op);

/* ============================================================
 * .no BINARY FORMAT STRUCTURES
 * ============================================================ */

/**
 * @brief .no file header (on-disk format)
 *
 * All multi-byte fields are little-endian (canonical format).
 */
typedef struct {
    uint32_t magic;         /**< NOVA_BYTECODE_MAGIC (0x4E4F5641) */
    uint16_t version;       /**< Format version (major << 8 | minor) */
    uint16_t flags;         /**< Flags (see NOVA_BC_FLAG_*) */
    uint32_t platform;      /**< Platform tag (0 = portable) */
    uint32_t _reserved;     /**< Reserved for future use */
    uint64_t timestamp;     /**< Compilation timestamp (Unix epoch) */
    uint64_t checksum;      /**< NXH64 checksum of payload */
} NovaBytecodeHeader;

/** Bytecode flags */
#define NOVA_BC_FLAG_DEBUG      0x0001  /**< Contains debug info */
#define NOVA_BC_FLAG_STRIPPED   0x0002  /**< Debug info stripped */
#define NOVA_BC_FLAG_OPTIMIZED  0x0004  /**< Optimization passes applied */

/**
 * @brief Write bytecode to file
 *
 * @param proto     Root function prototype
 * @param path      Output file path
 * @param flags     Bytecode flags (NOVA_BC_FLAG_*)
 * @return 0 on success, -1 on error
 */
int nova_bytecode_write(const NovaProto *proto, const char *path,
                        uint16_t flags);

/**
 * @brief Read bytecode from file
 *
 * @param path      Input file path
 * @param proto     Output: loaded prototype (caller must free)
 * @return 0 on success, -1 on error
 */
int nova_bytecode_read(const char *path, NovaProto **proto);

/**
 * @brief Verify bytecode integrity
 *
 * @param data      Bytecode data
 * @param len       Data length
 * @return 0 if valid, -1 if invalid
 */
int nova_bytecode_verify(const void *data, size_t len);

/**
 * @brief Disassemble a function prototype (debug)
 *
 * @param proto     Function prototype
 * @param out       Output stream (e.g., stdout)
 */
void nova_bytecode_dump(const NovaProto *proto, FILE *out);

#endif /* NOVA_CODE_H */
