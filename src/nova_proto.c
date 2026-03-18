/**
 * @file nova_proto.c
 * @brief Nova Language - Function Prototype Implementation
 *
 * Implements NovaProto lifecycle, instruction emission, constant
 * pool management, sub-prototype linkage, upvalue tracking,
 * local variable debug info, and disassembly output.
 *
 * All dynamic arrays use geometric growth (2x) for amortized
 * O(1) append. Constant deduplication uses linear scan -- fine
 * for typical function sizes (< 256 constants). If this becomes
 * a bottleneck, we can add a hash index later.
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

#include "nova/nova_proto.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

/* ============================================================
 * INTERNAL GROWTH HELPERS
 * ============================================================ */

/** Default initial capacity for dynamic arrays */
#define NOVAI_PROTO_INIT_CODE   64
#define NOVAI_PROTO_INIT_CONST  16
#define NOVAI_PROTO_INIT_PROTOS 4
#define NOVAI_PROTO_INIT_LOCALS 16

/**
 * @brief Ensure the code array has room for at least one more instruction.
 *
 * @param proto  Target proto
 * @return 0 on success, -1 on allocation failure
 */
static int novai_ensure_code(NovaProto *proto) {
    if (proto->code_count < proto->code_capacity) {
        return 0;
    }

    uint32_t new_cap = (proto->code_capacity == 0)
        ? NOVAI_PROTO_INIT_CODE
        : proto->code_capacity * 2;

    NovaInstruction *new_code = realloc(
        proto->code, (size_t)new_cap * sizeof(NovaInstruction));
    if (new_code == NULL) {
        return -1;
    }
    proto->code = new_code;

    /* Also grow line info in lockstep */
    uint32_t *new_lines = realloc(
        proto->lines.line_numbers, (size_t)new_cap * sizeof(uint32_t));
    if (new_lines == NULL) {
        return -1;
    }
    proto->lines.line_numbers = new_lines;

    proto->code_capacity = new_cap;
    return 0;
}

/**
 * @brief Ensure the constant pool has room for at least one more entry.
 *
 * @param proto  Target proto
 * @return 0 on success, -1 on allocation failure
 */
static int novai_ensure_constants(NovaProto *proto) {
    if (proto->const_count < proto->const_capacity) {
        return 0;
    }

    uint32_t new_cap = (proto->const_capacity == 0)
        ? NOVAI_PROTO_INIT_CONST
        : proto->const_capacity * 2;

    NovaConstant *new_consts = realloc(
        proto->constants, (size_t)new_cap * sizeof(NovaConstant));
    if (new_consts == NULL) {
        return -1;
    }
    proto->constants = new_consts;
    proto->const_capacity = new_cap;
    return 0;
}

/**
 * @brief Ensure the sub-proto array has room for one more child.
 *
 * @param proto  Target proto
 * @return 0 on success, -1 on allocation failure
 */
static int novai_ensure_protos(NovaProto *proto) {
    if (proto->proto_count < proto->proto_capacity) {
        return 0;
    }

    uint32_t new_cap = (proto->proto_capacity == 0)
        ? NOVAI_PROTO_INIT_PROTOS
        : proto->proto_capacity * 2;

    NovaProto **new_arr = realloc(
        proto->protos, (size_t)new_cap * sizeof(NovaProto *));
    if (new_arr == NULL) {
        return -1;
    }
    proto->protos = new_arr;
    proto->proto_capacity = new_cap;
    return 0;
}

/**
 * @brief Ensure the local info array has room for one more entry.
 *
 * @param proto  Target proto
 * @return 0 on success, -1 on allocation failure
 */
static int novai_ensure_locals(NovaProto *proto) {
    if (proto->local_count < proto->local_capacity) {
        return 0;
    }

    uint32_t new_cap = (proto->local_capacity == 0)
        ? NOVAI_PROTO_INIT_LOCALS
        : proto->local_capacity * 2;

    NovaLocalInfo *new_locals = realloc(
        proto->locals, (size_t)new_cap * sizeof(NovaLocalInfo));
    if (new_locals == NULL) {
        return -1;
    }
    proto->locals = new_locals;
    proto->local_capacity = new_cap;
    return 0;
}

