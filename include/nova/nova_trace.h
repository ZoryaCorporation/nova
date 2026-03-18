/**
 * @file nova_trace.h
 * @brief Nova Language - Developer Debug Trace System
 *
 * Comprehensive internal instrumentation for Nova language
 * development. Provides per-channel trace output to stderr
 * with color coding, call-depth indentation, stack dumps,
 * value formatting, and opcode disassembly.
 *
 * USAGE:
 *   Build with NOVA_TRACE=1:
 *     make DEBUG=1 TRACE=1
 *     -or-
 *     make trace
 *
 *   Select channels at runtime:
 *     NOVA_TRACE=vm,call,stack ./bin/nova script.n
 *     ./bin/nova --trace=vm,call script.n
 *     NOVA_TRACE=all ./bin/nova script.n
 *
 *   In code:
 *     NTRACE(VM, "GETTABLE A=%d B=%d C=%d", a, b, c);
 *     NTRACE_STACK(vm, "after CALL");
 *     NTRACE_VALUE("result", val);
 *     NTRACE_ENTER("nova_base_require");
 *     NTRACE_LEAVE();
 *
 * CHANNELS:
 *   VM       Opcode execution, register reads/writes
 *   STACK    Stack push/pop/top changes, cfunc_base
 *   CALL     Function entry/exit, args, return values
 *   GC       Allocation, collection, sweep, barrier
 *   PP       Preprocessor token emission, directive handling
 *   COMPILE  Bytecode generation, register allocation
 *   MODULE   require() search, load, cache, path resolution
 *   LEX      Lexer token scanning
 *   META     Metamethod lookup and invocation
 *   PARSE    Parser state transitions
 *   MEM      malloc/realloc/free tracking
 *   ALL      Enable every channel
 *
 * When NOVA_TRACE is not defined, ALL macros compile to nothing
 * (zero overhead in release builds).
 *
 * @author Anthony Taliento
 * @date 2026-02-13
 * @version 1.0.0
 *
 * @copyright Copyright (c) 2026 Zorya Corporation
 * @license MIT
 *
 * ZORYA-C COMPLIANCE: v2.0.0
 */

#ifndef NOVA_TRACE_H
#define NOVA_TRACE_H

#include <stdint.h>
#include <stddef.h>

/* Forward declarations to avoid circular includes */
struct NovaVM;

/* ============================================================
 * TRACE CHANNELS (bitmask)
 *
 * Each channel is a power-of-two bit in a 32-bit mask.
 * Multiple channels can be enabled simultaneously.
 * ============================================================ */

#define NOVA_TRACE_CH_VM       ((uint32_t)0x0001)  /**< Opcode execution      */
#define NOVA_TRACE_CH_STACK    ((uint32_t)0x0002)  /**< Stack operations       */
#define NOVA_TRACE_CH_CALL     ((uint32_t)0x0004)  /**< Function entry/exit    */
#define NOVA_TRACE_CH_GC       ((uint32_t)0x0008)  /**< Garbage collection     */
#define NOVA_TRACE_CH_PP       ((uint32_t)0x0010)  /**< Preprocessor           */
#define NOVA_TRACE_CH_COMPILE  ((uint32_t)0x0020)  /**< Compiler/codegen       */
#define NOVA_TRACE_CH_MODULE   ((uint32_t)0x0040)  /**< Module loading         */
#define NOVA_TRACE_CH_LEX      ((uint32_t)0x0080)  /**< Lexer                  */
#define NOVA_TRACE_CH_META     ((uint32_t)0x0100)  /**< Metamethods            */
#define NOVA_TRACE_CH_PARSE    ((uint32_t)0x0200)  /**< Parser                 */
#define NOVA_TRACE_CH_MEM      ((uint32_t)0x0400)  /**< Memory allocations     */
#define NOVA_TRACE_CH_ALL      ((uint32_t)0xFFFF)  /**< All channels           */

