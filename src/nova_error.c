/**
 * @file nova_error.c
 * @brief Nova Language - Rich Diagnostic Error System (Implementation)
 *
 * ANSI-colored, caret-style diagnostic renderer with error catalog.
 * Provides Rust/Clang-inspired error output for Nova compiler and
 * runtime errors.
 *
 * Output format:
 *
 *   error[E1001]: unexpected token
 *     --> script.n:10:5
 *      |
 *   10 |     local x = +
 *      |               ^
 *      = help: expected expression after '='
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
 *   - nova_error.h
 *   - <stdio.h>, <string.h>, <stdarg.h>, <stdlib.h>
 *   - <unistd.h> (POSIX: isatty)
 *
 * THREAD SAFETY:
 *   Color state and source context are module-global.
 *   Not thread-safe; call nova_diag_init() once at startup.
 */

#include "nova/nova_error.h"

#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <stdlib.h>
#include <ctype.h>
#include <unistd.h>    /* isatty(), STDERR_FILENO */

/* ============================================================
 * MODULE STATE
 * ============================================================ */

static int g_color_enabled = 0;
static const NovaDiagSource *g_source = NULL;

/* ============================================================
 * ERROR CODE CATALOG
 *
 * Each entry has: code, short name, optional long explanation.
 * ============================================================ */

typedef struct {
    NovaErrorCode code;
    const char   *name;
    const char   *explain;
} NovaErrorEntry;

