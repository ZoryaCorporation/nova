/**
 * @file nova_gc.c
 * @brief Nova Language - Incremental Tri-Color Mark-Sweep Garbage Collector
 *
 * @author Anthony Taliento
 * @date 2026-02-08
 * @version 0.2.0
 *
 * @copyright Copyright (c) 2026 Zorya Corporation
 * @license MIT
 *
 * DESCRIPTION:
 *   Implements a tri-color incremental mark-sweep GC for the Nova VM.
 *   All heap objects (strings, tables, closures, upvalues) are linked
 *   into a single all_objects list via their NovaGCHeader.gc_next field.
 *
 *   The collector operates in three phases:
 *     PAUSE -> MARK -> SWEEP -> PAUSE -> ...
 *
 *   MARK phase: Incrementally traverses reachable objects.
 *     - Root set: stack, globals table, open upvalues, gray list
 *     - Gray objects are popped and their children marked gray
 *     - Uses two white colors (flip-flop) so new allocations
 *       during marking are automatically the "other" white
 *
 *   SWEEP phase: Walks all_objects, freeing unreachable (white) objects.
 *     - Incremental: processes a batch per step
 *     - Live objects are re-colored to the current white
 *
 *   Pacing: GC work is proportional to allocation pressure.
 *     - gc_debt accumulates with each allocation
 *     - When debt > 0, nova_gc_step does work proportional to debt
 *     - gc_pause controls how much live data must grow before a new cycle
 *     - gc_step_mul controls how much work per step
 *
 * DESIGN DECISIONS:
 *   - No arena for GC objects: individual malloc/free per object.
 *     GC objects have variable lifetimes scattered throughout memory;
 *     arenas are wrong for selective deallocation.
 *   - Write barrier is "barrier forward" (shade parent gray on mutation).
 *     Simpler than Lua's "barrier back" approach, fewer edge cases.
 *   - Strings are treated as leaves (no children to traverse).
 *   - Tables traverse both array and hash parts + metatable.
 *   - Closures traverse their upvalue array.
 *   - Upvalues traverse their closed value (if closed).
 *
 * PERFORMANCE:
 *   - Allocation: O(1) — malloc + link to list head
 *   - Mark step:  O(k) — process k gray objects per step
 *   - Sweep step: O(k) — check k objects per step
 *   - Full collect: O(n) — all objects traversed
 *   - Write barrier: O(1) — single color check + gray promotion
 *
 * THREAD SAFETY:
 *   Not thread-safe. Each VM has its own GC state.
 *
 * ZORYA-C COMPLIANCE: v2.0.0
 */

#include "nova/nova_vm.h"
#include "nova/nova_trace.h"

#include <stdlib.h>
#include <string.h>

/* Internal type tag for upvalues — beyond public NovaValueType range.
 * Upvalues need a distinct gc_type so the GC knows how to traverse/free them
 * separately from NOVA_TYPE_USERDATA (8). */
#define NOVAI_GC_TYPE_UPVALUE 10

/* ============================================================
 * GC TUNING CONSTANTS
 * ============================================================ */

/** Initial GC threshold (128KB) — first collection triggers here */
#define NOVA_GC_INITIAL_THRESHOLD   ((size_t)(128 * 1024))

/** Default pause: 200% — wait until memory doubles before new cycle */
#define NOVA_GC_DEFAULT_PAUSE       200

/** Default step multiplier: 200 — do 2x work relative to allocation */
#define NOVA_GC_DEFAULT_STEP_MUL    200

/** Objects processed per sweep step */
#define NOVA_GC_SWEEP_BATCH         40

/** Gray objects processed per mark step */
#define NOVA_GC_MARK_BATCH          40

/** Minimum step size in bytes of work */
#define NOVA_GC_MIN_STEP_SIZE       ((size_t)1024)

/* ============================================================
 * INTERNAL HELPERS
 * ============================================================ */

/**
 * @brief Get the "other" white color.
 *
 * @param vm  VM instance
 * @return    The white color NOT currently in use
 */
static inline uint8_t novai_gc_other_white(const NovaVM *vm) {
    return (uint8_t)(vm->gc_current_white ^ 1u);
}

