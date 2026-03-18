/**
 * @file nova_suggest.c
 * @brief Nova Language - NLP-Inspired Error Intelligence Engine
 *
 * Contextual "did you mean?" suggestions and help hints for
 * runtime errors.  Draws on Natural Language Processing techniques:
 *
 *   - Levenshtein edit distance for fuzzy name matching
 *   - KWIC (Key Word In Context) source context analysis
 *   - Pattern-based error classification
 *   - N-gram style anti-pattern detection
 *
 * Architecture:
 *
 *   1. novai_levenshtein()       — O(min(m,n)) edit distance
 *   2. nova_suggest_name()       — best fuzzy match from candidates
 *   3. novai_extract_*()         — KWIC: extract identifiers from source
 *   4. novai_hint_*()            — per-error-class hint generators
 *   5. novai_line_contains()     — n-gram pattern detector
 *   6. nova_suggest_runtime_hints() — main dispatch
 *
 * The engine is a pure diagnostic utility — it depends only on
 * nova_error.h and standard C, not on the VM.
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

#include "nova/nova_suggest.h"
#include "nova/nova_error.h"

#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>

/* ============================================================
 * CONSTANTS
 * ============================================================ */

/** Maximum Levenshtein distance for "did you mean?" */
#define SUGGEST_MAX_DISTANCE  3

/** Maximum identifier length for extraction */
#define SUGGEST_MAX_NAME      64

/** Maximum identifiers to extract from a single source line */
#define SUGGEST_MAX_NAMES     8

/* ============================================================
 * STANDARD NAME DICTIONARIES
 *
 * Known global functions, module names, and method names that
 * serve as candidates for fuzzy matching.
 *
 * Organized by category so each hint generator can target the
 * right dictionary: call errors search globals, method errors
 * search methods, require errors search module names, etc.
 * ============================================================ */

/** Top-level global functions and module tables. */
static const char *novai_global_names[] = {
    /* base functions */
    "echo", "print", "printf", "sprintf", "fprintf",
    "type", "tostring", "tonumber",
    "error", "assert", "pcall", "xpcall",
    "rawget", "rawset", "rawlen", "rawequal",
    "select", "pairs", "ipairs", "next", "unpack",
    "require", "dofile", "load",
    "setmetatable", "getmetatable",
    /* module tables */
    "math", "string", "table", "io", "os", "fs",
    "coroutine", "debug", "tools", "async",
    "package", "net", "sql", "nlp", "data",
    NULL
};

/** Method / field names available on modules and metatables. */
static const char *novai_method_names[] = {
    /* string methods */
    "len", "sub", "upper", "lower", "rep",
    "find", "format", "gsub", "match", "gmatch",
    "byte", "char", "reverse",
    /* table methods */
    "insert", "remove", "sort", "concat", "move",
    "pack", "unpack",
    /* math functions */
    "abs", "ceil", "floor", "sqrt",
    "sin", "cos", "tan", "log", "exp",
    "random", "max", "min",
    /* io methods */
    "open", "close", "read", "write", "lines",
    /* os methods */
    "execute", "capture", "getenv", "setenv",
    "clock", "time", "date", "sleep", "platform",
    /* fs methods */
    "exists", "isfile", "isdir", "list", "walk",
    "glob", "mkdir", "copy", "stat",
    /* coroutine methods */
    "create", "resume", "yield", "wrap", "status",
    /* debug methods */
    "traceback", "getinfo", "getlocal", "sethook",
    /* nlp methods */
    "tokenize", "stem", "stems", "distance", "similarity",
    "fuzzy", "freq", "tfidf", "ngrams", "kwic",
    "sentences", "summarize", "normalize", "wordcount", "unique",
    /* common across modules */
    "tostring", "tonumber", "type",
    NULL
};

/** Known importable module names (for require / #import). */
static const char *novai_module_names[] = {
    "math", "string", "table", "io", "os", "fs",
    "coroutine", "debug", "tools", "async",
    "net", "sql", "nlp", "data",
    "json", "csv", "nini", "package",
    NULL
};

/* ============================================================
 * INTERNAL: LEVENSHTEIN EDIT DISTANCE
 *
 * Classic DP with O(min(m,n)) space — one-row optimization.
 * Case-insensitive comparison.  Lifted from the NLP library's
 * nlpi_levenshtein() and adapted for the diagnostic engine.
 * ============================================================ */

