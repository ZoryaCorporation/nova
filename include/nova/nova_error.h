/**
 * @file nova_error.h
 * @brief Nova Language - Rich Diagnostic Error System
 *
 * Provides ANSI-colored, caret-style error diagnostics for the
 * Nova compiler and runtime. Inspired by Rust/Clang error output.
 *
 * Features:
 *   - Severity levels: error, warning, note, help
 *   - ANSI 16-color output with isatty() auto-detection
 *   - Caret-style source context with line numbers and underlines
 *   - Error code catalog with --explain support
 *   - JSON error output mode (--error-format=json)
 *   - Thread-safe global color state
 *
 * @author Anthony Taliento
 * @date 2026-02-10
 * @version 1.0.0
 *
 * @copyright Copyright (c) 2026 Zorya Corporation
 * @license MIT
 *
 * ZORYA-C COMPLIANCE: v2.0.0
 *
 * DEPENDENCIES:
 *   - nova_lex.h (NovaSourceLoc)
 *
 * THREAD SAFETY:
 *   Color state is global. Call nova_diag_init() once at startup.
 */

#ifndef NOVA_ERROR_H
#define NOVA_ERROR_H

#include <stdint.h>
#include <stddef.h>

/* ============================================================
 * ANSI COLOR PALETTE
 *
 * Extended 256-color palette with Nova brand colors.
 * Terminals without 256-color support gracefully fall back
 * to the nearest 16-color equivalent.
 *
 * These are used internally by the renderer. External code
 * should not use these directly; call nova_diag_emit() instead.
 * ============================================================ */

/* ---- Reset & modifiers ---- */
#define NOVA_COLOR_RESET     "\033[0m"
#define NOVA_COLOR_BOLD      "\033[1m"
#define NOVA_COLOR_DIM       "\033[2m"
#define NOVA_COLOR_ITALIC    "\033[3m"
#define NOVA_COLOR_UNDERLINE "\033[4m"

/* ---- Brand colors (256-color) ---- */
#define NOVA_COLOR_HUNTER    "\033[38;5;22m"  /* Hunter green (brand) */
#define NOVA_COLOR_HUNTERB   "\033[1;38;5;22m" /* Hunter green bold   */
#define NOVA_COLOR_NOVA      "\033[1;38;5;40m" /* Nova green (success) */
#define NOVA_COLOR_EMERALD   "\033[38;5;35m"  /* Emerald (accents)    */
#define NOVA_COLOR_FOREST    "\033[38;5;28m"  /* Forest green (dim)   */
#define NOVA_COLOR_LIME      "\033[38;5;118m" /* Lime highlight       */

/* ---- Severity colors ---- */
#define NOVA_COLOR_ERROR     "\033[1;38;5;196m" /* Bright red bold    */
#define NOVA_COLOR_WARNING   "\033[1;38;5;214m" /* Amber/gold bold    */
#define NOVA_COLOR_NOTE      "\033[1;38;5;75m"  /* Sky blue bold      */
#define NOVA_COLOR_HELP      "\033[1;38;5;40m"  /* Nova green bold    */

/* ---- Structural colors ---- */
#define NOVA_COLOR_PATH      "\033[1;38;5;255m" /* Bright white bold  */
#define NOVA_COLOR_LINENUM   "\033[38;5;75m"    /* Sky blue           */
#define NOVA_COLOR_GUTTER    "\033[38;5;240m"   /* Medium gray        */
#define NOVA_COLOR_CARET     "\033[1;38;5;196m" /* Bright red         */
#define NOVA_COLOR_SOURCE    "\033[38;5;252m"   /* Light gray source  */
#define NOVA_COLOR_ERRCODE   "\033[1;38;5;40m"  /* Nova green (code)  */
#define NOVA_COLOR_ARROW     "\033[38;5;75m"    /* Sky blue           */
#define NOVA_COLOR_LNCOL     "\033[38;5;248m"   /* Silver (Ln/Col)    */
#define NOVA_COLOR_PIPE      "\033[38;5;40m"    /* Nova green pipes   */
#define NOVA_COLOR_BANNER    "\033[1;38;5;40m"  /* Nova green banner  */
#define NOVA_COLOR_ACCENT    "\033[38;5;35m"    /* Emerald accents    */
#define NOVA_COLOR_MUTED     "\033[38;5;245m"   /* Muted text         */
#define NOVA_COLOR_CTXLINE   "\033[38;5;242m"   /* Dimmed context     */
#define NOVA_COLOR_EXPECTED  "\033[1;38;5;40m"  /* Nova green (good)  */
#define NOVA_COLOR_GOT       "\033[1;38;5;196m" /* Bright red (bad)   */

