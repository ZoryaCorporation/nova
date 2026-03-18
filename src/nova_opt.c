/**
 * @file nova_opt.c
 * @brief Nova Language - Bytecode Optimizer Implementation
 *
 * Post-compilation optimization passes operating on NovaProto.
 * Transforms bytecode in-place between compilation and codegen.
 *
 * Pass ordering:
 *   1. Peephole (local instruction rewrites)
 *   2. Jump optimization (chain collapse, jump-to-next)
 *   3. Return specialization (RETURN -> RETURN0/RETURN1)
 *   4. Constant folding (evaluate constant arithmetic)
 *   5. Tail call detection (CALL+RETURN -> TAILCALL)
 *   6. Dead code elimination (NOP unreachable instructions)
 *   7. NOP squeeze (compact, adjust all jump targets)
 *
 * @author Anthony Taliento
 * @date 2026-02-08
 * @version 0.2.0
 *
 * @copyright Copyright (c) 2026 Zorya Corporation
 * @license MIT
 *
 * DEPENDENCIES:
 *   - nova_opcode.h (decode/encode macros, opcode metadata)
 *   - nova_proto.h  (NovaProto, NovaConstant types)
 *   - nova_conf.h   (limits, nova_int_t, nova_number_t)
 *
 * THREAD SAFETY:
 *   Not thread-safe. Each thread should optimize separate protos.
 *
 * ZORYA-C COMPLIANCE: v2.0.0
 */

#include "nova/nova_opt.h"
#include "nova/nova_opcode.h"
#include "nova/nova_proto.h"
#include "nova/nova_conf.h"

#include <stdint.h>
#include <string.h>  /* memset, memmove */
#include <stdlib.h>  /* malloc, free, calloc */
#include <stdio.h>   /* fprintf for dump_stats */

/* ============================================================
 * INTERNAL CONSTANTS
 * ============================================================ */

/** Maximum jump chain hops before giving up (prevent infinite loop) */
#define NOVAI_MAX_CHAIN_HOPS  16

/* ============================================================
 * PART 1: INTERNAL HELPERS
 * ============================================================ */

/**
 * @brief Replace an instruction with NOP, preserving line info.
 *
 * @param proto  Target proto
 * @param pc     Instruction index to NOP-ify
 */
static void novai_nopify(NovaProto *proto, uint32_t pc) {
    if (pc < proto->code_count) {
        proto->code[pc] = NOVA_ENCODE_ABC(NOVA_OP_NOP, 0, 0, 0);
    }
}

/**
 * @brief Check if an instruction is NOP.
 *
 * @param instr  Instruction to check
 * @return 1 if NOP, 0 otherwise
 */
static int novai_is_nop(NovaInstruction instr) {
    return NOVA_GET_OPCODE(instr) == NOVA_OP_NOP;
}

/**
 * @brief Check if an instruction is an EXTRAARG.
 *
 * EXTRAARG must never be separated from its preceding instruction
 * (LOADKX or SETLIST with large index).
 *
 * @param instr  Instruction to check
 * @return 1 if EXTRAARG, 0 otherwise
 */
static int novai_is_extraarg(NovaInstruction instr) {
    return NOVA_GET_OPCODE(instr) == NOVA_OP_EXTRAARG;
}

/**
 * @brief Compute the absolute target of a jump instruction.
 *
 * @param proto  Source proto
 * @param pc     Index of the JMP/FORPREP/FORLOOP/TFORLOOP instruction
 * @return Absolute target pc, or UINT32_MAX if out of bounds
 */
static uint32_t novai_get_jmp_target(const NovaProto *proto, uint32_t pc) {
    int sbx = NOVA_GET_SBX(proto->code[pc]);
    int target = (int)pc + 1 + sbx;
    if (target < 0 || (uint32_t)target >= proto->code_count) {
        return UINT32_MAX;
    }
    return (uint32_t)target;
}

/**
 * @brief Try to extract an integer constant from an RK-encoded operand.
 *
 * @param proto  Source proto (for constant pool)
 * @param rk     RK-encoded value (register or constant)
 * @param out    Output: integer value if successful
 * @return 1 if rk is a constant integer, 0 otherwise
 */
static int novai_get_rk_integer(const NovaProto *proto, uint8_t rk,
                                nova_int_t *out) {
    if (out == NULL) {
        return 0;
    }
    if (!NOVA_IS_RK_CONST(rk)) {
        return 0;  /* It's a register, not a constant */
    }
    uint32_t idx = (uint32_t)NOVA_RK_TO_CONST(rk);
    if (idx >= proto->const_count) {
        return 0;
    }
    if (proto->constants[idx].tag != NOVA_CONST_INTEGER) {
        return 0;
    }
    *out = proto->constants[idx].as.integer;
    return 1;
}

/**
 * @brief Try to extract a number constant from an RK-encoded operand.
 *
 * Also promotes NOVA_CONST_INTEGER to number.
 *
 * @param proto  Source proto (for constant pool)
 * @param rk     RK-encoded value
 * @param out    Output: number value if successful
 * @return 1 if rk is a constant number (or promotable integer), 0 otherwise
 */
static int novai_get_rk_number(const NovaProto *proto, uint8_t rk,
                               nova_number_t *out) {
    if (out == NULL) {
        return 0;
    }
    if (!NOVA_IS_RK_CONST(rk)) {
        return 0;
    }
    uint32_t idx = (uint32_t)NOVA_RK_TO_CONST(rk);
    if (idx >= proto->const_count) {
        return 0;
    }
    if (proto->constants[idx].tag == NOVA_CONST_NUMBER) {
        *out = proto->constants[idx].as.number;
        return 1;
    }
    if (proto->constants[idx].tag == NOVA_CONST_INTEGER) {
        *out = (nova_number_t)proto->constants[idx].as.integer;
        return 1;
    }
    return 0;
}

