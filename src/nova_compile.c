/**
 * @file nova_compile.c
 * @brief Nova Language - Compiler (Row AST -> Bytecode)
 *
 * Walks the row-based AST (NovaASTTable) and emits bytecode into
 * function prototypes (NovaProto). Built in 4 logical parts:
 *   Part 1: Foundation (scope, registers, jumps, discharge)
 *   Part 2: Expression compilation
 *   Part 3: Statement compilation
 *   Part 4: Public API
 *
 * See docs/NOVA_COMPILER_BLUEPRINT.md for the full design.
 *
 * @author Anthony Taliento
 * @date 2026-02-06
 * @version 0.2.0
 *
 * @copyright Copyright (c) 2026 Zorya Corporation
 * @license MIT
 *
 * ZORYA-C COMPLIANCE: v2.0.0
 *
 * DEPENDENCIES:
 *   - nova_compile.h (NovaCompiler, NovaFuncState, NovaExprDesc)
 *   - nova_proto.h   (NovaProto, emission API)
 *   - nova_opcode.h  (opcodes, encoding macros)
 *   - nova_ast_row.h (NovaASTTable, row AST access)
 *
 * THREAD SAFETY: Not thread-safe
 */

#include "nova/nova_compile.h"
#include "nova/nova_error.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ============================================================
 * PART 1: FOUNDATION
 *
 * Error reporting, AST access macro, register allocation,
 * scope management, local/upvalue resolution, jump lists,
 * and expression discharge helpers.
 * ============================================================ */

/* ---- Forward declarations for Parts 2-4 ---- */

static void novai_compile_expr(NovaFuncState *fs, NovaExprIdx idx,
                               NovaExprDesc *e);
static void novai_compile_stmt(NovaFuncState *fs, NovaStmtIdx idx);
static void novai_compile_block(NovaFuncState *fs, NovaBlockIdx idx);
static void novai_check_unresolved_gotos(NovaFuncState *fs);

/* ============================================================
 * AST ACCESS MACRO
 *
 * The compiler holds const NovaASTTable*, but the inline access
 * helpers (nova_get_expr etc.) take non-const. We only read,
 * so the const-cast is safe.
 * ============================================================ */

/**
 * PCM: AST
 * Purpose: Access the AST table from a FuncState without const warnings
 * Rationale: Called on every expression/statement compilation
 * Performance Impact: Single pointer dereference
 * Audit Date: 2026-02-06
 */
#define AST(fs) ((NovaASTTable *)(fs)->compiler->ast)

/** Shorthand for the current proto being built */
#define PROTO(fs) ((fs)->proto)

/** Shorthand to get source line from a NovaSourceLoc */
#define LOC_LINE(loc) ((uint32_t)(loc).line)

/* ============================================================
 * ERROR REPORTING
 * ============================================================ */

/**
 * @brief Infer the best error code from a compiler error message.
 *
 * Maps common compiler error message prefixes to specific
 * NovaErrorCode values for richer diagnostic output.
 */
static NovaErrorCode novai_infer_compile_code(const char *msg) {
    if (msg == NULL) { return NOVA_E0000; }
    if (strstr(msg, "too many registers"))       return NOVA_E1004;
    if (strstr(msg, "too many nested scopes"))   return NOVA_E1005;
    if (strstr(msg, "too many dec"))             return NOVA_E1003;
    if (strstr(msg, "too many nested functions")) return NOVA_E1006;
    if (strstr(msg, "invalid assignment"))       return NOVA_E1007;
    if (strstr(msg, "break") || strstr(msg, "continue"))
                                                  return NOVA_E1008;
    if (strstr(msg, "duplicate label"))          return NOVA_E1009;
    if (strstr(msg, "unresolved goto"))          return NOVA_E1010;
    if (strstr(msg, "too many arguments"))       return NOVA_E1011;
    if (strstr(msg, "too many pending goto"))    return NOVA_E1019;
    if (strstr(msg, "too many labels"))          return NOVA_E1018;
    if (strstr(msg, "constant overflow"))        return NOVA_E1013;
    if (strstr(msg, "out of memory"))            return NOVA_E1017;
    if (strstr(msg, "module name"))              return NOVA_E1020;
    return NOVA_E0000;
}

/**
 * @brief Report a compilation error.
 *
 * Sets the error flag and stores the message. Compilation
 * continues to catch more errors; the final nova_compile()
 * checks had_error before returning.
 *
 * @param fs    Current function state
 * @param line  Source line where error occurred
 * @param msg   Static error message string
 */
static void novai_error(NovaFuncState *fs, uint32_t line,
                        const char *msg) {
    if (fs == NULL || fs->compiler == NULL) {
        return;
    }
    NovaCompiler *c = fs->compiler;
    if (c->had_error == 0) {
        /* Only store the first error for now */
        c->had_error  = 1;
        c->error_msg  = msg;
        c->error_line = line;
        nova_diag_report(NOVA_DIAG_ERROR, novai_infer_compile_code(msg),
                         c->source, (int)line, 0,
                         "%s", msg);
    }
}

/* ============================================================
 * REGISTER ALLOCATION
 *
 * Linear bump allocator. Locals occupy low registers, temps
 * are allocated on top. free_reg is the high-water mark.
 * ============================================================ */

/**
 * @brief Allocate one temporary register.
 *
 * @param fs  Current function state
 * @return The allocated register index
 */
static uint8_t novai_alloc_reg(NovaFuncState *fs) {
    if (fs->free_reg >= NOVA_MAX_REGISTERS) {
        novai_error(fs, 0, "too many registers needed (>250)");
        return 0;
    }
    uint8_t reg = fs->free_reg;
    fs->free_reg++;
    if (fs->free_reg > PROTO(fs)->max_stack) {
        PROTO(fs)->max_stack = fs->free_reg;
    }
    return reg;
}

/**
 * @brief Set free_reg and update max_stack high-water mark.
 *
 * Use this instead of directly setting fs->free_reg to ensure
 * max_stack is always >= free_reg.
 *
 * @param fs    Current function state
 * @param reg   New value for free_reg
 */
static void novai_set_free_reg(NovaFuncState *fs, uint8_t reg) {
    fs->free_reg = reg;
    if (reg > PROTO(fs)->max_stack) {
        PROTO(fs)->max_stack = reg;
    }
}

/**
 * @brief Free the topmost temporary register.
 *
 * Only frees if reg is the last allocated temp (stack discipline).
 *
 * @param fs   Current function state
 * @param reg  Register to free
 */
static void novai_free_reg(NovaFuncState *fs, uint8_t reg) {
    if (reg >= fs->active_locals && reg == (uint8_t)(fs->free_reg - 1)) {
        fs->free_reg--;
    }
}

/**
 * @brief Reserve N consecutive registers above free_reg.
 *
 * @param fs     Current function state
 * @param count  Number of registers to reserve
 */
/* novai_reserve_regs removed — only used for old method-call
 * codegen which has been replaced by OP_SELF emission */

/* ============================================================
 * SCOPE MANAGEMENT
 * ============================================================ */

/**
 * @brief Enter a new lexical scope.
 *
 * @param fs       Current function state
 * @param is_loop  1 if this is a loop scope (for break/continue)
 */
static void novai_enter_scope(NovaFuncState *fs, int is_loop) {
    if (fs->scope_depth >= NOVA_MAX_SCOPE_DEPTH) {
        novai_error(fs, 0, "too many nested scopes");
        return;
    }
    NovaScope *scope = &fs->scopes[fs->scope_depth];
    scope->first_local    = fs->active_locals;
    scope->num_locals     = 0;
    scope->has_upvalues   = 0;
    scope->is_loop        = (uint8_t)(is_loop != 0);
    scope->break_list     = NOVA_NO_JUMP;
    scope->continue_list  = NOVA_NO_JUMP;
    fs->scope_depth++;
}

/**
 * @brief Leave the current lexical scope.
 *
 * Closes captured locals (CLOSE instruction), updates debug
 * info end_pc, and reclaims registers.
 *
 * @param fs  Current function state
 */
static void novai_leave_scope(NovaFuncState *fs) {
    if (fs->scope_depth == 0) {
        return;
    }
    fs->scope_depth--;
    NovaScope *scope = &fs->scopes[fs->scope_depth];

    uint32_t pc = nova_proto_pc(PROTO(fs));

    /* Emit CLOSE if any locals in this scope were captured */
    if (scope->has_upvalues) {
        nova_proto_emit_abc(PROTO(fs), NOVA_OP_CLOSE,
                            scope->first_local, 0, 0, pc);
    }

    /* Close debug info for locals declared in this scope
     * and mark them inactive for name resolution */
    while (fs->active_locals > scope->first_local) {
        fs->active_locals--;
        fs->locals[fs->active_locals].is_active = 0;
        uint8_t reg = fs->locals[fs->active_locals].reg;
        nova_proto_close_local(PROTO(fs), reg, pc);
    }

    /* Reclaim registers */
    fs->free_reg = fs->active_locals;
}

/* ============================================================
 * LOCAL VARIABLE MANAGEMENT
 * ============================================================ */

/**
 * @brief Declare a new local variable in the current scope.
 *
 * Allocates a register and adds debug info to the proto.
 *
 * @param fs    Current function state
 * @param name  Variable name (not owned, must outlive compilation)
 * @param len   Name length
 */
static void novai_add_local(NovaFuncState *fs, const char *name,
                            uint32_t len) {
    if (fs->active_locals >= NOVA_MAX_LOCALS) {
        novai_error(fs, 0, "too many dec variables");
        return;
    }

    uint8_t reg = novai_alloc_reg(fs);
    uint32_t pc = nova_proto_pc(PROTO(fs));

    /* Store at active_locals index (dense array, like Lua's nactvar).
     * When scopes close, active_locals goes back down and slots are
     * reused by new locals, keeping the array dense. */
    NovaCompLocal *loc = &fs->locals[fs->active_locals];
    loc->name        = name;
    loc->name_len    = len;
    loc->reg         = reg;
    loc->is_captured = 0;
    loc->is_const    = 0;
    loc->is_active   = 1;
    loc->start_pc    = pc;

    fs->local_count++;
    fs->active_locals++;

    /* Update scope's local count */
    if (fs->scope_depth > 0) {
        fs->scopes[fs->scope_depth - 1].num_locals++;
    }

    /* Add debug info to proto */
    nova_proto_add_local(PROTO(fs), name, reg, pc);
}

/**
 * @brief Resolve a name as a local variable.
 *
 * Searches active locals from most recent to oldest.
 *
 * @param fs    Current function state
 * @param name  Variable name to find
 * @param len   Name length
 *
 * @return Register index if found, -1 if not a local
 */
static int novai_resolve_local(NovaFuncState *fs, const char *name,
                               uint32_t len) {
    if (fs == NULL || name == NULL) {
        return -1;
    }

    /* Walk backwards through active locals (0..active_locals-1).
     * The array is dense: slots are reused when scopes close. */
    for (int i = (int)fs->active_locals - 1; i >= 0; i--) {
        const NovaCompLocal *loc = &fs->locals[i];
        if (loc->name_len == len
            && memcmp(loc->name, name, (size_t)len) == 0) {
            return (int)loc->reg;
        }
    }
    return -1;
}

/**
 * @brief Resolve a name as an upvalue (captured from outer scope).
 *
 * Recursively walks parent FuncStates looking for the variable
 * as a local or upvalue, then creates the upvalue chain.
 *
 * @param fs    Current function state
 * @param name  Variable name to find
 * @param len   Name length
 *
 * @return Upvalue index if found, -1 if not found in any enclosing scope
 */
static int novai_resolve_upvalue(NovaFuncState *fs, const char *name,
                                 uint32_t len) {
    if (fs->parent == NULL) {
        /* Top-level function: no enclosing scope to capture from */
        return -1;
    }

    /* Try as a local in the parent function */
    int parent_reg = novai_resolve_local(fs->parent, name, len);
    if (parent_reg >= 0) {
        /* Found as parent local -- mark as captured */
        for (int i = (int)fs->parent->active_locals - 1; i >= 0; i--) {
            if (fs->parent->locals[i].reg == (uint8_t)parent_reg
                && fs->parent->locals[i].is_active) {
                fs->parent->locals[i].is_captured = 1;
                break;
            }
        }
        /* Mark parent scope as having upvalues */
        if (fs->parent->scope_depth > 0) {
            fs->parent->scopes[fs->parent->scope_depth - 1].has_upvalues = 1;
        }
        /* Add upvalue descriptor to our proto: captured from parent stack */
        uint8_t uv = nova_proto_add_upvalue(
            PROTO(fs), (uint8_t)parent_reg, 1, name);
        return (uv == 255) ? -1 : (int)uv;
    }

    /* Try as an upvalue in the parent function (recursive) */
    int parent_uv = novai_resolve_upvalue(fs->parent, name, len);
    if (parent_uv >= 0) {
        /* Found as parent upvalue -- chain it */
        uint8_t uv = nova_proto_add_upvalue(
            PROTO(fs), (uint8_t)parent_uv, 0, name);
        return (uv == 255) ? -1 : (int)uv;
    }

    return -1;
}

/* ============================================================
 * JUMP LIST MANAGEMENT
 *
 * Forward jumps are emitted with a placeholder offset. The
 * sBx field of pending JMP instructions forms a linked list
 * (each points to the previous pending jump). When the target
 * is known, we walk the list and patch all offsets.
 *
 * Sentinel: NOVA_NO_JUMP (UINT32_MAX) = end of list.
 * ============================================================ */

/**
 * @brief Emit a JMP instruction with a placeholder offset.
 *
 * @param fs    Current function state
 * @param line  Source line
 * @return PC of the emitted JMP (for later patching)
 */
static uint32_t novai_emit_jmp(NovaFuncState *fs, uint32_t line) {
    /* Use max positive sBx as placeholder (will be patched later) */
    return nova_proto_emit_asbx(PROTO(fs), NOVA_OP_JMP, 0,
                                NOVA_MAX_SBX, line);
}

/**
 * @brief Get the jump target stored in a JMP instruction's sBx.
 *
 * Returns NOVA_NO_JUMP if this is the end of the list.
 *
 * @param fs  Current function state
 * @param pc  JMP instruction index
 * @return The "next" link in the jump list, or NOVA_NO_JUMP
 */
static uint32_t novai_get_jmp_dest(NovaFuncState *fs, uint32_t pc) {
    int offset = NOVA_GET_SBX(PROTO(fs)->code[pc]);
    if (offset == NOVA_MAX_SBX) {
        return NOVA_NO_JUMP; /* Unpatched placeholder */
    }
    return (uint32_t)((int)pc + 1 + offset);
}

/**
 * @brief Patch a single JMP instruction to jump to target.
 *
 * @param fs      Current function state
 * @param pc      JMP instruction to patch
 * @param target  Target PC to jump to
 */
static void novai_patch_jmp(NovaFuncState *fs, uint32_t pc,
                            uint32_t target) {
    int offset = (int)target - (int)pc - 1;
    nova_proto_patch_sbx(PROTO(fs), pc, offset);
}

/**
 * @brief Patch an entire linked list of JMP instructions to target.
 *
 * Walks the list following the sBx "next" links.
 *
 * @param fs      Current function state
 * @param list    Head of the jump list (PC of first JMP)
 * @param target  Target PC for all jumps in the list
 */