/* ============================================================
 * DIAGNOSTIC SEVERITY
 * ============================================================ */

typedef enum {
    NOVA_DIAG_ERROR   = 0,    /**< Fatal error, stops compilation   */
    NOVA_DIAG_WARNING = 1,    /**< Non-fatal warning                */
    NOVA_DIAG_NOTE    = 2,    /**< Informational note               */
    NOVA_DIAG_HELP    = 3     /**< Suggested fix (Nova green)       */
} NovaDiagSeverity;

/* ============================================================
 * ERROR CODES
 *
 * Format: NOVA_E1xxx = compile errors
 *         NOVA_E2xxx = runtime errors
 *         NOVA_E3xxx = I/O / system errors
 *         NOVA_W1xxx = warnings
 *
 * Each code has a catalog entry with a short description and
 * an optional --explain long explanation.
 * ============================================================ */

typedef enum {
    /* No error */
    NOVA_E0000 = 0,

    /* ---- Compile errors (1xxx) ---- */
    NOVA_E1001 = 1001,     /**< Unexpected token                  */
    NOVA_E1002 = 1002,     /**< Undefined variable                */
    NOVA_E1003 = 1003,     /**< Too many local variables          */
    NOVA_E1004 = 1004,     /**< Too many registers                */
    NOVA_E1005 = 1005,     /**< Too many nested scopes            */
    NOVA_E1006 = 1006,     /**< Too many nested functions         */
    NOVA_E1007 = 1007,     /**< Invalid assignment target         */
    NOVA_E1008 = 1008,     /**< Break outside of loop             */
    NOVA_E1009 = 1009,     /**< Duplicate label                   */
    NOVA_E1010 = 1010,     /**< Unresolved goto                   */
    NOVA_E1011 = 1011,     /**< Too many arguments                */
    NOVA_E1012 = 1012,     /**< Too many table fields             */
    NOVA_E1013 = 1013,     /**< Constant overflow                 */
    NOVA_E1014 = 1014,     /**< Expected token                    */
    NOVA_E1015 = 1015,     /**< String interpolation overflow     */
    NOVA_E1016 = 1016,     /**< Expected function after async     */
    NOVA_E1017 = 1017,     /**< Out of memory (compilation)       */
    NOVA_E1018 = 1018,     /**< Too many labels                   */
    NOVA_E1019 = 1019,     /**< Too many gotos                    */
    NOVA_E1020 = 1020,     /**< Module name overflow              */
    NOVA_E1021 = 1021,     /**< Too many parameters               */

    /* ---- Runtime errors (2xxx) ---- */
    NOVA_E2001 = 2001,     /**< Type error (generic)              */
    NOVA_E2002 = 2002,     /**< Stack overflow                    */
    NOVA_E2003 = 2003,     /**< Out of memory (runtime)           */
    NOVA_E2004 = 2004,     /**< Nil dereference                   */
    NOVA_E2005 = 2005,     /**< Division by zero                  */
    NOVA_E2006 = 2006,     /**< Invalid operand                   */
    NOVA_E2007 = 2007,     /**< Index out of bounds               */
    NOVA_E2008 = 2008,     /**< Call of non-function              */
    NOVA_E2009 = 2009,     /**< Wrong number of arguments         */
    NOVA_E2010 = 2010,     /**< Coroutine error                   */
    NOVA_E2011 = 2011,     /**< Metamethod error                  */
    NOVA_E2012 = 2012,     /**< General runtime error             */
    NOVA_E2013 = 2013,     /**< Arithmetic type error             */
    NOVA_E2014 = 2014,     /**< Comparison type error             */
    NOVA_E2015 = 2015,     /**< Concatenation type error          */
    NOVA_E2016 = 2016,     /**< Length type error                  */
    NOVA_E2017 = 2017,     /**< Negation type error               */
    NOVA_E2018 = 2018,     /**< Index on non-table                */
    NOVA_E2019 = 2019,     /**< Invalid key type                  */
    NOVA_E2020 = 2020,     /**< For loop type error               */
    NOVA_E2021 = 2021,     /**< Argument type error               */
    NOVA_E2022 = 2022,     /**< Argument count error              */
    NOVA_E2023 = 2023,     /**< Module not found                  */
    NOVA_E2024 = 2024,     /**< Metamethod chain too deep         */
    NOVA_E2025 = 2025,     /**< Bitwise on non-integer            */

    /* ---- I/O / system errors (3xxx) ---- */
    NOVA_E3001 = 3001,     /**< File not found                    */
    NOVA_E3002 = 3002,     /**< File read error                   */
    NOVA_E3003 = 3003,     /**< Preprocessor error                */
    NOVA_E3004 = 3004,     /**< Parser initialization failed      */
    NOVA_E3005 = 3005,     /**< Network error                     */
    NOVA_E3006 = 3006,     /**< Database error                    */
    NOVA_E3007 = 3007,     /**< Permission denied                 */
    NOVA_E3008 = 3008,     /**< Format/parse error                */

    /* ---- Warnings (W1xxx) ---- */
    NOVA_W1001 = 10001,    /**< Unused variable                   */
    NOVA_W1002 = 10002,    /**< Unreachable code                  */
    NOVA_W1003 = 10003,    /**< Shadowed variable                 */

    NOVA_ERROR_CODE_MAX
} NovaErrorCode;