/**
 * @brief Check if an object is "dead" (the other white).
 *
 * An object with the non-current white color was not reached
 * during the last mark phase and is eligible for collection.
 *
 * @param vm  VM instance
 * @param hdr Object header
 * @return    1 if dead, 0 if alive
 */
static inline int novai_gc_is_dead(const NovaVM *vm,
                                   const NovaGCHeader *hdr) {
    return hdr->gc_color == novai_gc_other_white(vm);
}

/**
 * @brief Mark a single NovaValue's referent gray (if it's a GC object).
 *
 * Only heap types (STRING, TABLE, FUNCTION, USERDATA) have GC headers.
 * NIL, BOOL, INTEGER, NUMBER, CFUNCTION are immediates — no GC.
 *
 * @param vm  VM instance
 * @param v   Value to mark
 */
static void novai_gc_mark_value(NovaVM *vm, NovaValue v) {
    NovaGCHeader *hdr = NULL;

    NovaValueType vtype = nova_typeof(v);
    switch (vtype) {
        case NOVA_TYPE_STRING:
            /* Interned strings are immortal (owned by string_pool,
             * not on all_objects).  They are always NOVA_GC_BLACK
             * and never need marking or traversal.  Skip entirely. */
            return;
        case NOVA_TYPE_TABLE:
            if (nova_as_table(v) != NULL) {
                hdr = NOVA_GC_HDR(nova_as_table(v));
            }
            break;
        case NOVA_TYPE_FUNCTION:
            if (nova_as_closure(v) != NULL) {
                hdr = NOVA_GC_HDR(nova_as_closure(v));
            }
            break;
        case NOVA_TYPE_USERDATA:
            if (nova_as_userdata(v) != NULL) {
                hdr = NOVA_GC_HDR(nova_as_userdata(v));
            }
            break;
        case NOVA_TYPE_THREAD:
            if (nova_as_coroutine(v) != NULL) {
                hdr = NOVA_GC_HDR(nova_as_coroutine(v));
            }
            break;
        default:
            /* Immediate types: NIL, BOOL, INTEGER, NUMBER, CFUNCTION */
            return;
    }

    if (hdr == NULL) {
        return;
    }

    /* Only mark if white (unreached) */
    if (!NOVA_GC_IS_WHITE(hdr)) {
        return;
    }

    /* Shade gray: push onto gray list for later traversal.
     * Uses gc_gray (NOT gc_next — that's the all_objects chain). */
    hdr->gc_color = NOVA_GC_GRAY;
    hdr->gc_gray = vm->gray_list;
    vm->gray_list = hdr;
}

/**
 * @brief Mark a GC object header gray (non-value variant).
 *
 * @param vm  VM instance
 * @param hdr Object header (may be NULL)
 */
static void novai_gc_mark_object(NovaVM *vm, NovaGCHeader *hdr) {
    if (hdr == NULL || !NOVA_GC_IS_WHITE(hdr)) {
        return;
    }
    hdr->gc_color = NOVA_GC_GRAY;
    hdr->gc_gray = vm->gray_list;
    vm->gray_list = hdr;
}

/* ============================================================
 * TRAVERSAL: Process gray objects (mark their children)
 * ============================================================ */

/**
 * @brief Traverse a table: mark all reachable values.
 *
 * Marks: array values, hash keys & values, metatable.
 *
 * @param vm  VM instance
 * @param t   Table to traverse
 * @return    Approximate "work" done (bytes traversed)
 */
static size_t novai_gc_traverse_table(NovaVM *vm, NovaTable *t) {
    size_t work = sizeof(NovaTable);

    /* Mark metatable */
    if (t->metatable != NULL) {
        novai_gc_mark_object(vm, NOVA_GC_HDR(t->metatable));
    }

    /* Mark array part */
    for (uint32_t i = 0; i < t->array_used; i++) {
        novai_gc_mark_value(vm, t->array[i]);
    }
    work += (size_t)t->array_size * sizeof(NovaValue);

    /* Mark hash part */
    for (uint32_t i = 0; i < t->hash_size; i++) {
        if (t->hash[i].occupied) {
            novai_gc_mark_value(vm, t->hash[i].key);
            novai_gc_mark_value(vm, t->hash[i].value);
        }
    }
    work += (size_t)t->hash_size * sizeof(NovaTableEntry);

    return work;
}

