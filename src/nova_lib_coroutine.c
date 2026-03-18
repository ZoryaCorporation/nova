/**
 * @file nova_lib_coroutine.c
 * @brief Nova Language - Coroutine Standard Library
 *
 * Provides the "coroutine" module for cooperative multitasking.
 *
 * Functions:
 *   coroutine.create(f)         Create a new coroutine
 *   coroutine.resume(co, ...)   Resume a suspended coroutine
 *   coroutine.yield(...)        Yield from running coroutine
 *   coroutine.wrap(f)           Create an iterator from coroutine
 *   coroutine.status(co)        Get coroutine status string
 *   coroutine.isyieldable()     Check if current context can yield
 *   coroutine.running()         Get the currently running coroutine
 *
 * @author Anthony Taliento
 * @date 2025-06-15
 * @version 0.2.0
 *
 * @copyright Copyright (c) 2025 Zorya Corporation
 * @license MIT
 *
 * DEPENDENCIES:
 *   - nova_lib.h
 *   - nova_vm.h
 *
 * ZORYA-C COMPLIANCE: v2.0.0
 */

#include "nova/nova_lib.h"
#include "nova/nova_vm.h"

#include <stdlib.h>
#include <string.h>

/* ============================================================
 * COROUTINE.CREATE
 *
 * coroutine.create(f) -> thread
 *
 * Creates a new coroutine from function f. The coroutine
 * starts in SUSPENDED state; call resume() to begin execution.
 * ============================================================ */

/**
 * @brief coroutine.create(f) - Create a new coroutine.
 *
 * @param vm  VM instance
 * @return 1 (coroutine pushed), or -1 on error.
 */
static int nova_co_create(NovaVM *vm) {
    if (nova_lib_check_args(vm, 1) != 0) {
        return -1;
    }

    NovaValue func = nova_vm_get(vm, 0);
    if (!nova_is_function(func)) {
        nova_vm_raise_error(vm,
            "bad argument #1 to 'create' (function expected, got %s)",
            nova_vm_typename(nova_typeof(func)));
        return -1;
    }

    NovaCoroutine *co = nova_coroutine_new(vm, nova_as_closure(func));
    if (co == NULL) {
        nova_vm_raise_error(vm, "failed to create coroutine");
        return -1;
    }

    nova_vm_push_value(vm, nova_value_coroutine(co));
    return 1;
}

/* ============================================================
 * COROUTINE.RESUME
 *
 * coroutine.resume(co, ...) -> true, results...
 *                           -> false, error_msg
 *
 * Resumes a suspended coroutine. Returns true + yielded/returned
 * values on success, or false + error message on failure.
 * ============================================================ */

/**
 * @brief coroutine.resume(co, ...) - Resume a coroutine.
 *
 * @param vm  VM instance
 * @return Number of results pushed (1 + nvalues), or -1 on error.
 */
