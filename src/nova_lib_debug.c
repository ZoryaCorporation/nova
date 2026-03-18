/**
 * @file nova_lib_debug.c
 * @brief Nova Language - Debug Standard Library
 *
 * Provides introspection and debugging facilities:
 *   debug.traceback([msg [, level]])   - Stack traceback string
 *   debug.getinfo(f [, what])          - Function/frame introspection
 *   debug.getlocal(level, index)       - Read local variable
 *   debug.sethook(hook, mask [, count])- Set debug hook (stub)
 *
 * @author Anthony Taliento
 * @date 2026-02-15
 * @version 1.0.0
 *
 * @copyright Copyright (c) 2026 Zorya Corporation
 * @license MIT
 *
 * DEPENDENCIES:
 *   - nova_vm.h, nova_lib.h
 *
 * THREAD SAFETY:
 *   Not thread-safe. Each thread should use a separate VM.
 *
 * ZORYA-C COMPLIANCE: v2.0.0
 */

#include "nova/nova_lib.h"
#include "nova/nova_vm.h"
#include "nova/nova_proto.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ============================================================
 * DEBUG.TRACEBACK
 * ============================================================ */

/**
 * @brief debug.traceback([message [, level]]) - Get a stack traceback.
 *
 * Returns a string describing the call stack from the given level
 * upward.  If `message` is provided and is a string, it is prepended
 * to the traceback.  `level` defaults to 1 (the function calling
 * traceback).
 *
 * @param vm  VM instance (must not be NULL)
 * @return 1 result: string
 *
 * @pre vm != NULL
 *
 * COMPLEXITY: O(n) where n is call depth
 * THREAD SAFETY: Not thread-safe
 *
 * @example
 *   print(debug.traceback("error here", 2))
 */
static int nova_debug_traceback(NovaVM *vm) {
    /* Optional first arg: message string */
    const char *msg = NULL;
    int level = 1;

    int nargs = nova_vm_get_top(vm);

    if (nargs >= 1) {
        NovaValue v = nova_vm_get(vm, 0);
        if (nova_is_string(v)) {
            msg = nova_str_data(nova_as_string(v));
        }
    }

    if (nargs >= 2) {
        NovaValue v = nova_vm_get(vm, 1);
        if (nova_is_integer(v)) {
            level = (int)nova_as_integer(v);
        } else if (nova_is_number(v)) {
            level = (int)nova_as_number(v);
        }
    }

    if (level < 0) {
        level = 0;
    }

    char *tb = nova_vm_traceback(vm, msg, level);
    if (tb == NULL) {
        nova_vm_push_string(vm, "stack traceback:\n\t(unavailable)", 32);
        return 1;
    }

    nova_vm_push_string(vm, tb, strlen(tb));
    free(tb);
    return 1;
}

/* ============================================================
 * DEBUG.GETINFO
 * ============================================================ */

/**
 * @brief debug.getinfo(f [, what]) - Get info about a function or
 *        stack level.
 *
 * If `f` is a number, returns info about the function at that stack
 * level (1 = direct caller, 2 = caller's caller, etc.).
 * If `f` is a function, returns info about that function.
 *
 * Returns a table with fields depending on `what` string:
 *   "n" - name, namewhat
 *   "S" - source, short_src, what, linedefined, lastlinedefined
 *   "l" - currentline
 *   "u" - nups (number of upvalues), nparams, isvararg
 *
 * Default `what` = "Slnu"
 *
 * @param vm  VM instance (must not be NULL)
 * @return 1 result: table (or nil if invalid level)
 *
 * @pre vm != NULL
 *
 * COMPLEXITY: O(1)
 * THREAD SAFETY: Not thread-safe
 *
 * @example
 *   local info = debug.getinfo(1, "Sl")
 *   print(info.source, info.currentline)
 */