/* ============================================================
 * DIAGNOSTIC STRUCTURE
 *
 * A single diagnostic message with optional source context.
 * Diagnostics can be chained: an error may have attached
 * notes or help messages.
 * ============================================================ */

typedef struct NovaDiagnostic {
    NovaDiagSeverity  severity;       /**< Error, warning, note, help */
    NovaErrorCode     code;           /**< Error code (0 = none)      */

    /* Location */
    const char       *filename;       /**< Source file path            */
    int               line;           /**< 1-based line number         */
    int               column;         /**< 1-based column number       */
    int               end_column;     /**< End column for underline    */

    /* Message */
    char              message[512];   /**< Formatted message text      */

    /* Source context (optional) */
    const char       *source_line;    /**< The actual source line text  */
    int               source_line_len;/**< Length of source_line        */

    /* Chained sub-diagnostics */
    struct NovaDiagnostic *sub;       /**< Attached note/help (owned)  */
} NovaDiagnostic;

/* ============================================================
 * DIAGNOSTIC CONTEXT
 *
 * Holds the source text for a file so the renderer can extract
 * individual lines for caret display. Create one per file.
 * ============================================================ */

typedef struct {
    const char  *filename;          /**< File path                    */
    const char  *source;            /**< Full source text             */
    size_t       source_len;        /**< Length of source text        */
} NovaDiagSource;

/* ============================================================
 * PUBLIC API
 * ============================================================ */

/**
 * @brief Initialize the diagnostic system.
 *
 * Detects whether stderr is a TTY and enables/disables color
 * accordingly. Call once at program startup before any output.
 *
 * @pre  None
 * @post Color state is initialized based on isatty(STDERR_FILENO)
 */
void nova_diag_init(void);

/**
 * @brief Force color output on or off.
 *
 * Overrides the isatty() auto-detection. Use for --no-color or
 * --color=always CLI flags.
 *
 * @param enabled  1 to enable color, 0 to disable
 */
void nova_diag_set_color(int enabled);

/**
 * @brief Check if color output is currently enabled.
 *
 * @return 1 if color is enabled, 0 otherwise
 */
int nova_diag_color_enabled(void);

/**
 * @brief Set the source text for caret-style diagnostics.
 *
 * The diagnostic renderer uses this to extract individual source
 * lines when displaying errors. The source pointer must remain
 * valid for the lifetime of any diagnostics referencing it.
 *
 * @param src  Pointer to source context (not copied, not owned)
 *
 * @pre  src != NULL
 */
void nova_diag_set_source(const NovaDiagSource *src);

/**
 * @brief Get the currently set source context.
 *
 * @return Pointer to current source context, or NULL if not set
 */
const NovaDiagSource *nova_diag_get_source(void);

