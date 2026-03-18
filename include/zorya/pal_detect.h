/**
 * @file pal_detect.h
 * @brief ZORYA PAL - Platform, Architecture & Compiler Detection
 *
 * Zero-dependency detection header. Normalizes the nightmare of
 * compiler/OS/architecture predefined macros into clean ZORYA_*
 * symbols that the rest of the codebase can rely on.
 *
 * Also provides the ZORYA_DISPATCH macro system for bytecode
 * interpreters — self-contained, platform-agnostic instruction
 * dispatch that selects the fastest available mechanism:
 *
 *   Mode 1: Computed goto  (GCC/Clang labels-as-values extension)
 *   Mode 2: Switch/case    (Universal C99 fallback)
 *
 * @author Anthony Taliento
 * @date 2026-06-28
 * @version 1.0.0
 *
 * @copyright Copyright (c) 2026 Zorya Corporation
 * @license Apache-2.0
 *
 * ZORYA-C COMPLIANCE: v2.0.0 (Strict Mode)
 *
 * ============================================================================
 * USAGE
 * ============================================================================
 *
 *   #include <zorya/pal_detect.h>
 *
 *   #if ZORYA_OS_LINUX
 *       // Linux-specific path
 *   #endif
 *
 *   #if ZORYA_ARCH_ARM64
 *       // ARM64 optimization
 *   #endif
 *
 * ============================================================================
 * DISPATCH MACROS - Bytecode VM Instruction Dispatch
 * ============================================================================
 *
 * The dispatch system encapsulates the entire mechanism in macros.
 * On GCC/Clang, it uses labels-as-values (computed goto) for ~30%
 * faster dispatch. On everything else, it falls back to switch/case.
 *
 * Usage pattern:
 *
 *   // 1. Define the dispatch table (computed goto mode only)
 *   ZORYA_DISPATCH_TABLE_BEGIN
 *       ZORYA_DISPATCH_ENTRY(OP_ADD),
 *       ZORYA_DISPATCH_ENTRY(OP_SUB),
 *       ...
 *   ZORYA_DISPATCH_TABLE_END
 *
 *   // 2. Start the dispatch loop
 *   ZORYA_DISPATCH_START(GET_OPCODE(*ip))
 *
 *   // 3. Each handler uses ZORYA_DISPATCH_CASE
 *   ZORYA_DISPATCH_CASE(OP_ADD) {
 *       ... handle add ...
 *       ip++;
 *       my_dispatch_next();   // your app-level macro wrapping below
 *   }
 *
 *   // 4. Close the dispatch
 *   ZORYA_DISPATCH_STOP
 *
 *   // Your app wraps ZORYA_DISPATCH_JUMP for the "next" logic:
 *   #define my_dispatch_next() do { \
 *       save_state(); \
 *       ZORYA_DISPATCH_JUMP(GET_OPCODE(*ip)); \
 *   } while(0)
 *
 * In computed-goto mode, ZORYA_DISPATCH_JUMP does:
 *     goto *zorya__dtab[opcode]
 *
 * In switch mode, ZORYA_DISPATCH_JUMP does:
 *     break    (falls through to the for(;;) switch re-entry)
 *
 * The jump table, labels, and all platform-specific machinery
 * live INSIDE the macros. Your VM code stays clean.
 *
 * ============================================================================
 * COMPILER SUPPORT
 * ============================================================================
 *
 *   GCC 4.8+       Full detection, computed goto
 *   Clang 3.4+     Full detection, computed goto
 *   MSVC 2015+     Full detection, switch fallback
 *   ICC 16+        Full detection, computed goto
 *   TCC            Basic detection, switch fallback
 *   ARM CC         Basic detection, switch fallback
 *
 */

#ifndef ZORYA_PAL_DETECT_H
#define ZORYA_PAL_DETECT_H

/* ============================================================
 * COMPILER DETECTION
 *
 * Order matters: Clang defines __GNUC__ too, so test Clang first.
 * ICC defines __GNUC__ on Linux, so test ICC before GCC.
 * ============================================================ */

