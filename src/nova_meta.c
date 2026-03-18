/**
 * @file nova_meta.c
 * @brief Centralized metamethod pipeline for the Nova VM
 *
 * @author Anthony Taliento
 * @date 2026-02-08
 * @version 1.0.0
 *
 * @copyright Copyright (c) 2026 Zorya Corporation
 * @license MIT
 *
 * DESCRIPTION:
 *   Single module for ALL metamethod resolution and invocation.
 *   The VM dispatch loop calls into these functions; no metamethod
 *   logic exists anywhere else.
 *
 *   Designed to be clean, auditable, and debuggable:
 *   - One place to set breakpoints for any metamethod
 *   - One recursion guard that covers all chains
 *   - One invocation path for calling metamethods
 *
 * DEPENDENCIES:
 *   - nova_vm.h (NovaVM, NovaValue, NovaTable)
 *   - nova_meta.h (public API)
 *   - zorya/nxh.h (for string hashing during name intern)
 *
 * THREAD SAFETY:
 *   Not thread-safe.  Each VM instance is single-threaded.
 */

#include "nova/nova_meta.h"
#include "nova/nova_vm.h"
#include "nova/nova_opcode.h"
#include "nova/nova_trace.h"
#include "nova/nova_error.h"
#include "zorya/nxh.h"
#include "zorya/pcm.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

/* NOVA_STRING_SEED comes from nova_vm.h (single source of truth) */

/* === Per-type metatables (module-global, single-threaded) === */
static NovaTable *g_string_mt = NULL;

/* Forward declarations for functions defined later in this file */
static int nova_meta_raw_set(NovaVM *vm, NovaTable *t,
                             NovaValue key, NovaValue val);

/* ============================================================
 * METAMETHOD NAME TABLE
 * ============================================================
 *
 * Canonical string names for each metamethod tag.
 * Order MUST match the NovaMetaTag enum exactly.
 */

static const char *const nova_meta_names[NOVA_TM__COUNT] = {
    "__index",      /* NOVA_TM_INDEX     */
    "__newindex",   /* NOVA_TM_NEWINDEX  */
    "__call",       /* NOVA_TM_CALL      */
    "__add",        /* NOVA_TM_ADD       */
    "__sub",        /* NOVA_TM_SUB       */
    "__mul",        /* NOVA_TM_MUL       */
    "__div",        /* NOVA_TM_DIV       */
    "__mod",        /* NOVA_TM_MOD       */
    "__pow",        /* NOVA_TM_POW       */
    "__unm",        /* NOVA_TM_UNM       */
    "__idiv",       /* NOVA_TM_IDIV      */
    "__band",       /* NOVA_TM_BAND      */
    "__bor",        /* NOVA_TM_BOR       */
    "__bxor",       /* NOVA_TM_BXOR      */
    "__bnot",       /* NOVA_TM_BNOT      */
    "__shl",        /* NOVA_TM_SHL       */
    "__shr",        /* NOVA_TM_SHR       */
    "__eq",         /* NOVA_TM_EQ        */
    "__lt",         /* NOVA_TM_LT        */
    "__le",         /* NOVA_TM_LE        */
    "__concat",     /* NOVA_TM_CONCAT    */
    "__len",        /* NOVA_TM_LEN       */
    "__tostring",   /* NOVA_TM_TOSTRING  */
    "__gc",         /* NOVA_TM_GC        */
    "__metatable",  /* NOVA_TM_METATABLE */
};

/* ============================================================
 * INTERNAL: TABLE RAW ACCESS
 * ============================================================
 *
 * We need raw table get/set that bypass metamethods.
 *
 * Phase 10.5: These now delegate to the public table API
 * in nova_vm.c instead of duplicating the hash probing logic.
 */

/**
 * @brief Raw get by NovaValue key (dispatches to int or str).
 *
 * Uses the public nova_table_get_str/int API. No more
 * duplicated linear probing — if the table backend changes
 * to DAGGER, this function doesn't need to change.
 */
static NovaValue novai_meta_raw_get(NovaTable *t, NovaValue key) {
    if (nova_is_integer(key)) {
        return nova_table_get_int(t, nova_as_integer(key));
    }
    if (nova_is_string(key)) {
        return nova_table_get_str(t, nova_as_string(key));
    }
    if (nova_is_number(key)) {
        nova_int_t ik = (nova_int_t)nova_as_number(key);
        if ((nova_number_t)ik == nova_as_number(key)) {
            return nova_table_get_int(t, ik);
        }
    }
    return nova_value_nil();
}

/**
 * @brief Lookup a metamethod by C string name.
 *
 * Uses nova_string_new to create a temporary key, then
 * delegates to nova_table_get_str. This is only called
 * for metamethod lookups which are not hot-path.
 */
static NovaValue novai_meta_raw_get_str_cstr(NovaTable *t,
                                             const char *key,
                                             uint32_t key_len) {
    if (t == NULL) {
        return nova_value_nil();
    }
    return nova_table_get_cstr(t, key, key_len);
}

/* ============================================================
 * INTERNAL: VM CALL HELPERS
 * ============================================================
 *
 * Call a metamethod value (closure or cfunc) from C code.
 * This pushes args onto the VM stack, invokes the function,
 * and retrieves results.
 *
 * This is the SINGLE invocation codepath for all metamethods.
 */

