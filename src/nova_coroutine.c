/**
 * @file nova_coroutine.c
 * @brief Nova Language - Coroutine (Thread) Runtime
 *
 * Implements cooperative multitasking via stackful coroutines.
 * Each coroutine owns its own value stack and call frame array
 * but shares the global table and garbage collector with the
 * parent VM. This follows the Lua coroutine model:
 *
 *   co = coroutine.create(f)   -- allocate coroutine
 *   ok, ... = coroutine.resume(co, args...)  -- run/continue
 *   coroutine.yield(vals...)   -- suspend and return to caller
 *
 * State transitions:
 *   SUSPENDED --resume--> RUNNING --yield--> SUSPENDED
 *                                 --return--> DEAD
 *                                 --error---> DEAD
 *
 * When a coroutine resumes another, the caller becomes NORMAL
 * (suspended but not yieldable by external code) until the
 * callee yields or returns.
 *
 * @author Anthony Taliento
 * @date 2025-06-15
 * @version 0.2.0
 *
 * @copyright Copyright (c) 2025 Zorya Corporation
 * @license MIT
 *
 * DEPENDENCIES:
 *   - nova_vm.h (NovaCoroutine, NovaVM, NovaValue, etc.)
 *
 * THREAD SAFETY:
 *   Not thread-safe. Coroutines are cooperative, not preemptive.
 *
 * ZORYA-C COMPLIANCE: v2.0.0
 */

#include "nova/nova_vm.h"
#include <stdlib.h>
#include <string.h>

/* ============================================================
 * INTERNAL: FORWARD DECLARATIONS
 * ============================================================ */

/* Defined in nova_vm.c - we need these internal APIs */
extern void  novai_error(NovaVM *vm, int code, const char *msg);
extern int   novai_execute(NovaVM *vm);

/* ============================================================
 * PART 1: COROUTINE CREATION
 * ============================================================ */

/**
 * @brief Create a new coroutine wrapping a closure.
 *
 * Allocates an independent stack and call frame array for the
 * coroutine. The body closure is stored but NOT pushed yet;
 * that happens on first resume.
 *
 * @param vm   Parent VM (shared globals, GC)
 * @param body Closure to execute inside the coroutine
 *
 * @return New coroutine in SUSPENDED state, or NULL on failure.
 *
 * @pre vm != NULL && body != NULL
 *
 * COMPLEXITY: O(1)
 * THREAD SAFETY: Not thread-safe
 */
NovaCoroutine *nova_coroutine_new(NovaVM *vm, NovaClosure *body) {
    if (vm == NULL || body == NULL) {
        return NULL;
    }

    size_t co_size = sizeof(NovaCoroutine);

    /* Trigger GC BEFORE allocation */
    nova_gc_check(vm, co_size);

    NovaCoroutine *co = (NovaCoroutine *)malloc(co_size);
    if (co == NULL) {
        return NULL;
    }

    /* Allocate the coroutine's own value stack */
    size_t stack_bytes = NOVA_COROUTINE_STACK_SIZE * sizeof(NovaValue);
    co->stack = (NovaValue *)malloc(stack_bytes);
    if (co->stack == NULL) {
        free(co);
        return NULL;
    }

    /* Allocate the coroutine's own call frame array */
    size_t frames_bytes =
        (size_t)NOVA_COROUTINE_MAX_FRAMES * sizeof(NovaCallFrame);
    co->frames = (NovaCallFrame *)malloc(frames_bytes);
    if (co->frames == NULL) {
        free(co->stack);
        free(co);
        return NULL;
    }

    /* Initialize the stack to nil */
    for (size_t i = 0; i < NOVA_COROUTINE_STACK_SIZE; i++) {
        co->stack[i] = nova_value_nil();
    }

    /* Zero-initialize frames */
    memset(co->frames, 0, frames_bytes);

    /* Initialize coroutine fields */
    co->stack_top       = co->stack;
    co->stack_size      = NOVA_COROUTINE_STACK_SIZE;
    co->frame_count     = 0;
    co->max_frames      = NOVA_COROUTINE_MAX_FRAMES;
    co->body            = body;
    co->vm              = vm;
    co->status          = NOVA_CO_SUSPENDED;
    co->open_upvalues   = NULL;
    co->error_jmp       = NULL;
    co->meta_stop_frame = -1;
    co->nresults        = 0;
    co->pending_args    = NULL;
    co->pending_nargs   = 0;

    /* Link into GC */
    nova_gc_link(vm, &co->gc);
    co->gc.gc_type = NOVA_TYPE_THREAD;
    co->gc.gc_size = (uint32_t)(co_size + stack_bytes + frames_bytes);

    /* Write barrier: coroutine references the body closure */
    nova_gc_barrier(vm, &co->gc);

    return co;
}