/** @brief Clang/LLVM */
#if defined(__clang__)
    #define ZORYA_CC_CLANG  1
    #define ZORYA_CC_GCC    0
    #define ZORYA_CC_MSVC   0
    #define ZORYA_CC_ICC    0
    #define ZORYA_CC_TCC    0
    #define ZORYA_CC_ARMCC  0
    #define ZORYA_CC_VER_MAJOR  __clang_major__
    #define ZORYA_CC_VER_MINOR  __clang_minor__

/** @brief Intel C/C++ Compiler */
#elif defined(__INTEL_COMPILER) || defined(__ICC)
    #define ZORYA_CC_CLANG  0
    #define ZORYA_CC_GCC    0
    #define ZORYA_CC_MSVC   0
    #define ZORYA_CC_ICC    1
    #define ZORYA_CC_TCC    0
    #define ZORYA_CC_ARMCC  0
    #define ZORYA_CC_VER_MAJOR  (__INTEL_COMPILER / 100)
    #define ZORYA_CC_VER_MINOR  ((__INTEL_COMPILER / 10) % 10)

/** @brief GCC (must be after Clang and ICC checks) */
#elif defined(__GNUC__)
    #define ZORYA_CC_CLANG  0
    #define ZORYA_CC_GCC    1
    #define ZORYA_CC_MSVC   0
    #define ZORYA_CC_ICC    0
    #define ZORYA_CC_TCC    0
    #define ZORYA_CC_ARMCC  0
    #define ZORYA_CC_VER_MAJOR  __GNUC__
    #define ZORYA_CC_VER_MINOR  __GNUC_MINOR__

/** @brief Microsoft Visual C++ */
#elif defined(_MSC_VER)
    #define ZORYA_CC_CLANG  0
    #define ZORYA_CC_GCC    0
    #define ZORYA_CC_MSVC   1
    #define ZORYA_CC_ICC    0
    #define ZORYA_CC_TCC    0
    #define ZORYA_CC_ARMCC  0
    #define ZORYA_CC_VER_MAJOR  (_MSC_VER / 100)
    #define ZORYA_CC_VER_MINOR  (_MSC_VER % 100)

/** @brief Tiny C Compiler */
#elif defined(__TINYC__)
    #define ZORYA_CC_CLANG  0
    #define ZORYA_CC_GCC    0
    #define ZORYA_CC_MSVC   0
    #define ZORYA_CC_ICC    0
    #define ZORYA_CC_TCC    1
    #define ZORYA_CC_ARMCC  0
    #define ZORYA_CC_VER_MAJOR  0
    #define ZORYA_CC_VER_MINOR  0

/** @brief ARM Compiler */
#elif defined(__ARMCC_VERSION) || defined(__CC_ARM)
    #define ZORYA_CC_CLANG  0
    #define ZORYA_CC_GCC    0
    #define ZORYA_CC_MSVC   0
    #define ZORYA_CC_ICC    0
    #define ZORYA_CC_TCC    0
    #define ZORYA_CC_ARMCC  1
    #define ZORYA_CC_VER_MAJOR  0
    #define ZORYA_CC_VER_MINOR  0

/** @brief Unknown compiler */
#else
    #define ZORYA_CC_CLANG  0
    #define ZORYA_CC_GCC    0
    #define ZORYA_CC_MSVC   0
    #define ZORYA_CC_ICC    0
    #define ZORYA_CC_TCC    0
    #define ZORYA_CC_ARMCC  0
    #define ZORYA_CC_VER_MAJOR  0
    #define ZORYA_CC_VER_MINOR  0
#endif

/** @brief True if compiler supports GNU extensions (GCC, Clang, ICC on Linux) */
#define ZORYA_CC_GNU_COMPAT  (ZORYA_CC_GCC || ZORYA_CC_CLANG || ZORYA_CC_ICC)

/* ============================================================
 * OPERATING SYSTEM DETECTION
 * ============================================================ */

