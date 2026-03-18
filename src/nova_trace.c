/**
 * @file nova_trace.c
 * @brief Nova Language - Developer Debug Trace System Implementation
 *
 * Provides per-channel color-coded trace output to stderr with
 * call-depth indentation, stack dumps, register window dumps,
 * value formatting, single-instruction disassembly, and GC/memory
 * event tracing.
 *
 * This file is ONLY compiled when NOVA_TRACE is defined.
 * See nova_trace.h for usage documentation.
 *
 * @author Anthony Taliento
 * @date 2026-02-13
 * @version 1.0.0
 *
 * @copyright Copyright (c) 2026 Zorya Corporation
 * @license MIT
 *
 * DEPENDENCIES:
 *   - nova_trace.h (macro definitions, API declarations)
 *   - nova_vm.h (NovaVM, NovaValue, NovaValueType)
 *   - nova_proto.h (NovaProto, NovaConstant, NovaConstTag)
 *   - nova_opcode.h (NovaOpcode, NovaOpcodeInfo, nova_opcode_info)
 *
 * THREAD SAFETY:
 *   Not thread-safe - uses file-scope static state.
 *   Each thread should have its own VM; trace is for single-threaded
 *   developer debugging only.
 *
 * ZORYA-C COMPLIANCE: v2.0.0
 */

#ifdef NOVA_TRACE

#include "nova/nova_trace.h"
#include "nova/nova_vm.h"
#include "nova/nova_proto.h"
#include "nova/nova_opcode.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <ctype.h>
#include <inttypes.h>

/* ============================================================
 * GLOBAL STATE
 * ============================================================ */

/** Active channel bitmask (0 = all channels off) */
static uint32_t g_trace_mask = 0;

/** Indentation depth (nesting level) */
static int g_trace_depth = 0;

/** Maximum indentation depth to prevent runaway output */
#define NOVA_TRACE_MAX_DEPTH 64

/** Whether nova_trace_init() has been called */
static int g_trace_initialized = 0;

/* ============================================================
 * ANSI COLOR CODES
 *
 * Each channel gets a unique color for easy visual scanning
 * of interleaved trace output.
 * ============================================================ */

#define TRACE_RESET   "\033[0m"
#define TRACE_BOLD    "\033[1m"
#define TRACE_DIM     "\033[2m"

/* Channel colors */
#define TRACE_COLOR_VM       "\033[36m"     /* Cyan        */
#define TRACE_COLOR_STACK    "\033[33m"     /* Yellow      */
#define TRACE_COLOR_CALL     "\033[32m"     /* Green       */
#define TRACE_COLOR_GC       "\033[31m"     /* Red         */
#define TRACE_COLOR_PP       "\033[35m"     /* Magenta     */
#define TRACE_COLOR_COMPILE  "\033[34m"     /* Blue        */
#define TRACE_COLOR_MODULE   "\033[37m"     /* White       */
#define TRACE_COLOR_LEX      "\033[96m"     /* Bright Cyan */
#define TRACE_COLOR_META     "\033[95m"     /* Bright Magenta */
#define TRACE_COLOR_PARSE    "\033[92m"     /* Bright Green */
#define TRACE_COLOR_MEM      "\033[93m"     /* Bright Yellow */

/* ============================================================
 * CHANNEL NAME TABLE
 *
 * Maps channel bitmask to human-readable name and color.
 * ============================================================ */

typedef struct {
    const char *name;
    uint32_t    mask;
    const char *color;
} NovaTraceChannelInfo;

static const NovaTraceChannelInfo g_channel_table[] = {
    { "VM",      NOVA_TRACE_CH_VM,      TRACE_COLOR_VM      },
    { "STACK",   NOVA_TRACE_CH_STACK,   TRACE_COLOR_STACK   },
    { "CALL",    NOVA_TRACE_CH_CALL,    TRACE_COLOR_CALL    },
    { "GC",      NOVA_TRACE_CH_GC,      TRACE_COLOR_GC      },
    { "PP",      NOVA_TRACE_CH_PP,      TRACE_COLOR_PP      },
    { "COMPILE", NOVA_TRACE_CH_COMPILE, TRACE_COLOR_COMPILE },
    { "MODULE",  NOVA_TRACE_CH_MODULE,  TRACE_COLOR_MODULE  },
    { "LEX",     NOVA_TRACE_CH_LEX,     TRACE_COLOR_LEX     },
    { "META",    NOVA_TRACE_CH_META,    TRACE_COLOR_META    },
    { "PARSE",   NOVA_TRACE_CH_PARSE,   TRACE_COLOR_PARSE   },
    { "MEM",     NOVA_TRACE_CH_MEM,     TRACE_COLOR_MEM     },
    { NULL,      0,                     NULL                 }
};