/* ============================================================
 * PART 2: COROUTINE STACK GROWTH
 * ============================================================ */

/**
 * @brief Ensure the coroutine's stack has at least `needed` free slots.
 *
 * @param co     Coroutine
 * @param needed Number of free slots required
 *
 * @return 0 on success, -1 on failure.
 */
static int novai_co_stack_ensure(NovaCoroutine *co, size_t needed) {
    size_t used = (size_t)(co->stack_top - co->stack);
    size_t required = used + needed;

    if (required <= co->stack_size) {
        return 0;
    }

    if (required > NOVA_MAX_STACK_SIZE) {
        return -1;
    }

    size_t new_size = co->stack_size;
    while (new_size < required) {
        new_size *= 2;
    }
    if (new_size > NOVA_MAX_STACK_SIZE) {
        new_size = NOVA_MAX_STACK_SIZE;
    }

    NovaValue *new_stack = (NovaValue *)realloc(
        co->stack, new_size * sizeof(NovaValue));
    if (new_stack == NULL) {
        return -1;
    }

    /* Update pointers if stack moved */
    ptrdiff_t delta = new_stack - co->stack;
    co->stack = new_stack;
    co->stack_top = new_stack + used;
    co->stack_size = new_size;

    /* Update frame base pointers */
    for (int i = 0; i < co->frame_count; i++) {
        co->frames[i].base += delta;
    }

    /* Update open upvalue locations */
    for (NovaUpvalue *uv = co->open_upvalues; uv != NULL; uv = uv->next) {
        if (uv->location >= co->stack &&
            uv->location < co->stack + new_size) {
            uv->location += delta;
        }
    }

    /* Track in GC */
    co->vm->bytes_allocated +=
        (new_size - co->stack_size) * sizeof(NovaValue);

    return 0;
}

/* ============================================================
 * PART 3: VM STATE SWAP
 *
 * When a coroutine is resumed, we "swap in" its stack and
 * frames into the VM, and "swap out" the caller's state.
 * The VM's execution engine (novai_execute) then runs using
 * the coroutine's stack.
 * ============================================================ */

/**
 * @brief Saved VM state for swap-in/swap-out during coroutine execution.
 */
typedef struct {
    NovaValue     *stack;
    NovaValue     *stack_top;
    size_t         stack_size;
    int            frame_count;
    NovaUpvalue   *open_upvalues;
    NovaErrorJmp  *error_jmp;
    int            meta_stop_frame;
    NovaCoroutine *running_coroutine;
    int            status;
} NovaSavedState;

/**
 * @brief Save the VM's current execution state.
 */
static void novai_save_vm_state(NovaVM *vm, NovaSavedState *saved) {
    saved->stack             = vm->stack;
    saved->stack_top         = vm->stack_top;
    saved->stack_size        = vm->stack_size;
    saved->frame_count       = vm->frame_count;
    saved->open_upvalues     = vm->open_upvalues;
    saved->error_jmp         = vm->error_jmp;
    saved->meta_stop_frame   = vm->meta_stop_frame;
    saved->running_coroutine = vm->running_coroutine;
    saved->status            = vm->status;
}

/**
 * @brief Swap coroutine state into the VM.
 *
 * After this call, the VM's stack/frames/etc. point to the
 * coroutine's own storage.
 */