static const NovaErrorEntry g_error_catalog[] = {
    /* Compile errors */
    { NOVA_E1001, "unexpected token",
      "The parser encountered a token that does not fit the current\n"
      "grammar context. This usually means a typo, missing operator,\n"
      "or mismatched brackets.\n\n"
      "Example:\n"
      "  local x = +\n"
      "              ^ unexpected '+' here\n\n"
      "Fix: check the expression and ensure all operators have operands." },

    { NOVA_E1002, "undefined variable",
      "A variable or function name was used that has not been declared\n"
      "in the current scope or any enclosing scope.\n\n"
      "Example:\n"
      "  print(foo)  -- 'foo' was never defined\n\n"
      "Fix: define the variable with 'local' or as a global before use." },

    { NOVA_E1003, "too many local variables",
      "A function exceeded the maximum number of local variables\n"
      "(typically 200). This includes function parameters.\n\n"
      "Fix: refactor the function into smaller functions, or use\n"
      "a table to group related variables." },

    { NOVA_E1004, "too many registers",
      "The compiler ran out of virtual registers (max 250) for a\n"
      "single function. This happens with very complex expressions\n"
      "or deeply nested calls.\n\n"
      "Fix: break complex expressions into intermediate locals." },

    { NOVA_E1005, "too many nested scopes",
      "Exceeded the maximum nesting depth for scopes (blocks, ifs,\n"
      "loops). This prevents stack overflow in the compiler.\n\n"
      "Fix: reduce nesting by extracting helper functions." },

    { NOVA_E1006, "too many nested functions",
      "Exceeded the maximum number of nested function definitions.\n\n"
      "Fix: move inner functions to the module level." },

    { NOVA_E1007, "invalid assignment target",
      "The left side of an assignment is not a valid target.\n"
      "Only variables, table fields, and index expressions can\n"
      "be assigned to.\n\n"
      "Example:\n"
      "  1 + 2 = 3    -- can't assign to an expression\n\n"
      "Fix: ensure the left side is a variable or field access." },

    { NOVA_E1008, "break outside of loop",
      "'break' can only be used inside a for, while, or repeat loop.\n\n"
      "Fix: remove the break or restructure the code to use a loop." },

    { NOVA_E1009, "duplicate label",
      "A label with the same name already exists in this function.\n"
      "Label names must be unique within a function scope.\n\n"
      "Fix: rename one of the duplicate labels." },

    { NOVA_E1010, "unresolved goto",
      "A goto statement references a label that does not exist in\n"
      "the current function. Labels must be defined at the same\n"
      "function level as the goto.\n\n"
      "Fix: define the target label, or check for typos." },

    { NOVA_E1011, "too many arguments",
      "A function call exceeded the maximum number of arguments.\n\n"
      "Fix: pass a table containing the extra values instead." },

    { NOVA_E1012, "too many table fields",
      "A table constructor exceeded the maximum number of fields.\n\n"
      "Fix: build the table incrementally with separate assignments." },

    { NOVA_E1013, "constant overflow",
      "The function's constant pool is full. Each unique string,\n"
      "number, or global name in a function takes one slot.\n\n"
      "Fix: reduce the number of unique constants (e.g., reuse\n"
      "strings via local variables)." },

    { NOVA_E1014, "expected token",
      "The parser expected a specific token (like ')' or 'end')\n"
      "but found something else. This usually indicates a missing\n"
      "closing delimiter.\n\n"
      "Fix: check for unmatched parentheses, brackets, or keywords." },

    { NOVA_E1015, "string interpolation overflow",
      "A template string has too many interpolated ${...} parts.\n\n"
      "Fix: break the string into multiple concatenated parts." },

    { NOVA_E1016, "expected function after async",
      "The 'async' keyword must be followed by 'function'.\n\n"
      "Example:\n"
      "  async function fetch() ... end" },

    { NOVA_E1017, "out of memory (compilation)",
      "The compiler failed to allocate memory for a function\n"
      "prototype or internal structure. This is rare and usually\n"
      "means the system is critically low on memory." },

    { NOVA_E1018, "too many labels",
      "Exceeded the maximum number of labels in a function.\n\n"
      "Fix: reduce the number of labels or extract code into\n"
      "separate functions." },

    { NOVA_E1019, "too many gotos",
      "Exceeded the maximum number of pending goto statements.\n\n"
      "Fix: simplify control flow or use structured loops." },

    { NOVA_E1020, "module name overflow",
      "The module name constant pool is full.\n\n"
      "Fix: reduce the number of require() calls in this function." },

    { NOVA_E1021, "too many parameters",
      "A function definition has too many parameters.\n\n"
      "Fix: pass a table of options instead of many parameters." },

    /* Runtime errors */
    { NOVA_E2001, "type error",
      "An operation was applied to a value of the wrong type.\n\n"
      "Example:\n"
      "  local x = 'hello' + 1  -- can't add string and number\n\n"
      "Fix: check value types before operating, or use tonumber()/\n"
      "tostring() for explicit conversion." },

    { NOVA_E2002, "stack overflow",
      "The call stack exceeded its maximum depth (typically 200\n"
      "nested calls). This usually indicates infinite recursion.\n\n"
      "Fix: add a base case to recursive functions, or convert\n"
      "recursion to iteration." },

    { NOVA_E2003, "out of memory (runtime)",
      "Nova failed to allocate memory during program execution.\n"
      "The garbage collector could not free enough memory.\n\n"
      "Fix: reduce memory usage, check for memory leaks in\n"
      "table references, or increase available system memory." },

    { NOVA_E2004, "nil dereference",
      "Attempted to index or call a nil value.\n\n"
      "Example:\n"
      "  local t = nil\n"
      "  print(t.x)   -- nil has no fields\n\n"
      "Fix: check for nil before indexing." },

    { NOVA_E2005, "division by zero",
      "Attempted to divide or modulo by zero.\n\n"
      "Fix: check the divisor before dividing." },

    { NOVA_E2006, "invalid operand",
      "An arithmetic or comparison operator was applied to an\n"
      "incompatible value type.\n\n"
      "Fix: ensure both operands have compatible types." },

    { NOVA_E2007, "index out of bounds",
      "An array index was outside the valid range [0, length-1].\n\n"
      "Fix: check the index against #array before accessing." },

    { NOVA_E2008, "call of non-function",
      "Attempted to call a value that is not a function or does\n"
      "not have a __call metamethod.\n\n"
      "Fix: verify the value is a function before calling." },

    { NOVA_E2009, "wrong number of arguments",
      "A function received more or fewer arguments than expected.\n\n"
      "Fix: check the function signature and match the call." },

    { NOVA_E2010, "coroutine error",
      "An error related to coroutine state: resuming a dead\n"
      "coroutine, yielding from the main thread, etc.\n\n"
      "Fix: check coroutine.status() before resuming." },

    { NOVA_E2011, "metamethod error",
      "An error occurred inside a metamethod (__add, __index, etc.).\n"
      "The metamethod either raised an error or had incorrect types." },

    { NOVA_E2012, "general runtime error",
      "A catch-all for runtime errors that don't fit other categories.\n"
      "Check the error message for specific details." },

    /* I/O errors */
    { NOVA_E3001, "file not found",
      "The specified file could not be found or opened.\n\n"
      "Fix: check the file path and permissions." },

    { NOVA_E3002, "file read error",
      "An error occurred while reading a file.\n\n"
      "Fix: check file permissions and disk space." },

    { NOVA_E3003, "preprocessor error",
      "The Nova preprocessor encountered an error while\n"
      "processing #include or #define directives." },

    { NOVA_E3004, "parser initialization failed",
      "The parser could not be initialized (out of memory)." },

    /* Warnings */
    { NOVA_W1001, "unused variable",
      "A local variable was declared but never read.\n\n"
      "Fix: remove the variable, or prefix with '_' to indicate\n"
      "it is intentionally unused." },

    { NOVA_W1002, "unreachable code",
      "Code follows a return, break, or goto and will never execute.\n\n"
      "Fix: remove the dead code." },

    { NOVA_W1003, "shadowed variable",
      "A local variable has the same name as one in an outer scope.\n"
      "This can cause confusion.\n\n"
      "Fix: rename the inner variable to avoid ambiguity." },

    /* Sentinel */
    { NOVA_E0000, NULL, NULL }
};