/**
 * @brief Build a reachability bitmap for dead code elimination.
 *
 * Uses a worklist-based forward walk from pc=0, following all
 * jump targets and fall-through paths.
 *
 * @param proto  Source proto
 * @return Heap-allocated bitmap (caller must free), or NULL on failure.
 *         reachable[pc] = 1 if instruction at pc is reachable.
 */
static uint8_t *novai_build_reachable(const NovaProto *proto) {
    uint32_t count = proto->code_count;
    if (count == 0) {
        return NULL;
    }

    uint8_t *reachable = (uint8_t *)calloc(count, 1);
    if (reachable == NULL) {
        return NULL;
    }

    /* Worklist: stack of pc values to explore */
    uint32_t *worklist = (uint32_t *)malloc(count * sizeof(uint32_t));
    if (worklist == NULL) {
        free(reachable);
        return NULL;
    }

    uint32_t wl_top = 0;

    /* Seed with pc=0 */
    worklist[wl_top++] = 0;

    while (wl_top > 0) {
        uint32_t pc = worklist[--wl_top];

        /* Bounds check and dedup */
        if (pc >= count || reachable[pc] != 0) {
            continue;
        }
        reachable[pc] = 1;

        NovaInstruction instr = proto->code[pc];
        NovaOpcode op = NOVA_GET_OPCODE(instr);

        /* EXTRAARG: always reachable if parent is, fall through */
        if (op == NOVA_OP_EXTRAARG) {
            if (pc + 1 < count) {
                worklist[wl_top++] = pc + 1;
            }
            continue;
        }

        /* Return variants: no successors */
        if (nova_opcode_is_return(op)) {
            continue;
        }

        /* Unconditional JMP */
        if (op == NOVA_OP_JMP) {
            uint32_t target = novai_get_jmp_target(proto, pc);
            if (target != UINT32_MAX) {
                worklist[wl_top++] = target;
            }
            continue;  /* No fall-through for unconditional JMP */
        }

        /* FORPREP: jumps to loop body end (targets FORLOOP) */
        if (op == NOVA_OP_FORPREP) {
            uint32_t target = novai_get_jmp_target(proto, pc);
            if (target != UINT32_MAX) {
                worklist[wl_top++] = target;
            }
            /* Also falls through to loop body */
            if (pc + 1 < count) {
                worklist[wl_top++] = pc + 1;
            }
            continue;
        }

        /* FORLOOP / TFORLOOP: branch back or fall through */
        if (op == NOVA_OP_FORLOOP || op == NOVA_OP_TFORLOOP) {
            uint32_t target = novai_get_jmp_target(proto, pc);
            if (target != UINT32_MAX) {
                worklist[wl_top++] = target;
            }
            if (pc + 1 < count) {
                worklist[wl_top++] = pc + 1;
            }
            continue;
        }

        /* Comparison / test (testflag=1): skip next on failure */
        if (nova_opcode_info[(uint8_t)op].testflag != 0) {
            /* Fall through (test passes) */
            if (pc + 1 < count) {
                worklist[wl_top++] = pc + 1;
            }
            /* Skip next (test fails) */
            if (pc + 2 < count) {
                worklist[wl_top++] = pc + 2;
            }
            continue;
        }

        /* LOADBOOL with C != 0: skip next */
        if (op == NOVA_OP_LOADBOOL && NOVA_GET_C(instr) != 0) {
            if (pc + 2 < count) {
                worklist[wl_top++] = pc + 2;
            }
            continue;  /* Does NOT fall through -- skips next */
        }

        /* TFORCALL: always falls through to TFORLOOP */
        /* Default: fall through to next instruction */
        if (pc + 1 < count) {
            worklist[wl_top++] = pc + 1;
        }
    }

    free(worklist);
    return reachable;
}

/* ============================================================
 * PART 2: PEEPHOLE PASS
 *
 * Single forward sweep. Pattern-match small instruction
 * sequences and rewrite them in-place.
 * ============================================================ */

/**
 * @brief Run peephole optimization pass.
 *
 * Patterns:
 *   P1: MOVE R(A), R(A) -> NOP (redundant self-move)
 *   P2: MOVE R(A),R(B) + RETURN1 R(A) -> RETURN1 R(B) + NOP
 *   P3: MOVE R(A),R(B) + RETURN R(A),N -> RETURN R(B),N + NOP
 *   P4: Double NOT collapse
 *   P5: Redundant LOADNIL (subsumed by following LOADNIL)
 *   P6: MOVE then immediate overwrite of same register
 *
 * @param proto  Proto to optimize
 * @param stats  Statistics output (may be NULL)
 */