static int novai_levenshtein(const char *a, int la,
                             const char *b, int lb) {
    /* Swap so 'a' is the shorter string (less memory). */
    if (la > lb) {
        const char *t = a; a = b; b = t;
        int ti = la; la = lb; lb = ti;
    }

    int *row = (int *)malloc(sizeof(int) * (size_t)(la + 1));
    if (row == NULL) { return 999; }

    for (int i = 0; i <= la; i++) { row[i] = i; }

    for (int j = 1; j <= lb; j++) {
        int prev = row[0];
        row[0] = j;
        for (int i = 1; i <= la; i++) {
            int cost = (tolower((unsigned char)a[i - 1]) ==
                        tolower((unsigned char)b[j - 1])) ? 0 : 1;
            int ins = row[i] + 1;
            int del = row[i - 1] + 1;
            int sub = prev + cost;
            prev = row[i];

            int best = ins < del ? ins : del;
            if (sub < best) { best = sub; }
            row[i] = best;
        }
    }

    int result = row[la];
    free(row);
    return result;
}

/* ============================================================
 * PUBLIC: FUZZY NAME MATCHER
 * ============================================================ */

const char *nova_suggest_name(const char *input, int input_len,
                              const char **candidates, int count,
                              int max_distance) {
    if (input == NULL || input_len <= 0 || candidates == NULL) {
        return NULL;
    }

    const char *best = NULL;
    int best_dist = max_distance + 1;
    int limit = (count < 0) ? 100000 : count;

    for (int i = 0; i < limit && candidates[i] != NULL; i++) {
        int cand_len = (int)strlen(candidates[i]);

        /* Adaptive threshold: short names need closer matches.
         * 1-3 chars → max 1, 4-5 chars → max 2, 6+ → full limit. */
        int thresh = max_distance;
        if (input_len <= 3) { thresh = 1; }
        else if (input_len <= 5) { thresh = thresh > 2 ? 2 : thresh; }

        /* Quick reject: lengths differ too much. */
        int len_diff = input_len - cand_len;
        if (len_diff < 0) { len_diff = -len_diff; }
        if (len_diff > thresh) { continue; }

        /* Skip exact matches (the user didn't misspell this one). */
        if (input_len == cand_len) {
            int eq = 1;
            for (int c = 0; c < input_len; c++) {
                if (tolower((unsigned char)input[c]) !=
                    tolower((unsigned char)candidates[i][c])) {
                    eq = 0;
                    break;
                }
            }
            if (eq) { continue; }
        }

        int d = novai_levenshtein(input, input_len,
                                  candidates[i], cand_len);
        if (d <= thresh && d < best_dist) {
            best_dist = d;
            best = candidates[i];
        }
    }

    return best;
}

/* ============================================================
 * INTERNAL: SOURCE LINE ANALYSIS  (KWIC-INSPIRED)
 *
 * These helpers extract identifiers and detect patterns in the
 * source line surrounding an error — the "Key Word In Context"
 * approach applied to code rather than prose.
 * ============================================================ */

/**
 * @brief Check if a source line contains a substring.
 *
 * Used for n-gram style anti-pattern detection: specific
 * character sequences in the source imply specific mistakes.
 */
static int novai_line_contains(const char *line, int len,
                               const char *pat) {
    if (line == NULL || pat == NULL) { return 0; }
    int plen = (int)strlen(pat);
    if (plen > len || plen == 0) { return 0; }
    for (int i = 0; i <= len - plen; i++) {
        if (memcmp(line + i, pat, (size_t)plen) == 0) { return 1; }
    }
    return 0;
}

/**
 * @brief Extract identifiers that appear before '(' in the source.
 *
 * These are likely function/method call targets.  For a line like
 * `prnt("hello")`, extracts "prnt".  For `math.sqr(x)`, extracts
 * both "math" and "sqr".
 */
static int novai_extract_call_names(const char *line, int len,
                                    char names[][SUGGEST_MAX_NAME],
                                    int max_names) {
    int count = 0;

    for (int i = 0; i < len && count < max_names; i++) {
        if (line[i] != '(') { continue; }

        /* Walk backwards past whitespace */
        int j = i - 1;
        while (j >= 0 && line[j] == ' ') { j--; }
        if (j < 0) { continue; }

        /* Walk backwards through identifier chars */
        int end = j + 1;
        while (j >= 0 && (isalnum((unsigned char)line[j]) ||
                          line[j] == '_')) {
            j--;
        }
        int start = j + 1;

        if (start < end) {
            int nlen = end - start;
            if (nlen >= SUGGEST_MAX_NAME) { nlen = SUGGEST_MAX_NAME - 1; }
            memcpy(names[count], line + start, (size_t)nlen);
            names[count][nlen] = '\0';
            count++;
        }
    }

    return count;
}

