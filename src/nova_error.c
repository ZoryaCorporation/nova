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
 *   - zorya/pal.h (platform abstraction for terminal detection)
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
#include <zorya/pal.h>  /* zorya_isatty() — portable terminal detection */

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
      "  dec x = +\n"
      "              ^ unexpected '+' here\n\n"
      "Fix: check the expression and ensure all operators have operands." },

    { NOVA_E1002, "undefined variable",
      "A variable or function name was used that has not been declared\n"
      "in the current scope or any enclosing scope.\n\n"
      "Example:\n"
      "  print(foo)  -- 'foo' was never defined\n\n"
      "Fix: define the variable with 'dec' or as a global before use." },

    { NOVA_E1003, "too many dec variables",
      "A function exceeded the maximum number of dec variables\n"
      "(typically 200). This includes function parameters.\n\n"
      "Fix: refactor the function into smaller functions, or use\n"
      "a table to group related variables." },

    { NOVA_E1004, "too many registers",
      "The compiler ran out of virtual registers (max 250) for a\n"
      "single function. This happens with very complex expressions\n"
      "or deeply nested calls.\n\n"
      "Fix: break complex expressions into intermediate decs." },

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
      "strings via dec variables)." },

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
      "  dec x = 'hello' + 1  -- can't add string and number\n\n"
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
      "  dec t = nil\n"
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

    { NOVA_E2013, "arithmetic type error",
      "An arithmetic operator (+, -, *, /, //, %, ^) was applied to\n"
      "a value that is not a number and has no arithmetic metamethod.\n\n"
      "Example:\n"
      "  dec x = 'hello' + 1\n"
      "            ^^^^^^^ string cannot be added\n\n"
      "The error message shows the exact operator and the types of\n"
      "both operands so you can identify the culprit.\n\n"
      "Fix: convert with tonumber(), or define a __add / __sub / etc.\n"
      "metamethod on the value's metatable." },

    { NOVA_E2014, "comparison type error",
      "A relational operator (<, <=) was applied to values of\n"
      "incompatible types that have no comparison metamethod.\n\n"
      "Example:\n"
      "  if 'hello' < 42 then ... end\n"
      "     ^^^^^^    ^^ string vs number\n\n"
      "Note: == and != work on any types (different types are never\n"
      "equal), but < and <= require same-type or a metamethod.\n\n"
      "Fix: ensure both sides are the same type, or define __lt / __le\n"
      "metamethods." },

    { NOVA_E2015, "concatenation type error",
      "The .. operator requires string or number operands, but\n"
      "received an incompatible type with no __concat metamethod.\n\n"
      "Example:\n"
      "  dec x = 'hello ' .. true\n"
      "                        ^^^^ boolean cannot be concatenated\n\n"
      "Fix: use tostring() to convert, or define a __concat metamethod." },

    { NOVA_E2016, "length type error",
      "The # (length) operator was applied to a value that is not\n"
      "a string or table and has no __len metamethod.\n\n"
      "Example:\n"
      "  dec n = #42\n"
      "             ^^ number has no length\n\n"
      "Fix: ensure the value is a string or table, or define __len." },

    { NOVA_E2017, "negation type error",
      "The unary minus (-) operator was applied to a non-number\n"
      "value with no __unm metamethod.\n\n"
      "Example:\n"
      "  dec x = -'hello'\n"
      "             ^^^^^^^ string cannot be negated\n\n"
      "Fix: use tonumber() or define __unm on the value's metatable." },

    { NOVA_E2018, "index on non-table",
      "Attempted to use the index operator ([] or .) on a value\n"
      "that is not a table and has no __index or __newindex metamethod.\n\n"
      "Example:\n"
      "  dec x = 42\n"
      "  print(x.name)  -- number has no fields\n"
      "        ^^^^^^ attempt to index a number value\n\n"
      "Common causes:\n"
      "  - A function returned nil instead of a table\n"
      "  - A variable was overwritten with a non-table value\n"
      "  - Forgot to initialize a table with {}\n\n"
      "Fix: check that the value is a table before indexing. Use\n"
      "type(v) == 'table' to verify, or initialize with {}." },

    { NOVA_E2019, "invalid key type",
      "A table key must be a string, integer, or numeric value\n"
      "convertible to integer. Other types (nil, boolean, table,\n"
      "function) are not valid keys.\n\n"
      "Example:\n"
      "  dec t = {}\n"
      "  t[true] = 1   -- boolean is not a valid key\n\n"
      "Fix: convert the key to a string with tostring()." },

    { NOVA_E2020, "for loop type error",
      "A numeric for loop requires its init, limit, and step values\n"
      "to be numbers. A non-numeric value was provided.\n\n"
      "Example:\n"
      "  for i = 'a', 10 do end\n"
      "          ^^^ string is not a number\n\n"
      "Fix: ensure all three for-loop parameters are numbers.\n"
      "Use tonumber() if converting from strings." },

    { NOVA_E2021, "argument type error",
      "A standard library function received an argument of the\n"
      "wrong type. The error message shows which argument and\n"
      "what type was expected vs. received.\n\n"
      "Example:\n"
      "  string.upper(42)\n"
      "  bad argument #1 to 'upper' (string expected, got integer)\n\n"
      "Fix: check the function signature and pass the correct type.\n"
      "Use type() to inspect values before passing them." },

    { NOVA_E2022, "argument count error",
      "A function was called with too many or too few arguments.\n\n"
      "Fix: check the function's expected parameter count." },

    { NOVA_E2023, "module not found",
      "require() could not locate the specified module.\n"
      "Nova searches package.path for .n and .no files.\n\n"
      "Fix: verify the module name, check package.path, and ensure\n"
      "the file exists in one of the search directories." },

    { NOVA_E2024, "metamethod chain too deep",
      "A __index or __newindex chain exceeded the maximum depth.\n"
      "This usually means a circular metatable reference.\n\n"
      "Example:\n"
      "  dec a, b = {}, {}\n"
      "  setmetatable(a, { __index = b })\n"
      "  setmetatable(b, { __index = a })  -- loop!\n\n"
      "Fix: break the circular reference in your metatable chain." },

    { NOVA_E2025, "bitwise on non-integer",
      "A bitwise operator (&, |, ~, <<, >>) was applied to a\n"
      "non-integer value. Bitwise operations only work on integers.\n\n"
      "Example:\n"
      "  dec x = 3.14 & 0xFF\n"
      "            ^^^^ float cannot be used in bitwise op\n\n"
      "Fix: use math.floor() or math.tointeger() to convert to\n"
      "an integer first." },

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

    { NOVA_E3005, "network error",
      "An error occurred during a network operation (HTTP request,\n"
      "socket I/O, etc.). The underlying error from libcurl or the\n"
      "network stack is included in the message.\n\n"
      "Fix: check network connectivity, verify the URL, and ensure\n"
      "the server is reachable." },

    { NOVA_E3006, "database error",
      "An error occurred during a SQLite database operation.\n"
      "The underlying SQLite error message is included.\n\n"
      "Fix: check the SQL syntax, verify the database path, and\n"
      "ensure the database file is not locked or corrupt." },

    { NOVA_E3007, "permission denied",
      "A filesystem operation failed due to insufficient permissions.\n\n"
      "Fix: check file/directory permissions with fs.stat() and\n"
      "ensure the process has the necessary access rights." },

    { NOVA_E3008, "format/parse error",
      "An error occurred while parsing or encoding structured data\n"
      "(JSON, CSV, NINI, TOML, etc.). The format-specific error\n"
      "message describes the issue.\n\n"
      "Fix: validate the input data format before parsing." },

    /* Warnings */
    { NOVA_W1001, "unused variable",
      "A dec variable was declared but never read.\n\n"
      "Fix: remove the variable, or prefix with '_' to indicate\n"
      "it is intentionally unused." },

    { NOVA_W1002, "unreachable code",
      "Code follows a return, break, or goto and will never execute.\n\n"
      "Fix: remove the dead code." },

    { NOVA_W1003, "shadowed variable",
      "A dec variable has the same name as one in an outer scope.\n"
      "This can cause confusion.\n\n"
      "Fix: rename the inner variable to avoid ambiguity." },

    /* Sentinel */
    { NOVA_E0000, NULL, NULL }
};