/**
 * @brief Internal: invoke a callable value with N args, M results.
 *
 * @param vm       VM instance
 * @param func     The function/closure/cfunc to call
 * @param args     Array of argument values
 * @param nargs    Number of arguments
 * @param results  [out] Array of result values (must be pre-allocated)
 * @param nresults Number of results wanted
 * @return         0 on success, -1 on error
 */
static int novai_meta_invoke(NovaVM *vm, NovaValue func,
                             const NovaValue *args, int nargs,
                             NovaValue *results, int nresults) {
    if (vm == NULL) {
        return -1;
    }

    NTRACE(META, "meta_invoke nargs=%d nresults=%d func_type=%d",
           nargs, nresults, nova_typeof(func));

    /* ---- C function fast path ---- */
    if (nova_is_cfunction(func)) {
        /* Set up cfunc_base to point at the args */
        NovaValue *save_cfunc_base = vm->cfunc_base;
        NovaValue *save_stack_top = vm->stack_top;

        /* Find safe area above current frame's register space.
         * stack_top may be WITHIN the current frame's max_stack,
         * so we must go past it to avoid clobbering live locals. */
        NovaValue *safe_start = vm->stack_top;
        if (vm->frame_count > 0) {
            NovaCallFrame *cur = &vm->frames[vm->frame_count - 1];
            NovaValue *frame_end = cur->base + cur->proto->max_stack;
            if (frame_end > safe_start) {
                safe_start = frame_end;
            }
        }

        /* Push args to stack (ensure space) */
        ptrdiff_t safe_off = safe_start - vm->stack;
        if (safe_off + nargs + 4 >= (ptrdiff_t)vm->stack_size) {
            /* Need more stack - grow it */
            uint32_t new_size = (uint32_t)(vm->stack_size * 2);
            if (new_size < (uint32_t)(safe_off + nargs + 64)) {
                new_size = (uint32_t)(safe_off + nargs + 64);
            }
            NovaValue *new_stack = (NovaValue *)realloc(
                vm->stack, new_size * sizeof(NovaValue));
            if (new_stack == NULL) {
                vm->cfunc_base = save_cfunc_base;
                return -1;
            }
            /* Fix up pointers - save_stack_top is now invalid */
            save_stack_top = new_stack + (save_stack_top - vm->stack);
            if (save_cfunc_base != NULL) {
                save_cfunc_base = new_stack +
                    (save_cfunc_base - vm->stack);
            }
            /* Fix up frame bases */
            for (int i = 0; i < vm->frame_count; i++) {
                vm->frames[i].base = new_stack +
                    (vm->frames[i].base - vm->stack);
            }
            safe_start = new_stack + safe_off;
            vm->stack = new_stack;
            vm->stack_size = new_size;
            vm->stack_top = save_stack_top;
        }

        NovaValue *arg_base = safe_start;
        for (int i = 0; i < nargs; i++) {
            arg_base[i] = args[i];
        }
        vm->cfunc_base = arg_base;
        vm->stack_top = arg_base + nargs;

        int got = nova_as_cfunction(func)(vm);

        if (got < 0 || vm->status != NOVA_VM_OK) {
            vm->cfunc_base = save_cfunc_base;
            vm->stack_top = save_stack_top;
            return -1;
        }

        /* Copy results out */
        NovaValue *res_start = arg_base + nargs;
        for (int i = 0; i < nresults; i++) {
            if (i < got) {
                results[i] = res_start[i];
            } else {
                results[i] = nova_value_nil();
            }
        }

        vm->cfunc_base = save_cfunc_base;
        vm->stack_top = save_stack_top;
        return 0;
    }

    /* ---- Nova closure path ---- */
    if (nova_is_function(func)) {
        NovaClosure *cl = nova_as_closure(func);
        const NovaProto *callee = cl->proto;

        if (vm->frame_count >= NOVA_MAX_CALL_DEPTH) {
            /* Cannot push frame - stack overflow */
            return -1;
        }

        /* Save stack state as OFFSETS (pointers invalidated by realloc) */
        ptrdiff_t save_top_off = vm->stack_top - vm->stack;

        /* Find safe area above current frame's register space.
         * stack_top may be within the current frame's max_stack,
         * so we must go past it to avoid clobbering live locals. */
        NovaValue *safe_start = vm->stack_top;
        if (vm->frame_count > 0) {
            NovaCallFrame *cur = &vm->frames[vm->frame_count - 1];
            NovaValue *frame_end = cur->base + cur->proto->max_stack;
            if (frame_end > safe_start) {
                safe_start = frame_end;
            }
        }
        ptrdiff_t safe_off = safe_start - vm->stack;

        /* Ensure stack space for callee (extra +1 for function slot) */
        ptrdiff_t needed = safe_off + 1 + nargs + callee->max_stack + 20;
        if (needed >= (ptrdiff_t)vm->stack_size) {
            uint32_t new_size = (uint32_t)(vm->stack_size * 2);
            if (new_size < (uint32_t)needed + 64) {
                new_size = (uint32_t)needed + 64;
            }
            NovaValue *new_stack = (NovaValue *)realloc(
                vm->stack, new_size * sizeof(NovaValue));
            if (new_stack == NULL) {
                return -1;
            }
            /* Fix up all pointers */
            ptrdiff_t delta = new_stack - vm->stack;
            for (int i = 0; i < vm->frame_count; i++) {
                vm->frames[i].base += delta;
            }
            vm->stack = new_stack;
            vm->stack_size = new_size;
            vm->stack_top = vm->stack + save_top_off;
        }

        /* Push args at safe position, with function slot at [0] */
        NovaValue *call_base = vm->stack + safe_off;
        ptrdiff_t call_base_off = safe_off;
        call_base[0] = func;  /* Function slot (RETURN writes to base-1) */
        for (int i = 0; i < nargs; i++) {
            call_base[1 + i] = args[i];
        }
        /* Pad with nil if callee expects more params */
        for (int i = nargs; i < callee->num_params; i++) {
            call_base[1 + i] = nova_value_nil();
        }

        /* Save current frame's IP if there is a frame */
        int saved_frame_count = vm->frame_count;

        /* Push new frame - base points past the function slot */
        NovaCallFrame *new_frame = &vm->frames[vm->frame_count++];
        new_frame->proto = callee;
        new_frame->closure = cl;
        new_frame->base = call_base + 1;  /* Past function slot */
        new_frame->ip = callee->code;
        new_frame->num_results = nresults;
        new_frame->num_args = nargs;
        new_frame->varargs = NULL;
        new_frame->num_varargs = 0;

        vm->stack_top = call_base + 1 +
            (callee->max_stack > callee->num_params
             ? callee->max_stack : callee->num_params);

        /* Execute until this frame returns.
         * nova_vm_execute runs from current frame and returns when
         * frame_count drops back to saved_frame_count. */
        int exec_result = nova_vm_execute_frames(vm, saved_frame_count);

        if (exec_result != NOVA_VM_OK) {
            vm->stack_top = vm->stack + save_top_off;
            return -1;
        }

        /* Results are at call_base[0..nresults-1]
         * Recompute call_base in case stack was reallocated */
        call_base = vm->stack + call_base_off;
        for (int i = 0; i < nresults; i++) {
            results[i] = call_base[i];
        }

        vm->stack_top = vm->stack + save_top_off;
        return 0;
    }

    /* Not callable */
    return -1;
}