static void novai_patch_list(NovaFuncState *fs, uint32_t list,
                             uint32_t target) {
    while (list != NOVA_NO_JUMP) {
        uint32_t next = novai_get_jmp_dest(fs, list);
        novai_patch_jmp(fs, list, target);
        list = next;
    }
}

/**
 * @brief Patch a jump list to jump to the current PC.
 *
 * @param fs    Current function state
 * @param list  Head of the jump list
 */
static void novai_patch_to_here(NovaFuncState *fs, uint32_t list) {
    novai_patch_list(fs, list, nova_proto_pc(PROTO(fs)));
}

/**
 * @brief Concatenate two jump lists.
 *
 * Appends l2 to the end of *l1. If *l1 is empty, sets *l1 = l2.
 *
 * @param fs  Current function state
 * @param l1  Pointer to the head of the first list (modified)
 * @param l2  Head of the second list to append
 */
static void novai_concat_jmp_list(NovaFuncState *fs, uint32_t *l1,
                                  uint32_t l2) {
    if (l2 == NOVA_NO_JUMP) {
        return;
    }
    if (*l1 == NOVA_NO_JUMP) {
        *l1 = l2;
        return;
    }

    /* Walk *l1 to find the tail */
    uint32_t cursor = *l1;
    uint32_t next = novai_get_jmp_dest(fs, cursor);
    while (next != NOVA_NO_JUMP) {
        cursor = next;
        next = novai_get_jmp_dest(fs, cursor);
    }

    /* Link tail to l2 */
    novai_patch_jmp(fs, cursor, l2);
}

/* ============================================================
 * EXPRESSION DISCHARGE
 *
 * Moves an expression descriptor's value into a concrete
 * register. This is the bridge between the lazy expression
 * tracking (NovaExprDesc) and actual register contents.
 * ============================================================ */

/**
 * @brief Free any temporary register used by an expression.
 *
 * @param fs  Current function state
 * @param e   Expression descriptor
 */
static void novai_expr_free(NovaFuncState *fs, NovaExprDesc *e) {
    if ((e->kind == NOVA_EK_REG || e->kind == NOVA_EK_CALL) &&
        e->u.reg >= fs->active_locals) {
        novai_free_reg(fs, e->u.reg);
    }
    /* Free the table AND key temp registers for indexed/field
     * expressions so that discharge_to_next can reuse them.
     *
     * For INDEXED: if the key was loaded into a temp register
     * (because the constant index exceeded the RK range), that
     * register must also be freed.  Otherwise it "leaks," pushing
     * subsequent register allocations one slot higher and causing
     * call arguments to end up in the wrong positions.
     *
     * Free key first (higher register) to maintain stack discipline
     * required by novai_free_reg(). */
    else if (e->kind == NOVA_EK_INDEXED) {
        /* Free key register if it's a temp (not an RK constant) */
        uint8_t key = (uint8_t)e->u.index.key;
        if (!NOVA_IS_RK_CONST(key) && key >= fs->active_locals) {
            novai_free_reg(fs, key);
        }
        /* Free table register if it's a temp */
        if (e->u.index.table >= fs->active_locals) {
            novai_free_reg(fs, e->u.index.table);
        }
    }
    else if (e->kind == NOVA_EK_FIELD &&
             e->u.index.table >= fs->active_locals) {
        novai_free_reg(fs, e->u.index.table);
    }
}

/**
 * @brief Discharge an expression into a specific register.
 *
 * Emits the necessary instruction(s) to put the expression's
 * value into register `reg`, then updates `e` to NOVA_EK_REG.
 *
 * @param fs   Current function state
 * @param e    Expression descriptor (modified in place)
 * @param reg  Target register
 */
static void novai_discharge_to_reg(NovaFuncState *fs, NovaExprDesc *e,
                                   uint8_t reg) {
    NovaProto *proto = PROTO(fs);
    uint32_t line = e->line;

    switch (e->kind) {
        case NOVA_EK_NIL:
            nova_proto_emit_abc(proto, NOVA_OP_LOADNIL,
                                reg, 0, 0, line);
            break;

        case NOVA_EK_TRUE:
            nova_proto_emit_abc(proto, NOVA_OP_LOADBOOL,
                                reg, 1, 0, line);
            break;

        case NOVA_EK_FALSE:
            nova_proto_emit_abc(proto, NOVA_OP_LOADBOOL,
                                reg, 0, 0, line);
            break;

        case NOVA_EK_INTEGER: {
            /* Check if value fits in signed 16-bit (sBx range) */
            nova_int_t val = e->u.integer;
            if (val >= NOVA_MIN_SBX && val <= NOVA_MAX_SBX) {
                nova_proto_emit_asbx(proto, NOVA_OP_LOADINT,
                                     reg, (int)val, line);
            } else {
                /* Doesn't fit: add to constant pool */
                uint32_t kidx = nova_proto_add_integer(proto, val);
                if (kidx <= NOVA_MAX_BX) {
                    nova_proto_emit_abx(proto, NOVA_OP_LOADK,
                                        reg, (uint16_t)kidx, line);
                } else {
                    novai_error(fs, line,
                                "too many constants (integer overflow)");
                }
            }
            break;
        }

        case NOVA_EK_NUMBER:
        case NOVA_EK_STRING:
        case NOVA_EK_CONST: {
            uint32_t kidx = e->u.const_idx;
            if (kidx <= NOVA_MAX_BX) {
                nova_proto_emit_abx(proto, NOVA_OP_LOADK,
                                    reg, (uint16_t)kidx, line);
            } else {
                novai_error(fs, line,
                            "too many constants (pool overflow)");
            }
            break;
        }

        case NOVA_EK_REG:
        case NOVA_EK_LOCAL:
            if (e->u.reg != reg) {
                nova_proto_emit_abc(proto, NOVA_OP_MOVE,
                                    reg, e->u.reg, 0, line);
            }
            break;

        case NOVA_EK_UPVAL:
            nova_proto_emit_abc(proto, NOVA_OP_GETUPVAL,
                                reg, e->u.upval, 0, line);
            break;

        case NOVA_EK_GLOBAL: {
            uint32_t kidx = e->u.const_idx;
            if (kidx <= NOVA_MAX_BX) {
                nova_proto_emit_abx(proto, NOVA_OP_GETGLOBAL,
                                    reg, (uint16_t)kidx, line);
            } else {
                novai_error(fs, line,
                            "too many constants (global name overflow)");
            }
            break;
        }

        case NOVA_EK_INDEXED:
            nova_proto_emit_abc(proto, NOVA_OP_GETTABLE,
                                reg, e->u.index.table,
                                (uint8_t)e->u.index.key, line);
            break;

        case NOVA_EK_FIELD:
            nova_proto_emit_abc(proto, NOVA_OP_GETFIELD,
                                reg, e->u.index.table,
                                (uint8_t)e->u.index.key, line);
            break;

        case NOVA_EK_CALL:
            /* Call result is in the register that was the CALL's A field.
             * If that's not our target, move it. */
            if (e->u.reg != reg) {
                nova_proto_emit_abc(proto, NOVA_OP_MOVE,
                                    reg, e->u.reg, 0, line);
            }
            break;

        case NOVA_EK_VARARG:
            /* VARARG reg, 2 → 1 result into reg */
            nova_proto_emit_abc(proto, NOVA_OP_VARARG,
                                reg, 2, 0, line);
            break;

        case NOVA_EK_RELOC:
            /* Patch the A field of the instruction at e->u.pc */
            nova_proto_patch_a(proto, e->u.pc, reg);
            break;

        case NOVA_EK_JMP:
            /* TODO: discharge boolean test to register.
             * For now, emit LOADBOOL with conditional skip.
             * true_list jumps get LOADBOOL reg, 1, 1
             * false_list jumps get LOADBOOL reg, 0, 0 */
            novai_patch_to_here(fs, e->u.jmp.false_list);
            nova_proto_emit_abc(proto, NOVA_OP_LOADBOOL,
                                reg, 0, 1, line);
            novai_patch_to_here(fs, e->u.jmp.true_list);
            nova_proto_emit_abc(proto, NOVA_OP_LOADBOOL,
                                reg, 1, 0, line);
            break;

        case NOVA_EK_VOID:
        default:
            /* No value to discharge */
            break;
    }

    e->kind  = NOVA_EK_REG;
    e->u.reg = reg;
}

/**
 * @brief Discharge expression to any register (reuse if possible).
 *
 * If expression is already in a register, does nothing.
 * Otherwise allocates a temp and discharges there.
 *
 * @param fs  Current function state
 * @param e   Expression descriptor (modified)
 */
static void novai_discharge_to_any(NovaFuncState *fs, NovaExprDesc *e) {
    if (e->kind == NOVA_EK_REG || e->kind == NOVA_EK_LOCAL) {
        return; /* Already in a register */
    }
    /* CALL result is already sitting in its register; promote to REG
     * instead of allocating a new temp and emitting a MOVE.  This
     * keeps the register stack compact so that subsequent stack-based
     * frees can properly unwind (e.g. binary op operand freeing). */
    if (e->kind == NOVA_EK_CALL) {
        e->kind = NOVA_EK_REG;
        return;
    }
    /* Free any temp occupied by compound expressions (INDEXED/FIELD)
     * so the subsequent alloc reuses the slot and the result register
     * stays compact.  Without this, the GETUPVAL register used to
     * load the table leaks, pushing subsequent registers higher and
     * corrupting return/call alignment. */
    novai_expr_free(fs, e);
    uint8_t reg = novai_alloc_reg(fs);
    novai_discharge_to_reg(fs, e, reg);
}

/**
 * @brief Discharge expression to the next free register.
 *
 * Always allocates a new temp register, even if expression
 * is already in one. This ensures consecutive register layout
 * for multi-value operations (call args, return values).
 *
 * @param fs  Current function state
 * @param e   Expression descriptor (modified)
 */
static void novai_discharge_to_next(NovaFuncState *fs, NovaExprDesc *e) {
    /* Free the expression's temp register first (if applicable).
     * This allows re-allocation of the same register, avoiding
     * unnecessary MOVE instructions when the expr is already at
     * the correct position (e.g., call results as call arguments). */
    novai_expr_free(fs, e);
    uint8_t target = novai_alloc_reg(fs);
    novai_discharge_to_reg(fs, e, target);
}

/* ============================================================
 * EXPRESSION STORE HELPERS
 *
 * These handle storing a value to a target expression
 * (for assignment statements).
 * ============================================================ */

/**
 * @brief Store a value register into an expression target (lvalue).
 *
 * Handles locals, upvalues, globals, table indices, and fields.
 *
 * @param fs       Current function state
 * @param target   Expression descriptor of the lvalue
 * @param val_reg  Register containing the value to store
 * @param line     Source line for error reporting
 */
static void novai_store_var(NovaFuncState *fs, NovaExprDesc *target,
                            uint8_t val_reg, uint32_t line) {
    NovaProto *proto = PROTO(fs);

    switch (target->kind) {
        case NOVA_EK_LOCAL:
            if (target->u.reg != val_reg) {
                nova_proto_emit_abc(proto, NOVA_OP_MOVE,
                                    target->u.reg, val_reg, 0, line);
            }
            break;

        case NOVA_EK_UPVAL:
            nova_proto_emit_abc(proto, NOVA_OP_SETUPVAL,
                                val_reg, target->u.upval, 0, line);
            break;

        case NOVA_EK_GLOBAL: {
            uint32_t kidx = target->u.const_idx;
            if (kidx <= NOVA_MAX_BX) {
                nova_proto_emit_abx(proto, NOVA_OP_SETGLOBAL,
                                    val_reg, (uint16_t)kidx, line);
            } else {
                novai_error(fs, line, "global name constant overflow");
            }
            break;
        }

        case NOVA_EK_INDEXED:
            nova_proto_emit_abc(proto, NOVA_OP_SETTABLE,
                                target->u.index.table,
                                (uint8_t)target->u.index.key,
                                val_reg, line);
            break;

        case NOVA_EK_FIELD:
            nova_proto_emit_abc(proto, NOVA_OP_SETFIELD,
                                target->u.index.table,
                                (uint8_t)target->u.index.key,
                                val_reg, line);
            break;

        default:
            novai_error(fs, line, "invalid assignment target");
            break;
    }
}

/* ============================================================
 * RK ENCODING HELPER
 *
 * Tries to encode a constant index as an RK value (for opcodes
 * that accept register-or-constant in B/C fields). If the
 * constant index fits in the RK range, returns the RK-encoded
 * value. Otherwise, loads the constant into a temp register
 * and returns that register index.
 * ============================================================ */

/**
 * @brief Try to encode an expression as an RK value.
 *
 * If the expression is a constant that fits in RK encoding,
 * returns the RK value directly. Otherwise discharges to a
 * register and returns that register.
 *
 * @param fs   Current function state
 * @param e    Expression descriptor (may be modified)
 * @param line Source line for emission
 * @return RK-encoded value (register or constant)
 */
static uint8_t novai_expr_to_rk(NovaFuncState *fs, NovaExprDesc *e,
                                uint32_t line) {
    (void)line;

    /* Constants that fit in RK range */
    if (e->kind == NOVA_EK_CONST || e->kind == NOVA_EK_STRING
        || e->kind == NOVA_EK_NUMBER) {
        if (e->u.const_idx <= NOVA_MAX_RK_CONST) {
            return (uint8_t)NOVA_CONST_TO_RK(e->u.const_idx);
        }
    }

    /* Integer literals: add to constant pool for RK encoding */
    if (e->kind == NOVA_EK_INTEGER) {
        uint32_t kidx = nova_proto_add_integer(PROTO(fs), e->u.integer);
        if (kidx <= NOVA_MAX_RK_CONST) {
            return (uint8_t)NOVA_CONST_TO_RK(kidx);
        }
    }

    /* Fall back: discharge to register */
    novai_discharge_to_any(fs, e);
    return e->u.reg;
}

/* ============================================================
 * FUNCSTATE INITIALIZATION
 *
 * Sets up a new FuncState for compiling a function body.
 * Called once for the top-level chunk and once per nested
 * function literal.
 * ============================================================ */

/**
 * @brief Initialize a FuncState for a new function.
 *
 * @param fs        FuncState to initialize (stack-allocated by caller)
 * @param proto     The proto being built
 * @param parent    Parent FuncState (NULL for top-level)
 * @param compiler  Top-level compiler state
 */
static void novai_init_funcstate(NovaFuncState *fs, NovaProto *proto,
                                 NovaFuncState *parent,
                                 NovaCompiler *compiler) {
    memset(fs, 0, sizeof(NovaFuncState));
    fs->proto          = proto;
    fs->parent         = parent;
    fs->compiler       = compiler;
    fs->free_reg       = 0;
    fs->active_locals  = 0;
    fs->local_count    = 0;
    fs->scope_depth    = 0;
    fs->last_target    = NOVA_NO_JUMP;
    fs->is_async       = 0;

    compiler->current_fs = fs;
}

/* ============================================================
 * END OF PART 1 -- FOUNDATION
 *
 * Parts 2-4 follow: expressions, statements, public API.
 * Each part was designed from docs/NOVA_COMPILER_BLUEPRINT.md.
 * ============================================================ */

/* ============================================================
 * PART 2: EXPRESSION COMPILATION
 * ============================================================ */