/**
 * @brief Extract field/method names after '.' or ':' in the source.
 *
 * For `s:uper()`, extracts "uper".  For `math.sqr(x)`, extracts "sqr".
 * These are likely method-call or field-access targets.
 */
static int novai_extract_method_names(const char *line, int len,
                                      char names[][SUGGEST_MAX_NAME],
                                      int max_names) {
    int count = 0;

    for (int i = 0; i < len - 1 && count < max_names; i++) {
        if (line[i] != '.' && line[i] != ':') { continue; }

        int j = i + 1;
        if (j >= len) { continue; }
        if (!isalpha((unsigned char)line[j]) && line[j] != '_') {
            continue;
        }

        int start = j;
        while (j < len && (isalnum((unsigned char)line[j]) ||
                           line[j] == '_')) {
            j++;
        }

        int nlen = j - start;
        if (nlen >= SUGGEST_MAX_NAME) { nlen = SUGGEST_MAX_NAME - 1; }
        memcpy(names[count], line + start, (size_t)nlen);
        names[count][nlen] = '\0';
        count++;
    }

    return count;
}

/* ============================================================
 * INTERNAL: HINT BUILDER
 *
 * Writes a formatted sub-diagnostic into the hints array.
 * Returns the new hint count.
 * ============================================================ */

static int novai_add_hint(NovaDiagnostic *hints, int idx, int max,
                          NovaDiagSeverity sev,
                          const char *fmt, ...) {
    if (idx >= max) { return idx; }

    memset(&hints[idx], 0, sizeof(hints[idx]));
    hints[idx].severity = sev;
    hints[idx].code     = NOVA_E0000;

    va_list ap;
    va_start(ap, fmt);
    (void)vsnprintf(hints[idx].message, sizeof(hints[idx].message),
                    fmt, ap);
    va_end(ap);

    return idx + 1;
}

/* ============================================================
 * HINT GENERATORS — ONE PER ERROR CLASS
 *
 * Each function analyses the error message and source context
 * (KWIC style) to produce 0-N help/note sub-diagnostics for
 * a specific category of error.
 * ============================================================ */

/* ─── E2008: attempt to call a <type> value ────────────────── */

static int novai_hint_call(NovaDiagnostic *hints, int n, int max,
                           const char *msg,
                           const char *line, int line_len) {
    int is_nil = (strstr(msg, "nil") != NULL);

    if (is_nil && line != NULL && line_len > 0) {
        /* ── KWIC: try method names after . or : first ──
         * These are the most specific: math.sqr → sqrt,
         * s:uper → upper.  Check before bare call names. */
        char method_names[SUGGEST_MAX_NAMES][SUGGEST_MAX_NAME];
        int nm = novai_extract_method_names(line, line_len,
                                            method_names,
                                            SUGGEST_MAX_NAMES);

        for (int i = 0; i < nm; i++) {
            int mlen = (int)strlen(method_names[i]);
            const char *match = nova_suggest_name(
                method_names[i], mlen, novai_method_names, -1,
                SUGGEST_MAX_DISTANCE);

            if (match != NULL) {
                n = novai_add_hint(hints, n, max, NOVA_DIAG_HELP,
                                   "did you mean '%s'?", match);
                return n;
            }
        }

        /* ── KWIC: try bare call names against globals ──
         * Covers: prnt("hello") → print, ech("hi") → echo. */
        char call_names[SUGGEST_MAX_NAMES][SUGGEST_MAX_NAME];
        int nc = novai_extract_call_names(line, line_len,
                                          call_names, SUGGEST_MAX_NAMES);

        for (int i = 0; i < nc; i++) {
            int nlen = (int)strlen(call_names[i]);

            const char *match = nova_suggest_name(
                call_names[i], nlen, novai_global_names, -1,
                SUGGEST_MAX_DISTANCE);

            if (match != NULL) {
                n = novai_add_hint(hints, n, max, NOVA_DIAG_HELP,
                                   "did you mean '%s'?", match);
                return n;
            }

            /* Also try method names for bare calls */
            match = nova_suggest_name(
                call_names[i], nlen, novai_method_names, -1,
                SUGGEST_MAX_DISTANCE);

            if (match != NULL) {
                n = novai_add_hint(hints, n, max, NOVA_DIAG_HELP,
                                   "did you mean '%s'?", match);
                return n;
            }
        }

        /* ── KWIC: check for require() context ── */
        if (novai_line_contains(line, line_len, "require")) {
            n = novai_add_hint(hints, n, max, NOVA_DIAG_HELP,
                "if this came from require(), check that the module "
                "exists in package.path");
        } else {
            n = novai_add_hint(hints, n, max, NOVA_DIAG_HELP,
                "the variable is nil — it may not have been "
                "assigned a function");
        }
    } else if (is_nil) {
        /* No source line available */
        n = novai_add_hint(hints, n, max, NOVA_DIAG_HELP,
            "the value is nil — check that the function name is "
            "spelled correctly");
    } else {
        /* Called an integer, string, table, etc. */
        n = novai_add_hint(hints, n, max, NOVA_DIAG_HELP,
            "only functions can be called — check the variable's "
            "value or define a __call metamethod");
    }

    return n;
}