static void novai_pass_peephole(NovaProto *proto, NovaOptStats *stats) {
    if (proto == NULL || proto->code_count < 1) {
        return;
    }

    NovaInstruction *code = proto->code;
    uint32_t count = proto->code_count;

    for (uint32_t pc = 0; pc < count; pc++) {
        NovaOpcode op = NOVA_GET_OPCODE(code[pc]);

        /* Skip NOPs and EXTRAARGs */
        if (op == NOVA_OP_NOP || op == NOVA_OP_EXTRAARG) {
            continue;
        }

        /* Skip CLOSURE and its pseudo-instructions.
         * The instructions following CLOSURE are upvalue descriptors
         * encoded as MOVE, NOT real MOVEs. Do not optimize them. */
        if (op == NOVA_OP_CLOSURE) {
            uint32_t bx = NOVA_GET_BX(code[pc]);
            if (bx < proto->proto_count) {
                pc += proto->protos[bx]->upvalue_count;
            }
            continue;
        }

        /* ---- P1: Redundant self-MOVE ---- */
        if (op == NOVA_OP_MOVE) {
            uint8_t a = NOVA_GET_A(code[pc]);
            uint8_t b = NOVA_GET_B(code[pc]);

            if (a == b) {
                novai_nopify(proto, pc);
                if (stats != NULL) {
                    stats->peephole_rewrites++;
                }
                continue;
            }

            /* ---- P2: MOVE + RETURN1 ---- */
            if (pc + 1 < count) {
                NovaOpcode next_op = NOVA_GET_OPCODE(code[pc + 1]);

                if (next_op == NOVA_OP_RETURN1) {
                    uint8_t ret_a = NOVA_GET_A(code[pc + 1]);
                    if (ret_a == a) {
                        /* Rewrite RETURN1 to use B directly */
                        code[pc + 1] = NOVA_ENCODE_ABC(
                            NOVA_OP_RETURN1, b, 0, 0);
                        novai_nopify(proto, pc);
                        if (stats != NULL) {
                            stats->peephole_rewrites++;
                        }
                        continue;
                    }
                }

                /* ---- P3: MOVE + RETURN ---- */
                if (next_op == NOVA_OP_RETURN) {
                    uint8_t ret_a = NOVA_GET_A(code[pc + 1]);
                    if (ret_a == a) {
                        uint8_t ret_b = NOVA_GET_B(code[pc + 1]);
                        uint8_t ret_c = NOVA_GET_C(code[pc + 1]);
                        code[pc + 1] = NOVA_ENCODE_ABC(
                            NOVA_OP_RETURN, b, ret_b, ret_c);
                        novai_nopify(proto, pc);
                        if (stats != NULL) {
                            stats->peephole_rewrites++;
                        }
                        continue;
                    }
                }

                /* ---- P6: MOVE then overwrite ----
                 * MOVE R(A), R(B) followed by instruction that sets R(A)
                 * without reading it. NOP the MOVE.
                 *
                 * Only safe if next instruction sets A (setsflag) and
                 * doesn't read A through its B or C fields.
                 *
                 * EXCEPTION: CALL, TAILCALL, and SELF read R(A) as
                 * the function to call before overwriting it with
                 * results. Don't NOP the MOVE in those cases.
                 */
                if (nova_opcode_info[(uint8_t)next_op].setsflag != 0
                    && next_op != NOVA_OP_CALL
                    && next_op != NOVA_OP_TAILCALL
                    && next_op != NOVA_OP_SELF
                    && next_op != NOVA_OP_TFORCALL
                    && next_op != NOVA_OP_CONCAT
                    && next_op != NOVA_OP_SETLIST) {
                    uint8_t next_a = NOVA_GET_A(code[pc + 1]);
                    if (next_a == a) {
                        /* Check that next instr doesn't READ register A */
                        uint8_t next_b = NOVA_GET_B(code[pc + 1]);
                        uint8_t next_c = NOVA_GET_C(code[pc + 1]);
                        int reads_a = 0;

                        /* Check B field if it's a register mode */
                        NovaArgMode bmode =
                            nova_opcode_info[(uint8_t)next_op].arg_b;
                        if ((bmode == NOVA_ARGMODE_REG ||
                             bmode == NOVA_ARGMODE_RK) && next_b == a) {
                            reads_a = 1;
                        }

                        /* Check C field if it's a register mode */
                        NovaArgMode cmode =
                            nova_opcode_info[(uint8_t)next_op].arg_c;
                        if ((cmode == NOVA_ARGMODE_REG ||
                             cmode == NOVA_ARGMODE_RK) && next_c == a) {
                            reads_a = 1;
                        }

                        if (reads_a == 0) {
                            novai_nopify(proto, pc);
                            if (stats != NULL) {
                                stats->peephole_rewrites++;
                            }
                            continue;
                        }
                    }
                }
            }

            continue;  /* Done with MOVE patterns */
        }

        /* ---- P4: Double NOT ---- */
        if (op == NOVA_OP_NOT && pc + 1 < count) {
            uint8_t a1 = NOVA_GET_A(code[pc]);
            uint8_t b1 = NOVA_GET_B(code[pc]);
            NovaOpcode next_op = NOVA_GET_OPCODE(code[pc + 1]);

            if (next_op == NOVA_OP_NOT) {
                uint8_t a2 = NOVA_GET_A(code[pc + 1]);
                uint8_t b2 = NOVA_GET_B(code[pc + 1]);

                /* NOT R(X), R(Y) followed by NOT R(Y), R(X)
                 * => the value is restored: NOP both */
                if (b2 == a1 && a2 == b1) {
                    novai_nopify(proto, pc);
                    novai_nopify(proto, pc + 1);
                    if (stats != NULL) {
                        stats->peephole_rewrites += 2;
                    }
                    pc++;  /* Skip the second NOT */
                    continue;
                }
            }
        }

        /* ---- P5: Redundant LOADNIL ---- */
        if (op == NOVA_OP_LOADNIL && pc + 1 < count) {
            NovaOpcode next_op = NOVA_GET_OPCODE(code[pc + 1]);
            if (next_op == NOVA_OP_LOADNIL) {
                uint8_t a1 = NOVA_GET_A(code[pc]);
                uint8_t b1 = NOVA_GET_B(code[pc]);
                uint8_t a2 = NOVA_GET_A(code[pc + 1]);
                uint8_t b2 = NOVA_GET_B(code[pc + 1]);

                /* If second LOADNIL covers same start and wider range,
                 * first is redundant */
                if (a2 == a1 && b2 >= b1) {
                    novai_nopify(proto, pc);
                    if (stats != NULL) {
                        stats->peephole_rewrites++;
                    }
                    continue;
                }
            }
        }
    }
}