/* ============================================================
 * INITIALIZATION
 * ============================================================ */

void nova_diag_init(void) {
    /* Auto-detect: enable color if stderr is a TTY */
    g_color_enabled = isatty(STDERR_FILENO);

    /* Also check NO_COLOR environment variable (de facto standard) */
    const char *no_color = getenv("NO_COLOR");
    if (no_color != NULL && no_color[0] != '\0') {
        g_color_enabled = 0;
    }
}

void nova_diag_set_color(int enabled) {
    g_color_enabled = (enabled != 0) ? 1 : 0;
}

int nova_diag_color_enabled(void) {
    return g_color_enabled;
}

void nova_diag_set_source(const NovaDiagSource *src) {
    g_source = src;
}

const NovaDiagSource *nova_diag_get_source(void) {
    return g_source;
}

/* ============================================================
 * SOURCE LINE EXTRACTION
 * ============================================================ */

const char *nova_source_get_line(const char *source, size_t source_len,
                                 int line, int *out_len) {
    if (source == NULL || out_len == NULL || line < 1) {
        return NULL;
    }

    int current_line = 1;
    const char *line_start = source;
    const char *end = source + source_len;

    while (line_start < end) {
        if (current_line == line) {
            /* Found the target line - find its end */
            const char *line_end = line_start;
            while (line_end < end && *line_end != '\n' && *line_end != '\r') {
                line_end++;
            }
            *out_len = (int)(line_end - line_start);
            return line_start;
        }

        /* Skip to next line */
        while (line_start < end && *line_start != '\n') {
            line_start++;
        }
        if (line_start < end) {
            line_start++;  /* Skip the \n */
        }
        current_line++;
    }

    /* Line not found */
    *out_len = 0;
    return NULL;
}

/* ============================================================
 * ERROR CODE LOOKUP
 * ============================================================ */

/**
 * @brief Find a catalog entry by error code.
 */