/* ─── E2013 / E2025: arithmetic / bitwise type errors ──────── */

static int novai_hint_arith(NovaDiagnostic *hints, int n, int max,
                            const char *msg,
                            const char *line, int line_len) {
    (void)line; (void)line_len;

    if (strstr(msg, "string") != NULL) {
        n = novai_add_hint(hints, n, max, NOVA_DIAG_HELP,
            "use tonumber() to convert a string to a number, "
            "or '..' to concatenate strings");
    } else if (strstr(msg, "nil") != NULL) {
        n = novai_add_hint(hints, n, max, NOVA_DIAG_HELP,
            "one of the operands is nil — check that all variables "
            "are initialized before use");
    } else if (strstr(msg, "boolean") != NULL) {
        n = novai_add_hint(hints, n, max, NOVA_DIAG_HELP,
            "booleans cannot be used in arithmetic — did you mean "
            "to use a conditional expression?");
    } else if (strstr(msg, "table") != NULL) {
        n = novai_add_hint(hints, n, max, NOVA_DIAG_HELP,
            "tables cannot be used in arithmetic directly — define "
            "an arithmetic metamethod (__add, __sub, etc.)");
    }

    return n;
}

/* ─── E2014: comparison type error ─────────────────────────── */

static int novai_hint_compare(NovaDiagnostic *hints, int n, int max,
                              const char *msg,
                              const char *line, int line_len) {
    (void)msg; (void)line; (void)line_len;

    n = novai_add_hint(hints, n, max, NOVA_DIAG_HELP,
        "only values of the same type can be ordered with "
        "<, >, <=, >=");
    n = novai_add_hint(hints, n, max, NOVA_DIAG_NOTE,
        "== and != work across types (different types are "
        "never equal)");

    return n;
}

/* ─── E2015: concatenation type error ──────────────────────── */

static int novai_hint_concat(NovaDiagnostic *hints, int n, int max,
                             const char *msg,
                             const char *line, int line_len) {
    (void)line; (void)line_len;

    if (strstr(msg, "nil") != NULL) {
        n = novai_add_hint(hints, n, max, NOVA_DIAG_HELP,
            "one of the operands is nil — check that the variable "
            "has been assigned a value");
    } else {
        n = novai_add_hint(hints, n, max, NOVA_DIAG_HELP,
            "use tostring() to convert a value to string before "
            "concatenating with '..'");
    }

    return n;
}

/* ─── E2017: negation / unary type error ───────────────────── */

static int novai_hint_negate(NovaDiagnostic *hints, int n, int max,
                             const char *msg,
                             const char *line, int line_len) {
    (void)line; (void)line_len;

    if (strstr(msg, "string") != NULL) {
        n = novai_add_hint(hints, n, max, NOVA_DIAG_HELP,
            "use tonumber() to convert the string before negating");
    } else if (strstr(msg, "nil") != NULL) {
        n = novai_add_hint(hints, n, max, NOVA_DIAG_HELP,
            "the variable is nil — check that it has been assigned "
            "a numeric value");
    } else if (strstr(msg, "length") != NULL ||
               strstr(msg, "len") != NULL) {
        n = novai_add_hint(hints, n, max, NOVA_DIAG_HELP,
            "the # operator requires a string or table");
    }

    return n;
}