/* ============================================================
 * INITIALIZATION
 * ============================================================ */

void nova_diag_init(void) {
    /* Colors ON by default — Nova embraces a colorful terminal.
     * Only disable for explicit NO_COLOR or --no-color.            */
    g_color_enabled = 1;

    /* Respect the NO_COLOR environment variable (https://no-color.org) */
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
 * MESSAGE COLORING HELPER
 *
 * Parses diagnostic messages and colorizes:
 *   'token' after "expected" → Nova green (the good/wanted thing)
 *   'token' after "got" or "unexpected token" → red (the bad thing)
 * ============================================================ */

/**
 * @brief Emit diagnostic message with colored expected/got tokens.
 *
 * Scans for patterns: expected 'X', got 'Y', unexpected token 'Z'
 * and wraps the quoted tokens in NOVA_COLOR_EXPECTED (green) or
 * NOVA_COLOR_GOT (red).
 */
static void novai_emit_colored_msg(FILE *f, const char *msg) {
    if (msg == NULL) { return; }
    if (!g_color_enabled) { fputs(msg, f); return; }

    /* State machine for expected/got coloring */
    typedef enum { NEUTRAL, WANT_GREEN, WANT_RED } QState;
    QState state = NEUTRAL;
    const char *p = msg;

    while (*p != '\0') {
        /* "expected " → next quoted thing is green */
        if (strncmp(p, "expected ", 9) == 0) {
            fwrite(p, 1, 9, f);
            p += 9;
            state = WANT_GREEN;
            continue;
        }
        /* "unexpected token " → next quoted thing is red */
        if (strncmp(p, "unexpected token ", 17) == 0) {
            fwrite(p, 1, 17, f);
            p += 17;
            state = WANT_RED;
            continue;
        }
        /* "got " → next quoted thing is red */
        if (strncmp(p, "got ", 4) == 0) {
            fwrite(p, 1, 4, f);
            p += 4;
            state = WANT_RED;
            continue;
        }

        /* Quoted or bracketed segment → apply current color state */
        if (state != NEUTRAL && (*p == '\'' || *p == '<')) {
            char close = (*p == '\'') ? '\'' : '>';
            const char *end = strchr(p + 1, close);
            if (end != NULL) {
                const char *qcolor = (state == WANT_GREEN)
                    ? NOVA_COLOR_EXPECTED : NOVA_COLOR_GOT;
                novai_color(f, qcolor);
                fwrite(p, 1, (size_t)(end - p + 1), f);
                novai_color(f, NOVA_COLOR_RESET);
                novai_color(f, NOVA_COLOR_PATH);
                p = end + 1;
                state = NEUTRAL;
                continue;
            }
        }

        fputc(*p, f);
        p++;
    }
}

/* ============================================================
 * TOKEN LENGTH ESTIMATOR
 *
 * Guesses the length of the token at a given source column so
 * the caret line can underline the full token, not just one char.
 * ============================================================ */

/**
 * @brief Estimate token length at a 0-based column position.
 */
static int novai_token_length(const char *src, int src_len, int col0) {
    if (src == NULL || col0 < 0 || col0 >= src_len) {
        return 1;
    }
    unsigned char ch = (unsigned char)src[col0];

    /* Identifier or keyword: [A-Za-z_][A-Za-z0-9_]* */
    if (isalpha(ch) || ch == '_') {
        int len = 1;
        while (col0 + len < src_len) {
            unsigned char c = (unsigned char)src[col0 + len];
            if (isalnum(c) || c == '_') { len++; } else { break; }
        }
        return len;
    }
    /* Number literal */
    if (isdigit(ch)) {
        int len = 1;
        while (col0 + len < src_len &&
               (isdigit((unsigned char)src[col0 + len]) ||
                src[col0 + len] == '.')) {
            len++;
        }
        return len;
    }
    /* String literal */
    if (ch == '"' || ch == '\'') {
        int len = 1;
        while (col0 + len < src_len &&
               (unsigned char)src[col0 + len] != ch) {
            len++;
        }
        if (col0 + len < src_len) { len++; } /* closing quote */
        return len;
    }
    /* Two-char operators */
    if (col0 + 1 < src_len) {
        char c2 = src[col0 + 1];
        if ((ch == '=' && c2 == '=') || (ch == '~' && c2 == '=') ||
            (ch == '<' && c2 == '=') || (ch == '>' && c2 == '=') ||
            (ch == '.' && c2 == '.')) {
            return 2;
        }
    }
    return 1;
}

/* ============================================================
 * MAIN DIAGNOSTIC RENDERER
 *
 * Nova's signature retro-modern TUI error display.
 * 1980s Nova-green pipes, context window, token highlighting,
 * and dashes+carets pointing to the error.
 *
 *   [E1014]: expected ')' after parameters, got 'return'
 *     FILE: script.n  Ln 10, Col 5
 *         │
 *    8    │ -- comment
 *    9    │ local function calculate(a, b
 *   10    │     return a + b
 *         │ ────^^^^^^
 *   11    │ end
 *
 * ============================================================ */

/** Context window: lines shown before/after the error line. */
#define NOVAI_CTX_BEFORE 3
#define NOVAI_CTX_AFTER  1

void nova_diag_emit(const NovaDiagnostic *diag) {
    if (diag == NULL) {
        return;
    }

    FILE *f = stderr;
    const char *sev_color = novai_severity_color(diag->severity);

    /* ──────────────────────────────────────────────────────────
     * Line 1: [CODE]: message
     *
     * For errors, drop the "error" prefix — the code is enough.
     * For warnings/notes/help, keep the severity label.
     * ────────────────────────────────────────────────────────── */
    if (diag->code != NOVA_E0000) {
        if (diag->severity != NOVA_DIAG_ERROR) {
            novai_color(f, sev_color);
            novai_color(f, NOVA_COLOR_BOLD);
            fputs(novai_severity_label(diag->severity), f);
        }
        novai_color(f, NOVA_COLOR_RESET);
        novai_color(f, NOVA_COLOR_ERRCODE);
        fputc('[', f);
        if ((int)diag->code >= 10000) {
            fprintf(f, "W%04d", (int)diag->code - 10000);
        } else {
            fprintf(f, "E%04d", (int)diag->code);
        }
        fputc(']', f);
    } else {
        novai_color(f, sev_color);
        novai_color(f, NOVA_COLOR_BOLD);
        fputs(novai_severity_label(diag->severity), f);
    }

    novai_color(f, NOVA_COLOR_RESET);
    novai_color(f, NOVA_COLOR_BOLD);
    fputs(": ", f);
    novai_color(f, NOVA_COLOR_PATH);
    novai_emit_colored_msg(f, diag->message);
    novai_color(f, NOVA_COLOR_RESET);
    fputc('\n', f);

    /* ──────────────────────────────────────────────────────────
     * Line 2: FILE: path  Ln X, Col Y
     * ────────────────────────────────────────────────────────── */
    if (diag->filename != NULL) {
        fputs("  ", f);
        novai_color(f, NOVA_COLOR_MUTED);
        fputs("FILE: ", f);
        novai_color(f, NOVA_COLOR_PATH);
        fputs(diag->filename, f);

        if (diag->line > 0) {
            novai_color(f, NOVA_COLOR_RESET);
            fputs("  ", f);
            novai_color(f, NOVA_COLOR_LNCOL);
            fprintf(f, "Ln %d", diag->line);
            if (diag->column > 0) {
                fprintf(f, ", Col %d", diag->column);
            }
        }
        novai_color(f, NOVA_COLOR_RESET);
        fputc('\n', f);
    }

    /* ──────────────────────────────────────────────────────────
     * Source context block with surrounding lines
     * ────────────────────────────────────────────────────────── */
    if (diag->filename != NULL && diag->line > 0) {
        /* Determine context window */
        int first_line = diag->line - NOVAI_CTX_BEFORE;
        if (first_line < 1) { first_line = 1; }
        int last_line = diag->line + NOVAI_CTX_AFTER;

        /* Gutter width from largest line number */
        int gutter = novai_digit_count(last_line);
        if (gutter < 2) { gutter = 2; }
        int pipe_col = gutter + 3;

        /* Check if source text is available */
        int have_global_src = (g_source != NULL && g_source->source != NULL);
        int have_diag_src = (diag->source_line != NULL &&
                             diag->source_line_len > 0);

        if (have_global_src || have_diag_src) {
            /* Empty pipe line (separator) */
            novai_spaces(f, pipe_col);
            novai_color(f, NOVA_COLOR_PIPE);
            fputs("\xe2\x94\x82\n", f);  /* │ */
            novai_color(f, NOVA_COLOR_RESET);

            /* Iterate context lines */
            for (int ln = first_line; ln <= last_line; ln++) {
                int line_len = 0;
                const char *line_text = NULL;

                /* Get source line text */
                if (have_global_src) {
                    line_text = nova_source_get_line(
                        g_source->source, g_source->source_len,
                        ln, &line_len);
                } else if (ln == diag->line && have_diag_src) {
                    line_text = diag->source_line;
                    line_len = diag->source_line_len;
                }

                if (line_text == NULL) { continue; }

                int is_err = (ln == diag->line);

                /* Line number: error line in red, others in muted */
                if (is_err) {
                    novai_color(f, NOVA_COLOR_ERROR);
                } else {
                    novai_color(f, NOVA_COLOR_MUTED);
                }
                fprintf(f, "%*d", gutter, ln);
                novai_color(f, NOVA_COLOR_RESET);

                /* Gap + pipe */
                fputs("   ", f);
                novai_color(f, NOVA_COLOR_PIPE);
                fputs("\xe2\x94\x82 ", f);  /* │ */
                novai_color(f, NOVA_COLOR_RESET);

                /* Source text */
                if (is_err && diag->column > 0) {
                    /* Error line: highlight the error token */
                    int col0 = diag->column - 1;
                    int tok_len = novai_token_length(
                        line_text, line_len, col0);

                    /* Text before error token */
                    novai_color(f, NOVA_COLOR_SOURCE);
                    for (int i = 0; i < col0 && i < line_len; i++) {
                        if (line_text[i] == '\t') { fputs("    ", f); }
                        else { fputc(line_text[i], f); }
                    }

                    /* Error token in severity color (bold red) */
                    novai_color(f, sev_color);
                    novai_color(f, NOVA_COLOR_BOLD);
                    for (int i = col0;
                         i < col0 + tok_len && i < line_len; i++) {
                        fputc(line_text[i], f);
                    }
                    novai_color(f, NOVA_COLOR_RESET);

                    /* Text after error token */
                    novai_color(f, NOVA_COLOR_SOURCE);
                    for (int i = col0 + tok_len; i < line_len; i++) {
                        if (line_text[i] == '\t') { fputs("    ", f); }
                        else { fputc(line_text[i], f); }
                    }
                    novai_color(f, NOVA_COLOR_RESET);
                } else {
                    /* Context line: dimmed source */
                    novai_color(f, is_err ? NOVA_COLOR_SOURCE
                                         : NOVA_COLOR_CTXLINE);
                    for (int i = 0; i < line_len; i++) {
                        if (line_text[i] == '\t') { fputs("    ", f); }
                        else { fputc(line_text[i], f); }
                    }
                    novai_color(f, NOVA_COLOR_RESET);
                }
                fputc('\n', f);

                /* ── Caret/underline line after the error line ── */
                if (is_err && diag->column > 0) {
                    novai_spaces(f, pipe_col);
                    novai_color(f, NOVA_COLOR_PIPE);
                    fputs("\xe2\x94\x82 ", f);  /* │ */

                    novai_color(f, sev_color);

                    int col0 = diag->column - 1;
                    int tok_len = novai_token_length(
                        line_text, line_len, col0);

                    /* Visual column (tabs = 4 spaces) */
                    int vis_col = 0;
                    for (int i = 0; i < col0 && i < line_len; i++) {
                        if (line_text[i] == '\t') { vis_col += 4; }
                        else { vis_col++; }
                    }

                    /* Dashes leading up to error position */
                    for (int i = 0; i < vis_col; i++) {
                        /* box-drawing ─ (U+2500) */
                        fputs("\xe2\x94\x80", f);
                    }

                    /* Carets on the error token */
                    int carets = tok_len;
                    if (diag->end_column > diag->column) {
                        carets = diag->end_column - diag->column + 1;
                    }
                    if (carets < 1) { carets = 1; }
                    for (int i = 0; i < carets; i++) {
                        fputc('^', f);
                    }

                    novai_color(f, NOVA_COLOR_RESET);
                    fputc('\n', f);
                }
            }
        }
    }

    /* ──────────────────────────────────────────────────────────
     * Sub-diagnostics (notes, help)
     * ────────────────────────────────────────────────────────── */
    const NovaDiagnostic *sub = diag->sub;
    while (sub != NULL) {
        int gutter = 2;
        if (diag->line > 0) {
            int last = diag->line + NOVAI_CTX_AFTER;
            gutter = novai_digit_count(last);
            if (gutter < 2) { gutter = 2; }
        }
        int pipe_col = gutter + 3;

        novai_spaces(f, pipe_col);
        novai_color(f, NOVA_COLOR_PIPE);
        fputs("\xe2\x95\xb0\xe2\x94\x80\xe2\x94\x80 ", f);  /* ╰── */
        novai_color(f, novai_severity_color(sub->severity));
        novai_color(f, NOVA_COLOR_BOLD);
        fputs(novai_severity_label(sub->severity), f);
        novai_color(f, NOVA_COLOR_RESET);
        novai_color(f, NOVA_COLOR_BOLD);
        fputs(": ", f);
        novai_color(f, NOVA_COLOR_RESET);
        novai_color(f, NOVA_COLOR_MUTED);
        fputs(sub->message, f);
        novai_color(f, NOVA_COLOR_RESET);
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
