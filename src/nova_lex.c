/**
 * @file nova_lex.c
 * @brief Nova Language - Lexer (Tokenizer) Implementation
 *
 * Single-pass lexer converting Nova source text into tokens.
 * Handles all Nova syntax: keywords, operators, numbers, strings,
 * comments, preprocessor directives, and string interpolation.
 *
 * The lexer produces a token stream consumed by the preprocessor
 * and then the parser. It tracks source locations for error messages.
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
 *   - nova_lex.h (public API)
 *   - nova_conf.h (numeric types, limits)
 *   - zorya/pcm.h (LIKELY/UNLIKELY, branch hints)
 */

#include "nova/nova_lex.h"
#include "zorya/pcm.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <errno.h>

/* ============================================================
 * INTERNAL CONSTANTS
 * ============================================================ */

/** Initial size of the lexer string buffer */
#define NOVA_LEX_BUFFER_INIT 256

/** Growth factor for the lexer string buffer */
#define NOVA_LEX_BUFFER_GROW 2

/* ============================================================
 * CHARACTER CLASSIFICATION
 *
 * Inline helpers for character classification.
 * Using direct comparisons for hot-path performance instead
 * of locale-dependent ctype.h functions.
 * ============================================================ */

/*
** PCM: nova_is_alpha
** Purpose: Check if character is alphabetic or underscore
** Rationale: Called on every character during identifier scanning
** Performance Impact: 3 comparisons, no function call overhead
** Audit Date: 2026-02-05
*/
static inline int nova_is_alpha(char c) {
    return (c >= 'a' && c <= 'z') ||
           (c >= 'A' && c <= 'Z') ||
           c == '_';
}

/*
** PCM: nova_is_digit
** Purpose: Check if character is decimal digit
** Rationale: Called on every character during number scanning
** Performance Impact: Single range check
** Audit Date: 2026-02-05
*/
static inline int nova_is_digit(char c) {
    return c >= '0' && c <= '9';
}

/*
** PCM: nova_is_hex_digit
** Purpose: Check if character is hexadecimal digit
** Rationale: Used in hex literal scanning (0x...)
** Performance Impact: 3 range checks
** Audit Date: 2026-02-05
*/
static inline int nova_is_hex_digit(char c) {
    return (c >= '0' && c <= '9') ||
           (c >= 'a' && c <= 'f') ||
           (c >= 'A' && c <= 'F');
}

static inline int nova_is_alnum(char c) {
    return nova_is_alpha(c) || nova_is_digit(c);
}

static inline int nova_is_space(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v';
}

static inline int nova_hex_value(char c) {
    if (c >= '0' && c <= '9') return (int)(c - '0');
    if (c >= 'a' && c <= 'f') return (int)(c - 'a') + 10;
    if (c >= 'A' && c <= 'F') return (int)(c - 'A') + 10;
    return -1;
}

/* ============================================================
 * KEYWORD TABLE
 *
 * Sorted for binary search. Keywords are checked after scanning
 * an identifier -- if the identifier matches a keyword, its
 * token type is promoted from NAME to the keyword type.
 * ============================================================ */

typedef struct {
    const char    *text;
    NovaTokenType  type;
} NovaKeyword;

/**
 * @brief Keyword lookup table (sorted alphabetically)
 *
 * Must remain sorted for binary search to work correctly.
 * Includes both Lua-compatible and Nova extension keywords.
 */
static const NovaKeyword nova_keywords[] = {
    { "and",      NOVA_TOKEN_AND },
    { "async",    NOVA_TOKEN_ASYNC },
    { "await",    NOVA_TOKEN_AWAIT },
    { "break",    NOVA_TOKEN_BREAK },
    { "const",    NOVA_TOKEN_CONST },
    { "continue", NOVA_TOKEN_CONTINUE },
    { "do",       NOVA_TOKEN_DO },
    { "else",     NOVA_TOKEN_ELSE },
    { "elseif",   NOVA_TOKEN_ELSEIF },
    { "end",      NOVA_TOKEN_END },
    { "enum",     NOVA_TOKEN_ENUM },
    { "export",   NOVA_TOKEN_EXPORT },
    { "false",    NOVA_TOKEN_FALSE },
    { "for",      NOVA_TOKEN_FOR },
    { "function", NOVA_TOKEN_FUNCTION },
    { "goto",     NOVA_TOKEN_GOTO },
    { "if",       NOVA_TOKEN_IF },
    { "import",   NOVA_TOKEN_IMPORT },
    { "in",       NOVA_TOKEN_IN },
    { "nil",      NOVA_TOKEN_NIL },
    { "not",      NOVA_TOKEN_NOT },
    { "or",       NOVA_TOKEN_OR },
    { "repeat",   NOVA_TOKEN_REPEAT },
    { "return",   NOVA_TOKEN_RETURN },
    { "spawn",    NOVA_TOKEN_SPAWN },
    { "struct",   NOVA_TOKEN_STRUCT },
    { "then",     NOVA_TOKEN_THEN },
    { "true",     NOVA_TOKEN_TRUE },
    { "typedec",  NOVA_TOKEN_TYPEDEC },
    { "until",    NOVA_TOKEN_UNTIL },
    { "while",    NOVA_TOKEN_WHILE },
    { "yield",    NOVA_TOKEN_YIELD },
};

/**
 * @brief Additional keyword aliases (checked after main table)
 *
 * These map to existing token types but use different spellings.
 * 'dec' and 'declare' both map to NOVA_TOKEN_LOCAL (scope binding).
 */
static const NovaKeyword nova_keyword_aliases[] = {
    { "dec",      NOVA_TOKEN_LOCAL },
    { "declare",  NOVA_TOKEN_LOCAL },
};

#define NOVA_KEYWORD_ALIAS_COUNT \
    (sizeof(nova_keyword_aliases) / sizeof(nova_keyword_aliases[0]))

#define NOVA_KEYWORD_COUNT \
    (sizeof(nova_keywords) / sizeof(nova_keywords[0]))

/**
 * @brief Preprocessor directive lookup table (sorted)
 */
