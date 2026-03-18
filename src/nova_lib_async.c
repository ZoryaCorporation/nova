/**
 * @file nova_lib_async.c
 * @brief Nova Language - Async Standard Library
 *
 * Provides the "async" module for cooperative async/await
 * concurrency. Built on top of the coroutine infrastructure
 * and the task scheduler in nova_async.c.
 *
 * Functions:
 *   async.run(fn, ...)       Run the event loop with a root task
 *   async.spawn(fn, ...)     Create and enqueue a detached task
 *   async.sleep(n)           Cooperative yield (n iterations)
 *   async.wrap(fn)           Wrap a function as an async callable
 *   async.status(task)       Get task status string
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
 * ASYNC.RUN
 *
 * async.run(fn, ...) -> results...
 *
 * Creates a root task from fn, runs the event loop until the
 * root task and all spawned tasks complete. Returns the root
 * task's return values.
 * ============================================================ */

/**
 * @brief async.run(fn, ...) - Run the async event loop.
 *
 * @param vm  VM instance
 * @return Number of results, or -1 on error.
 */
static int nova_async_lib_run(NovaVM *vm) {
    if (nova_lib_check_args(vm, 1) != 0) {
        return -1;
    }

    NovaValue func = nova_vm_get(vm, 0);
    int total_args = nova_vm_get_top(vm);
    int nargs = total_args - 1;  /* Args after the function */

    /* Accept either a function or an already-created coroutine */
    NovaCoroutine *root = NULL;

    if (nova_is_function(func)) {
        NovaClosure *cl = nova_as_closure(func);
        root = nova_coroutine_new(vm, cl);
        if (root == NULL) {
            nova_vm_raise_error(vm, "async.run: failed to create root task");
            return -1;
        }

        /* Stash arguments for first resume */
        if (nargs > 0) {
            root->pending_args = (NovaValue *)malloc(
                (size_t)nargs * sizeof(NovaValue));
            if (root->pending_args == NULL) {
                nova_vm_raise_error(vm,
                    "async.run: argument allocation failed");
                return -1;
            }
            for (int i = 0; i < nargs; i++) {
                root->pending_args[i] = nova_vm_get(vm, i + 1);
            }
            root->pending_nargs = nargs;
        }
    } else if (nova_is_thread(func)) {
        root = nova_as_coroutine(func);
        /* If extra args were passed with a coroutine, stash them */
        if (nargs > 0 && root->pending_args == NULL) {
            root->pending_args = (NovaValue *)malloc(
                (size_t)nargs * sizeof(NovaValue));
            if (root->pending_args == NULL) {
                nova_vm_raise_error(vm,
                    "async.run: argument allocation failed");
                return -1;
            }
            for (int i = 0; i < nargs; i++) {
                root->pending_args[i] = nova_vm_get(vm, i + 1);
            }
            root->pending_nargs = nargs;
        }
    } else {
        nova_vm_raise_error(vm,
            "bad argument #1 to 'run' (function or task expected, got %s)",
            nova_vm_typename(nova_typeof(func)));
        return -1;
    }

    /* Run the event loop */
    int result = nova_async_run(vm, root, 0);

    if (result != NOVA_VM_OK) {
        /* Re-raise the error so pcall can catch it properly.
         * nova_async_run stored the message in vm->error_msg
         * but the error handler was inside the coroutine.
         * Re-raising triggers the outer longjmp to pcall. */
        const char *msg = vm->error_msg != NULL
                          ? vm->error_msg
                          : "async task error";
        nova_vm_raise_error(vm, "%s", msg);
        return -1;
    }

    /* Return values from root are on the stack.
     * nova_async_run leaves them via nova_coroutine_resume. */
    int nret = root->nresults;
    return nret;
}

/* ============================================================
 * ASYNC.SPAWN
 *
 * async.spawn(fn, ...) -> task
 *
 * Creates a task from fn with the given arguments and registers
 * it with the scheduler. Returns the task handle for optional
 * later awaiting.
 * ============================================================ */

/**
 * @brief async.spawn(fn, ...) - Spawn a detached async task.
 *
 * @param vm  VM instance
 * @return 1 (task handle pushed), or -1 on error.
 */
static int nova_async_lib_spawn(NovaVM *vm) {
    if (nova_lib_check_args(vm, 1) != 0) {
        return -1;
    }

    NovaValue func = nova_vm_get(vm, 0);
    int total_args = nova_vm_get_top(vm);
    int nargs = total_args - 1;

    NovaCoroutine *task = NULL;

    if (nova_is_function(func)) {
        NovaClosure *cl = nova_as_closure(func);
        task = nova_coroutine_new(vm, cl);
        if (task == NULL) {
            nova_vm_raise_error(vm, "async.spawn: failed to create task");
            return -1;
        }

        /* Stash arguments */
        if (nargs > 0) {
            task->pending_args = (NovaValue *)malloc(
                (size_t)nargs * sizeof(NovaValue));
            if (task->pending_args == NULL) {
                nova_vm_raise_error(vm,
                    "async.spawn: argument allocation failed");
                return -1;
            }
            for (int i = 0; i < nargs; i++) {
                task->pending_args[i] = nova_vm_get(vm, i + 1);
            }
            task->pending_nargs = nargs;
        }
    } else if (nova_is_thread(func)) {
        task = nova_as_coroutine(func);
    } else {
        nova_vm_raise_error(vm,
            "bad argument #1 to 'spawn' (function or task expected, got %s)",
            nova_vm_typename(nova_typeof(func)));
        return -1;
    }

    if (task->status != NOVA_CO_SUSPENDED) {
        nova_vm_raise_error(vm,
            "async.spawn: task is not in suspended state");
        return -1;
    }

    /* Enqueue the task with the scheduler */
    if (nova_async_enqueue(vm, task) != 0) {
        nova_vm_raise_error(vm, "async.spawn: failed to enqueue task");
        return -1;
    }

    /* Return the task handle */
    nova_vm_push_value(vm, nova_value_coroutine(task));
    return 1;
}

