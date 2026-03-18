/**
 * @file nova_opcode.c
 * @brief Nova Language - Opcode Metadata Table
 *
 * Defines the nova_opcode_info[256] table that maps every opcode
 * to its name, format, operand modes, and properties. This table
 * is the single source of truth for instruction introspection --
 * used by the disassembler, optimizer, debugger, and error system.
 *
 * Unassigned opcode slots have name == NULL, which allows runtime
 * validation: if nova_opcode_info[op].name == NULL, the opcode is
 * invalid (useful for ECC validation on bytecode loads).
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

#include "nova/nova_opcode.h"

#include <stddef.h>  /* NULL */

/* ============================================================
 * SHORTHAND MACROS FOR TABLE ENTRIES
 *
 * These macros are local to this file (undefined at bottom).
 * They reduce repetition and prevent column-alignment drift.
 * ============================================================ */

/*
** PCM: OP_ENTRY / OP_GAP
** Purpose: Build opcode info table entries with zero risk of field mismatch
** Rationale: 256-entry table built once, queried millions of times per second
** Performance Impact: Zero runtime cost (static const data)
** Audit Date: 2026-02-06
*/

#define OP_ENTRY(name, fmt, b_mode, c_mode, test, sets) \
    { (name), (fmt), (b_mode), (c_mode), (test), (sets) }

#define OP_GAP \
    { NULL, NOVA_FMT_NONE, NOVA_ARGMODE_UNUSED, NOVA_ARGMODE_UNUSED, 0, 0 }

/* Shorter aliases for enum values to keep columns aligned */
#define F_ABC   NOVA_FMT_ABC
#define F_ABX   NOVA_FMT_ABX
#define F_ASBX  NOVA_FMT_ASBX
#define F_AX    NOVA_FMT_AX
#define F_NONE  NOVA_FMT_NONE
#define M_NONE  NOVA_ARGMODE_UNUSED
#define M_REG   NOVA_ARGMODE_REG
#define M_CONST NOVA_ARGMODE_CONST
#define M_RK    NOVA_ARGMODE_RK
#define M_INT   NOVA_ARGMODE_INT
#define M_JUMP  NOVA_ARGMODE_JUMP

/**
 * @brief Master opcode metadata table.
 *
 * Indexed by raw opcode byte (0x00-0xFF). Each entry describes:
 *   - name:     Human-readable opcode name (NULL for unassigned)
 *   - format:   Instruction layout (ABC, ABx, AsBx, Ax, NONE)
 *   - arg_b:    How the B field is interpreted
 *   - arg_c:    How the C field is interpreted
 *   - testflag: 1 if this is a conditional test (affects pc)
 *   - setsflag: 1 if this instruction writes to R(A)
 */