/* ============================================================
 * PROTO LIFECYCLE
 * ============================================================ */

NovaProto *nova_proto_create(void) {
    NovaProto *proto = calloc(1, sizeof(NovaProto));
    if (proto == NULL) {
        return NULL;
    }
    /* calloc zeroes everything: counts=0, pointers=NULL, flags=0 */
    return proto;
}

void nova_proto_destroy(NovaProto *proto) {
    if (proto == NULL) {
        return;
    }

    /* Recursively destroy sub-prototypes */
    for (uint32_t i = 0; i < proto->proto_count; i++) {
        nova_proto_destroy(proto->protos[i]);
    }

    free(proto->code);
    proto->code = NULL;

    free(proto->constants);
    proto->constants = NULL;

    free(proto->protos);
    proto->protos = NULL;

    free(proto->upvalues);
    proto->upvalues = NULL;

    free(proto->locals);
    proto->locals = NULL;

    free(proto->lines.line_numbers);
    proto->lines.line_numbers = NULL;

    free(proto);
}

/* ============================================================
 * INSTRUCTION EMISSION
 * ============================================================ */

uint32_t nova_proto_emit(NovaProto *proto, NovaInstruction instr,
                         uint32_t line) {
    if (proto == NULL) {
        return UINT32_MAX;
    }

    if (novai_ensure_code(proto) != 0) {
        return UINT32_MAX;
    }

    uint32_t pc = proto->code_count;
    proto->code[pc] = instr;
    proto->lines.line_numbers[pc] = line;
    proto->lines.count = pc + 1;
    proto->code_count = pc + 1;
    return pc;
}

uint32_t nova_proto_emit_abc(NovaProto *proto, NovaOpcode op,
                             uint8_t a, uint8_t b, uint8_t c,
                             uint32_t line) {
    return nova_proto_emit(proto, NOVA_ENCODE_ABC(op, a, b, c), line);
}

uint32_t nova_proto_emit_abx(NovaProto *proto, NovaOpcode op,
                             uint8_t a, uint16_t bx,
                             uint32_t line) {
    return nova_proto_emit(proto, NOVA_ENCODE_ABX(op, a, bx), line);
}

uint32_t nova_proto_emit_asbx(NovaProto *proto, NovaOpcode op,
                              uint8_t a, int sbx,
                              uint32_t line) {
    return nova_proto_emit(proto, NOVA_ENCODE_ASBX(op, a, sbx), line);
}

/* ============================================================
 * BACK-PATCHING
 * ============================================================ */

void nova_proto_patch_sbx(NovaProto *proto, uint32_t pc, int sbx) {
    if (proto == NULL || pc >= proto->code_count) {
        return;
    }
    NOVA_SET_SBX(proto->code[pc], sbx);
}

void nova_proto_patch_a(NovaProto *proto, uint32_t pc, uint8_t a) {
    if (proto == NULL || pc >= proto->code_count) {
        return;
    }
    NOVA_SET_A(proto->code[pc], a);
}

/* ============================================================
 * CONSTANT POOL
 * ============================================================ */

uint32_t nova_proto_add_nil(NovaProto *proto) {
    if (proto == NULL) {
        return UINT32_MAX;
    }

    /* Check for existing nil constant */
    for (uint32_t i = 0; i < proto->const_count; i++) {
        if (proto->constants[i].tag == NOVA_CONST_NIL) {
            return i;
        }
    }

    if (novai_ensure_constants(proto) != 0) {
        return UINT32_MAX;
    }

    uint32_t idx = proto->const_count;
    proto->constants[idx].tag = NOVA_CONST_NIL;
    memset(&proto->constants[idx].as, 0, sizeof(proto->constants[idx].as));
    proto->const_count = idx + 1;
    return idx;
}

uint32_t nova_proto_add_bool(NovaProto *proto, int value) {
    if (proto == NULL) {
        return UINT32_MAX;
    }

    int normalized = (value != 0) ? 1 : 0;

    /* Check for existing bool constant with same value */
    for (uint32_t i = 0; i < proto->const_count; i++) {
        if (proto->constants[i].tag == NOVA_CONST_BOOL
            && proto->constants[i].as.boolean == normalized) {
            return i;
        }
    }

    if (novai_ensure_constants(proto) != 0) {
        return UINT32_MAX;
    }

    uint32_t idx = proto->const_count;
    proto->constants[idx].tag = NOVA_CONST_BOOL;
    proto->constants[idx].as.boolean = normalized;
    proto->const_count = idx + 1;
    return idx;
}

