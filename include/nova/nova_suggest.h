/**
 * @file nova_suggest.h
 * @brief Nova Language - NLP-Inspired Error Intelligence Engine
 *
 * Provides contextual "did you mean?" suggestions and help hints
 * for runtime and compile-time errors.  Draws on techniques from
 * Natural Language Processing:
 *
 *   - Levenshtein edit distance for fuzzy name matching
 *   - KWIC (Key Word In Context) analysis of source lines
 *   - Pattern-based classification of error contexts
 *   - N-gram style anti-pattern detection
 *
 * The engine analyses the error code, message text, and the
 * surrounding source context to produce actionable sub-diagnostics
 * (help notes) that are attached to the primary error.
 *
 * @author Anthony Taliento
 * @date 2026-03-13
 * @version 0.1.0
 *
 * @copyright Copyright (c) 2026 Zorya Corporation
 * @license MIT
 *
 * ZORYA-C COMPLIANCE: v2.0.0
 */

#ifndef NOVA_SUGGEST_H
#define NOVA_SUGGEST_H

#include "nova/nova_error.h"

/* ============================================================
 * FUZZY NAME MATCHING
 * ============================================================ */

/**
 * @brief Find the closest matching name from a candidate list.
 *
 * Uses Levenshtein edit distance (case-insensitive) to find
 * the best match within max_distance edits.  Candidates may
 * be a NULL-terminated array (pass count = -1) or a counted
 * array.  Exact matches are skipped (not a typo).
 *
 * @param input         The misspelled name
 * @param input_len     Length of input
 * @param candidates    Array of candidate names
 * @param count         Number of candidates, or -1 for NULL-terminated
 * @param max_distance  Maximum acceptable edit distance
 * @return Best matching name, or NULL if none is close enough
 */
const char *nova_suggest_name(const char *input, int input_len,
                              const char **candidates, int count,
                              int max_distance);

/* ============================================================
 * RUNTIME HINT GENERATION
 * ============================================================ */

/**
 * @brief Generate contextual help hints for a runtime error.
 *
 * Analyses the diagnostic code, error message, and source
 * context to produce actionable help/note sub-diagnostics.
 * Writes up to max_hints NovaDiagnostic structs into the
 * caller-provided array.  Each hint has severity NOVA_DIAG_HELP
 * or NOVA_DIAG_NOTE and code NOVA_E0000.
 *
 * The caller should chain the returned hints as sub-diagnostics
 * of the primary error via nova_diag_attach().
 *
 * @param hints         Output array of sub-diagnostics
 * @param max_hints     Capacity of the hints array
 * @param diag_code     Fine-grained error code (NovaErrorCode)
 * @param msg           The error message text
 * @param source_line   Source line where the error occurred (may be NULL)
 * @param source_len    Length of source_line
 * @return Number of hints written (0 = no suggestions)
 */
int nova_suggest_runtime_hints(NovaDiagnostic *hints, int max_hints,
                               int diag_code, const char *msg,
                               const char *source_line, int source_len);

#endif /* NOVA_SUGGEST_H */