const NovaOpcodeInfo nova_opcode_info[256] = {

    /* 0x00 - Data Movement */
    /* 0x00 MOVE      */ OP_ENTRY("MOVE",     F_ABC,  M_REG,  M_NONE, 0, 1),
    /* 0x01 LOADK     */ OP_ENTRY("LOADK",    F_ABX,  M_NONE, M_NONE, 0, 1),
    /* 0x02 LOADNIL   */ OP_ENTRY("LOADNIL",  F_ABC,  M_INT,  M_NONE, 0, 1),
    /* 0x03 LOADBOOL  */ OP_ENTRY("LOADBOOL", F_ABC,  M_INT,  M_INT,  0, 1),
    /* 0x04 LOADINT   */ OP_ENTRY("LOADINT",  F_ASBX, M_NONE, M_NONE, 0, 1),
    /* 0x05 LOADKX    */ OP_ENTRY("LOADKX",   F_ABC,  M_NONE, M_NONE, 0, 1),
    /* 0x06 */ OP_GAP,
    /* 0x07 */ OP_GAP,
    /* 0x08 */ OP_GAP,
    /* 0x09 */ OP_GAP,
    /* 0x0A */ OP_GAP,
    /* 0x0B */ OP_GAP,
    /* 0x0C */ OP_GAP,
    /* 0x0D */ OP_GAP,
    /* 0x0E */ OP_GAP,
    /* 0x0F */ OP_GAP,

    /* 0x10 - Table Operations */
    /* 0x10 NEWTABLE  */ OP_ENTRY("NEWTABLE", F_ABC,  M_INT,  M_INT,  0, 1),
    /* 0x11 GETTABLE  */ OP_ENTRY("GETTABLE", F_ABC,  M_REG,  M_RK,   0, 1),
    /* 0x12 SETTABLE  */ OP_ENTRY("SETTABLE", F_ABC,  M_RK,   M_RK,   0, 0),
    /* 0x13 GETFIELD  */ OP_ENTRY("GETFIELD", F_ABC,  M_REG,  M_CONST,0, 1),
    /* 0x14 SETFIELD  */ OP_ENTRY("SETFIELD", F_ABC,  M_CONST,M_RK,   0, 0),
    /* 0x15 GETI      */ OP_ENTRY("GETI",     F_ABC,  M_REG,  M_INT,  0, 1),
    /* 0x16 SETI      */ OP_ENTRY("SETI",     F_ABC,  M_INT,  M_RK,   0, 0),
    /* 0x17 SETLIST   */ OP_ENTRY("SETLIST",  F_ABC,  M_INT,  M_INT,  0, 0),
    /* 0x18 SELF      */ OP_ENTRY("SELF",     F_ABC,  M_REG,  M_RK,   0, 1),
    /* 0x19 */ OP_GAP,
    /* 0x1A */ OP_GAP,
    /* 0x1B */ OP_GAP,
    /* 0x1C */ OP_GAP,
    /* 0x1D */ OP_GAP,
    /* 0x1E */ OP_GAP,
    /* 0x1F */ OP_GAP,

    /* 0x20 - Upvalue / Global / Environment */
    /* 0x20 GETUPVAL  */ OP_ENTRY("GETUPVAL", F_ABC,  M_INT,  M_NONE, 0, 1),
    /* 0x21 SETUPVAL  */ OP_ENTRY("SETUPVAL", F_ABC,  M_INT,  M_NONE, 0, 0),
    /* 0x22 GETGLOBAL */ OP_ENTRY("GETGLOBAL",F_ABX,  M_NONE, M_NONE, 0, 1),
    /* 0x23 SETGLOBAL */ OP_ENTRY("SETGLOBAL",F_ABX,  M_NONE, M_NONE, 0, 0),
    /* 0x24 GETTABUP  */ OP_ENTRY("GETTABUP", F_ABC,  M_INT,  M_RK,   0, 1),
    /* 0x25 SETTABUP  */ OP_ENTRY("SETTABUP", F_ABC,  M_RK,   M_RK,   0, 0),
    /* 0x26 */ OP_GAP,
    /* 0x27 */ OP_GAP,
    /* 0x28 */ OP_GAP,
    /* 0x29 */ OP_GAP,
    /* 0x2A */ OP_GAP,
    /* 0x2B */ OP_GAP,
    /* 0x2C */ OP_GAP,
    /* 0x2D */ OP_GAP,
    /* 0x2E */ OP_GAP,
    /* 0x2F */ OP_GAP,

    /* 0x30 - Arithmetic */
    /* 0x30 ADD       */ OP_ENTRY("ADD",      F_ABC,  M_REG,  M_REG,  0, 1),
    /* 0x31 SUB       */ OP_ENTRY("SUB",      F_ABC,  M_REG,  M_REG,  0, 1),
    /* 0x32 MUL       */ OP_ENTRY("MUL",      F_ABC,  M_REG,  M_REG,  0, 1),
    /* 0x33 DIV       */ OP_ENTRY("DIV",      F_ABC,  M_REG,  M_REG,  0, 1),
    /* 0x34 IDIV      */ OP_ENTRY("IDIV",     F_ABC,  M_REG,  M_REG,  0, 1),
    /* 0x35 MOD       */ OP_ENTRY("MOD",      F_ABC,  M_REG,  M_REG,  0, 1),
    /* 0x36 POW       */ OP_ENTRY("POW",      F_ABC,  M_REG,  M_REG,  0, 1),
    /* 0x37 UNM       */ OP_ENTRY("UNM",      F_ABC,  M_REG,  M_NONE, 0, 1),
    /* 0x38 ADDI      */ OP_ENTRY("ADDI",     F_ASBX, M_NONE, M_NONE, 0, 1),
    /* 0x39 ADDK      */ OP_ENTRY("ADDK",     F_ABC,  M_REG,  M_CONST,0, 1),
    /* 0x3A SUBK      */ OP_ENTRY("SUBK",     F_ABC,  M_REG,  M_CONST,0, 1),
    /* 0x3B MULK      */ OP_ENTRY("MULK",     F_ABC,  M_REG,  M_CONST,0, 1),
    /* 0x3C DIVK      */ OP_ENTRY("DIVK",     F_ABC,  M_REG,  M_CONST,0, 1),
    /* 0x3D MODK      */ OP_ENTRY("MODK",     F_ABC,  M_REG,  M_CONST,0, 1),
    /* 0x3E */ OP_GAP,
    /* 0x3F */ OP_GAP,

    /* 0x40 - Bitwise */
    /* 0x40 BAND      */ OP_ENTRY("BAND",     F_ABC,  M_REG,  M_REG,  0, 1),
    /* 0x41 BOR       */ OP_ENTRY("BOR",      F_ABC,  M_REG,  M_REG,  0, 1),
    /* 0x42 BXOR      */ OP_ENTRY("BXOR",     F_ABC,  M_REG,  M_REG,  0, 1),
    /* 0x43 BNOT      */ OP_ENTRY("BNOT",     F_ABC,  M_REG,  M_NONE, 0, 1),
    /* 0x44 SHL       */ OP_ENTRY("SHL",      F_ABC,  M_REG,  M_REG,  0, 1),
    /* 0x45 SHR       */ OP_ENTRY("SHR",      F_ABC,  M_REG,  M_REG,  0, 1),
    /* 0x46 */ OP_GAP,
    /* 0x47 */ OP_GAP,
    /* 0x48 */ OP_GAP,
    /* 0x49 */ OP_GAP,
    /* 0x4A */ OP_GAP,
    /* 0x4B */ OP_GAP,
    /* 0x4C */ OP_GAP,
    /* 0x4D */ OP_GAP,
    /* 0x4E */ OP_GAP,
    /* 0x4F */ OP_GAP,

    /* 0x50 - String */
    /* 0x50 CONCAT    */ OP_ENTRY("CONCAT",   F_ABC,  M_INT,  M_NONE, 0, 1),
    /* 0x51 STRLEN    */ OP_ENTRY("STRLEN",   F_ABC,  M_REG,  M_NONE, 0, 1),
    /* 0x52 */ OP_GAP,
    /* 0x53 */ OP_GAP,
    /* 0x54 */ OP_GAP,
    /* 0x55 */ OP_GAP,
    /* 0x56 */ OP_GAP,
    /* 0x57 */ OP_GAP,

    /* 0x58 - Comparison */
    /* 0x58 EQ        */ OP_ENTRY("EQ",       F_ABC,  M_REG,  M_REG,  1, 0),
    /* 0x59 LT        */ OP_ENTRY("LT",       F_ABC,  M_REG,  M_REG,  1, 0),
    /* 0x5A LE        */ OP_ENTRY("LE",       F_ABC,  M_REG,  M_REG,  1, 0),
    /* 0x5B EQK       */ OP_ENTRY("EQK",      F_ABC,  M_REG,  M_CONST,1, 0),
    /* 0x5C EQI       */ OP_ENTRY("EQI",      F_ASBX, M_NONE, M_NONE, 1, 0),
    /* 0x5D LTI       */ OP_ENTRY("LTI",      F_ASBX, M_NONE, M_NONE, 1, 0),
    /* 0x5E LEI       */ OP_ENTRY("LEI",      F_ASBX, M_NONE, M_NONE, 1, 0),
    /* 0x5F GTI       */ OP_ENTRY("GTI",      F_ASBX, M_NONE, M_NONE, 1, 0),
    /* 0x60 GEI       */ OP_ENTRY("GEI",      F_ASBX, M_NONE, M_NONE, 1, 0),
    /* 0x61 */ OP_GAP,
    /* 0x62 */ OP_GAP,
    /* 0x63 */ OP_GAP,
    /* 0x64 */ OP_GAP,
    /* 0x65 */ OP_GAP,
    /* 0x66 */ OP_GAP,
    /* 0x67 */ OP_GAP,

    /* 0x68 - Logical */
    /* 0x68 NOT       */ OP_ENTRY("NOT",      F_ABC,  M_REG,  M_NONE, 0, 1),
    /* 0x69 TEST      */ OP_ENTRY("TEST",     F_ABC,  M_NONE, M_INT,  1, 0),
    /* 0x6A TESTSET   */ OP_ENTRY("TESTSET",  F_ABC,  M_REG,  M_INT,  1, 1),
    /* 0x6B */ OP_GAP,
    /* 0x6C */ OP_GAP,
    /* 0x6D */ OP_GAP,
    /* 0x6E */ OP_GAP,
    /* 0x6F */ OP_GAP,

    /* 0x70 - Control Flow */
    /* 0x70 JMP       */ OP_ENTRY("JMP",      F_ASBX, M_NONE, M_NONE, 0, 0),
    /* 0x71 CALL      */ OP_ENTRY("CALL",     F_ABC,  M_INT,  M_INT,  0, 1),
    /* 0x72 TAILCALL  */ OP_ENTRY("TAILCALL", F_ABC,  M_INT,  M_NONE, 0, 0),
    /* 0x73 RETURN    */ OP_ENTRY("RETURN",   F_ABC,  M_INT,  M_NONE, 0, 0),
    /* 0x74 RETURN0   */ OP_ENTRY("RETURN0",  F_NONE, M_NONE, M_NONE, 0, 0),
    /* 0x75 RETURN1   */ OP_ENTRY("RETURN1",  F_ABC,  M_NONE, M_NONE, 0, 0),
    /* 0x76 */ OP_GAP,
    /* 0x77 */ OP_GAP,
    /* 0x78 */ OP_GAP,
    /* 0x79 */ OP_GAP,
    /* 0x7A */ OP_GAP,
    /* 0x7B */ OP_GAP,
    /* 0x7C */ OP_GAP,
    /* 0x7D */ OP_GAP,
    /* 0x7E */ OP_GAP,
    /* 0x7F */ OP_GAP,

    /* 0x80 - Loops */
    /* 0x80 FORPREP   */ OP_ENTRY("FORPREP",  F_ASBX, M_NONE, M_NONE, 0, 1),
    /* 0x81 FORLOOP   */ OP_ENTRY("FORLOOP",  F_ASBX, M_NONE, M_NONE, 0, 1),
    /* 0x82 TFORCALL  */ OP_ENTRY("TFORCALL", F_ABC,  M_NONE, M_INT,  0, 0),
    /* 0x83 TFORLOOP  */ OP_ENTRY("TFORLOOP", F_ASBX, M_NONE, M_NONE, 1, 1),
    /* 0x84 */ OP_GAP,
    /* 0x85 */ OP_GAP,
    /* 0x86 */ OP_GAP,
    /* 0x87 */ OP_GAP,
    /* 0x88 */ OP_GAP,
    /* 0x89 */ OP_GAP,
    /* 0x8A */ OP_GAP,
    /* 0x8B */ OP_GAP,
    /* 0x8C */ OP_GAP,
    /* 0x8D */ OP_GAP,
    /* 0x8E */ OP_GAP,
    /* 0x8F */ OP_GAP,

    /* 0x90 - Closures and Varargs */
    /* 0x90 CLOSURE   */ OP_ENTRY("CLOSURE",  F_ABX,  M_NONE, M_NONE, 0, 1),
    /* 0x91 VARARG    */ OP_ENTRY("VARARG",   F_ABC,  M_NONE, M_INT,  0, 1),
    /* 0x92 VARARGPREP*/ OP_ENTRY("VARARGPREP",F_ABC, M_NONE, M_NONE, 0, 0),
    /* 0x93 CLOSE     */ OP_ENTRY("CLOSE",    F_ABC,  M_NONE, M_NONE, 0, 0),
    /* 0x94 */ OP_GAP,
    /* 0x95 */ OP_GAP,
    /* 0x96 */ OP_GAP,
    /* 0x97 */ OP_GAP,
    /* 0x98 */ OP_GAP,
    /* 0x99 */ OP_GAP,
    /* 0x9A */ OP_GAP,
    /* 0x9B */ OP_GAP,
    /* 0x9C */ OP_GAP,
    /* 0x9D */ OP_GAP,
    /* 0x9E */ OP_GAP,
    /* 0x9F */ OP_GAP,

    /* 0xA0 - Async / Concurrency */
    /* 0xA0 AWAIT     */ OP_ENTRY("AWAIT",    F_ABC,  M_REG,  M_NONE, 0, 1),
    /* 0xA1 SPAWN     */ OP_ENTRY("SPAWN",    F_ABC,  M_INT,  M_NONE, 0, 0),
    /* 0xA2 YIELD     */ OP_ENTRY("YIELD",    F_ABC,  M_INT,  M_INT,  0, 1),
    /* 0xA3 */ OP_GAP,
    /* 0xA4 */ OP_GAP,
    /* 0xA5 */ OP_GAP,
    /* 0xA6 */ OP_GAP,
    /* 0xA7 */ OP_GAP,
    /* 0xA8 */ OP_GAP,
    /* 0xA9 */ OP_GAP,
    /* 0xAA */ OP_GAP,
    /* 0xAB */ OP_GAP,
    /* 0xAC */ OP_GAP,
    /* 0xAD */ OP_GAP,
    /* 0xAE */ OP_GAP,
    /* 0xAF */ OP_GAP,

    /* 0xB0 - Module */
    /* 0xB0 IMPORT    */ OP_ENTRY("IMPORT",   F_ABX,  M_NONE, M_NONE, 0, 1),
    /* 0xB1 EXPORT    */ OP_ENTRY("EXPORT",   F_ABX,  M_NONE, M_NONE, 0, 0),
    /* 0xB2 */ OP_GAP,
    /* 0xB3 */ OP_GAP,
    /* 0xB4 */ OP_GAP,
    /* 0xB5 */ OP_GAP,
    /* 0xB6 */ OP_GAP,
    /* 0xB7 */ OP_GAP,
    /* 0xB8 */ OP_GAP,
    /* 0xB9 */ OP_GAP,
    /* 0xBA */ OP_GAP,
    /* 0xBB */ OP_GAP,
    /* 0xBC */ OP_GAP,
    /* 0xBD */ OP_GAP,
    /* 0xBE */ OP_GAP,
    /* 0xBF */ OP_GAP,

    /* 0xC0-0xEF: Reserved for future expansion */
    /* 0xC0 */ OP_GAP, /* 0xC1 */ OP_GAP,
    /* 0xC2 */ OP_GAP, /* 0xC3 */ OP_GAP,
    /* 0xC4 */ OP_GAP, /* 0xC5 */ OP_GAP,
    /* 0xC6 */ OP_GAP, /* 0xC7 */ OP_GAP,
    /* 0xC8 */ OP_GAP, /* 0xC9 */ OP_GAP,
    /* 0xCA */ OP_GAP, /* 0xCB */ OP_GAP,
    /* 0xCC */ OP_GAP, /* 0xCD */ OP_GAP,
    /* 0xCE */ OP_GAP, /* 0xCF */ OP_GAP,
    /* 0xD0 */ OP_GAP, /* 0xD1 */ OP_GAP,
    /* 0xD2 */ OP_GAP, /* 0xD3 */ OP_GAP,
    /* 0xD4 */ OP_GAP, /* 0xD5 */ OP_GAP,
    /* 0xD6 */ OP_GAP, /* 0xD7 */ OP_GAP,
    /* 0xD8 */ OP_GAP, /* 0xD9 */ OP_GAP,
    /* 0xDA */ OP_GAP, /* 0xDB */ OP_GAP,
    /* 0xDC */ OP_GAP, /* 0xDD */ OP_GAP,
    /* 0xDE */ OP_GAP, /* 0xDF */ OP_GAP,
    /* 0xE0 */ OP_GAP, /* 0xE1 */ OP_GAP,
    /* 0xE2 */ OP_GAP, /* 0xE3 */ OP_GAP,
    /* 0xE4 */ OP_GAP, /* 0xE5 */ OP_GAP,
    /* 0xE6 */ OP_GAP, /* 0xE7 */ OP_GAP,
    /* 0xE8 */ OP_GAP, /* 0xE9 */ OP_GAP,
    /* 0xEA */ OP_GAP, /* 0xEB */ OP_GAP,
    /* 0xEC */ OP_GAP, /* 0xED */ OP_GAP,
    /* 0xEE */ OP_GAP, /* 0xEF */ OP_GAP,

    /* 0xF0 - Special */
    /* 0xF0 NOP       */ OP_ENTRY("NOP",      F_NONE, M_NONE, M_NONE, 0, 0),
    /* 0xF1 DEBUG     */ OP_ENTRY("DEBUG",    F_ABC,  M_NONE, M_NONE, 0, 0),
    /* 0xF2 */ OP_GAP,
    /* 0xF3 */ OP_GAP,
    /* 0xF4 */ OP_GAP,
    /* 0xF5 */ OP_GAP,
    /* 0xF6 */ OP_GAP,
    /* 0xF7 */ OP_GAP,
    /* 0xF8 */ OP_GAP,
    /* 0xF9 */ OP_GAP,
    /* 0xFA */ OP_GAP,
    /* 0xFB */ OP_GAP,
    /* 0xFC */ OP_GAP,
    /* 0xFD */ OP_GAP,
    /* 0xFE EXTRAARG  */ OP_ENTRY("EXTRAARG", F_AX,   M_NONE, M_NONE, 0, 0),
    /* 0xFF (sentinel, not a real opcode) */
                         OP_GAP,
};

/* Clean up local shorthand macros */
#undef OP_ENTRY
#undef OP_GAP
#undef F_ABC
#undef F_ABX
#undef F_ASBX
#undef F_AX
#undef F_NONE
#undef M_NONE
#undef M_REG
#undef M_CONST
#undef M_RK
#undef M_INT
#undef M_JUMP