#if defined(__linux__)
    #define ZORYA_OS_LINUX    1
    #define ZORYA_OS_MACOS    0
    #define ZORYA_OS_WINDOWS  0
    #define ZORYA_OS_FREEBSD  0
    #define ZORYA_OS_OPENBSD  0
    #define ZORYA_OS_NETBSD   0
    #define ZORYA_OS_IOS      0
    #define ZORYA_OS_ANDROID  0
    #define ZORYA_OS_NAME     "Linux"

#elif defined(__APPLE__) && defined(__MACH__)
    #include <TargetConditionals.h>
    #if TARGET_OS_IPHONE
        #define ZORYA_OS_LINUX    0
        #define ZORYA_OS_MACOS    0
        #define ZORYA_OS_WINDOWS  0
        #define ZORYA_OS_FREEBSD  0
        #define ZORYA_OS_OPENBSD  0
        #define ZORYA_OS_NETBSD   0
        #define ZORYA_OS_IOS      1
        #define ZORYA_OS_ANDROID  0
        #define ZORYA_OS_NAME     "iOS"
    #else
        #define ZORYA_OS_LINUX    0
        #define ZORYA_OS_MACOS    1
        #define ZORYA_OS_WINDOWS  0
        #define ZORYA_OS_FREEBSD  0
        #define ZORYA_OS_OPENBSD  0
        #define ZORYA_OS_NETBSD   0
        #define ZORYA_OS_IOS      0
        #define ZORYA_OS_ANDROID  0
        #define ZORYA_OS_NAME     "macOS"
    #endif

#elif defined(_WIN32) || defined(_WIN64)
    #define ZORYA_OS_LINUX    0
    #define ZORYA_OS_MACOS    0
    #define ZORYA_OS_WINDOWS  1
    #define ZORYA_OS_FREEBSD  0
    #define ZORYA_OS_OPENBSD  0
    #define ZORYA_OS_NETBSD   0
    #define ZORYA_OS_IOS      0
    #define ZORYA_OS_ANDROID  0
    #define ZORYA_OS_NAME     "Windows"

#elif defined(__FreeBSD__)
    #define ZORYA_OS_LINUX    0
    #define ZORYA_OS_MACOS    0
    #define ZORYA_OS_WINDOWS  0
    #define ZORYA_OS_FREEBSD  1
    #define ZORYA_OS_OPENBSD  0
    #define ZORYA_OS_NETBSD   0
    #define ZORYA_OS_IOS      0
    #define ZORYA_OS_ANDROID  0
    #define ZORYA_OS_NAME     "FreeBSD"

#elif defined(__OpenBSD__)
    #define ZORYA_OS_LINUX    0
    #define ZORYA_OS_MACOS    0
    #define ZORYA_OS_WINDOWS  0
    #define ZORYA_OS_FREEBSD  0
    #define ZORYA_OS_OPENBSD  1
    #define ZORYA_OS_NETBSD   0
    #define ZORYA_OS_IOS      0
    #define ZORYA_OS_ANDROID  0
    #define ZORYA_OS_NAME     "OpenBSD"

#elif defined(__NetBSD__)
    #define ZORYA_OS_LINUX    0
    #define ZORYA_OS_MACOS    0
    #define ZORYA_OS_WINDOWS  0
    #define ZORYA_OS_FREEBSD  0
    #define ZORYA_OS_OPENBSD  0
    #define ZORYA_OS_NETBSD   1
    #define ZORYA_OS_IOS      0
    #define ZORYA_OS_ANDROID  0
    #define ZORYA_OS_NAME     "NetBSD"

#elif defined(__ANDROID__)
    #define ZORYA_OS_LINUX    0
    #define ZORYA_OS_MACOS    0
    #define ZORYA_OS_WINDOWS  0
    #define ZORYA_OS_FREEBSD  0
    #define ZORYA_OS_OPENBSD  0
    #define ZORYA_OS_NETBSD   0
    #define ZORYA_OS_IOS      0
    #define ZORYA_OS_ANDROID  1
    #define ZORYA_OS_NAME     "Android"