static const NovaKeyword nova_pp_directives[] = {
    { "define",   NOVA_TOKEN_PP_DEFINE },
    { "elif",     NOVA_TOKEN_PP_ELIF },
    { "else",     NOVA_TOKEN_PP_ELSE },
    { "endif",    NOVA_TOKEN_PP_ENDIF },
    { "error",    NOVA_TOKEN_PP_ERROR },
    { "if",       NOVA_TOKEN_PP_IF },
    { "ifdef",    NOVA_TOKEN_PP_IFDEF },
    { "ifndef",   NOVA_TOKEN_PP_IFNDEF },
    { "import",   NOVA_TOKEN_PP_IMPORT },
    { "include",  NOVA_TOKEN_PP_INCLUDE },
    { "line",     NOVA_TOKEN_PP_LINE },
    { "pragma",   NOVA_TOKEN_PP_PRAGMA },
    { "require",  NOVA_TOKEN_PP_REQUIRE },
    { "undef",    NOVA_TOKEN_PP_UNDEF },
    { "warning",  NOVA_TOKEN_PP_WARNING },
};

#define NOVA_PP_DIRECTIVE_COUNT \
    (sizeof(nova_pp_directives) / sizeof(nova_pp_directives[0]))

/* ============================================================
 * INTERNAL HELPERS
 * ============================================================ */

/**
 * @brief Binary search a sorted keyword table
 *
 * @param table   Keyword table (must be sorted by text)
 * @param count   Number of entries
 * @param text    Text to search for
 * @param len     Text length
 * @return Matching token type, or NOVA_TOKEN_NAME if not found
 */
static NovaTokenType novai_keyword_lookup(
    const NovaKeyword *table, size_t count,
    const char *text, size_t len
) {
    size_t lo = 0;
    size_t hi = count;

    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int cmp = strncmp(table[mid].text, text, len);

        if (cmp == 0) {
            /* Length must also match (strncmp doesn't check trailing) */
            if (table[mid].text[len] == '\0') {
                return table[mid].type;
            }
            /* Keyword is longer than our text -- our text sorts before */
            if (table[mid].text[len] > '\0') {
                hi = mid;
            } else {
                lo = mid + 1;
            }
        } else if (cmp < 0) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }

    return NOVA_TOKEN_NAME;
}

/**
 * @brief Peek at the current character without advancing
 *
 * @param L Lexer state
 * @return Current character, or '\0' at end of input
 */
static inline char novai_peek(const NovaLexer *L) {
    if (L->pos < L->source_len) {
        return L->source[L->pos];
    }
    return '\0';
}

/**
 * @brief Peek at the character N positions ahead
 *
 * @param L      Lexer state
 * @param offset Lookahead distance (0 = current)
 * @return Character at offset, or '\0' if past end
 */
static inline char novai_peek_at(const NovaLexer *L, size_t offset) {
    size_t idx = L->pos + offset;
    if (idx < L->source_len) {
        return L->source[idx];
    }
    return '\0';
}

/**
 * @brief Advance the lexer by one character
 *
 * Tracks newlines for line counting.
 *
 * @param L Lexer state
 * @return The character that was consumed, or '\0' at EOF
 */
static inline char novai_advance(NovaLexer *L) {
    if (L->pos >= L->source_len) {
        return '\0';
    }
    char c = L->source[L->pos];
    L->pos++;
    if (c == '\n') {
        L->line++;
        L->line_start = (int)L->pos;
        L->at_line_start = 1;
    } else if (!nova_is_space(c)) {
        /* Whitespace preserves at_line_start so that
         * indented preprocessor directives (#define after spaces)
         * are correctly recognized. Only non-whitespace clears it. */
        L->at_line_start = 0;
    }
    return c;
}

/**
 * @brief Check if lexer is at end of input
 */
static inline int novai_at_end(const NovaLexer *L) {
    return L->pos >= L->source_len;
}

/**
 * @brief Get current column number (1-based)
 */
static inline int novai_column(const NovaLexer *L) {
    return (int)(L->pos - (size_t)L->line_start) + 1;
}

/**
 * @brief Make a source location for the current position
 */
static inline NovaSourceLoc novai_loc(const NovaLexer *L) {
    NovaSourceLoc loc = {
        .filename = L->filename,
        .line     = L->line,
        .column   = novai_column(L)
    };
    return loc;
}

/**
 * @brief Set an error on the lexer
 *
 * @param L   Lexer state
 * @param fmt Printf-style format string
 */
static void novai_lex_error(NovaLexer *L, const char *fmt, ...) {
    va_list args;
    char msg[200] = {0};

    va_start(args, fmt);
    (void)vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);

    (void)snprintf(L->error_msg, sizeof(L->error_msg),
                   "%s:%d:%d: error: %s",
                   L->filename ? L->filename : "<input>",
                   L->line, novai_column(L), msg);

    L->error_count++;
}

/* ============================================================
 * STRING BUFFER OPERATIONS
 *
 * The lexer maintains a reusable buffer for building string
 * and identifier token values. It grows as needed.
 * ============================================================ */

/**
 * @brief Reset the buffer to empty (reuse memory)
 */
static inline void novai_buf_reset(NovaLexer *L) {
    L->buffer_len = 0;
}

/**
 * @brief Ensure buffer has room for at least 'needed' more bytes
 *
 * @param L      Lexer state
 * @param needed Bytes needed
 * @return 0 on success, -1 on allocation failure
 */
static int novai_buf_grow(NovaLexer *L, size_t needed) {
    size_t required = L->buffer_len + needed + 1; /* +1 for NUL */

    if (required <= L->buffer_cap) {
        return 0;
    }

    size_t new_cap = L->buffer_cap * NOVA_LEX_BUFFER_GROW;
    if (new_cap < required) {
        new_cap = required;
    }

    char *new_buf = (char *)realloc(L->buffer, new_cap);
    if (new_buf == NULL) {
        novai_lex_error(L, "out of memory in lexer buffer");
        return -1;
    }

    L->buffer = new_buf;
    L->buffer_cap = new_cap;
    return 0;
}

/**
 * @brief Append a single character to the buffer
 */
static inline int novai_buf_push(NovaLexer *L, char c) {
    if (L->buffer_len + 1 >= L->buffer_cap) {
        if (novai_buf_grow(L, 1) != 0) {
            return -1;
        }
    }
    L->buffer[L->buffer_len] = c;
    L->buffer_len++;
    L->buffer[L->buffer_len] = '\0';
    return 0;
}

/**
 * @brief Append a string slice to the buffer
 *
 * Currently used internally by long string scanning.
 * Retained for future preprocessor use.
 */