/* ============================================================
 * PART 3: CONSTANT FOLDING PASS
 *
 * Look for arithmetic on RK constants and evaluate at
 * compile-time, replacing with LOADK or LOADINT.
 * ============================================================ */

/**
 * @brief Run constant folding pass.
 *
 * Folds:
 *   CF1: Binary arith with two RK constants -> LOADK/LOADINT
 *   CF2: UNM on constant -> LOADK negated
 *   CF3: BNOT on constant integer -> LOADK inverted
 *
 * @param proto  Proto to optimize
 * @param stats  Statistics output (may be NULL)
 */
static void novai_pass_const_fold(NovaProto *proto, NovaOptStats *stats) {
    if (proto == NULL || proto->code_count < 1) {
        return;
    }

    NovaInstruction *code = proto->code;
    uint32_t count = proto->code_count;

    for (uint32_t pc = 0; pc < count; pc++) {
        NovaOpcode op = NOVA_GET_OPCODE(code[pc]);

        /* ---- CF1: Binary arithmetic with two RK constants ----
         * ADD/SUB/MUL/IDIV/MOD R(A), RK(B), RK(C)
         * where both B and C are constants
         */
        if (op >= NOVA_OP_ADD && op <= NOVA_OP_MOD) {
            uint8_t a = NOVA_GET_A(code[pc]);
            uint8_t b = NOVA_GET_B(code[pc]);
            uint8_t c = NOVA_GET_C(code[pc]);

            /* Try integer folding first */
            nova_int_t ival_b = 0;
            nova_int_t ival_c = 0;

            if (novai_get_rk_integer(proto, b, &ival_b) &&
                novai_get_rk_integer(proto, c, &ival_c)) {

                nova_int_t result = 0;
                int valid = 1;

                switch (op) {
                    case NOVA_OP_ADD:
                        result = ival_b + ival_c;
                        break;
                    case NOVA_OP_SUB:
                        result = ival_b - ival_c;
                        break;
                    case NOVA_OP_MUL:
                        result = ival_b * ival_c;
                        break;
                    case NOVA_OP_IDIV:
                        if (ival_c == 0) {
                            valid = 0;  /* Division by zero: don't fold */
                        } else {
                            result = ival_b / ival_c;
                        }
                        break;
                    case NOVA_OP_MOD:
                        if (ival_c == 0) {
                            valid = 0;  /* Mod by zero: don't fold */
                        } else {
                            result = ival_b % ival_c;
                        }
                        break;
                    default:
                        valid = 0;
                        break;
                }

                if (valid != 0) {
                    /* Check if result fits in LOADINT's sBx range */
                    if (result >= NOVA_MIN_SBX && result <= NOVA_MAX_SBX) {
                        code[pc] = NOVA_ENCODE_ASBX(
                            NOVA_OP_LOADINT, a, (int)result);
                    } else {
                        /* Add to constant pool and use LOADK */
                        uint32_t kidx =
                            nova_proto_add_integer(proto, result);
                        if (kidx != UINT32_MAX && kidx <= NOVA_MAX_BX) {
                            code[pc] = NOVA_ENCODE_ABX(
                                NOVA_OP_LOADK, a, (uint16_t)kidx);
                        } else {
                            continue;  /* Pool full, skip */
                        }
                    }
                    if (stats != NULL) {
                        stats->constants_folded++;
                    }
                    continue;
                }
            }

            /* Try number (float) folding */
            nova_number_t nval_b = 0.0;
            nova_number_t nval_c = 0.0;

            if (novai_get_rk_number(proto, b, &nval_b) &&
                novai_get_rk_number(proto, c, &nval_c)) {

                nova_number_t result = 0.0;
                int valid = 1;

                switch (op) {
                    case NOVA_OP_ADD:
                        result = nval_b + nval_c;
                        break;
                    case NOVA_OP_SUB:
                        result = nval_b - nval_c;
                        break;
                    case NOVA_OP_MUL:
                        result = nval_b * nval_c;
                        break;
                    case NOVA_OP_DIV:
                        if (nval_c == 0.0) {
                            valid = 0;
                        } else {
                            result = nval_b / nval_c;
                        }
                        break;
                    default:
                        valid = 0;
                        break;
                }

                if (valid != 0) {
                    uint32_t kidx = nova_proto_add_number(proto, result);
                    if (kidx != UINT32_MAX && kidx <= NOVA_MAX_BX) {
                        code[pc] = NOVA_ENCODE_ABX(
                            NOVA_OP_LOADK, a, (uint16_t)kidx);
                        if (stats != NULL) {
                            stats->constants_folded++;
                        }
                    }
                }
            }

            continue;
        }

        /* ---- CF1b: Constant-key arithmetic (ADDK/SUBK/MULK/DIVK/MODK) ----
         * R(A) = R(B) op K(C)
         * We can only fold if B is also a constant load immediately prior.
         * This is harder to detect safely, so skip for now.
         * (Future: data-flow analysis)
         */

        /* ---- CF2: Unary minus on constant ---- */
        if (op == NOVA_OP_UNM && pc > 0) {
            uint8_t a = NOVA_GET_A(code[pc]);
            uint8_t b = NOVA_GET_B(code[pc]);

            /* Check if previous instruction loads a constant into R(B) */
            NovaOpcode prev_op = NOVA_GET_OPCODE(code[pc - 1]);

            if (prev_op == NOVA_OP_LOADINT && NOVA_GET_A(code[pc - 1]) == b) {
                /* LOADINT R(B), sBx; UNM R(A), R(B) */
                int prev_sbx = NOVA_GET_SBX(code[pc - 1]);
                int negated = -prev_sbx;

                if (negated >= NOVA_MIN_SBX && negated <= NOVA_MAX_SBX) {
                    code[pc - 1] = NOVA_ENCODE_ASBX(
                        NOVA_OP_LOADINT, a, negated);
                    novai_nopify(proto, pc);
                    if (stats != NULL) {
                        stats->constants_folded++;
                    }
                    continue;
                }
            }

            if (prev_op == NOVA_OP_LOADK && NOVA_GET_A(code[pc - 1]) == b) {
                /* LOADK R(B), K(Bx); UNM R(A), R(B) */
                uint16_t kidx = NOVA_GET_BX(code[pc - 1]);
                if (kidx < proto->const_count) {
                    NovaConstant *k = &proto->constants[kidx];

                    if (k->tag == NOVA_CONST_INTEGER) {
                        nova_int_t neg = -k->as.integer;
                        uint32_t new_kidx =
                            nova_proto_add_integer(proto, neg);
                        if (new_kidx != UINT32_MAX &&
                            new_kidx <= NOVA_MAX_BX) {
                            code[pc - 1] = NOVA_ENCODE_ABX(
                                NOVA_OP_LOADK, a, (uint16_t)new_kidx);
                            novai_nopify(proto, pc);
                            if (stats != NULL) {
                                stats->constants_folded++;
                            }
                            continue;
                        }
                    } else if (k->tag == NOVA_CONST_NUMBER) {
                        nova_number_t neg = -k->as.number;
                        uint32_t new_kidx =
                            nova_proto_add_number(proto, neg);
                        if (new_kidx != UINT32_MAX &&
                            new_kidx <= NOVA_MAX_BX) {
                            code[pc - 1] = NOVA_ENCODE_ABX(
                                NOVA_OP_LOADK, a, (uint16_t)new_kidx);
                            novai_nopify(proto, pc);
                            if (stats != NULL) {
                                stats->constants_folded++;
                            }
                            continue;
                        }
                    }
                }
            }
        }

        /* ---- CF3: Bitwise NOT on constant integer ---- */
        if (op == NOVA_OP_BNOT && pc > 0) {
            uint8_t a = NOVA_GET_A(code[pc]);
            uint8_t b = NOVA_GET_B(code[pc]);

            NovaOpcode prev_op = NOVA_GET_OPCODE(code[pc - 1]);

            if (prev_op == NOVA_OP_LOADINT && NOVA_GET_A(code[pc - 1]) == b) {
                int prev_sbx = NOVA_GET_SBX(code[pc - 1]);
                int inverted = ~prev_sbx;

                if (inverted >= NOVA_MIN_SBX && inverted <= NOVA_MAX_SBX) {
                    code[pc - 1] = NOVA_ENCODE_ASBX(
                        NOVA_OP_LOADINT, a, inverted);
                    novai_nopify(proto, pc);
                    if (stats != NULL) {
                        stats->constants_folded++;
                    }
                    continue;
                }
            }
        }
    }
}