/* ============================================================
 * ASYNC.SLEEP
 *
 * async.sleep(n) -> nil
 *
 * Cooperative yield: yields the current coroutine n times.
 * Each yield gives other tasks a chance to run. With n=0 or
 * no argument, yields once.
 * ============================================================ */

/**
 * @brief async.sleep(n) - Cooperative sleep (yield n times).
 *
 * @param vm  VM instance
 * @return 0, or -1 on error.
 */
static int nova_async_lib_sleep(NovaVM *vm) {
    int iterations = 1;

    if (nova_vm_get_top(vm) > 0) {
        nova_int_t n = 0;
        if (nova_lib_check_integer(vm, 0, &n) == 0) {
            return -1;
        }
        if (n > 0) {
            iterations = (int)n;
        }
    }

    /* Can only yield inside a coroutine */
    if (vm->running_coroutine == NULL) {
        /* In main thread, sleep is a no-op (no scheduler to yield to) */
        return 0;
    }

    /* Yield the specified number of times */
    for (int i = 0; i < iterations; i++) {
        nova_coroutine_yield(vm, 0);
        return NOVA_VM_YIELD;
    }

    return 0;
}

/* ============================================================
 * ASYNC.STATUS
 *
 * async.status(task) -> string
 *
 * Returns the status of a task: "suspended", "running", "dead".
 * ============================================================ */

/**
 * @brief async.status(task) - Get task status.
 *
 * @param vm  VM instance
 * @return 1 (status string pushed), or -1 on error.
 */
static int nova_async_lib_status(NovaVM *vm) {
    if (nova_lib_check_args(vm, 1) != 0) {
        return -1;
    }

    NovaValue task_val = nova_vm_get(vm, 0);
    if (!nova_is_thread(task_val)) {
        nova_vm_raise_error(vm,
            "bad argument #1 to 'status' (task expected, got %s)",
            nova_vm_typename(nova_typeof(task_val)));
        return -1;
    }

    NovaCoroutine *task = nova_as_coroutine(task_val);
    const char *status = nova_coroutine_status_str(task);

    /* Push status string */
    NovaString *str = nova_string_new(vm, status, strlen(status));
    if (str == NULL) {
        nova_vm_raise_error(vm, "async.status: string allocation failed");
        return -1;
    }
    nova_vm_push_value(vm, nova_value_string(str));
    return 1;
}

/* ============================================================
 * ASYNC.WRAP
 *
 * async.wrap(fn) -> wrapped_fn
 *
 * Creates an async wrapper around a regular function. When
 * called, the wrapper creates a coroutine (task) from fn
 * instead of executing it directly. Essentially makes any
 * function behave as if declared with 'async'.
 *
 * Note: This is a convenience function. Prefer using the
 * 'async function' syntax when possible.
 * ============================================================ */

/**
 * @brief async.wrap(fn) - Wrap a function as async.
 *
 * For now, this creates a coroutine from the function
 * and returns it in suspended state.
 *
 * @param vm  VM instance
 * @return 1 (task pushed), or -1 on error.
 */
static int nova_async_lib_wrap(NovaVM *vm) {
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

    NovaClosure *cl = nova_as_closure(func);
    NovaCoroutine *co = nova_coroutine_new(vm, cl);
    if (co == NULL) {
        nova_vm_raise_error(vm, "async.wrap: failed to create task");
        return -1;
    }

    nova_vm_push_value(vm, nova_value_coroutine(co));
    return 1;
}

/* ============================================================
 * REGISTRATION TABLE
 * ============================================================ */

static const NovaLibReg nova_async_lib[] = {
    {"run",    nova_async_lib_run},
    {"spawn",  nova_async_lib_spawn},
    {"sleep",  nova_async_lib_sleep},
    {"status", nova_async_lib_status},
    {"wrap",   nova_async_lib_wrap},
    {NULL,     NULL}
};

/* ============================================================
 * MODULE OPENER
 * ============================================================ */

/**
 * @brief Open the async standard library.
 *
 * Registers the "async" module table containing all
 * async task management functions.
 *
 * @param vm  VM instance (must not be NULL)
 * @return 0 on success, -1 on failure.
 */
int nova_open_async(NovaVM *vm) {
    if (vm == NULL) {
        return -1;
    }

    nova_lib_register_module(vm, "task", nova_async_lib);
    return 0;
}