static int nova_co_resume(NovaVM *vm) {
    if (nova_lib_check_args(vm, 1) != 0) {
        return -1;
    }

    NovaValue co_val = nova_vm_get(vm, 0);
    if (!nova_is_thread(co_val)) {
        nova_vm_raise_error(vm,
            "bad argument #1 to 'resume' (coroutine expected, got %s)",
            nova_vm_typename(nova_typeof(co_val)));
        return -1;
    }

    NovaCoroutine *co = nova_as_coroutine(co_val);
    if (co == NULL) {
        nova_vm_raise_error(vm, "attempt to resume null coroutine");
        return -1;
    }

    if (co->status != NOVA_CO_SUSPENDED) {
        /* Like Lua: return false, "cannot resume dead/running coroutine" */
        nova_vm_push_bool(vm, 0);
        const char *reason = "unknown";
        switch (co->status) {
            case NOVA_CO_RUNNING:
                reason = "cannot resume running coroutine";
                break;
            case NOVA_CO_DEAD:
                reason = "cannot resume dead coroutine";
                break;
            case NOVA_CO_NORMAL:
                reason = "cannot resume normal coroutine";
                break;
            default:
                reason = "cannot resume coroutine";
                break;
        }
        nova_vm_push_string(vm, reason, strlen(reason));
        return 2;
    }

    /* Gather extra arguments (everything after the coroutine itself) */
    int top = nova_vm_get_top(vm);
    int nargs = top - 1;  /* Exclude the coroutine argument */

    /* Push args onto vm->stack_top so resume can consume them.
     * The args are already on the stack via the C function calling
     * convention (cfunc_base + 1..top), but we need them at stack_top.
     * We copy them to stack_top for resume to find. */
    NovaValue *cfunc_base_saved = vm->cfunc_base;
    NovaValue *args_start = cfunc_base_saved + 1;  /* Skip coroutine arg */

    /* Reset cfunc_base before resume so the VM stack is in normal mode */
    vm->cfunc_base = NULL;

    /* Push args at current stack_top */
    for (int i = 0; i < nargs; i++) {
        nova_vm_push_value(vm, args_start[i]);
    }

    /* Resume the coroutine */
    int result = nova_coroutine_resume(vm, co, nargs);

    /* Restore cfunc_base for our return */
    vm->cfunc_base = cfunc_base_saved;

    if (result == NOVA_VM_OK || result == NOVA_VM_YIELD) {
        /* Success: push true, then results */
        /* Results were already pushed onto our stack by resume.
         * We need to figure out how many. */
        int nret = co->nresults;

        /* Move results down: currently they're above cfunc_base area.
         * We need to push true first, then the results.
         * Save results, push true, then push results back. */

        /* Save results from stack_top */
        NovaValue results[256];
        int safe_nret = nret;
        if (safe_nret > 256) {
            safe_nret = 256;
        }
        NovaValue *result_base = vm->stack_top - safe_nret;
        for (int i = 0; i < safe_nret; i++) {
            results[i] = result_base[i];
        }
        /* Pop the results temporarily */
        vm->stack_top -= safe_nret;

        /* Push true as first return value */
        nova_vm_push_bool(vm, 1);

        /* Push results back */
        for (int i = 0; i < safe_nret; i++) {
            nova_vm_push_value(vm, results[i]);
        }

        return 1 + safe_nret;

    } else {
        /* Error: push false, error_msg */
        nova_vm_push_bool(vm, 0);
        const char *err = nova_vm_error(vm);
        nova_vm_push_string(vm, err, strlen(err));
        vm->status = NOVA_VM_OK;  /* Clear error for caller */
        return 2;
    }
}

/* ============================================================
 * COROUTINE.YIELD
 *
 * coroutine.yield(...) -> values from next resume
 *
 * Yields the currently running coroutine. Values passed to
 * yield are returned by the corresponding resume() call.
 * ============================================================ */

/**
 * @brief coroutine.yield(...) - Yield from current coroutine.
 *
 * @param vm  VM instance
 * @return Does not return normally; returns via NOVA_VM_YIELD.
 */
static int nova_co_yield(NovaVM *vm) {
    int nargs = nova_vm_get_top(vm);

    /* Push yield values onto the VM stack (they're already there
     * via the C function calling convention) */

    /* Call the coroutine yield mechanism */
    int result = nova_coroutine_yield(vm, nargs);
    (void)result;

    /* This function doesn't truly "return" in the normal sense.
     * The dispatch loop will see vm->status == NOVA_VM_YIELD
     * and exit. When the coroutine is resumed, execution continues
     * after the CALL instruction that invoked this C function.
     * The resume values will be placed as our "return values". */
    return nargs;
}

/* ============================================================
 * COROUTINE.WRAP
 *
 * coroutine.wrap(f) -> function
 *
 * Creates a coroutine and returns an iterator function that
 * resumes it each time it's called. Unlike resume(), the
 * iterator doesn't return a status boolean — it raises errors.
 * ============================================================ */

/** Forward declaration for wrap's __call handler */
static int nova_co_wrap_call(NovaVM *vm);

/**
 * @brief coroutine.wrap(f) - Create an iterator from coroutine.
 *
 * Returns a callable wrapper table. Each call resumes the
 * internal coroutine and returns the yielded values directly
 * (without a status boolean). Raises an error if the coroutine
 * is dead.
 *
 * @param vm  VM instance
 * @return 1 (callable table pushed), or -1 on error.
 */