/** Map NovaBinOp → NovaOpcode for arithmetic/bitwise/string ops */
static NovaOpcode novai_binop_to_opcode(NovaBinOp op) {
    switch (op) {
        case NOVA_BINOP_ADD:    return NOVA_OP_ADD;
        case NOVA_BINOP_SUB:    return NOVA_OP_SUB;
        case NOVA_BINOP_MUL:    return NOVA_OP_MUL;
        case NOVA_BINOP_DIV:    return NOVA_OP_DIV;
        case NOVA_BINOP_IDIV:   return NOVA_OP_IDIV;
        case NOVA_BINOP_MOD:    return NOVA_OP_MOD;
        case NOVA_BINOP_POW:    return NOVA_OP_POW;
        case NOVA_BINOP_CONCAT: return NOVA_OP_CONCAT;
        case NOVA_BINOP_BAND:   return NOVA_OP_BAND;
        case NOVA_BINOP_BOR:    return NOVA_OP_BOR;
        case NOVA_BINOP_BXOR:   return NOVA_OP_BXOR;
        case NOVA_BINOP_SHL:    return NOVA_OP_SHL;
        case NOVA_BINOP_SHR:    return NOVA_OP_SHR;
        default:                return NOVA_OP_NOP;
    }
}

/** Map NovaBinOp → comparison NovaOpcode */
static NovaOpcode novai_cmpop_to_opcode(NovaBinOp op) {
    switch (op) {
        case NOVA_BINOP_EQ:  return NOVA_OP_EQ;
        case NOVA_BINOP_NEQ: return NOVA_OP_EQ;   /* negate A */
        case NOVA_BINOP_LT:  return NOVA_OP_LT;
        case NOVA_BINOP_LE:  return NOVA_OP_LE;
        case NOVA_BINOP_GT:  return NOVA_OP_LT;   /* swap B,C */
        case NOVA_BINOP_GE:  return NOVA_OP_LE;   /* swap B,C */
        default:             return NOVA_OP_NOP;
    }
}

/** Check if a binop is a comparison */
static int novai_is_comparison(NovaBinOp op) {
    return op >= NOVA_BINOP_EQ && op <= NOVA_BINOP_GE;
}

/** Check if a binop is logical (and/or) */
static int novai_is_logical(NovaBinOp op) {
    return op == NOVA_BINOP_AND || op == NOVA_BINOP_OR;
}

/** Map NovaUnOp → NovaOpcode */
static NovaOpcode novai_unop_to_opcode(NovaUnOp op) {
    switch (op) {
        case NOVA_UNOP_NEGATE: return NOVA_OP_UNM;
        case NOVA_UNOP_NOT:    return NOVA_OP_NOT;
        case NOVA_UNOP_LEN:    return NOVA_OP_STRLEN;
        case NOVA_UNOP_BNOT:   return NOVA_OP_BNOT;
        default:               return NOVA_OP_NOP;
    }
}

/* ---- Expression compilation: names ---- */

static void novai_compile_name(NovaFuncState *fs, NovaRowExpr *expr,
                               NovaExprDesc *e) {
    const char *name  = expr->as.string.data;
    uint32_t    len   = (uint32_t)expr->as.string.len;

    /* Try local */
    int reg = novai_resolve_local(fs, name, len);
    if (reg >= 0) {
        e->kind  = NOVA_EK_LOCAL;
        e->u.reg = (uint8_t)reg;
        return;
    }

    /* Try upvalue */
    int uv = novai_resolve_upvalue(fs, name, len);
    if (uv >= 0) {
        e->kind    = NOVA_EK_UPVAL;
        e->u.upval = (uint8_t)uv;
        return;
    }

    /* Global */
    uint32_t kidx = nova_proto_find_or_add_string(
        PROTO(fs), name, len);
    e->kind        = NOVA_EK_GLOBAL;
    e->u.const_idx = kidx;
}

/* ---- Expression compilation: unary ---- */

static void novai_compile_unary(NovaFuncState *fs, NovaRowExpr *expr,
                                NovaExprDesc *e) {
    NovaExprDesc operand;
    memset(&operand, 0, sizeof(operand));

    novai_compile_expr(fs, expr->as.unary.operand, &operand);
    novai_discharge_to_any(fs, &operand);

    NovaOpcode op = novai_unop_to_opcode(expr->as.unary.op);
    uint32_t pc = nova_proto_emit_abc(
        PROTO(fs), op, 0, operand.u.reg, 0,
        LOC_LINE(expr->loc));

    novai_expr_free(fs, &operand);

    e->kind = NOVA_EK_RELOC;
    e->u.pc = pc;
}

/* ---- Expression compilation: binary ---- */

static void novai_compile_binary(NovaFuncState *fs, NovaRowExpr *expr,
                                 NovaExprDesc *e) {
    NovaBinOp binop = expr->as.binary.op;
    uint32_t line = LOC_LINE(expr->loc);

    /* Handle logical operators (short-circuit) */
    if (novai_is_logical(binop)) {
        NovaExprDesc left;
        memset(&left, 0, sizeof(left));
        novai_compile_expr(fs, expr->as.binary.left, &left);
        novai_discharge_to_any(fs, &left);

        /* If the left operand is a local variable, the result must
         * go into a fresh temporary register.  Otherwise, the right
         * side's value (e.g. `not tbl[nb]`) is discharged directly
         * into the local's register, clobbering the variable so that
         * subsequent uses in the if-body see the boolean result
         * instead of the original value. */
        uint8_t result_reg;
        if (left.kind == NOVA_EK_LOCAL) {
            result_reg = novai_alloc_reg(fs);
        } else {
            result_reg = left.u.reg;
        }

        /* TESTSET result, left, condition
         * JMP → skip (if short-circuit taken) */
        uint8_t cond = (binop == NOVA_BINOP_AND) ? 0 : 1;
        nova_proto_emit_abc(PROTO(fs), NOVA_OP_TESTSET,
                            result_reg, left.u.reg, cond, line);
        uint32_t skip_jmp = novai_emit_jmp(fs, line);

        /* Compile right side into the result register */
        NovaExprDesc right;
        memset(&right, 0, sizeof(right));
        novai_compile_expr(fs, expr->as.binary.right, &right);
        novai_discharge_to_reg(fs, &right, result_reg);
        novai_expr_free(fs, &right);

        /* After freeing the right side's temp, the result register
         * must stay reserved.  novai_expr_free freed result_reg
         * because the right side was discharged there, but we still
         * need it as our output.  Without this, the caller's next
         * alloc will hand out result_reg again, clobbering the
         * and/or result (e.g. `(x and y or z) * 10` → 10*10). */
        if (fs->free_reg <= result_reg) {
            novai_set_free_reg(fs, (uint8_t)(result_reg + 1));
        }

        /* Patch: the skip goes to after the right side */
        novai_patch_to_here(fs, skip_jmp);

        e->kind  = NOVA_EK_REG;
        e->u.reg = result_reg;
        return;
    }

    /* Handle comparison operators (produce booleans via jumps) */
    if (novai_is_comparison(binop)) {
        NovaExprDesc left;
        memset(&left, 0, sizeof(left));
        novai_compile_expr(fs, expr->as.binary.left, &left);
        novai_discharge_to_any(fs, &left);

        NovaExprDesc right;
        memset(&right, 0, sizeof(right));
        novai_compile_expr(fs, expr->as.binary.right, &right);
        novai_discharge_to_any(fs, &right);

        NovaOpcode cmp_op = novai_cmpop_to_opcode(binop);
        uint8_t left_reg  = left.u.reg;
        uint8_t right_reg = right.u.reg;

        /* GT/GE: swap operands and use LT/LE */
        if (binop == NOVA_BINOP_GT || binop == NOVA_BINOP_GE) {
            uint8_t tmp = left_reg;
            left_reg  = right_reg;
            right_reg = tmp;
        }

        /* A field: 1 for normal, 0 for NEQ (inverted) */
        uint8_t a_cond = (binop == NOVA_BINOP_NEQ) ? 0 : 1;

        /* Emit: CMP a_cond, left_reg, right_reg */
        nova_proto_emit_abc(PROTO(fs), cmp_op,
                            a_cond, left_reg, right_reg, line);
        /* Emit JMP for the true case (comparison succeeded) */
        uint32_t true_jmp = novai_emit_jmp(fs, line);

        novai_expr_free(fs, &right);
        novai_expr_free(fs, &left);

        e->kind            = NOVA_EK_JMP;
        e->u.jmp.true_list  = true_jmp;
        e->u.jmp.false_list = NOVA_NO_JUMP;
        return;
    }

    /* Arithmetic / bitwise / concat operators */
    NovaExprDesc left;
    memset(&left, 0, sizeof(left));
    novai_compile_expr(fs, expr->as.binary.left, &left);
    novai_discharge_to_any(fs, &left);

    NovaExprDesc right;
    memset(&right, 0, sizeof(right));
    novai_compile_expr(fs, expr->as.binary.right, &right);
    novai_discharge_to_any(fs, &right);

    NovaOpcode arith_op = novai_binop_to_opcode(binop);

    uint32_t pc = nova_proto_emit_abc(
        PROTO(fs), arith_op, 0, left.u.reg, right.u.reg, line);

    novai_expr_free(fs, &right);
    novai_expr_free(fs, &left);

    e->kind = NOVA_EK_RELOC;
    e->u.pc = pc;
}

/* ---- Expression compilation: call ---- */

static void novai_compile_call(NovaFuncState *fs, NovaRowExpr *expr,
                               NovaExprDesc *e, int is_method) {
    uint32_t line = LOC_LINE(expr->loc);
    uint8_t base = fs->free_reg;

    if (is_method) {
        /* Method call: obj:method(args)
         * Use OP_SELF to atomically copy receiver and look up method:
         *   R[base+1] = R[obj]          (receiver as first arg)
         *   R[base]   = R[obj][K[name]] (method via meta_index)
         */
        NovaRowExpr *callee_expr = nova_get_expr(AST(fs),
                                                 expr->as.call.callee);

        /* Compile the receiver object into a register */
        NovaExprDesc obj;
        memset(&obj, 0, sizeof(obj));
        novai_compile_expr(fs, callee_expr->as.field.object, &obj);
        novai_discharge_to_any(fs, &obj);

        /* Add method name as constant and encode as RK */
        uint32_t kidx = nova_proto_find_or_add_string(
            PROTO(fs), callee_expr->as.field.name,
            (uint32_t)callee_expr->as.field.name_len);
        uint8_t rk_c = (uint8_t)(NOVA_RK_CONST_BASE + kidx);

        /* Emit SELF: R[base+1]=obj, R[base]=obj[method] */
        nova_proto_emit_abc(PROTO(fs), NOVA_OP_SELF,
                            base, obj.u.reg, rk_c, line);

        /* base = method, base+1 = self, args start at base+2 */
        novai_set_free_reg(fs, (uint8_t)(base + 2));
    } else {
        /* Normal function call */
        NovaExprDesc callee;
        memset(&callee, 0, sizeof(callee));
        novai_compile_expr(fs, expr->as.call.callee, &callee);
        novai_discharge_to_reg(fs, &callee, base);

        /* Reserve the callee register so args go into base+1 */
        novai_set_free_reg(fs, (uint8_t)(base + 1));
    }

    /* Compile arguments into consecutive registers.
     * Track the last argument's expression kind so we can detect
     * vararg (...) or multi-return call as the final argument.   */
    int nargs = expr->as.call.arg_count;
    int last_is_multret = 0;
    for (int i = 0; i < nargs; i++) {
        NovaExprIdx arg_idx = nova_get_extra_expr(
            AST(fs), expr->as.call.args_start, i);
        NovaExprDesc arg;
        memset(&arg, 0, sizeof(arg));
        novai_compile_expr(fs, arg_idx, &arg);

        if (i == nargs - 1 && arg.kind == NOVA_EK_VARARG) {
            /* Last argument is `...` — emit VARARG with C=0 so the
             * VM expands ALL varargs and adjusts stack_top.  We do
             * NOT use the normal discharge path which would emit
             * C=0 but also bump free_reg by only 1.              */
            uint8_t dest = fs->free_reg;
            nova_proto_emit_abc(PROTO(fs), NOVA_OP_VARARG,
                                dest, 0, 0, line);
            /* Don't bump free_reg — CALL with B=0 reads stack_top */
            last_is_multret = 1;
        } else if (i == nargs - 1 && arg.kind == NOVA_EK_CALL) {
            /* Last argument is a function call — discharge it
             * normally (it emits a MOVE if needed), then walk back
             * to find the inner OP_CALL and patch its C field to 0
             * (variable results) so it adjusts stack_top.         */
            novai_discharge_to_next(fs, &arg);
            uint32_t end_pc = nova_proto_pc(PROTO(fs));
            for (uint32_t scan = end_pc; scan > 0; scan--) {
                NovaInstruction inst = PROTO(fs)->code[scan - 1];
                if (NOVA_GET_OPCODE(inst) == NOVA_OP_CALL) {
                    PROTO(fs)->code[scan - 1] = NOVA_ENCODE_ABC(
                        NOVA_OP_CALL,
                        NOVA_GET_A(inst),
                        NOVA_GET_B(inst),
                        0   /* C=0 → variable results */
                    );
                    break;
                }
            }
            last_is_multret = 1;
        } else {
            novai_discharge_to_next(fs, &arg);
        }
    }

    /* B = nargs + 1 (0 for multi-return last arg), C = 2 (1 result).
     *
     * If the last argument expanded to variable results (vararg or
     * multi-return call), set B=0 so the VM computes nargs from
     * stack_top instead of using a fixed count.                    */
    uint8_t b = (uint8_t)(nargs + 1 + (is_method ? 1 : 0));
    uint8_t c = 2; /* Single result expected */

    if (last_is_multret) {
        b = 0;
    }

    nova_proto_emit_abc(PROTO(fs), NOVA_OP_CALL,
                        base, b, c, line);

    /* Free temp registers used by args */
    novai_set_free_reg(fs, (uint8_t)(base + 1));

    e->kind  = NOVA_EK_CALL;
    e->u.reg = base;
}

/* ---- Expression compilation: table constructor ---- */