#else
    #define ZORYA_OS_LINUX    0
    #define ZORYA_OS_MACOS    0
    #define ZORYA_OS_WINDOWS  0
    #define ZORYA_OS_FREEBSD  0
    #define ZORYA_OS_OPENBSD  0
    #define ZORYA_OS_NETBSD   0
    #define ZORYA_OS_IOS      0
    #define ZORYA_OS_ANDROID  0
    #define ZORYA_OS_NAME     "Unknown"
#endif

/** @brief True on any POSIX-compliant OS (Linux, macOS, *BSD, Android) */
#define ZORYA_OS_POSIX  \
    (ZORYA_OS_LINUX || ZORYA_OS_MACOS || ZORYA_OS_FREEBSD || \
     ZORYA_OS_OPENBSD || ZORYA_OS_NETBSD || ZORYA_OS_ANDROID)

/** @brief True on any Unix-like OS (POSIX + iOS) */
#define ZORYA_OS_UNIX   (ZORYA_OS_POSIX || ZORYA_OS_IOS)

/* ============================================================
 * ARCHITECTURE DETECTION
 * ============================================================ */

#if defined(__x86_64__) || defined(_M_X64) || defined(__amd64__)
    #define ZORYA_ARCH_X64    1
    #define ZORYA_ARCH_X86    0
    #define ZORYA_ARCH_ARM64  0
    #define ZORYA_ARCH_ARM32  0
    #define ZORYA_ARCH_RISCV  0
    #define ZORYA_ARCH_MIPS   0
    #define ZORYA_ARCH_PPC    0
    #define ZORYA_ARCH_BITS   64
    #define ZORYA_ARCH_NAME   "x86_64"

#elif defined(__i386__) || defined(_M_IX86) || defined(__i686__)
    #define ZORYA_ARCH_X64    0
    #define ZORYA_ARCH_X86    1
    #define ZORYA_ARCH_ARM64  0
    #define ZORYA_ARCH_ARM32  0
    #define ZORYA_ARCH_RISCV  0
    #define ZORYA_ARCH_MIPS   0
    #define ZORYA_ARCH_PPC    0
    #define ZORYA_ARCH_BITS   32
    #define ZORYA_ARCH_NAME   "x86"

#elif defined(__aarch64__) || defined(_M_ARM64)
    #define ZORYA_ARCH_X64    0
    #define ZORYA_ARCH_X86    0
    #define ZORYA_ARCH_ARM64  1
    #define ZORYA_ARCH_ARM32  0
    #define ZORYA_ARCH_RISCV  0
    #define ZORYA_ARCH_MIPS   0
    #define ZORYA_ARCH_PPC    0
    #define ZORYA_ARCH_BITS   64
    #define ZORYA_ARCH_NAME   "ARM64"

#elif defined(__arm__) || defined(_M_ARM)
    #define ZORYA_ARCH_X64    0
    #define ZORYA_ARCH_X86    0
    #define ZORYA_ARCH_ARM64  0
    #define ZORYA_ARCH_ARM32  1
    #define ZORYA_ARCH_RISCV  0
    #define ZORYA_ARCH_MIPS   0
    #define ZORYA_ARCH_PPC    0
    #define ZORYA_ARCH_BITS   32
    #define ZORYA_ARCH_NAME   "ARM32"

#elif defined(__riscv)
    #define ZORYA_ARCH_X64    0
    #define ZORYA_ARCH_X86    0
    #define ZORYA_ARCH_ARM64  0
    #define ZORYA_ARCH_ARM32  0
    #define ZORYA_ARCH_RISCV  1
    #define ZORYA_ARCH_MIPS   0
    #define ZORYA_ARCH_PPC    0
    #if __riscv_xlen == 64
        #define ZORYA_ARCH_BITS 64
    #else
        #define ZORYA_ARCH_BITS 32
    #endif
    #define ZORYA_ARCH_NAME   "RISC-V"