uint32_t nova_proto_add_integer(NovaProto *proto, nova_int_t value) {
    if (proto == NULL) {
        return UINT32_MAX;
    }

    /* Check for existing integer constant */
    for (uint32_t i = 0; i < proto->const_count; i++) {
        if (proto->constants[i].tag == NOVA_CONST_INTEGER
            && proto->constants[i].as.integer == value) {
            return i;
        }
    }

    if (novai_ensure_constants(proto) != 0) {
        return UINT32_MAX;
    }

    uint32_t idx = proto->const_count;
    proto->constants[idx].tag = NOVA_CONST_INTEGER;
    proto->constants[idx].as.integer = value;
    proto->const_count = idx + 1;
    return idx;
}

uint32_t nova_proto_add_number(NovaProto *proto, nova_number_t value) {
    if (proto == NULL) {
        return UINT32_MAX;
    }

    /* Check for existing number constant (exact bit comparison) */
    for (uint32_t i = 0; i < proto->const_count; i++) {
        if (proto->constants[i].tag == NOVA_CONST_NUMBER) {
            /* Use memcmp to handle NaN correctly */
            if (memcmp(&proto->constants[i].as.number, &value,
                       sizeof(nova_number_t)) == 0) {
                return i;
            }
        }
    }

    if (novai_ensure_constants(proto) != 0) {
        return UINT32_MAX;
    }

    uint32_t idx = proto->const_count;
    proto->constants[idx].tag = NOVA_CONST_NUMBER;
    proto->constants[idx].as.number = value;
    proto->const_count = idx + 1;
    return idx;
}

uint32_t nova_proto_add_string(NovaProto *proto, const char *str,
                               uint32_t length) {
    if (proto == NULL || str == NULL) {
        return UINT32_MAX;
    }

    if (novai_ensure_constants(proto) != 0) {
        return UINT32_MAX;
    }

    uint32_t idx = proto->const_count;
    proto->constants[idx].tag = NOVA_CONST_STRING;
    proto->constants[idx].as.string.data = str;
    proto->constants[idx].as.string.length = length;
    proto->const_count = idx + 1;
    return idx;
}

uint32_t nova_proto_find_or_add_string(NovaProto *proto, const char *str,
                                       uint32_t length) {
    if (proto == NULL || str == NULL) {
        return UINT32_MAX;
    }

    /* Linear scan for duplicate */
    for (uint32_t i = 0; i < proto->const_count; i++) {
        if (proto->constants[i].tag == NOVA_CONST_STRING
            && proto->constants[i].as.string.length == length
            && memcmp(proto->constants[i].as.string.data, str,
                      (size_t)length) == 0) {
            return i;
        }
    }

    return nova_proto_add_string(proto, str, length);
}

/* ============================================================
 * SUB-PROTOTYPES
 * ============================================================ */

uint32_t nova_proto_add_child(NovaProto *proto, NovaProto *child) {
    if (proto == NULL || child == NULL) {
        return UINT32_MAX;
    }

    if (novai_ensure_protos(proto) != 0) {
        return UINT32_MAX;
    }

    uint32_t idx = proto->proto_count;
    proto->protos[idx] = child;
    proto->proto_count = idx + 1;
    return idx;
}

/* ============================================================
 * UPVALUE MANAGEMENT
 * ============================================================ */

uint8_t nova_proto_add_upvalue(NovaProto *proto, uint8_t index,
                               uint8_t in_stack, const char *name) {
    if (proto == NULL || proto->upvalue_count >= NOVA_MAX_UPVALUES) {
        return 255;
    }

    /* Check for existing upvalue with same capture source */
    for (uint8_t i = 0; i < proto->upvalue_count; i++) {
        if (proto->upvalues[i].index == index
            && proto->upvalues[i].in_stack == in_stack) {
            return i;
        }
    }

    /* Grow upvalue array */
    uint8_t count = proto->upvalue_count;
    uint8_t new_count = count + 1;
    NovaUpvalDesc *new_arr = realloc(
        proto->upvalues, (size_t)new_count * sizeof(NovaUpvalDesc));
    if (new_arr == NULL) {
        return 255;
    }
    proto->upvalues = new_arr;

    proto->upvalues[count].index    = index;
    proto->upvalues[count].in_stack = in_stack;
    proto->upvalues[count].kind     = 0;
    proto->upvalues[count].pad_     = 0;
    proto->upvalues[count].name     = name;
    proto->upvalue_count = new_count;
    return count;
}