static int nova_debug_getinfo(NovaVM *vm) {
    if (nova_lib_check_args(vm, 1) != 0) {
        return -1;
    }

    NovaValue arg = nova_vm_get(vm, 0);
    const char *what = "Slnu";
    int nargs = nova_vm_get_top(vm);

    if (nargs >= 2) {
        NovaValue w = nova_vm_get(vm, 1);
        if (nova_is_string(w)) {
            what = nova_str_data(nova_as_string(w));
        }
    }

    const NovaProto *proto = NULL;
    const NovaCallFrame *target_frame = NULL;
    int frame_idx = -1;

    if (nova_is_integer(arg) || nova_is_number(arg)) {
        /* Stack level */
        int level = 0;
        if (nova_is_integer(arg)) {
            level = (int)nova_as_integer(arg);
        } else {
            level = (int)nova_as_number(arg);
        }

        /* Stack level.  Since cfuncs don't push frames:
         *   level 1 = caller of getinfo = vm->frames[frame_count - 1]
         *   level 2 = caller's caller  = vm->frames[frame_count - 2] */
        frame_idx = vm->frame_count - (int)level;
        if (frame_idx < 0 || frame_idx >= vm->frame_count) {
            nova_vm_push_nil(vm);
            return 1;
        }
        target_frame = &vm->frames[frame_idx];
        proto = target_frame->proto;
    } else if (nova_is_function(arg)) {
        /* Function value */
        NovaClosure *cl = nova_as_closure(arg);
        if (cl != NULL) {
            proto = cl->proto;
        }
    } else {
        nova_vm_push_nil(vm);
        return 1;
    }

    if (proto == NULL) {
        /* C function or invalid — return minimal info */
        nova_vm_push_table(vm);
        int tidx = nova_vm_get_top(vm) - 1;

        nova_vm_push_string(vm, "C", 1);
        nova_vm_set_field(vm, tidx, "what");

        nova_vm_push_string(vm, "[C]", 3);
        nova_vm_set_field(vm, tidx, "source");

        nova_vm_push_string(vm, "[C]", 3);
        nova_vm_set_field(vm, tidx, "short_src");

        nova_vm_push_integer(vm, 0);
        nova_vm_set_field(vm, tidx, "currentline");

        nova_vm_set_top(vm, tidx + 1);
        return 1;
    }

    /* Build the result table */
    nova_vm_push_table(vm);
    int tidx = nova_vm_get_top(vm) - 1;

    /* -- "S" fields: source info -- */
    if (strchr(what, 'S') != NULL) {
        const char *src = proto->source ? proto->source : "?";
        nova_vm_push_string(vm, src, strlen(src));
        nova_vm_set_field(vm, tidx, "source");

        /* short_src: just the filename, truncated to 60 chars */
        const char *short_src = src;
        if (strlen(src) > 60) {
            short_src = src + strlen(src) - 57;
        }
        nova_vm_push_string(vm, short_src, strlen(short_src));
        nova_vm_set_field(vm, tidx, "short_src");

        /* what: "main" for top-level, "Nova" for functions */
        if (frame_idx == 0) {
            nova_vm_push_string(vm, "main", 4);
        } else {
            nova_vm_push_string(vm, "Nova", 4);
        }
        nova_vm_set_field(vm, tidx, "what");

        nova_vm_push_integer(vm, (nova_int_t)proto->line_defined);
        nova_vm_set_field(vm, tidx, "linedefined");

        nova_vm_push_integer(vm, (nova_int_t)proto->last_line);
        nova_vm_set_field(vm, tidx, "lastlinedefined");
    }

    /* -- "l" field: current line -- */
    if (strchr(what, 'l') != NULL) {
        uint32_t line = 0;
        if (target_frame != NULL && target_frame->ip != NULL &&
            proto->lines.line_numbers != NULL) {
            uint32_t pc = (uint32_t)(target_frame->ip - proto->code);
            if (pc < proto->lines.count) {
                line = proto->lines.line_numbers[pc];
            }
        }
        nova_vm_push_integer(vm, (nova_int_t)line);
        nova_vm_set_field(vm, tidx, "currentline");
    }

    /* -- "u" fields: upvalue info -- */
    if (strchr(what, 'u') != NULL) {
        nova_vm_push_integer(vm, (nova_int_t)proto->upvalue_count);
        nova_vm_set_field(vm, tidx, "nups");

        nova_vm_push_integer(vm, (nova_int_t)proto->num_params);
        nova_vm_set_field(vm, tidx, "nparams");

        nova_vm_push_integer(vm, (nova_int_t)proto->is_vararg);
        nova_vm_set_field(vm, tidx, "isvararg");
    }

    /* -- "n" fields: name info (best effort) -- */
    if (strchr(what, 'n') != NULL) {
        /* We don't have function name stored in proto directly.
         * Try to extract from parent frame's local info.
         * For now, use source:line_defined as an identifier.        */
        char name_buf[128];
        const char *src = proto->source ? proto->source : "?";
        int n = snprintf(name_buf, sizeof(name_buf), "%s:%u",
                         src, proto->line_defined);
        if (n < 0) {
            n = 0;
        }
        nova_vm_push_string(vm, name_buf, (size_t)n);
        nova_vm_set_field(vm, tidx, "name");

        nova_vm_push_string(vm, "dec", 3);
        nova_vm_set_field(vm, tidx, "namewhat");
    }

    nova_vm_set_top(vm, tidx + 1);
    return 1;
}