#elif defined(__mips__) || defined(__mips)
    #define ZORYA_ARCH_X64    0
    #define ZORYA_ARCH_X86    0
    #define ZORYA_ARCH_ARM64  0
    #define ZORYA_ARCH_ARM32  0
    #define ZORYA_ARCH_RISCV  0
    #define ZORYA_ARCH_MIPS   1
    #define ZORYA_ARCH_PPC    0
    #define ZORYA_ARCH_BITS   32
    #define ZORYA_ARCH_NAME   "MIPS"

#elif defined(__powerpc__) || defined(__ppc__) || defined(_M_PPC)
    #define ZORYA_ARCH_X64    0
    #define ZORYA_ARCH_X86    0
    #define ZORYA_ARCH_ARM64  0
    #define ZORYA_ARCH_ARM32  0
    #define ZORYA_ARCH_RISCV  0
    #define ZORYA_ARCH_MIPS   0
    #define ZORYA_ARCH_PPC    1
    #if defined(__powerpc64__) || defined(__ppc64__)
        #define ZORYA_ARCH_BITS 64
    #else
        #define ZORYA_ARCH_BITS 32
    #endif
    #define ZORYA_ARCH_NAME   "PowerPC"

#else
    #define ZORYA_ARCH_X64    0
    #define ZORYA_ARCH_X86    0
    #define ZORYA_ARCH_ARM64  0
    #define ZORYA_ARCH_ARM32  0
    #define ZORYA_ARCH_RISCV  0
    #define ZORYA_ARCH_MIPS   0
    #define ZORYA_ARCH_PPC    0
    #define ZORYA_ARCH_BITS   0
    #define ZORYA_ARCH_NAME   "Unknown"
#endif

/* ============================================================
 * ENDIANNESS DETECTION
 * ============================================================ */

#if defined(__BYTE_ORDER__)
    #if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
        #define ZORYA_ENDIAN_LITTLE 1
        #define ZORYA_ENDIAN_BIG    0
    #elif __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
        #define ZORYA_ENDIAN_LITTLE 0
        #define ZORYA_ENDIAN_BIG    1
    #else
        #define ZORYA_ENDIAN_LITTLE 0
        #define ZORYA_ENDIAN_BIG    0
    #endif
#elif ZORYA_ARCH_X64 || ZORYA_ARCH_X86
    /* x86 is always little-endian */
    #define ZORYA_ENDIAN_LITTLE 1
    #define ZORYA_ENDIAN_BIG    0
#elif ZORYA_OS_WINDOWS
    /* Windows is always little-endian in practice */
    #define ZORYA_ENDIAN_LITTLE 1
    #define ZORYA_ENDIAN_BIG    0
#else
    /* Assume little-endian as default (correct for ARM Linux, RISC-V) */
    #define ZORYA_ENDIAN_LITTLE 1
    #define ZORYA_ENDIAN_BIG    0
#endif

/* ============================================================
 * POINTER SIZE
 * ============================================================ */

#if ZORYA_ARCH_BITS == 64
    #define ZORYA_PTR_SIZE 8
#elif ZORYA_ARCH_BITS == 32
    #define ZORYA_PTR_SIZE 4
#else
    /* Fallback: detect from sizeof(void*) at compile time */
    #define ZORYA_PTR_SIZE 0   /* Unknown — use sizeof(void*) */
#endif

/* ============================================================
 * C STANDARD DETECTION
 * ============================================================ */

#if defined(__STDC_VERSION__)
    #if __STDC_VERSION__ >= 202311L
        #define ZORYA_C_STD 23
    #elif __STDC_VERSION__ >= 201710L
        #define ZORYA_C_STD 17
    #elif __STDC_VERSION__ >= 201112L
        #define ZORYA_C_STD 11
    #elif __STDC_VERSION__ >= 199901L
        #define ZORYA_C_STD 99
    #else
        #define ZORYA_C_STD 89
    #endif
#else
    #define ZORYA_C_STD 89
#endif