#if 0  /* Unused for now -- enable when preprocessor needs it */
static int novai_buf_append(NovaLexer *L, const char *str, size_t len) {
    if (novai_buf_grow(L, len) != 0) {
        return -1;
    }
    memcpy(L->buffer + L->buffer_len, str, len);
    L->buffer_len += len;
    L->buffer[L->buffer_len] = '\0';
    return 0;
}
#endif

/* ============================================================
 * MAKE TOKEN HELPERS
 * ============================================================ */

/**
 * @brief Create a simple token (no value payload)
 */
static NovaToken novai_make_token(const NovaLexer *L, NovaTokenType type,
                                  NovaSourceLoc loc) {
    (void)L;  /* Available for future use (e.g., memory pool) */
    NovaToken tok = {0};
    tok.type = type;
    tok.loc = loc;
    return tok;
}

/**
 * @brief Create an error token
 */
static NovaToken novai_make_error(NovaLexer *L, const char *msg) {
    NovaSourceLoc loc = novai_loc(L);
    novai_lex_error(L, "%s", msg);
    NovaToken tok = {0};
    tok.type = NOVA_TOKEN_ERROR;
    tok.loc = loc;
    tok.value.string.data = L->error_msg;
    tok.value.string.len = strlen(L->error_msg);
    return tok;
}

/* ============================================================
 * SKIP WHITESPACE AND COMMENTS
 * ============================================================ */

/**
 * @brief Count the level of long bracket: [==...==[
 *
 * @param L Lexer state (positioned at first '[')
 * @return Number of '=' characters, or -1 if not a long bracket
 */
static int novai_count_long_bracket(const NovaLexer *L) {
    size_t p = L->pos;

    if (p >= L->source_len || L->source[p] != '[') {
        return -1;
    }
    p++; /* skip first '[' */

    int level = 0;
    while (p < L->source_len && L->source[p] == '=') {
        level++;
        p++;
    }

    if (p < L->source_len && L->source[p] == '[') {
        return level;
    }
    return -1;
}

/**
 * @brief Skip a long string/comment body: [==[...]==]
 *
 * Assumes the opening [==[ has already been consumed.
 *
 * @param L     Lexer state
 * @param level Number of '=' in the bracket
 * @return 0 on success, -1 if unterminated
 */
static int novai_skip_long_body(NovaLexer *L, int level) {
    while (!novai_at_end(L)) {
        char c = novai_advance(L);

        if (c == ']') {
            /* Check for closing bracket of matching level */
            int count = 0;
            while (!novai_at_end(L) && novai_peek(L) == '=' && count < level) {
                novai_advance(L);
                count++;
            }
            if (count == level && novai_peek(L) == ']') {
                novai_advance(L); /* consume closing ']' */
                return 0;
            }
        }
    }

    novai_lex_error(L, "unterminated long string or comment");
    return -1;
}

/**
 * @brief Skip whitespace, newlines, and comments
 *
 * This is called before scanning each token. It handles:
 * - Spaces, tabs, carriage returns
 * - Newlines (with line tracking)
 * - Line comments: -- ...
 * - Block comments: --[[ ... ]]
 */
static void novai_skip_whitespace(NovaLexer *L) {
    while (!novai_at_end(L)) {
        char c = novai_peek(L);

        /* Whitespace (not newline) */
        if (nova_is_space(c)) {
            novai_advance(L);
            continue;
        }

        /* Newline */
        if (c == '\n') {
            novai_advance(L);
            continue;
        }

        /* Comments start with -- */
        if (c == '-' && novai_peek_at(L, 1) == '-') {
            novai_advance(L); /* consume first '-' */
            novai_advance(L); /* consume second '-' */

            /* Check for block comment --[[ or --[==[ */
            if (novai_peek(L) == '[') {
                int level = novai_count_long_bracket(L);
                if (level >= 0) {
                    /* Skip opening [==[ */
                    novai_advance(L); /* '[' */
                    for (int i = 0; i < level; i++) {
                        novai_advance(L); /* '=' */
                    }
                    novai_advance(L); /* '[' */

                    /* Skip block comment body and closing */
                    (void)novai_skip_long_body(L, level);
                    continue;
                }
            }

            /* Line comment: skip to end of line */
            while (!novai_at_end(L) && novai_peek(L) != '\n') {
                novai_advance(L);
            }
            continue;
        }

        /* Not whitespace or comment -- done */
        break;
    }
}

/* ============================================================
 * NUMBER SCANNING
 *
 * Supports:
 *   - Decimal integers: 42, 1000
 *   - Hex integers: 0xFF, 0XAB
 *   - Binary integers: 0b1010, 0B1111
 *   - Decimal floats: 3.14, .5, 1., 1e10, 2.5e-3
 *   - Hex floats: 0x1.Fp10 (Lua 5.3 compatible)
 *   - Underscore separators: 1_000_000, 0xFF_FF
 * ============================================================ */

/**
 * @brief Scan a number literal (integer or float)
 *
 * @param L Lexer state (positioned at first digit or '.')
 * @return Token (INTEGER or NUMBER type)
 */