static int nova_co_wrap(NovaVM *vm) {
    if (nova_lib_check_args(vm, 1) != 0) {
        return -1;
    }

    NovaValue func = nova_vm_get(vm, 0);
    if (!nova_is_function(func)) {
        nova_vm_raise_error(vm,
            "bad argument #1 to 'wrap' (function expected, got %s)",
            nova_vm_typename(nova_typeof(func)));
        return -1;
    }

    /* Create coroutine */
    NovaCoroutine *co = nova_coroutine_new(vm, nova_as_closure(func));
    if (co == NULL) {
        nova_vm_raise_error(vm, "failed to create coroutine");
        return -1;
    }

    /* Save result slot for cfunction stack protocol */
    NovaValue *result_slot = vm->stack_top;

    /* Build wrapper table with coroutine at array[0] */
    nova_vm_push_table(vm);
    int wrap_idx = nova_vm_get_top(vm) - 1;

    NovaValue wrap_val = nova_vm_get(vm, wrap_idx);
    nova_table_raw_set_int(vm, nova_as_table(wrap_val), 0,
                           nova_value_coroutine(co));

    /* Build metatable { __call = nova_co_wrap_call } */
    nova_vm_push_table(vm);
    int mt_idx = nova_vm_get_top(vm) - 1;

    nova_vm_push_cfunction(vm, nova_co_wrap_call);
    nova_vm_set_field(vm, mt_idx, "__call");

    /* Set metatable on wrapper */
    NovaValue mt_val = nova_vm_get(vm, mt_idx);
    nova_table_set_metatable(nova_as_table(wrap_val),
                            nova_as_table(mt_val));
    nova_gc_barrier(vm, NOVA_GC_HDR(nova_as_table(wrap_val)));

    /* Reset stack and push only the wrapper */
    vm->stack_top = result_slot;
    nova_vm_push_value(vm, wrap_val);
    return 1;
}

/**
 * @brief __call handler for coroutine.wrap wrapper tables.
 *
 * Called when the wrapped table is invoked like a function.
 * Resumes the internal coroutine and returns yielded values
 * directly (without status boolean). Raises error on failure.
 *
 * @param vm  VM instance
 * @return Number of results, or -1 on error.
 */
static int nova_co_wrap_call(NovaVM *vm) {
    /* self is the wrapper table (arg 0 due to __call protocol) */
    NovaValue self = nova_vm_get(vm, 0);
    if (!nova_is_table(self)) {
        nova_vm_raise_error(vm, "corrupt coroutine.wrap iterator");
        return -1;
    }

    /* Extract coroutine from array slot 0 */
    NovaTable *t = nova_as_table(self);
    NovaValue co_val = nova_value_nil();
    if (nova_table_array_len(t) > 0) {
        co_val = nova_table_get_int(t, 0);
    }

    if (!nova_is_thread(co_val)) {
        nova_vm_raise_error(vm,
            "cannot resume dead coroutine");
        return -1;
    }

    NovaCoroutine *co = nova_as_coroutine(co_val);
    if (co->status != NOVA_CO_SUSPENDED) {
        nova_vm_raise_error(vm,
            "cannot resume dead coroutine");
        return -1;
    }

    /* Save result slot for cfunction stack protocol */
    NovaValue *result_slot = vm->stack_top;

    /* Gather extra arguments (skip self) */
    int top = nova_vm_get_top(vm);
    int nargs = top - 1;

    NovaValue *cfunc_base_saved = vm->cfunc_base;
    NovaValue *args_start = cfunc_base_saved + 1;  /* Skip self */

    vm->cfunc_base = NULL;

    /* Push args at current stack_top for resume */
    for (int i = 0; i < nargs; i++) {
        nova_vm_push_value(vm, args_start[i]);
    }

    /* Resume the coroutine */
    int result = nova_coroutine_resume(vm, co, nargs);

    /* Restore cfunc_base */
    vm->cfunc_base = cfunc_base_saved;

    if (result == NOVA_VM_OK || result == NOVA_VM_YIELD) {
        /* Save results from wherever resume placed them */
        int nret = co->nresults;
        NovaValue results[256];
        int safe_nret = nret;
        if (safe_nret > 256) {
            safe_nret = 256;
        }
        NovaValue *rbase = vm->stack_top - safe_nret;
        for (int i = 0; i < safe_nret; i++) {
            results[i] = rbase[i];
        }

        /* Reset stack to result slot and push results there */
        vm->stack_top = result_slot;
        for (int i = 0; i < safe_nret; i++) {
            nova_vm_push_value(vm, results[i]);
        }
        return safe_nret;
    } else {
        /* Unlike resume(), wrap iterators raise errors */
        const char *err = nova_vm_error(vm);
        vm->status = NOVA_VM_OK;
        vm->stack_top = result_slot;
        nova_vm_raise_error(vm, "%s", err);
        return -1;
    }
}