/* ============================================================
 * INTERNAL HELPERS
 * ============================================================ */

/**
 * @brief Get the color string for a channel bitmask.
 *
 * If multiple channels are set, returns the first match.
 */
static const char *novai_trace_channel_color(uint32_t channel) {
    const NovaTraceChannelInfo *info = g_channel_table;
    while (info->name != NULL) {
        if ((channel & info->mask) != 0) {
            return info->color;
        }
        info++;
    }
    return TRACE_RESET;
}

/**
 * @brief Get the tag string for a channel bitmask.
 */
static const char *novai_trace_channel_tag(uint32_t channel) {
    const NovaTraceChannelInfo *info = g_channel_table;
    while (info->name != NULL) {
        if ((channel & info->mask) != 0) {
            return info->name;
        }
        info++;
    }
    return "???";
}

/**
 * @brief Strip directory prefix from __FILE__ path.
 */
static const char *novai_trace_basename(const char *path) {
    if (path == NULL) {
        return "<null>";
    }
    const char *last = strrchr(path, '/');
    if (last != NULL) {
        return last + 1;
    }
    return path;
}

/**
 * @brief Parse a channel name to its bitmask.
 *
 * Case-insensitive. Returns 0 if unknown.
 */
static uint32_t novai_trace_parse_channel_name(const char *name,
                                                size_t len) {
    if (len == 0) {
        return 0;
    }

    /* Check "all" first */
    if (len == 3) {
        char buf[4] = {0};
        for (size_t i = 0; i < 3; i++) {
            buf[i] = (char)tolower((unsigned char)name[i]);
        }
        if (strcmp(buf, "all") == 0) {
            return NOVA_TRACE_CH_ALL;
        }
    }

    /* Search channel table */
    const NovaTraceChannelInfo *info = g_channel_table;
    while (info->name != NULL) {
        size_t tag_len = strlen(info->name);
        if (tag_len == len) {
            int match = 1;
            for (size_t i = 0; i < len; i++) {
                if (tolower((unsigned char)name[i]) !=
                    tolower((unsigned char)info->name[i])) {
                    match = 0;
                    break;
                }
            }
            if (match) {
                return info->mask;
            }
        }
        info++;
    }

    return 0;
}

/**
 * @brief Parse a comma-separated channel string into a bitmask.
 *
 * Example: "vm,call,stack" -> CH_VM | CH_CALL | CH_STACK
 *          "all"           -> CH_ALL
 */
static uint32_t novai_trace_parse_channels(const char *str) {
    if (str == NULL || str[0] == '\0') {
        return 0;
    }

    uint32_t mask = 0;
    const char *p = str;

    while (*p != '\0') {
        /* Skip whitespace and commas */
        while (*p == ',' || *p == ' ' || *p == '\t') {
            p++;
        }
        if (*p == '\0') {
            break;
        }

        /* Find end of channel name */
        const char *start = p;
        while (*p != '\0' && *p != ',' && *p != ' ' && *p != '\t') {
            p++;
        }

        size_t len = (size_t)(p - start);
        uint32_t ch = novai_trace_parse_channel_name(start, len);
        if (ch == 0) {
            fprintf(stderr, "[TRACE] WARNING: unknown channel '");
            fwrite(start, 1, len, stderr);
            fprintf(stderr, "' (ignored)\n");
        }
        mask |= ch;
    }

    return mask;
}

/**
 * @brief Print indentation (2 spaces per depth level).
 */
static void novai_trace_print_indent(FILE *fp) {
    int depth = g_trace_depth;
    if (depth > NOVA_TRACE_MAX_DEPTH) {
        depth = NOVA_TRACE_MAX_DEPTH;
    }
    for (int i = 0; i < depth; i++) {
        fprintf(fp, "  ");
    }
}

/* ============================================================
 * PUBLIC API IMPLEMENTATION
 * ============================================================ */

/**
 * @brief Initialize the trace system.
 *
 * Reads the NOVA_TRACE environment variable if channels is NULL.
 * Can be called multiple times (reinitializes).
 *
 * @param channels  Channel string or NULL (uses env var).
 */