/* ============================================================
 * RESOLUTION FUNCTIONS
 * ============================================================ */

NovaTable *nova_meta_get_mt(NovaValue v) {
    if (nova_is_table(v) && nova_as_table(v) != NULL) {
        return nova_table_get_metatable(nova_as_table(v));
    }
    /* Per-type metatables: strings get a shared metatable
     * with __index = string module table, enabling s:find() etc. */
    if (nova_is_string(v) && g_string_mt != NULL) {
        return g_string_mt;
    }
    /* Future: userdata metatables */
    /* Future: per-type metatables for number */
    return NULL;
}

void nova_meta_set_string_mt(NovaVM *vm, NovaTable *mt) {
    g_string_mt = mt;
    if (vm != NULL) {
        vm->string_mt = mt;
    }
}

NovaValue nova_meta_get_method(NovaTable *mt, NovaMetaTag tag) {
    NovaValue nil_val;
    nil_val = nova_value_nil();

    if (mt == NULL || (int)tag < 0 || (int)tag >= NOVA_TM__COUNT) {
        return nil_val;
    }

    const char *name = nova_meta_names[tag];
    uint32_t name_len = (uint32_t)strlen(name);

    return novai_meta_raw_get_str_cstr(mt, name, name_len);
}

NovaValue nova_meta_get_tm(NovaValue v, NovaMetaTag tag) {
    return nova_meta_get_method(nova_meta_get_mt(v), tag);
}

/* ============================================================
 * INVOCATION WRAPPERS
 * ============================================================ */

int nova_meta_call1(NovaVM *vm, NovaValue method,
                    NovaValue arg1, NovaValue *result) {
    NovaValue args[1];
    args[0] = arg1;
    return novai_meta_invoke(vm, method, args, 1, result, 1);
}

int nova_meta_call2(NovaVM *vm, NovaValue method,
                    NovaValue arg1, NovaValue arg2,
                    NovaValue *result) {
    NovaValue args[2];
    args[0] = arg1;
    args[1] = arg2;
    return novai_meta_invoke(vm, method, args, 2, result, 1);
}

int nova_meta_call3(NovaVM *vm, NovaValue method,
                    NovaValue arg1, NovaValue arg2, NovaValue arg3) {
    NovaValue args[3];
    NovaValue dummy;
    args[0] = arg1;
    args[1] = arg2;
    args[2] = arg3;
    return novai_meta_invoke(vm, method, args, 3, &dummy, 0);
}

/* ============================================================
 * HIGH-LEVEL PIPELINE: __index
 * ============================================================ */