static const NovaErrorEntry *novai_find_entry(NovaErrorCode code) {
    for (int i = 0; g_error_catalog[i].name != NULL; i++) {
        if (g_error_catalog[i].code == code) {
            return &g_error_catalog[i];
        }
    }
    return NULL;
}

const char *nova_error_name(NovaErrorCode code) {
    const NovaErrorEntry *entry = novai_find_entry(code);
    if (entry != NULL) {
        return entry->name;
    }
    return "unknown error";
}

const char *nova_error_explain(NovaErrorCode code) {
    const NovaErrorEntry *entry = novai_find_entry(code);
    if (entry != NULL) {
        return entry->explain;
    }
    return NULL;
}

NovaErrorCode nova_error_parse_code(const char *str) {
    if (str == NULL || str[0] == '\0') {
        return NOVA_E0000;
    }

    /* Skip optional 'E' or 'e' prefix */
    const char *p = str;
    if (*p == 'E' || *p == 'e') {
        p++;
    }
    /* Also skip optional 'W' or 'w' prefix for warnings */
    else if (*p == 'W' || *p == 'w') {
        p++;
    }

    /* Parse the number */
    int code = 0;
    while (*p >= '0' && *p <= '9') {
        code = code * 10 + (*p - '0');
        p++;
    }

    if (*p != '\0') {
        return NOVA_E0000;  /* Trailing garbage */
    }

    /* Check if it was a warning code */
    if (str[0] == 'W' || str[0] == 'w') {
        code += 10000;
    }

    /* Validate the code exists */
    if (novai_find_entry((NovaErrorCode)code) != NULL) {
        return (NovaErrorCode)code;
    }
    return NOVA_E0000;
}

/* ============================================================
 * INTERNAL RENDERING HELPERS
 * ============================================================ */

/**
 * @brief Print a string only if color is enabled.
 */
static void novai_color(FILE *f, const char *code) {
    if (g_color_enabled) {
        fputs(code, f);
    }
}

/**
 * @brief Get the severity label string.
 */
static const char *novai_severity_label(NovaDiagSeverity sev) {
    switch (sev) {
        case NOVA_DIAG_ERROR:   return "error";
        case NOVA_DIAG_WARNING: return "warning";
        case NOVA_DIAG_NOTE:    return "note";
        case NOVA_DIAG_HELP:    return "help";
        default:                return "error";
    }
}

/**
 * @brief Get the ANSI color code for a severity level.
 */
static const char *novai_severity_color(NovaDiagSeverity sev) {
    switch (sev) {
        case NOVA_DIAG_ERROR:   return NOVA_COLOR_ERROR;
        case NOVA_DIAG_WARNING: return NOVA_COLOR_WARNING;
        case NOVA_DIAG_NOTE:    return NOVA_COLOR_NOTE;
        case NOVA_DIAG_HELP:    return NOVA_COLOR_HELP;
        default:                return NOVA_COLOR_ERROR;
    }
}

/**
 * @brief Count digits in a line number (for gutter width).
 */
static int novai_digit_count(int n) {
    if (n < 10) return 1;
    if (n < 100) return 2;
    if (n < 1000) return 3;
    if (n < 10000) return 4;
    return 5;
}

/**
 * @brief Print N spaces.
 */
static void novai_spaces(FILE *f, int count) {
    for (int i = 0; i < count; i++) {
        fputc(' ', f);
    }
}

/* ============================================================
 * MAIN DIAGNOSTIC RENDERER
 *
 * Renders a diagnostic in the following format:
 *
 *   error[E1001]: unexpected token
 *     --> script.n:10:5
 *      |
 *   10 |     local x = +
 *      |               ^
 *      = help: expected expression after '='
 *
 * ============================================================ */

