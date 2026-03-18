/**
 * @file nova_nini.h
 * @brief NINI — Nova's native INI format (lingua franca)
 *
 * Enhanced INI with:
 *   - Auto-typed values (int, float, bool, nil, string)
 *   - Arrays via key[] = value and inline [a, b, c]
 *   - Nested sections via dot notation: [parent.child]
 *   - Variable interpolation: ${section.key}
 *   - Multi-line strings: key = """..."""
 *   - Task definitions: [task:name]
 *   - @include directives
 *   - Comments: # and ;
 *
 * Standalone module — no dependency on NDP.
 * NDP includes this header for format conversion dispatch.
 * Scripts access via `#import nini` which provides:
 *   nini.decode(text)   -- string -> table
 *   nini.encode(table)  -- table -> string
 *   nini.load(filename) -- file -> table
 *   nini.save(filename, table) -- table -> file
 *
 * @author Anthony Taliento
 * @date 2026-02-18
 * @version 1.0.0
 *
 * @copyright Copyright (c) 2026 Zorya Corporation
 * @license MIT
 *
 * ZORYA-C COMPLIANCE: v2.0.0
 *
 * DEPENDENCIES:
 *   - nova_vm.h (NovaVM, value types, table API)
 *
 * THREAD SAFETY:
 *   Not thread-safe. Each call operates on a single VM instance.
 */

#ifndef NOVA_NINI_H
#define NOVA_NINI_H

#include <stddef.h>

/* Forward declaration (avoid pulling in nova_vm.h everywhere) */
struct NovaVM;

/* ============================================================
 * OPTIONS
 * ============================================================ */

typedef struct {
    int interpolate;    /**< 1 = resolve ${section.key} references  */
    int tasks_only;     /**< 1 = parse only [task:*] sections       */
} NiniOptions;

/**
 * @brief Initialize NINI options with defaults.
 *
 * @param opts  Options to initialize (must not be NULL)
 *
 * @pre opts != NULL
 * @post interpolate = 1, tasks_only = 0
 */
void nova_nini_options_init(NiniOptions *opts);

/* ============================================================
 * DECODE / ENCODE API
 * ============================================================ */

/**
 * @brief Decode NINI text into a Nova table.
 *
 * Pushes the result table onto the VM stack on success.
 *
 * @param vm         VM instance (must not be NULL)
 * @param text       NINI text to parse (must not be NULL)
 * @param len        Length of text
 * @param opts       Options (must not be NULL)
 * @param errbuf     Error message buffer (filled on error, may be NULL)
 * @param errbuf_len Size of errbuf
 *
 * @return 0 on success (table pushed on stack), -1 on error
 *
 * @pre vm != NULL && text != NULL && opts != NULL
 */
int nova_nini_decode(struct NovaVM *vm, const char *text, size_t len,
                     const NiniOptions *opts,
                     char *errbuf, size_t errbuf_len);

/**
 * @brief Encode a Nova table into NINI text.
 *
 * Allocates a heap buffer containing the NINI text.
 * Caller must free(*out) when done.
 *
 * @param vm         VM instance (must not be NULL)
 * @param idx        Stack index of the table to encode
 * @param opts       Options (must not be NULL)
 * @param out        Output: pointer to allocated NINI text
 * @param out_len    Output: length of NINI text
 * @param errbuf     Error message buffer (filled on error, may be NULL)
 * @param errbuf_len Size of errbuf
 *
 * @return 0 on success (*out set), -1 on error
 *
 * @pre vm != NULL && opts != NULL && out != NULL && out_len != NULL
 */
int nova_nini_encode(struct NovaVM *vm, int idx,
                     const NiniOptions *opts,
                     char **out, size_t *out_len,
                     char *errbuf, size_t errbuf_len);

#endif /* NOVA_NINI_H */