/**
 * @brief Traverse a closure: mark upvalues.
 *
 * @param vm  VM instance
 * @param cl  Closure to traverse
 * @return    Approximate work done
 */
static size_t novai_gc_traverse_closure(NovaVM *vm, NovaClosure *cl) {
    size_t work = sizeof(NovaClosure);

    for (uint8_t i = 0; i < cl->upvalue_count; i++) {
        if (cl->upvalues[i] != NULL) {
            novai_gc_mark_object(vm, NOVA_GC_HDR(cl->upvalues[i]));
        }
    }
    work += (size_t)cl->upvalue_count * sizeof(NovaUpvalue *);

    return work;
}

/**
 * @brief Traverse an upvalue: mark the closed value if closed.
 *
 * @param vm  VM instance
 * @param uv  Upvalue to traverse
 * @return    Approximate work done
 */
static size_t novai_gc_traverse_upvalue(NovaVM *vm, NovaUpvalue *uv) {
    /* If the upvalue is closed, its value may reference GC objects */
    if (uv->location == &uv->closed) {
        novai_gc_mark_value(vm, uv->closed);
    }
    /* If still open, the stack slot is traversed as part of roots */
    return sizeof(NovaUpvalue);
}

/**
 * @brief Process one gray object: traverse its children, turn black.
 *
 * Pops from gray_list, traverses children (marking them gray),
 * then colors the object black.
 *
 * @param vm  VM instance
 * @return    Approximate work done (bytes)
 */
static size_t novai_gc_propagate_one(NovaVM *vm) {
    NovaGCHeader *hdr = vm->gray_list;
    if (hdr == NULL) {
        return 0;
    }

    /* Pop from gray list (via gc_gray, NOT gc_next) */
    vm->gray_list = hdr->gc_gray;
    hdr->gc_gray = NULL;

    /* Traverse based on object type */
    size_t work = 0;
    switch (hdr->gc_type) {
        case NOVA_TYPE_STRING:
            /* Strings are leaves — no children to traverse */
            work = sizeof(NovaString) + nova_str_len((NovaString *)hdr);
            break;

        case NOVA_TYPE_TABLE:
            work = novai_gc_traverse_table(vm, (NovaTable *)hdr);
            break;

        case NOVA_TYPE_FUNCTION:
            work = novai_gc_traverse_closure(vm, (NovaClosure *)hdr);
            break;

        case NOVAI_GC_TYPE_UPVALUE:
            work = novai_gc_traverse_upvalue(vm, (NovaUpvalue *)hdr);
            break;

        case NOVA_TYPE_THREAD: {
            NovaCoroutine *co = (NovaCoroutine *)hdr;
            work = sizeof(NovaCoroutine);
            /* Mark the body closure */
            if (co->body != NULL) {
                novai_gc_mark_object(vm, NOVA_GC_HDR(co->body));
            }
            /* Mark all live values on the coroutine's stack */
            for (NovaValue *slot = co->stack; slot < co->stack_top; slot++) {
                novai_gc_mark_value(vm, *slot);
            }
            /* Mark closures in call frames */
            for (int i = 0; i < co->frame_count; i++) {
                if (co->frames[i].closure != NULL) {
                    novai_gc_mark_object(vm, NOVA_GC_HDR(co->frames[i].closure));
                }
            }
            /* Mark open upvalues */
            for (NovaUpvalue *uv = co->open_upvalues; uv != NULL; uv = uv->next) {
                novai_gc_mark_object(vm, NOVA_GC_HDR(uv));
            }
            /* Mark pending args (stashed from async function call) */
            if (co->pending_args != NULL) {
                for (int pa = 0; pa < co->pending_nargs; pa++) {
                    novai_gc_mark_value(vm, co->pending_args[pa]);
                }
            }
            work += co->stack_size * sizeof(NovaValue);
            break;
        }

        default:
            work = 16;
            break;
    }

    /* Color black: fully scanned */
    hdr->gc_color = NOVA_GC_BLACK;
    return work;
}

/* ============================================================
 * ROOT SCANNING
 * ============================================================ */

/**
 * @brief Mark all GC roots: stack, globals, open upvalues.
 *
 * This must be called at the start of a mark cycle to seed
 * the gray list with all directly reachable objects.
 *
 * @param vm  VM instance
 */