/* ============================================================
 * DISPATCH MACROS - Platform-Agnostic VM Instruction Dispatch
 * ============================================================
 *
 * Encapsulates computed-goto vs switch/case dispatch in a single
 * macro set. The jump table lives INSIDE the macros — your VM
 * code stays clean and portable.
 *
 * Computed goto (GCC/Clang):
 *   - Labels-as-values extension: &&label
 *   - O(1) indirect jump via goto *table[op]
 *   - ~30% faster than switch in tight loops
 *   - Better branch prediction (per-site predictor entries)
 *
 * Switch/case fallback (all compilers):
 *   - Standard C99, works everywhere
 *   - Compiler may optimize to jump table anyway at -O2
 *   - Clean, debugger-friendly
 *
 * The function pointer table approach was considered but rejected:
 * it adds function call overhead (prologue/epilogue) and prevents
 * the compiler from keeping registers live across handlers. Switch
 * keeps everything in one function — closer to computed goto perf.
 */

/** @brief 1 if compiler supports labels-as-values (computed goto) */
#if ZORYA_CC_GNU_COMPAT
    #define ZORYA_HAS_COMPUTED_GOTO 1
#else
    #define ZORYA_HAS_COMPUTED_GOTO 0
#endif

#if ZORYA_HAS_COMPUTED_GOTO

    /**
     * @brief Begin the dispatch jump table definition.
     * Declares a static array of label addresses.
     */
    #define ZORYA_DISPATCH_TABLE_BEGIN \
        static const void *zorya__dtab[] = {

    /**
     * @brief Add an entry to the dispatch table.
     * Maps opcode enum value → label address.
     * @param op  The opcode enum constant (e.g., OP_ADD)
     */
    #define ZORYA_DISPATCH_ENTRY(op) \
        [op] = &&ZORYA_L_##op

    /**
     * @brief End the dispatch table definition.
     */
    #define ZORYA_DISPATCH_TABLE_END \
        };

    /**
     * @brief Start execution — jump to the first instruction.
     * Call this AFTER the table definition, with the initial opcode.
     * @param opcode  Expression yielding the current opcode
     */
    #define ZORYA_DISPATCH_START(opcode) \
        goto *zorya__dtab[opcode];

    /**
     * @brief Label for an opcode handler.
     * @param op  The opcode enum constant
     */
    #define ZORYA_DISPATCH_CASE(op) \
        ZORYA_L_##op:

    /**
     * @brief Jump to the next instruction's handler.
     * In computed-goto mode: indirect jump via the table.
     * @param opcode  Expression yielding the next opcode
     */
    #define ZORYA_DISPATCH_JUMP(opcode) \
        goto *zorya__dtab[opcode]

    /**
     * @brief End of the dispatch region (no-op in computed-goto mode).
     */
    #define ZORYA_DISPATCH_STOP  /* nothing */

#else /* Switch/case fallback */

    #define ZORYA_DISPATCH_TABLE_BEGIN    /* no table */
    #define ZORYA_DISPATCH_ENTRY(op)      /* unused */
    #define ZORYA_DISPATCH_TABLE_END      /* no table */

    /**
     * @brief Start the dispatch loop — opens for(;;) switch.
     * @param opcode  Expression yielding the current opcode
     */
    #define ZORYA_DISPATCH_START(opcode) \
        for (;;) { switch (opcode) {

    /**
     * @brief Case label for an opcode handler.
     * @param op  The opcode enum constant
     */
    #define ZORYA_DISPATCH_CASE(op) \
        case op:

    /**
     * @brief "Jump" to next instruction — just break back to the switch.
     * @param opcode  Ignored in switch mode (re-evaluated at loop top)
     */
    #define ZORYA_DISPATCH_JUMP(opcode) \
        break

    /**
     * @brief Close the dispatch loop.
     */
    #define ZORYA_DISPATCH_STOP \
        default: break; } }

#endif /* ZORYA_HAS_COMPUTED_GOTO */

#endif /* ZORYA_PAL_DETECT_H */
