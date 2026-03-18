/**
 * @file nova_pp.c
 * @brief Nova Language - Token-Based Preprocessor
 *
 * Implements a C-like preprocessor that operates on the token stream
 * produced by the Nova lexer. Supports:
 *
 *   - #include "file" / #include <file>   (file inclusion)
 *   - #define NAME [value]                (object-like macros)
 *   - #define NAME(args...) body          (function-like macros)
 *   - #undef NAME                         (undefine)
 *   - #ifdef / #ifndef / #if / #elif      (conditional compilation)
 *   - #else / #endif
 *   - #error / #warning / #line / #pragma
 *   - # (stringify) and ## (token paste)  (inside macro bodies)
 *   - __FILE__, __LINE__, __COUNTER__     (predefined macros)
 *   - Variadic macros with __VA_ARGS__
 *
 * Design philosophy:
 *   Token-based rather than text-based. This gives us hygienic
 *   expansion, accurate locations through macro expansions, and
 *   feeds directly into the parser's AOT token stream.
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
 *   - nova_pp.h (public API)
 *   - nova_lex.h (lexer for tokenization)
 *   - zorya/dagger.h (O(1) macro lookup)
 */

#include "nova/nova_pp.h"
#include "zorya/dagger.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <inttypes.h>

/* ============================================================
 * INTERNAL CONSTANTS
 * ============================================================ */

/** Initial capacity for the output token buffer */
#define NOVA_PP_OUTPUT_INIT     256

/** Growth factor for the output buffer */
#define NOVA_PP_OUTPUT_GROW     2

/** Initial capacity for the macro table */
#define NOVA_PP_MACRO_TABLE_CAP 64

/** Maximum length for file read buffer */
#define NOVA_PP_MAX_FILE_SIZE   (16 * 1024 * 1024)

/** Maximum search path entries */
#define NOVA_PP_MAX_SEARCH_PATHS 32

/* ============================================================
 * INTERNAL HELPERS
 * ============================================================ */

/**
 * @brief Set an error on the preprocessor
 *
 * @param pp  Preprocessor state
 * @param loc Source location (can be NULL)
 * @param fmt Printf-style format string
 */
static void novai_pp_error(NovaPP *pp, const NovaSourceLoc *loc,
                           const char *fmt, ...) {
    va_list args;
    char msg[400] = {0};

    va_start(args, fmt);
    (void)vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);

    if (loc != NULL && loc->filename != NULL) {
        (void)snprintf(pp->error_msg, sizeof(pp->error_msg),
                       "%s:%d:%d: error: %s",
                       loc->filename, loc->line, loc->column, msg);
    } else {
        (void)snprintf(pp->error_msg, sizeof(pp->error_msg),
                       "error: %s", msg);
    }

    pp->error_count++;
}

/**
 * @brief Set a warning on the preprocessor
 */
static void novai_pp_warning(NovaPP *pp, const NovaSourceLoc *loc,
                             const char *fmt, ...) {
    va_list args;
    char msg[400] = {0};

    va_start(args, fmt);
    (void)vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);

    if (loc != NULL && loc->filename != NULL) {
        (void)snprintf(pp->error_msg, sizeof(pp->error_msg),
                       "%s:%d:%d: warning: %s",
                       loc->filename, loc->line, loc->column, msg);
    } else {
        (void)snprintf(pp->error_msg, sizeof(pp->error_msg),
                       "warning: %s", msg);
    }

    pp->warning_count++;
}

/**
 * @brief Duplicate a string with explicit length
 *
 * @param str  Source string
 * @param len  Length to copy
 * @return Newly allocated NUL-terminated copy, or NULL
 */
static char *novai_strndup(const char *str, size_t len) {
    if (str == NULL) {
        return NULL;
    }
    char *dup = (char *)malloc(len + 1);
    if (dup == NULL) {
        return NULL;
    }
    memcpy(dup, str, len);
    dup[len] = '\0';
    return dup;
}

/* ============================================================
 * MACRO MANAGEMENT
 * ============================================================ */

/**
 * @brief Free a macro definition and its owned data
 */
static void novai_macro_destroy(void *value) {
    NovaMacroDef *def = (NovaMacroDef *)value;
    if (def == NULL) {
        return;
    }

    /* Free owned copies of name */
    free((void *)def->name);

    /* Free parameter names */
    if (def->params != NULL) {
        for (int i = 0; i < def->param_count; i++) {
            free((void *)def->params[i].name);
        }
        free(def->params);
    }

    /* Free body tokens (text was strndup'd) */
    if (def->body != NULL) {
        for (int i = 0; i < def->body_count; i++) {
            free((void *)def->body[i].text);
        }
        free(def->body);
    }

    free(def);
}

/**
 * @brief Look up a macro by name
 *
 * @param pp   Preprocessor state
 * @param name Macro name
 * @param len  Name length
 * @return Macro definition, or NULL if not found
 */
static NovaMacroDef *novai_macro_find(const NovaPP *pp,
                                      const char *name, size_t len) {
    if (pp->macros == NULL) {
        return NULL;
    }

    void *value = NULL;
    dagger_result_t res = dagger_get(
        (const DaggerTable *)pp->macros,
        name, (uint32_t)len,
        &value
    );

    if (res == DAGGER_OK) {
        return (NovaMacroDef *)value;
    }
    return NULL;
}

/**
 * @brief Store a macro definition in the macro table
 *
 * @param pp  Preprocessor state
 * @param def Macro definition (ownership transferred)
 * @return 0 on success, -1 on error
 */
static int novai_macro_store(NovaPP *pp, NovaMacroDef *def) {
    if (pp->macros == NULL) {
        return -1;
    }

    dagger_result_t res = dagger_set(
        (DaggerTable *)pp->macros,
        def->name, (uint32_t)def->name_len,
        def,
        1  /* replace existing */
    );

    if (res < 0) {
        novai_pp_error(pp, NULL, "failed to store macro '%s'", def->name);
        return -1;
    }
    return 0;
}

/* ============================================================
 * OUTPUT BUFFER
 *
 * The preprocessor builds up an output token buffer as it
 * processes directives and expands macros. The parser reads
 * from this buffer via nova_pp_next_token().
 * ============================================================ */

/**
 * @brief Ensure the output buffer has room for one more token
 *
 * @param pp Preprocessor state
 * @return 0 on success, -1 on allocation failure
 */
static int novai_output_grow(NovaPP *pp) {
    if (pp->output_count < pp->output_cap) {
        return 0;
    }

    size_t new_cap = (pp->output_cap == 0)
        ? NOVA_PP_OUTPUT_INIT
        : pp->output_cap * NOVA_PP_OUTPUT_GROW;

    NovaToken *new_buf = (NovaToken *)realloc(
        pp->output, new_cap * sizeof(NovaToken)
    );
    if (new_buf == NULL) {
        novai_pp_error(pp, NULL, "out of memory in preprocessor output");
        return -1;
    }

    pp->output = new_buf;
    pp->output_cap = new_cap;
    return 0;
}

/**
 * @brief Check if a token type requires string data duplication
 *
 * Tokens with string.data pointing to lexer internal buffer need to
 * have their string data copied to persistent storage.
 *
 * @param type Token type to check
 * @return 1 if string needs duplication, 0 otherwise
 */
static int novai_token_needs_string_dup(NovaTokenType type) {
    switch (type) {
        case NOVA_TOKEN_NAME:
        case NOVA_TOKEN_STRING:
        case NOVA_TOKEN_LONG_STRING:
        case NOVA_TOKEN_INTERP_STRING:
        case NOVA_TOKEN_INTERP_SEGMENT:
        case NOVA_TOKEN_ERROR:
            return 1;
        default:
            break;
    }
    /* Keyword tokens (NOVA_TOKEN_AND..NOVA_TOKEN_YIELD) also carry
     * string data pointing into the lexer source buffer.  The parser
     * reads keyword string data when keywords are used as field or
     * method names (e.g. coroutine.yield, table.insert).  We must
     * duplicate them here so the pointers survive content buffer
     * freeing in novai_process_tokens. */
    if (type >= NOVA_TOKEN_AND && type <= NOVA_TOKEN_YIELD) {
        return 1;
    }
    return 0;
}

/**
 * @brief Push a token onto the output buffer
 *
 * For tokens with string data that points to the lexer's internal buffer,
 * the string is duplicated to ensure it persists.
 *
 * @param pp  Preprocessor state
 * @param tok Token to append
 * @return 0 on success, -1 on error
 */
static int novai_output_push(NovaPP *pp, const NovaToken *tok) {
    if (novai_output_grow(pp) != 0) {
        return -1;
    }
    pp->output[pp->output_count] = *tok;

    /* Duplicate string data for tokens that reference lexer buffer.
     * Must always dup, even for zero-length strings, to avoid a
     * dangling pointer into the lexer buffer after nova_lex_free. */
    if (novai_token_needs_string_dup(tok->type)) {
        if (tok->value.string.data != NULL) {
            char *dup = novai_strndup(tok->value.string.data,
                                      tok->value.string.len);
            if (dup == NULL) {
                return -1;  /* allocation failed */
            }
            pp->output[pp->output_count].value.string.data = dup;
        }
    }

    pp->output_count++;
    return 0;
}

/* ============================================================
 * FILE I/O
 * ============================================================ */

/**
 * @brief Read an entire file into memory
 *
 * @param path     File path
 * @param out_size Output: file size
 * @return Allocated buffer with file contents, or NULL on error
 *
 * @post Caller must free() the returned buffer
 */
static char *novai_read_file(const char *path, size_t *out_size) {
    FILE *fp = fopen(path, "rb");
    if (fp == NULL) {
        return NULL;
    }

    /* Get file size */
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return NULL;
    }
    long fsize = ftell(fp);
    if (fsize < 0 || (unsigned long)fsize > NOVA_PP_MAX_FILE_SIZE) {
        fclose(fp);
        return NULL;
    }
    if (fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        return NULL;
    }

    size_t size = (size_t)fsize;
    char *buf = (char *)malloc(size + 1);
    if (buf == NULL) {
        fclose(fp);
        return NULL;
    }

    size_t read_bytes = fread(buf, 1, size, fp);
    fclose(fp);

    if (read_bytes != size) {
        free(buf);
        return NULL;
    }

    buf[size] = '\0';
    if (out_size != NULL) {
        *out_size = size;
    }
    return buf;
}

/**
 * @brief Resolve an include path
 *
 * Searches the include search paths for the requested file.
 *
 * @param pp       Preprocessor state
 * @param name     Requested filename
 * @param name_len Filename length
 * @param buf      Output path buffer
 * @param buf_size Buffer size
 * @return 0 if found, -1 if not found
 */

/**
 * @brief Check if a filename already has an extension.
 * @return 1 if has extension (contains '.' after last '/'), 0 otherwise.
 */
static int novai_has_extension(const char *name, size_t len) {
    size_t i = len;
    while (i > 0) {
        i--;
        if (name[i] == '/') {
            return 0;   /* reached directory separator, no dot found */
        }
        if (name[i] == '.') {
            return 1;
        }
    }
    return 0;
}

/**
 * @brief Try to open a file, optionally appending ".m" extension.
 *
 * First tries the exact path in buf. If that fails and the filename
 * has no extension, tries buf + ".m".
 *
 * @param buf       Path buffer (must have room for 2 extra bytes)
 * @param path_len  Current string length in buf
 * @param buf_size  Total buffer capacity
 * @param try_m     If nonzero, try appending ".m" on failure
 * @return 0 if found (buf contains resolved path), -1 if not found
 */