int nova_meta_index(NovaVM *vm, NovaValue obj, NovaValue key,
                    NovaValue *result) {
    if (result == NULL) {
        return -1;
    }

    NTRACE(META, "meta_index obj_type=%d key_type=%d",
           nova_typeof(obj), nova_typeof(key));

    *result = nova_value_nil();

    for (int depth = 0; depth < NOVA_META_MAX_DEPTH; depth++) {

        /* Step 1: If it's a table, try raw get */
        if (nova_is_table(obj)) {
            NovaValue raw = novai_meta_raw_get(nova_as_table(obj), key);
            if (!nova_is_nil(raw)) {
                *result = raw;
                return 0;
            }
        }

        /* Step 2: Look for __index metamethod */
        NovaTable *mt = nova_meta_get_mt(obj);
        if (mt == NULL) {
            /* No metatable -- return nil for tables, error for others */
            if (nova_is_table(obj)) {
                *result = nova_value_nil();
                return 0;
            }
            {
                char buf[256];
                snprintf(buf, sizeof(buf),
                         "attempt to index a %s value",
                         nova_vm_typename(nova_typeof(obj)));
                vm->diag_code = NOVA_E2018;
                novai_error(vm, NOVA_VM_ERR_TYPE, buf);
            }
            return -1;
        }

        NovaValue handler = nova_meta_get_method(mt, NOVA_TM_INDEX);
        if (nova_is_nil(handler)) {
            /* Metatable exists but no __index -- return nil */
            if (nova_is_table(obj)) {
                *result = nova_value_nil();
                return 0;
            }
            {
                char buf[256];
                snprintf(buf, sizeof(buf),
                         "attempt to index a %s value (no __index metamethod)",
                         nova_vm_typename(nova_typeof(obj)));
                vm->diag_code = NOVA_E2018;
                novai_error(vm, NOVA_VM_ERR_TYPE, buf);
            }
            return -1;
        }

        /* Step 3a: __index is a table -- chain into it */
        if (nova_is_table(handler)) {
            obj = handler;  /* Recurse with the __index table */
            continue;
        }

        /* Step 3b: __index is a function -- call it */
        if (nova_is_function(handler) ||
            nova_is_cfunction(handler)) {
            return nova_meta_call2(vm, handler, obj, key, result);
        }

        /* __index is something weird -- error */
        vm->diag_code = NOVA_E2011;
        novai_error(vm, NOVA_VM_ERR_TYPE,
                    "invalid __index metamethod (expected table or function)");
        return -1;
    }

    /* Exhausted depth limit */
    vm->diag_code = NOVA_E2024;
    novai_error(vm, NOVA_VM_ERR_TYPE,
                "__index chain too deep (possible loop)");
    return -1;
}

/* ============================================================
 * HIGH-LEVEL PIPELINE: __newindex
 * ============================================================ */

int nova_meta_newindex(NovaVM *vm, NovaValue obj, NovaValue key,
                       NovaValue val) {
    for (int depth = 0; depth < NOVA_META_MAX_DEPTH; depth++) {

        /* Must be a table (or have __newindex) */
        if (!nova_is_table(obj)) {
            NovaValue handler = nova_meta_get_tm(obj, NOVA_TM_NEWINDEX);
            if (!nova_is_nil(handler)) {
                return nova_meta_call3(vm, handler, obj, key, val);
            }
            {
                char buf[256];
                snprintf(buf, sizeof(buf),
                         "attempt to index a %s value",
                         nova_vm_typename(nova_typeof(obj)));
                vm->diag_code = NOVA_E2018;
                novai_error(vm, NOVA_VM_ERR_TYPE, buf);
            }
            return -1;
        }

        NovaTable *t = nova_as_table(obj);

        /* Check if key already exists in the table (raw) */
        NovaValue existing = novai_meta_raw_get(t, key);
        if (!nova_is_nil(existing)) {
            /* Key exists: always do raw set (no __newindex) */
            goto raw_set;
        }

        /* Key doesn't exist: check for __newindex */
        NovaTable *mt = nova_table_get_metatable(t);
        if (mt != NULL) {
            NovaValue handler = nova_meta_get_method(mt, NOVA_TM_NEWINDEX);
            if (!nova_is_nil(handler)) {
                /* __newindex is a table: redirect set to that table */
                if (nova_is_table(handler)) {
                    obj = handler;
                    continue;
                }
                /* __newindex is a function: call it */
                if (nova_is_function(handler) ||
                    nova_is_cfunction(handler)) {
                    return nova_meta_call3(vm, handler, obj, key, val);
                }
                novai_error(vm, NOVA_VM_ERR_TYPE,
                    "invalid __newindex (expected table or function)");
                return -1;
            }
        }

        /* No __newindex: raw set */
raw_set:
        /* Use the VM's table set functions via a direct call.
         * Since novai_table_set_xxx are static to nova_vm.c, we
         * call the public nova_meta_raw_set helper below. */
        return nova_meta_raw_set(vm, t, key, val);  /* forward-declared */
    }

    vm->diag_code = NOVA_E2024;
    novai_error(vm, NOVA_VM_ERR_TYPE,
                "__newindex chain too deep (possible loop)");
    return -1;
}

/* ============================================================
 * RAW SET HELPER (public, for __newindex final writes)
 * ============================================================ */

static int nova_meta_raw_set(NovaVM *vm, NovaTable *t,
                            NovaValue key, NovaValue val) {
    if (t == NULL) {
        return -1;
    }

    if (nova_is_integer(key)) {
        return nova_table_raw_set_int(vm, t, nova_as_integer(key), val);
    }
    if (nova_is_string(key)) {
        return nova_table_raw_set_str(vm, t, nova_as_string(key), val);
    }
    if (nova_is_number(key)) {
        nova_int_t ik = (nova_int_t)nova_as_number(key);
        if ((nova_number_t)ik == nova_as_number(key)) {
            return nova_table_raw_set_int(vm, t, ik, val);
        }
    }

    vm->diag_code = NOVA_E2019;
    novai_error(vm, NOVA_VM_ERR_TYPE, "invalid table key type");
    return -1;
}