void nova_trace_init(const char *channels) {
    g_trace_depth = 0;
    g_trace_initialized = 1;

    if (channels != NULL) {
        g_trace_mask = novai_trace_parse_channels(channels);
    } else {
        const char *env = getenv("NOVA_TRACE");
        if (env != NULL && env[0] != '\0') {
            g_trace_mask = novai_trace_parse_channels(env);
        } else {
            g_trace_mask = 0;
        }
    }

    if (g_trace_mask != 0) {
        fprintf(stderr,
                TRACE_BOLD "[TRACE] Trace system initialized. "
                "Channels: 0x%04X" TRACE_RESET "\n",
                g_trace_mask);

        /* List enabled channels */
        fprintf(stderr, TRACE_DIM "[TRACE] Active:");
        const NovaTraceChannelInfo *info = g_channel_table;
        while (info->name != NULL) {
            if ((g_trace_mask & info->mask) != 0) {
                fprintf(stderr, " %s%s" TRACE_RESET,
                        info->color, info->name);
            }
            info++;
        }
        fprintf(stderr, TRACE_RESET "\n");
    }
}

/**
 * @brief Check if a channel is currently enabled.
 */
int nova_trace_enabled(uint32_t channel_mask) {
    return (g_trace_mask & channel_mask) != 0;
}

/**
 * @brief Set the active channel mask directly.
 */
void nova_trace_set_channels(uint32_t mask) {
    g_trace_mask = mask;
}

/**
 * @brief Get the current channel mask.
 */
uint32_t nova_trace_get_channels(void) {
    return g_trace_mask;
}

/**
 * @brief Emit a formatted trace message.
 *
 * Format: [TAG] file:line  indent  message
 * Color-coded by channel.
 */
void nova_trace_emit(uint32_t channel, const char *file,
                     int line, const char *fmt, ...) {
    if ((g_trace_mask & channel) == 0) {
        return;
    }

    const char *color = novai_trace_channel_color(channel);
    const char *tag   = novai_trace_channel_tag(channel);
    const char *base  = novai_trace_basename(file);

    /* Print channel tag and source location */
    fprintf(stderr, "%s[%-7s]" TRACE_RESET " " TRACE_DIM "%s:%d"
            TRACE_RESET " ",
            color, tag, base, line);

    /* Print indentation */
    novai_trace_print_indent(stderr);

    /* Print the formatted message */
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);

    fprintf(stderr, "\n");
}

/**
 * @brief Push a named scope (increases indentation).
 */
void nova_trace_indent_push(const char *name) {
    if (g_trace_mask != 0 && name != NULL) {
        fprintf(stderr, TRACE_COLOR_CALL "[CALL   ]" TRACE_RESET " ");
        novai_trace_print_indent(stderr);
        fprintf(stderr, TRACE_BOLD ">>> %s" TRACE_RESET "\n", name);
    }
    if (g_trace_depth < NOVA_TRACE_MAX_DEPTH) {
        g_trace_depth++;
    }
}

/**
 * @brief Pop a scope (decreases indentation).
 */
void nova_trace_indent_pop(void) {
    if (g_trace_depth > 0) {
        g_trace_depth--;
    }
    if (g_trace_mask != 0) {
        fprintf(stderr, TRACE_COLOR_CALL "[CALL   ]" TRACE_RESET " ");
        novai_trace_print_indent(stderr);
        fprintf(stderr, TRACE_DIM "<<<" TRACE_RESET "\n");
    }
}

/* ============================================================
 * VALUE FORMATTING
 * ============================================================ */

/**
 * @brief Format a NovaValue into a buffer for trace output.
 *
 * Produces a human-readable representation like:
 *   nil, true, false, 42 (int), 3.14 (num), "hello" (str:5),
 *   table:0x1234, function:0x5678, cfunction:0x9abc
 *
 * @return Number of chars written (excluding NUL).
 */