/* ============================================================
 * PART 4: JUMP OPTIMIZATION PASS
 *
 * Collapse jump chains, remove jump-to-next, and clean up
 * dead comparison+JMP pairs.
 * ============================================================ */

/**
 * @brief Run jump optimization pass.
 *
 * Patterns:
 *   J1: Jump chain collapse (JMP -> JMP -> ... -> target)
 *   J2: Jump to next instruction (JMP +0 with A==0 -> NOP)
 *   J3: Comparison + JMP-to-next (both dead -> NOP both)
 *   J4: Jump targeting NOP chain (skip NOPs)
 *
 * @param proto  Proto to optimize
 * @param stats  Statistics output (may be NULL)
 */
static void novai_pass_jump_opt(NovaProto *proto, NovaOptStats *stats) {
    if (proto == NULL || proto->code_count < 1) {
        return;
    }

    NovaInstruction *code = proto->code;
    uint32_t count = proto->code_count;

    for (uint32_t pc = 0; pc < count; pc++) {
        NovaOpcode op = NOVA_GET_OPCODE(code[pc]);

        if (op != NOVA_OP_JMP) {
            continue;
        }

        uint8_t a = NOVA_GET_A(code[pc]);
        int sbx = NOVA_GET_SBX(code[pc]);

        /* ---- J2: Jump to next instruction ---- */
        if (sbx == 0 && a == 0) {
            /* JMP +0 with no upvalue close: completely dead */
            novai_nopify(proto, pc);
            if (stats != NULL) {
                stats->jumps_removed++;
            }

            /* ---- J3: Check if previous is a comparison ----
             * The comparison skips this JMP on test failure.
             * If the JMP is gone, the comparison is dead too.
             */
            if (pc > 0) {
                NovaOpcode prev_op = NOVA_GET_OPCODE(code[pc - 1]);
                if (nova_opcode_info[(uint8_t)prev_op].testflag != 0) {
                    novai_nopify(proto, pc - 1);
                    if (stats != NULL) {
                        stats->jumps_removed++;
                    }
                }
            }
            continue;
        }

        /* Skip JMPs with upvalue close (A > 0) -- these have
         * side effects even if they jump to next instruction */
        if (a != 0) {
            continue;
        }

        /* ---- J1 + J4: Chain collapse and NOP skip ---- */
        uint32_t target = novai_get_jmp_target(proto, pc);
        if (target == UINT32_MAX) {
            continue;
        }

        int hops = 0;
        uint32_t final_target = target;

        while (hops < NOVAI_MAX_CHAIN_HOPS && final_target < count) {
            NovaInstruction target_instr = code[final_target];
            NovaOpcode target_op = NOVA_GET_OPCODE(target_instr);

            /* Skip over NOPs */
            if (target_op == NOVA_OP_NOP) {
                final_target++;
                hops++;
                continue;
            }

            /* Follow JMP chain (only if no upvalue close) */
            if (target_op == NOVA_OP_JMP && NOVA_GET_A(target_instr) == 0) {
                uint32_t next_target = novai_get_jmp_target(
                    proto, final_target);
                if (next_target == UINT32_MAX) {
                    break;
                }
                final_target = next_target;
                hops++;
                continue;
            }

            break;  /* Reached a real instruction */
        }

        /* Retarget if changed */
        if (final_target != target && final_target < count) {
            int new_sbx = (int)final_target - (int)(pc + 1);
            if (new_sbx >= NOVA_MIN_SBX && new_sbx <= NOVA_MAX_SBX) {
                code[pc] = NOVA_ENCODE_ASBX(NOVA_OP_JMP, a, new_sbx);
                if (stats != NULL) {
                    stats->jumps_shortened++;
                }
            }
        }

        /* Check if after retargeting, we now jump to next */
        int final_sbx = NOVA_GET_SBX(code[pc]);
        if (final_sbx == 0 && a == 0) {
            novai_nopify(proto, pc);
            if (stats != NULL) {
                stats->jumps_removed++;
            }
        }
    }
}