static void novai_swap_in_coroutine(NovaVM *vm, NovaCoroutine *co) {
    vm->stack             = co->stack;
    vm->stack_top         = co->stack_top;
    vm->stack_size        = co->stack_size;
    vm->open_upvalues     = co->open_upvalues;
    vm->error_jmp         = co->error_jmp;
    vm->meta_stop_frame   = co->meta_stop_frame;
    vm->running_coroutine = co;
    vm->status            = NOVA_VM_OK;

    /* Copy frame array into VM's fixed frame array */
    for (int i = 0; i < co->frame_count; i++) {
        vm->frames[i] = co->frames[i];
    }
    vm->frame_count = co->frame_count;
}

/**
 * @brief Save coroutine state from the VM and restore caller state.
 *
 * After this call, the coroutine's own storage is updated,
 * and the VM's stack/frames/etc. point back to the caller's storage.
 */
static void novai_swap_out_coroutine(NovaVM *vm, NovaCoroutine *co,
                                     const NovaSavedState *saved) {
    /* Save VM state back into coroutine */
    co->stack_top       = vm->stack_top;
    co->open_upvalues   = vm->open_upvalues;
    co->error_jmp       = vm->error_jmp;
    co->meta_stop_frame = vm->meta_stop_frame;

    /* Save frame state back into coroutine's frame array */
    for (int i = 0; i < vm->frame_count; i++) {
        co->frames[i] = vm->frames[i];
    }
    co->frame_count = vm->frame_count;

    /* Restore caller's state */
    vm->stack             = saved->stack;
    vm->stack_top         = saved->stack_top;
    vm->stack_size        = saved->stack_size;
    vm->open_upvalues     = saved->open_upvalues;
    vm->error_jmp         = saved->error_jmp;
    vm->meta_stop_frame   = saved->meta_stop_frame;
    vm->running_coroutine = saved->running_coroutine;
    vm->status            = saved->status;

    /* Restore caller frames */
    vm->frame_count = saved->frame_count;
}

/* ============================================================
 * PART 4: COROUTINE RESUME
 * ============================================================ */

/**
 * @brief Resume a suspended coroutine.
 *
 * On first resume: pushes the body closure and arguments onto
 * the coroutine's stack, sets up frame 0, and runs novai_execute.
 *
 * On subsequent resumes: pushes resumed values as the return
 * values of the previous yield, then continues novai_execute.
 *
 * @param vm     Parent VM
 * @param co     Coroutine to resume (must be SUSPENDED)
 * @param nargs  Number of arguments on the caller's stack
 *
 * @return NOVA_VM_OK if coroutine returned normally,
 *         NOVA_VM_YIELD if coroutine yielded,
 *         or error code on failure.
 *
 * @pre vm != NULL && co != NULL && co->status == NOVA_CO_SUSPENDED
 *
 * COMPLEXITY: O(execution time of coroutine until yield/return)
 * THREAD SAFETY: Not thread-safe
 */