/* ============================================================
 * LOCAL VARIABLE INFO
 * ============================================================ */

uint32_t nova_proto_add_local(NovaProto *proto, const char *name,
                              uint8_t reg, uint32_t start_pc) {
    if (proto == NULL) {
        return UINT32_MAX;
    }

    if (novai_ensure_locals(proto) != 0) {
        return UINT32_MAX;
    }

    uint32_t idx = proto->local_count;
    proto->locals[idx].name     = name;
    proto->locals[idx].reg      = reg;
    proto->locals[idx].start_pc = start_pc;
    proto->locals[idx].end_pc   = UINT32_MAX;  /* Open until closed */
    memset(proto->locals[idx].pad_, 0, sizeof(proto->locals[idx].pad_));
    proto->local_count = idx + 1;
    return idx;
}

void nova_proto_close_local(NovaProto *proto, uint8_t reg,
                            uint32_t end_pc) {
    if (proto == NULL) {
        return;
    }

    /* Search backward for the most recent open local on this register */
    for (uint32_t i = proto->local_count; i > 0; i--) {
        uint32_t idx = i - 1;
        if (proto->locals[idx].reg == reg
            && proto->locals[idx].end_pc == UINT32_MAX) {
            proto->locals[idx].end_pc = end_pc;
            return;
        }
    }
}

/* ============================================================
 * DISASSEMBLY
 * ============================================================ */

/**
 * @brief Print a single instruction in human-readable form.
 */
static void novai_dump_instruction(const NovaProto *proto, uint32_t pc,
                                   int indent) {
    NovaInstruction instr = proto->code[pc];
    NovaOpcode op = NOVA_GET_OPCODE(instr);
    const NovaOpcodeInfo *info = &nova_opcode_info[(uint8_t)op];
    const char *name = (info->name != NULL) ? info->name : "???";

    /* Indent */
    for (int j = 0; j < indent; j++) {
        fputc(' ', stderr);
    }

    /* Line number and PC */
    uint32_t line = 0;
    if (pc < proto->lines.count) {
        line = proto->lines.line_numbers[pc];
    }

    fprintf(stderr, "  [%04u] %-4u %-12s", pc, line, name);

    /* Format-specific operand printing */
    switch (info->format) {
        case NOVA_FMT_ABC: {
            uint8_t a = NOVA_GET_A(instr);
            uint8_t b = NOVA_GET_B(instr);
            uint8_t c = NOVA_GET_C(instr);
            fprintf(stderr, "A=%-3u B=%-3u C=%-3u", a, b, c);
            break;
        }
        case NOVA_FMT_ABX: {
            uint8_t a = NOVA_GET_A(instr);
            uint16_t bx = NOVA_GET_BX(instr);
            fprintf(stderr, "A=%-3u Bx=%-5u", a, bx);

            /* Show constant value for LOADK-type ops */
            if (info->setsflag && bx < proto->const_count) {
                NovaConstant *k = &proto->constants[bx];
                fprintf(stderr, " ; ");
                switch (k->tag) {
                    case NOVA_CONST_NIL:
                        fprintf(stderr, "nil");
                        break;
                    case NOVA_CONST_BOOL:
                        fprintf(stderr, "%s", k->as.boolean ? "true" : "false");
                        break;
                    case NOVA_CONST_INTEGER:
                        fprintf(stderr, "%ld", (long)k->as.integer);
                        break;
                    case NOVA_CONST_NUMBER:
                        fprintf(stderr, "%g", k->as.number);
                        break;
                    case NOVA_CONST_STRING:
                        fprintf(stderr, "\"%.*s\"",
                                (int)k->as.string.length, k->as.string.data);
                        break;
                    default:
                        fprintf(stderr, "?");
                        break;
                }
            }
            break;
        }
        case NOVA_FMT_ASBX: {
            uint8_t a = NOVA_GET_A(instr);
            int sbx = NOVA_GET_SBX(instr);
            fprintf(stderr, "A=%-3u sBx=%-5d", a, sbx);

            /* Show jump target for jump-type ops */
            if (nova_opcode_is_jump(op)) {
                int target = (int)pc + 1 + sbx;
                fprintf(stderr, " ; -> [%04d]", target);
            }
            break;
        }
        case NOVA_FMT_AX: {
            uint32_t ax = NOVA_GET_AX(instr);
            fprintf(stderr, "Ax=%u", ax);
            break;
        }
        case NOVA_FMT_NONE:
            /* No operands */
            break;
        default:
            break;
    }

    fputc('\n', stderr);
}

