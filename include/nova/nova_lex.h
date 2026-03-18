/**
 * @file nova_lex.h
 * @brief Nova Language - Lexer (Tokenizer)
 *
 * Single-pass lexer that converts Nova source text into a stream
 * of tokens. Handles all Nova syntax including preprocessor
 * directives, string interpolation, and Lua-compatible constructs.
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
 *   - nova_conf.h
 *   - nova_object.h (NovaString for identifiers)
 */

#ifndef NOVA_LEX_H
#define NOVA_LEX_H

#include "nova_conf.h"

#include <stddef.h>
#include <stdint.h>

/* ============================================================
 * TOKEN TYPES
 * ============================================================ */

/**
 * @brief Token type enumeration
 *
 * Single-character tokens use their ASCII value directly.
 * Multi-character tokens and keywords start at 256.
 */
typedef enum {
    /* End of file / error */
    NOVA_TOKEN_EOF      = 0,
    NOVA_TOKEN_ERROR    = -1,

    /* Single-character tokens (use ASCII value) */
    /* '(' ')' '{' '}' '[' ']' ',' ';' ':' '.' '+' '-' '*' '/' '%' '^'
       '#' '~' '<' '>' '=' '&' '|' '!' '@' */

    /* Multi-character operators (starting at 256) */
    NOVA_TOKEN_DOTDOT   = 256,  /* ..                */
    NOVA_TOKEN_DOTDOTDOT,       /* ...               */
    NOVA_TOKEN_EQ,              /* ==                */
    NOVA_TOKEN_NEQ,             /* !=                */
    NOVA_TOKEN_LE,              /* <=                */
    NOVA_TOKEN_GE,              /* >=                */
    NOVA_TOKEN_SHL,             /* <<                */
    NOVA_TOKEN_SHR,             /* >>                */
    NOVA_TOKEN_IDIV,            /* //                */
    NOVA_TOKEN_CONCAT,          /* .. (same as DOTDOT, context-dependent) */
    NOVA_TOKEN_ARROW,           /* ->                */
    NOVA_TOKEN_DBCOLON,         /* :: (goto labels)  */

    /* Literals */
    NOVA_TOKEN_INTEGER,         /* Integer literal   */
    NOVA_TOKEN_NUMBER,          /* Float literal     */
    NOVA_TOKEN_STRING,          /* String literal    */
    NOVA_TOKEN_INTERP_STRING,   /* `interpolated` (legacy, unused) */
    NOVA_TOKEN_LONG_STRING,     /* [[long string]]   */
    NOVA_TOKEN_NAME,            /* Identifier        */

    /* String interpolation tokens */
    NOVA_TOKEN_INTERP_START,    /* Opening backtick  */
    NOVA_TOKEN_INTERP_SEGMENT,  /* Literal text segment in `...` */
    NOVA_TOKEN_INTERP_END,      /* Closing backtick  */

    /* Keywords (alphabetical) */
    NOVA_TOKEN_AND,             /* and               */
    NOVA_TOKEN_BREAK,           /* break             */
    NOVA_TOKEN_CONTINUE,        /* continue          */
    NOVA_TOKEN_DO,              /* do                */
    NOVA_TOKEN_ELSE,            /* else              */
    NOVA_TOKEN_ELSEIF,          /* elseif            */
    NOVA_TOKEN_END,             /* end               */
    NOVA_TOKEN_FALSE,           /* false             */
    NOVA_TOKEN_FOR,             /* for               */
    NOVA_TOKEN_FUNCTION,        /* function          */
    NOVA_TOKEN_GOTO,            /* goto              */
    NOVA_TOKEN_IF,              /* if                */
    NOVA_TOKEN_IN,              /* in                */
    NOVA_TOKEN_LOCAL,           /* dec / declare     */
    NOVA_TOKEN_NIL,             /* nil               */
    NOVA_TOKEN_NOT,             /* not               */
    NOVA_TOKEN_OR,              /* or                */
    NOVA_TOKEN_REPEAT,          /* repeat            */
    NOVA_TOKEN_RETURN,          /* return            */
    NOVA_TOKEN_THEN,            /* then              */
    NOVA_TOKEN_TRUE,            /* true              */
    NOVA_TOKEN_UNTIL,           /* until             */
    NOVA_TOKEN_WHILE,           /* while             */

    /* Nova extensions (not in standard Lua) */
    NOVA_TOKEN_ASYNC,           /* async             */
    NOVA_TOKEN_AWAIT,           /* await             */
    NOVA_TOKEN_CONST,           /* const             */
    NOVA_TOKEN_ENUM,            /* enum              */
    NOVA_TOKEN_EXPORT,          /* export            */
    NOVA_TOKEN_IMPORT,          /* import            */
    NOVA_TOKEN_SPAWN,           /* spawn             */
    NOVA_TOKEN_STRUCT,          /* struct             */
    NOVA_TOKEN_TYPEDEC,         /* typedec            */
    NOVA_TOKEN_YIELD,           /* yield             */

    /* Preprocessor directives */
    NOVA_TOKEN_PP_INCLUDE,      /* #include          */
    NOVA_TOKEN_PP_IMPORT,       /* #import           */
    NOVA_TOKEN_PP_REQUIRE,      /* #require          */
    NOVA_TOKEN_PP_DEFINE,       /* #define           */
    NOVA_TOKEN_PP_UNDEF,        /* #undef            */
    NOVA_TOKEN_PP_IFDEF,        /* #ifdef            */
    NOVA_TOKEN_PP_IFNDEF,       /* #ifndef           */
    NOVA_TOKEN_PP_IF,           /* #if               */
    NOVA_TOKEN_PP_ELIF,         /* #elif             */
    NOVA_TOKEN_PP_ELSE,         /* #else             */
    NOVA_TOKEN_PP_ENDIF,        /* #endif            */
    NOVA_TOKEN_PP_PRAGMA,       /* #pragma           */
    NOVA_TOKEN_PP_ERROR,        /* #error            */
    NOVA_TOKEN_PP_WARNING,      /* #warning          */
    NOVA_TOKEN_PP_LINE,         /* #line             */

    /* Preprocessor-specific tokens */
    NOVA_TOKEN_PP_STRINGIFY,    /* # (in macro body) */
    NOVA_TOKEN_PP_PASTE,        /* ## (token paste)  */

    NOVA_TOKEN_COUNT            /* Total token types (internal) */
} NovaTokenType;