int nova_coroutine_resume(NovaVM *vm, NovaCoroutine *co, int nargs) {
    if (vm == NULL || co == NULL) {
        return NOVA_VM_ERR_NULLPTR;
    }

    if (co->status != NOVA_CO_SUSPENDED) {
        novai_error(vm, NOVA_VM_ERR_RUNTIME,
                    "cannot resume non-suspended coroutine");
        return vm->status;
    }

    int first_resume = (co->frame_count == 0);

    /* -- Copy arguments from caller's stack into coroutine's stack -- */

    if (first_resume) {
        /* First resume: push closure + args onto coroutine stack */
        size_t slots_needed = (size_t)(1 + nargs) +
                              co->body->proto->max_stack + 10;
        if (novai_co_stack_ensure(co, slots_needed) != 0) {
            novai_error(vm, NOVA_VM_ERR_MEMORY,
                        "coroutine stack allocation failed");
            return vm->status;
        }

        /* Push the closure value at slot 0 */
        *co->stack_top = nova_value_closure(co->body);
        co->stack_top++;

        /* Push arguments */
        NovaValue *args_src = vm->stack_top - nargs;
        for (int i = 0; i < nargs; i++) {
            *co->stack_top = args_src[i];
            co->stack_top++;
        }

        /* Pop args from caller stack */
        vm->stack_top -= nargs;

        /* Set up frame 0 for the body closure */
        NovaCallFrame *frame = &co->frames[0];
        frame->proto       = co->body->proto;
        frame->closure     = co->body;
        frame->base        = co->stack + 1;  /* After the closure slot */
        frame->ip          = co->body->proto->code;
        frame->num_results = -1;  /* Variable results */
        frame->num_args    = nargs;
        frame->varargs     = NULL;
        frame->num_varargs = 0;
        co->frame_count    = 1;

    } else {
        /* Subsequent resume: push values as yield results.
         * The yield left co->stack_top at base[A + nyield_prev].
         * Resume values overwrite that area as the "return values"
         * of the coroutine.yield() call inside the coroutine.
         * stack_top - nresults_prev gives us base[A]. */
        if (novai_co_stack_ensure(co, (size_t)(nargs + 2)) != 0) {
            novai_error(vm, NOVA_VM_ERR_MEMORY,
                        "coroutine stack allocation failed");
            return vm->status;
        }

        /* Compute destination: same position where yield values were */
        NovaValue *dest = co->stack_top - co->nresults;

        /* Copy resume args from caller's stack into coroutine */
        NovaValue *args_src = vm->stack_top - nargs;
        for (int i = 0; i < nargs; i++) {
            dest[i] = args_src[i];
        }

        /* Pop args from caller stack */
        vm->stack_top -= nargs;

        /* Adjust coroutine stack_top past resume values */
        co->stack_top = dest + nargs;
        co->nresults = nargs;
    }

    /* -- Swap VM state to coroutine -- */
    NovaSavedState saved;
    novai_save_vm_state(vm, &saved);

    /* Save caller frames that swap_in will overwrite.
     * novai_swap_in_coroutine copies co->frame_count frames into
     * vm->frames[], destroying the caller's frame data.  We must
     * save and restore those frames so the caller can continue
     * correctly after the coroutine finishes/yields.  */
    int caller_fc = vm->frame_count;
    if (caller_fc > NOVA_COROUTINE_MAX_FRAMES) {
        caller_fc = NOVA_COROUTINE_MAX_FRAMES;
    }
    NovaCallFrame caller_frames[NOVA_COROUTINE_MAX_FRAMES];
    memcpy(caller_frames, vm->frames,
           (size_t)caller_fc * sizeof(NovaCallFrame));

    /* Register the caller's stack with the VM so the GC can mark
     * it as an additional root set.  Without this, a GC triggered
     * inside the coroutine would only see the coroutine's stack
     * and would sweep objects only referenced from the caller. */
    NovaSavedStackRef gc_ref;
    gc_ref.stack       = vm->stack;
    gc_ref.stack_top   = vm->stack_top;
    gc_ref.stack_size  = vm->stack_size;
    gc_ref.frame_count = caller_fc;
    gc_ref.frames      = caller_frames;
    gc_ref.prev        = vm->saved_stacks;
    vm->saved_stacks   = &gc_ref;

    /* Mark caller as NORMAL if it's a coroutine */
    NovaCoroutine *caller_co = vm->running_coroutine;
    if (caller_co != NULL) {
        caller_co->status = NOVA_CO_NORMAL;
    }

    /* Swap in coroutine state */
    novai_swap_in_coroutine(vm, co);
    co->status = NOVA_CO_RUNNING;

    /* -- Execute the coroutine -- */

    /* Set up a coroutine-local error handler.  Without this,
     * error() inside the coroutine would longjmp directly to an
     * outer pcall handler, bypassing our swap-out cleanup and
     * leaving the VM in a corrupted state (coroutine stack still
     * swapped in, saved_stacks dangling, etc.).  We intercept
     * the longjmp here, do proper cleanup below, and let the
     * error propagate through normal return codes. */
    NovaErrorJmp co_ej;
    co_ej.previous    = vm->error_jmp;
    co_ej.status      = NOVA_VM_OK;
    co_ej.frame_count = vm->frame_count;
    co_ej.stack_top   = vm->stack_top;
    vm->error_jmp     = &co_ej;

    int result;
    if (setjmp(co_ej.buf) == 0) {
        result = novai_execute(vm);
    } else {
        /* error() inside the coroutine longjmp'd here */
        result = co_ej.status;
    }

    /* Restore the previous error handler */
    vm->error_jmp = co_ej.previous;

    /* -- Swap back and handle results -- */

    /* Save coroutine's updated stack_top and frames before swap-out */
    NovaValue *co_stack_top = vm->stack_top;
    (void)co_stack_top;  /* Used in OK path below */

    novai_swap_out_coroutine(vm, co, &saved);

    /* Unregister the caller's stack from the GC root chain now
     * that we're back on the caller's stack. */
    vm->saved_stacks = gc_ref.prev;

    /* Restore caller frames that were overwritten by swap_in */
    memcpy(vm->frames, caller_frames,
           (size_t)caller_fc * sizeof(NovaCallFrame));

    /* Restore caller coroutine status */
    if (caller_co != NULL) {
        caller_co->status = NOVA_CO_RUNNING;
    }

    if (result == NOVA_VM_YIELD) {
        /* Coroutine yielded: copy yielded values to caller's stack.
         * The CALL handler moved yield values to base[A..A+ny-1]
         * and set stack_top = base[A + ny].  After swap-out,
         * these are at co->stack_top - nresults .. co->stack_top - 1. */
        co->status = NOVA_CO_SUSPENDED;

        int nyielded = co->nresults;
        NovaValue *yield_src = co->stack_top - nyielded;

        for (int i = 0; i < nyielded; i++) {
            *vm->stack_top = yield_src[i];
            vm->stack_top++;
        }

        return NOVA_VM_YIELD;

    } else if (result == NOVA_VM_OK) {
        /* Coroutine returned normally: copy return values to caller */
        co->status = NOVA_CO_DEAD;

        /* Return values are at the bottom of the coroutine's stack
         * (after RETURN handler ran). co_stack_top points past them. */
        int nret = (int)(co_stack_top - co->stack);
        for (int i = 0; i < nret; i++) {
            *vm->stack_top = co->stack[i];
            vm->stack_top++;
        }
        co->nresults = nret;

        return NOVA_VM_OK;

    } else {
        /* Coroutine errored */
        co->status = NOVA_CO_DEAD;
        return result;
    }
}