/* ─── E2018: attempt to index a non-table value ────────────── */

static int novai_hint_index(NovaDiagnostic *hints, int n, int max,
                            const char *msg,
                            const char *line, int line_len) {
    int is_nil = (strstr(msg, "nil") != NULL);

    if (is_nil) {
        /* ── KWIC: check context for require() ── */
        if (line != NULL &&
            novai_line_contains(line, line_len, "require")) {
            n = novai_add_hint(hints, n, max, NOVA_DIAG_HELP,
                "require() returned nil — check that the module "
                "exists in package.path");
        } else {
            n = novai_add_hint(hints, n, max, NOVA_DIAG_HELP,
                "the variable is nil — check that it was initialized "
                "with {} or returned from a function");
        }
    } else {
        n = novai_add_hint(hints, n, max, NOVA_DIAG_HELP,
            "only tables can be indexed with [] or '.' — check "
            "the value's type with type()");
    }

    /* ── KWIC: detect 0-based indexing pitfalls ── */
    if (line != NULL && line_len > 0) {
        if (novai_line_contains(line, line_len, "[#") ||
            novai_line_contains(line, line_len, ":len()]")) {
            n = novai_add_hint(hints, n, max, NOVA_DIAG_NOTE,
                "Nova uses 0-based indexing — the last element "
                "is at #t - 1, not #t");
        }
    }

    return n;
}

/* ─── E2020: 'for' loop requires numeric values ───────────── */

static int novai_hint_for(NovaDiagnostic *hints, int n, int max,
                          const char *msg,
                          const char *line, int line_len) {
    (void)msg; (void)line; (void)line_len;

    n = novai_add_hint(hints, n, max, NOVA_DIAG_HELP,
        "all three 'for' bounds (start, stop, step) must be "
        "numbers — use tonumber() if they are strings");

    return n;
}

/* ─── E2021: bad argument type ─────────────────────────────── */

static int novai_hint_arg(NovaDiagnostic *hints, int n, int max,
                          const char *msg,
                          const char *line, int line_len) {
    (void)line; (void)line_len;

    if (strstr(msg, "string expected") != NULL) {
        n = novai_add_hint(hints, n, max, NOVA_DIAG_HELP,
            "use tostring() to convert the argument to a string");
    } else if (strstr(msg, "number expected") != NULL ||
               strstr(msg, "integer expected") != NULL) {
        n = novai_add_hint(hints, n, max, NOVA_DIAG_HELP,
            "use tonumber() to convert the argument to a number");
    } else if (strstr(msg, "table expected") != NULL) {
        n = novai_add_hint(hints, n, max, NOVA_DIAG_HELP,
            "expected a table — initialize with {} or check the "
            "return value of the function");
    } else if (strstr(msg, "function expected") != NULL) {
        n = novai_add_hint(hints, n, max, NOVA_DIAG_HELP,
            "expected a function — check that the callback is "
            "defined and not nil");
    }

    /* ── KWIC: check for method name typos in stdlib calls ── */
    if (line != NULL && line_len > 0) {
        char methods[SUGGEST_MAX_NAMES][SUGGEST_MAX_NAME];
        int nm = novai_extract_method_names(line, line_len,
                                            methods,
                                            SUGGEST_MAX_NAMES);

        for (int i = 0; i < nm; i++) {
            int mlen = (int)strlen(methods[i]);
            const char *match = nova_suggest_name(
                methods[i], mlen, novai_method_names, -1,
                SUGGEST_MAX_DISTANCE);

            if (match != NULL) {
                n = novai_add_hint(hints, n, max, NOVA_DIAG_HELP,
                                   "did you mean '%s'?", match);
                break;
            }
        }
    }

    return n;
}

/* ─── E2022: missing argument (value expected) ─────────────── */

static int novai_hint_argcount(NovaDiagnostic *hints, int n, int max,
                               const char *msg,
                               const char *line, int line_len) {
    (void)msg; (void)line; (void)line_len;

    n = novai_add_hint(hints, n, max, NOVA_DIAG_HELP,
        "a required argument is missing — check the function's "
        "expected parameters");

    return n;
}

/* ─── E2023: module not found ──────────────────────────────── */