/* ============================================================
 * HIGH-LEVEL PIPELINE: ARITHMETIC
 * ============================================================ */

/**
 * @brief Map an opcode to the corresponding metamethod tag.
 */
static NovaMetaTag novai_opcode_to_tm(NovaOpcode op) {
    switch (op) {
        case NOVA_OP_ADD:  return NOVA_TM_ADD;
        case NOVA_OP_SUB:  return NOVA_TM_SUB;
        case NOVA_OP_MUL:  return NOVA_TM_MUL;
        case NOVA_OP_DIV:  return NOVA_TM_DIV;
        case NOVA_OP_MOD:  return NOVA_TM_MOD;
        case NOVA_OP_POW:  return NOVA_TM_POW;
        case NOVA_OP_IDIV: return NOVA_TM_IDIV;
        case NOVA_OP_BAND: return NOVA_TM_BAND;
        case NOVA_OP_BOR:  return NOVA_TM_BOR;
        case NOVA_OP_BXOR: return NOVA_TM_BXOR;
        case NOVA_OP_SHL:  return NOVA_TM_SHL;
        case NOVA_OP_SHR:  return NOVA_TM_SHR;
        default:           return NOVA_TM_ADD;  /* fallback */
    }
}

/**
 * @brief Map an arithmetic opcode to its operator symbol for errors.
 */
static const char *novai_opcode_symbol(NovaOpcode op) {
    switch (op) {
        case NOVA_OP_ADD:  case NOVA_OP_ADDI:
        case NOVA_OP_ADDK: return "+";
        case NOVA_OP_SUB:  case NOVA_OP_SUBK: return "-";
        case NOVA_OP_MUL:  case NOVA_OP_MULK: return "*";
        case NOVA_OP_DIV:  case NOVA_OP_DIVK: return "/";
        case NOVA_OP_MOD:  case NOVA_OP_MODK: return "%";
        case NOVA_OP_POW:  return "^";
        case NOVA_OP_IDIV: return "//";
        case NOVA_OP_BAND: return "&";
        case NOVA_OP_BOR:  return "|";
        case NOVA_OP_BXOR: return "~";
        case NOVA_OP_SHL:  return "<<";
        case NOVA_OP_SHR:  return ">>";
        case NOVA_OP_UNM:  return "-";
        case NOVA_OP_BNOT: return "~";
        case NOVA_OP_STRLEN: return "#";
        default:           return "?";
    }
}

/**
 * @brief Core numeric conversion helpers (duplicated from VM for
 *        independence -- these are trivial).
 */
static int novai_meta_to_integer(NovaValue v, nova_int_t *out) {
    if (nova_is_integer(v)) {
        *out = nova_as_integer(v);
        return 1;
    }
    if (nova_is_number(v)) {
        nova_number_t n = nova_as_number(v);
        nova_int_t i = (nova_int_t)n;
        if ((nova_number_t)i == n) {
            *out = i;
            return 1;
        }
    }
    return 0;
}

static int novai_meta_to_number(NovaValue v, nova_number_t *out) {
    if (nova_is_number(v)) {
        *out = nova_as_number(v);
        return 1;
    }
    if (nova_is_integer(v)) {
        *out = (nova_number_t)nova_as_integer(v);
        return 1;
    }
    return 0;
}

/**
 * @brief Try raw arithmetic.  Returns 1 if successful, 0 if types
 *        don't match (caller should try metamethods).
 */
static int novai_meta_try_raw_arith(NovaVM *vm, NovaOpcode op,
                                    NovaValue left, NovaValue right,
                                    NovaValue *result) {
    nova_int_t il = 0, ir = 0;
    nova_number_t nl = 0.0, nr = 0.0;

    /* Integer path */
    if (novai_meta_to_integer(left, &il) &&
        novai_meta_to_integer(right, &ir)) {
        nova_int_t r = 0;
        switch (op) {
            case NOVA_OP_ADD:  r = il + ir; break;
            case NOVA_OP_SUB:  r = il - ir; break;
            case NOVA_OP_MUL:  r = il * ir; break;
            case NOVA_OP_IDIV:
                if (ir == 0) {
                    novai_error(vm, NOVA_VM_ERR_DIVZERO,
                                "integer division by zero");
                    return -1;
                }
                r = il / ir;
                break;
            case NOVA_OP_MOD:
                if (ir == 0) {
                    novai_error(vm, NOVA_VM_ERR_DIVZERO,
                                "modulo by zero");
                    return -1;
                }
                r = il % ir;
                break;
            case NOVA_OP_BAND: r = il & ir; break;
            case NOVA_OP_BOR:  r = il | ir; break;
            case NOVA_OP_BXOR: r = il ^ ir; break;
            case NOVA_OP_SHL:  r = il << (ir & 63); break;
            case NOVA_OP_SHR:  r = il >> (ir & 63); break;
            default:
                goto float_path;
        }
        *result = nova_value_integer(r);
        return 1;
    }

float_path:
    /* Float path */
    if (novai_meta_to_number(left, &nl) &&
        novai_meta_to_number(right, &nr)) {
        nova_number_t r = 0.0;
        switch (op) {
            case NOVA_OP_ADD:  r = nl + nr; break;
            case NOVA_OP_SUB:  r = nl - nr; break;
            case NOVA_OP_MUL:  r = nl * nr; break;
            case NOVA_OP_DIV:
                if (nr == 0.0) {
                    novai_error(vm, NOVA_VM_ERR_DIVZERO,
                                "division by zero");
                    return -1;
                }
                r = nl / nr;
                break;
            case NOVA_OP_IDIV:
                if (nr == 0.0) {
                    novai_error(vm, NOVA_VM_ERR_DIVZERO,
                                "floor division by zero");
                    return -1;
                }
                r = floor(nl / nr);
                break;
            case NOVA_OP_MOD:
                if (nr == 0.0) {
                    novai_error(vm, NOVA_VM_ERR_DIVZERO,
                                "modulo by zero");
                    return -1;
                }
                r = nl - floor(nl / nr) * nr;
                break;
            case NOVA_OP_POW:
                r = pow(nl, nr);
                break;
            default:
                /* Bitwise ops on floats: not valid */
                return 0;
        }
        *result = nova_value_number(r);
        return 1;
    }

    /* Types can't do raw arithmetic */
    return 0;
}

