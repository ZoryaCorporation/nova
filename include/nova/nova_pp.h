/**
 * @file nova_pp.h
 * @brief Nova Language - Preprocessor
 *
 * Token-based preprocessor supporting #include, #define (object-like
 * and function-like macros), conditional compilation, and more.
 *
 * The preprocessor operates on token streams from the lexer,
 * not on raw text. This gives us:
 *   - Hygienic macros (no accidental name capture)
 *   - Accurate source location tracking through expansions
 *   - Type-aware expansion (tokens carry type info)
 *
 * @author Anthony Taliento
 * @date 2026-02-05
 * @version 0.2.0
 *
 * @copyright Copyright (c) 2026 Zorya Corporation
 * @license MIT
 *
 * ZORYA-C COMPLIANCE: v2.0.0
 *
 * DEPENDENCIES:
 *   - nova_lex.h (token types and lexer)
 *   - nova_conf.h (limits: max depth, max params)
 *   - zorya/dagger.h (macro table storage)
 *   - zorya/weave.h (string operations)
 */

#ifndef NOVA_PP_H
#define NOVA_PP_H

#include "nova_lex.h"
#include "nova_conf.h"

#include <stddef.h>
#include <stdint.h>

/* ============================================================
 * MACRO DEFINITIONS
 * ============================================================ */

/**
 * @brief Macro parameter descriptor
 */
typedef struct {
    const char *name;       /**< Parameter name                 */
    size_t      name_len;   /**< Parameter name length          */
} NovaMacroParam;

/**
 * @brief Token in a macro body (replacement list)
 *
 * For numeric tokens (INTEGER/NUMBER), the actual value is stored
 * in numeric_int / numeric_num rather than relying on the text field
 * alone. This avoids union aliasing bugs when the lexer token's
 * value.string is read for a numeric type.
 */
typedef struct {
    NovaTokenType type;     /**< Token type                     */
    const char   *text;     /**< Token text (owned by PP)       */
    size_t        text_len; /**< Token text length              */
    nova_int_t    numeric_int;  /**< Integer value (for INTEGER) */
    nova_number_t numeric_num;  /**< Float value (for NUMBER)    */
    int           param_idx;/**< If >= 0, index of macro param  */
    int           stringify;/**< Preceded by # (stringify op)   */
    int           paste_left;/**< Followed by ## (paste left)   */
    int           paste_right;/**< Preceded by ## (paste right) */
} NovaMacroToken;

/**
 * @brief Macro definition
 */
typedef struct {
    const char      *name;          /**< Macro name                  */
    size_t           name_len;      /**< Macro name length           */

    int              is_function;   /**< Function-like macro?        */
    int              is_variadic;   /**< Has __VA_ARGS__?            */
    int              is_predefined; /**< Built-in macro?             */

    /* Parameters (for function-like macros) */
    NovaMacroParam  *params;        /**< Parameter array             */
    int              param_count;   /**< Number of parameters        */

    /* Replacement token list */
    NovaMacroToken  *body;          /**< Body token array            */
    int              body_count;    /**< Number of body tokens       */

    /* Source location of definition */
    NovaSourceLoc    def_loc;       /**< Where this macro was defined*/
} NovaMacroDef;

/* ============================================================
 * PREPROCESSOR STATE
 * ============================================================ */

/**
 * @brief File inclusion stack entry
 */
typedef struct {
    NovaLexer   lexer;      /**< Lexer for this file             */
    const char *path;       /**< File path (owned)               */
    char       *content;    /**< File content (owned, freed on pop) */
} NovaPPFileEntry;

/**
 * @brief Conditional compilation stack entry
 */
typedef struct {
    int active;     /**< Is this branch currently active?         */
    int seen_true;  /**< Have we seen a true branch yet?          */
    int is_else;    /**< Is this the #else branch?                */
} NovaPPCondEntry;

/**
 * @brief Preprocessor state
 */
typedef struct {
    /* Macro table (DAGGER-backed) */
    void           *macros;         /**< DaggerTable* for macro lookup */

    /* File inclusion stack */
    NovaPPFileEntry file_stack[NOVA_PP_MAX_INCLUDE_DEPTH];
    int             file_depth;     /**< Current inclusion depth    */

    /* Conditional compilation stack */
    NovaPPCondEntry cond_stack[NOVA_PP_MAX_IF_DEPTH];
    int             cond_depth;     /**< Current conditional depth  */

    /* Macro expansion tracking */
    int             expansion_depth;/**< Current macro expansion depth */
    uint64_t        counter;        /**< __COUNTER__ value          */

    /* Output token buffer */
    NovaToken      *output;         /**< Expanded token buffer      */
    size_t          output_count;   /**< Tokens in output           */
    size_t          output_cap;     /**< Output buffer capacity     */
    size_t          output_pos;     /**< Read position in output    */

    /* Include search paths */
    const char    **search_paths;   /**< Array of include directories */
    int             search_path_count;

    /* #import flags (bitfield, set during preprocessing) */
    uint32_t        import_flags;   /**< NOVA_IMPORT_* bitfield     */

    /* Error state */
    char            error_msg[512]; /**< Last error message         */
    int             error_count;    /**< Total errors               */
    int             warning_count;  /**< Total warnings             */

    /* Callbacks */
    void           *userdata;       /**< User context               */
} NovaPP;