static int novai_probe_path(char *buf, size_t path_len,
                            size_t buf_size, int try_m) {
    /* Try exact path */
    FILE *probe = fopen(buf, "r");
    if (probe != NULL) {
        fclose(probe);
        return 0;
    }

    /* Try with .m extension if the name has no extension */
    if (try_m && path_len + 2 < buf_size) {
        buf[path_len] = '.';
        buf[path_len + 1] = 'm';
        buf[path_len + 2] = '\0';

        probe = fopen(buf, "r");
        if (probe != NULL) {
            fclose(probe);
            return 0;
        }
        /* Restore original */
        buf[path_len] = '\0';
    }

    return -1;
}

static int novai_resolve_include(const NovaPP *pp,
                                 const char *name, size_t name_len,
                                 char *buf, size_t buf_size) {
    int try_m = !novai_has_extension(name, name_len);

    /* First, try relative to the current file's directory */
    if (pp->file_depth > 0) {
        const char *cur_path = pp->file_stack[pp->file_depth - 1].path;
        if (cur_path != NULL) {
            const char *last_slash = strrchr(cur_path, '/');
            if (last_slash != NULL) {
                size_t dir_len = (size_t)(last_slash - cur_path + 1);
                if (dir_len + name_len + 3 <= buf_size) {
                    memcpy(buf, cur_path, dir_len);
                    memcpy(buf + dir_len, name, name_len);
                    buf[dir_len + name_len] = '\0';

                    if (novai_probe_path(buf, dir_len + name_len,
                                         buf_size, try_m) == 0) {
                        return 0;
                    }
                }
            }
        }
    }

    /* Then try each search path */
    for (int i = 0; i < pp->search_path_count; i++) {
        size_t dlen = strlen(pp->search_paths[i]);
        /* path + '/' + name + '.m' + '\0' */
        if (dlen + 1 + name_len + 3 <= buf_size) {
            memcpy(buf, pp->search_paths[i], dlen);
            buf[dlen] = '/';
            memcpy(buf + dlen + 1, name, name_len);
            buf[dlen + 1 + name_len] = '\0';

            if (novai_probe_path(buf, dlen + 1 + name_len,
                                 buf_size, try_m) == 0) {
                return 0;
            }
        }
    }

    return -1;
}

/* ============================================================
 * LEXER HELPER
 *
 * The PP wraps the lexer to provide unified access to the
 * current file's token stream.
 * ============================================================ */

/**
 * @brief Get the current lexer (top of file stack)
 */
static inline NovaLexer *novai_current_lexer(NovaPP *pp) {
    if (pp->file_depth <= 0) {
        return NULL;
    }
    return &pp->file_stack[pp->file_depth - 1].lexer;
}

/**
 * @brief Advance the current lexer and return token type
 */
static inline NovaTokenType novai_lex_advance(NovaPP *pp) {
    NovaLexer *lex = novai_current_lexer(pp);
    if (lex == NULL) {
        return NOVA_TOKEN_EOF;
    }
    return nova_lex_next(lex);
}

/**
 * @brief Get the current token from the active lexer
 */
static inline const NovaToken *novai_lex_current(NovaPP *pp) {
    NovaLexer *lex = novai_current_lexer(pp);
    if (lex == NULL) {
        return NULL;
    }
    return nova_lex_current(lex);
}

/**
 * @brief Skip tokens until end of line (for PP directive arguments)
 *
 * Consumes tokens until we see a token on a different line.
 * Used to skip the rest of a PP directive line.
 */
static void novai_skip_to_eol(NovaPP *pp) {
    NovaLexer *lex = novai_current_lexer(pp);
    if (lex == NULL) {
        return;
    }

    int start_line = nova_lex_current(lex)->loc.line;
    while (nova_lex_current(lex)->type != NOVA_TOKEN_EOF) {
        if (nova_lex_current(lex)->loc.line != start_line) {
            break;
        }
        nova_lex_next(lex);
    }
}

/* ============================================================
 * CONDITIONAL COMPILATION
 *
 * Manages #if / #ifdef / #ifndef / #elif / #else / #endif.
 * Uses a stack of NovaPPCondEntry to track nesting.
 * ============================================================ */

/**
 * @brief Check if code is currently active (not skipped)
 *
 * Code is active if ALL conditions in the stack are active.
 */
static int novai_cond_active(const NovaPP *pp) {
    for (int i = 0; i < pp->cond_depth; i++) {
        if (!pp->cond_stack[i].active) {
            return 0;
        }
    }
    return 1;
}

/**
 * @brief Push a new conditional state onto the stack
 *
 * @param pp     Preprocessor state
 * @param active Is this branch active?
 * @param loc    Source location
 * @return 0 on success, -1 on stack overflow
 */
static int novai_cond_push(NovaPP *pp, int active,
                           const NovaSourceLoc *loc) {
    if (pp->cond_depth >= NOVA_PP_MAX_IF_DEPTH) {
        novai_pp_error(pp, loc,
                       "#if nesting too deep (max %d)",
                       NOVA_PP_MAX_IF_DEPTH);
        return -1;
    }

    NovaPPCondEntry *entry = &pp->cond_stack[pp->cond_depth];
    entry->active = active;
    entry->seen_true = active;
    entry->is_else = 0;

    pp->cond_depth++;
    return 0;
}

/**
 * @brief Handle #elif
 */
static int novai_cond_elif(NovaPP *pp, int condition,
                           const NovaSourceLoc *loc) {
    if (pp->cond_depth <= 0) {
        novai_pp_error(pp, loc, "#elif without matching #if");
        return -1;
    }

    NovaPPCondEntry *top = &pp->cond_stack[pp->cond_depth - 1];
    if (top->is_else) {
        novai_pp_error(pp, loc, "#elif after #else");
        return -1;
    }

    if (top->seen_true) {
        /* Already found a true branch -- skip this one */
        top->active = 0;
    } else if (condition) {
        top->active = 1;
        top->seen_true = 1;
    } else {
        top->active = 0;
    }

    return 0;
}

/**
 * @brief Handle #else
 */
static int novai_cond_else(NovaPP *pp, const NovaSourceLoc *loc) {
    if (pp->cond_depth <= 0) {
        novai_pp_error(pp, loc, "#else without matching #if");
        return -1;
    }

    NovaPPCondEntry *top = &pp->cond_stack[pp->cond_depth - 1];
    if (top->is_else) {
        novai_pp_error(pp, loc, "duplicate #else");
        return -1;
    }

    top->is_else = 1;
    top->active = !top->seen_true;

    return 0;
}

/**
 * @brief Handle #endif
 */
static int novai_cond_endif(NovaPP *pp, const NovaSourceLoc *loc) {
    if (pp->cond_depth <= 0) {
        novai_pp_error(pp, loc, "#endif without matching #if");
        return -1;
    }
    pp->cond_depth--;
    return 0;
}

/* ============================================================
 * PREDEFINED MACROS
 * ============================================================ */

/**
 * @brief Define the built-in predefined macros
 *
 * - __NOVA__        : always defined (value = version number)
 * - __NOVA_VERSION__: version string (from nova_conf.h)
 */
static int novai_define_predefined(NovaPP *pp) {
    int rc = 0;

    /* Stringify version components from nova_conf.h */
    #define NOVAI_STR_HELPER(x) #x
    #define NOVAI_STR(x) NOVAI_STR_HELPER(x)

    rc |= nova_pp_define(pp, "__NOVA__",
                         NOVA_VERSION_STRING);
    rc |= nova_pp_define(pp, "__NOVA_VERSION__",
                         NOVA_VERSION_STRING);
    rc |= nova_pp_define(pp, "__NOVA_MAJOR__",
                         NOVAI_STR(NOVA_VERSION_MAJOR));
    rc |= nova_pp_define(pp, "__NOVA_MINOR__",
                         NOVAI_STR(NOVA_VERSION_MINOR));
    rc |= nova_pp_define(pp, "__NOVA_PATCH__",
                         NOVAI_STR(NOVA_VERSION_PATCH));

    #undef NOVAI_STR
    #undef NOVAI_STR_HELPER

    return rc;
}

/* ============================================================
 * DIRECTIVE: #define
 *
 * Object-like:    #define NAME replacement_tokens...
 * Function-like:  #define NAME(a, b, ...) body_tokens...
 *
 * The body is stored as a sequence of NovaMacroTokens.
 * Parameter references are resolved at definition time and
 * stored as param_idx in the body tokens.
 * ============================================================ */

/**
 * @brief Find a parameter name in the param list
 *
 * @return Parameter index (>= 0), or -1 if not found
 */
static int novai_find_param(const NovaMacroDef *def,
                            const char *name, size_t len) {
    for (int i = 0; i < def->param_count; i++) {
        if (def->params[i].name_len == len &&
            memcmp(def->params[i].name, name, len) == 0) {
            return i;
        }
    }
    /* Check for __VA_ARGS__ */
    if (def->is_variadic && len == 11 &&
        memcmp(name, "__VA_ARGS__", 11) == 0) {
        return def->param_count; /* Index after last named param */
    }
    return -1;
}

/**
 * @brief Parse function-like macro parameters: (a, b, ...)
 *
 * @param pp  Preprocessor state
 * @param def Macro definition being built
 * @param loc Location for error messages
 * @return 0 on success, -1 on error
 */
static int novai_parse_macro_params(NovaPP *pp, NovaMacroDef *def,
                                    const NovaSourceLoc *loc) {
    NovaLexer *lex = novai_current_lexer(pp);
    if (lex == NULL) {
        return -1;
    }

    /* Current token should be '(' */
    nova_lex_next(lex); /* consume '(' */

    /* Allocate parameter array */
    def->params = (NovaMacroParam *)calloc(
        NOVA_PP_MAX_MACRO_PARAMS, sizeof(NovaMacroParam)
    );
    if (def->params == NULL) {
        novai_pp_error(pp, loc, "out of memory for macro parameters");
        return -1;
    }

    /* Empty parameter list: #define FOO() */
    if (nova_lex_current(lex)->type == (NovaTokenType)')') {
        nova_lex_next(lex); /* consume ')' */
        return 0;
    }

    while (1) {
        const NovaToken *tok = nova_lex_current(lex);

        /* Check for variadic: ... */
        if (tok->type == NOVA_TOKEN_DOTDOTDOT) {
            def->is_variadic = 1;
            nova_lex_next(lex);

            if (nova_lex_current(lex)->type != (NovaTokenType)')') {
                novai_pp_error(pp, &tok->loc,
                               "'...' must be last parameter");
                return -1;
            }
            nova_lex_next(lex); /* consume ')' */
            return 0;
        }

        /* Expected: parameter name */
        if (tok->type != NOVA_TOKEN_NAME) {
            novai_pp_error(pp, &tok->loc,
                           "expected parameter name, got %s",
                           nova_token_name(tok->type));
            return -1;
        }

        if (def->param_count >= NOVA_PP_MAX_MACRO_PARAMS) {
            novai_pp_error(pp, &tok->loc,
                           "too many macro parameters (max %d)",
                           NOVA_PP_MAX_MACRO_PARAMS);
            return -1;
        }

        /* Store parameter */
        def->params[def->param_count].name =
            novai_strndup(tok->value.string.data, tok->value.string.len);
        def->params[def->param_count].name_len = tok->value.string.len;

        if (def->params[def->param_count].name == NULL) {
            novai_pp_error(pp, loc, "out of memory");
            return -1;
        }

        def->param_count++;
        nova_lex_next(lex);

        /* Next should be ',' or ')' */
        tok = nova_lex_current(lex);
        if (tok->type == (NovaTokenType)')') {
            nova_lex_next(lex); /* consume ')' */
            return 0;
        }

        if (tok->type == (NovaTokenType)',') {
            nova_lex_next(lex); /* consume ',' */
        } else {
            novai_pp_error(pp, &tok->loc,
                           "expected ',' or ')' in macro parameters");
            return -1;
        }
    }
}