static NovaToken novai_scan_number(NovaLexer *L) {
    NovaSourceLoc loc = novai_loc(L);
    novai_buf_reset(L);
    int is_float = 0;
    int is_hex = 0;
    int is_bin = 0;

    /* Check for hex or binary prefix */
    if (novai_peek(L) == '0') {
        char next = novai_peek_at(L, 1);

        if (next == 'x' || next == 'X') {
            is_hex = 1;
            (void)novai_buf_push(L, novai_advance(L)); /* '0' */
            (void)novai_buf_push(L, novai_advance(L)); /* 'x' */
        } else if (next == 'b' || next == 'B') {
            is_bin = 1;
            novai_advance(L); /* skip '0' */
            novai_advance(L); /* skip 'b' */
        }
    }

    if (is_hex) {
        /* Hex digits */
        while (!novai_at_end(L) &&
               (nova_is_hex_digit(novai_peek(L)) || novai_peek(L) == '_')) {
            char c = novai_advance(L);
            if (c != '_') {
                (void)novai_buf_push(L, c);
            }
        }
        /* Hex float: 0x1.Fp10 */
        if (novai_peek(L) == '.') {
            is_float = 1;
            (void)novai_buf_push(L, novai_advance(L)); /* '.' */
            while (!novai_at_end(L) &&
                   (nova_is_hex_digit(novai_peek(L)) || novai_peek(L) == '_')) {
                char c = novai_advance(L);
                if (c != '_') {
                    (void)novai_buf_push(L, c);
                }
            }
        }
        /* Hex exponent: p+10 or P-3 */
        if (novai_peek(L) == 'p' || novai_peek(L) == 'P') {
            is_float = 1;
            (void)novai_buf_push(L, novai_advance(L)); /* 'p' */
            if (novai_peek(L) == '+' || novai_peek(L) == '-') {
                (void)novai_buf_push(L, novai_advance(L));
            }
            while (!novai_at_end(L) && nova_is_digit(novai_peek(L))) {
                (void)novai_buf_push(L, novai_advance(L));
            }
        }
    } else if (is_bin) {
        /* Binary digits */
        while (!novai_at_end(L) &&
               (novai_peek(L) == '0' || novai_peek(L) == '1' ||
                novai_peek(L) == '_')) {
            char c = novai_advance(L);
            if (c != '_') {
                (void)novai_buf_push(L, c);
            }
        }
    } else {
        /* Decimal digits */
        while (!novai_at_end(L) &&
               (nova_is_digit(novai_peek(L)) || novai_peek(L) == '_')) {
            char c = novai_advance(L);
            if (c != '_') {
                (void)novai_buf_push(L, c);
            }
        }

        /* Decimal point */
        if (novai_peek(L) == '.' && novai_peek_at(L, 1) != '.') {
            is_float = 1;
            (void)novai_buf_push(L, novai_advance(L)); /* '.' */
            while (!novai_at_end(L) &&
                   (nova_is_digit(novai_peek(L)) || novai_peek(L) == '_')) {
                char c = novai_advance(L);
                if (c != '_') {
                    (void)novai_buf_push(L, c);
                }
            }
        }

        /* Exponent: e10 or E-3 */
        if (novai_peek(L) == 'e' || novai_peek(L) == 'E') {
            is_float = 1;
            (void)novai_buf_push(L, novai_advance(L)); /* 'e' */
            if (novai_peek(L) == '+' || novai_peek(L) == '-') {
                (void)novai_buf_push(L, novai_advance(L));
            }
            while (!novai_at_end(L) && nova_is_digit(novai_peek(L))) {
                (void)novai_buf_push(L, novai_advance(L));
            }
        }
    }

    /* Parse the buffered text into a value */
    NovaToken tok = novai_make_token(L, is_float ? NOVA_TOKEN_NUMBER : NOVA_TOKEN_INTEGER, loc);

    if (is_float) {
        char *endptr = NULL;
        errno = 0;
        tok.value.number = strtod(L->buffer, &endptr);
        if (errno == ERANGE) {
            novai_lex_error(L, "number literal out of range");
            tok.type = NOVA_TOKEN_ERROR;
        }
    } else if (is_bin) {
        /* Parse binary manually */
        nova_int_t val = 0;
        for (size_t i = 0; i < L->buffer_len; i++) {
            val = (val << 1) | (nova_int_t)(L->buffer[i] - '0');
        }
        tok.value.integer = val;
    } else if (is_hex) {
        char *endptr = NULL;
        errno = 0;
        unsigned long long ull = strtoull(L->buffer, &endptr, 16);
        if (errno == ERANGE) {
            novai_lex_error(L, "hex literal out of range");
            tok.type = NOVA_TOKEN_ERROR;
        }
        tok.value.integer = (nova_int_t)ull;
    } else {
        char *endptr = NULL;
        errno = 0;
        unsigned long long ull = strtoull(L->buffer, &endptr, 10);
        if (errno == ERANGE) {
            novai_lex_error(L, "integer literal out of range");
            tok.type = NOVA_TOKEN_ERROR;
        }
        tok.value.integer = (nova_int_t)ull;
    }

    return tok;
}

/* ============================================================
 * STRING SCANNING
 * ============================================================ */

/**
 * @brief Process an escape sequence in a string literal
 *
 * Reads the escape character after the backslash and appends
 * the decoded byte to the buffer.
 *
 * @param L Lexer state (positioned AFTER the backslash)
 * @return 0 on success, -1 on error
 */
static int novai_scan_escape(NovaLexer *L) {
    if (novai_at_end(L)) {
        novai_lex_error(L, "unterminated escape sequence");
        return -1;
    }

    char c = novai_advance(L);

    switch (c) {
        case 'a':  (void)novai_buf_push(L, '\a'); break;
        case 'b':  (void)novai_buf_push(L, '\b'); break;
        case 'f':  (void)novai_buf_push(L, '\f'); break;
        case 'n':  (void)novai_buf_push(L, '\n'); break;
        case 'r':  (void)novai_buf_push(L, '\r'); break;
        case 't':  (void)novai_buf_push(L, '\t'); break;
        case 'v':  (void)novai_buf_push(L, '\v'); break;
        case '\\': (void)novai_buf_push(L, '\\'); break;
        case '\'': (void)novai_buf_push(L, '\''); break;
        case '\"': (void)novai_buf_push(L, '\"'); break;
        case '\n': (void)novai_buf_push(L, '\n'); break;
        case '\r':
            (void)novai_buf_push(L, '\n');
            /* \r\n counts as single newline */
            if (novai_peek(L) == '\n') {
                novai_advance(L);
            }
            break;

        case '0': case '1': case '2': case '3': case '4':
        case '5': case '6': case '7': case '8': case '9': {
            /* Decimal escape: \DDD (up to 3 digits, max 255) */
            int val = (int)(c - '0');
            int count = 1;
            while (count < 3 && nova_is_digit(novai_peek(L))) {
                val = val * 10 + (int)(novai_advance(L) - '0');
                count++;
            }
            if (val > 255) {
                novai_lex_error(L, "decimal escape too large (max 255)");
                return -1;
            }
            (void)novai_buf_push(L, (char)val);
            break;
        }

        case 'x': {
            /* Hex escape: \xHH (exactly 2 digits) */
            int val = 0;
            for (int i = 0; i < 2; i++) {
                int d = nova_hex_value(novai_peek(L));
                if (d < 0) {
                    novai_lex_error(L, "invalid hex escape sequence");
                    return -1;
                }
                val = (val << 4) | d;
                novai_advance(L);
            }
            (void)novai_buf_push(L, (char)val);
            break;
        }

        case 'z':
            /* \z skips following whitespace (Lua 5.2+) */
            while (!novai_at_end(L) && (nova_is_space(novai_peek(L)) ||
                   novai_peek(L) == '\n' || novai_peek(L) == '\r')) {
                novai_advance(L);
            }
            break;

        default:
            novai_lex_error(L, "invalid escape sequence '\\%c'", c);
            return -1;
    }

    return 0;
}