int nova_trace_fmt_value(char *buf, size_t size,
                         const NovaValue *val) {
    if (val == NULL) {
        return snprintf(buf, size, "<null-val>");
    }

    NovaValue v = *val;
    NovaValueType vtype = nova_typeof(v);

    switch (vtype) {
        case NOVA_TYPE_NIL:
            return snprintf(buf, size, "nil");

        case NOVA_TYPE_BOOL:
            return snprintf(buf, size, "%s",
                            nova_as_bool(v) ? "true" : "false");

        case NOVA_TYPE_INTEGER:
            return snprintf(buf, size, "%" PRId64 " (int)",
                            nova_as_integer(v));

        case NOVA_TYPE_NUMBER:
            return snprintf(buf, size, "%.14g (num)",
                            nova_as_number(v));

        case NOVA_TYPE_STRING: {
            NovaString *s = nova_as_string(v);
            if (s != NULL) {
                /* Truncate long strings in trace output */
                size_t slen = nova_str_len(s);
                if (slen > 40) {
                    return snprintf(buf, size,
                                    "\"%.40s...\" (str:%zu)",
                                    nova_str_data(s), slen);
                }
                return snprintf(buf, size, "\"%s\" (str:%zu)",
                                nova_str_data(s), slen);
            }
            return snprintf(buf, size, "<null-string>");
        }

        case NOVA_TYPE_TABLE:
            return snprintf(buf, size, "table:%p",
                            (const void *)nova_as_table(v));

        case NOVA_TYPE_FUNCTION: {
            NovaClosure *cl = nova_as_closure(v);
            if (cl != NULL &&
                cl->proto != NULL &&
                cl->proto->source != NULL) {
                return snprintf(buf, size, "function:%p <%s>",
                                (const void *)cl,
                                cl->proto->source);
            }
            return snprintf(buf, size, "function:%p",
                            (const void *)cl);
        }

        case NOVA_TYPE_CFUNCTION:
            return snprintf(buf, size, "cfunction:%p",
                            (const void *)(uintptr_t)nova_as_cfunction(v));

        case NOVA_TYPE_USERDATA:
            return snprintf(buf, size, "userdata:%p",
                            nova_as_userdata(v));

        case NOVA_TYPE_THREAD:
            return snprintf(buf, size, "coroutine:%p",
                            (const void *)nova_as_coroutine(v));

        default:
            return snprintf(buf, size, "<type=%d>", vtype);
    }
}

/**
 * @brief Format a compile-time constant into a buffer.
 */
int nova_trace_fmt_const(char *buf, size_t size,
                         const NovaConstant *k) {
    if (k == NULL) {
        return snprintf(buf, size, "<null-const>");
    }

    switch (k->tag) {
        case NOVA_CONST_NIL:
            return snprintf(buf, size, "nil");

        case NOVA_CONST_BOOL:
            return snprintf(buf, size, "%s",
                            k->as.boolean ? "true" : "false");

        case NOVA_CONST_INTEGER:
            return snprintf(buf, size, "%" PRId64,
                            k->as.integer);

        case NOVA_CONST_NUMBER:
            return snprintf(buf, size, "%.14g",
                            k->as.number);

        case NOVA_CONST_STRING:
            if (k->as.string.data != NULL) {
                uint32_t slen = k->as.string.length;
                if (slen > 40) {
                    return snprintf(buf, size,
                                    "\"%.40s...\" (len:%u)",
                                    k->as.string.data, slen);
                }
                return snprintf(buf, size, "\"%.*s\"",
                                (int)slen, k->as.string.data);
            }
            return snprintf(buf, size, "<null-string>");

        default:
            return snprintf(buf, size, "<tag=%d>", k->tag);
    }
}

/* ============================================================
 * STACK AND REGISTER DUMPS
 * ============================================================ */

/**
 * @brief Dump the entire VM stack to trace output.
 *
 * Prints each slot from stack[0] to stack_top-1 with its
 * index, formatted value, and a marker for frame bases.
 */