/**
 * @brief Read macro body tokens until end of line
 *
 * @param pp  Preprocessor state
 * @param def Macro definition being built
 * @param loc Location for error messages
 * @return 0 on success, -1 on error
 */
static int novai_parse_macro_body(NovaPP *pp, NovaMacroDef *def,
                                  const NovaSourceLoc *loc) {
    NovaLexer *lex = novai_current_lexer(pp);
    if (lex == NULL) {
        return -1;
    }

    /* Collect tokens until end of directive (next line) */
    int def_line = loc->line;
    int cap = 16;
    int count = 0;

    def->body = (NovaMacroToken *)calloc((size_t)cap, sizeof(NovaMacroToken));
    if (def->body == NULL) {
        novai_pp_error(pp, loc, "out of memory for macro body");
        return -1;
    }

    int prev_was_paste = 0;

    while (nova_lex_current(lex)->type != NOVA_TOKEN_EOF) {
        const NovaToken *tok = nova_lex_current(lex);

        /* End of directive: token is on a different line */
        if (tok->loc.line != def_line) {
            break;
        }

        /* Grow body array if needed */
        if (count >= cap) {
            cap *= 2;
            NovaMacroToken *new_body = (NovaMacroToken *)realloc(
                def->body, (size_t)cap * sizeof(NovaMacroToken)
            );
            if (new_body == NULL) {
                novai_pp_error(pp, loc, "out of memory");
                return -1;
            }
            def->body = new_body;
        }

        NovaMacroToken *mt = &def->body[count];
        memset(mt, 0, sizeof(NovaMacroToken));
        mt->type = tok->type;
        mt->param_idx = -1;

        /* Check for stringify (#) operator */
        if (tok->type == NOVA_TOKEN_PP_STRINGIFY) {
            /* Next token should be a parameter name */
            nova_lex_next(lex);
            tok = nova_lex_current(lex);

            if (tok->type != NOVA_TOKEN_NAME ||
                tok->loc.line != def_line) {
                novai_pp_error(pp, &tok->loc,
                               "'#' must be followed by a macro parameter");
                return -1;
            }

            int pidx = novai_find_param(def,
                                        tok->value.string.data,
                                        tok->value.string.len);
            if (pidx < 0) {
                novai_pp_error(pp, &tok->loc,
                               "'%.*s' is not a macro parameter",
                               (int)tok->value.string.len,
                               tok->value.string.data);
                return -1;
            }

            mt->type = NOVA_TOKEN_NAME;
            mt->text = novai_strndup(tok->value.string.data,
                                     tok->value.string.len);
            mt->text_len = tok->value.string.len;
            mt->param_idx = pidx;
            mt->stringify = 1;
            count++;
            nova_lex_next(lex);
            prev_was_paste = 0;
            continue;
        }

        /* Check for token paste (##) */
        if (tok->type == NOVA_TOKEN_PP_PASTE) {
            if (count == 0) {
                novai_pp_error(pp, &tok->loc,
                               "'##' at start of macro body");
                return -1;
            }
            /* Mark previous token as paste-left */
            def->body[count - 1].paste_left = 1;
            prev_was_paste = 1;
            nova_lex_next(lex);
            continue;
        }

        /* Store the token text.
         * IMPORTANT: For INTEGER/NUMBER tokens, the lexer's union
         * stores the value in value.integer / value.number, NOT in
         * value.string. Reading value.string for those types is
         * undefined behavior (union aliasing). Instead, we convert
         * the numeric value to text with snprintf and store the
         * actual numeric value in the NovaMacroToken fields. */
        if (tok->type == NOVA_TOKEN_INTEGER) {
            char numbuf[32];
            int nlen = snprintf(numbuf, sizeof(numbuf), "%" PRId64,
                                (int64_t)tok->value.integer);
            mt->text = novai_strndup(numbuf, (size_t)nlen);
            mt->text_len = (size_t)nlen;
            mt->numeric_int = tok->value.integer;
        } else if (tok->type == NOVA_TOKEN_NUMBER) {
            char numbuf[64];
            int nlen = snprintf(numbuf, sizeof(numbuf), "%.17g",
                                tok->value.number);
            mt->text = novai_strndup(numbuf, (size_t)nlen);
            mt->text_len = (size_t)nlen;
            mt->numeric_num = tok->value.number;
        } else if (tok->type == NOVA_TOKEN_NAME ||
                   tok->type == NOVA_TOKEN_STRING) {
            mt->text = novai_strndup(tok->value.string.data,
                                     tok->value.string.len);
            mt->text_len = tok->value.string.len;
        } else {
            /* For operators and keywords, generate text from token name */
            const char *tname = nova_token_name(tok->type);
            size_t tlen = strlen(tname);
            /* Strip surrounding quotes from token names like "'and'" */
            if (tlen >= 2 && tname[0] == '\'') {
                tname++; tlen -= 2;
            }
            mt->text = novai_strndup(tname, tlen);
            mt->text_len = tlen;
        }

        if (mt->text == NULL) {
            novai_pp_error(pp, loc, "out of memory");
            return -1;
        }

        /* Check if this is a parameter reference */
        if (tok->type == NOVA_TOKEN_NAME && def->is_function) {
            int pidx = novai_find_param(def,
                                        tok->value.string.data,
                                        tok->value.string.len);
            if (pidx >= 0) {
                mt->param_idx = pidx;
            }
        }

        if (prev_was_paste) {
            mt->paste_right = 1;
            prev_was_paste = 0;
        }

        count++;
        nova_lex_next(lex);
    }

    def->body_count = count;
    return 0;
}

/**
 * @brief Handle #define directive
 *
 * @param pp  Preprocessor state
 * @param loc Source location of the directive
 * @return 0 on success, -1 on error
 */
static int novai_handle_define(NovaPP *pp, const NovaSourceLoc *loc) {
    NovaLexer *lex = novai_current_lexer(pp);
    if (lex == NULL) {
        return -1;
    }

    /* Expect: macro name */
    const NovaToken *name_tok = nova_lex_current(lex);
    if (name_tok->type != NOVA_TOKEN_NAME) {
        novai_pp_error(pp, loc, "#define expects a name, got %s",
                       nova_token_name(name_tok->type));
        novai_skip_to_eol(pp);
        return -1;
    }

    /* Create macro definition */
    NovaMacroDef *def = (NovaMacroDef *)calloc(1, sizeof(NovaMacroDef));
    if (def == NULL) {
        novai_pp_error(pp, loc, "out of memory");
        return -1;
    }

    def->name = novai_strndup(name_tok->value.string.data,
                              name_tok->value.string.len);
    def->name_len = name_tok->value.string.len;
    def->def_loc = *loc;

    if (def->name == NULL) {
        novai_macro_destroy(def);
        novai_pp_error(pp, loc, "out of memory");
        return -1;
    }

    /* Save name token position before advancing — nova_lex_next
     * overwrites lex->current, invalidating name_tok pointer. */
    int name_end_col = (int)(name_tok->loc.column +
                             (int)name_tok->value.string.len);

    nova_lex_next(lex);

    /* Check for function-like macro: NAME( -- '(' immediately after name */
    const NovaToken *next = nova_lex_current(lex);
    if (next->type == (NovaTokenType)'(' &&
        next->loc.column == name_end_col) {
        /* Function-like macro */
        def->is_function = 1;
        if (novai_parse_macro_params(pp, def, loc) != 0) {
            novai_macro_destroy(def);
            novai_skip_to_eol(pp);
            return -1;
        }
    }

    /* Parse replacement body */
    if (novai_parse_macro_body(pp, def, loc) != 0) {
        novai_macro_destroy(def);
        return -1;
    }

    /* Store in macro table */
    if (novai_macro_store(pp, def) != 0) {
        novai_macro_destroy(def);
        return -1;
    }

    return 0;
}

/* ============================================================
 * DIRECTIVE: #import
 *
 * Registers data format modules for use in the program.
 *
 *   #import json       - register json.decode(), json.encode(), etc.
 *   #import csv        - register csv.decode(), csv.encode(), etc.
 *   #import tsv        - register tsv.decode(), tsv.encode(), etc.
 *   #import ini        - register ini.decode(), ini.encode(), etc.
 *   #import toml       - register toml.decode(), toml.encode(), etc.
 *   #import html       - register html.decode(), html.encode(), etc.
 *   #import data       - register ALL format modules + data.* meta-module
 *
 * Multiple #import directives accumulate (bitwise OR).
 * Also defines NOVA_HAS_<FORMAT> macros for conditional compilation.
 * ============================================================ */

/**
 * @brief Handle #import directive
 *
 * Parses the module name token after #import and sets the
 * corresponding bit in pp->import_flags.
 */
static int novai_handle_import(NovaPP *pp, const NovaSourceLoc *loc) {
    NovaLexer *lex = novai_current_lexer(pp);
    if (lex == NULL) {
        return -1;
    }

    const NovaToken *tok = nova_lex_current(lex);
    if (tok->type != NOVA_TOKEN_NAME) {
        novai_pp_error(pp, loc,
                       "#import expects a module name "
                       "(json, csv, tsv, ini, toml, html, data)");
        novai_skip_to_eol(pp);
        return -1;
    }

    const char *name = tok->value.string.data;
    size_t nlen = tok->value.string.len;

    /* Match module name and set flag */
    uint32_t flag = 0;
    const char *macro_name = NULL;

    if (nlen == 4 && memcmp(name, "json", 4) == 0) {
        flag = NOVA_IMPORT_JSON;
        macro_name = "NOVA_HAS_JSON";
    } else if (nlen == 3 && memcmp(name, "csv", 3) == 0) {
        flag = NOVA_IMPORT_CSV;
        macro_name = "NOVA_HAS_CSV";
    } else if (nlen == 3 && memcmp(name, "tsv", 3) == 0) {
        flag = NOVA_IMPORT_TSV;
        macro_name = "NOVA_HAS_TSV";
    } else if (nlen == 3 && memcmp(name, "ini", 3) == 0) {
        flag = NOVA_IMPORT_INI;
        macro_name = "NOVA_HAS_INI";
    } else if (nlen == 4 && memcmp(name, "toml", 4) == 0) {
        flag = NOVA_IMPORT_TOML;
        macro_name = "NOVA_HAS_TOML";
    } else if (nlen == 4 && memcmp(name, "html", 4) == 0) {
        flag = NOVA_IMPORT_HTML;
        macro_name = "NOVA_HAS_HTML";
    } else if (nlen == 4 && memcmp(name, "yaml", 4) == 0) {
        flag = NOVA_IMPORT_YAML;
        macro_name = "NOVA_HAS_YAML";
    } else if (nlen == 3 && memcmp(name, "net", 3) == 0) {
        flag = NOVA_IMPORT_NET;
        macro_name = "NOVA_HAS_NET";
    } else if (nlen == 3 && memcmp(name, "sql", 3) == 0) {
        flag = NOVA_IMPORT_SQL;
        macro_name = "NOVA_HAS_SQL";
    } else if (nlen == 4 && memcmp(name, "nini", 4) == 0) {
        flag = NOVA_IMPORT_NINI;
        macro_name = "NOVA_HAS_NINI";
    } else if (nlen == 4 && memcmp(name, "data", 4) == 0) {
        flag = NOVA_IMPORT_ALL;
        macro_name = "NOVA_HAS_DATA";
    } else {
        novai_pp_error(pp, loc,
                       "#import: unknown module '%.*s' "
                       "(expected json, csv, tsv, ini, toml, html, yaml, "
                       "net, sql, nini, data)",
                       (int)nlen, name);
        novai_skip_to_eol(pp);
        return -1;
    }

    pp->import_flags |= flag;

    /* Define a corresponding macro so user can #ifdef NOVA_HAS_JSON */
    if (macro_name != NULL) {
        (void)nova_pp_define(pp, macro_name, "1");
    }

    /* If importing 'data', also define all individual macros */
    if (flag == NOVA_IMPORT_ALL) {
        (void)nova_pp_define(pp, "NOVA_HAS_JSON", "1");
        (void)nova_pp_define(pp, "NOVA_HAS_CSV", "1");
        (void)nova_pp_define(pp, "NOVA_HAS_TSV", "1");
        (void)nova_pp_define(pp, "NOVA_HAS_INI", "1");
        (void)nova_pp_define(pp, "NOVA_HAS_TOML", "1");
        (void)nova_pp_define(pp, "NOVA_HAS_HTML", "1");
        (void)nova_pp_define(pp, "NOVA_HAS_YAML", "1");
        (void)nova_pp_define(pp, "NOVA_HAS_NET", "1");
        (void)nova_pp_define(pp, "NOVA_HAS_SQL", "1");
        (void)nova_pp_define(pp, "NOVA_HAS_NINI", "1");
        (void)nova_pp_define(pp, "NOVA_HAS_DATA", "1");
    }

    nova_lex_next(lex);  /* consume the module name */

    return 0;
}