/**
 * @brief Emit a diagnostic message to stderr.
 *
 * Renders the diagnostic with colors, source context, caret
 * underlines, and any chained sub-diagnostics (notes/help).
 *
 * Output format (with color):
 *
 *   error[E1001]: unexpected token
 *     --> script.n:10:5
 *      |
 *   10 |     local x = +
 *      |               ^
 *      = help: expected expression after '='
 *
 * @param diag  Diagnostic to emit (must not be NULL)
 *
 * @pre  diag != NULL
 * @post Diagnostic is written to stderr
 */
void nova_diag_emit(const NovaDiagnostic *diag);

/**
 * @brief Create a simple diagnostic with a formatted message.
 *
 * Convenience function that fills a NovaDiagnostic struct from
 * individual parameters. source_line is looked up automatically
 * from the current source context if available.
 *
 * @param diag      Output diagnostic (must not be NULL)
 * @param severity  Diagnostic severity level
 * @param code      Error code (0 for no code)
 * @param filename  Source file path (may be NULL)
 * @param line      1-based line number (0 if unknown)
 * @param column    1-based column number (0 if unknown)
 * @param fmt       printf-style format string
 * @param ...       Format arguments
 *
 * @pre  diag != NULL && fmt != NULL
 * @post diag is filled with the diagnostic information
 */
void nova_diag_create(NovaDiagnostic *diag,
                      NovaDiagSeverity severity,
                      NovaErrorCode code,
                      const char *filename,
                      int line, int column,
                      const char *fmt, ...);

/**
 * @brief Emit a simple diagnostic in one call.
 *
 * Shorthand for nova_diag_create() + nova_diag_emit().
 *
 * @param severity  Diagnostic severity level
 * @param code      Error code (0 for no code)
 * @param filename  Source file path (may be NULL)
 * @param line      1-based line number (0 if unknown)
 * @param column    1-based column number (0 if unknown)
 * @param fmt       printf-style format string
 * @param ...       Format arguments
 */
void nova_diag_report(NovaDiagSeverity severity,
                      NovaErrorCode code,
                      const char *filename,
                      int line, int column,
                      const char *fmt, ...);

/**
 * @brief Attach a sub-diagnostic (note or help) to a parent.
 *
 * The sub-diagnostic is owned by the parent and will be freed
 * when the parent's sub chain is cleaned up.
 *
 * @param parent  Parent diagnostic (must not be NULL)
 * @param sub     Sub-diagnostic to attach (must not be NULL)
 *
 * @pre parent != NULL && sub != NULL
 */
void nova_diag_attach(NovaDiagnostic *parent, NovaDiagnostic *sub);

/**
 * @brief Free a diagnostic's sub-diagnostic chain.
 *
 * Does not free the diagnostic itself (it may be stack-allocated).
 * Only frees heap-allocated sub-diagnostics.
 *
 * @param diag  Diagnostic whose chain to free (may be NULL)
 */
void nova_diag_free(NovaDiagnostic *diag);

/**
 * @brief Look up the short description for an error code.
 *
 * @param code  Error code to look up
 * @return Static string description, or "unknown error" if not found
 */
const char *nova_error_name(NovaErrorCode code);

/**
 * @brief Look up the long explanation for an error code.
 *
 * Used by --explain E1001 to print tutorial-style help.
 *
 * @param code  Error code to look up
 * @return Static string explanation, or NULL if no explanation
 */
const char *nova_error_explain(NovaErrorCode code);

/**
 * @brief Parse an error code string like "E1001" or "e1001".
 *
 * @param str  String to parse
 * @return The error code, or NOVA_E0000 if invalid
 */
NovaErrorCode nova_error_parse_code(const char *str);

/**
 * @brief Extract a single line from source text.
 *
 * Returns a pointer into the source buffer and sets the length.
 * The returned pointer is NOT null-terminated; use the length.
 *
 * @param source      Full source text
 * @param source_len  Length of source text
 * @param line        1-based line number
 * @param out_len     Output: length of the line (excluding newline)
 *
 * @return Pointer to start of line, or NULL if line is out of range
 *
 * @pre source != NULL && out_len != NULL
 */
const char *nova_source_get_line(const char *source, size_t source_len,
                                 int line, int *out_len);

#endif /* NOVA_ERROR_H */