static void novai_gc_mark_roots(NovaVM *vm) {
    /* 1. Mark the value stack.  Compute the true GC scan limit as
     * the maximum of stack_top and every frame's register window
     * (base + proto->max_stack).  During C function calls stack_top
     * is temporarily shrunk to cover only the cfunc's arguments,
     * which would hide the calling frame's registers and cause
     * live objects to be incorrectly swept.  Scanning each frame's
     * full window prevents that. */
    NovaValue *gc_top = vm->stack_top;
    for (int i = 0; i < vm->frame_count; i++) {
        const NovaCallFrame *f = &vm->frames[i];
        if (f->proto != NULL) {
            NovaValue *frame_end = f->base + f->proto->max_stack;
            if (frame_end > gc_top) {
                gc_top = frame_end;
            }
        }
    }
    for (NovaValue *p = vm->stack; p < gc_top; p++) {
        novai_gc_mark_value(vm, *p);
    }

    /* 2. Mark call frame varargs */
    for (int i = 0; i < vm->frame_count; i++) {
        NovaCallFrame *f = &vm->frames[i];
        if (f->varargs != NULL) {
            for (int j = 0; j < f->num_varargs; j++) {
                novai_gc_mark_value(vm, f->varargs[j]);
            }
        }
        /* Mark closures referenced by frames */
        if (f->closure != NULL) {
            novai_gc_mark_object(vm, NOVA_GC_HDR(f->closure));
        }
    }

    /* 3. Mark global table */
    if (vm->globals != NULL) {
        novai_gc_mark_object(vm, NOVA_GC_HDR(vm->globals));
    }

    /* 4. Mark open upvalues (still pointing at stack) */
    for (NovaUpvalue *uv = vm->open_upvalues; uv != NULL; uv = uv->next) {
        novai_gc_mark_object(vm, NOVA_GC_HDR(uv));
    }

    /* 5. Mark the currently running coroutine.
     * During coroutine execution, the VM's stack/frames are swapped
     * to the coroutine's storage.  The caller's stack (which holds
     * the NovaValue reference to the coroutine) is saved on the C
     * stack and invisible to the GC.  We must explicitly mark the
     * running coroutine to prevent it from being swept. */
    if (vm->running_coroutine != NULL) {
        novai_gc_mark_object(vm, NOVA_GC_HDR(vm->running_coroutine));
    }

    /* 6. Mark all saved caller stacks (coroutine nesting).
     * When coroutine A resumes B, A's stack is saved in a
     * NovaSavedStackRef on the C stack and chained via
     * vm->saved_stacks.  We must GC-mark all values on those
     * saved stacks, plus their frame closures and varargs,
     * so nothing reachable from the caller(s) gets swept. */
    for (const NovaSavedStackRef *ref = vm->saved_stacks;
         ref != NULL; ref = ref->prev) {
        /* Compute the GC scan limit for this saved stack, respecting
         * each frame's register window (identical logic to step 1). */
        NovaValue *ref_gc_top = ref->stack_top;
        for (int i = 0; i < ref->frame_count; i++) {
            const NovaCallFrame *f = &ref->frames[i];
            if (f->proto != NULL) {
                NovaValue *frame_end = f->base + f->proto->max_stack;
                if (frame_end > ref_gc_top) {
                    ref_gc_top = frame_end;
                }
            }
        }
        /* Mark all values on the saved stack */
        for (NovaValue *p = ref->stack; p < ref_gc_top; p++) {
            novai_gc_mark_value(vm, *p);
        }
        /* Mark frame closures and varargs */
        for (int i = 0; i < ref->frame_count; i++) {
            const NovaCallFrame *f = &ref->frames[i];
            if (f->varargs != NULL) {
                for (int j = 0; j < f->num_varargs; j++) {
                    novai_gc_mark_value(vm, f->varargs[j]);
                }
            }
            if (f->closure != NULL) {
                novai_gc_mark_object(vm, NOVA_GC_HDR(f->closure));
            }
        }
    }

    /* 7. Mark all tasks in the async task queue.
     * Spawned tasks are coroutines that may be invisible from the
     * main stack if no variable holds a reference. The scheduler
     * owns them via vm->task_queue. */
    for (int i = 0; i < vm->task_count; i++) {
        if (vm->task_queue[i] != NULL) {
            novai_gc_mark_object(vm, NOVA_GC_HDR(vm->task_queue[i]));
        }
    }

    /* 8. Mark per-type metatables.
     * The string metatable is a module-global pointer that must
     * survive GC.  Marking it here ensures it stays reachable
     * across every collection cycle. */
    if (vm->string_mt != NULL) {
        novai_gc_mark_object(vm, NOVA_GC_HDR(vm->string_mt));
    }
}