int nova_meta_arith(NovaVM *vm, NovaOpcode op,
                    NovaValue left, NovaValue right,
                    NovaValue *result) {
    if (result == NULL) {
        return -1;
    }

    /* Step 1: Try raw arithmetic */
    int raw = novai_meta_try_raw_arith(vm, op, left, right, result);
    if (raw == 1) {
        return 0;   /* Success */
    }
    if (raw == -1) {
        return -1;  /* Division by zero (error already set) */
    }

    /* Step 2: Look for metamethod on left operand first */
    NovaMetaTag tag = novai_opcode_to_tm(op);

    NovaValue method = nova_meta_get_tm(left, tag);
    if (!nova_is_nil(method)) {
        return nova_meta_call2(vm, method, left, right, result);
    }

    /* Step 3: Try right operand */
    method = nova_meta_get_tm(right, tag);
    if (!nova_is_nil(method)) {
        return nova_meta_call2(vm, method, left, right, result);
    }

    /* Step 4: No metamethod -- error */
    {
        const char *ltype = nova_vm_typename(nova_typeof(left));
        const char *rtype = nova_vm_typename(nova_typeof(right));
        const char *sym   = novai_opcode_symbol(op);
        int is_bitwise = (op == NOVA_OP_BAND || op == NOVA_OP_BOR ||
                          op == NOVA_OP_BXOR || op == NOVA_OP_SHL ||
                          op == NOVA_OP_SHR);
        char buf[256];
        if (is_bitwise) {
            snprintf(buf, sizeof(buf),
                     "attempt to perform bitwise '%s' on %s and %s",
                     sym, ltype, rtype);
            vm->diag_code = NOVA_E2025;
        } else {
            snprintf(buf, sizeof(buf),
                     "attempt to perform '%s' on %s and %s",
                     sym, ltype, rtype);
            vm->diag_code = NOVA_E2013;
        }
        novai_error(vm, NOVA_VM_ERR_TYPE, buf);
    }
    return -1;
}

/* ============================================================
 * HIGH-LEVEL PIPELINE: UNARY OPERATIONS
 * ============================================================ */

int nova_meta_unary(NovaVM *vm, NovaMetaTag tag,
                    NovaValue operand, NovaValue *result) {
    if (result == NULL) {
        return -1;
    }

    /* Try raw operations first */
    if (tag == NOVA_TM_UNM) {
        if (nova_is_integer(operand)) {
            *result = nova_value_integer(-nova_as_integer(operand));
            return 0;
        }
        if (nova_is_number(operand)) {
            *result = nova_value_number(-nova_as_number(operand));
            return 0;
        }
    }

    if (tag == NOVA_TM_BNOT) {
        if (nova_is_integer(operand)) {
            *result = nova_value_integer(~nova_as_integer(operand));
            return 0;
        }
    }

    if (tag == NOVA_TM_LEN) {
        if (nova_is_string(operand)) {
            *result = nova_value_integer(
                (nova_int_t)nova_str_len(nova_as_string(operand)));
            return 0;
        }
        if (nova_is_table(operand)) {
            /* Check for __len metamethod first */
            NovaValue len_method = nova_meta_get_tm(operand, NOVA_TM_LEN);
            if (!nova_is_nil(len_method)) {
                return nova_meta_call1(vm, len_method, operand, result);
            }
            /* Raw table length = array_used */
            *result = nova_value_integer(
                (nova_int_t)nova_table_array_len(nova_as_table(operand)));
            return 0;
        }
    }

    /* Try metamethod */
    NovaValue method = nova_meta_get_tm(operand, tag);
    if (!nova_is_nil(method)) {
        return nova_meta_call1(vm, method, operand, result);
    }

    /* Error */
    /* Error */
    {
        const char *vtype = nova_vm_typename(nova_typeof(operand));
        char buf[256];
        switch (tag) {
            case NOVA_TM_UNM:
                snprintf(buf, sizeof(buf),
                         "attempt to negate a %s value (number expected)",
                         vtype);
                vm->diag_code = NOVA_E2017;
                break;
            case NOVA_TM_BNOT:
                snprintf(buf, sizeof(buf),
                         "attempt to bitwise-not a %s value (integer expected)",
                         vtype);
                vm->diag_code = NOVA_E2025;
                break;
            case NOVA_TM_LEN:
                snprintf(buf, sizeof(buf),
                         "attempt to get length of a %s value (string or table expected)",
                         vtype);
                vm->diag_code = NOVA_E2016;
                break;
            default:
                snprintf(buf, sizeof(buf),
                         "attempt to perform invalid operation on a %s value",
                         vtype);
                vm->diag_code = NOVA_E2001;
                break;
        }
        novai_error(vm, NOVA_VM_ERR_TYPE, buf);
    }
    return -1;
}