/**
 * @brief Scan a short string literal (single or double quoted)
 *
 * @param L     Lexer state
 * @param quote The opening quote character (' or ")
 * @return String token
 */
static NovaToken novai_scan_string(NovaLexer *L, char quote) {
    NovaSourceLoc loc = novai_loc(L);
    novai_buf_reset(L);

    novai_advance(L); /* consume opening quote */

    while (!novai_at_end(L)) {
        char c = novai_peek(L);

        if (c == quote) {
            novai_advance(L); /* consume closing quote */
            NovaToken tok = novai_make_token(L, NOVA_TOKEN_STRING, loc);
            tok.value.string.data = L->buffer;
            tok.value.string.len = L->buffer_len;
            return tok;
        }

        if (c == '\n' || c == '\r') {
            return novai_make_error(L, "unterminated string literal");
        }

        if (c == '\\') {
            novai_advance(L); /* consume backslash */
            if (novai_scan_escape(L) != 0) {
                NovaToken tok = {0};
                tok.type = NOVA_TOKEN_ERROR;
                tok.loc = loc;
                return tok;
            }
        } else {
            (void)novai_buf_push(L, novai_advance(L));
        }
    }

    return novai_make_error(L, "unterminated string literal");
}

/**
 * @brief Scan a long string: [[...]] or [==[...]==]
 *
 * @param L     Lexer state (positioned at first '[')
 * @param level Number of '=' in the bracket
 * @return String token
 */
static NovaToken novai_scan_long_string(NovaLexer *L, int level) {
    NovaSourceLoc loc = novai_loc(L);
    novai_buf_reset(L);

    /* Skip opening [==[ */
    novai_advance(L); /* first '[' */
    for (int i = 0; i < level; i++) {
        novai_advance(L); /* '=' */
    }
    novai_advance(L); /* second '[' */

    /* Skip optional immediate newline after opening bracket */
    if (novai_peek(L) == '\n') {
        novai_advance(L);
    } else if (novai_peek(L) == '\r') {
        novai_advance(L);
        if (novai_peek(L) == '\n') {
            novai_advance(L);
        }
    }

    /* Read body until closing bracket */
    while (!novai_at_end(L)) {
        char c = novai_peek(L);

        if (c == ']') {
            /* Check for closing bracket of matching level */
            size_t save_pos = L->pos;
            int save_line = L->line;
            int save_line_start = L->line_start;

            novai_advance(L); /* consume ']' */
            int count = 0;
            while (!novai_at_end(L) && novai_peek(L) == '=' && count < level) {
                novai_advance(L);
                count++;
            }

            if (count == level && novai_peek(L) == ']') {
                novai_advance(L); /* consume closing ']' */
                /* Success: return the string */
                NovaToken tok = novai_make_token(L, NOVA_TOKEN_LONG_STRING, loc);
                tok.value.string.data = L->buffer;
                tok.value.string.len = L->buffer_len;
                return tok;
            }

            /* Not a closing bracket -- restore and push what we read */
            L->pos = save_pos;
            L->line = save_line;
            L->line_start = save_line_start;
            (void)novai_buf_push(L, novai_advance(L));
        } else {
            (void)novai_buf_push(L, novai_advance(L));
        }
    }

    return novai_make_error(L, "unterminated long string");
}

/**
 * @brief Scan the opening backtick of an interpolated string
 *
 * Emits INTERP_START and sets the lexer into interpolation mode.
 * Subsequent calls to novai_scan_token() will produce INTERP_SEGMENT
 * tokens for literal text and normal expression tokens for ${...}.
 *
 * @param L Lexer state (positioned at opening backtick)
 * @return INTERP_START token
 */
static NovaToken novai_scan_interp_start(NovaLexer *L) {
    NovaSourceLoc loc = novai_loc(L);
    novai_advance(L); /* consume opening backtick */
    L->in_interp = 1;
    L->interp_brace_depth = 0;
    L->interp_pending_end = 0;
    return novai_make_token(L, NOVA_TOKEN_INTERP_START, loc);
}

/**
 * @brief Scan a literal text segment within a backtick string
 *
 * Called when in_interp is true and interp_brace_depth is 0.
 * Scans literal text until ${  (expression start) or ` (end).
 *
 * On ${: emits INTERP_SEGMENT with the text, sets brace_depth=1
 * On `:  emits INTERP_SEGMENT with the text, sets pending_end=1
 * On EOF: emits error (unterminated)
 *
 * @param L Lexer state (positioned inside backtick string body)
 * @return INTERP_SEGMENT token
 */
static NovaToken novai_scan_interp_continuation(NovaLexer *L) {
    NovaSourceLoc loc = novai_loc(L);
    novai_buf_reset(L);

    while (!novai_at_end(L)) {
        char c = novai_peek(L);

        if (c == '`') {
            /* End of interpolated string */
            novai_advance(L); /* consume closing backtick */
            NovaToken tok = novai_make_token(L, NOVA_TOKEN_INTERP_SEGMENT, loc);
            tok.value.string.data = L->buffer;
            tok.value.string.len = L->buffer_len;
            L->in_interp = 0;
            L->interp_pending_end = 1;
            return tok;
        }

        if (c == '$' && novai_peek_at(L, 1) == '{') {
            /* Expression interpolation: ${...} */
            novai_advance(L); /* consume $ */
            novai_advance(L); /* consume { */
            NovaToken tok = novai_make_token(L, NOVA_TOKEN_INTERP_SEGMENT, loc);
            tok.value.string.data = L->buffer;
            tok.value.string.len = L->buffer_len;
            L->interp_brace_depth = 1;
            return tok;
        }

        if (c == '\\') {
            novai_advance(L);
            if (novai_scan_escape(L) != 0) {
                NovaToken tok = {0};
                tok.type = NOVA_TOKEN_ERROR;
                tok.loc = loc;
                return tok;
            }
        } else {
            (void)novai_buf_push(L, novai_advance(L));
        }
    }

    return novai_make_error(L, "unterminated interpolated string");
}