/* ============================================================
 * PART 5: TAIL CALL DETECTION PASS
 *
 * Convert CALL + RETURN sequences into TAILCALL.
 * ============================================================ */

/**
 * @brief Run tail call detection pass.
 *
 * Pattern: CALL R(A), B, C followed by RETURN/RETURN1 from R(A)
 * becomes TAILCALL R(A), B, 0.
 *
 * @param proto  Proto to optimize
 * @param stats  Statistics output (may be NULL)
 */
static void novai_pass_tailcall(NovaProto *proto, NovaOptStats *stats) {
    if (proto == NULL || proto->code_count < 2) {
        return;
    }

    NovaInstruction *code = proto->code;
    uint32_t count = proto->code_count;

    for (uint32_t pc = 0; pc + 1 < count; pc++) {
        NovaOpcode op = NOVA_GET_OPCODE(code[pc]);

        if (op != NOVA_OP_CALL) {
            continue;
        }

        uint8_t call_a = NOVA_GET_A(code[pc]);
        uint8_t call_b = NOVA_GET_B(code[pc]);

        NovaOpcode next_op = NOVA_GET_OPCODE(code[pc + 1]);

        /* CALL R(A), B, C + RETURN1 R(A) */
        if (next_op == NOVA_OP_RETURN1) {
            uint8_t ret_a = NOVA_GET_A(code[pc + 1]);
            if (ret_a == call_a) {
                /* Convert to TAILCALL R(A), B, 0 */
                code[pc] = NOVA_ENCODE_ABC(
                    NOVA_OP_TAILCALL, call_a, call_b, 0);
                /* Keep the RETURN1 as a safety net for the VM,
                 * but it's effectively dead after TAILCALL.
                 * Some VMs expect it. We'll NOP it. */
                novai_nopify(proto, pc + 1);
                if (stats != NULL) {
                    stats->tailcalls_detected++;
                }
                pc++;  /* Skip past the NOPed return */
                continue;
            }
        }

        /* CALL R(A), B, C + RETURN R(A), D */
        if (next_op == NOVA_OP_RETURN) {
            uint8_t ret_a = NOVA_GET_A(code[pc + 1]);
            if (ret_a == call_a) {
                /* Convert to TAILCALL R(A), B, 0 */
                code[pc] = NOVA_ENCODE_ABC(
                    NOVA_OP_TAILCALL, call_a, call_b, 0);
                novai_nopify(proto, pc + 1);
                if (stats != NULL) {
                    stats->tailcalls_detected++;
                }
                pc++;
                continue;
            }
        }
    }
}

/* ============================================================
 * PART 6: DEAD CODE ELIMINATION PASS
 *
 * Uses reachability bitmap to NOP unreachable instructions.
 * ============================================================ */

/**
 * @brief Run dead code elimination pass.
 *
 * Builds a reachability bitmap from pc=0 using a worklist
 * algorithm, then NOPs all unreachable instructions.
 *
 * @param proto  Proto to optimize
 * @param stats  Statistics output (may be NULL)
 */
static void novai_pass_dead_code(NovaProto *proto, NovaOptStats *stats) {
    if (proto == NULL || proto->code_count < 2) {
        return;
    }

    uint8_t *reachable = novai_build_reachable(proto);
    if (reachable == NULL) {
        return;  /* Allocation failed, skip pass */
    }

    uint32_t count = proto->code_count;
    for (uint32_t pc = 0; pc < count; pc++) {
        if (reachable[pc] == 0 && !novai_is_nop(proto->code[pc])) {
            /* Don't NOP EXTRAARGs whose parent is reachable */
            if (novai_is_extraarg(proto->code[pc]) && pc > 0 &&
                reachable[pc - 1] != 0) {
                continue;
            }

            novai_nopify(proto, pc);
            if (stats != NULL) {
                stats->dead_instructions++;
            }
        }
    }

    free(reachable);
}