static void novai_compile_table(NovaFuncState *fs, NovaRowExpr *expr,
                                NovaExprDesc *e) {
    uint32_t line = LOC_LINE(expr->loc);
    uint8_t table_reg = novai_alloc_reg(fs);

    /* Emit NEWTABLE with size hints */
    int field_count = expr->as.table.field_count;
    nova_proto_emit_abc(PROTO(fs), NOVA_OP_NEWTABLE,
                        table_reg, (uint8_t)(field_count > 255 ? 255 : field_count),
                        0, line);

    NovaFieldIdx start = expr->as.table.field_start;
    int list_count = 0;

    for (int i = 0; i < field_count; i++) {
        NovaRowTableField *fld = &AST(fs)->fields[start + (uint32_t)i];
        uint32_t fld_line = LOC_LINE(fld->loc);

        switch (fld->kind) {
            case NOVA_FIELD_LIST: {
                /* Positional: accumulate in consecutive regs, flush with SETLIST */
                NovaExprDesc val;
                memset(&val, 0, sizeof(val));
                novai_compile_expr(fs, fld->value, &val);
                novai_discharge_to_next(fs, &val);
                list_count++;

                if (list_count >= NOVA_FIELDS_PER_FLUSH) {
                    nova_proto_emit_abc(PROTO(fs), NOVA_OP_SETLIST,
                                        table_reg, (uint8_t)list_count,
                                        (uint8_t)((list_count - 1) / NOVA_FIELDS_PER_FLUSH + 1),
                                        fld_line);
                    novai_set_free_reg(fs, (uint8_t)(table_reg + 1));
                    list_count = 0;
                }
                break;
            }

            case NOVA_FIELD_RECORD: {
                /* name = val: use SETFIELD */
                NovaExprDesc key;
                memset(&key, 0, sizeof(key));
                novai_compile_expr(fs, fld->key, &key);
                uint8_t key_rk = novai_expr_to_rk(fs, &key, fld_line);

                NovaExprDesc val;
                memset(&val, 0, sizeof(val));
                novai_compile_expr(fs, fld->value, &val);
                uint8_t val_rk = novai_expr_to_rk(fs, &val, fld_line);

                nova_proto_emit_abc(PROTO(fs), NOVA_OP_SETTABLE,
                                    table_reg, key_rk, val_rk, fld_line);

                novai_expr_free(fs, &val);
                novai_expr_free(fs, &key);
                break;
            }

            case NOVA_FIELD_BRACKET: {
                /* [expr] = val: use SETTABLE */
                NovaExprDesc key;
                memset(&key, 0, sizeof(key));
                novai_compile_expr(fs, fld->key, &key);
                uint8_t key_rk = novai_expr_to_rk(fs, &key, fld_line);

                NovaExprDesc val;
                memset(&val, 0, sizeof(val));
                novai_compile_expr(fs, fld->value, &val);
                uint8_t val_rk = novai_expr_to_rk(fs, &val, fld_line);

                nova_proto_emit_abc(PROTO(fs), NOVA_OP_SETTABLE,
                                    table_reg, key_rk, val_rk, fld_line);

                novai_expr_free(fs, &val);
                novai_expr_free(fs, &key);
                break;
            }

            default:
                break;
        }
    }

    /* Flush remaining list fields */
    if (list_count > 0) {
        nova_proto_emit_abc(PROTO(fs), NOVA_OP_SETLIST,
                            table_reg, (uint8_t)list_count, 1,
                            line);
        novai_set_free_reg(fs, (uint8_t)(table_reg + 1));
    }

    e->kind  = NOVA_EK_REG;
    e->u.reg = table_reg;
}

/* ---- Expression compilation: function literal ---- */

static void novai_compile_function(NovaFuncState *fs, NovaRowExpr *expr,
                                   NovaExprDesc *e) {
    uint32_t line = LOC_LINE(expr->loc);

    /* Create child proto */
    NovaProto *child = nova_proto_create();
    if (child == NULL) {
        novai_error(fs, line, "out of memory creating function proto");
        e->kind = NOVA_EK_NIL;
        return;
    }

    child->source       = fs->compiler->source;
    child->line_defined = line;
    child->num_params   = (uint8_t)expr->as.function.param_count;
    child->is_vararg    = (uint8_t)expr->as.function.is_variadic;
    child->is_async     = (uint8_t)expr->as.function.is_async;

    /* Set up child FuncState */
    NovaFuncState child_fs;
    novai_init_funcstate(&child_fs, child, fs, fs->compiler);
    child_fs.is_async = child->is_async;

    /* Enter function scope */
    novai_enter_scope(&child_fs, 0);

    /* Add parameters as locals */
    int nparams = expr->as.function.param_count;
    NovaParamIdx pstart = expr->as.function.param_start;
    for (int i = 0; i < nparams; i++) {
        NovaRowParam *prm = &AST(fs)->params[pstart + (uint32_t)i];
        novai_add_local(&child_fs, prm->name, (uint32_t)prm->name_len);
    }

    /* Emit VARARGPREP to save excess args as varargs */
    if (child->is_vararg) {
        nova_proto_emit(child,
                        NOVA_ENCODE_ABC(NOVA_OP_VARARGPREP, 0, 0, 0),
                        line);
    }

    /* Compile body */
    if (expr->as.function.body != NOVA_IDX_NONE) {
        novai_compile_block(&child_fs, expr->as.function.body);
    }

    /* Check for unresolved gotos before closing function */
    novai_check_unresolved_gotos(&child_fs);

    /* Emit implicit return at end of function */
    nova_proto_emit(child, NOVA_ENCODE_ABC(NOVA_OP_RETURN0, 0, 0, 0),
                    line);

    child->last_line = line; /* TODO: get actual last line */

    /* Leave function scope */
    novai_leave_scope(&child_fs);

    /* Restore parent as current */
    fs->compiler->current_fs = fs;

    /* Add child proto to parent */
    uint32_t child_idx = nova_proto_add_child(PROTO(fs), child);

    /* Emit CLOSURE in parent */
    if (child_idx <= NOVA_MAX_BX) {
        uint32_t pc = nova_proto_emit_abx(
            PROTO(fs), NOVA_OP_CLOSURE, 0, (uint16_t)child_idx, line);
        e->kind = NOVA_EK_RELOC;
        e->u.pc = pc;
    } else {
        novai_error(fs, line, "too many nested functions");
        e->kind = NOVA_EK_NIL;
        return;
    }

    /* Emit upvalue pseudo-instructions after CLOSURE.
     * The VM reads child->upvalue_count extra instructions,
     * decoding A=in_stack, B=index from each one. */
    for (uint8_t i = 0; i < child->upvalue_count; i++) {
        nova_proto_emit_abc(PROTO(fs), NOVA_OP_MOVE,
                            child->upvalues[i].in_stack,
                            child->upvalues[i].index, 0, line);
    }
}

/* ---- Expression compilation: async ops ---- */

static void novai_compile_async_op(NovaFuncState *fs, NovaRowExpr *expr,
                                   NovaExprDesc *e, NovaOpcode op) {
    uint32_t line = LOC_LINE(expr->loc);

    NovaExprDesc operand;
    memset(&operand, 0, sizeof(operand));
    novai_compile_expr(fs, expr->as.async_op.operand, &operand);
    novai_discharge_to_any(fs, &operand);

    uint32_t pc = nova_proto_emit_abc(
        PROTO(fs), op, 0, operand.u.reg, 0, line);

    novai_expr_free(fs, &operand);

    e->kind = NOVA_EK_RELOC;
    e->u.pc = pc;
}

/* ---- Expression compilation: interpolated string ---- */

static void novai_compile_interp(NovaFuncState *fs, NovaRowExpr *expr,
                                 NovaExprDesc *e) {
    uint32_t line = LOC_LINE(expr->loc);
    int nparts = expr->as.interp.part_count;
    uint8_t base = fs->free_reg;

    if (nparts == 0) {
        /* Empty interpolated string → load empty string constant */
        uint32_t kidx = nova_proto_find_or_add_string(
            PROTO(fs), "", 0);
        e->kind        = NOVA_EK_STRING;
        e->u.const_idx = kidx;
        return;
    }

    if (nparts == 1) {
        /* Single part: check if it's already a string literal */
        NovaExprIdx part_idx = nova_get_extra_expr(
            AST(fs), expr->as.interp.parts_start, 0);
        NovaRowExpr *part_expr = nova_get_expr(AST(fs), part_idx);
        if (part_expr->kind == NOVA_EXPR_STRING) {
            /* String literal → compile directly, no coercion needed */
            novai_compile_expr(fs, part_idx, e);
            return;
        }
        /* Non-string expression → concat with "" to force string coercion */
        uint32_t kidx = nova_proto_find_or_add_string(PROTO(fs), "", 0);
        nova_proto_emit_abx(PROTO(fs), NOVA_OP_LOADK,
                            base, (uint16_t)kidx, line);
        novai_set_free_reg(fs, (uint8_t)(base + 1));

        NovaExprDesc part;
        memset(&part, 0, sizeof(part));
        novai_compile_expr(fs, part_idx, &part);
        novai_discharge_to_reg(fs, &part, (uint8_t)(base + 1));
        novai_set_free_reg(fs, (uint8_t)(base + 2));

        nova_proto_emit_abc(PROTO(fs), NOVA_OP_CONCAT,
                            base, base, (uint8_t)(base + 1), line);
        novai_set_free_reg(fs, (uint8_t)(base + 1));
        e->kind  = NOVA_EK_REG;
        e->u.reg = base;
        return;
    }

    /* Multiple parts → chain binary CONCATs:
     *   base = part[0]
     *   base+1 = part[1]
     *   CONCAT base, base, base+1   → base = part0 .. part1
     *   base+1 = part[2]
     *   CONCAT base, base, base+1   → base = part0..part1..part2
     *   ...
     */

    /* Compile first part into base */
    {
        NovaExprIdx p0 = nova_get_extra_expr(
            AST(fs), expr->as.interp.parts_start, 0);
        NovaExprDesc part;
        memset(&part, 0, sizeof(part));
        novai_compile_expr(fs, p0, &part);
        novai_discharge_to_reg(fs, &part, base);
        novai_set_free_reg(fs, (uint8_t)(base + 1));
    }

    /* Chain remaining parts with binary CONCAT */
    for (int i = 1; i < nparts; i++) {
        NovaExprIdx pi = nova_get_extra_expr(
            AST(fs), expr->as.interp.parts_start, i);
        NovaExprDesc part;
        memset(&part, 0, sizeof(part));
        novai_compile_expr(fs, pi, &part);
        novai_discharge_to_reg(fs, &part, (uint8_t)(base + 1));
        novai_set_free_reg(fs, (uint8_t)(base + 2));

        /* CONCAT base, base, base+1 → result in base */
        nova_proto_emit_abc(PROTO(fs), NOVA_OP_CONCAT,
                            base, base, (uint8_t)(base + 1), line);
        novai_set_free_reg(fs, (uint8_t)(base + 1));
    }

    e->kind  = NOVA_EK_REG;
    e->u.reg = base;
}

/* ---- Expression compilation: index and field access ---- */

static void novai_compile_index(NovaFuncState *fs, NovaRowExpr *expr,
                                NovaExprDesc *e) {
    /* Compile object */
    NovaExprDesc obj;
    memset(&obj, 0, sizeof(obj));
    novai_compile_expr(fs, expr->as.index.object, &obj);
    novai_discharge_to_any(fs, &obj);

    /* Compile key */
    NovaExprDesc key;
    memset(&key, 0, sizeof(key));
    novai_compile_expr(fs, expr->as.index.index, &key);
    uint8_t key_rk = novai_expr_to_rk(fs, &key, LOC_LINE(expr->loc));

    e->kind            = NOVA_EK_INDEXED;
    e->u.index.table   = obj.u.reg;
    e->u.index.key     = key_rk;
}

static void novai_compile_field(NovaFuncState *fs, NovaRowExpr *expr,
                                NovaExprDesc *e) {
    /* Compile object */
    NovaExprDesc obj;
    memset(&obj, 0, sizeof(obj));
    novai_compile_expr(fs, expr->as.field.object, &obj);
    novai_discharge_to_any(fs, &obj);

    /* Add field name as constant */
    uint32_t kidx = nova_proto_find_or_add_string(
        PROTO(fs), expr->as.field.name, (uint32_t)expr->as.field.name_len);

    e->kind            = NOVA_EK_FIELD;
    e->u.index.table   = obj.u.reg;
    e->u.index.key     = (uint16_t)kidx;
}

/* ============================================================
 * MAIN EXPRESSION DISPATCH
 * ============================================================ */

static void novai_compile_expr(NovaFuncState *fs, NovaExprIdx idx,
                               NovaExprDesc *e) {
    if (idx == NOVA_IDX_NONE) {
        e->kind = NOVA_EK_NIL;
        return;
    }

    NovaRowExpr *expr = nova_get_expr(AST(fs), idx);

    switch (expr->kind) {
        /* ---- Literals ---- */
        case NOVA_EXPR_NIL:
            e->kind = NOVA_EK_NIL;
            break;

        case NOVA_EXPR_TRUE:
            e->kind = NOVA_EK_TRUE;
            break;

        case NOVA_EXPR_FALSE:
            e->kind = NOVA_EK_FALSE;
            break;

        case NOVA_EXPR_INTEGER:
            e->kind      = NOVA_EK_INTEGER;
            e->u.integer = expr->as.integer.value;
            break;

        case NOVA_EXPR_NUMBER: {
            uint32_t kidx = nova_proto_add_number(
                PROTO(fs), expr->as.number.value);
            e->kind        = NOVA_EK_NUMBER;
            e->u.const_idx = kidx;
            break;
        }

        case NOVA_EXPR_STRING: {
            uint32_t kidx = nova_proto_find_or_add_string(
                PROTO(fs), expr->as.string.data,
                (uint32_t)expr->as.string.len);
            e->kind        = NOVA_EK_STRING;
            e->u.const_idx = kidx;
            break;
        }

        case NOVA_EXPR_VARARG:
            e->kind = NOVA_EK_VARARG;
            break;

        /* ---- Name resolution ---- */
        case NOVA_EXPR_NAME:
            novai_compile_name(fs, expr, e);
            break;

        /* ---- Access ---- */
        case NOVA_EXPR_INDEX:
            novai_compile_index(fs, expr, e);
            break;

        case NOVA_EXPR_FIELD:
        case NOVA_EXPR_METHOD:
            novai_compile_field(fs, expr, e);
            break;

        /* ---- Operators ---- */
        case NOVA_EXPR_UNARY:
            novai_compile_unary(fs, expr, e);
            break;

        case NOVA_EXPR_BINARY:
            novai_compile_binary(fs, expr, e);
            break;

        /* ---- Calls ---- */
        case NOVA_EXPR_CALL:
            novai_compile_call(fs, expr, e, 0);
            break;

        case NOVA_EXPR_METHOD_CALL:
            novai_compile_call(fs, expr, e, 1);
            break;

        /* ---- Constructors ---- */
        case NOVA_EXPR_TABLE:
            novai_compile_table(fs, expr, e);
            break;

        case NOVA_EXPR_FUNCTION:
            novai_compile_function(fs, expr, e);
            break;

        /* ---- Interpolated string ---- */
        case NOVA_EXPR_INTERP_STRING:
            novai_compile_interp(fs, expr, e);
            break;

        /* ---- Async expressions ---- */
        case NOVA_EXPR_AWAIT:
            novai_compile_async_op(fs, expr, e, NOVA_OP_AWAIT);
            break;

        case NOVA_EXPR_SPAWN:
            novai_compile_async_op(fs, expr, e, NOVA_OP_SPAWN);
            break;

        case NOVA_EXPR_YIELD:
            novai_compile_async_op(fs, expr, e, NOVA_OP_YIELD);
            break;

        default:
            novai_error(fs, LOC_LINE(expr->loc),
                        "unknown expression type in compiler");
            e->kind = NOVA_EK_NIL;
            break;
    }

    /* Attach source line to every compiled expression so that
     * novai_discharge_to_reg emits correct debug line info.   */
    e->line = LOC_LINE(expr->loc);
}

/* ============================================================
 * PART 3: STATEMENT COMPILATION
 * ============================================================ */

/* ---- Statement: expression (side effects only) ---- */

static void novai_compile_stmt_expr(NovaFuncState *fs,
                                    NovaRowStmt *stmt) {
    uint8_t saved_free_reg = fs->free_reg;
    NovaExprDesc e;
    memset(&e, 0, sizeof(e));
    novai_compile_expr(fs, stmt->as.expr.expr, &e);
    novai_expr_free(fs, &e);
    /* Restore free_reg: expression statements produce no locals,
     * so all temp registers should be released. */
    fs->free_reg = saved_free_reg;
}

/* ---- Statement: dec declaration ---- */