/* ============================================================
 * IDENTIFIER AND KEYWORD SCANNING
 * ============================================================ */

/**
 * @brief Scan an identifier or keyword
 *
 * Reads [a-zA-Z_][a-zA-Z0-9_]* and checks against the
 * keyword table via binary search.
 *
 * @param L Lexer state
 * @return NAME or keyword token
 */
static NovaToken novai_scan_identifier(NovaLexer *L) {
    NovaSourceLoc loc = novai_loc(L);
    const char *start = L->source + L->pos;

    /* Consume all alphanumeric + underscore characters */
    while (!novai_at_end(L) && nova_is_alnum(novai_peek(L))) {
        novai_advance(L);
    }

    size_t len = (size_t)((L->source + L->pos) - start);

    /* Check if it's a keyword */
    NovaTokenType kw = novai_keyword_lookup(
        nova_keywords, NOVA_KEYWORD_COUNT, start, len
    );

    /* Check keyword aliases (dec, declare) */
    if (kw == NOVA_TOKEN_NAME) {
        kw = novai_keyword_lookup(
            nova_keyword_aliases, NOVA_KEYWORD_ALIAS_COUNT, start, len
        );
    }

    NovaToken tok = novai_make_token(L, kw, loc);
    tok.value.string.data = start;
    tok.value.string.len = len;
    return tok;
}

/* ============================================================
 * PREPROCESSOR DIRECTIVE SCANNING
 * ============================================================ */

/**
 * @brief Scan a preprocessor directive (after '#')
 *
 * Reads the directive name and maps it to a PP token type.
 * The '#' has already been consumed.
 *
 * @param L Lexer state
 * @return Preprocessor directive token
 */
static NovaToken novai_scan_pp_directive(NovaLexer *L) {
    NovaSourceLoc loc = novai_loc(L);

    /* Skip whitespace between '#' and directive name */
    while (!novai_at_end(L) && nova_is_space(novai_peek(L))) {
        novai_advance(L);
    }

    /* Check for ## (token paste) */
    if (novai_peek(L) == '#') {
        novai_advance(L);
        return novai_make_token(L, NOVA_TOKEN_PP_PASTE, loc);
    }

    /* Read directive name */
    const char *start = L->source + L->pos;
    while (!novai_at_end(L) && nova_is_alpha(novai_peek(L))) {
        novai_advance(L);
    }
    size_t len = (size_t)((L->source + L->pos) - start);

    if (len == 0) {
        /* Bare '#' -- in macro body context this is stringify */
        return novai_make_token(L, NOVA_TOKEN_PP_STRINGIFY, loc);
    }

    /* Look up directive */
    NovaTokenType directive = novai_keyword_lookup(
        nova_pp_directives, NOVA_PP_DIRECTIVE_COUNT, start, len
    );

    if (directive == NOVA_TOKEN_NAME) {
        /* Not a known directive. If we're already inside a PP
         * directive (e.g. #define body), this is the stringify
         * operator: #param.  Rewind past the name so the next
         * scan picks it up as a regular identifier. */
        if (L->in_pp_directive) {
            L->pos = (size_t)(start - L->source);
            return novai_make_token(L, NOVA_TOKEN_PP_STRINGIFY, loc);
        }
        /* Unknown directive at line start */
        novai_lex_error(L, "unknown preprocessor directive '#%.*s'",
                        (int)len, start);
        NovaToken tok = {0};
        tok.type = NOVA_TOKEN_ERROR;
        tok.loc = loc;
        return tok;
    }

    NovaToken tok = novai_make_token(L, directive, loc);
    tok.value.string.data = start;
    tok.value.string.len = len;

    /* Mark that we're inside a PP directive */
    L->in_pp_directive = 1;

    return tok;
}

/* ============================================================
 * MAIN TOKEN SCANNER
 *
 * This is the core of the lexer. It reads the next token from
 * the source and returns it. Handles all Nova syntax.
 * ============================================================ */

/**
 * @brief Scan the next token from source
 *
 * @param L Lexer state
 * @return Next token
 *
 * HOT PATH: Called for every token in the source
 */
