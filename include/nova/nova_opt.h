/**
 * @file nova_opt.h
 * @brief Nova Language - Bytecode Optimizer
 *
 * Post-compilation optimization passes operating on NovaProto.
 * The optimizer transforms bytecode in-place after the compiler
 * produces it and before the codegen serializes it to .no format.
 *
 * Pipeline:  AST --> Compiler --> Optimizer --> Codegen
 *                                 ^^^^^^^^^
 *
 * Optimization passes (applied in order):
 *   1. Peephole:       Local instruction-level rewrites
 *   2. Constant fold:  Evaluate constant arithmetic at compile time
 *   3. Jump optimize:  Collapse jump chains, remove jump-to-next
 *   4. Tail call:      Convert CALL+RETURN sequences to TAILCALL
 *   5. Dead code:      Remove unreachable instructions after returns/jumps
 *   6. Return fixup:   Ensure RETURN0/RETURN1 specialization
 *
 * Each pass is safe to run independently. Higher optimization
 * levels enable more aggressive (and costly) passes.
 *
 * @author Anthony Taliento
 * @date 2026-02-08
 * @version 0.2.0
 *
 * @copyright Copyright (c) 2026 Zorya Corporation
 * @license MIT
 *
 * ZORYA-C COMPLIANCE: v2.0.0
 */

#ifndef NOVA_OPT_H
#define NOVA_OPT_H

#include "nova_proto.h"

/* ============================================================
 * OPTIMIZATION LEVELS
 *
 * Mirror common compiler conventions:
 *   O0 = no optimization (skip entirely)
 *   O1 = safe, fast passes (peephole + jump folding)
 *   O2 = full optimization (all passes)
 *   O3 = aggressive (future: register reallocation, etc.)
 * ============================================================ */

#define NOVA_OPT_NONE       0   /* No optimization                */
#define NOVA_OPT_BASIC      1   /* Peephole + jump optimization   */
#define NOVA_OPT_FULL       2   /* All passes (default)           */
#define NOVA_OPT_AGGRESSIVE 3   /* Future: register reallocation  */

/** Default optimization level */
#ifndef NOVA_OPT_DEFAULT
    #define NOVA_OPT_DEFAULT NOVA_OPT_FULL
#endif

/* ============================================================
 * OPTIMIZATION STATISTICS
 *
 * Optional counters to track what the optimizer did.
 * Useful for debugging and benchmarking the optimizer itself.
 * ============================================================ */

/** Statistics collected during optimization */
typedef struct {
    uint32_t peephole_rewrites;     /* Instructions rewritten         */
    uint32_t constants_folded;      /* Constant expressions evaluated */
    uint32_t jumps_shortened;       /* Jump chains collapsed          */
    uint32_t jumps_removed;         /* Jump-to-next eliminated        */
    uint32_t tailcalls_detected;    /* CALL+RETURN -> TAILCALL        */
    uint32_t dead_instructions;     /* Unreachable instructions NOPed */
    uint32_t returns_specialized;   /* RETURN -> RETURN0/RETURN1      */
    uint32_t nops_removed;          /* NOPs removed in final squeeze  */
    uint32_t total_before;          /* Instruction count before opt   */
    uint32_t total_after;           /* Instruction count after opt    */
} NovaOptStats;

/* ============================================================
 * PUBLIC API
 * ============================================================ */

/**
 * @brief Optimize a compiled function prototype in-place.
 *
 * Runs optimization passes on the proto's bytecode according
 * to the specified optimization level. Also recurses into all
 * sub-prototypes (nested functions / closures).
 *
 * Safe to call multiple times, though redundant. Safe to call
 * with level=NOVA_OPT_NONE (returns immediately).
 *
 * @param proto  Compiled prototype to optimize (must not be NULL)
 * @param level  Optimization level (NOVA_OPT_NONE .. NOVA_OPT_AGGRESSIVE)
 *
 * @pre proto != NULL
 * @pre proto->code != NULL && proto->code_count > 0
 * @post proto->code may be rewritten; proto->code_count may decrease
 *
 * COMPLEXITY: O(n) per pass where n = instruction count
 * THREAD SAFETY: Not thread-safe
 */
void nova_optimize(NovaProto *proto, int level);

/**
 * @brief Optimize with statistics collection.
 *
 * Same as nova_optimize(), but also fills in the stats struct
 * with counters for each optimization performed. Stats are
 * accumulated across all sub-prototypes.
 *
 * @param proto  Compiled prototype to optimize (must not be NULL)
 * @param level  Optimization level
 * @param stats  Output statistics (must not be NULL, zeroed first)
 *
 * @pre proto != NULL && stats != NULL
 *
 * COMPLEXITY: O(n) per pass
 * THREAD SAFETY: Not thread-safe
 */
void nova_optimize_stats(NovaProto *proto, int level, NovaOptStats *stats);

/**
 * @brief Print optimization statistics to stderr.
 *
 * Formats the stats struct as a human-readable report.
 * Useful for development and benchmarking.
 *
 * @param stats  Statistics to print (must not be NULL)
 */
void nova_opt_dump_stats(const NovaOptStats *stats);

#endif /* NOVA_OPT_H */