static void novai_compile_stmt_local(NovaFuncState *fs,
                                     NovaRowStmt *stmt) {
    uint32_t line = LOC_LINE(stmt->loc);
    int nnames  = stmt->as.local.name_count;
    int nvalues = stmt->as.local.value_count;

    /* Target registers for locals: start at current active_locals.
     * The i-th local will occupy register (base_reg + i). */
    uint8_t base_reg = fs->active_locals;

    /* Compile RHS values into consecutive target registers.
     * If the last value expression is a function call or vararg and
     * nnames > nvalues, patch it to return multiple results. */
    int multret_extra = 0;
    for (int i = 0; i < nnames; i++) {
        uint8_t target = (uint8_t)(base_reg + i);

        if (multret_extra > 0) {
            multret_extra--;
            continue;
        }
        if (i < nvalues) {
            NovaExprIdx val_idx = nova_get_extra_expr(
                AST(fs), stmt->as.local.values_start, i);
            NovaExprDesc val;
            memset(&val, 0, sizeof(val));

            /* Ensure free_reg is past target so intermediates
             * don't pollute the target range */
            if (fs->free_reg <= target) {
                fs->free_reg = target;
            }

            novai_compile_expr(fs, val_idx, &val);

            /* Last value expr with remaining unfilled locals? */
            if (i == nvalues - 1 && nnames > nvalues) {
                int wanted = nnames - i;

                if (val.kind == NOVA_EK_CALL) {
                    /* Patch the CALL's C field for multi-return.
                     * CALL result base is val.u.reg (where the func was). */
                    uint32_t pc = nova_proto_pc(PROTO(fs)) - 1;
                    NovaInstruction inst = PROTO(fs)->code[pc];
                    if (NOVA_GET_OPCODE(inst) == NOVA_OP_CALL) {
                        PROTO(fs)->code[pc] = NOVA_ENCODE_ABC(
                            NOVA_OP_CALL,
                            NOVA_GET_A(inst),
                            NOVA_GET_B(inst),
                            (uint8_t)(wanted + 1)
                        );
                    }
                    /* Results go into val.u.reg, val.u.reg+1, ...,
                     * val.u.reg+wanted-1. If that's not at target,
                     * we need to move them. */
                    if (val.u.reg != target) {
                        for (int j = 0; j < wanted; j++) {
                            nova_proto_emit_abc(PROTO(fs), NOVA_OP_MOVE,
                                (uint8_t)(target + j),
                                (uint8_t)(val.u.reg + j), 0, line);
                        }
                    }
                    novai_set_free_reg(fs, (uint8_t)(target + wanted));
                    multret_extra = wanted - 1;
                    continue;
                }

                if (val.kind == NOVA_EK_VARARG) {
                    nova_proto_emit_abc(PROTO(fs), NOVA_OP_VARARG,
                                        target, (uint8_t)(wanted + 1),
                                        0, line);
                    novai_set_free_reg(fs, (uint8_t)(target + wanted));
                    multret_extra = wanted - 1;
                    continue;
                }
            }

            /* Discharge value into exact target register */
            novai_discharge_to_reg(fs, &val, target);
            novai_set_free_reg(fs, (uint8_t)(target + 1));
        } else {
            /* No value: load nil into target */
            nova_proto_emit_abc(PROTO(fs), NOVA_OP_LOADNIL,
                                target, 0, 0, line);
            novai_set_free_reg(fs, (uint8_t)(target + 1));
        }
    }

    /* Now register them as locals (rewind free_reg since
     * novai_add_local will re-allocate registers) */
    fs->free_reg -= (uint8_t)nnames;
    fs->active_locals -= 0; /* Don't adjust -- add_local will do it */

    for (int i = 0; i < nnames; i++) {
        NovaRowNameRef *nm = &AST(fs)->names[
            stmt->as.local.names_start + (uint32_t)i];
        /* The value was already placed in the register that
         * add_local will allocate (since free_reg was rewound) */
        novai_add_local(fs, nm->data, (uint32_t)nm->len);
    }
}

/* ---- Statement: assignment ---- */

static void novai_compile_stmt_assign(NovaFuncState *fs,
                                      NovaRowStmt *stmt) {
    uint32_t line = LOC_LINE(stmt->loc);
    int ntargets = stmt->as.assign.target_count;
    int nvalues  = stmt->as.assign.value_count;

    /* Single assignment optimization: target = value */
    if (ntargets == 1 && nvalues == 1) {
        /* Compile target as lvalue */
        NovaExprIdx target_idx = nova_get_extra_expr(
            AST(fs), stmt->as.assign.targets_start, 0);
        NovaExprDesc target;
        memset(&target, 0, sizeof(target));
        novai_compile_expr(fs, target_idx, &target);

        /* Compile value */
        NovaExprIdx val_idx = nova_get_extra_expr(
            AST(fs), stmt->as.assign.values_start, 0);
        NovaExprDesc val;
        memset(&val, 0, sizeof(val));
        novai_compile_expr(fs, val_idx, &val);
        novai_discharge_to_any(fs, &val);

        novai_store_var(fs, &target, val.u.reg, line);
        novai_expr_free(fs, &val);
        return;
    }

    /* Multi-assignment: compile all RHS into temps first.
     * If the last value is a call/vararg and ntargets > nvalues,
     * patch it to return multiple results. */
    uint8_t val_base = fs->free_reg;
    int multret_assign = 0; /* extra slots filled by multi-return */
    for (int i = 0; i < nvalues; i++) {
        NovaExprIdx val_idx = nova_get_extra_expr(
            AST(fs), stmt->as.assign.values_start, i);
        NovaExprDesc val;
        memset(&val, 0, sizeof(val));
        novai_compile_expr(fs, val_idx, &val);

        /* Last value with more targets remaining? */
        if (i == nvalues - 1 && ntargets > nvalues) {
            int wanted = ntargets - i;

            if (val.kind == NOVA_EK_CALL) {
                uint32_t pc = nova_proto_pc(PROTO(fs)) - 1;
                NovaInstruction inst = PROTO(fs)->code[pc];
                if (NOVA_GET_OPCODE(inst) == NOVA_OP_CALL) {
                    PROTO(fs)->code[pc] = NOVA_ENCODE_ABC(
                        NOVA_OP_CALL,
                        NOVA_GET_A(inst),
                        NOVA_GET_B(inst),
                        (uint8_t)(wanted + 1)
                    );
                }
                for (int j = 1; j < wanted; j++) {
                    novai_alloc_reg(fs);
                }
                multret_assign = wanted - 1;
                continue;
            }

            if (val.kind == NOVA_EK_VARARG) {
                uint8_t va_base = novai_alloc_reg(fs);
                nova_proto_emit_abc(PROTO(fs), NOVA_OP_VARARG,
                                    va_base, (uint8_t)(wanted + 1),
                                    0, line);
                for (int j = 1; j < wanted; j++) {
                    novai_alloc_reg(fs);
                }
                multret_assign = wanted - 1;
                continue;
            }
        }

        novai_discharge_to_next(fs, &val);
    }

    /* Load nil for any missing values (skip if multi-return filled them) */
    for (int i = nvalues; i < ntargets; i++) {
        if (multret_assign > 0) {
            multret_assign--;
        } else {
            uint8_t reg = novai_alloc_reg(fs);
            nova_proto_emit_abc(PROTO(fs), NOVA_OP_LOADNIL,
                                reg, 0, 0, line);
        }
    }

    /* Now store each value into its target */
    for (int i = 0; i < ntargets; i++) {
        NovaExprIdx target_idx = nova_get_extra_expr(
            AST(fs), stmt->as.assign.targets_start, i);
        NovaExprDesc target;
        memset(&target, 0, sizeof(target));
        novai_compile_expr(fs, target_idx, &target);

        uint8_t val_reg = (uint8_t)(val_base + i);
        novai_store_var(fs, &target, val_reg, line);
    }

    /* Free all temp registers */
    fs->free_reg = val_base;
}

/* ---- Statement: if/elseif/else ---- */

static void novai_compile_stmt_if(NovaFuncState *fs,
                                  NovaRowStmt *stmt) {
    uint32_t line = LOC_LINE(stmt->loc);
    int nbranches = stmt->as.if_stmt.branch_count;
    NovaBranchIdx bstart = stmt->as.if_stmt.branch_start;

    uint32_t exit_list = NOVA_NO_JUMP;

    for (int i = 0; i < nbranches; i++) {
        NovaRowIfBranch *br = &AST(fs)->branches[bstart + (uint32_t)i];
        uint32_t br_line = LOC_LINE(br->loc);

        if (br->condition != NOVA_IDX_NONE) {
            /* if/elseif: compile condition */
            NovaExprDesc cond;
            memset(&cond, 0, sizeof(cond));
            novai_compile_expr(fs, br->condition, &cond);
            novai_discharge_to_any(fs, &cond);

            /* TEST cond_reg, 0 → skip next if false */
            nova_proto_emit_abc(PROTO(fs), NOVA_OP_TEST,
                                cond.u.reg, 0, 0, br_line);
            uint32_t false_jmp = novai_emit_jmp(fs, br_line);

            novai_expr_free(fs, &cond);

            /* Compile body */
            novai_enter_scope(fs, 0);
            if (br->body != NOVA_IDX_NONE) {
                novai_compile_block(fs, br->body);
            }
            novai_leave_scope(fs);

            /* JMP to end of entire if chain */
            if (i < nbranches - 1) {
                uint32_t end_jmp = novai_emit_jmp(fs, br_line);
                novai_concat_jmp_list(fs, &exit_list, end_jmp);
            }

            /* Patch false jump to here (next branch) */
            novai_patch_to_here(fs, false_jmp);
        } else {
            /* else branch: no condition */
            novai_enter_scope(fs, 0);
            if (br->body != NOVA_IDX_NONE) {
                novai_compile_block(fs, br->body);
            }
            novai_leave_scope(fs);
        }
    }

    /* Patch all exit jumps to here */
    novai_patch_to_here(fs, exit_list);
    (void)line;
}

/* ---- Statement: while loop ---- */

static void novai_compile_stmt_while(NovaFuncState *fs,
                                     NovaRowStmt *stmt) {
    uint32_t line = LOC_LINE(stmt->loc);

    uint32_t loop_start = nova_proto_pc(PROTO(fs));

    /* Compile condition */
    NovaExprDesc cond;
    memset(&cond, 0, sizeof(cond));
    novai_compile_expr(fs, stmt->as.while_stmt.condition, &cond);
    novai_discharge_to_any(fs, &cond);

    /* TEST cond_reg, 0 → skip if false */
    nova_proto_emit_abc(PROTO(fs), NOVA_OP_TEST,
                        cond.u.reg, 0, 0, line);
    uint32_t exit_jmp = novai_emit_jmp(fs, line);
    novai_expr_free(fs, &cond);

    /* Compile body */
    novai_enter_scope(fs, 1);
    if (stmt->as.while_stmt.body != NOVA_IDX_NONE) {
        novai_compile_block(fs, stmt->as.while_stmt.body);
    }

    /* JMP back to loop start */
    uint32_t back_jmp = novai_emit_jmp(fs, line);
    novai_patch_jmp(fs, back_jmp, loop_start);

    /* Patch exit and break jumps */
    novai_patch_to_here(fs, exit_jmp);
    novai_patch_to_here(fs, fs->scopes[fs->scope_depth - 1].break_list);

    /* Patch continue jumps to loop start */
    novai_patch_list(fs, fs->scopes[fs->scope_depth - 1].continue_list,
                     loop_start);

    novai_leave_scope(fs);
}

/* ---- Statement: repeat...until ---- */

static void novai_compile_stmt_repeat(NovaFuncState *fs,
                                      NovaRowStmt *stmt) {
    uint32_t line = LOC_LINE(stmt->loc);
    uint32_t loop_start = nova_proto_pc(PROTO(fs));

    novai_enter_scope(fs, 1);

    /* Compile body */
    if (stmt->as.repeat_stmt.body != NOVA_IDX_NONE) {
        novai_compile_block(fs, stmt->as.repeat_stmt.body);
    }

    /* Compile condition */
    NovaExprDesc cond;
    memset(&cond, 0, sizeof(cond));
    novai_compile_expr(fs, stmt->as.repeat_stmt.condition, &cond);
    novai_discharge_to_any(fs, &cond);

    /* TEST cond_reg, 0 → if false, jump back to start */
    nova_proto_emit_abc(PROTO(fs), NOVA_OP_TEST,
                        cond.u.reg, 0, 0, line);
    uint32_t back_jmp = novai_emit_jmp(fs, line);
    novai_patch_jmp(fs, back_jmp, loop_start);
    novai_expr_free(fs, &cond);

    /* Patch break/continue */
    novai_patch_to_here(fs, fs->scopes[fs->scope_depth - 1].break_list);
    novai_patch_list(fs, fs->scopes[fs->scope_depth - 1].continue_list,
                     loop_start);

    novai_leave_scope(fs);
}

/* ---- Statement: for numeric ---- */

static void novai_compile_stmt_for_numeric(NovaFuncState *fs,
                                           NovaRowStmt *stmt) {
    uint32_t line = LOC_LINE(stmt->loc);

    novai_enter_scope(fs, 1);

    /* Internal loop variables: R(A)=init, R(A+1)=limit, R(A+2)=step */
    uint8_t base = fs->free_reg;

    /* Compile start */
    NovaExprDesc start;
    memset(&start, 0, sizeof(start));
    novai_compile_expr(fs, stmt->as.for_numeric.start, &start);
    novai_discharge_to_next(fs, &start);

    /* Compile stop */
    NovaExprDesc stop;
    memset(&stop, 0, sizeof(stop));
    novai_compile_expr(fs, stmt->as.for_numeric.stop, &stop);
    novai_discharge_to_next(fs, &stop);

    /* Compile step (or default to 1) */
    if (stmt->as.for_numeric.step != NOVA_IDX_NONE) {
        NovaExprDesc step;
        memset(&step, 0, sizeof(step));
        novai_compile_expr(fs, stmt->as.for_numeric.step, &step);
        novai_discharge_to_next(fs, &step);
    } else {
        /* Default step = 1 */
        uint8_t step_reg = novai_alloc_reg(fs);
        nova_proto_emit_asbx(PROTO(fs), NOVA_OP_LOADINT,
                             step_reg, 1, line);
    }

    /* Register the 3 internal loop variables (init, limit, step)
     * as hidden locals so that active_locals correctly reflects
     * register usage.  This prevents body locals from getting
     * registers that overlap with the loop's internal state.
     * Names starting with '(' are inaccessible from user code. */
    {
        uint8_t saved_freg = fs->free_reg;
        fs->free_reg = base;
        novai_add_local(fs, "(for state)", 11);
        novai_add_local(fs, "(for limit)", 11);
        novai_add_local(fs, "(for step)",  10);
        (void)saved_freg; /* free_reg == base+3 == saved_freg */
    }

    /* FORPREP base, jmp_forward_to_test */
    uint32_t prep_pc = nova_proto_emit_asbx(
        PROTO(fs), NOVA_OP_FORPREP, base, 0, line);

    /* Loop body: the loop variable is R(A+3) */
    uint32_t body_start = nova_proto_pc(PROTO(fs));
    novai_add_local(fs, stmt->as.for_numeric.name,
                    (uint32_t)stmt->as.for_numeric.name_len);

    if (stmt->as.for_numeric.body != NOVA_IDX_NONE) {
        novai_compile_block(fs, stmt->as.for_numeric.body);
    }

    /* Close upvalues captured from the loop variable (base+3)
     * before next iteration so each iteration gets its own copy.
     * This is a no-op if no closures captured the loop variable. */
    uint32_t continue_target = nova_proto_pc(PROTO(fs));
    nova_proto_emit_abc(PROTO(fs), NOVA_OP_CLOSE,
                        (uint8_t)(base + 3), 0, 0, line);

    /* FORLOOP base, jmp_back_to_body */
    uint32_t loop_pc = nova_proto_pc(PROTO(fs));
    int body_offset = (int)body_start - (int)loop_pc - 1;
    nova_proto_emit_asbx(PROTO(fs), NOVA_OP_FORLOOP,
                         base, body_offset, line);

    /* Patch FORPREP to jump to FORLOOP */
    int prep_offset = (int)loop_pc - (int)prep_pc - 1;
    nova_proto_patch_sbx(PROTO(fs), prep_pc, prep_offset);

    /* Patch break list */
    novai_patch_to_here(fs, fs->scopes[fs->scope_depth - 1].break_list);

    /* Patch continue jumps to CLOSE (before FORLOOP) */
    novai_patch_list(fs, fs->scopes[fs->scope_depth - 1].continue_list,
                     continue_target);

    novai_leave_scope(fs);
}