/* ============================================================
 * DIRECTIVE: #require
 *
 * Syntax:
 *   #require <module_path>              -- auto-bind to last segment
 *   #require <module_path> as <name>    -- explicit binding name
 *
 * Module path can use slashes or dots as separators:
 *   #require lib/parser       -- binds to local `parser`
 *   #require lib.parser       -- same effect
 *   #require lib/parser as P  -- binds to local `P`
 *   #require "lib/parser"     -- quoted path also accepted
 *
 * Emits synthetic tokens into the PP output stream:
 *   local <binding> = __require__("<modname>")
 *
 * The __require__ global calls nova_base_require() at runtime,
 * which searches package.path, compiles, executes, and caches
 * the module in package.loaded.
 * ============================================================ */

/**
 * @brief Handle #require directive
 *
 * Parses the module path (bare identifiers separated by / or .,
 * or a quoted string), optional `as <alias>`, and emits synthetic
 * tokens equivalent to: local <binding> = __require__("<path>")
 *
 * @param pp  Preprocessor state
 * @param loc Source location of the #require directive
 * @return 0 on success, -1 on error
 *
 * @pre pp != NULL && loc != NULL
 */
static int novai_handle_require(NovaPP *pp, const NovaSourceLoc *loc) {
    NovaLexer *lex = novai_current_lexer(pp);
    if (lex == NULL) {
        return -1;
    }

    const NovaToken *tok = nova_lex_current(lex);

    /* Module path buffer (dot-separated for require compatibility) */
    char modpath[256];
    size_t modpath_len = 0;

    /* Binding name buffer */
    char binding[64];
    size_t binding_len = 0;

    /* --- Parse module path --- */
    if (tok->type == NOVA_TOKEN_STRING) {
        /* Quoted path: #require "lib/parser" */
        if (tok->value.string.len >= sizeof(modpath)) {
            novai_pp_error(pp, loc,
                           "#require: module path too long (max %d)",
                           (int)(sizeof(modpath) - 1));
            novai_skip_to_eol(pp);
            return -1;
        }
        memcpy(modpath, tok->value.string.data, tok->value.string.len);
        modpath_len = tok->value.string.len;
        modpath[modpath_len] = '\0';
        nova_lex_next(lex);
    } else if (tok->type == NOVA_TOKEN_NAME) {
        /* Bare path: #require lib/parser  or  #require utils */
        int start_line = tok->loc.line;
        while (tok->type == NOVA_TOKEN_NAME) {
            /* Check "as" keyword — stops path parsing */
            if (tok->value.string.len == 2 &&
                memcmp(tok->value.string.data, "as", 2) == 0) {
                break;
            }

            /* Append separator if not first segment */
            if (modpath_len > 0) {
                if (modpath_len + 1 >= sizeof(modpath)) {
                    novai_pp_error(pp, loc,
                                   "#require: module path too long");
                    novai_skip_to_eol(pp);
                    return -1;
                }
                modpath[modpath_len++] = '.';
            }

            /* Append segment */
            if (modpath_len + tok->value.string.len >= sizeof(modpath)) {
                novai_pp_error(pp, loc,
                               "#require: module path too long");
                novai_skip_to_eol(pp);
                return -1;
            }
            memcpy(modpath + modpath_len, tok->value.string.data,
                   tok->value.string.len);
            modpath_len += tok->value.string.len;

            nova_lex_next(lex);
            tok = nova_lex_current(lex);

            /* Check for separator: '/' or '.' */
            if ((tok->type == (NovaTokenType)'/' ||
                 tok->type == (NovaTokenType)'.') &&
                tok->loc.line == start_line) {
                nova_lex_next(lex);
                tok = nova_lex_current(lex);
            } else {
                break;
            }
        }
        modpath[modpath_len] = '\0';
    } else {
        novai_pp_error(pp, loc,
                       "#require expects a module path "
                       "(e.g. #require utils, #require lib/parser)");
        novai_skip_to_eol(pp);
        return -1;
    }

    if (modpath_len == 0) {
        novai_pp_error(pp, loc, "#require: empty module path");
        novai_skip_to_eol(pp);
        return -1;
    }

    /* --- Extract default binding name (last path segment) --- */
    const char *last_sep = NULL;
    for (size_t i = 0; i < modpath_len; i++) {
        if (modpath[i] == '.' || modpath[i] == '/') {
            last_sep = modpath + i;
        }
    }

    if (last_sep != NULL) {
        size_t seg_len = modpath_len -
                         (size_t)(last_sep - modpath) - 1;
        if (seg_len == 0 || seg_len >= sizeof(binding)) {
            novai_pp_error(pp, loc,
                           "#require: invalid module path '%s'",
                           modpath);
            novai_skip_to_eol(pp);
            return -1;
        }
        memcpy(binding, last_sep + 1, seg_len);
        binding_len = seg_len;
    } else {
        if (modpath_len >= sizeof(binding)) {
            novai_pp_error(pp, loc,
                           "#require: binding name too long");
            novai_skip_to_eol(pp);
            return -1;
        }
        memcpy(binding, modpath, modpath_len);
        binding_len = modpath_len;
    }
    binding[binding_len] = '\0';

    /* --- Parse optional 'as <alias>' --- */
    tok = nova_lex_current(lex);
    if (tok->type == NOVA_TOKEN_NAME &&
        tok->value.string.len == 2 &&
        memcmp(tok->value.string.data, "as", 2) == 0) {
        nova_lex_next(lex);
        tok = nova_lex_current(lex);
        if (tok->type != NOVA_TOKEN_NAME) {
            novai_pp_error(pp, loc,
                           "#require: expected binding name after 'as'");
            novai_skip_to_eol(pp);
            return -1;
        }
        if (tok->value.string.len >= sizeof(binding)) {
            novai_pp_error(pp, loc,
                           "#require: alias name too long");
            novai_skip_to_eol(pp);
            return -1;
        }
        memcpy(binding, tok->value.string.data, tok->value.string.len);
        binding_len = tok->value.string.len;
        binding[binding_len] = '\0';
        nova_lex_next(lex);
    }

    /* --- Emit synthetic tokens --- *
     * local <binding> = __require__("<modpath>")
     */
    NovaToken syn;
    memset(&syn, 0, sizeof(syn));
    syn.loc = *loc;

    /* local */
    syn.type = NOVA_TOKEN_LOCAL;
    if (novai_output_push(pp, &syn) != 0) {
        return -1;
    }

    /* <binding> */
    syn.type = NOVA_TOKEN_NAME;
    syn.value.string.data = binding;
    syn.value.string.len = binding_len;
    if (novai_output_push(pp, &syn) != 0) {
        return -1;
    }

    /* = */
    syn.type = (NovaTokenType)'=';
    syn.value.string.data = NULL;
    syn.value.string.len = 0;
    if (novai_output_push(pp, &syn) != 0) {
        return -1;
    }

    /* __require__ */
    syn.type = NOVA_TOKEN_NAME;
    syn.value.string.data = "__require__";
    syn.value.string.len = 11;
    if (novai_output_push(pp, &syn) != 0) {
        return -1;
    }

    /* ( */
    syn.type = (NovaTokenType)'(';
    syn.value.string.data = NULL;
    syn.value.string.len = 0;
    if (novai_output_push(pp, &syn) != 0) {
        return -1;
    }

    /* "<modpath>" */
    syn.type = NOVA_TOKEN_STRING;
    syn.value.string.data = modpath;
    syn.value.string.len = modpath_len;
    if (novai_output_push(pp, &syn) != 0) {
        return -1;
    }

    /* ) */
    syn.type = (NovaTokenType)')';
    syn.value.string.data = NULL;
    syn.value.string.len = 0;
    if (novai_output_push(pp, &syn) != 0) {
        return -1;
    }

    return 0;
}

/* ============================================================
 * DIRECTIVE: #include
 *
 * Supports:
 *   #include "path"    - search relative to current file then paths
 *   #include <path>    - search paths only (future: system headers)
 * ============================================================ */

/**
 * @brief Handle #include directive
 */
static int novai_handle_include(NovaPP *pp, const NovaSourceLoc *loc) {
    NovaLexer *lex = novai_current_lexer(pp);
    if (lex == NULL) {
        return -1;
    }

    const NovaToken *tok = nova_lex_current(lex);
    const char *inc_name = NULL;
    size_t inc_len = 0;

    /* #include "path" */
    if (tok->type == NOVA_TOKEN_STRING) {
        inc_name = tok->value.string.data;
        inc_len = tok->value.string.len;
        nova_lex_next(lex);
    }
    /* #include <path> -- lexer produces '<' then NAME tokens... */
    else if (tok->type == (NovaTokenType)'<') {
        /* For <path>, we need to read raw text until '>' */
        /* The lexer doesn't handle this natively, so we scan manually */
        /* using the lexer's current buffer. For now, we'll handle
         * the simple case: #include <name.m> as separate tokens. */

        /* Simpler approach: collect text between < and > */
        NovaLexer *l = lex;
        size_t start_pos = l->pos;

        /* Scan forward to find '>' */
        while (l->pos < l->source_len && l->source[l->pos] != '>') {
            l->pos++;
        }

        if (l->pos >= l->source_len) {
            novai_pp_error(pp, loc,
                           "#include: expected '>' to close angle bracket");
            return -1;
        }

        inc_name = l->source + start_pos;
        inc_len = l->pos - start_pos;
        l->pos++; /* skip '>' */

        /* Re-sync the lexer by scanning the next real token */
        nova_lex_next(lex);
    } else {
        novai_pp_error(pp, &tok->loc,
                       "#include expects \"filename\" or <filename>");
        novai_skip_to_eol(pp);
        return -1;
    }

    /* Check include depth */
    if (pp->file_depth >= NOVA_PP_MAX_INCLUDE_DEPTH) {
        novai_pp_error(pp, loc,
                       "#include nesting too deep (max %d)",
                       NOVA_PP_MAX_INCLUDE_DEPTH);
        return -1;
    }

    /* Resolve the include path */
    char resolved[1024] = {0};
    if (novai_resolve_include(pp, inc_name, inc_len,
                              resolved, sizeof(resolved)) != 0) {
        /* Try the name directly */
        if (inc_len < sizeof(resolved)) {
            memcpy(resolved, inc_name, inc_len);
            resolved[inc_len] = '\0';
        }
    }

    /* Read the file */
    size_t file_size = 0;
    char *content = novai_read_file(resolved, &file_size);
    if (content == NULL) {
        novai_pp_error(pp, loc,
                       "cannot open included file '%.*s'",
                       (int)inc_len, inc_name);
        return -1;
    }

    /* Push file onto inclusion stack */
    NovaPPFileEntry *entry = &pp->file_stack[pp->file_depth];
    /* Free stale path from a previously popped file at this slot */
    free((void *)entry->path);
    entry->path = novai_strndup(resolved, strlen(resolved));
    entry->content = content;

    if (nova_lex_init(&entry->lexer, content, file_size,
                      entry->path) != 0) {
        free(content);
        free((void *)entry->path);
        novai_pp_error(pp, loc, "failed to initialize lexer for '%s'",
                       resolved);
        return -1;
    }

    pp->file_depth++;
    return 0;
}