static NovaToken novai_scan_token(NovaLexer *L) {

    /* ---- Interpolation mode: pending INTERP_END ---- */
    if (L->interp_pending_end) {
        L->interp_pending_end = 0;
        return novai_make_token(L, NOVA_TOKEN_INTERP_END, novai_loc(L));
    }

    /* ---- Interpolation mode: scan next literal segment ---- */
    if (L->in_interp && L->interp_brace_depth == 0) {
        return novai_scan_interp_continuation(L);
    }

    novai_skip_whitespace(L);

    /* Reset PP directive flag when we're on a new line.
     * Checked AFTER whitespace so indented # directives work. */
    if (L->at_line_start) {
        L->in_pp_directive = 0;
    }

    if (novai_at_end(L)) {
        return novai_make_token(L, NOVA_TOKEN_EOF, novai_loc(L));
    }

    NovaSourceLoc loc = novai_loc(L);
    char c = novai_peek(L);

    /* ---- Numbers ---- */
    if (nova_is_digit(c)) {
        return novai_scan_number(L);
    }

    /* ---- Leading dot: could be .  ..  ...  or .5 (float) ---- */
    if (c == '.' && nova_is_digit(novai_peek_at(L, 1))) {
        return novai_scan_number(L);
    }

    /* ---- Identifiers and keywords ---- */
    if (nova_is_alpha(c)) {
        return novai_scan_identifier(L);
    }

    /* ---- String literals ---- */
    if (c == '"' || c == '\'') {
        return novai_scan_string(L, c);
    }

    /* ---- Backtick interpolated strings ---- */
    if (c == '`') {
        return novai_scan_interp_start(L);
    }

    /* ---- Long strings [[ or [==[ ---- */
    if (c == '[') {
        int level = novai_count_long_bracket(L);
        if (level >= 0) {
            return novai_scan_long_string(L, level);
        }
    }

    /* ---- Closing } in interpolation expression ---- */
    if (c == '}' && L->in_interp && L->interp_brace_depth == 1) {
        novai_advance(L); /* consume } */
        L->interp_brace_depth = 0;
        /* Immediately scan the next literal segment */
        return novai_scan_interp_continuation(L);
    }

    /* ---- Consume the character ---- */
    int was_line_start = L->at_line_start;
    novai_advance(L);

    /* ---- Multi-character operators ---- */
    switch (c) {

    /* Dot, concat, vararg */
    case '.':
        if (novai_peek(L) == '.') {
            novai_advance(L);
            if (novai_peek(L) == '.') {
                novai_advance(L);
                return novai_make_token(L, NOVA_TOKEN_DOTDOTDOT, loc);
            }
            return novai_make_token(L, NOVA_TOKEN_DOTDOT, loc);
        }
        return novai_make_token(L, (NovaTokenType)'.', loc);

    /* Equals, comparison */
    case '=':
        if (novai_peek(L) == '=') {
            novai_advance(L);
            return novai_make_token(L, NOVA_TOKEN_EQ, loc);
        }
        return novai_make_token(L, (NovaTokenType)'=', loc);

    case '~':
        return novai_make_token(L, (NovaTokenType)'~', loc);

    case '!':
        if (novai_peek(L) == '=') {
            novai_advance(L);
            return novai_make_token(L, NOVA_TOKEN_NEQ, loc);
        }
        return novai_make_token(L, (NovaTokenType)'!', loc);

    case '<':
        if (novai_peek(L) == '=') {
            novai_advance(L);
            return novai_make_token(L, NOVA_TOKEN_LE, loc);
        }
        if (novai_peek(L) == '<') {
            novai_advance(L);
            return novai_make_token(L, NOVA_TOKEN_SHL, loc);
        }
        return novai_make_token(L, (NovaTokenType)'<', loc);

    case '>':
        if (novai_peek(L) == '=') {
            novai_advance(L);
            return novai_make_token(L, NOVA_TOKEN_GE, loc);
        }
        if (novai_peek(L) == '>') {
            novai_advance(L);
            return novai_make_token(L, NOVA_TOKEN_SHR, loc);
        }
        return novai_make_token(L, (NovaTokenType)'>', loc);

    case '/':
        if (novai_peek(L) == '/') {
            novai_advance(L);
            return novai_make_token(L, NOVA_TOKEN_IDIV, loc);
        }
        return novai_make_token(L, (NovaTokenType)'/', loc);

    case '-':
        /* Note: -- comments are handled in skip_whitespace */
        if (novai_peek(L) == '>') {
            novai_advance(L);
            return novai_make_token(L, NOVA_TOKEN_ARROW, loc);
        }
        return novai_make_token(L, (NovaTokenType)'-', loc);

    case ':':
        if (novai_peek(L) == ':') {
            novai_advance(L);
            return novai_make_token(L, NOVA_TOKEN_DBCOLON, loc);
        }
        return novai_make_token(L, (NovaTokenType)':', loc);

    /* Preprocessor: '#' at line start = directive, otherwise stringify */
    case '#':
        if (was_line_start || L->in_pp_directive) {
            return novai_scan_pp_directive(L);
        }
        /* Inside a macro expansion context: could be stringify */
        if (novai_peek(L) == '#') {
            novai_advance(L);
            return novai_make_token(L, NOVA_TOKEN_PP_PASTE, loc);
        }
        /* Otherwise treat as length operator (like Lua's #) */
        return novai_make_token(L, (NovaTokenType)'#', loc);

    /* Single-character tokens passed through directly */
    case '(':  case ')':
    case '[':  case ']':  case ',':  case ';':
    case '+':  case '*':  case '%':  case '^':
    case '&':  case '|':  case '@':
        return novai_make_token(L, (NovaTokenType)c, loc);

    /* Braces: track nesting depth in interpolation expressions */
    case '{':
        if (L->in_interp) {
            L->interp_brace_depth++;
        }
        return novai_make_token(L, (NovaTokenType)'{', loc);
    case '}':
        if (L->in_interp && L->interp_brace_depth > 0) {
            L->interp_brace_depth--;
        }
        return novai_make_token(L, (NovaTokenType)'}', loc);

    default:
        break;
    }

    /* Unknown character */
    novai_lex_error(L, "unexpected character '%c' (0x%02X)", c, (unsigned char)c);
    NovaToken tok = {0};
    tok.type = NOVA_TOKEN_ERROR;
    tok.loc = loc;
    return tok;
}

/* ============================================================
 * PUBLIC API IMPLEMENTATION
 * ============================================================ */

int nova_lex_init(NovaLexer *L, const char *source, size_t len,
                  const char *filename) {
    if (L == NULL || source == NULL) {
        return -1;
    }

    memset(L, 0, sizeof(NovaLexer));

    L->source = source;
    L->source_len = (len > 0) ? len : strlen(source);
    L->pos = 0;

    L->filename = filename ? filename : "<input>";
    L->line = 1;
    L->line_start = 0;

    L->current.type = NOVA_TOKEN_EOF;
    L->lookahead.type = NOVA_TOKEN_EOF;
    L->has_lookahead = 0;

    L->at_line_start = 1;
    L->in_pp_directive = 0;

    L->error_msg[0] = '\0';
    L->error_count = 0;

    /* Allocate initial string buffer */
    L->buffer_cap = NOVA_LEX_BUFFER_INIT;
    L->buffer = (char *)malloc(L->buffer_cap);
    if (L->buffer == NULL) {
        return -1;
    }
    L->buffer[0] = '\0';
    L->buffer_len = 0;

    /* Prime the pump: scan the first token */
    nova_lex_next(L);

    return 0;
}

void nova_lex_free(NovaLexer *L) {
    if (L == NULL) {
        return;
    }
    free(L->buffer);
    L->buffer = NULL;
    L->buffer_len = 0;
    L->buffer_cap = 0;
}

NovaTokenType nova_lex_next(NovaLexer *L) {
    if (L == NULL) {
        return NOVA_TOKEN_EOF;
    }

    /* If we have a cached lookahead, use it */
    if (L->has_lookahead) {
        L->current = L->lookahead;
        L->has_lookahead = 0;
    } else {
        L->current = novai_scan_token(L);
    }

    return L->current.type;
}

NovaTokenType nova_lex_peek(NovaLexer *L) {
    if (L == NULL) {
        return NOVA_TOKEN_EOF;
    }

    if (!L->has_lookahead) {
        L->lookahead = novai_scan_token(L);
        L->has_lookahead = 1;
    }

    return L->lookahead.type;
}