/* ============================================================
 * DEBUG.GETLOCAL
 * ============================================================ */

/**
 * @brief debug.getlocal(level, index) - Get a local variable.
 *
 * Returns the name and value of the local variable at the given
 * index in the function at the given stack level.
 *
 * @param vm  VM instance (must not be NULL)
 * @return 2 results: name (string), value; or nil if invalid
 *
 * @pre vm != NULL
 *
 * COMPLEXITY: O(n) where n is local_count
 * THREAD SAFETY: Not thread-safe
 */
static int nova_debug_getlocal(NovaVM *vm) {
    if (nova_lib_check_args(vm, 2) != 0) {
        return -1;
    }

    nova_int_t level = 0;
    nova_int_t index = 0;
    if (!nova_lib_check_integer(vm, 0, &level)) {
        return -1;
    }
    if (!nova_lib_check_integer(vm, 1, &index)) {
        return -1;
    }

    int frame_idx = vm->frame_count - (int)level;
    if (frame_idx < 0 || frame_idx >= vm->frame_count) {
        nova_vm_push_nil(vm);
        return 1;
    }

    const NovaCallFrame *f = &vm->frames[frame_idx];
    const NovaProto *proto = f->proto;
    if (proto == NULL) {
        nova_vm_push_nil(vm);
        return 1;
    }

    /* Compute current PC for local variable range check */
    uint32_t pc = 0;
    if (f->ip != NULL) {
        pc = (uint32_t)(f->ip - proto->code);
    }

    /* Walk locals to find the one at the given index (1-based) */
    int found_idx = 0;
    for (uint32_t i = 0; i < proto->local_count; i++) {
        const NovaLocalInfo *local = &proto->locals[i];
        if (pc >= local->start_pc && pc <= local->end_pc) {
            found_idx++;
            if (found_idx == (int)index) {
                /* Found it — return name and value */
                const char *name = local->name ? local->name : "?";
                nova_vm_push_string(vm, name, strlen(name));

                /* Read the register value from the frame */
                if (f->base != NULL && local->reg < proto->max_stack) {
                    nova_vm_push_value(vm, f->base[local->reg]);
                } else {
                    nova_vm_push_nil(vm);
                }
                return 2;
            }
        }
    }

    nova_vm_push_nil(vm);
    return 1;
}

/* ============================================================
 * DEBUG.SETHOOK (stub)
 * ============================================================ */

/**
 * @brief debug.sethook([hook, mask [, count]]) - Set a debug hook.
 *
 * Currently a stub — hooks are not yet implemented.
 *
 * @param vm  VM instance (must not be NULL)
 * @return 0 results
 */
static int nova_debug_sethook(NovaVM *vm) {
    (void)vm;
    /* Stub: debug hooks not yet implemented */
    return 0;
}

/* ============================================================
 * DEBUG.GETHOOK (stub)
 * ============================================================ */

/**
 * @brief debug.gethook() - Get the current debug hook.
 *
 * Currently a stub — returns nil, "", 0.
 *
 * @param vm  VM instance (must not be NULL)
 * @return 3 results: nil, "", 0
 */
static int nova_debug_gethook(NovaVM *vm) {
    nova_vm_push_nil(vm);
    nova_vm_push_string(vm, "", 0);
    nova_vm_push_integer(vm, 0);
    return 3;
}

/* ============================================================
 * MODULE REGISTRATION
 * ============================================================ */

static const NovaLibReg nova_debug_lib[] = {
    {"traceback",  nova_debug_traceback},
    {"getinfo",    nova_debug_getinfo},
    {"getlocal",   nova_debug_getlocal},
    {"sethook",    nova_debug_sethook},
    {"gethook",    nova_debug_gethook},
    {NULL,         NULL}
};

/**
 * @brief Open the debug library.
 *
 * Registers the "debug" module table with traceback, getinfo, etc.
 *
 * @param vm  VM instance (must not be NULL)
 * @return 0 on success, -1 on failure
 */
int nova_open_debug(NovaVM *vm) {
    if (vm == NULL) {
        return -1;
    }

    nova_lib_register_module(vm, "debug", nova_debug_lib);
    return 0;
}