/* ============================================================
 * DIRECTIVE: #undef, #ifdef, #ifndef, #if, #elif, #else, #endif
 * ============================================================ */

/**
 * @brief Handle #undef directive
 */
static int novai_handle_undef(NovaPP *pp, const NovaSourceLoc *loc) {
    NovaLexer *lex = novai_current_lexer(pp);
    if (lex == NULL) {
        return -1;
    }

    const NovaToken *tok = nova_lex_current(lex);
    if (tok->type != NOVA_TOKEN_NAME) {
        novai_pp_error(pp, loc, "#undef expects a name");
        novai_skip_to_eol(pp);
        return -1;
    }

    char *name = novai_strndup(tok->value.string.data,
                               tok->value.string.len);
    if (name == NULL) {
        novai_pp_error(pp, loc, "out of memory");
        return -1;
    }

    (void)dagger_remove((DaggerTable *)pp->macros,
                        name, (uint32_t)tok->value.string.len);

    free(name);
    nova_lex_next(lex);
    return 0;
}

/**
 * @brief Handle #ifdef directive
 */
static int novai_handle_ifdef(NovaPP *pp, const NovaSourceLoc *loc) {
    NovaLexer *lex = novai_current_lexer(pp);
    if (lex == NULL) {
        return -1;
    }

    const NovaToken *tok = nova_lex_current(lex);
    if (tok->type != NOVA_TOKEN_NAME) {
        novai_pp_error(pp, loc, "#ifdef expects a name");
        novai_skip_to_eol(pp);
        return -1;
    }

    int defined = (novai_macro_find(pp, tok->value.string.data,
                                    tok->value.string.len) != NULL);
    nova_lex_next(lex);

    /* Only activate if the enclosing scope is also active */
    int parent_active = novai_cond_active(pp);
    return novai_cond_push(pp, parent_active && defined, loc);
}

/**
 * @brief Handle #ifndef directive
 */
static int novai_handle_ifndef(NovaPP *pp, const NovaSourceLoc *loc) {
    NovaLexer *lex = novai_current_lexer(pp);
    if (lex == NULL) {
        return -1;
    }

    const NovaToken *tok = nova_lex_current(lex);
    if (tok->type != NOVA_TOKEN_NAME) {
        novai_pp_error(pp, loc, "#ifndef expects a name");
        novai_skip_to_eol(pp);
        return -1;
    }

    int defined = (novai_macro_find(pp, tok->value.string.data,
                                    tok->value.string.len) != NULL);
    nova_lex_next(lex);

    int parent_active = novai_cond_active(pp);
    return novai_cond_push(pp, parent_active && !defined, loc);
}

/**
 * @brief Handle #if directive
 *
 * Evaluates a constant expression. For now we support:
 *   - Integer literals (0 = false, nonzero = true)
 *   - defined(NAME) / defined NAME
 *   - Simple comparisons: NAME == value, NAME != value
 *
 * Full expression evaluation is deferred to a later version.
 */
static int novai_handle_if(NovaPP *pp, const NovaSourceLoc *loc) {
    NovaLexer *lex = novai_current_lexer(pp);
    if (lex == NULL) {
        return -1;
    }

    int result = 0;
    const NovaToken *tok = nova_lex_current(lex);

    /* Simple: #if 0 or #if 1 */
    if (tok->type == NOVA_TOKEN_INTEGER) {
        result = (tok->value.integer != 0) ? 1 : 0;
        nova_lex_next(lex);
    }
    /* #if defined(NAME) or #if defined NAME */
    else if (tok->type == NOVA_TOKEN_NAME &&
             tok->value.string.len == 7 &&
             memcmp(tok->value.string.data, "defined", 7) == 0) {
        nova_lex_next(lex);
        tok = nova_lex_current(lex);

        int has_paren = 0;
        if (tok->type == (NovaTokenType)'(') {
            has_paren = 1;
            nova_lex_next(lex);
            tok = nova_lex_current(lex);
        }

        if (tok->type == NOVA_TOKEN_NAME) {
            result = (novai_macro_find(pp, tok->value.string.data,
                                       tok->value.string.len) != NULL)
                     ? 1 : 0;
            nova_lex_next(lex);
        } else {
            novai_pp_error(pp, &tok->loc,
                           "expected name after 'defined'");
            novai_skip_to_eol(pp);
            return -1;
        }

        if (has_paren) {
            if (nova_lex_current(lex)->type == (NovaTokenType)')') {
                nova_lex_next(lex);
            } else {
                novai_pp_error(pp, &tok->loc,
                               "expected ')' after defined(name");
            }
        }
    }
    /* Anything else: for now, if we can't evaluate it, treat as 0 */
    else {
        novai_pp_warning(pp, loc,
                         "#if expression not fully supported, "
                         "treating as false");
        novai_skip_to_eol(pp);
        result = 0;
    }

    int parent_active = novai_cond_active(pp);
    return novai_cond_push(pp, parent_active && result, loc);
}

/**
 * @brief Handle #elif directive
 */
static int novai_handle_elif(NovaPP *pp, const NovaSourceLoc *loc) {
    NovaLexer *lex = novai_current_lexer(pp);
    if (lex == NULL) {
        return -1;
    }

    int result = 0;
    const NovaToken *tok = nova_lex_current(lex);

    if (tok->type == NOVA_TOKEN_INTEGER) {
        result = (tok->value.integer != 0) ? 1 : 0;
        nova_lex_next(lex);
    } else if (tok->type == NOVA_TOKEN_NAME &&
               tok->value.string.len == 7 &&
               memcmp(tok->value.string.data, "defined", 7) == 0) {
        nova_lex_next(lex);
        tok = nova_lex_current(lex);

        int has_paren = 0;
        if (tok->type == (NovaTokenType)'(') {
            has_paren = 1;
            nova_lex_next(lex);
            tok = nova_lex_current(lex);
        }

        if (tok->type == NOVA_TOKEN_NAME) {
            result = (novai_macro_find(pp, tok->value.string.data,
                                       tok->value.string.len) != NULL)
                     ? 1 : 0;
            nova_lex_next(lex);
        }

        if (has_paren && nova_lex_current(lex)->type == (NovaTokenType)')') {
            nova_lex_next(lex);
        }
    } else {
        novai_skip_to_eol(pp);
    }

    return novai_cond_elif(pp, result, loc);
}

/**
 * @brief Handle #error directive
 */
static void novai_handle_pp_error(NovaPP *pp, const NovaSourceLoc *loc) {
    NovaLexer *lex = novai_current_lexer(pp);
    if (lex == NULL) {
        return;
    }

    /* Collect the rest of the line as the error message */
    char msg[256] = {0};
    size_t msg_len = 0;
    int start_line = loc->line;

    while (nova_lex_current(lex)->type != NOVA_TOKEN_EOF &&
           nova_lex_current(lex)->loc.line == start_line) {
        const NovaToken *tok = nova_lex_current(lex);
        const char *text = tok->value.string.data;
        size_t tlen = tok->value.string.len;

        if (text != NULL && tlen > 0 &&
            msg_len + tlen + 1 < sizeof(msg)) {
            if (msg_len > 0) {
                msg[msg_len++] = ' ';
            }
            memcpy(msg + msg_len, text, tlen);
            msg_len += tlen;
        }
        nova_lex_next(lex);
    }

    msg[msg_len] = '\0';
    novai_pp_error(pp, loc, "#error %s", msg);
}

/**
 * @brief Handle #warning directive
 */
static void novai_handle_pp_warning(NovaPP *pp, const NovaSourceLoc *loc) {
    NovaLexer *lex = novai_current_lexer(pp);
    if (lex == NULL) {
        return;
    }

    char msg[256] = {0};
    size_t msg_len = 0;
    int start_line = loc->line;

    while (nova_lex_current(lex)->type != NOVA_TOKEN_EOF &&
           nova_lex_current(lex)->loc.line == start_line) {
        const NovaToken *tok = nova_lex_current(lex);
        const char *text = tok->value.string.data;
        size_t tlen = tok->value.string.len;

        if (text != NULL && tlen > 0 &&
            msg_len + tlen + 1 < sizeof(msg)) {
            if (msg_len > 0) {
                msg[msg_len++] = ' ';
            }
            memcpy(msg + msg_len, text, tlen);
            msg_len += tlen;
        }
        nova_lex_next(lex);
    }

    msg[msg_len] = '\0';
    novai_pp_warning(pp, loc, "#warning %s", msg);
}

/**
 * @brief Handle #line directive
 *
 * Syntax: #line number ["filename"]
 */
static void novai_handle_line(NovaPP *pp, const NovaSourceLoc *loc) {
    NovaLexer *lex = novai_current_lexer(pp);
    if (lex == NULL) {
        return;
    }

    const NovaToken *tok = nova_lex_current(lex);
    if (tok->type == NOVA_TOKEN_INTEGER) {
        lex->line = (int)tok->value.integer;
        nova_lex_next(lex);

        tok = nova_lex_current(lex);
        if (tok->type == NOVA_TOKEN_STRING) {
            /* We can't actually change the filename pointer since
             * it's const, but we note it for error messages. */
            lex->filename = tok->value.string.data;
            nova_lex_next(lex);
        }
    } else {
        novai_pp_error(pp, loc, "#line expects a line number");
        novai_skip_to_eol(pp);
    }
}

/* ============================================================
 * MACRO EXPANSION
 *
 * When we encounter an identifier that matches a macro,
 * we expand it by substituting parameters and generating
 * output tokens.
 * ============================================================ */

/**
 * @brief Collect arguments for a function-like macro call
 *
 * Reads balanced parenthesized argument lists, splitting on commas.
 * Returns an array of token arrays (one per argument).
 *
 * @param pp        Preprocessor state
 * @param def       Macro definition
 * @param arg_tokens Output: array of token arrays
 * @param arg_counts Output: number of tokens per argument
 * @param arg_count  Output: number of arguments
 * @return 0 on success, -1 on error
 */