/* ============================================================
 * TOKEN STRUCTURE
 * ============================================================ */

/**
 * @brief Source location for error reporting
 */
typedef struct {
    const char *filename;   /**< Source file name                */
    int         line;       /**< Line number (1-based)           */
    int         column;     /**< Column number (1-based)         */
} NovaSourceLoc;

/**
 * @brief Token with value and location
 */
typedef struct {
    NovaTokenType type;     /**< Token type                      */
    NovaSourceLoc loc;      /**< Source location                  */

    union {
        nova_number_t number;   /**< For NOVA_TOKEN_NUMBER       */
        nova_int_t    integer;  /**< For NOVA_TOKEN_INTEGER      */
        struct {
            const char *data;   /**< String data (not owned)     */
            size_t      len;    /**< String length               */
        } string;               /**< For NOVA_TOKEN_STRING, NAME */
    } value;
} NovaToken;

/* ============================================================
 * LEXER STATE
 * ============================================================ */

/**
 * @brief Lexer state
 *
 * Maintains all state needed for tokenization.
 * One lexer instance per source file.
 */
typedef struct {
    /* Source input */
    const char *source;         /**< Source text (not owned)      */
    size_t      source_len;     /**< Total source length          */
    size_t      pos;            /**< Current position in source   */

    /* Location tracking */
    const char *filename;       /**< Source filename              */
    int         line;           /**< Current line (1-based)       */
    int         line_start;     /**< Position of current line start */

    /* Current and lookahead tokens */
    NovaToken   current;        /**< Current token                */
    NovaToken   lookahead;      /**< Lookahead token              */
    int         has_lookahead;  /**< Is lookahead valid?          */

    /* State flags */
    int         at_line_start;  /**< At beginning of a line?      */
    int         in_pp_directive;/**< Inside preprocessor directive?*/

    /* Error state */
    char        error_msg[256]; /**< Last error message           */
    int         error_count;    /**< Number of errors encountered */

    /* String buffer for token values */
    char       *buffer;         /**< Token string buffer          */
    size_t      buffer_len;     /**< Used bytes in buffer         */
    size_t      buffer_cap;     /**< Buffer capacity              */

    /* String interpolation state */
    int         in_interp;          /**< Inside backtick string?      */
    int         interp_brace_depth; /**< Brace nesting in ${...}      */
    int         interp_pending_end; /**< Need to emit INTERP_END next */
} NovaLexer;

/* ============================================================
 * LEXER API
 * ============================================================ */

/**
 * @brief Initialize a lexer for a source string
 *
 * @param L         Lexer state to initialize
 * @param source    Source code (must remain valid for lexer lifetime)
 * @param len       Source length (0 = compute via strlen)
 * @param filename  Source filename for error messages
 * @return 0 on success, -1 on error
 *
 * @pre L != NULL && source != NULL
 */
int nova_lex_init(NovaLexer *L, const char *source, size_t len,
                  const char *filename);

/**
 * @brief Free lexer resources
 *
 * @param L Lexer state (NULL is safe/no-op)
 */
void nova_lex_free(NovaLexer *L);

/**
 * @brief Advance to the next token
 *
 * Returns the current token type and advances the lexer.
 *
 * @param L Lexer state
 * @return Token type of the consumed token
 *
 * @pre L != NULL
 */
NovaTokenType nova_lex_next(NovaLexer *L);

/**
 * @brief Peek at the next token without consuming
 *
 * @param L Lexer state
 * @return Token type of the next token
 */
NovaTokenType nova_lex_peek(NovaLexer *L);

/**
 * @brief Get the current token
 *
 * @param L Lexer state
 * @return Pointer to current token (valid until next nova_lex_next)
 */
const NovaToken *nova_lex_current(const NovaLexer *L);

/**
 * @brief Check if current token matches expected type
 *
 * @param L     Lexer state
 * @param type  Expected token type
 * @return 1 if match, 0 if not
 */
int nova_lex_check(const NovaLexer *L, NovaTokenType type);

/**
 * @brief Consume current token if it matches expected type
 *
 * @param L     Lexer state
 * @param type  Expected token type
 * @return 1 if consumed, 0 if no match (token not consumed)
 */
int nova_lex_match(NovaLexer *L, NovaTokenType type);

/**
 * @brief Expect current token to be of given type, error if not
 *
 * Consumes the token if it matches. Sets error if it doesn't.
 *
 * @param L     Lexer state
 * @param type  Expected token type
 * @return 0 on success, -1 on error
 */
int nova_lex_expect(NovaLexer *L, NovaTokenType type);

/**
 * @brief Get string representation of a token type
 *
 * @param type Token type
 * @return Human-readable string (static, do not free)
 */
const char *nova_token_name(NovaTokenType type);

/**
 * @brief Get the last lexer error message
 *
 * @param L Lexer state
 * @return Error message string (valid until next error)
 */
const char *nova_lex_error(const NovaLexer *L);

#endif /* NOVA_LEX_H */