/* ============================================================
 * PART 7: RETURN SPECIALIZATION PASS
 *
 * Convert generic RETURN to RETURN0/RETURN1 where applicable.
 * ============================================================ */

/**
 * @brief Run return specialization pass.
 *
 * Patterns:
 *   R1: RETURN R(A), 1, 0 -> RETURN0 (0 return values)
 *   R2: RETURN R(A), 2, 0 -> RETURN1 R(A) (1 return value)
 *
 * @param proto  Proto to optimize
 * @param stats  Statistics output (may be NULL)
 */
static void novai_pass_return_spec(NovaProto *proto, NovaOptStats *stats) {
    if (proto == NULL || proto->code_count < 1) {
        return;
    }

    NovaInstruction *code = proto->code;
    uint32_t count = proto->code_count;

    for (uint32_t pc = 0; pc < count; pc++) {
        NovaOpcode op = NOVA_GET_OPCODE(code[pc]);

        if (op != NOVA_OP_RETURN) {
            continue;
        }

        uint8_t a = NOVA_GET_A(code[pc]);
        uint8_t b = NOVA_GET_B(code[pc]);

        /* R1: RETURN R(A), 1 means 0 return values -> RETURN0 */
        if (b == 1) {
            code[pc] = NOVA_ENCODE_ABC(NOVA_OP_RETURN0, 0, 0, 0);
            if (stats != NULL) {
                stats->returns_specialized++;
            }
            continue;
        }

        /* R2: RETURN R(A), 2 means 1 return value -> RETURN1 R(A) */
        if (b == 2) {
            code[pc] = NOVA_ENCODE_ABC(NOVA_OP_RETURN1, a, 0, 0);
            if (stats != NULL) {
                stats->returns_specialized++;
            }
            continue;
        }

        /* b == 0 means "return all from A to top of stack" -- can't
         * specialize without knowing the stack state at runtime */
        (void)a;
    }
}

/* ============================================================
 * PART 8: NOP SQUEEZE (COMPACTION)
 *
 * Remove all NOPs from the instruction stream and adjust
 * all jump targets to account for the compaction.
 * ============================================================ */

/**
 * @brief Build a pc remapping table for NOP removal.
 *
 * remap[old_pc] = new_pc after removing NOPs.
 * For NOP instructions, remap points to where the next non-NOP
 * instruction will land (used for retargeting jumps that aim at NOPs).
 *
 * @param code       Instruction array
 * @param count      Number of instructions
 * @param remap_out  Output: remap array (must be pre-allocated, count entries)
 * @return Number of non-NOP instructions (new code_count)
 */
static uint32_t novai_build_remap(const NovaInstruction *code,
                                  uint32_t count, uint32_t *remap_out) {
    uint32_t write = 0;

    for (uint32_t pc = 0; pc < count; pc++) {
        remap_out[pc] = write;
        if (!novai_is_nop(code[pc])) {
            write++;
        }
    }

    return write;
}

/**
 * @brief Run NOP squeeze pass.
 *
 * Removes all NOPs and adjusts jump targets. Also compacts
 * the line number array in lockstep.
 *
 * @param proto  Proto to optimize
 * @param stats  Statistics output (may be NULL)
 */
static void novai_pass_squeeze(NovaProto *proto, NovaOptStats *stats) {
    if (proto == NULL || proto->code_count < 1) {
        return;
    }

    uint32_t count = proto->code_count;
    NovaInstruction *code = proto->code;

    /* Count NOPs first -- if none, skip entirely */
    uint32_t nop_count = 0;
    for (uint32_t pc = 0; pc < count; pc++) {
        if (novai_is_nop(code[pc])) {
            nop_count++;
        }
    }

    if (nop_count == 0) {
        return;  /* Nothing to squeeze */
    }

    /* Build remap table */
    uint32_t *remap = (uint32_t *)malloc(count * sizeof(uint32_t));
    if (remap == NULL) {
        return;  /* Allocation failed, skip */
    }

    uint32_t new_count = novai_build_remap(code, count, remap);

    /* Pass 1: Adjust all jump targets using remap */
    for (uint32_t pc = 0; pc < count; pc++) {
        if (novai_is_nop(code[pc])) {
            continue;
        }

        NovaOpcode op = NOVA_GET_OPCODE(code[pc]);

        /* Instructions with sBx jump offsets */
        if (op == NOVA_OP_JMP || op == NOVA_OP_FORPREP ||
            op == NOVA_OP_FORLOOP || op == NOVA_OP_TFORLOOP) {

            int sbx = NOVA_GET_SBX(code[pc]);
            int old_target = (int)pc + 1 + sbx;

            /* Bounds check */
            if (old_target >= 0 && (uint32_t)old_target < count) {
                uint32_t new_target = remap[(uint32_t)old_target];
                uint32_t new_pc = remap[pc];
                int new_sbx = (int)new_target - (int)(new_pc + 1);

                if (new_sbx >= NOVA_MIN_SBX && new_sbx <= NOVA_MAX_SBX) {
                    uint8_t a = NOVA_GET_A(code[pc]);
                    code[pc] = NOVA_ENCODE_ASBX(op, a, new_sbx);
                }
            }
        }
    }

    /* Pass 2: Compact instructions and line numbers */
    uint32_t *line_numbers = proto->lines.line_numbers;
    uint32_t write = 0;

    for (uint32_t pc = 0; pc < count; pc++) {
        if (!novai_is_nop(code[pc])) {
            code[write] = code[pc];
            if (line_numbers != NULL && pc < proto->lines.count) {
                line_numbers[write] = line_numbers[pc];
            }
            write++;
        }
    }

    /* Update counts */
    proto->code_count = new_count;
    proto->lines.count = new_count;

    if (stats != NULL) {
        stats->nops_removed += nop_count;
    }

    free(remap);
}