/* ---- Statement: for generic ---- */

static void novai_compile_stmt_for_generic(NovaFuncState *fs,
                                           NovaRowStmt *stmt) {
    uint32_t line = LOC_LINE(stmt->loc);

    novai_enter_scope(fs, 1);

    /* Compile iterator expressions into R(A), R(A+1), R(A+2).
     * If there's only 1 expression and it's a CALL (e.g., ipairs(t)),
     * patch the CALL to return 3 values. */
    uint8_t base = fs->free_reg;
    int niter = stmt->as.for_generic.iter_count;

    if (niter == 1) {
        /* Single expression (common case: ipairs/pairs) */
        NovaExprIdx iter_idx = nova_get_extra_expr(
            AST(fs), stmt->as.for_generic.iters_start, 0);
        NovaExprDesc iter;
        memset(&iter, 0, sizeof(iter));
        novai_compile_expr(fs, iter_idx, &iter);

        if (iter.kind == NOVA_EK_CALL) {
            /* Patch CALL to return 3 results */
            uint32_t pc = nova_proto_pc(PROTO(fs)) - 1;
            NovaInstruction inst = PROTO(fs)->code[pc];
            if (NOVA_GET_OPCODE(inst) == NOVA_OP_CALL) {
                PROTO(fs)->code[pc] = NOVA_ENCODE_ABC(
                    NOVA_OP_CALL,
                    NOVA_GET_A(inst),
                    NOVA_GET_B(inst),
                    (uint8_t)(3 + 1)  /* 3 results */
                );
            }
            /* Allocate 3 registers for iter, state, control */
            novai_set_free_reg(fs, (uint8_t)(base + 3));
        } else {
            novai_discharge_to_next(fs, &iter);
        }
    } else {
        for (int i = 0; i < niter && i < 3; i++) {
            NovaExprIdx iter_idx = nova_get_extra_expr(
                AST(fs), stmt->as.for_generic.iters_start, i);
            NovaExprDesc iter;
            memset(&iter, 0, sizeof(iter));
            novai_compile_expr(fs, iter_idx, &iter);
            novai_discharge_to_next(fs, &iter);
        }
    }

    /* Pad to 3 if fewer iterators provided */
    while (fs->free_reg < (uint8_t)(base + 3)) {
        uint8_t reg = novai_alloc_reg(fs);
        nova_proto_emit_abc(PROTO(fs), NOVA_OP_LOADNIL,
                            reg, 0, 0, line);
    }

    /* Register the 3 internal iterator variables as hidden locals
     * so that active_locals correctly reflects register usage.
     * This prevents body locals from overlapping with loop internals.
     * Names starting with '(' are inaccessible from user code. */
    {
        uint8_t saved_freg = fs->free_reg;
        fs->free_reg = base;
        novai_add_local(fs, "(for generator)", 15);
        novai_add_local(fs, "(for state)",     11);
        novai_add_local(fs, "(for control)",   13);
        (void)saved_freg; /* free_reg == base+3 == saved_freg */
    }

    /* Reserve R(A+3), R(A+4), ... for loop variables */
    int nnames = stmt->as.for_generic.name_count;

    /* JMP to TFORCALL */
    uint32_t jmp_pc = novai_emit_jmp(fs, line);

    /* Loop body start */
    uint32_t body_start = nova_proto_pc(PROTO(fs));

    /* Add loop variable names as locals: R(A+3), R(A+4), ... */
    for (int i = 0; i < nnames; i++) {
        NovaRowNameRef *nm = &AST(fs)->names[
            stmt->as.for_generic.names_start + (uint32_t)i];
        novai_add_local(fs, nm->data, (uint32_t)nm->len);
    }

    if (stmt->as.for_generic.body != NOVA_IDX_NONE) {
        novai_compile_block(fs, stmt->as.for_generic.body);
    }

    /* Close upvalues captured from loop variables before next iteration */
    uint32_t continue_target = nova_proto_pc(PROTO(fs));
    nova_proto_emit_abc(PROTO(fs), NOVA_OP_CLOSE,
                        (uint8_t)(base + 3), 0, 0, line);

    /* Patch initial JMP to here (TFORCALL location) */
    novai_patch_to_here(fs, jmp_pc);

    /* TFORCALL base, nresults */
    nova_proto_emit_abc(PROTO(fs), NOVA_OP_TFORCALL,
                        base, 0, (uint8_t)nnames, line);

    /* TFORLOOP base, jmp_back_to_body */
    int back_offset = (int)body_start - (int)nova_proto_pc(PROTO(fs)) - 1;
    nova_proto_emit_asbx(PROTO(fs), NOVA_OP_TFORLOOP,
                         base, back_offset, line);

    /* Patch break list */
    novai_patch_to_here(fs, fs->scopes[fs->scope_depth - 1].break_list);

    /* Patch continue jumps to CLOSE (before TFORCALL) */
    novai_patch_list(fs, fs->scopes[fs->scope_depth - 1].continue_list,
                     continue_target);

    novai_leave_scope(fs);
}

/* ---- Statement: do...end ---- */

static void novai_compile_stmt_do(NovaFuncState *fs,
                                  NovaRowStmt *stmt) {
    novai_enter_scope(fs, 0);
    if (stmt->as.do_stmt.body != NOVA_IDX_NONE) {
        novai_compile_block(fs, stmt->as.do_stmt.body);
    }
    novai_leave_scope(fs);
}

/* ---- Statement: break ---- */

static void novai_compile_stmt_break(NovaFuncState *fs,
                                     NovaRowStmt *stmt) {
    uint32_t line = LOC_LINE(stmt->loc);

    /* Find the nearest loop scope */
    int found = 0;
    for (int d = (int)fs->scope_depth - 1; d >= 0; d--) {
        if (fs->scopes[d].is_loop) {
            uint32_t jmp = novai_emit_jmp(fs, line);
            novai_concat_jmp_list(fs, &fs->scopes[d].break_list, jmp);
            found = 1;
            break;
        }
    }

    if (found == 0) {
        novai_error(fs, line, "'break' outside of loop");
    }
}

/* ---- Statement: continue ---- */

static void novai_compile_stmt_continue(NovaFuncState *fs,
                                        NovaRowStmt *stmt) {
    uint32_t line = LOC_LINE(stmt->loc);

    /* Find the nearest loop scope */
    int found = 0;
    for (int d = (int)fs->scope_depth - 1; d >= 0; d--) {
        if (fs->scopes[d].is_loop) {
            uint32_t jmp = novai_emit_jmp(fs, line);
            novai_concat_jmp_list(fs, &fs->scopes[d].continue_list, jmp);
            found = 1;
            break;
        }
    }

    if (found == 0) {
        novai_error(fs, line, "'continue' outside of loop");
    }
}

/* ---- Statement: goto ---- */

/**
 * @brief Find a label by name in the current function.
 *
 * @param fs       Current function state
 * @param name     Label name
 * @param name_len Label name length
 *
 * @return Index into fs->labels, or -1 if not found.
 */
static int novai_find_label(NovaFuncState *fs, const char *name,
                            size_t name_len) {
    for (int i = 0; i < (int)fs->label_count; i++) {
        if (fs->labels[i].name_len == name_len &&
            memcmp(fs->labels[i].name, name, name_len) == 0) {
            return i;
        }
    }
    return -1;
}

/**
 * @brief Compile a goto statement.
 *
 * If the target label already exists (backward jump), patch
 * the JMP immediately. Otherwise, record a pending goto to
 * be resolved when the label is encountered or at function end.
 *
 * @param fs   Current function state
 * @param stmt Goto AST statement
 */
static void novai_compile_stmt_goto(NovaFuncState *fs,
                                    NovaRowStmt *stmt) {
    uint32_t line = LOC_LINE(stmt->loc);
    const char *name = stmt->as.goto_stmt.label;
    size_t name_len = stmt->as.goto_stmt.label_len;

    /* Emit the JMP instruction (with placeholder offset) */
    uint32_t jmp_pc = novai_emit_jmp(fs, line);

    /* Search for an existing label (backward goto) */
    int label_idx = novai_find_label(fs, name, name_len);
    if (label_idx >= 0) {
        /* Backward jump: validate scope — cannot jump into a scope
         * that declares new locals (the label's active_locals must
         * be >= goto's active_locals, meaning no new locals appeared
         * between the label and the goto). */
        NovaLabelDesc *lbl = &fs->labels[label_idx];
        if (lbl->active_locals < fs->active_locals) {
            /* This is a backward goto to a label that had fewer
             * locals; that's fine — we're just going back. */
        }
        novai_patch_jmp(fs, jmp_pc, lbl->pc);
        return;
    }

    /* Forward goto: add to pending list */
    if (fs->goto_count >= NOVA_MAX_GOTOS) {
        novai_error(fs, line, "too many pending goto statements");
        return;
    }
    NovaGotoDesc *g = &fs->gotos[fs->goto_count++];
    g->name = name;
    g->name_len = name_len;
    g->pc = jmp_pc;
    g->line = line;
    g->active_locals = fs->active_locals;
    g->scope_depth = fs->scope_depth;
}

/* ---- Statement: label ---- */

/**
 * @brief Compile a label statement (::name::).
 *
 * Records the label and resolves any pending gotos that
 * target this label (forward jumps).
 *
 * @param fs   Current function state
 * @param stmt Label AST statement
 */
static void novai_compile_stmt_label(NovaFuncState *fs,
                                     NovaRowStmt *stmt) {
    uint32_t line = LOC_LINE(stmt->loc);
    const char *name = stmt->as.label_stmt.label;
    size_t name_len = stmt->as.label_stmt.label_len;

    /* Check for duplicate label in the same function */
    int existing = novai_find_label(fs, name, name_len);
    if (existing >= 0) {
        novai_error(fs, line, "duplicate label in function");
        return;
    }

    /* Record the label */
    if (fs->label_count >= NOVA_MAX_LABELS) {
        novai_error(fs, line, "too many labels");
        return;
    }
    uint32_t label_pc = nova_proto_pc(PROTO(fs));
    NovaLabelDesc *lbl = &fs->labels[fs->label_count++];
    lbl->name = name;
    lbl->name_len = name_len;
    lbl->pc = label_pc;
    lbl->active_locals = fs->active_locals;
    lbl->scope_depth = fs->scope_depth;

    /* Resolve any forward gotos that target this label */
    int i = 0;
    while (i < (int)fs->goto_count) {
        NovaGotoDesc *g = &fs->gotos[i];
        if (g->name_len == name_len &&
            memcmp(g->name, name, name_len) == 0) {
            /* Validate: forward goto must not jump over locals
             * into a scope with more active locals than at the goto. */
            if (lbl->active_locals > g->active_locals) {
                novai_error(fs, g->line,
                            "goto jumps over dec variable");
            }
            novai_patch_jmp(fs, g->pc, label_pc);
            /* Remove resolved goto by swapping with last */
            fs->gotos[i] = fs->gotos[fs->goto_count - 1];
            fs->goto_count--;
            /* Don't increment i — check the swapped entry */
        } else {
            i++;
        }
    }
}

/**
 * @brief Check for unresolved gotos at end of function.
 *
 * Called before emitting the implicit RETURN. Any pending
 * goto whose target label was never defined is an error.
 *
 * @param fs  Current function state
 */
static void novai_check_unresolved_gotos(NovaFuncState *fs) {
    for (int i = 0; i < (int)fs->goto_count; i++) {
        NovaGotoDesc *g = &fs->gotos[i];
        novai_error(fs, g->line,
                    "label not found for goto");
    }
}

/* ---- Statement: return ---- */

static void novai_compile_stmt_return(NovaFuncState *fs,
                                      NovaRowStmt *stmt) {
    uint32_t line = LOC_LINE(stmt->loc);
    int nvalues = stmt->as.return_stmt.value_count;

    if (nvalues == 0) {
        nova_proto_emit(PROTO(fs),
                        NOVA_ENCODE_ABC(NOVA_OP_RETURN0, 0, 0, 0), line);
        return;
    }

    uint8_t base = fs->free_reg;

    /* Compile return values into consecutive registers */
    for (int i = 0; i < nvalues; i++) {
        NovaExprIdx val_idx = nova_get_extra_expr(
            AST(fs), stmt->as.return_stmt.values_start, i);
        NovaExprDesc val;
        memset(&val, 0, sizeof(val));
        novai_compile_expr(fs, val_idx, &val);
        novai_discharge_to_next(fs, &val);
    }

    if (nvalues == 1) {
        /* Optimize: RETURN1 for single value */
        nova_proto_emit_abc(PROTO(fs), NOVA_OP_RETURN1,
                            base, 0, 0, line);
    } else {
        /* RETURN base, nvalues+1 */
        nova_proto_emit_abc(PROTO(fs), NOVA_OP_RETURN,
                            base, (uint8_t)(nvalues + 1), 0, line);
    }

    /* Free temps (though execution won't reach past return) */
    fs->free_reg = base;
}

/* ---- Statement: function declaration ---- */