void nova_trace_dump_stack(const struct NovaVM *vm,
                           const char *label) {
    if (vm == NULL) {
        fprintf(stderr, TRACE_COLOR_STACK "[STACK  ]" TRACE_RESET
                " <null-vm>\n");
        return;
    }

    ptrdiff_t depth = vm->stack_top - vm->stack;

    fprintf(stderr, TRACE_COLOR_STACK "[STACK  ]" TRACE_RESET " ");
    novai_trace_print_indent(stderr);
    fprintf(stderr, TRACE_BOLD "--- Stack Dump: %s "
            "(depth=%td, top=%p, base=%p) ---" TRACE_RESET "\n",
            label != NULL ? label : "",
            depth,
            (const void *)vm->stack_top,
            (const void *)vm->stack);

    if (depth <= 0) {
        fprintf(stderr, TRACE_COLOR_STACK "[STACK  ]" TRACE_RESET " ");
        novai_trace_print_indent(stderr);
        fprintf(stderr, "  (empty)\n");
        return;
    }

    /* Cap output for huge stacks */
    ptrdiff_t max_show = depth;
    int truncated = 0;
    if (max_show > 64) {
        max_show = 64;
        truncated = 1;
    }

    char vbuf[256] = {0};

    for (ptrdiff_t i = 0; i < max_show; i++) {
        nova_trace_fmt_value(vbuf, sizeof(vbuf), &vm->stack[i]);

        /* Check if this slot is a frame base */
        int is_base = 0;
        for (int f = 0; f < vm->frame_count; f++) {
            if (vm->frames[f].base == &vm->stack[i]) {
                is_base = 1;
                break;
            }
        }

        fprintf(stderr, TRACE_COLOR_STACK "[STACK  ]" TRACE_RESET " ");
        novai_trace_print_indent(stderr);
        fprintf(stderr, "  [%3td] %s%s" TRACE_RESET "%s\n",
                i,
                is_base ? TRACE_BOLD : "",
                vbuf,
                is_base ? " <<< FRAME BASE" : "");
    }

    if (truncated) {
        fprintf(stderr, TRACE_COLOR_STACK "[STACK  ]" TRACE_RESET " ");
        novai_trace_print_indent(stderr);
        fprintf(stderr, "  ... (%td more slots)\n",
                depth - max_show);
    }

    fprintf(stderr, TRACE_COLOR_STACK "[STACK  ]" TRACE_RESET " ");
    novai_trace_print_indent(stderr);
    fprintf(stderr, TRACE_BOLD "--- End Stack ---" TRACE_RESET "\n");
}

/**
 * @brief Format and trace a single value.
 */
void nova_trace_dump_value(const char *label,
                           const NovaValue *val) {
    char vbuf[256] = {0};
    nova_trace_fmt_value(vbuf, sizeof(vbuf), val);

    fprintf(stderr, TRACE_COLOR_VM "[VM     ]" TRACE_RESET " ");
    novai_trace_print_indent(stderr);
    fprintf(stderr, "%s = %s\n",
            label != NULL ? label : "?", vbuf);
}

/**
 * @brief Dump a register window (base[0..count-1]).
 */
void nova_trace_dump_regs(const struct NovaVM *vm,
                          const NovaValue *base,
                          int count, const char *label) {
    (void)vm;  /* Reserved for future type resolution */

    fprintf(stderr, TRACE_COLOR_VM "[VM     ]" TRACE_RESET " ");
    novai_trace_print_indent(stderr);
    fprintf(stderr, TRACE_BOLD "--- Registers: %s "
            "(count=%d, base=%p) ---" TRACE_RESET "\n",
            label != NULL ? label : "",
            count, (const void *)base);

    if (base == NULL || count <= 0) {
        fprintf(stderr, TRACE_COLOR_VM "[VM     ]" TRACE_RESET " ");
        novai_trace_print_indent(stderr);
        fprintf(stderr, "  (empty)\n");
        return;
    }

    /* Cap output */
    int max_show = count;
    if (max_show > 32) {
        max_show = 32;
    }

    char vbuf[256] = {0};

    for (int i = 0; i < max_show; i++) {
        nova_trace_fmt_value(vbuf, sizeof(vbuf), &base[i]);
        fprintf(stderr, TRACE_COLOR_VM "[VM     ]" TRACE_RESET " ");
        novai_trace_print_indent(stderr);
        fprintf(stderr, "  R(%d) = %s\n", i, vbuf);
    }

    if (count > max_show) {
        fprintf(stderr, TRACE_COLOR_VM "[VM     ]" TRACE_RESET " ");
        novai_trace_print_indent(stderr);
        fprintf(stderr, "  ... (%d more registers)\n",
                count - max_show);
    }
}

/* ============================================================
 * INSTRUCTION DISASSEMBLY
 * ============================================================ */

/**
 * @brief Disassemble a single instruction to trace output.
 *
 * Uses the nova_opcode_info[] table for opcode names and formats.
 * For RK operands, resolves and prints the constant value.
 */