static int novai_collect_args(NovaPP *pp, const NovaMacroDef *def,
                              NovaToken ***arg_tokens, int **arg_counts,
                              int *arg_count) {
    NovaLexer *lex = novai_current_lexer(pp);
    if (lex == NULL) {
        return -1;
    }

    int expected = def->param_count + (def->is_variadic ? 1 : 0);
    if (expected == 0 && !def->is_variadic) {
        expected = 1; /* Still need to handle empty () */
    }

    *arg_tokens = (NovaToken **)calloc((size_t)expected + 1,
                                       sizeof(NovaToken *));
    *arg_counts = (int *)calloc((size_t)expected + 1, sizeof(int));
    if (*arg_tokens == NULL || *arg_counts == NULL) {
        free(*arg_tokens);
        free(*arg_counts);
        *arg_tokens = NULL;
        *arg_counts = NULL;
        return -1;
    }

    int current_arg = 0;
    int cap = 16;
    int count = 0;
    int paren_depth = 0;

    (*arg_tokens)[0] = (NovaToken *)calloc((size_t)cap, sizeof(NovaToken));
    if ((*arg_tokens)[0] == NULL) {
        return -1;
    }

    while (1) {
        const NovaToken *tok = nova_lex_current(lex);

        if (tok->type == NOVA_TOKEN_EOF) {
            novai_pp_error(pp, &tok->loc,
                           "unterminated macro argument list");
            return -1;
        }

        /* Closing paren at depth 0 = end of arguments */
        if (tok->type == (NovaTokenType)')' && paren_depth == 0) {
            (*arg_counts)[current_arg] = count;
            *arg_count = current_arg + 1;

            /* If only argument is empty, and macro expects 0 params */
            if (*arg_count == 1 && count == 0 &&
                def->param_count == 0 && !def->is_variadic) {
                *arg_count = 0;
            }

            nova_lex_next(lex); /* consume ')' */
            return 0;
        }

        /* Comma at depth 0 = argument separator */
        if (tok->type == (NovaTokenType)',' && paren_depth == 0) {
            (*arg_counts)[current_arg] = count;
            current_arg++;

            if (current_arg >= expected + 1) {
                /* Allow extra args to be collected into __VA_ARGS__ */
                int new_expected = current_arg + 1;
                NovaToken **new_at = (NovaToken **)realloc(
                    *arg_tokens,
                    (size_t)new_expected * sizeof(NovaToken *)
                );
                int *new_ac = (int *)realloc(
                    *arg_counts,
                    (size_t)new_expected * sizeof(int)
                );
                if (new_at == NULL || new_ac == NULL) {
                    return -1;
                }
                *arg_tokens = new_at;
                *arg_counts = new_ac;
                expected = new_expected;
            }

            count = 0;
            cap = 16;
            (*arg_tokens)[current_arg] =
                (NovaToken *)calloc((size_t)cap, sizeof(NovaToken));
            if ((*arg_tokens)[current_arg] == NULL) {
                return -1;
            }
            nova_lex_next(lex);
            continue;
        }

        if (tok->type == (NovaTokenType)'(') {
            paren_depth++;
        } else if (tok->type == (NovaTokenType)')') {
            paren_depth--;
        }

        /* Grow the current arg's token array if needed */
        if (count >= cap) {
            cap *= 2;
            NovaToken *new_toks = (NovaToken *)realloc(
                (*arg_tokens)[current_arg],
                (size_t)cap * sizeof(NovaToken)
            );
            if (new_toks == NULL) {
                return -1;
            }
            (*arg_tokens)[current_arg] = new_toks;
        }

        (*arg_tokens)[current_arg][count] = *tok;

        /* Duplicate string data for tokens pointing into the lexer's
         * scratch buffer.  The buffer is shared and overwritten on
         * each nova_lex_next, so later arguments would corrupt
         * earlier ones (e.g. "usr","local" → both point to "local"). */
        if (novai_token_needs_string_dup(tok->type) &&
            tok->value.string.data != NULL) {
            char *dup = novai_strndup(tok->value.string.data,
                                      tok->value.string.len);
            if (dup == NULL) {
                return -1;
            }
            (*arg_tokens)[current_arg][count].value.string.data = dup;
        }

        count++;
        nova_lex_next(lex);
    }
}

/**
 * @brief Free collected argument arrays
 */
static void novai_free_args(NovaToken **arg_tokens, int *arg_counts,
                            int arg_count) {
    if (arg_tokens != NULL) {
        for (int i = 0; i < arg_count; i++) {
            if (arg_tokens[i] != NULL) {
                /* Free duplicated string data in each token */
                for (int j = 0; j < arg_counts[i]; j++) {
                    if (novai_token_needs_string_dup(
                            arg_tokens[i][j].type) &&
                        arg_tokens[i][j].value.string.data != NULL) {
                        free((char *)arg_tokens[i][j].value.string.data);
                    }
                }
                free(arg_tokens[i]);
            }
        }
        free(arg_tokens);
    }
    free(arg_counts);
}

/**
 * @brief Expand macro body with parameter substitution and rescan
 *
 * Performs parameter substitution on the macro body, pushes tokens
 * to the output buffer, then rescans for nested macro references
 * (C99 6.10.3.4 rescan rule).  Called by novai_expand_macro and
 * recursively by the rescan pass for function-like nested macros.
 *
 * @param pp          Preprocessor state
 * @param def         Macro whose body to expand
 * @param loc         Invocation location for error reporting
 * @param arg_tokens  Pre-collected argument token arrays (or NULL)
 * @param arg_counts  Number of tokens per argument (or NULL)
 * @param arg_count   Total number of arguments
 */
static void novai_expand_body(NovaPP *pp, const NovaMacroDef *def,
                              const NovaSourceLoc *loc,
                              NovaToken **arg_tokens, int *arg_counts,
                              int arg_count) {
    size_t rescan_start = pp->output_count;

    for (int i = 0; i < def->body_count; i++) {
        const NovaMacroToken *mt = &def->body[i];

        /* Parameter substitution */
        if (mt->param_idx >= 0 && arg_tokens != NULL) {
            int pidx = mt->param_idx;

            /* Stringify: #param -> "param_value_text" */
            if (mt->stringify) {
                char buf[1024] = {0};
                size_t buf_len = 0;
                buf[0] = '\0';

                if (pidx < arg_count) {
                    for (int j = 0; j < arg_counts[pidx]; j++) {
                        const NovaToken *at = &arg_tokens[pidx][j];
                        const char *text = NULL;
                        size_t tlen = 0;
                        char numbuf[64];

                        if (at->type == NOVA_TOKEN_INTEGER) {
                            int n = snprintf(numbuf, sizeof(numbuf),
                                             "%" PRId64,
                                             (int64_t)at->value.integer);
                            text = numbuf;
                            tlen = (size_t)(n > 0 ? n : 0);
                        } else if (at->type == NOVA_TOKEN_NUMBER) {
                            int n = snprintf(numbuf, sizeof(numbuf),
                                             "%.17g", at->value.number);
                            text = numbuf;
                            tlen = (size_t)(n > 0 ? n : 0);
                        } else if (at->value.string.data != NULL &&
                                   at->value.string.len > 0) {
                            text = at->value.string.data;
                            tlen = at->value.string.len;
                        } else {
                            const char *tname = nova_token_name(at->type);
                            if (tname != NULL) {
                                size_t tnl = strlen(tname);
                                if (tnl >= 2 && tname[0] == '\'') {
                                    tname++; tnl -= 2;
                                }
                                text = tname;
                                tlen = tnl;
                            }
                        }

                        if (text != NULL && tlen > 0) {
                            if (buf_len > 0 && buf_len < sizeof(buf) - 1) {
                                buf[buf_len++] = ' ';
                            }
                            size_t copy = tlen;
                            if (buf_len + copy >= sizeof(buf)) {
                                copy = sizeof(buf) - buf_len - 1;
                            }
                            memcpy(buf + buf_len, text, copy);
                            buf_len += copy;
                        }
                    }
                }
                buf[buf_len] = '\0';

                NovaToken str_tok = {0};
                str_tok.type = NOVA_TOKEN_STRING;
                str_tok.loc = *loc;
                str_tok.value.string.data = buf;
                str_tok.value.string.len = buf_len;
                (void)novai_output_push(pp, &str_tok);
                continue;
            }

            /* Normal substitution: replace with argument tokens */
            if (pidx < arg_count) {
                for (int j = 0; j < arg_counts[pidx]; j++) {
                    NovaToken at = arg_tokens[pidx][j];
                    at.loc = *loc;
                    (void)novai_output_push(pp, &at);
                }
            }
            continue;
        }

        /* __LINE__ expansion */
        if (mt->type == NOVA_TOKEN_NAME && mt->text_len == 8 &&
            memcmp(mt->text, "__LINE__", 8) == 0) {
            NovaToken lt = {0};
            lt.type = NOVA_TOKEN_INTEGER;
            lt.loc = *loc;
            lt.value.integer = (nova_int_t)loc->line;
            (void)novai_output_push(pp, &lt);
            continue;
        }

        /* __FILE__ expansion */
        if (mt->type == NOVA_TOKEN_NAME && mt->text_len == 8 &&
            memcmp(mt->text, "__FILE__", 8) == 0) {
            NovaToken ft = {0};
            ft.type = NOVA_TOKEN_STRING;
            ft.loc = *loc;
            ft.value.string.data = loc->filename;
            ft.value.string.len = (loc->filename != NULL)
                                  ? strlen(loc->filename) : 0;
            (void)novai_output_push(pp, &ft);
            continue;
        }

        /* __COUNTER__ expansion */
        if (mt->type == NOVA_TOKEN_NAME && mt->text_len == 11 &&
            memcmp(mt->text, "__COUNTER__", 11) == 0) {
            NovaToken ct = {0};
            ct.type = NOVA_TOKEN_INTEGER;
            ct.loc = *loc;
            ct.value.integer = (nova_int_t)pp->counter;
            pp->counter++;
            (void)novai_output_push(pp, &ct);
            continue;
        }

        /* TODO: ## token paste (combine adjacent tokens) */

        /* Default: push body token as-is */
        NovaToken out = {0};
        out.type = mt->type;
        out.loc = *loc;
        if (mt->type == NOVA_TOKEN_INTEGER) {
            out.value.integer = mt->numeric_int;
        } else if (mt->type == NOVA_TOKEN_NUMBER) {
            out.value.number = mt->numeric_num;
        } else {
            out.value.string.data = mt->text;
            out.value.string.len = mt->text_len;
        }
        (void)novai_output_push(pp, &out);
    }

    /* ---- Rescan: expand nested macro references ----
     *
     * After initial body expansion, the output may contain NAME
     * tokens that reference other macros (e.g. PI in TWO_PI's body,
     * or SQUARE in SQUARE_SUM's body).  We copy the newly emitted
     * tokens to a temp buffer, rewind the output, and re-process
     * them — expanding any macros found.  This mirrors C's rescan
     * rule (C99 6.10.3.4).  The expansion_depth guard prevents
     * infinite loops from recursive macros. */
    {
        size_t rescan_count = pp->output_count - rescan_start;
        if (rescan_count > 0 &&
            pp->expansion_depth < NOVA_PP_MAX_MACRO_DEPTH) {
            NovaToken *temp = (NovaToken *)malloc(
                rescan_count * sizeof(NovaToken));
            if (temp != NULL) {
                memcpy(temp, &pp->output[rescan_start],
                       rescan_count * sizeof(NovaToken));
                pp->output_count = rescan_start;

                size_t ti = 0;
                while (ti < rescan_count) {
                    NovaToken *t = &temp[ti];

                    if (t->type == NOVA_TOKEN_NAME) {
                        NovaMacroDef *nested = novai_macro_find(
                            pp, t->value.string.data,
                            t->value.string.len);

                        if (nested != NULL) {
                            if (!nested->is_function) {
                                /* Object-like nested macro */
                                pp->expansion_depth++;
                                novai_expand_body(pp, nested, loc,
                                                  NULL, NULL, 0);
                                pp->expansion_depth--;
                                ti++;
                                continue;
                            }

                            /* Function-like nested macro: needs '(' */
                            if (ti + 1 < rescan_count &&
                                temp[ti + 1].type == (NovaTokenType)'(') {
                                /* Collect args from temp token array */
                                size_t ai = ti + 2;
                                int depth = 1;
                                int fn_argc = 0;
                                NovaToken *fn_argv[64];
                                int fn_argl[64];

                                memset(fn_argv, 0, sizeof(fn_argv));
                                memset(fn_argl, 0, sizeof(fn_argl));

                                if (ai < rescan_count) {
                                    fn_argv[0] = &temp[ai];
                                    fn_argl[0] = 0;
                                    fn_argc = 1;
                                }

                                while (ai < rescan_count && depth > 0) {
                                    if (temp[ai].type == (NovaTokenType)'(') {
                                        depth++;
                                    } else if (temp[ai].type ==
                                               (NovaTokenType)')') {
                                        depth--;
                                        if (depth == 0) break;
                                    } else if (temp[ai].type ==
                                               (NovaTokenType)',' &&
                                               depth == 1) {
                                        if (fn_argc < 64) {
                                            fn_argc++;
                                            fn_argv[fn_argc - 1] =
                                                &temp[ai + 1];
                                            fn_argl[fn_argc - 1] = 0;
                                        }
                                        ai++;
                                        continue;
                                    }
                                    if (fn_argc > 0) {
                                        fn_argl[fn_argc - 1]++;
                                    }
                                    ai++;
                                }

                                if (depth == 0) {
                                    pp->expansion_depth++;
                                    novai_expand_body(pp, nested, loc,
                                                      fn_argv, fn_argl,
                                                      fn_argc);
                                    pp->expansion_depth--;
                                    ti = ai + 1;
                                    continue;
                                }
                            }
                        }
                    }

                    /* Non-macro token: push as-is */
                    (void)novai_output_push(pp, t);
                    ti++;
                }

                free(temp);
            }
        }
    }
}