void nova_diag_emit(const NovaDiagnostic *diag) {
    if (diag == NULL) {
        return;
    }

    FILE *f = stderr;
    const char *sev_color = novai_severity_color(diag->severity);
    const char *sev_label = novai_severity_label(diag->severity);

    /* ---- Line 1: severity[code]: message ---- */
    novai_color(f, sev_color);
    fputs(sev_label, f);

    if (diag->code != NOVA_E0000) {
        fputc('[', f);
        /* Format code: E1001 or W1001 */
        if ((int)diag->code >= 10000) {
            fprintf(f, "W%04d", (int)diag->code - 10000);
        } else {
            fprintf(f, "E%04d", (int)diag->code);
        }
        fputc(']', f);
    }

    novai_color(f, NOVA_COLOR_RESET);
    novai_color(f, NOVA_COLOR_BOLD);
    fputs(": ", f);
    fputs(diag->message, f);
    novai_color(f, NOVA_COLOR_RESET);
    fputc('\n', f);

    /* ---- Line 2: --> file:line:col ---- */
    if (diag->filename != NULL && diag->line > 0) {
        int gutter = novai_digit_count(diag->line);
        if (gutter < 2) {
            gutter = 2;
        }

        novai_spaces(f, gutter);
        novai_color(f, NOVA_COLOR_GUTTER);
        fputs("--> ", f);
        novai_color(f, NOVA_COLOR_RESET);

        novai_color(f, NOVA_COLOR_PATH);
        fprintf(f, "%s:%d", diag->filename, diag->line);
        if (diag->column > 0) {
            fprintf(f, ":%d", diag->column);
        }
        novai_color(f, NOVA_COLOR_RESET);
        fputc('\n', f);

        /* ---- Source context lines ---- */
        const char *src_line = diag->source_line;
        int src_len = diag->source_line_len;

        /* Try to get source line from global context if not provided */
        if (src_line == NULL && g_source != NULL && g_source->source != NULL) {
            src_line = nova_source_get_line(
                g_source->source, g_source->source_len,
                diag->line, &src_len
            );
        }

        if (src_line != NULL && src_len > 0) {
            /* Empty gutter line */
            novai_spaces(f, gutter + 1);
            novai_color(f, NOVA_COLOR_GUTTER);
            fputs("|\n", f);
            novai_color(f, NOVA_COLOR_RESET);

            /* Source line: "10 |     local x = +" */
            novai_color(f, NOVA_COLOR_LINENUM);
            fprintf(f, "%*d", gutter, diag->line);
            novai_color(f, NOVA_COLOR_RESET);
            fputc(' ', f);
            novai_color(f, NOVA_COLOR_GUTTER);
            fputs("| ", f);
            novai_color(f, NOVA_COLOR_RESET);

            /* Print source line, replacing tabs with spaces */
            for (int i = 0; i < src_len; i++) {
                if (src_line[i] == '\t') {
                    fputs("    ", f);
                } else {
                    fputc(src_line[i], f);
                }
            }
            fputc('\n', f);

            /* Caret line: "   |               ^" or "^^^" */
            if (diag->column > 0) {
                novai_spaces(f, gutter + 1);
                novai_color(f, NOVA_COLOR_GUTTER);
                fputs("| ", f);

                novai_color(f, sev_color);

                /* Calculate visual column (tabs = 4 spaces) */
                int visual_col = 0;
                for (int i = 0; i < diag->column - 1 && i < src_len; i++) {
                    if (src_line[i] == '\t') {
                        visual_col += 4;
                    } else {
                        visual_col++;
                    }
                }

                novai_spaces(f, visual_col);

                /* Draw carets */
                int caret_count = 1;
                if (diag->end_column > diag->column) {
                    caret_count = diag->end_column - diag->column + 1;
                }
                for (int i = 0; i < caret_count; i++) {
                    fputc('^', f);
                }

                novai_color(f, NOVA_COLOR_RESET);
                fputc('\n', f);
            }
        }
    } else if (diag->filename != NULL) {
        /* File path only, no line number */
        fputs("  ", f);
        novai_color(f, NOVA_COLOR_GUTTER);
        fputs("--> ", f);
        novai_color(f, NOVA_COLOR_RESET);
        novai_color(f, NOVA_COLOR_PATH);
        fputs(diag->filename, f);
        novai_color(f, NOVA_COLOR_RESET);
        fputc('\n', f);
    }

    /* ---- Sub-diagnostics (notes, help) ---- */
    const NovaDiagnostic *sub = diag->sub;
    while (sub != NULL) {
        int gutter = 2;
        if (diag->line > 0) {
            gutter = novai_digit_count(diag->line);
            if (gutter < 2) {
                gutter = 2;
            }
        }

        novai_spaces(f, gutter + 1);
        novai_color(f, NOVA_COLOR_GUTTER);
        fputs("= ", f);
        novai_color(f, novai_severity_color(sub->severity));
        fputs(novai_severity_label(sub->severity), f);
        novai_color(f, NOVA_COLOR_RESET);
        novai_color(f, NOVA_COLOR_BOLD);
        fputs(": ", f);
        novai_color(f, NOVA_COLOR_RESET);
        fputs(sub->message, f);
        fputc('\n', f);

        sub = sub->sub;
    }

    fputc('\n', f);
}

