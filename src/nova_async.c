/**
 * @file nova_async.c
 * @brief Nova Language - Async Task Scheduler
 *
 * Implements a cooperative task scheduler built on Nova's
 * coroutine infrastructure. Async functions create coroutines;
 * the scheduler runs spawned tasks round-robin alongside the
 * main task, providing cooperative concurrency without OS threads.
 *
 * Architecture:
 *   - Tasks are NovaCoroutines created from async function calls
 *   - The task queue lives on the NovaVM struct (task_queue[])
 *   - async.run(fn) drives the event loop: root task + spawned
 *   - OP_SPAWN registers tasks; OP_AWAIT runs tasks to completion
 *   - During await yield points, spawned tasks get CPU time
 *
 * @author Anthony Taliento
 * @date 2025-06-15
 * @version 0.2.0
 *
 * @copyright Copyright (c) 2025 Zorya Corporation
 * @license MIT
 *
 * DEPENDENCIES:
 *   - nova_vm.h (NovaVM, NovaCoroutine, NovaValue)
 *
 * THREAD SAFETY:
 *   Not thread-safe. All concurrency is cooperative.
 *
 * ZORYA-C COMPLIANCE: v2.0.0
 */

#include "nova/nova_vm.h"
#include <stdlib.h>
#include <string.h>

/* ============================================================
 * INTERNAL: FORWARD DECLARATIONS
 * ============================================================ */

extern void novai_error(NovaVM *vm, int code, const char *msg);

/* Initial task queue capacity */
#define NOVA_ASYNC_INITIAL_CAPACITY 8

/* ============================================================
 * PART 1: TASK QUEUE MANAGEMENT
 * ============================================================ */

/**
 * @brief Ensure the task queue has room for at least one more task.
 *
 * @param vm  VM instance
 * @return 0 on success, -1 on allocation failure.
 */
static int novai_async_grow(NovaVM *vm) {
    if (vm->task_count < vm->task_capacity) {
        return 0;
    }

    int new_cap = (vm->task_capacity == 0)
                  ? NOVA_ASYNC_INITIAL_CAPACITY
                  : vm->task_capacity * 2;

    NovaCoroutine **new_queue = (NovaCoroutine **)realloc(
        vm->task_queue,
        (size_t)new_cap * sizeof(NovaCoroutine *));
    if (new_queue == NULL) {
        return -1;
    }

    vm->task_queue = new_queue;
    vm->task_capacity = new_cap;
    return 0;
}

/**
 * @brief Enqueue a coroutine as a spawned task.
 *
 * Adds the task to the VM's task queue. The scheduler will
 * resume it during event loop ticks.
 *
 * @param vm   VM instance (must not be NULL)
 * @param task Coroutine to enqueue (must be SUSPENDED)
 *
 * @return 0 on success, -1 on failure.
 *
 * @pre vm != NULL && task != NULL
 * @pre task->status == NOVA_CO_SUSPENDED
 *
 * COMPLEXITY: O(1) amortized
 * THREAD SAFETY: Not thread-safe
 */
int nova_async_enqueue(NovaVM *vm, NovaCoroutine *task) {
    if (vm == NULL || task == NULL) {
        return -1;
    }

    if (novai_async_grow(vm) != 0) {
        return -1;
    }

    vm->task_queue[vm->task_count] = task;
    vm->task_count++;
    return 0;
}

/**
 * @brief Remove dead tasks from the queue (compact).
 *
 * Shifts remaining tasks left to fill gaps. Called periodically
 * by the event loop to clean up completed tasks.
 *
 * @param vm  VM instance
 *
 * COMPLEXITY: O(task_count)
 */
static void novai_async_compact(NovaVM *vm) {
    int write = 0;
    for (int read = 0; read < vm->task_count; read++) {
        if (vm->task_queue[read]->status != NOVA_CO_DEAD) {
            vm->task_queue[write] = vm->task_queue[read];
            write++;
        }
    }
    vm->task_count = write;
}

/* ============================================================
 * PART 2: SCHEDULER TICK
 * ============================================================ */

/**
 * @brief Run one round-robin tick of all spawned tasks.
 *
 * Resumes each SUSPENDED task exactly once. If a task yields,
 * its yield values are discarded (spawned tasks communicate
 * via shared state, not yield values). Dead tasks are compacted
 * out after the tick.
 *
 * @param vm  VM instance (must not be NULL)
 *
 * COMPLEXITY: O(task_count)
 * THREAD SAFETY: Not thread-safe
 */