void nova_trace_disasm_instr(uint32_t instr,
                             const NovaConstant *constants,
                             const NovaProto *proto) {
    NovaOpcode op = NOVA_GET_OPCODE(instr);
    uint8_t a = NOVA_GET_A(instr);
    uint8_t b = NOVA_GET_B(instr);
    uint8_t c = NOVA_GET_C(instr);
    uint16_t bx = NOVA_GET_BX(instr);
    int sbx = NOVA_GET_SBX(instr);

    const NovaOpcodeInfo *info = &nova_opcode_info[(uint8_t)op];
    const char *name = info->name;
    if (name == NULL) {
        name = "UNKNOWN";
    }

    fprintf(stderr, TRACE_COLOR_VM "[VM     ]" TRACE_RESET " ");
    novai_trace_print_indent(stderr);

    char kbuf[128] = {0};

    switch (info->format) {
        case NOVA_FMT_ABC:
            fprintf(stderr, "  %-12s A=%d B=%d C=%d",
                    name, a, b, c);

            /* Show RK-resolved values */
            if (info->arg_b == NOVA_ARGMODE_RK &&
                NOVA_IS_RK_CONST(b) && constants != NULL) {
                int ki = NOVA_RK_TO_CONST(b);
                nova_trace_fmt_const(kbuf, sizeof(kbuf),
                                     &constants[ki]);
                fprintf(stderr, "  ; B=K(%d)=%s", ki, kbuf);
            }
            if (info->arg_c == NOVA_ARGMODE_RK &&
                NOVA_IS_RK_CONST(c) && constants != NULL) {
                int ki = NOVA_RK_TO_CONST(c);
                nova_trace_fmt_const(kbuf, sizeof(kbuf),
                                     &constants[ki]);
                fprintf(stderr, "  ; C=K(%d)=%s", ki, kbuf);
            }
            /* Show constant for CONST-mode fields */
            if (info->arg_c == NOVA_ARGMODE_CONST &&
                constants != NULL) {
                nova_trace_fmt_const(kbuf, sizeof(kbuf),
                                     &constants[c]);
                fprintf(stderr, "  ; C=K(%d)=%s", c, kbuf);
            }
            break;

        case NOVA_FMT_ABX:
            fprintf(stderr, "  %-12s A=%d Bx=%u", name, a, bx);
            /* Resolve constant for LOADK, GETGLOBAL, etc. */
            if (constants != NULL && bx < 65535u) {
                nova_trace_fmt_const(kbuf, sizeof(kbuf),
                                     &constants[bx]);
                fprintf(stderr, "  ; K(%u)=%s", bx, kbuf);
            }
            break;

        case NOVA_FMT_ASBX:
            fprintf(stderr, "  %-12s A=%d sBx=%d", name, a, sbx);
            break;

        case NOVA_FMT_AX: {
            uint32_t ax = NOVA_GET_AX(instr);
            fprintf(stderr, "  %-12s Ax=%u", name, ax);
            break;
        }

        case NOVA_FMT_NONE:
            fprintf(stderr, "  %-12s", name);
            break;

        default:
            fprintf(stderr, "  %-12s [0x%08X]", name, instr);
            break;
    }

    /* Show sub-proto info for CLOSURE */
    if (op == NOVA_OP_CLOSURE && proto != NULL &&
        bx < (uint16_t)proto->proto_count) {
        const NovaProto *child = proto->protos[bx];
        if (child != NULL && child->source != NULL) {
            fprintf(stderr, "  ; proto <%s>", child->source);
        }
    }

    fprintf(stderr, "\n");
}

/* ============================================================
 * GC AND MEMORY TRACING
 * ============================================================ */

/**
 * @brief Trace a GC event (mark, sweep, free, barrier, etc.).
 */
void nova_trace_gc_event(const char *event, const void *ptr,
                         size_t size) {
    fprintf(stderr, TRACE_COLOR_GC "[GC     ]" TRACE_RESET " ");
    novai_trace_print_indent(stderr);
    fprintf(stderr, "%s ptr=%p size=%zu\n",
            event != NULL ? event : "?",
            ptr, size);
}

/**
 * @brief Trace a memory operation (alloc, realloc, free).
 */
void nova_trace_mem_op(const char *op, const void *ptr,
                       size_t size) {
    fprintf(stderr, TRACE_COLOR_MEM "[MEM    ]" TRACE_RESET " ");
    novai_trace_print_indent(stderr);
    fprintf(stderr, "%-8s ptr=%p size=%zu\n",
            op != NULL ? op : "?",
            ptr, size);
}

#endif /* NOVA_TRACE */

/* ISO C forbids empty translation units.
 * Provide a dummy symbol when NOVA_TRACE is not defined. */
#ifndef NOVA_TRACE
typedef int novai_trace_dummy_t;
#endif