/* ============================================================
 * HIGH-LEVEL PIPELINE: COMPARISONS
 * ============================================================ */

int nova_meta_compare(NovaVM *vm, NovaMetaTag tag,
                      NovaValue left, NovaValue right,
                      int *result) {
    if (result == NULL) {
        return -1;
    }

    /* -- Raw comparison for numbers -- */
    if ((nova_is_integer(left) || nova_is_number(left)) &&
        (nova_is_integer(right) || nova_is_number(right))) {
        nova_number_t nl = 0.0, nr = 0.0;
        (void)novai_meta_to_number(left, &nl);
        (void)novai_meta_to_number(right, &nr);

        switch (tag) {
            case NOVA_TM_EQ: *result = (nl == nr) ? 1 : 0; return 0;
            case NOVA_TM_LT: *result = (nl <  nr) ? 1 : 0; return 0;
            case NOVA_TM_LE: *result = (nl <= nr) ? 1 : 0; return 0;
            default: break;
        }
    }

    /* -- Raw comparison for strings -- */
    if (nova_is_string(left) && nova_is_string(right)) {
        int cmp = strcmp(nova_str_data(nova_as_string(left)), nova_str_data(nova_as_string(right)));
        switch (tag) {
            case NOVA_TM_EQ: *result = (cmp == 0) ? 1 : 0; return 0;
            case NOVA_TM_LT: *result = (cmp <  0) ? 1 : 0; return 0;
            case NOVA_TM_LE: *result = (cmp <= 0) ? 1 : 0; return 0;
            default: break;
        }
    }

    /* -- Raw EQ: same type + same pointer for tables/functions -- */
    if (tag == NOVA_TM_EQ) {
        if (nova_typeof(left) != nova_typeof(right)) {
            /* Different types are never equal (standard semantics).
             * __eq is only consulted for same-type comparisons. */
            *result = 0;
            return 0;
        }
        /* Same-type raw equality */
        switch (nova_typeof(left)) {
            case NOVA_TYPE_NIL:
                *result = 1;
                return 0;
            case NOVA_TYPE_BOOL:
                *result = (nova_as_bool(left) == nova_as_bool(right)) ? 1 : 0;
                return 0;
            case NOVA_TYPE_TABLE:
                if (nova_as_table(left) == nova_as_table(right)) {
                    *result = 1;
                    return 0;
                }
                break; /* Fall to metamethod */
            case NOVA_TYPE_FUNCTION:
                *result = (nova_as_closure(left) == nova_as_closure(right)) ? 1 : 0;
                return 0;
            case NOVA_TYPE_CFUNCTION:
                *result = (nova_as_cfunction(left) == nova_as_cfunction(right)) ? 1 : 0;
                return 0;
            default:
                break;
        }
    }

    /* -- Try metamethods -- */
    {
        NovaValue method = nova_meta_get_tm(left, tag);
        if (nova_is_nil(method)) {
            method = nova_meta_get_tm(right, tag);
        }
        if (!nova_is_nil(method)) {
            NovaValue tm_result;
            int rc = nova_meta_call2(vm, method, left, right, &tm_result);
            if (rc != 0) {
                return -1;
            }
            /* Truthy test on result */
            *result = (!nova_is_nil(tm_result) &&
                       !(nova_is_bool(tm_result) &&
                         nova_as_bool(tm_result) == 0)) ? 1 : 0;
            return 0;
        }
    }

    /* -- No metamethod for EQ: default to false -- */
    if (tag == NOVA_TM_EQ) {
        *result = 0;
        return 0;
    }

    /* -- No metamethod for LT/LE: error -- */
    {
        const char *ltype = nova_vm_typename(nova_typeof(left));
        const char *rtype = nova_vm_typename(nova_typeof(right));
        const char *op_sym = (tag == NOVA_TM_LT) ? "<" : "<=";
        char buf[256];
        snprintf(buf, sizeof(buf),
                 "attempt to compare %s with %s using '%s'",
                 ltype, rtype, op_sym);
        vm->diag_code = NOVA_E2014;
        novai_error(vm, NOVA_VM_ERR_TYPE, buf);
    }
    return -1;
}

/* ============================================================
 * HIGH-LEVEL PIPELINE: CONCATENATION
 * ============================================================ */