/* ============================================================
 * PART 9: PUBLIC API
 * ============================================================ */

/**
 * @brief Optimize a compiled function prototype in-place.
 *
 * Runs optimization passes on the proto's bytecode according
 * to the specified optimization level. Recurses into all
 * sub-prototypes.
 *
 * @param proto  Compiled prototype to optimize (must not be NULL)
 * @param level  Optimization level (NOVA_OPT_NONE .. NOVA_OPT_AGGRESSIVE)
 *
 * @pre proto != NULL
 *
 * COMPLEXITY: O(n) per pass where n = instruction count
 * THREAD SAFETY: Not thread-safe
 */
void nova_optimize(NovaProto *proto, int level) {
    nova_optimize_stats(proto, level, NULL);
}

/**
 * @brief Optimize with statistics collection.
 *
 * Same as nova_optimize(), but also fills in the stats struct
 * with counters for each optimization performed. Stats are
 * accumulated across all sub-prototypes.
 *
 * @param proto  Compiled prototype to optimize (must not be NULL)
 * @param level  Optimization level
 * @param stats  Output statistics (NULL-safe; if NULL, stats discarded)
 *
 * @pre proto != NULL
 *
 * COMPLEXITY: O(n) per pass
 * THREAD SAFETY: Not thread-safe
 */
void nova_optimize_stats(NovaProto *proto, int level, NovaOptStats *stats) {
    NovaOptStats local_stats;
    uint32_t i = 0;

    if (proto == NULL) {
        return;
    }

    if (level <= NOVA_OPT_NONE) {
        return;
    }

    if (proto->code == NULL || proto->code_count == 0) {
        return;
    }

    memset(&local_stats, 0, sizeof(local_stats));
    local_stats.total_before = proto->code_count;

    /* ---- Level 1+: Basic passes ---- */
    novai_pass_peephole(proto, &local_stats);
    novai_pass_jump_opt(proto, &local_stats);
    novai_pass_return_spec(proto, &local_stats);

    /* ---- Level 2+: Full optimization ---- */
    if (level >= NOVA_OPT_FULL) {
        novai_pass_const_fold(proto, &local_stats);
        novai_pass_tailcall(proto, &local_stats);
        novai_pass_dead_code(proto, &local_stats);

        /* Second peephole + jump pass: earlier passes may create
         * new optimization opportunities (e.g. const fold creates
         * NOPs that make jumps shorter) */
        novai_pass_peephole(proto, &local_stats);
        novai_pass_jump_opt(proto, &local_stats);
    }

    /* ---- Always: compact NOPs ---- */
    novai_pass_squeeze(proto, &local_stats);

    local_stats.total_after = proto->code_count;

    /* ---- Recurse into sub-prototypes ---- */
    for (i = 0; i < proto->proto_count; i++) {
        if (proto->protos[i] != NULL) {
            nova_optimize_stats(proto->protos[i], level, &local_stats);
        }
    }

    /* ---- Accumulate into caller's stats ---- */
    if (stats != NULL) {
        stats->peephole_rewrites   += local_stats.peephole_rewrites;
        stats->constants_folded    += local_stats.constants_folded;
        stats->jumps_shortened     += local_stats.jumps_shortened;
        stats->jumps_removed       += local_stats.jumps_removed;
        stats->tailcalls_detected  += local_stats.tailcalls_detected;
        stats->dead_instructions   += local_stats.dead_instructions;
        stats->returns_specialized += local_stats.returns_specialized;
        stats->nops_removed        += local_stats.nops_removed;
        stats->total_before        += local_stats.total_before;
        stats->total_after         += local_stats.total_after;
    }
}

/**
 * @brief Print optimization statistics to stderr.
 *
 * Formats the stats struct as a human-readable report.
 *
 * @param stats  Statistics to print (must not be NULL)
 */
void nova_opt_dump_stats(const NovaOptStats *stats) {
    if (stats == NULL) {
        return;
    }

    fprintf(stderr, "=== Nova Optimizer Statistics ===\n");
    fprintf(stderr, "  Instructions before:  %u\n", stats->total_before);
    fprintf(stderr, "  Instructions after:   %u\n", stats->total_after);

    if (stats->total_before > 0) {
        uint32_t saved = stats->total_before - stats->total_after;
        double pct = 100.0 * (double)saved / (double)stats->total_before;
        fprintf(stderr, "  Reduction:            %u (%.1f%%)\n",
                saved, pct);
    }

    fprintf(stderr, "  ---\n");
    fprintf(stderr, "  Peephole rewrites:    %u\n",
            stats->peephole_rewrites);
    fprintf(stderr, "  Constants folded:     %u\n",
            stats->constants_folded);
    fprintf(stderr, "  Jumps shortened:      %u\n",
            stats->jumps_shortened);
    fprintf(stderr, "  Jumps removed:        %u\n",
            stats->jumps_removed);
    fprintf(stderr, "  Tail calls detected:  %u\n",
            stats->tailcalls_detected);
    fprintf(stderr, "  Dead instructions:    %u\n",
            stats->dead_instructions);
    fprintf(stderr, "  Returns specialized:  %u\n",
            stats->returns_specialized);
    fprintf(stderr, "  NOPs removed:         %u\n",
            stats->nops_removed);
    fprintf(stderr, "================================\n");
}