void nova_proto_dump(const NovaProto *proto, int indent) {
    if (proto == NULL) {
        return;
    }

    /* Header */
    for (int j = 0; j < indent; j++) {
        fputc(' ', stderr);
    }
    fprintf(stderr, "== function");
    if (proto->source != NULL) {
        fprintf(stderr, " <%s:%u-%u>",
                proto->source, proto->line_defined, proto->last_line);
    }
    fprintf(stderr, " (%u instructions, %u constants, %u upvalues",
            proto->code_count, proto->const_count, proto->upvalue_count);
    if (proto->is_async) {
        fprintf(stderr, ", async");
    }
    if (proto->is_vararg) {
        fprintf(stderr, ", vararg");
    }
    fprintf(stderr, ") ==\n");

    /* Parameters and stack */
    for (int j = 0; j < indent; j++) {
        fputc(' ', stderr);
    }
    fprintf(stderr, "  params=%u, maxstack=%u\n",
            proto->num_params, proto->max_stack);

    /* Instructions */
    for (uint32_t pc = 0; pc < proto->code_count; pc++) {
        novai_dump_instruction(proto, pc, indent);
    }

    /* Constants */
    if (proto->const_count > 0) {
        for (int j = 0; j < indent; j++) {
            fputc(' ', stderr);
        }
        fprintf(stderr, "  -- constants (%u):\n", proto->const_count);
        for (uint32_t i = 0; i < proto->const_count; i++) {
            for (int j = 0; j < indent; j++) {
                fputc(' ', stderr);
            }
            fprintf(stderr, "  K[%u] = ", i);
            switch (proto->constants[i].tag) {
                case NOVA_CONST_NIL:
                    fprintf(stderr, "nil");
                    break;
                case NOVA_CONST_BOOL:
                    fprintf(stderr, "%s",
                            proto->constants[i].as.boolean ? "true" : "false");
                    break;
                case NOVA_CONST_INTEGER:
                    fprintf(stderr, "%ld",
                            (long)proto->constants[i].as.integer);
                    break;
                case NOVA_CONST_NUMBER:
                    fprintf(stderr, "%g", proto->constants[i].as.number);
                    break;
                case NOVA_CONST_STRING:
                    fprintf(stderr, "\"%.*s\"",
                            (int)proto->constants[i].as.string.length,
                            proto->constants[i].as.string.data);
                    break;
                default:
                    fprintf(stderr, "?");
                    break;
            }
            fputc('\n', stderr);
        }
    }

    /* Upvalues */
    if (proto->upvalue_count > 0) {
        for (int j = 0; j < indent; j++) {
            fputc(' ', stderr);
        }
        fprintf(stderr, "  -- upvalues (%u):\n", proto->upvalue_count);
        for (uint8_t i = 0; i < proto->upvalue_count; i++) {
            for (int j = 0; j < indent; j++) {
                fputc(' ', stderr);
            }
            fprintf(stderr, "  U[%u] = %s idx=%u %s\n",
                    i,
                    proto->upvalues[i].name ? proto->upvalues[i].name : "?",
                    proto->upvalues[i].index,
                    proto->upvalues[i].in_stack ? "(dec)" : "(upvalue)");
        }
    }

    /* Sub-prototypes (recursive) */
    for (uint32_t i = 0; i < proto->proto_count; i++) {
        fprintf(stderr, "\n");
        nova_proto_dump(proto->protos[i], indent + 2);
    }
}