/* ============================================================
 * TRACE MACROS
 *
 * When NOVA_TRACE is defined, these expand to actual calls.
 * When not defined, they compile to nothing (zero overhead).
 * ============================================================ */

#ifdef NOVA_TRACE

/**
 * @brief Emit a trace message on a specific channel.
 *
 * Usage: NTRACE(VM, "MOVE A=%d B=%d", a, b);
 *        NTRACE(CALL, "ENTER %s nargs=%d", name, nargs);
 */
#define NTRACE(channel, fmt, ...) \
    do { \
        if (nova_trace_enabled(NOVA_TRACE_CH_##channel)) { \
            nova_trace_emit(NOVA_TRACE_CH_##channel, \
                            __FILE__, __LINE__, \
                            fmt, ##__VA_ARGS__); \
        } \
    } while (0)

/**
 * @brief Push a named scope for indentation tracking.
 *
 * Usage: NTRACE_ENTER("nova_base_require");
 */
#define NTRACE_ENTER(name) nova_trace_indent_push(name)

/**
 * @brief Pop a scope level.
 */
#define NTRACE_LEAVE() nova_trace_indent_pop()

/**
 * @brief Dump the VM stack to trace output.
 *
 * Usage: NTRACE_STACK(vm, "after CALL");
 */
#define NTRACE_STACK(vm, label) \
    do { \
        if (nova_trace_enabled(NOVA_TRACE_CH_STACK)) { \
            nova_trace_dump_stack((vm), (label)); \
        } \
    } while (0)

/**
 * @brief Format and trace a single NovaValue.
 *
 * Usage: NTRACE_VALUE("result", val);
 */
#define NTRACE_VALUE(label, val) \
    do { \
        if (nova_trace_enabled(NOVA_TRACE_CH_VM | NOVA_TRACE_CH_STACK)) { \
            nova_trace_dump_value((label), &(val)); \
        } \
    } while (0)

/**
 * @brief Trace a register window (base[0..n]).
 *
 * Usage: NTRACE_REGS(vm, base, max_stack, "frame 2");
 */
#define NTRACE_REGS(vm, base_ptr, count, label) \
    do { \
        if (nova_trace_enabled(NOVA_TRACE_CH_VM)) { \
            nova_trace_dump_regs((vm), (base_ptr), (count), (label)); \
        } \
    } while (0)

/**
 * @brief Disassemble a single instruction to trace output.
 *
 * Usage: NTRACE_INSTR(ip, K, proto);
 */
#define NTRACE_INSTR(ip_ptr, constants, proto_ptr) \
    do { \
        if (nova_trace_enabled(NOVA_TRACE_CH_VM)) { \
            nova_trace_disasm_instr(*(ip_ptr), (constants), (proto_ptr)); \
        } \
    } while (0)

/**
 * @brief Trace a GC event.
 *
 * Usage: NTRACE_GC("mark", obj, size);
 */
#define NTRACE_GC_EVENT(event, ptr, size) \
    do { \
        if (nova_trace_enabled(NOVA_TRACE_CH_GC)) { \
            nova_trace_gc_event((event), (ptr), (size)); \
        } \
    } while (0)

/**
 * @brief Trace a memory operation.
 */
#define NTRACE_MEM(op, ptr, size) \
    do { \
        if (nova_trace_enabled(NOVA_TRACE_CH_MEM)) { \
            nova_trace_mem_op((op), (ptr), (size)); \
        } \
    } while (0)

/**
 * @brief Conditional trace (for very hot paths).
 * Only evaluates the expression block if channel is enabled.
 */
#define NTRACE_IF(channel) \
    if (nova_trace_enabled(NOVA_TRACE_CH_##channel))

#else /* !NOVA_TRACE */

#define NTRACE(channel, fmt, ...)             ((void)0)
#define NTRACE_ENTER(name)                    ((void)0)
#define NTRACE_LEAVE()                        ((void)0)
#define NTRACE_STACK(vm, label)               ((void)0)
#define NTRACE_VALUE(label, val)              ((void)0)
#define NTRACE_REGS(vm, base_ptr, count, label) ((void)0)
#define NTRACE_INSTR(ip_ptr, constants, proto_ptr) ((void)0)
#define NTRACE_GC_EVENT(event, ptr, size)     ((void)0)
#define NTRACE_MEM(op, ptr, size)             ((void)0)
#define NTRACE_IF(channel)                    if (0)

#endif /* NOVA_TRACE */

/* ============================================================
 * TRACE API (implementation in nova_trace.c)
 *
 * These functions are only compiled when NOVA_TRACE is set.
 * They are NOT meant to be called directly -- use the macros.
 * ============================================================ */

#ifdef NOVA_TRACE

/* Forward declarations for value types */
#include "nova_vm.h"
#include "nova_proto.h"

/**
 * @brief Initialize the trace system.
 *
 * Reads NOVA_TRACE env var and sets channel mask.
 * Called once at startup (from nova.c main or nova_vm_create).
 *
 * @param channels  Channel string ("vm,call,stack" or "all")
 *                  NULL = read from NOVA_TRACE env var.
 */
void nova_trace_init(const char *channels);

/**
 * @brief Check if a channel is currently enabled.
 */
int nova_trace_enabled(uint32_t channel_mask);

/**
 * @brief Set the active channel mask directly.
 */
void nova_trace_set_channels(uint32_t mask);

/**
 * @brief Get the current channel mask.
 */
uint32_t nova_trace_get_channels(void);

/**
 * @brief Emit a formatted trace message.
 *
 * @param channel  Channel bitmask (for color selection)
 * @param file     Source file (__FILE__)
 * @param line     Source line (__LINE__)
 * @param fmt      printf-style format string
 */
void nova_trace_emit(uint32_t channel, const char *file,
                     int line, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));

/**
 * @brief Push a named scope (increases indentation).
 */
void nova_trace_indent_push(const char *name);

/**
 * @brief Pop a scope (decreases indentation).
 */
void nova_trace_indent_pop(void);

/**
 * @brief Dump the entire VM stack to trace output.
 *
 * @param vm     VM instance
 * @param label  Descriptive label for context
 */
void nova_trace_dump_stack(const struct NovaVM *vm, const char *label);

/**
 * @brief Format a single value to trace output.
 *
 * @param label  Name/description
 * @param val    Pointer to the value
 */
void nova_trace_dump_value(const char *label, const NovaValue *val);

/**yy
 * @brief Dump a register window.
 *
 * @param vm     VM instance (for type resolution)
 * @param base   Start of register window
 * @param count  Number of registers to dump
 * @param label  Descriptive label
 */
void nova_trace_dump_regs(const struct NovaVM *vm,
                          const NovaValue *base,
                          int count, const char *label);

/**
 * @brief Disassemble one instruction to trace output.
 *
 * @param instr      The instruction word
 * @param constants  Constant pool (for K references)
 * @param proto      Prototype (for sub-proto references)
 */
void nova_trace_disasm_instr(uint32_t instr,
                             const NovaConstant *constants,
                             const NovaProto *proto);

/**
 * @brief Trace a GC event (mark, sweep, free, etc.).
 */
void nova_trace_gc_event(const char *event, const void *ptr,
                         size_t size);

/**
 * @brief Trace a memory operation (alloc, realloc, free).
 */
void nova_trace_mem_op(const char *op, const void *ptr,
                       size_t size);

/**
 * @brief Format a NovaValue into a string buffer.
 *
 * Utility function for building trace messages.
 *
 * @param buf    Output buffer
 * @param size   Buffer size
 * @param val    Value to format
 * @return Number of chars written (excluding NUL)
 */
int nova_trace_fmt_value(char *buf, size_t size,
                         const NovaValue *val);

/**
 * @brief Format a constant into a string buffer.
 */
int nova_trace_fmt_const(char *buf, size_t size,
                         const NovaConstant *k);

#endif /* NOVA_TRACE */

#endif /* NOVA_TRACE_H */