/**
 * @brief Expand a macro and push result tokens to output
 *
 * @param pp   Preprocessor state
 * @param def  Macro to expand
 * @param loc  Invocation location
 * @return 0 on success, -1 on error
 */
static int novai_expand_macro(NovaPP *pp, const NovaMacroDef *def,
                              const NovaSourceLoc *loc) {
    if (pp->expansion_depth >= NOVA_PP_MAX_MACRO_DEPTH) {
        novai_pp_error(pp, loc,
                       "macro expansion too deep (max %d), "
                       "possible recursive macro '%s'",
                       NOVA_PP_MAX_MACRO_DEPTH, def->name);
        return -1;
    }

    pp->expansion_depth++;

    /* For function-like macros, collect arguments */
    NovaToken **arg_tokens = NULL;
    int *arg_counts = NULL;
    int arg_count = 0;

    if (def->is_function) {
        NovaLexer *lex = novai_current_lexer(pp);
        const NovaToken *next = nova_lex_current(lex);

        if (next->type != (NovaTokenType)'(') {
            /* Function-like macro name without '(' is NOT expanded */
            NovaToken name_tok = {0};
            name_tok.type = NOVA_TOKEN_NAME;
            name_tok.loc = *loc;
            name_tok.value.string.data = def->name;
            name_tok.value.string.len = def->name_len;
            (void)novai_output_push(pp, &name_tok);
            pp->expansion_depth--;
            return 0;
        }

        nova_lex_next(lex); /* consume '(' */

        if (novai_collect_args(pp, def, &arg_tokens,
                               &arg_counts, &arg_count) != 0) {
            novai_free_args(arg_tokens, arg_counts, arg_count);
            pp->expansion_depth--;
            return -1;
        }

        /* Validate argument count */
        int min_args = def->param_count;
        if (!def->is_variadic && arg_count != min_args) {
            novai_pp_error(pp, loc,
                           "macro '%s' expects %d arguments, got %d",
                           def->name, min_args, arg_count);
            novai_free_args(arg_tokens, arg_counts, arg_count);
            pp->expansion_depth--;
            return -1;
        }
        if (def->is_variadic && arg_count < min_args) {
            novai_pp_error(pp, loc,
                           "macro '%s' expects at least %d arguments, "
                           "got %d",
                           def->name, min_args, arg_count);
            novai_free_args(arg_tokens, arg_counts, arg_count);
            pp->expansion_depth--;
            return -1;
        }
    }

    /* Expand macro body with rescan for nested macros */
    (void)novai_expand_body(pp, def, loc, arg_tokens, arg_counts, arg_count);

    novai_free_args(arg_tokens, arg_counts, arg_count);
    pp->expansion_depth--;
    return 0;
}

/* ============================================================
 * MAIN PROCESSING LOOP
 *
 * Drives the lexer, handles directives, expands macros,
 * and pushes regular tokens to the output buffer.
 * ============================================================ */

/**
 * @brief Process tokens from the current lexer on the file stack
 *
 * This is the core loop: read tokens, handle directives and
 * macros, emit everything else to the output buffer.
 *
 * @param pp Preprocessor state
 * @return 0 on success, -1 on error
 */
static int novai_process_tokens(NovaPP *pp) {
    while (pp->file_depth > 0) {
        NovaLexer *lex = novai_current_lexer(pp);
        if (lex == NULL) {
            break;
        }

        const NovaToken *tok = nova_lex_current(lex);

        /* EOF on current file: pop the file stack */
        if (tok->type == NOVA_TOKEN_EOF) {
            pp->file_depth--;
            NovaPPFileEntry *entry = &pp->file_stack[pp->file_depth];
            nova_lex_free(&entry->lexer);
            free(entry->content);
            /* NOTE: Keep entry->path alive — output tokens reference
             * it via loc.filename. Freed in nova_pp_destroy(). */
            entry->content = NULL;
            continue;
        }

        /* Lexer error: propagate */
        if (tok->type == NOVA_TOKEN_ERROR) {
            novai_pp_error(pp, &tok->loc, "%s",
                           nova_lex_error(lex));
            nova_lex_next(lex);
            continue;
        }

        /* ---- Preprocessor directives ---- */
        if (tok->type >= NOVA_TOKEN_PP_INCLUDE &&
            tok->type <= NOVA_TOKEN_PP_LINE) {

            NovaTokenType directive = tok->type;
            NovaSourceLoc dir_loc = tok->loc;
            nova_lex_next(lex); /* advance past directive token */

            /* If inside a false conditional, only process
             * #if/#ifdef/#ifndef/#elif/#else/#endif for nesting */
            if (!novai_cond_active(pp)) {
                switch (directive) {
                    case NOVA_TOKEN_PP_IF:
                    case NOVA_TOKEN_PP_IFDEF:
                    case NOVA_TOKEN_PP_IFNDEF:
                        (void)novai_cond_push(pp, 0, &dir_loc);
                        novai_skip_to_eol(pp);
                        break;
                    case NOVA_TOKEN_PP_ELIF:
                        (void)novai_handle_elif(pp, &dir_loc);
                        break;
                    case NOVA_TOKEN_PP_ELSE:
                        (void)novai_cond_else(pp, &dir_loc);
                        break;
                    case NOVA_TOKEN_PP_ENDIF:
                        (void)novai_cond_endif(pp, &dir_loc);
                        break;
                    default:
                        /* Skip all other directives in false branch */
                        novai_skip_to_eol(pp);
                        break;
                }
                continue;
            }

            /* Active branch: handle the directive */
            switch (directive) {
                case NOVA_TOKEN_PP_INCLUDE:
                    (void)novai_handle_include(pp, &dir_loc);
                    break;
                case NOVA_TOKEN_PP_IMPORT:
                    (void)novai_handle_import(pp, &dir_loc);
                    break;
                case NOVA_TOKEN_PP_REQUIRE:
                    (void)novai_handle_require(pp, &dir_loc);
                    break;
                case NOVA_TOKEN_PP_DEFINE:
                    (void)novai_handle_define(pp, &dir_loc);
                    break;
                case NOVA_TOKEN_PP_UNDEF:
                    (void)novai_handle_undef(pp, &dir_loc);
                    break;
                case NOVA_TOKEN_PP_IFDEF:
                    (void)novai_handle_ifdef(pp, &dir_loc);
                    break;
                case NOVA_TOKEN_PP_IFNDEF:
                    (void)novai_handle_ifndef(pp, &dir_loc);
                    break;
                case NOVA_TOKEN_PP_IF:
                    (void)novai_handle_if(pp, &dir_loc);
                    break;
                case NOVA_TOKEN_PP_ELIF:
                    (void)novai_handle_elif(pp, &dir_loc);
                    break;
                case NOVA_TOKEN_PP_ELSE:
                    (void)novai_cond_else(pp, &dir_loc);
                    break;
                case NOVA_TOKEN_PP_ENDIF:
                    (void)novai_cond_endif(pp, &dir_loc);
                    break;
                case NOVA_TOKEN_PP_ERROR:
                    novai_handle_pp_error(pp, &dir_loc);
                    break;
                case NOVA_TOKEN_PP_WARNING:
                    novai_handle_pp_warning(pp, &dir_loc);
                    break;
                case NOVA_TOKEN_PP_LINE:
                    novai_handle_line(pp, &dir_loc);
                    break;
                case NOVA_TOKEN_PP_PRAGMA:
                    /* Skip pragma for now */
                    novai_skip_to_eol(pp);
                    break;
                default:
                    novai_skip_to_eol(pp);
                    break;
            }
            continue;
        }

        /* ---- Inside a false conditional: skip everything ---- */
        if (!novai_cond_active(pp)) {
            nova_lex_next(lex);
            continue;
        }

        /* ---- Macro expansion ---- */
        if (tok->type == NOVA_TOKEN_NAME) {
            /* Handle __LINE__, __FILE__, __COUNTER__ inline */
            if (tok->value.string.len == 8 &&
                memcmp(tok->value.string.data, "__LINE__", 8) == 0) {
                NovaToken lt = {0};
                lt.type = NOVA_TOKEN_INTEGER;
                lt.loc = tok->loc;
                lt.value.integer = (nova_int_t)tok->loc.line;
                (void)novai_output_push(pp, &lt);
                nova_lex_next(lex);
                continue;
            }
            if (tok->value.string.len == 8 &&
                memcmp(tok->value.string.data, "__FILE__", 8) == 0) {
                NovaToken ft = {0};
                ft.type = NOVA_TOKEN_STRING;
                ft.loc = tok->loc;
                ft.value.string.data = tok->loc.filename;
                ft.value.string.len = (tok->loc.filename != NULL)
                                      ? strlen(tok->loc.filename) : 0;
                (void)novai_output_push(pp, &ft);
                nova_lex_next(lex);
                continue;
            }
            if (tok->value.string.len == 11 &&
                memcmp(tok->value.string.data, "__COUNTER__", 11) == 0) {
                NovaToken ct = {0};
                ct.type = NOVA_TOKEN_INTEGER;
                ct.loc = tok->loc;
                ct.value.integer = (nova_int_t)pp->counter;
                pp->counter++;
                (void)novai_output_push(pp, &ct);
                nova_lex_next(lex);
                continue;
            }

            NovaMacroDef *def = novai_macro_find(
                pp, tok->value.string.data, tok->value.string.len
            );

            if (def != NULL) {
                NovaSourceLoc inv_loc = tok->loc;
                nova_lex_next(lex); /* consume macro name */

                /* Expand __LINE__ / __FILE__ / __COUNTER__ inline */
                if (def->body_count == 0 && !def->is_function) {
                    /* Flag-only macro with no body -- skip */
                    continue;
                }

                if (novai_expand_macro(pp, def, &inv_loc) != 0) {
                    return -1;
                }
                continue;
            }
        }

        /* ---- Regular token: push to output ---- */
        (void)novai_output_push(pp, tok);
        nova_lex_next(lex);
    }

    /* Check for unterminated conditionals */
    if (pp->cond_depth > 0) {
        novai_pp_error(pp, NULL,
                       "unterminated conditional directive "
                       "(%d #if/#ifdef without #endif)",
                       pp->cond_depth);
    }

    /* Push EOF token — preserve filename so parse errors at EOF
     * still report the correct source file. */
    NovaToken eof = {0};
    eof.type = NOVA_TOKEN_EOF;
    if (pp->file_stack[0].path != NULL) {
        eof.loc.filename = pp->file_stack[0].path;
    }
    /* Set line to the last token's line so caret display can work */
    if (pp->output_count > 0) {
        eof.loc.line   = pp->output[pp->output_count - 1].loc.line;
        eof.loc.column = pp->output[pp->output_count - 1].loc.column;
    }
    (void)novai_output_push(pp, &eof);

    return (pp->error_count > 0) ? -1 : 0;
}