static void novai_compile_stmt_function(NovaFuncState *fs,
                                        NovaRowStmt *stmt,
                                        int is_local) {
    uint32_t line = LOC_LINE(stmt->loc);

    if (is_local) {
        /* local function name() ... end
         * Add local first (allows self-recursion) */
        NovaRowExpr *name_expr = nova_get_expr(
            AST(fs), stmt->as.func_stmt.name);
        novai_add_local(fs, name_expr->as.string.data,
                        (uint32_t)name_expr->as.string.len);
    }

    /* Create child proto */
    NovaProto *child = nova_proto_create();
    if (child == NULL) {
        novai_error(fs, line, "out of memory creating function proto");
        return;
    }

    child->source       = fs->compiler->source;
    child->line_defined = line;
    child->num_params   = (uint8_t)stmt->as.func_stmt.param_count;
    child->is_vararg    = (uint8_t)stmt->as.func_stmt.is_variadic;
    child->is_async     = (uint8_t)stmt->as.func_stmt.is_async;

    /* Set up child FuncState */
    NovaFuncState child_fs;
    novai_init_funcstate(&child_fs, child, fs, fs->compiler);
    child_fs.is_async = child->is_async;

    novai_enter_scope(&child_fs, 0);

    /* Add parameters as locals */
    int nparams = stmt->as.func_stmt.param_count;
    NovaParamIdx pstart = stmt->as.func_stmt.param_start;
    for (int i = 0; i < nparams; i++) {
        NovaRowParam *prm = &AST(fs)->params[pstart + (uint32_t)i];
        novai_add_local(&child_fs, prm->name, (uint32_t)prm->name_len);
    }

    /* Emit VARARGPREP to save excess args as varargs */
    if (child->is_vararg) {
        nova_proto_emit(child,
                        NOVA_ENCODE_ABC(NOVA_OP_VARARGPREP, 0, 0, 0),
                        line);
    }

    /* Compile body */
    if (stmt->as.func_stmt.body != NOVA_IDX_NONE) {
        novai_compile_block(&child_fs, stmt->as.func_stmt.body);
    }

    /* Implicit return */
    nova_proto_emit(child,
                    NOVA_ENCODE_ABC(NOVA_OP_RETURN0, 0, 0, 0), line);
    child->last_line = line;

    novai_leave_scope(&child_fs);
    fs->compiler->current_fs = fs;

    /* Add child to parent and emit CLOSURE */
    uint32_t child_idx = nova_proto_add_child(PROTO(fs), child);
    uint8_t dest_reg = 0;

    if (is_local) {
        /* Local function: destination is the local's register
         * (which was the last local added) */
        dest_reg = fs->locals[fs->active_locals - 1].reg;
    } else {
        dest_reg = novai_alloc_reg(fs);
    }

    if (child_idx <= NOVA_MAX_BX) {
        nova_proto_emit_abx(PROTO(fs), NOVA_OP_CLOSURE,
                            dest_reg, (uint16_t)child_idx, line);
    } else {
        novai_error(fs, line, "too many nested functions");
        return;
    }

    /* Emit upvalue pseudo-instructions after CLOSURE.
     * The VM reads child->upvalue_count extra instructions,
     * decoding A=in_stack, B=index from each one. */
    for (uint8_t i = 0; i < child->upvalue_count; i++) {
        nova_proto_emit_abc(PROTO(fs), NOVA_OP_MOVE,
                            child->upvalues[i].in_stack,
                            child->upvalues[i].index, 0, line);
    }

    if (!is_local) {
        /* Global function: store into global */
        NovaExprDesc target;
        memset(&target, 0, sizeof(target));
        novai_compile_expr(fs, stmt->as.func_stmt.name, &target);
        novai_store_var(fs, &target, dest_reg, line);
        novai_free_reg(fs, dest_reg);
    }
}

/* ---- Statement: import ---- */

static void novai_compile_stmt_import(NovaFuncState *fs,
                                      NovaRowStmt *stmt) {
    uint32_t line = LOC_LINE(stmt->loc);

    uint32_t mod_kidx = nova_proto_find_or_add_string(
        PROTO(fs), stmt->as.import_stmt.module,
        (uint32_t)stmt->as.import_stmt.module_len);

    uint8_t reg = novai_alloc_reg(fs);
    if (mod_kidx <= NOVA_MAX_BX) {
        nova_proto_emit_abx(PROTO(fs), NOVA_OP_IMPORT,
                            reg, (uint16_t)mod_kidx, line);
    } else {
        novai_error(fs, line, "module name constant overflow");
    }

    /* Add as local: alias if present, otherwise module name */
    const char *name = stmt->as.import_stmt.alias;
    uint32_t name_len = (uint32_t)stmt->as.import_stmt.alias_len;
    if (name == NULL) {
        name     = stmt->as.import_stmt.module;
        name_len = (uint32_t)stmt->as.import_stmt.module_len;
    }

    /* Rewind free_reg since add_local will re-allocate */
    fs->free_reg--;
    novai_add_local(fs, name, name_len);
}

/* ---- Statement: export ---- */

static void novai_compile_stmt_export(NovaFuncState *fs,
                                      NovaRowStmt *stmt) {
    uint32_t line = LOC_LINE(stmt->loc);

    NovaExprDesc val;
    memset(&val, 0, sizeof(val));
    novai_compile_expr(fs, stmt->as.export_stmt.value, &val);
    novai_discharge_to_any(fs, &val);

    /* For now, EXPORT just marks the value. Full module system later. */
    nova_proto_emit_abc(PROTO(fs), NOVA_OP_EXPORT,
                        val.u.reg, 0, 0, line);
    novai_expr_free(fs, &val);
}

/* ---- Statement: const ---- */

static void novai_compile_stmt_const(NovaFuncState *fs,
                                     NovaRowStmt *stmt) {
    uint32_t line = LOC_LINE(stmt->loc);

    /* Compile value */
    NovaExprDesc val;
    memset(&val, 0, sizeof(val));
    novai_compile_expr(fs, stmt->as.const_stmt.value, &val);
    novai_discharge_to_next(fs, &val);

    /* Add as local with const flag */
    fs->free_reg--;
    novai_add_local(fs, stmt->as.const_stmt.name,
                    (uint32_t)stmt->as.const_stmt.name_len);
    /* Mark as const */
    fs->locals[fs->active_locals - 1].is_const = 1;
    (void)line;
}

/* ---- Statement: enum ---- */

static void novai_compile_stmt_enum(NovaFuncState *fs,
                                    NovaRowStmt *stmt) {
    uint32_t line = LOC_LINE(stmt->loc);
    int member_count = stmt->as.enum_stmt.member_count;

    /* Create table: R(table_reg) = {} */
    uint8_t table_reg = novai_alloc_reg(fs);
    nova_proto_emit_abc(PROTO(fs), NOVA_OP_NEWTABLE,
                        table_reg,
                        0,
                        (uint8_t)(member_count > 255 ? 255 : member_count),
                        line);

    NovaNameIdx  ns = stmt->as.enum_stmt.members_start;
    NovaExtraIdx vs = stmt->as.enum_stmt.values_start;
    nova_int_t auto_val = 0; /* running auto-increment counter */

    for (int i = 0; i < member_count; i++) {
        NovaRowNameRef *nm = &AST(fs)->names[ns + (uint32_t)i];

        /* Key: constant string for member name */
        uint32_t key_kidx = nova_proto_find_or_add_string(
            PROTO(fs), nm->data, (uint32_t)nm->len);
        if (key_kidx > NOVA_MAX_RK_CONST) {
            novai_error(fs, line, "enum member name constant overflow");
            return;
        }
        uint8_t key_rk = (uint8_t)NOVA_CONST_TO_RK(key_kidx);

        /* Value: explicit expression or auto-increment integer */
        NovaExprIdx val_idx = nova_get_extra_expr(AST(fs), vs, i);
        uint8_t val_rk;

        if (val_idx != NOVA_IDX_NONE) {
            /* Explicit value: compile the expression */
            NovaExprDesc val;
            memset(&val, 0, sizeof(val));
            novai_compile_expr(fs, val_idx, &val);
            val_rk = novai_expr_to_rk(fs, &val, line);
            nova_proto_emit_abc(PROTO(fs), NOVA_OP_SETTABLE,
                                table_reg, key_rk, val_rk, line);
            novai_expr_free(fs, &val);
            /* Update auto counter if explicit value is integer literal */
            NovaRowExpr *vexpr = &AST(fs)->exprs[val_idx];
            if (vexpr->kind == NOVA_EXPR_INTEGER) {
                auto_val = vexpr->as.integer.value + 1;
            } else {
                auto_val++;
            }
        } else {
            /* Auto-value: running counter */
            uint32_t int_kidx = nova_proto_add_integer(PROTO(fs),
                                                       auto_val);
            if (int_kidx > NOVA_MAX_RK_CONST) {
                novai_error(fs, line,
                            "enum auto-value constant overflow");
                return;
            }
            val_rk = (uint8_t)NOVA_CONST_TO_RK(int_kidx);
            nova_proto_emit_abc(PROTO(fs), NOVA_OP_SETTABLE,
                                table_reg, key_rk, val_rk, line);
            auto_val++;
        }
    }

    /* Typed enum: add __type and __base metadata */
    if (stmt->as.enum_stmt.typed) {
        /* __type = "EnumName" */
        uint32_t tkey = nova_proto_find_or_add_string(
            PROTO(fs), "__type", 6);
        uint32_t tval = nova_proto_find_or_add_string(
            PROTO(fs), stmt->as.enum_stmt.name,
            (uint32_t)stmt->as.enum_stmt.name_len);
        if (tkey <= NOVA_MAX_RK_CONST && tval <= NOVA_MAX_RK_CONST) {
            nova_proto_emit_abc(PROTO(fs), NOVA_OP_SETTABLE, table_reg,
                (uint8_t)NOVA_CONST_TO_RK(tkey),
                (uint8_t)NOVA_CONST_TO_RK(tval), line);
        }
        /* __base = "enum" */
        uint32_t bkey = nova_proto_find_or_add_string(
            PROTO(fs), "__base", 6);
        uint32_t bval = nova_proto_find_or_add_string(
            PROTO(fs), "enum", 4);
        if (bkey <= NOVA_MAX_RK_CONST && bval <= NOVA_MAX_RK_CONST) {
            nova_proto_emit_abc(PROTO(fs), NOVA_OP_SETTABLE, table_reg,
                (uint8_t)NOVA_CONST_TO_RK(bkey),
                (uint8_t)NOVA_CONST_TO_RK(bval), line);
        }
    }

    /* Store as global: G[K("EnumName")] = R(table_reg) */
    uint32_t name_kidx = nova_proto_find_or_add_string(
        PROTO(fs), stmt->as.enum_stmt.name,
        (uint32_t)stmt->as.enum_stmt.name_len);
    if (name_kidx <= NOVA_MAX_BX) {
        nova_proto_emit_abx(PROTO(fs), NOVA_OP_SETGLOBAL,
                            table_reg, (uint16_t)name_kidx, line);
    } else {
        novai_error(fs, line, "enum name constant overflow");
    }
    novai_free_reg(fs, table_reg);
}

/* ---- Statement: struct ---- */

static void novai_compile_stmt_struct(NovaFuncState *fs,
                                      NovaRowStmt *stmt) {
    uint32_t line = LOC_LINE(stmt->loc);
    int field_count = stmt->as.struct_stmt.field_count;

    /*
     * Struct compiles to a constructor closure:
     *
     *   function StructName(arg0, arg1, ...)
     *       dec t = {}
     *       t.field0 = arg0 ~= nil and arg0 or default0
     *       t.field1 = arg1 ~= nil and arg1 or default1
     *       ...
     *       t.__type = "StructName"
     *       return t
     *   end
     *
     * Simplified codegen (no nil-check, positional args override
     * defaults; if arg is nil, default is used via TESTSET):
     *
     * For each field:
     *   If argument was passed (param reg), use it.
     *   If not passed or nil, use default.
     *   We emit: MOVE param to temp, TEST, use default if nil.
     *
     * Actually, for simplicity and correctness, let's just
     * emit straightforward code: the constructor takes N params.
     * For each field, if the param is nil and a default exists,
     * use the default. Otherwise use the param.
     */

    /* Create child proto for constructor */
    NovaProto *child = nova_proto_create();
    if (child == NULL) {
        novai_error(fs, line, "out of memory creating struct proto");
        return;
    }

    child->source       = fs->compiler->source;
    child->line_defined = line;
    child->num_params   = (uint8_t)field_count;
    child->is_vararg    = 0;
    child->is_async     = 0;

    /* Set up child FuncState */
    NovaFuncState child_fs;
    novai_init_funcstate(&child_fs, child, fs, fs->compiler);

    novai_enter_scope(&child_fs, 0);

    /* Add params as locals (one per field) */
    NovaNameIdx fns = stmt->as.struct_stmt.fields_start;
    for (int i = 0; i < field_count; i++) {
        NovaRowNameRef *nm = &AST(fs)->names[fns + (uint32_t)i];
        novai_add_local(&child_fs, nm->data, (uint32_t)nm->len);
    }

    /* Create table: R(table_reg) = {} */
    uint8_t table_reg = novai_alloc_reg(&child_fs);
    nova_proto_emit_abc(child, NOVA_OP_NEWTABLE,
                        table_reg, 0,
                        (uint8_t)(field_count + 1 > 255
                                  ? 255 : field_count + 1),
                        line);

    /* For each field, set table[field_name] = param_or_default */
    NovaExtraIdx ds = stmt->as.struct_stmt.defaults_start;
    for (int i = 0; i < field_count; i++) {
        NovaRowNameRef *nm = &AST(fs)->names[fns + (uint32_t)i];

        /* Key: field name as string constant */
        uint32_t key_kidx = nova_proto_find_or_add_string(
            child, nm->data, (uint32_t)nm->len);
        if (key_kidx > NOVA_MAX_RK_CONST) {
            novai_error(&child_fs, line,
                        "struct field name constant overflow");
            break;
        }
        uint8_t key_rk = (uint8_t)NOVA_CONST_TO_RK(key_kidx);

        /* Value: param register is i (0-based) */
        uint8_t param_reg = (uint8_t)i;

        /* Check if there's a default value */
        NovaExprIdx def_idx = nova_get_extra_expr(AST(fs), ds, i);

        if (def_idx != NOVA_IDX_NONE) {
            /* Has default: if param is nil, use default.
             *
             * Emit:
             *   TEST param_reg, 0     -- if param is falsy, skip
             *   JMP +2                -- param is truthy, use it
             *   LOADK temp, default   -- load default
             *   MOVE param_reg, temp  -- (we actually just set the
             *                            field directly below)
             *
             * Simpler approach: use TESTSET to conditionally
             * select param vs default:
             *
             *   R(temp) = default
             *   TESTSET R(temp), R(param_reg), 0
             *     -- if param is falsy: R(temp) = param stays default
             *     -- if param is truthy: R(temp) = R(param), skip JMP
             *   JMP +0 (patched)
             *
             * Actually even simpler: just SETFIELD with param.
             * If caller doesn't pass an arg, it will be nil.
             *
             * We'll use a test-and-branch approach:
             *   TEST R(param_reg), 1   -- test if truthy
             *   JMP over_default       -- if truthy, skip default
             *   [compile default to temp]
             *   SETFIELD table, key, temp
             *   JMP over_param_set
             * over_default:
             *   SETFIELD table, key, param_reg
             * over_param_set:
             *
             * Actually, let's keep it simple. A struct with defaults
             * should work like: if the argument is nil, use default.
             * The simplest codegen:
             *
             *   TEST R(param), 1        -- skip next if truthy
             *   JMP  +N                 -- jump to param_set
             *   <compile default to R(temp)>
             *   SETFIELD table, key, R(temp)
             *   JMP  +1                 -- skip param_set
             * param_set:
             *   SETFIELD table, key, param
             */
            uint8_t temp_reg = novai_alloc_reg(&child_fs);

            /* TEST param, 1: if param is truthy (non-nil/non-false),
             * skip next instruction */
            nova_proto_emit_abc(child, NOVA_OP_TEST,
                                param_reg, 0, 1, line);
            /* JMP to param_set (patched below) */
            uint32_t jmp_to_param = nova_proto_pc(child);
            nova_proto_emit_abx(child, NOVA_OP_JMP, 0, NOVA_SBX_BIAS,
                                line);

            /* Compile default value */
            NovaExprDesc def_val;
            memset(&def_val, 0, sizeof(def_val));
            novai_compile_expr(&child_fs, def_idx, &def_val);
            novai_discharge_to_reg(&child_fs, &def_val, temp_reg);

            /* SETTABLE table, key, temp (default) */
            nova_proto_emit_abc(child, NOVA_OP_SETTABLE,
                                table_reg, key_rk, temp_reg, line);

            /* JMP over param_set */
            uint32_t jmp_over = nova_proto_pc(child);
            nova_proto_emit_abx(child, NOVA_OP_JMP, 0, NOVA_SBX_BIAS,
                                line);

            /* Patch jmp_to_param: target is current pc */
            uint32_t param_set_pc = nova_proto_pc(child);
            int32_t offset1 = (int32_t)param_set_pc -
                              (int32_t)jmp_to_param - 1;
            NOVA_SET_SBX(child->code[jmp_to_param], offset1);

            /* SETTABLE table, key, param_reg */
            nova_proto_emit_abc(child, NOVA_OP_SETTABLE,
                                table_reg, key_rk, param_reg, line);

            /* Patch jmp_over: target is current pc */
            uint32_t after_pc = nova_proto_pc(child);
            int32_t offset2 = (int32_t)after_pc -
                              (int32_t)jmp_over - 1;
            NOVA_SET_SBX(child->code[jmp_over], offset2);

            novai_free_reg(&child_fs, temp_reg);
        } else {
            /* No default: just set field from param (might be nil) */
            nova_proto_emit_abc(child, NOVA_OP_SETTABLE,
                                table_reg, key_rk, param_reg, line);
        }
    }

    /* Set __type field: table.__type = "StructName" */
    uint32_t type_key_kidx = nova_proto_find_or_add_string(
        child, "__type", 6);
    uint32_t type_val_kidx = nova_proto_find_or_add_string(
        child, stmt->as.struct_stmt.name,
        (uint32_t)stmt->as.struct_stmt.name_len);
    if (type_key_kidx <= NOVA_MAX_RK_CONST &&
        type_val_kidx <= NOVA_MAX_RK_CONST) {
        nova_proto_emit_abc(child, NOVA_OP_SETTABLE,
            table_reg,
            (uint8_t)NOVA_CONST_TO_RK(type_key_kidx),
            (uint8_t)NOVA_CONST_TO_RK(type_val_kidx),
            line);
    }

    /* Typed struct: add __base = "struct" */
    if (stmt->as.struct_stmt.typed) {
        uint32_t bkey = nova_proto_find_or_add_string(
            child, "__base", 6);
        uint32_t bval = nova_proto_find_or_add_string(
            child, "struct", 6);
        if (bkey <= NOVA_MAX_RK_CONST && bval <= NOVA_MAX_RK_CONST) {
            nova_proto_emit_abc(child, NOVA_OP_SETTABLE,
                table_reg,
                (uint8_t)NOVA_CONST_TO_RK(bkey),
                (uint8_t)NOVA_CONST_TO_RK(bval),
                line);
        }
    }

    /* Return the table */
    nova_proto_emit_abc(child, NOVA_OP_RETURN1, table_reg, 0, 0, line);
    child->last_line = line;

    novai_leave_scope(&child_fs);
    fs->compiler->current_fs = fs;

    /* Add child to parent and emit CLOSURE */
    uint32_t child_idx = nova_proto_add_child(PROTO(fs), child);
    uint8_t dest_reg = novai_alloc_reg(fs);

    if (child_idx <= NOVA_MAX_BX) {
        nova_proto_emit_abx(PROTO(fs), NOVA_OP_CLOSURE,
                            dest_reg, (uint16_t)child_idx, line);
    } else {
        novai_error(fs, line, "too many nested functions");
        novai_free_reg(fs, dest_reg);
        return;
    }

    /* Emit upvalue pseudo-instructions after CLOSURE */
    for (uint8_t i = 0; i < child->upvalue_count; i++) {
        nova_proto_emit_abc(PROTO(fs), NOVA_OP_MOVE,
                            child->upvalues[i].in_stack,
                            child->upvalues[i].index, 0, line);
    }

    /* Store as global: G[K("StructName")] = R(dest_reg) */
    uint32_t name_kidx = nova_proto_find_or_add_string(
        PROTO(fs), stmt->as.struct_stmt.name,
        (uint32_t)stmt->as.struct_stmt.name_len);
    if (name_kidx <= NOVA_MAX_BX) {
        nova_proto_emit_abx(PROTO(fs), NOVA_OP_SETGLOBAL,
                            dest_reg, (uint16_t)name_kidx, line);
    } else {
        novai_error(fs, line, "struct name constant overflow");
    }
    novai_free_reg(fs, dest_reg);
}