/* ============================================================
 * #import FLAGS
 *
 * Bitfield constants for nova_pp_get_imports(). Each bit
 * corresponds to a data format module loaded via #import.
 *
 *   #import json   -> NOVA_IMPORT_JSON
 *   #import csv    -> NOVA_IMPORT_CSV
 *   #import tsv    -> NOVA_IMPORT_TSV
 *   #import ini    -> NOVA_IMPORT_INI
 *   #import toml   -> NOVA_IMPORT_TOML
 *   #import html   -> NOVA_IMPORT_HTML
 *   #import yaml   -> NOVA_IMPORT_YAML
 *   #import nini   -> NOVA_IMPORT_NINI
 *   #import data   -> NOVA_IMPORT_ALL (all formats + data.*)
 * ============================================================ */

#define NOVA_IMPORT_JSON  ((uint32_t)0x0001)
#define NOVA_IMPORT_CSV   ((uint32_t)0x0002)
#define NOVA_IMPORT_TSV   ((uint32_t)0x0004)
#define NOVA_IMPORT_INI   ((uint32_t)0x0008)
#define NOVA_IMPORT_TOML  ((uint32_t)0x0010)
#define NOVA_IMPORT_HTML  ((uint32_t)0x0020)
#define NOVA_IMPORT_YAML  ((uint32_t)0x0040)
#define NOVA_IMPORT_NET   ((uint32_t)0x0080)  /**< net.* HTTP/HTTPS    */
#define NOVA_IMPORT_SQL   ((uint32_t)0x0100)  /**< sql.* database      */
#define NOVA_IMPORT_NINI  ((uint32_t)0x0200)  /**< nini.* Nova INI     */
#define NOVA_IMPORT_DATA  ((uint32_t)0x8000)  /**< data.* meta-module  */
#define NOVA_IMPORT_ALL   ((uint32_t)0x83FF)  /**< all of the above    */

/* ============================================================
 * PREPROCESSOR API
 * ============================================================ */

/**
 * @brief Create a new preprocessor instance
 *
 * @return New preprocessor, or NULL on allocation failure
 *
 * @post Caller must call nova_pp_destroy() when done
 */
NovaPP *nova_pp_create(void);

/**
 * @brief Destroy preprocessor and free all resources
 *
 * @param pp Preprocessor instance (NULL is safe/no-op)
 */
void nova_pp_destroy(NovaPP *pp);

/**
 * @brief Add an include search path
 *
 * @param pp   Preprocessor instance
 * @param path Directory path to search for #include files
 * @return 0 on success, -1 on error
 */
int nova_pp_add_search_path(NovaPP *pp, const char *path);

/**
 * @brief Define a macro from C code
 *
 * Equivalent to: #define name value
 *
 * @param pp    Preprocessor instance
 * @param name  Macro name
 * @param value Macro value (can be NULL for flag-only macros)
 * @return 0 on success, -1 on error
 */
int nova_pp_define(NovaPP *pp, const char *name, const char *value);

/**
 * @brief Undefine a macro
 *
 * @param pp   Preprocessor instance
 * @param name Macro name to undefine
 * @return 0 on success, -1 if not defined
 */
int nova_pp_undef(NovaPP *pp, const char *name);

/**
 * @brief Check if a macro is defined
 *
 * @param pp   Preprocessor instance
 * @param name Macro name
 * @return 1 if defined, 0 if not
 */
int nova_pp_is_defined(const NovaPP *pp, const char *name);

/**
 * @brief Process a source file through the preprocessor
 *
 * Reads the file, runs lexer + preprocessor, and produces
 * an expanded token stream ready for the parser.
 *
 * @param pp    Preprocessor instance
 * @param path  Path to source file (.n or .m)
 * @return 0 on success, -1 on error
 */
int nova_pp_process_file(NovaPP *pp, const char *path);

/**
 * @brief Process a source string through the preprocessor
 *
 * @param pp       Preprocessor instance
 * @param source   Source code string
 * @param len      Source length (0 = strlen)
 * @param filename Name for error messages
 * @return 0 on success, -1 on error
 */
int nova_pp_process_string(NovaPP *pp, const char *source,
                           size_t len, const char *filename);

/**
 * @brief Get the next preprocessed token
 *
 * Returns tokens after macro expansion and conditional evaluation.
 *
 * @param pp    Preprocessor instance
 * @param token Output token (must not be NULL)
 * @return Token type, or NOVA_TOKEN_EOF at end, or NOVA_TOKEN_ERROR
 */
NovaTokenType nova_pp_next_token(NovaPP *pp, NovaToken *token);

/**
 * @brief Get preprocessor error message
 *
 * @param pp Preprocessor instance
 * @return Error message (valid until next operation)
 */
const char *nova_pp_error(const NovaPP *pp);

/**
 * @brief Get error count
 */
int nova_pp_error_count(const NovaPP *pp);

/**
 * @brief Get warning count
 */
int nova_pp_warning_count(const NovaPP *pp);

/**
 * @brief Reset preprocessor state for reuse
 *
 * Clears all state except search paths and predefined macros.
 *
 * @param pp Preprocessor instance
 */
void nova_pp_reset(NovaPP *pp);

/**
 * @brief Get the #import flags collected during preprocessing.
 *
 * Returns a bitfield of NOVA_IMPORT_* values representing which
 * data format modules were requested via #import directives.
 *
 * @param pp  Preprocessor instance
 * @return Bitfield of NOVA_IMPORT_* flags, 0 if none
 */
uint32_t nova_pp_get_imports(const NovaPP *pp);

#endif /* NOVA_PP_H */