/* ============================================================
 * PUBLIC API IMPLEMENTATION
 * ============================================================ */

NovaPP *nova_pp_create(void) {
    NovaPP *pp = (NovaPP *)calloc(1, sizeof(NovaPP));
    if (pp == NULL) {
        return NULL;
    }

    /* Create the macro hash table */
    pp->macros = (void *)dagger_create(NOVA_PP_MACRO_TABLE_CAP, NULL);
    if (pp->macros == NULL) {
        free(pp);
        return NULL;
    }

    /* Set destructor so macros are freed when table is destroyed */
    dagger_set_value_destroy((DaggerTable *)pp->macros,
                             novai_macro_destroy);

    /* Allocate search path array */
    pp->search_paths = (const char **)calloc(
        NOVA_PP_MAX_SEARCH_PATHS, sizeof(const char *)
    );
    if (pp->search_paths == NULL) {
        dagger_destroy((DaggerTable *)pp->macros);
        free(pp);
        return NULL;
    }

    /* Define predefined macros */
    (void)novai_define_predefined(pp);

    return pp;
}

void nova_pp_destroy(NovaPP *pp) {
    if (pp == NULL) {
        return;
    }

    /* Destroy macro table (frees all macro definitions) */
    if (pp->macros != NULL) {
        dagger_destroy((DaggerTable *)pp->macros);
    }

    /* Free file stack entries (in case of early abort) */
    for (int i = 0; i < pp->file_depth; i++) {
        nova_lex_free(&pp->file_stack[i].lexer);
        free(pp->file_stack[i].content);
    }

    /* Free all file paths (including already-popped entries whose
     * paths were kept alive for output token loc.filename refs) */
    for (int i = 0; i < NOVA_PP_MAX_INCLUDE_DEPTH; i++) {
        free((void *)pp->file_stack[i].path);
        pp->file_stack[i].path = NULL;
    }

    /* Free duplicated string data in output tokens */
    if (pp->output != NULL) {
        for (size_t i = 0; i < pp->output_count; i++) {
            if (novai_token_needs_string_dup(pp->output[i].type)) {
                free((void *)pp->output[i].value.string.data);
            }
        }
    }

    /* Free output buffer */
    free(pp->output);

    /* Free search paths */
    if (pp->search_paths != NULL) {
        for (int i = 0; i < pp->search_path_count; i++) {
            free((void *)pp->search_paths[i]);
        }
        free(pp->search_paths);
    }

    free(pp);
}

int nova_pp_add_search_path(NovaPP *pp, const char *path) {
    if (pp == NULL || path == NULL) {
        return -1;
    }

    if (pp->search_path_count >= NOVA_PP_MAX_SEARCH_PATHS) {
        novai_pp_error(pp, NULL, "too many include search paths");
        return -1;
    }

    char *dup = novai_strndup(path, strlen(path));
    if (dup == NULL) {
        return -1;
    }

    pp->search_paths[pp->search_path_count] = dup;
    pp->search_path_count++;
    return 0;
}

int nova_pp_define(NovaPP *pp, const char *name, const char *value) {
    if (pp == NULL || name == NULL) {
        return -1;
    }

    size_t name_len = strlen(name);

    NovaMacroDef *def = (NovaMacroDef *)calloc(1, sizeof(NovaMacroDef));
    if (def == NULL) {
        return -1;
    }

    def->name = novai_strndup(name, name_len);
    def->name_len = name_len;
    def->is_predefined = 1;

    if (def->name == NULL) {
        free(def);
        return -1;
    }

    /* If a value is provided, tokenize it into a single body token */
    if (value != NULL) {
        size_t vlen = strlen(value);

        def->body = (NovaMacroToken *)calloc(1, sizeof(NovaMacroToken));
        if (def->body == NULL) {
            novai_macro_destroy(def);
            return -1;
        }
        def->body_count = 1;

        /* Determine if the value is a number or string */
        def->body[0].text = novai_strndup(value, vlen);
        def->body[0].text_len = vlen;
        def->body[0].param_idx = -1;

        if (def->body[0].text == NULL) {
            novai_macro_destroy(def);
            return -1;
        }

        /* Classify the value token type:
         * - Contains '.' or 'e'/'E' → float (NUMBER)
         * - All digits (optionally prefixed with '-') → INTEGER
         * - Otherwise → NAME */
        if (vlen > 0 && (value[0] >= '0' && value[0] <= '9')) {
            int is_float = 0;
            for (size_t vi = 0; vi < vlen; vi++) {
                if (value[vi] == '.' || value[vi] == 'e' ||
                    value[vi] == 'E') {
                    is_float = 1;
                    break;
                }
            }
            if (is_float) {
                def->body[0].type = NOVA_TOKEN_NUMBER;
                def->body[0].numeric_num = strtod(value, NULL);
            } else {
                def->body[0].type = NOVA_TOKEN_INTEGER;
                def->body[0].numeric_int =
                    (nova_int_t)strtoll(value, NULL, 10);
            }
        } else {
            def->body[0].type = NOVA_TOKEN_NAME;
        }
    }

    return novai_macro_store(pp, def);
}

int nova_pp_undef(NovaPP *pp, const char *name) {
    if (pp == NULL || name == NULL) {
        return -1;
    }

    dagger_result_t res = dagger_remove_str(
        (DaggerTable *)pp->macros, name
    );

    return (res == DAGGER_OK) ? 0 : -1;
}

int nova_pp_is_defined(const NovaPP *pp, const char *name) {
    if (pp == NULL || name == NULL) {
        return 0;
    }
    return dagger_contains_str((const DaggerTable *)pp->macros, name);
}

int nova_pp_process_file(NovaPP *pp, const char *path) {
    if (pp == NULL || path == NULL) {
        return -1;
    }

    /* Reset output buffer for fresh processing */
    pp->output_count = 0;
    pp->output_pos = 0;

    /* Read the file */
    size_t file_size = 0;
    char *content = novai_read_file(path, &file_size);
    if (content == NULL) {
        novai_pp_error(pp, NULL, "cannot open file '%s'", path);
        return -1;
    }

    /* Push onto file stack */
    NovaPPFileEntry *entry = &pp->file_stack[0];
    free((void *)entry->path);  /* Free stale path from prior run */
    entry->path = novai_strndup(path, strlen(path));
    entry->content = content;

    if (nova_lex_init(&entry->lexer, content, file_size, entry->path) != 0) {
        free(content);
        free((void *)entry->path);
        novai_pp_error(pp, NULL,
                       "failed to initialize lexer for '%s'", path);
        return -1;
    }

    pp->file_depth = 1;

    return novai_process_tokens(pp);
}

int nova_pp_process_string(NovaPP *pp, const char *source,
                           size_t len, const char *filename) {
    if (pp == NULL || source == NULL) {
        return -1;
    }

    /* Reset output buffer */
    pp->output_count = 0;
    pp->output_pos = 0;

    size_t slen = (len > 0) ? len : strlen(source);

    /* We need to own a copy since lexer expects source to live
     * for its lifetime and the PP may outlive the caller's buffer */
    char *content = novai_strndup(source, slen);
    if (content == NULL) {
        novai_pp_error(pp, NULL, "out of memory");
        return -1;
    }

    NovaPPFileEntry *entry = &pp->file_stack[0];
    free((void *)entry->path);  /* Free stale path from prior run */
    entry->path = novai_strndup(filename ? filename : "<string>",
                                strlen(filename ? filename : "<string>"));
    entry->content = content;

    if (nova_lex_init(&entry->lexer, content, slen, entry->path) != 0) {
        free(content);
        free((void *)entry->path);
        novai_pp_error(pp, NULL, "failed to initialize lexer");
        return -1;
    }

    pp->file_depth = 1;

    return novai_process_tokens(pp);
}

NovaTokenType nova_pp_next_token(NovaPP *pp, NovaToken *token) {
    if (pp == NULL || token == NULL) {
        if (token != NULL) {
            memset(token, 0, sizeof(NovaToken));
            token->type = NOVA_TOKEN_EOF;
        }
        return NOVA_TOKEN_EOF;
    }

    if (pp->output_pos >= pp->output_count) {
        memset(token, 0, sizeof(NovaToken));
        token->type = NOVA_TOKEN_EOF;
        /* Preserve filename from the main source file so that
         * parse errors at EOF still show the correct path. */
        if (pp->file_stack[0].path != NULL) {
            token->loc.filename = pp->file_stack[0].path;
        }
        return NOVA_TOKEN_EOF;
    }

    *token = pp->output[pp->output_pos];
    pp->output_pos++;
    return token->type;
}

const char *nova_pp_error(const NovaPP *pp) {
    if (pp == NULL) {
        return "preprocessor is NULL";
    }
    return pp->error_msg;
}

int nova_pp_error_count(const NovaPP *pp) {
    if (pp == NULL) {
        return 0;
    }
    return pp->error_count;
}

int nova_pp_warning_count(const NovaPP *pp) {
    if (pp == NULL) {
        return 0;
    }
    return pp->warning_count;
}

void nova_pp_reset(NovaPP *pp) {
    if (pp == NULL) {
        return;
    }

    /* Close any open files on the stack */
    for (int i = 0; i < pp->file_depth; i++) {
        nova_lex_free(&pp->file_stack[i].lexer);
        free(pp->file_stack[i].content);
        pp->file_stack[i].content = NULL;
    }
    /* Free all file paths (including already-popped entries) */
    for (int i = 0; i < NOVA_PP_MAX_INCLUDE_DEPTH; i++) {
        free((void *)pp->file_stack[i].path);
        pp->file_stack[i].path = NULL;
    }
    pp->file_depth = 0;

    /* Reset conditional stack */
    pp->cond_depth = 0;

    /* Reset expansion depth */
    pp->expansion_depth = 0;

    /* Reset output buffer (keep allocated memory) */
    pp->output_count = 0;
    pp->output_pos = 0;

    /* Clear errors */
    pp->error_msg[0] = '\0';
    pp->error_count = 0;
    pp->warning_count = 0;

    /* Note: macros and search paths are preserved across reset */
}

uint32_t nova_pp_get_imports(const NovaPP *pp) {
    if (pp == NULL) {
        return 0;
    }
    return pp->import_flags;
}