/* ============================================================
 * COROUTINE.STATUS
 *
 * coroutine.status(co) -> string
 *
 * Returns "suspended", "running", "dead", or "normal".
 * ============================================================ */

/**
 * @brief coroutine.status(co) - Get coroutine status as string.
 *
 * @param vm  VM instance
 * @return 1 (string pushed), or -1 on error.
 */
static int nova_co_status(NovaVM *vm) {
    if (nova_lib_check_args(vm, 1) != 0) {
        return -1;
    }

    NovaValue co_val = nova_vm_get(vm, 0);
    if (!nova_is_thread(co_val)) {
        nova_vm_raise_error(vm,
            "bad argument #1 to 'status' (coroutine expected, got %s)",
            nova_vm_typename(nova_typeof(co_val)));
        return -1;
    }

    NovaCoroutine *co = nova_as_coroutine(co_val);
    const char *status = nova_coroutine_status_str(co);
    nova_vm_push_string(vm, status, strlen(status));
    return 1;
}

/* ============================================================
 * COROUTINE.ISYIELDABLE
 *
 * coroutine.isyieldable() -> boolean
 *
 * Returns true if the current context is inside a coroutine
 * that can yield (i.e., not the main thread).
 * ============================================================ */

/**
 * @brief coroutine.isyieldable() - Check if current context can yield.
 *
 * @param vm  VM instance
 * @return 1 (boolean pushed).
 */
static int nova_co_isyieldable(NovaVM *vm) {
    int yieldable = (vm->running_coroutine != NULL) ? 1 : 0;
    nova_vm_push_bool(vm, yieldable);
    return 1;
}

/* ============================================================
 * COROUTINE.RUNNING
 *
 * coroutine.running() -> coroutine, boolean
 *
 * Returns the currently running coroutine (or nil if main)
 * and a boolean indicating if it's the main thread.
 * ============================================================ */

/**
 * @brief coroutine.running() - Get the running coroutine.
 *
 * @param vm  VM instance
 * @return 2 (coroutine + is_main pushed).
 */
static int nova_co_running(NovaVM *vm) {
    NovaCoroutine *co = vm->running_coroutine;
    if (co != NULL) {
        nova_vm_push_value(vm, nova_value_coroutine(co));
        nova_vm_push_bool(vm, 0);  /* Not the main thread */
    } else {
        nova_vm_push_nil(vm);
        nova_vm_push_bool(vm, 1);  /* Is the main thread */
    }
    return 2;
}

/* ============================================================
 * REGISTRATION TABLE
 * ============================================================ */

static const NovaLibReg nova_coroutine_lib[] = {
    {"create",      nova_co_create},
    {"resume",      nova_co_resume},
    {"yield",       nova_co_yield},
    {"wrap",        nova_co_wrap},
    {"status",      nova_co_status},
    {"isyieldable", nova_co_isyieldable},
    {"running",     nova_co_running},
    {NULL,          NULL}
};

/* ============================================================
 * MODULE OPENER
 * ============================================================ */

/**
 * @brief Open the coroutine standard library.
 *
 * Registers the "coroutine" module table containing all
 * coroutine manipulation functions.
 *
 * @param vm  VM instance (must not be NULL)
 * @return 0 on success, -1 on failure.
 */
int nova_open_coroutine(NovaVM *vm) {
    if (vm == NULL) {
        return -1;
    }

    nova_lib_register_module(vm, "coroutine", nova_coroutine_lib);
    return 0;
}