const NovaToken *nova_lex_current(const NovaLexer *L) {
    if (L == NULL) {
        return NULL;
    }
    return &L->current;
}

int nova_lex_check(const NovaLexer *L, NovaTokenType type) {
    if (L == NULL) {
        return 0;
    }
    return L->current.type == type ? 1 : 0;
}

int nova_lex_match(NovaLexer *L, NovaTokenType type) {
    if (L == NULL) {
        return 0;
    }
    if (L->current.type == type) {
        nova_lex_next(L);
        return 1;
    }
    return 0;
}

int nova_lex_expect(NovaLexer *L, NovaTokenType type) {
    if (L == NULL) {
        return -1;
    }
    if (L->current.type == type) {
        nova_lex_next(L);
        return 0;
    }
    novai_lex_error(L, "expected %s, got %s",
                    nova_token_name(type),
                    nova_token_name(L->current.type));
    return -1;
}

/* ============================================================
 * TOKEN NAME TABLE
 * ============================================================ */

/**
 * @brief Get human-readable name for a token type
 *
 * @param type Token type
 * @return Static string name (do not free)
 */
const char *nova_token_name(NovaTokenType type) {
    /* Single-character tokens */
    if (type > 0 && type < 256) {
        /* Return a static string for printable ASCII */
        static char single[4] = {'\'', 0, '\'', 0};
        single[1] = (char)type;
        return single;
    }

    switch (type) {
        case NOVA_TOKEN_EOF:            return "<eof>";
        case NOVA_TOKEN_ERROR:          return "<error>";

        /* Multi-character operators */
        case NOVA_TOKEN_DOTDOT:         return "'..'";
        case NOVA_TOKEN_DOTDOTDOT:      return "'...'";
        case NOVA_TOKEN_EQ:             return "'=='";
        case NOVA_TOKEN_NEQ:            return "'!='";
        case NOVA_TOKEN_LE:             return "'<='";
        case NOVA_TOKEN_GE:             return "'>='";
        case NOVA_TOKEN_SHL:            return "'<<'";
        case NOVA_TOKEN_SHR:            return "'>>'";
        case NOVA_TOKEN_IDIV:           return "'//'";
        case NOVA_TOKEN_CONCAT:         return "'..'";
        case NOVA_TOKEN_ARROW:          return "'->'";
        case NOVA_TOKEN_DBCOLON:        return "'::'";

        /* Literals */
        case NOVA_TOKEN_INTEGER:        return "<integer>";
        case NOVA_TOKEN_NUMBER:         return "<number>";
        case NOVA_TOKEN_STRING:         return "<string>";
        case NOVA_TOKEN_INTERP_STRING:  return "<interp_string>";
        case NOVA_TOKEN_LONG_STRING:    return "<long_string>";
        case NOVA_TOKEN_NAME:           return "<name>";

        /* String interpolation */
        case NOVA_TOKEN_INTERP_START:   return "<interp_start>";
        case NOVA_TOKEN_INTERP_SEGMENT: return "<interp_segment>";
        case NOVA_TOKEN_INTERP_END:     return "<interp_end>";

        /* Keywords */
        case NOVA_TOKEN_AND:            return "'and'";
        case NOVA_TOKEN_BREAK:          return "'break'";
        case NOVA_TOKEN_DO:             return "'do'";
        case NOVA_TOKEN_ELSE:           return "'else'";
        case NOVA_TOKEN_ELSEIF:         return "'elseif'";
        case NOVA_TOKEN_END:            return "'end'";
        case NOVA_TOKEN_FALSE:          return "'false'";
        case NOVA_TOKEN_FOR:            return "'for'";
        case NOVA_TOKEN_FUNCTION:       return "'function'";
        case NOVA_TOKEN_GOTO:           return "'goto'";
        case NOVA_TOKEN_IF:             return "'if'";
        case NOVA_TOKEN_IN:             return "'in'";
        case NOVA_TOKEN_LOCAL:          return "'dec'";
        case NOVA_TOKEN_NIL:            return "'nil'";
        case NOVA_TOKEN_NOT:            return "'not'";
        case NOVA_TOKEN_OR:             return "'or'";
        case NOVA_TOKEN_REPEAT:         return "'repeat'";
        case NOVA_TOKEN_RETURN:         return "'return'";
        case NOVA_TOKEN_THEN:           return "'then'";
        case NOVA_TOKEN_TRUE:           return "'true'";
        case NOVA_TOKEN_UNTIL:          return "'until'";
        case NOVA_TOKEN_WHILE:          return "'while'";

        /* Nova extensions */
        case NOVA_TOKEN_IMPORT:         return "'import'";
        case NOVA_TOKEN_EXPORT:         return "'export'";
        case NOVA_TOKEN_CONST:          return "'const'";

        /* Preprocessor */
        case NOVA_TOKEN_PP_INCLUDE:     return "'#include'";
        case NOVA_TOKEN_PP_IMPORT:      return "'#import'";
        case NOVA_TOKEN_PP_DEFINE:      return "'#define'";
        case NOVA_TOKEN_PP_UNDEF:       return "'#undef'";
        case NOVA_TOKEN_PP_IFDEF:       return "'#ifdef'";
        case NOVA_TOKEN_PP_IFNDEF:      return "'#ifndef'";
        case NOVA_TOKEN_PP_IF:          return "'#if'";
        case NOVA_TOKEN_PP_ELIF:        return "'#elif'";
        case NOVA_TOKEN_PP_ELSE:        return "'#else'";
        case NOVA_TOKEN_PP_ENDIF:       return "'#endif'";
        case NOVA_TOKEN_PP_PRAGMA:      return "'#pragma'";
        case NOVA_TOKEN_PP_ERROR:       return "'#error'";
        case NOVA_TOKEN_PP_WARNING:     return "'#warning'";
        case NOVA_TOKEN_PP_LINE:        return "'#line'";
        case NOVA_TOKEN_PP_STRINGIFY:   return "'#'";
        case NOVA_TOKEN_PP_PASTE:       return "'##'";

        default:
            return "<unknown>";
    }
}

const char *nova_lex_error(const NovaLexer *L) {
    if (L == NULL) {
        return "lexer is NULL";
    }
    return L->error_msg;
}