/* ============================================================
 * OBJECT FREEING
 * ============================================================ */

/**
 * @brief Free a single GC object based on its type.
 *
 * Deallocates the object and its owned memory (e.g., table
 * array/hash parts, closure upvalue pointer array).
 * Does NOT update vm->bytes_allocated — caller handles that.
 *
 * @param hdr  GC header of the object to free
 * @return     Number of bytes freed
 */
static size_t novai_gc_free_object(NovaGCHeader *hdr) {
    size_t freed = 0;

    switch (hdr->gc_type) {
        case NOVA_TYPE_STRING: {
            /* Interned strings are NOT on all_objects and should
             * never reach this path.  If dagger_set failed during
             * nova_string_new, the string was GC-linked as fallback
             * and can be freed here normally. */
            NovaString *str = (NovaString *)hdr;
            freed = sizeof(NovaString) + nova_str_len(str) + 1;
            free(str);
            break;
        }
        case NOVA_TYPE_TABLE: {
            NovaTable *t = (NovaTable *)hdr;
            freed = sizeof(NovaTable);
            if (t->array != NULL) {
                freed += (size_t)t->array_size * sizeof(NovaValue);
                free(t->array);
            }
            if (t->hash != NULL) {
                freed += (size_t)t->hash_size * sizeof(NovaTableEntry);
                free(t->hash);
            }
            free(t);
            break;
        }
        case NOVA_TYPE_FUNCTION: {
            NovaClosure *cl = (NovaClosure *)hdr;
            freed = sizeof(NovaClosure) +
                    (size_t)cl->upvalue_count * sizeof(NovaUpvalue *);
            /* Don't free upvalues themselves -- they're separate GC objects */
            free(cl);
            break;
        }
        case NOVAI_GC_TYPE_UPVALUE: {
            NovaUpvalue *uv = (NovaUpvalue *)hdr;
            freed = sizeof(NovaUpvalue);
            free(uv);
            break;
        }
        case NOVA_TYPE_THREAD: {
            NovaCoroutine *co = (NovaCoroutine *)hdr;
            freed = sizeof(NovaCoroutine);
            if (co->stack != NULL) {
                freed += co->stack_size * sizeof(NovaValue);
                free(co->stack);
            }
            if (co->frames != NULL) {
                freed += (size_t)co->max_frames * sizeof(NovaCallFrame);
                free(co->frames);
            }
            if (co->pending_args != NULL) {
                freed += (size_t)co->pending_nargs * sizeof(NovaValue);
                free(co->pending_args);
            }
            free(co);
            break;
        }
        default:
            /* Unknown type, just free the raw pointer */
            freed = (size_t)hdr->gc_size;
            free(hdr);
            break;
    }

    return freed;
}

/* ============================================================
 * SWEEP PHASE
 * ============================================================ */

/**
 * @brief Sweep a batch of objects from the all_objects list.
 *
 * Walks from gc_sweep_pos (a pointer-to-pointer into the chain),
 * freeing dead (other-white) objects and re-coloring live objects
 * to the current white.
 *
 * gc_sweep_pos is a NovaGCHeader** that points into the chain:
 * either &vm->all_objects or &some_alive_object->gc_next.
 * This correctly maintains the linked list when unlinking.
 *
 * @param vm     VM instance
 * @param limit  Maximum objects to process in this step
 * @return       Number of objects processed (0 = sweep complete)
 */