/* ============================================================
 * DIAGNOSTIC CREATION
 * ============================================================ */

void nova_diag_create(NovaDiagnostic *diag,
                      NovaDiagSeverity severity,
                      NovaErrorCode code,
                      const char *filename,
                      int line, int column,
                      const char *fmt, ...) {
    if (diag == NULL || fmt == NULL) {
        return;
    }

    memset(diag, 0, sizeof(*diag));
    diag->severity    = severity;
    diag->code        = code;
    diag->filename    = filename;
    diag->line        = line;
    diag->column      = column;
    diag->end_column  = 0;
    diag->source_line = NULL;
    diag->source_line_len = 0;
    diag->sub         = NULL;

    va_list args;
    va_start(args, fmt);
    (void)vsnprintf(diag->message, sizeof(diag->message), fmt, args);
    va_end(args);

    /* Try to look up source line from global context */
    if (g_source != NULL && g_source->source != NULL && line > 0) {
        diag->source_line = nova_source_get_line(
            g_source->source, g_source->source_len,
            line, &diag->source_line_len
        );
    }
}

void nova_diag_report(NovaDiagSeverity severity,
                      NovaErrorCode code,
                      const char *filename,
                      int line, int column,
                      const char *fmt, ...) {
    if (fmt == NULL) {
        return;
    }

    NovaDiagnostic diag;
    memset(&diag, 0, sizeof(diag));
    diag.severity = severity;
    diag.code     = code;
    diag.filename = filename;
    diag.line     = line;
    diag.column   = column;

    va_list args;
    va_start(args, fmt);
    (void)vsnprintf(diag.message, sizeof(diag.message), fmt, args);
    va_end(args);

    /* Look up source line */
    if (g_source != NULL && g_source->source != NULL && line > 0) {
        diag.source_line = nova_source_get_line(
            g_source->source, g_source->source_len,
            line, &diag.source_line_len
        );
    }

    nova_diag_emit(&diag);
}

/* ============================================================
 * SUB-DIAGNOSTIC MANAGEMENT
 * ============================================================ */

void nova_diag_attach(NovaDiagnostic *parent, NovaDiagnostic *sub) {
    if (parent == NULL || sub == NULL) {
        return;
    }

    /* Append to end of chain */
    if (parent->sub == NULL) {
        parent->sub = sub;
    } else {
        NovaDiagnostic *tail = parent->sub;
        while (tail->sub != NULL) {
            tail = tail->sub;
        }
        tail->sub = sub;
    }
}

void nova_diag_free(NovaDiagnostic *diag) {
    if (diag == NULL) {
        return;
    }

    NovaDiagnostic *sub = diag->sub;
    while (sub != NULL) {
        NovaDiagnostic *next = sub->sub;
        free(sub);
        sub = next;
    }
    diag->sub = NULL;
}