int nova_meta_concat(NovaVM *vm, NovaValue left, NovaValue right,
                     NovaValue *result) {
    if (result == NULL) {
        return -1;
    }

    /* Raw: both are strings */
    if (nova_is_string(left) && nova_is_string(right)) {
        /* Build concatenated string */
        uint32_t llen = (uint32_t)nova_str_len(nova_as_string(left));
        uint32_t rlen = (uint32_t)nova_str_len(nova_as_string(right));
        uint32_t total = llen + rlen;

        /* Allocate via VM string allocator */
        char *buf = (char *)malloc((size_t)(total + 1));
        if (buf == NULL) {
            novai_error(vm, NOVA_VM_ERR_MEMORY, "out of memory in concat");
            return -1;
        }
        memcpy(buf, nova_str_data(nova_as_string(left)), (size_t)llen);
        memcpy(buf + llen, nova_str_data(nova_as_string(right)), (size_t)rlen);
        buf[total] = '\0';
        NovaString *s = nova_vm_intern_string(vm, buf, (size_t)total);
        free(buf);
        if (s == NULL) {
            novai_error(vm, NOVA_VM_ERR_MEMORY, "out of memory in concat");
            return -1;
        }

        *result = nova_value_string(s);
        return 0;
    }

    /* Try coercing numbers to strings for concat */
    /* (Lua does this, we might want it too) */
    /* For now, go straight to metamethods */

    /* Try metamethod on left, then right */
    NovaValue method = nova_meta_get_tm(left, NOVA_TM_CONCAT);
    if (nova_is_nil(method)) {
        method = nova_meta_get_tm(right, NOVA_TM_CONCAT);
    }
    if (!nova_is_nil(method)) {
        return nova_meta_call2(vm, method, left, right, result);
    }

    {
        const char *ltype = nova_vm_typename(nova_typeof(left));
        const char *rtype = nova_vm_typename(nova_typeof(right));
        char buf[256];
        snprintf(buf, sizeof(buf),
                 "attempt to concatenate %s and %s",
                 ltype, rtype);
        vm->diag_code = NOVA_E2015;
        novai_error(vm, NOVA_VM_ERR_TYPE, buf);
    }
    return -1;
}

/* ============================================================
 * HIGH-LEVEL PIPELINE: __call
 * ============================================================ */

int nova_meta_call(NovaVM *vm, NovaValue obj, NovaValue *callable) {
    if (callable == NULL) {
        return -1;
    }

    /* Already callable? */
    if (nova_is_function(obj) ||
        nova_is_cfunction(obj)) {
        *callable = obj;
        return 0;
    }

    /* Look for __call */
    NovaValue method = nova_meta_get_tm(obj, NOVA_TM_CALL);
    if (nova_is_function(method) ||
        nova_is_cfunction(method)) {
        *callable = method;
        return 0;
    }

    {
        char buf[256];
        snprintf(buf, sizeof(buf),
                 "attempt to call a %s value",
                 nova_vm_typename(nova_typeof(obj)));
        vm->diag_code = NOVA_E2008;
        novai_error(vm, NOVA_VM_ERR_TYPE, buf);
    }
    return -1;
}

/* ============================================================
 * HIGH-LEVEL PIPELINE: __tostring
 * ============================================================ */

int nova_meta_tostring(NovaVM *vm, NovaValue v, NovaValue *result) {
    if (result == NULL) {
        return -1;
    }

    /* Check for __tostring metamethod first */
    NovaValue method = nova_meta_get_tm(v, NOVA_TM_TOSTRING);
    if (!nova_is_nil(method)) {
        return nova_meta_call1(vm, method, v, result);
    }

    /* Default conversions */
    char buf[64];
    int len = 0;

    switch (nova_typeof(v)) {
        case NOVA_TYPE_NIL:
            len = snprintf(buf, sizeof(buf), "nil");
            break;
        case NOVA_TYPE_BOOL:
            len = snprintf(buf, sizeof(buf), "%s",
                           nova_as_bool(v) ? "true" : "false");
            break;
        case NOVA_TYPE_INTEGER:
            len = snprintf(buf, sizeof(buf), "%lld",
                           (long long)nova_as_integer(v));
            break;
        case NOVA_TYPE_NUMBER:
            len = snprintf(buf, sizeof(buf), "%g", nova_as_number(v));
            break;
        case NOVA_TYPE_STRING:
            *result = v;  /* Already a string */
            return 0;
        case NOVA_TYPE_TABLE:
            len = snprintf(buf, sizeof(buf), "table: %p",
                           (void *)nova_as_table(v));
            break;
        case NOVA_TYPE_FUNCTION:
            len = snprintf(buf, sizeof(buf), "function: %p",
                           (void *)nova_as_closure(v));
            break;
        case NOVA_TYPE_CFUNCTION:
            len = snprintf(buf, sizeof(buf), "function: %p",
                           (void *)(uintptr_t)nova_as_cfunction(v));
            break;
        default:
            len = snprintf(buf, sizeof(buf), "<%s>",
                           nova_vm_typename(nova_typeof(v)));
            break;
    }

    if (len <= 0) {
        novai_error(vm, NOVA_VM_ERR_MEMORY, "tostring failed");
        return -1;
    }

    NovaString *s = nova_vm_intern_string(vm, buf, (size_t)len);
    if (s == NULL) {
        novai_error(vm, NOVA_VM_ERR_MEMORY, "out of memory in tostring");
        return -1;
    }

    *result = nova_value_string(s);
    return 0;
}

/* ============================================================
 * INITIALIZATION
 * ============================================================ */

void nova_meta_init(NovaVM *vm) {
    /* Future: pre-intern metamethod name strings into the
     * VM's string table for O(1) pointer comparisons.
     * For now, we use strcmp-based lookup which is fast enough
     * since metamethod names are short (max 12 chars). */
    (void)vm;
}