static int novai_hint_module(NovaDiagnostic *hints, int n, int max,
                             const char *msg,
                             const char *line, int line_len) {
    (void)line; (void)line_len;

    /* Try to extract the module name from "module 'xxx' not found" */
    const char *mod = strstr(msg, "module '");
    if (mod != NULL) {
        mod += 8;  /* skip "module '" */
        const char *end = strchr(mod, '\'');
        if (end != NULL) {
            char modname[SUGGEST_MAX_NAME];
            int mlen = (int)(end - mod);
            if (mlen >= SUGGEST_MAX_NAME) { mlen = SUGGEST_MAX_NAME - 1; }
            memcpy(modname, mod, (size_t)mlen);
            modname[mlen] = '\0';

            const char *match = nova_suggest_name(
                modname, mlen, novai_module_names, -1,
                SUGGEST_MAX_DISTANCE);

            if (match != NULL) {
                n = novai_add_hint(hints, n, max, NOVA_DIAG_HELP,
                                   "did you mean '%s'?", match);
                return n;
            }
        }
    }

    n = novai_add_hint(hints, n, max, NOVA_DIAG_HELP,
        "check that the module file exists and package.path "
        "includes its directory");

    return n;
}

/* ─── E2016: length on wrong type ──────────────────────────── */

static int novai_hint_length(NovaDiagnostic *hints, int n, int max,
                             const char *msg,
                             const char *line, int line_len) {
    (void)msg; (void)line; (void)line_len;

    n = novai_add_hint(hints, n, max, NOVA_DIAG_HELP,
        "the # operator requires a string or a table — check "
        "the value's type with type()");

    return n;
}

/* ============================================================
 * PUBLIC: MAIN ENTRY POINT
 *
 * Dispatches to per-error-class hint generators, then runs
 * universal KWIC context checks against the source line.
 * ============================================================ */

int nova_suggest_runtime_hints(NovaDiagnostic *hints, int max_hints,
                               int diag_code, const char *msg,
                               const char *source_line, int source_len) {
    if (hints == NULL || max_hints <= 0 || msg == NULL) {
        return 0;
    }

    int n = 0;

    /* ── Dispatch to per-error-class hint generator ── */
    switch (diag_code) {
        case NOVA_E2008:  /* attempt to call non-function */
            n = novai_hint_call(hints, n, max_hints,
                                msg, source_line, source_len);
            break;

        case NOVA_E2013:  /* arithmetic type error */
        case NOVA_E2025:  /* bitwise on non-integer */
            n = novai_hint_arith(hints, n, max_hints,
                                 msg, source_line, source_len);
            break;

        case NOVA_E2014:  /* comparison type error */
            n = novai_hint_compare(hints, n, max_hints,
                                   msg, source_line, source_len);
            break;

        case NOVA_E2015:  /* concatenation type error */
            n = novai_hint_concat(hints, n, max_hints,
                                  msg, source_line, source_len);
            break;

        case NOVA_E2016:  /* length on wrong type */
            n = novai_hint_length(hints, n, max_hints,
                                  msg, source_line, source_len);
            break;

        case NOVA_E2017:  /* negation type error */
            n = novai_hint_negate(hints, n, max_hints,
                                  msg, source_line, source_len);
            break;

        case NOVA_E2018:  /* index on non-table */
            n = novai_hint_index(hints, n, max_hints,
                                 msg, source_line, source_len);
            break;

        case NOVA_E2020:  /* for loop type error */
            n = novai_hint_for(hints, n, max_hints,
                               msg, source_line, source_len);
            break;

        case NOVA_E2021:  /* bad argument type */
            n = novai_hint_arg(hints, n, max_hints,
                               msg, source_line, source_len);
            break;

        case NOVA_E2022:  /* missing argument */
            n = novai_hint_argcount(hints, n, max_hints,
                                    msg, source_line, source_len);
            break;

        case NOVA_E2023:  /* module not found */
            n = novai_hint_module(hints, n, max_hints,
                                  msg, source_line, source_len);
            break;

        default:
            break;
    }

    /* ── KWIC: universal source-line patterns ── */
    if (source_line != NULL && source_len > 0 && n < max_hints) {
        /* Detect 0-based indexing mistakes (unless already handled) */
        if (diag_code != NOVA_E2018) {
            if (novai_line_contains(source_line, source_len, "[#")) {
                n = novai_add_hint(hints, n, max_hints, NOVA_DIAG_NOTE,
                    "Nova uses 0-based indexing — the last element "
                    "is at #t - 1, not #t");
            }
        }
    }

    return n;
}