/* ============================================================
 * PART 5: COROUTINE YIELD
 * ============================================================ */

/**
 * @brief Yield the currently running coroutine.
 *
 * This is called from within a running coroutine (either from
 * the YIELD opcode handler or from coroutine.yield() C function).
 *
 * The yield values are at the top of the coroutine's stack.
 * We save the coroutine's state and signal the dispatch loop
 * to return NOVA_VM_YIELD.
 *
 * @param vm       Parent VM (stack currently pointing to coroutine)
 * @param nresults Number of values being yielded
 *
 * @return NOVA_VM_YIELD
 *
 * @pre vm != NULL
 * @pre vm->running_coroutine != NULL
 *
 * COMPLEXITY: O(nresults)
 * THREAD SAFETY: Not thread-safe
 */
int nova_coroutine_yield(NovaVM *vm, int nresults) {
    if (vm == NULL) {
        return NOVA_VM_ERR_NULLPTR;
    }

    NovaCoroutine *co = vm->running_coroutine;
    if (co == NULL) {
        novai_error(vm, NOVA_VM_ERR_RUNTIME,
                    "cannot yield from main thread");
        return vm->status;
    }

    /* Save the number of yielded values so resume knows how many
     * to pick up from the coroutine's stack top */
    co->nresults = nresults;

    /* Signal the dispatch loop to exit with NOVA_VM_YIELD.
     * The dispatch loop will return this to nova_coroutine_resume(),
     * which handles the state swap and value copying. */
    vm->status = NOVA_VM_YIELD;
    return NOVA_VM_YIELD;
}

/* ============================================================
 * PART 6: STATUS QUERY
 * ============================================================ */

/**
 * @brief Get the human-readable status string for a coroutine.
 *
 * @param co  Coroutine to query (must not be NULL)
 *
 * @return "suspended", "running", "dead", or "normal"
 */
const char *nova_coroutine_status_str(NovaCoroutine *co) {
    if (co == NULL) {
        return "dead";
    }
    switch (co->status) {
        case NOVA_CO_SUSPENDED: return "suspended";
        case NOVA_CO_RUNNING:   return "running";
        case NOVA_CO_DEAD:      return "dead";
        case NOVA_CO_NORMAL:    return "normal";
        default:                return "unknown";
    }
}