/* ---- Statement: typedec ---- */

static void novai_compile_stmt_typedec(NovaFuncState *fs,
                                       NovaRowStmt *stmt) {
    uint32_t line = LOC_LINE(stmt->loc);

    /*
     * typedec creates a constructor function:
     *
     *   function TypeName(value)
     *       dec t = {}
     *       t.__type = "TypeName"
     *       t.__base = "base_type"
     *       t.value  = value
     *       return t
     *   end
     */

    NovaProto *child = nova_proto_create();
    if (child == NULL) {
        novai_error(fs, line, "out of memory creating typedec proto");
        return;
    }

    child->source       = fs->compiler->source;
    child->line_defined = line;
    child->num_params   = 1;
    child->is_vararg    = 0;
    child->is_async     = 0;

    NovaFuncState child_fs;
    novai_init_funcstate(&child_fs, child, fs, fs->compiler);

    novai_enter_scope(&child_fs, 0);

    /* Single param: "value" */
    novai_add_local(&child_fs, "value", 5);
    uint8_t val_param = 0;  /* param register */

    /* Create table */
    uint8_t table_reg = novai_alloc_reg(&child_fs);
    nova_proto_emit_abc(child, NOVA_OP_NEWTABLE,
                        table_reg, 0, 3, line);

    /* t.__type = "TypeName" */
    uint32_t type_key = nova_proto_find_or_add_string(
        child, "__type", 6);
    uint32_t type_val = nova_proto_find_or_add_string(
        child, stmt->as.typedec_stmt.name,
        (uint32_t)stmt->as.typedec_stmt.name_len);
    if (type_key <= NOVA_MAX_RK_CONST &&
        type_val <= NOVA_MAX_RK_CONST) {
        nova_proto_emit_abc(child, NOVA_OP_SETTABLE,
            table_reg,
            (uint8_t)NOVA_CONST_TO_RK(type_key),
            (uint8_t)NOVA_CONST_TO_RK(type_val),
            line);
    }

    /* t.__base = "base_type" */
    uint32_t base_key = nova_proto_find_or_add_string(
        child, "__base", 6);
    uint32_t base_val = nova_proto_find_or_add_string(
        child, stmt->as.typedec_stmt.base_type,
        (uint32_t)stmt->as.typedec_stmt.base_type_len);
    if (base_key <= NOVA_MAX_RK_CONST &&
        base_val <= NOVA_MAX_RK_CONST) {
        nova_proto_emit_abc(child, NOVA_OP_SETTABLE,
            table_reg,
            (uint8_t)NOVA_CONST_TO_RK(base_key),
            (uint8_t)NOVA_CONST_TO_RK(base_val),
            line);
    }

    /* t.value = value (param reg 0) */
    uint32_t val_key = nova_proto_find_or_add_string(
        child, "value", 5);
    if (val_key <= NOVA_MAX_RK_CONST) {
        nova_proto_emit_abc(child, NOVA_OP_SETTABLE,
            table_reg,
            (uint8_t)NOVA_CONST_TO_RK(val_key),
            val_param,
            line);
    }

    /* Return the table */
    nova_proto_emit_abc(child, NOVA_OP_RETURN1, table_reg, 0, 0, line);
    child->last_line = line;

    novai_leave_scope(&child_fs);
    fs->compiler->current_fs = fs;

    /* Add child to parent and emit CLOSURE */
    uint32_t child_idx = nova_proto_add_child(PROTO(fs), child);
    uint8_t dest_reg = novai_alloc_reg(fs);

    if (child_idx <= NOVA_MAX_BX) {
        nova_proto_emit_abx(PROTO(fs), NOVA_OP_CLOSURE,
                            dest_reg, (uint16_t)child_idx, line);
    } else {
        novai_error(fs, line, "too many nested functions");
        novai_free_reg(fs, dest_reg);
        return;
    }

    /* Emit upvalue pseudo-instructions */
    for (uint8_t i = 0; i < child->upvalue_count; i++) {
        nova_proto_emit_abc(PROTO(fs), NOVA_OP_MOVE,
                            child->upvalues[i].in_stack,
                            child->upvalues[i].index, 0, line);
    }

    /* Store as global */
    uint32_t name_kidx = nova_proto_find_or_add_string(
        PROTO(fs), stmt->as.typedec_stmt.name,
        (uint32_t)stmt->as.typedec_stmt.name_len);
    if (name_kidx <= NOVA_MAX_BX) {
        nova_proto_emit_abx(PROTO(fs), NOVA_OP_SETGLOBAL,
                            dest_reg, (uint16_t)name_kidx, line);
    } else {
        novai_error(fs, line, "typedec name constant overflow");
    }
    novai_free_reg(fs, dest_reg);
}

/* ============================================================
 * MAIN STATEMENT DISPATCH
 * ============================================================ */

static void novai_compile_stmt(NovaFuncState *fs, NovaStmtIdx idx) {
    if (idx == NOVA_IDX_NONE) {
        return;
    }

    NovaRowStmt *stmt = nova_get_stmt(AST(fs), idx);

    switch (stmt->kind) {
        case NOVA_STMT_EXPR:
            novai_compile_stmt_expr(fs, stmt);
            break;

        case NOVA_STMT_LOCAL:
            novai_compile_stmt_local(fs, stmt);
            break;

        case NOVA_STMT_ASSIGN:
            novai_compile_stmt_assign(fs, stmt);
            break;

        case NOVA_STMT_IF:
            novai_compile_stmt_if(fs, stmt);
            break;

        case NOVA_STMT_WHILE:
            novai_compile_stmt_while(fs, stmt);
            break;

        case NOVA_STMT_REPEAT:
            novai_compile_stmt_repeat(fs, stmt);
            break;

        case NOVA_STMT_FOR_NUMERIC:
            novai_compile_stmt_for_numeric(fs, stmt);
            break;

        case NOVA_STMT_FOR_GENERIC:
            novai_compile_stmt_for_generic(fs, stmt);
            break;

        case NOVA_STMT_DO:
            novai_compile_stmt_do(fs, stmt);
            break;

        case NOVA_STMT_BREAK:
            novai_compile_stmt_break(fs, stmt);
            break;

        case NOVA_STMT_CONTINUE:
            novai_compile_stmt_continue(fs, stmt);
            break;

        case NOVA_STMT_GOTO:
            novai_compile_stmt_goto(fs, stmt);
            break;

        case NOVA_STMT_LABEL:
            novai_compile_stmt_label(fs, stmt);
            break;

        case NOVA_STMT_RETURN:
            novai_compile_stmt_return(fs, stmt);
            break;

        case NOVA_STMT_FUNCTION:
            novai_compile_stmt_function(fs, stmt, 0);
            break;

        case NOVA_STMT_LOCAL_FUNCTION:
            novai_compile_stmt_function(fs, stmt, 1);
            break;

        case NOVA_STMT_IMPORT:
            novai_compile_stmt_import(fs, stmt);
            break;

        case NOVA_STMT_EXPORT:
            novai_compile_stmt_export(fs, stmt);
            break;

        case NOVA_STMT_CONST:
            novai_compile_stmt_const(fs, stmt);
            break;

        case NOVA_STMT_ENUM:
            novai_compile_stmt_enum(fs, stmt);
            break;

        case NOVA_STMT_STRUCT:
            novai_compile_stmt_struct(fs, stmt);
            break;

        case NOVA_STMT_TYPEDEC:
            novai_compile_stmt_typedec(fs, stmt);
            break;

        default:
            novai_error(fs, LOC_LINE(stmt->loc),
                        "unknown statement type in compiler");
            break;
    }
}

/* ============================================================
 * BLOCK COMPILATION
 * ============================================================ */

static void novai_compile_block(NovaFuncState *fs, NovaBlockIdx idx) {
    if (idx == NOVA_IDX_NONE) {
        return;
    }

    NovaRowBlock *blk = nova_get_block(AST(fs), idx);
    NovaStmtIdx stmt_idx = blk->first;

    while (stmt_idx != NOVA_IDX_NONE) {
        novai_compile_stmt(fs, stmt_idx);

        NovaRowStmt *stmt = nova_get_stmt(AST(fs), stmt_idx);
        stmt_idx = stmt->next;
    }
}

/* ============================================================
 * PART 4: PUBLIC API
 * ============================================================ */

NovaProto *nova_compile(const NovaASTTable *ast, const char *source) {
    if (ast == NULL || source == NULL) {
        return NULL;
    }

    /* Create top-level proto */
    NovaProto *proto = nova_proto_create();
    if (proto == NULL) {
        return NULL;
    }

    proto->source     = source;
    proto->is_vararg  = 1;  /* Top-level chunk is always vararg */
    proto->num_params = 0;

    /* Set up compiler state */
    NovaCompiler compiler;
    memset(&compiler, 0, sizeof(compiler));
    compiler.ast    = ast;
    compiler.source = source;

    /* Set up top-level FuncState */
    NovaFuncState fs;
    novai_init_funcstate(&fs, proto, NULL, &compiler);

    /* Enter top-level scope */
    novai_enter_scope(&fs, 0);

    /* Compile root block */
    if (ast->root != NOVA_IDX_NONE) {
        novai_compile_block(&fs, ast->root);
    }

    /* Check for unresolved gotos before closing top-level function */
    novai_check_unresolved_gotos(&fs);

    /* Emit implicit RETURN0 at end */
    nova_proto_emit(proto,
                    NOVA_ENCODE_ABC(NOVA_OP_RETURN0, 0, 0, 0),
                    0);

    /* Leave top-level scope */
    novai_leave_scope(&fs);

    /* Record max stack */
    if (fs.free_reg > proto->max_stack) {
        proto->max_stack = fs.free_reg;
    }

    /* Check for errors */
    if (compiler.had_error) {
        nova_proto_destroy(proto);
        return NULL;
    }

    return proto;
}

NovaProto *nova_compile_expression(const NovaASTTable *ast,
                                   NovaExprIdx expr,
                                   const char *source) {
    if (ast == NULL || source == NULL || expr == NOVA_IDX_NONE) {
        return NULL;
    }

    /* Create wrapper proto */
    NovaProto *proto = nova_proto_create();
    if (proto == NULL) {
        return NULL;
    }

    proto->source     = source;
    proto->is_vararg  = 0;
    proto->num_params = 0;

    /* Set up compiler state */
    NovaCompiler compiler;
    memset(&compiler, 0, sizeof(compiler));
    compiler.ast    = ast;
    compiler.source = source;

    /* Set up FuncState */
    NovaFuncState fs;
    novai_init_funcstate(&fs, proto, NULL, &compiler);

    novai_enter_scope(&fs, 0);

    /* Compile the expression */
    NovaExprDesc e;
    memset(&e, 0, sizeof(e));
    novai_compile_expr(&fs, expr, &e);
    novai_discharge_to_any(&fs, &e);

    /* Return the result */
    nova_proto_emit_abc(proto, NOVA_OP_RETURN1,
                        e.u.reg, 0, 0, 0);

    novai_leave_scope(&fs);

    if (fs.free_reg > proto->max_stack) {
        proto->max_stack = fs.free_reg;
    }

    if (compiler.had_error) {
        nova_proto_destroy(proto);
        return NULL;
    }

    return proto;
}