static size_t novai_gc_sweep_step(NovaVM *vm, size_t limit) {
    NovaGCHeader **pp = NULL;
    size_t count = 0;

    /* Resume sweep from saved pointer-to-pointer position */
    if (vm->gc_sweep_pos != NULL) {
        pp = vm->gc_sweep_pos;
    } else {
        pp = &vm->all_objects;
    }

    uint8_t dead_white = novai_gc_other_white(vm);

    while (*pp != NULL && count < limit) {
        NovaGCHeader *obj = *pp;
        count++;

        if (obj->gc_color == dead_white) {
            /* Dead object: unlink and free */
            *pp = obj->gc_next;  /* Remove from list (via correct chain pointer) */
            size_t freed = novai_gc_free_object(obj);
            if (vm->bytes_allocated >= freed) {
                vm->bytes_allocated -= freed;
            }
        } else {
            /* Live object: re-color to current white for next cycle */
            obj->gc_color = vm->gc_current_white;
            pp = &obj->gc_next;
        }
    }

    if (*pp == NULL) {
        /* Sweep complete */
        vm->gc_sweep_pos = NULL;
        return 0;
    }

    /* Save pointer-to-pointer position for next step */
    vm->gc_sweep_pos = pp;
    return count;
}

/* ============================================================
 * GC STATE MACHINE
 * ============================================================ */

/**
 * @brief Perform incremental GC work.
 *
 * Executes work proportional to `work_target` bytes.
 * Returns the amount of work actually done.
 *
 * @param vm           VM instance
 * @param work_target  Target amount of work in bytes
 * @return             Approximate work done
 */
static size_t novai_gc_run_step(NovaVM *vm, size_t work_target) {
    size_t work_done = 0;

    switch (vm->gc_phase) {
        case NOVA_GC_PHASE_PAUSE: {
            /* Start a new cycle: flip whites and mark roots */
            vm->gc_current_white ^= 1u;
            novai_gc_mark_roots(vm);
            vm->gc_phase = NOVA_GC_PHASE_MARK;
            work_done = (size_t)(vm->stack_top - vm->stack) * sizeof(NovaValue);
            break;
        }

        case NOVA_GC_PHASE_MARK: {
            /* Process gray objects until work budget exhausted or done */
            while (vm->gray_list != NULL && work_done < work_target) {
                work_done += novai_gc_propagate_one(vm);
            }

            if (vm->gray_list == NULL) {
                /* Mark phase complete — transition to sweep */
                vm->gc_phase = NOVA_GC_PHASE_SWEEP;
                vm->gc_sweep_pos = &vm->all_objects;
            }
            break;
        }

        case NOVA_GC_PHASE_SWEEP: {
            /* Sweep batch of objects */
            size_t swept = novai_gc_sweep_step(vm, NOVA_GC_SWEEP_BATCH);
            work_done = swept * 32;  /* Approximate */

            if (swept == 0) {
                /* Sweep complete — back to pause */
                vm->gc_phase = NOVA_GC_PHASE_PAUSE;
                vm->gc_estimate = vm->bytes_allocated;
                /* Set next threshold based on pause percentage */
                vm->gc_threshold = vm->gc_estimate *
                                   (size_t)vm->gc_pause / 100;
                if (vm->gc_threshold < NOVA_GC_INITIAL_THRESHOLD) {
                    vm->gc_threshold = NOVA_GC_INITIAL_THRESHOLD;
                }
                vm->gc_debt = 0;
            }
            break;
        }

        default:
            vm->gc_phase = NOVA_GC_PHASE_PAUSE;
            break;
    }

    return work_done;
}

/* ============================================================
 * PUBLIC API
 * ============================================================ */

void nova_gc_init(NovaVM *vm) {
    if (vm == NULL) {
        return;
    }
    vm->all_objects      = NULL;
    vm->gray_list        = NULL;
    vm->gc_threshold     = NOVA_GC_INITIAL_THRESHOLD;
    vm->gc_pause         = NOVA_GC_DEFAULT_PAUSE;
    vm->gc_step_mul      = NOVA_GC_DEFAULT_STEP_MUL;
    vm->gc_phase         = NOVA_GC_PHASE_PAUSE;
    vm->gc_current_white = NOVA_GC_WHITE0;
    vm->gc_running       = 1;
    vm->gc_emergency     = 0;
    vm->gc_sweep_pos     = NULL;
    vm->gc_estimate      = 0;
    vm->gc_debt          = 0;
}