void nova_async_tick(NovaVM *vm) {
    if (vm == NULL || vm->task_count == 0) {
        return;
    }

    int count = vm->task_count;  /* Snapshot: don't run tasks spawned this tick */

    for (int i = 0; i < count && i < vm->task_count; i++) {
        NovaCoroutine *task = vm->task_queue[i];

        if (task->status != NOVA_CO_SUSPENDED) {
            continue;
        }

        /* Flush pending args on first resume */
        int resume_nargs = 0;
        if (task->pending_args != NULL) {
            for (int a = 0; a < task->pending_nargs; a++) {
                *vm->stack_top = task->pending_args[a];
                vm->stack_top++;
            }
            resume_nargs = task->pending_nargs;
            free(task->pending_args);
            task->pending_args = NULL;
            task->pending_nargs = 0;
        }

        int result = nova_coroutine_resume(vm, task, resume_nargs);

        if (result == NOVA_VM_YIELD) {
            /* Discard yield values from spawned task */
            vm->stack_top -= task->nresults;
        } else if (result == NOVA_VM_OK) {
            /* Task completed - discard return values */
            vm->stack_top -= task->nresults;
            /* task->status is already DEAD from resume */
        } else {
            /* Error in spawned task - mark dead, discard.
             * Errors in spawned tasks are silently dropped
             * (fire-and-forget semantics). */
            task->status = NOVA_CO_DEAD;
        }
    }

    /* Remove dead tasks */
    novai_async_compact(vm);
}

/* ============================================================
 * PART 3: EVENT LOOP
 * ============================================================ */

/**
 * @brief Run the async event loop until all tasks complete.
 *
 * The root coroutine is the "main" async function. It runs
 * alongside all spawned tasks in round-robin fashion:
 *
 *   1. Resume root task
 *   2. If root yields, tick all spawned tasks
 *   3. Repeat until root returns
 *   4. Continue ticking spawned tasks until all done
 *
 * @param vm    VM instance (must not be NULL)
 * @param root  Root coroutine (must be SUSPENDED)
 * @param nargs Number of arguments for the root task
 *
 * @return NOVA_VM_OK on success, error code on failure.
 *
 * @pre vm != NULL && root != NULL
 *
 * COMPLEXITY: Depends on task work
 * THREAD SAFETY: Not thread-safe
 */
int nova_async_run(NovaVM *vm, NovaCoroutine *root, int nargs) {
    if (vm == NULL || root == NULL) {
        return NOVA_VM_ERR_NULLPTR;
    }

    if (root->status != NOVA_CO_SUSPENDED) {
        novai_error(vm, NOVA_VM_ERR_RUNTIME,
                    "async.run: task is not suspended");
        return vm->status;
    }

    /* Flush pending args for root task */
    int resume_nargs = nargs;
    if (root->pending_args != NULL && nargs == 0) {
        for (int a = 0; a < root->pending_nargs; a++) {
            *vm->stack_top = root->pending_args[a];
            vm->stack_top++;
        }
        resume_nargs = root->pending_nargs;
        free(root->pending_args);
        root->pending_args = NULL;
        root->pending_nargs = 0;
    }

    /* Phase 1: Run root task alongside spawned tasks */
    int result = nova_coroutine_resume(vm, root, resume_nargs);

    while (result == NOVA_VM_YIELD && root->status == NOVA_CO_SUSPENDED) {
        /* Discard root's yield values */
        vm->stack_top -= root->nresults;

        /* Push root coroutine onto VM stack as a GC root.
         * The root coroutine only lives in C locals during the
         * event loop — it's not on any VM-visible stack or in the
         * task queue.  Without this, a GC triggered by a spawned
         * task could collect the root coroutine while it's
         * suspended, leading to use-after-free on the next resume. */
        *vm->stack_top = nova_value_coroutine(root);
        vm->stack_top++;

        /* Give spawned tasks a turn */
        nova_async_tick(vm);

        /* Pop root GC anchor */
        vm->stack_top--;

        /* Resume root */
        result = nova_coroutine_resume(vm, root, 0);
    }

    if (result == NOVA_VM_OK) {
        /* Root completed - harvest return values (leave on stack) */
        /* Return values are already on the caller's stack from
         * nova_coroutine_resume. Caller can read them. */
    } else if (result != NOVA_VM_YIELD) {
        /* Error in root task */
        return result;
    }

    /* Phase 2: Drain remaining spawned tasks */
    int max_ticks = 100000;  /* Safety limit to prevent infinite loops */
    while (vm->task_count > 0 && max_ticks > 0) {
        nova_async_tick(vm);
        max_ticks--;
    }

    if (vm->task_count > 0) {
        novai_error(vm, NOVA_VM_ERR_RUNTIME,
                    "async.run: spawned tasks did not complete "
                    "(possible infinite loop)");
        return vm->status;
    }

    return NOVA_VM_OK;
}

/* ============================================================
 * PART 4: CLEANUP
 * ============================================================ */

/**
 * @brief Free the task queue array.
 *
 * Called by nova_vm_destroy. The task coroutines themselves
 * are GC-managed objects and will be freed by GC shutdown.
 *
 * @param vm  VM instance (must not be NULL)
 *
 * COMPLEXITY: O(1)
 * THREAD SAFETY: Not thread-safe
 */
void nova_async_cleanup(NovaVM *vm) {
    if (vm == NULL) {
        return;
    }

    free(vm->task_queue);
    vm->task_queue = NULL;
    vm->task_count = 0;
    vm->task_capacity = 0;
}