void nova_gc_link(NovaVM *vm, NovaGCHeader *hdr) {
    if (vm == NULL || hdr == NULL) {
        return;
    }
    /* Set color to current white (alive for this cycle) */
    hdr->gc_color = vm->gc_current_white;

    /* Link at head of all_objects */
    hdr->gc_next = vm->all_objects;
    vm->all_objects = hdr;

    NTRACE_GC_EVENT("link", hdr, hdr->gc_size);
}

void nova_gc_check(NovaVM *vm, size_t size) {
    if (vm == NULL || vm->gc_running == 0) {
        return;
    }

    vm->gc_debt += size;

    /* Trigger step if debt exceeds a reasonable minimum */
    if (vm->bytes_allocated >= vm->gc_threshold ||
        vm->gc_phase != NOVA_GC_PHASE_PAUSE) {
        nova_gc_step(vm);
    }
}

void nova_gc_step(NovaVM *vm) {
    if (vm == NULL || vm->gc_running == 0) {
        return;
    }

    NTRACE(GC, "gc_step phase=%d debt=%zu allocated=%zu",
           vm->gc_phase, vm->gc_debt, vm->bytes_allocated);

    /* Calculate work target proportional to debt and step multiplier */
    size_t work_target = vm->gc_debt * (size_t)vm->gc_step_mul / 100;
    if (work_target < NOVA_GC_MIN_STEP_SIZE) {
        work_target = NOVA_GC_MIN_STEP_SIZE;
    }

    size_t work_done = 0;
    while (work_done < work_target) {
        size_t step = novai_gc_run_step(vm, work_target - work_done);
        work_done += step;

        /* If we completed a full cycle (back to PAUSE), stop */
        if (vm->gc_phase == NOVA_GC_PHASE_PAUSE && work_done > 0) {
            break;
        }

        /* Safety: if step did no work, break to avoid infinite loop */
        if (step == 0) {
            break;
        }
    }

    /* Reset debt (partially — keep any overflow) */
    if (work_done >= vm->gc_debt) {
        vm->gc_debt = 0;
    } else {
        vm->gc_debt -= work_done;
    }
}

void nova_gc_full_collect(NovaVM *vm) {
    if (vm == NULL) {
        return;
    }

    uint8_t saved_running = vm->gc_running;
    vm->gc_running = 1;

    NTRACE(GC, "gc_full_collect START allocated=%zu", vm->bytes_allocated);

    /* If in the middle of a cycle, finish it first */
    while (vm->gc_phase != NOVA_GC_PHASE_PAUSE) {
        novai_gc_run_step(vm, SIZE_MAX);
    }

    /* Run a complete cycle: PAUSE -> MARK -> SWEEP -> PAUSE */
    novai_gc_run_step(vm, SIZE_MAX);  /* PAUSE -> MARK (marks roots) */

    while (vm->gc_phase == NOVA_GC_PHASE_MARK) {
        novai_gc_run_step(vm, SIZE_MAX);
    }

    while (vm->gc_phase == NOVA_GC_PHASE_SWEEP) {
        novai_gc_run_step(vm, SIZE_MAX);
    }

    NTRACE(GC, "gc_full_collect END allocated=%zu", vm->bytes_allocated);

    vm->gc_running = saved_running;
}

void nova_gc_shutdown(NovaVM *vm) {
    if (vm == NULL) {
        return;
    }

    /* Free ALL objects unconditionally */
    NovaGCHeader *obj = vm->all_objects;
    while (obj != NULL) {
        NovaGCHeader *next = obj->gc_next;
        size_t freed = novai_gc_free_object(obj);
        if (vm->bytes_allocated >= freed) {
            vm->bytes_allocated -= freed;
        }
        obj = next;
    }
    vm->all_objects = NULL;
    vm->gray_list = NULL;
}

void nova_gc_barrier(NovaVM *vm, NovaGCHeader *parent) {
    if (vm == NULL || parent == NULL) {
        return;
    }

    /* Write barrier only matters during marking phase.
     * If a black object gains a reference to a white child,
     * we must re-shade the parent gray to re-traverse it. */
    if (vm->gc_phase == NOVA_GC_PHASE_MARK && NOVA_GC_IS_BLACK(parent)) {
        NTRACE(GC, "gc_barrier parent=%p re-grayed", (const void *)parent);
        parent->gc_color = NOVA_GC_GRAY;
        parent->gc_gray = vm->gray_list;
        vm->gray_list = parent;
    }
}
