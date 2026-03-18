/**
 * @file nova_parse_row.c
 * @brief Nova Language - Row-Based Parser (Recursive Descent + Pratt TDOP)
 *
 * Architecture:
 *   Identical parsing logic to nova_parse.c, but emits into a
 *   flat-array NovaASTTable (nova_ast_row.h) backed by an arena
 *   allocator. All node references are uint32_t indices instead
 *   of pointers.
 *
 *   - Statements: Classic recursive descent
 *   - Expressions: Pratt (TDOP) with prefix/infix dispatch
 *   - Output: NovaASTTable (P->table) with O(1) total free
 *
 * @author Anthony Taliento
 * @date 2026-02-06
 * @version 0.2.0
 *
 * @copyright Copyright (c) 2026 Zorya Corporation
 * @license MIT
 *
 * ZORYA-C COMPLIANCE: v2.0.0
 *
 * DEPENDENCIES:
 *   - nova_parse.h (NovaParser, public API)
 *   - nova_ast_row.h (NovaASTTable, row types)
 *   - nova_pp.h (token source)
 */

#include "nova/nova_parse.h"
#include "nova/nova_error.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

/* Maximum items in stack-allocated temp buffers */
#define NOVAI_TEMP_MAX 64

/* ============================================================
 * PRATT PRECEDENCE LEVELS (same as nova_parse.c)
 * ============================================================ */

typedef enum {
    PREC_NONE       = 0,
    PREC_OR         = 1,
    PREC_AND        = 2,
    PREC_COMPARISON = 3,
    PREC_BOR        = 4,
    PREC_BXOR       = 5,
    PREC_BAND       = 6,
    PREC_SHIFT      = 7,
    PREC_CONCAT     = 8,
    PREC_ADDITIVE   = 9,
    PREC_MULTIPLICAT= 10,
    PREC_UNARY      = 11,
    PREC_POWER      = 12,
    PREC_POSTFIX    = 13,
    PREC_PRIMARY    = 14,
} NovaPrecedence;

/* ============================================================
 * TOKEN STREAM HELPERS (duplicated from nova_parse.c)
 * ============================================================ */

static void novai_advance(NovaParser *P) {
    P->previous = P->current;
    (void)nova_pp_next_token(P->pp, &P->current);
}

static inline int novai_check(const NovaParser *P, NovaTokenType type) {
    return P->current.type == type;
}

/**
 * @brief Check if current token is a NAME or keyword usable as field name
 *
 * Keywords are valid field/method names (e.g. coroutine.yield, t.end).
 */
static inline int novai_check_field_name(const NovaParser *P) {
    NovaTokenType t = P->current.type;
    if (t == NOVA_TOKEN_NAME) {
        return 1;
    }
    /* Keywords range: NOVA_TOKEN_AND .. NOVA_TOKEN_YIELD (inclusive) */
    if (t >= NOVA_TOKEN_AND && t <= NOVA_TOKEN_YIELD) {
        return 1;
    }
    return 0;
}

static int novai_match(NovaParser *P, NovaTokenType type) {
    if (P->current.type == type) {
        novai_advance(P);
        return 1;
    }
    return 0;
}

/**
 * @brief Infer the best error code from a parser error message.
 *
 * Maps common parser error message prefixes to specific
 * NovaErrorCode values for richer diagnostic output.
 */
static NovaErrorCode novai_infer_parse_code(const char *msg) {
    if (msg == NULL) { return NOVA_E1001; }
    if (strncmp(msg, "expected", 8) == 0)               return NOVA_E1014;
    if (strncmp(msg, "unexpected", 10) == 0)             return NOVA_E1001;
    if (strncmp(msg, "out of memory", 13) == 0)          return NOVA_E1017;
    if (strncmp(msg, "too many arguments", 18) == 0)     return NOVA_E1011;
    if (strncmp(msg, "too many table", 14) == 0)         return NOVA_E1012;
    if (strncmp(msg, "too many parameters", 19) == 0)    return NOVA_E1021;
    if (strncmp(msg, "too many parts", 14) == 0)         return NOVA_E1015;
    if (strncmp(msg, "too many names", 14) == 0)         return NOVA_E1003;
    if (strncmp(msg, "too many expressions", 20) == 0)   return NOVA_E1013;
    if (strncmp(msg, "too many if", 11) == 0)            return NOVA_E1005;
    return NOVA_E1001;  /* Default: unexpected token */
}

static void novai_error(NovaParser *P, const char *fmt, ...) {
    if (P->panic_mode) {
        return;
    }
    P->panic_mode = 1;
    P->had_error = 1;
    P->error_count++;

    va_list args;
    char msg[400] = {0};
    va_start(args, fmt);
    (void)vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);

    const NovaSourceLoc *loc = &P->current.loc;

    /* Store structured error message for callers */
    (void)snprintf(P->error_msg, sizeof(P->error_msg),
                   "%s:%d:%d: error: %s",
                   loc->filename ? loc->filename : "<input>",
                   loc->line, loc->column, msg);

    /* Emit colored diagnostic with inferred error code */
    nova_diag_report(NOVA_DIAG_ERROR, novai_infer_parse_code(msg),
                     loc->filename ? loc->filename : "<input>",
                     loc->line, loc->column,
                     "%s", msg);
}

static int novai_expect(NovaParser *P, NovaTokenType type,
                        const char *context) {
    if (P->current.type == type) {
        novai_advance(P);
        return 1;
    }
    novai_error(P, "expected %s%s%s, got %s",
                nova_token_name(type),
                context ? " " : "",
                context ? context : "",
                nova_token_name(P->current.type));
    return 0;
}

static void novai_synchronize(NovaParser *P) {
    P->panic_mode = 0;
    while (P->current.type != NOVA_TOKEN_EOF) {
        switch (P->current.type) {
            case NOVA_TOKEN_FUNCTION:
            case NOVA_TOKEN_LOCAL:
            case NOVA_TOKEN_IF:
            case NOVA_TOKEN_WHILE:
            case NOVA_TOKEN_FOR:
            case NOVA_TOKEN_REPEAT:
            case NOVA_TOKEN_DO:
            case NOVA_TOKEN_RETURN:
            case NOVA_TOKEN_BREAK:
            case NOVA_TOKEN_CONTINUE:
            case NOVA_TOKEN_GOTO:
            case NOVA_TOKEN_IMPORT:
            case NOVA_TOKEN_EXPORT:
            case NOVA_TOKEN_CONST:
            case NOVA_TOKEN_ASYNC:
            case NOVA_TOKEN_END:
                return;
            default:
                break;
        }
        novai_advance(P);
    }
}

/* ============================================================
 * OPERATOR HELPERS (same as nova_parse.c)
 * ============================================================ */

static int novai_get_binop_prec(NovaTokenType type) {
    switch ((int)type) {
        case NOVA_TOKEN_OR:         return PREC_OR;
        case NOVA_TOKEN_AND:        return PREC_AND;
        case NOVA_TOKEN_EQ:
        case NOVA_TOKEN_NEQ:
            return PREC_COMPARISON;
        case (NovaTokenType)'<':
        case (NovaTokenType)'>':
        case NOVA_TOKEN_LE:
        case NOVA_TOKEN_GE:
            return PREC_COMPARISON;
        case (NovaTokenType)'|':    return PREC_BOR;
        case (NovaTokenType)'~':    return PREC_BXOR;
        case (NovaTokenType)'&':    return PREC_BAND;
        case NOVA_TOKEN_SHL:
        case NOVA_TOKEN_SHR:
            return PREC_SHIFT;
        case NOVA_TOKEN_DOTDOT:     return PREC_CONCAT;
        case (NovaTokenType)'+':
        case (NovaTokenType)'-':
            return PREC_ADDITIVE;
        case (NovaTokenType)'*':
        case (NovaTokenType)'/':
        case NOVA_TOKEN_IDIV:
        case (NovaTokenType)'%':
            return PREC_MULTIPLICAT;
        case (NovaTokenType)'^':    return PREC_POWER;
        default:
            return PREC_NONE;
    }
}

static int novai_is_right_assoc(NovaTokenType type) {
    return (type == (NovaTokenType)'^' ||
            type == NOVA_TOKEN_DOTDOT);
}

static NovaBinOp novai_token_to_binop(NovaTokenType type) {
    switch ((int)type) {
        case (NovaTokenType)'+':    return NOVA_BINOP_ADD;
        case (NovaTokenType)'-':    return NOVA_BINOP_SUB;
        case (NovaTokenType)'*':    return NOVA_BINOP_MUL;
        case (NovaTokenType)'/':    return NOVA_BINOP_DIV;
        case NOVA_TOKEN_IDIV:       return NOVA_BINOP_IDIV;
        case (NovaTokenType)'%':    return NOVA_BINOP_MOD;
        case (NovaTokenType)'^':    return NOVA_BINOP_POW;
        case NOVA_TOKEN_DOTDOT:     return NOVA_BINOP_CONCAT;
        case NOVA_TOKEN_EQ:         return NOVA_BINOP_EQ;
        case NOVA_TOKEN_NEQ:        return NOVA_BINOP_NEQ;
        case (NovaTokenType)'<':    return NOVA_BINOP_LT;
        case NOVA_TOKEN_LE:         return NOVA_BINOP_LE;
        case (NovaTokenType)'>':    return NOVA_BINOP_GT;
        case NOVA_TOKEN_GE:         return NOVA_BINOP_GE;
        case NOVA_TOKEN_AND:        return NOVA_BINOP_AND;
        case NOVA_TOKEN_OR:         return NOVA_BINOP_OR;
        case (NovaTokenType)'&':    return NOVA_BINOP_BAND;
        case (NovaTokenType)'|':    return NOVA_BINOP_BOR;
        case (NovaTokenType)'~':    return NOVA_BINOP_BXOR;
        case NOVA_TOKEN_SHL:        return NOVA_BINOP_SHL;
        case NOVA_TOKEN_SHR:        return NOVA_BINOP_SHR;
        default:                    return NOVA_BINOP_ADD;
    }
}

/* ============================================================
 * FORWARD DECLARATIONS (mutual recursion)
 * ============================================================ */

static NovaExprIdx  novai_parse_expr(NovaParser *P, int min_prec);
static NovaStmtIdx  novai_parse_stmt(NovaParser *P);
static NovaBlockIdx novai_parse_block(NovaParser *P);
static NovaExprIdx  novai_parse_suffixed_expr(NovaParser *P);
static int          novai_is_block_end(const NovaParser *P);

/* Shorthand for table access */
#define T (&P->table)

/**
 * @brief Duplicate a string into the AST arena
 *
 * Token strings point to the lexer's internal buffer which is reused.
 * We must copy them into persistent storage (the arena) before the next
 * token is read.
 *
 * @param P   Parser state (provides table->arena)
 * @param str Source string (not null-terminated for length `len`)
 * @param len String length
 * @return Arena-allocated copy (null-terminated), or NULL on failure
 */
static const char *novai_dup_string(NovaParser *P, const char *str, size_t len) {
    if (str == NULL || T->arena == NULL) {
        return NULL;
    }
    return arena_strndup(T->arena, str, len);
}

/* ============================================================
 * EXPRESSION PARSING (ROW-BASED)
 * ============================================================ */

/**
 * @brief Parse an argument list: (expr, expr, ...)
 *
 * Collects args into a temp buffer, then reserves expr_extra slots.
 *
 * @param P          Parser state
 * @param out_start  Output: start index in expr_extra[]
 * @param out_count  Output: number of arguments
 * @return 0 on success, -1 on error
 */
static int novai_parse_args(NovaParser *P, NovaExtraIdx *out_start,
                            int *out_count) {
    NovaExprIdx temp[NOVAI_TEMP_MAX];
    int n = 0;

    /* Lua-style call: f "string"
     *
     * Parse as a full expression so concatenation and other
     * operators work naturally:
     *   echo "hello " .. name   -> echo("hello " .. name)
     */
    if (P->current.type == NOVA_TOKEN_STRING ||
        P->current.type == NOVA_TOKEN_LONG_STRING) {
        temp[0] = novai_parse_expr(P, PREC_NONE);
        n = 1;
        goto finish;
    }

    /* Lua-style call: f { table } */
    if (P->current.type == (NovaTokenType)'{') {
        temp[0] = novai_parse_expr(P, PREC_NONE);
        n = 1;
        goto finish;
    }

    /* Standard: (expr, expr, ...) */
    if (!novai_expect(P, (NovaTokenType)'(', "in function call")) {
        *out_start = NOVA_IDX_NONE;
        *out_count = 0;
        return -1;
    }

    if (!novai_check(P, (NovaTokenType)')')) {
        do {
            if (n >= NOVAI_TEMP_MAX) {
                novai_error(P, "too many arguments (max %d)",
                            NOVAI_TEMP_MAX);
                return -1;
            }
            temp[n] = novai_parse_expr(P, PREC_NONE);
            n++;
        } while (novai_match(P, (NovaTokenType)','));
    }

    (void)novai_expect(P, (NovaTokenType)')', "after arguments");

finish:
    if (n == 0) {
        *out_start = NOVA_IDX_NONE;
        *out_count = 0;
        return 0;
    }
    *out_start = nova_ast_add_expr_list(T, n);
    for (int i = 0; i < n; i++) {
        nova_set_extra_expr(T, *out_start, i, temp[i]);
    }
    *out_count = n;
    return 0;
}

/**
 * @brief Parse a table constructor: { fields }
 */
static NovaExprIdx novai_parse_table(NovaParser *P) {
    NovaSourceLoc loc = P->current.loc;
    novai_advance(P); /* consume '{' */

    NovaRowTableField temp_f[NOVAI_TEMP_MAX];
    int count = 0;

    while (!novai_check(P, (NovaTokenType)'}') &&
           !novai_check(P, NOVA_TOKEN_EOF)) {

        if (count >= NOVAI_TEMP_MAX) {
            novai_error(P, "too many table fields (max %d)",
                        NOVAI_TEMP_MAX);
            break;
        }

        NovaRowTableField *f = &temp_f[count];
        memset(f, 0, sizeof(*f));
        f->loc = P->current.loc;

        /* [expr] = expr */
        if (novai_match(P, (NovaTokenType)'[')) {
            f->kind = NOVA_FIELD_BRACKET;
            f->key = novai_parse_expr(P, PREC_NONE);
            (void)novai_expect(P, (NovaTokenType)']',
                               "in table field");
            (void)novai_expect(P, (NovaTokenType)'=',
                               "in table field");
            f->value = novai_parse_expr(P, PREC_NONE);
        }
        /* name = expr (check: NAME followed by '=') */
        else if (P->current.type == NOVA_TOKEN_NAME) {
            NovaToken save = P->current;
            novai_advance(P);
            if (novai_check(P, (NovaTokenType)'=')) {
                f->kind = NOVA_FIELD_RECORD;
                NovaExprIdx ki = nova_ast_add_expr(T, NOVA_EXPR_STRING,
                                                   save.loc);
                if (ki != NOVA_IDX_NONE) {
                    nova_get_expr(T, ki)->as.string.data =
                        novai_dup_string(P, save.value.string.data,
                                         save.value.string.len);
                    nova_get_expr(T, ki)->as.string.len =
                        save.value.string.len;
                }
                f->key = ki;
                novai_advance(P); /* consume '=' */
                f->value = novai_parse_expr(P, PREC_NONE);
            } else {
                f->kind = NOVA_FIELD_LIST;
                f->key = NOVA_IDX_NONE;
                NovaExprIdx ni = nova_ast_add_expr(T, NOVA_EXPR_NAME,
                                                   save.loc);
                if (ni != NOVA_IDX_NONE) {
                    nova_get_expr(T, ni)->as.string.data =
                        novai_dup_string(P, save.value.string.data,
                                         save.value.string.len);
                    nova_get_expr(T, ni)->as.string.len =
                        save.value.string.len;
                }
                f->value = ni;
            }
        }
        else {
            f->kind = NOVA_FIELD_LIST;
            f->key = NOVA_IDX_NONE;
            f->value = novai_parse_expr(P, PREC_NONE);
        }

        count++;
        if (!novai_match(P, (NovaTokenType)',') &&
            !novai_match(P, (NovaTokenType)';')) {
            break;
        }
    }

    (void)novai_expect(P, (NovaTokenType)'}', "to close table");

    NovaExprIdx ei = nova_ast_add_expr(T, NOVA_EXPR_TABLE, loc);
    if (ei != NOVA_IDX_NONE && count > 0) {
        NovaFieldIdx fi = nova_ast_add_fields(T, count);
        for (int i = 0; i < count; i++) {
            T->fields[fi + (uint32_t)i] = temp_f[i];
        }
        nova_get_expr(T, ei)->as.table.field_start = fi;
        nova_get_expr(T, ei)->as.table.field_count = count;
    } else if (ei != NOVA_IDX_NONE) {
        nova_get_expr(T, ei)->as.table.field_start = NOVA_IDX_NONE;
        nova_get_expr(T, ei)->as.table.field_count = 0;
    }
    return ei;
}

/**
 * @brief Parse a function body: (params) block end
 */
static int novai_parse_funcbody(NovaParser *P,
                                NovaParamIdx *out_param_start,
                                int *out_param_count,
                                int *out_variadic,
                                NovaBlockIdx *out_body) {
    (void)novai_expect(P, (NovaTokenType)'(', "in function definition");

    NovaRowParam temp_p[NOVAI_TEMP_MAX];
    int count = 0;
    int variadic = 0;

    if (!novai_check(P, (NovaTokenType)')')) {
        do {
            if (novai_match(P, NOVA_TOKEN_DOTDOTDOT)) {
                variadic = 1;
                break;
            }
            if (!novai_check(P, NOVA_TOKEN_NAME)) {
                novai_error(P, "expected parameter name");
                return -1;
            }
            if (count >= NOVAI_TEMP_MAX) {
                novai_error(P, "too many parameters");
                return -1;
            }
            temp_p[count].name = novai_dup_string(P,
                P->current.value.string.data,
                P->current.value.string.len);
            temp_p[count].name_len = P->current.value.string.len;
            temp_p[count].loc = P->current.loc;
            count++;
            novai_advance(P);
        } while (novai_match(P, (NovaTokenType)','));
    }

    (void)novai_expect(P, (NovaTokenType)')', "after parameters");

    /* Commit params to pool */
    if (count > 0) {
        *out_param_start = nova_ast_add_params(T, count);
        for (int i = 0; i < count; i++) {
            T->params[*out_param_start + (uint32_t)i] = temp_p[i];
        }
    } else {
        *out_param_start = NOVA_IDX_NONE;
    }
    *out_param_count = count;
    *out_variadic = variadic;

    /* Body */
    *out_body = novai_parse_block(P);
    (void)novai_expect(P, NOVA_TOKEN_END, "to close function body");

    return 0;
}

/**
 * @brief Parse a prefix expression (the "nud" in Pratt terms)
 */
static NovaExprIdx novai_parse_prefix(NovaParser *P) {
    NovaSourceLoc loc = P->current.loc;

    switch ((int)P->current.type) {

    case NOVA_TOKEN_NIL:
        novai_advance(P);
        return nova_ast_add_expr(T, NOVA_EXPR_NIL, loc);

    case NOVA_TOKEN_TRUE:
        novai_advance(P);
        return nova_ast_add_expr(T, NOVA_EXPR_TRUE, loc);

    case NOVA_TOKEN_FALSE:
        novai_advance(P);
        return nova_ast_add_expr(T, NOVA_EXPR_FALSE, loc);

    case NOVA_TOKEN_INTEGER: {
        NovaExprIdx ei = nova_ast_add_expr(T, NOVA_EXPR_INTEGER, loc);
        if (ei != NOVA_IDX_NONE) {
            nova_get_expr(T, ei)->as.integer.value = P->current.value.integer;
        }
        novai_advance(P);
        return ei;
    }

    case NOVA_TOKEN_NUMBER: {
        NovaExprIdx ei = nova_ast_add_expr(T, NOVA_EXPR_NUMBER, loc);
        if (ei != NOVA_IDX_NONE) {
            nova_get_expr(T, ei)->as.number.value = P->current.value.number;
        }
        novai_advance(P);
        return ei;
    }

    case NOVA_TOKEN_STRING:
    case NOVA_TOKEN_LONG_STRING: {
        NovaExprIdx ei = nova_ast_add_expr(T, NOVA_EXPR_STRING, loc);
        if (ei != NOVA_IDX_NONE) {
            nova_get_expr(T, ei)->as.string.data =
                novai_dup_string(P, P->current.value.string.data,
                                 P->current.value.string.len);
            nova_get_expr(T, ei)->as.string.len =
                P->current.value.string.len;
        }
        novai_advance(P);
        return ei;
    }

    case NOVA_TOKEN_INTERP_START: {
        /*
         * Parse backtick interpolated string: `text ${expr} text ...`
         *
         * Token stream from lexer:
         *   INTERP_START
         *   INTERP_SEGMENT "text"    (literal before first ${)
         *   <expression tokens>      (normal tokens for ${...} expr)
         *   INTERP_SEGMENT "text"    (literal after }, before next ${)
         *   <expression tokens>      (next expression)
         *   INTERP_SEGMENT "text"    (last literal)
         *   INTERP_END
         *
         * We collect all parts (string literals + expressions) into
         * the INTERP_STRING node's parts list.
         */
        novai_advance(P); /* consume INTERP_START */

        /* Collect parts into temp array */
        NovaExprIdx parts[NOVAI_TEMP_MAX];
        int nparts = 0;

        while (!novai_check(P, NOVA_TOKEN_INTERP_END) &&
               !novai_check(P, NOVA_TOKEN_EOF)) {

            /* Expect a segment (literal text) */
            if (novai_check(P, NOVA_TOKEN_INTERP_SEGMENT)) {
                /* Only add non-empty string segments */
                if (P->current.value.string.len > 0) {
                    if (nparts >= NOVAI_TEMP_MAX) {
                        novai_error(P, "too many parts in interpolated string");
                        break;
                    }
                    NovaExprIdx seg = nova_ast_add_expr(T, NOVA_EXPR_STRING, loc);
                    if (seg != NOVA_IDX_NONE) {
                        nova_get_expr(T, seg)->as.string.data =
                            novai_dup_string(P, P->current.value.string.data,
                                             P->current.value.string.len);
                        nova_get_expr(T, seg)->as.string.len =
                            P->current.value.string.len;
                    }
                    parts[nparts++] = seg;
                }
                novai_advance(P); /* consume INTERP_SEGMENT */
            }

            /* If not at end, parse an expression */
            if (!novai_check(P, NOVA_TOKEN_INTERP_END) &&
                !novai_check(P, NOVA_TOKEN_INTERP_SEGMENT) &&
                !novai_check(P, NOVA_TOKEN_EOF)) {
                if (nparts >= NOVAI_TEMP_MAX) {
                    novai_error(P, "too many parts in interpolated string");
                    break;
                }
                parts[nparts++] = novai_parse_expr(P, PREC_NONE);
            }
        }

        novai_match(P, NOVA_TOKEN_INTERP_END); /* consume INTERP_END */

        /* Build INTERP_STRING node with all parts */
        NovaExprIdx ei = nova_ast_add_expr(T, NOVA_EXPR_INTERP_STRING, loc);
        if (ei != NOVA_IDX_NONE && nparts > 0) {
            NovaExtraIdx ps = nova_ast_add_expr_list(T, nparts);
            for (int i = 0; i < nparts; i++) {
                nova_set_extra_expr(T, ps, i, parts[i]);
            }
            nova_get_expr(T, ei)->as.interp.parts_start = ps;
            nova_get_expr(T, ei)->as.interp.part_count = nparts;
        } else if (ei != NOVA_IDX_NONE) {
            /* Empty string: `\`` */
            NovaExprIdx empty = nova_ast_add_expr(T, NOVA_EXPR_STRING, loc);
            if (empty != NOVA_IDX_NONE) {
                nova_get_expr(T, empty)->as.string.data =
                    novai_dup_string(P, "", 0);
                nova_get_expr(T, empty)->as.string.len = 0;
            }
            NovaExtraIdx ps = nova_ast_add_expr_list(T, 1);
            nova_set_extra_expr(T, ps, 0, empty);
            nova_get_expr(T, ei)->as.interp.parts_start = ps;
            nova_get_expr(T, ei)->as.interp.part_count = 1;
        }
        return ei;
    }

    case NOVA_TOKEN_DOTDOTDOT:
        novai_advance(P);
        return nova_ast_add_expr(T, NOVA_EXPR_VARARG, loc);

    case NOVA_TOKEN_NAME: {
        NovaExprIdx ei = nova_ast_add_expr(T, NOVA_EXPR_NAME, loc);
        if (ei != NOVA_IDX_NONE) {
            nova_get_expr(T, ei)->as.string.data =
                novai_dup_string(P, P->current.value.string.data,
                                 P->current.value.string.len);
            nova_get_expr(T, ei)->as.string.len =
                P->current.value.string.len;
        }
        novai_advance(P);
        return ei;
    }

    case (NovaTokenType)'(': {
        novai_advance(P);
        NovaExprIdx ei = novai_parse_expr(P, PREC_NONE);
        (void)novai_expect(P, (NovaTokenType)')', "after expression");
        return ei;
    }

    case (NovaTokenType)'{':
        return novai_parse_table(P);

    /* ---- Unary operators ---- */
    case (NovaTokenType)'-': {
        novai_advance(P);
        NovaExprIdx operand = novai_parse_expr(P, PREC_UNARY);
        NovaExprIdx ei = nova_ast_add_expr(T, NOVA_EXPR_UNARY, loc);
        if (ei != NOVA_IDX_NONE) {
            nova_get_expr(T, ei)->as.unary.op = NOVA_UNOP_NEGATE;
            nova_get_expr(T, ei)->as.unary.operand = operand;
        }
        return ei;
    }

    case NOVA_TOKEN_NOT: {
        novai_advance(P);
        NovaExprIdx operand = novai_parse_expr(P, PREC_UNARY);
        NovaExprIdx ei = nova_ast_add_expr(T, NOVA_EXPR_UNARY, loc);
        if (ei != NOVA_IDX_NONE) {
            nova_get_expr(T, ei)->as.unary.op = NOVA_UNOP_NOT;
            nova_get_expr(T, ei)->as.unary.operand = operand;
        }
        return ei;
    }

    case (NovaTokenType)'#': {
        novai_advance(P);
        NovaExprIdx operand = novai_parse_expr(P, PREC_UNARY);
        NovaExprIdx ei = nova_ast_add_expr(T, NOVA_EXPR_UNARY, loc);
        if (ei != NOVA_IDX_NONE) {
            nova_get_expr(T, ei)->as.unary.op = NOVA_UNOP_LEN;
            nova_get_expr(T, ei)->as.unary.operand = operand;
        }
        return ei;
    }

    case (NovaTokenType)'~': {
        novai_advance(P);
        NovaExprIdx operand = novai_parse_expr(P, PREC_UNARY);
        NovaExprIdx ei = nova_ast_add_expr(T, NOVA_EXPR_UNARY, loc);
        if (ei != NOVA_IDX_NONE) {
            nova_get_expr(T, ei)->as.unary.op = NOVA_UNOP_BNOT;
            nova_get_expr(T, ei)->as.unary.operand = operand;
        }
        return ei;
    }

    /* ---- Function literal ---- */
    case NOVA_TOKEN_FUNCTION: {
        novai_advance(P);
        NovaParamIdx ps = NOVA_IDX_NONE;
        int pc = 0, va = 0;
        NovaBlockIdx body = NOVA_IDX_NONE;
        if (novai_parse_funcbody(P, &ps, &pc, &va, &body) != 0) {
            return NOVA_IDX_NONE;
        }
        NovaExprIdx ei = nova_ast_add_expr(T, NOVA_EXPR_FUNCTION, loc);
        if (ei != NOVA_IDX_NONE) {
            nova_get_expr(T, ei)->as.function.param_start = ps;
            nova_get_expr(T, ei)->as.function.param_count = pc;
            nova_get_expr(T, ei)->as.function.is_variadic = va;
            nova_get_expr(T, ei)->as.function.is_async = 0;
            nova_get_expr(T, ei)->as.function.body = body;
        }
        return ei;
    }

    /* ---- async function(...) ... end ---- */
    case NOVA_TOKEN_ASYNC: {
        novai_advance(P);
        if (!novai_check(P, NOVA_TOKEN_FUNCTION)) {
            novai_error(P, "expected 'function' after 'async'");
            return NOVA_IDX_NONE;
        }
        novai_advance(P);
        NovaParamIdx ps = NOVA_IDX_NONE;
        int pc = 0, va = 0;
        NovaBlockIdx body = NOVA_IDX_NONE;
        if (novai_parse_funcbody(P, &ps, &pc, &va, &body) != 0) {
            return NOVA_IDX_NONE;
        }
        NovaExprIdx ei = nova_ast_add_expr(T, NOVA_EXPR_FUNCTION, loc);
        if (ei != NOVA_IDX_NONE) {
            nova_get_expr(T, ei)->as.function.param_start = ps;
            nova_get_expr(T, ei)->as.function.param_count = pc;
            nova_get_expr(T, ei)->as.function.is_variadic = va;
            nova_get_expr(T, ei)->as.function.is_async = 1;
            nova_get_expr(T, ei)->as.function.body = body;
        }
        return ei;
    }

    /* ---- await expr ---- */
    case NOVA_TOKEN_AWAIT: {
        novai_advance(P);
        NovaExprIdx operand = novai_parse_expr(P, PREC_UNARY);
        NovaExprIdx ei = nova_ast_add_expr(T, NOVA_EXPR_AWAIT, loc);
        if (ei != NOVA_IDX_NONE) {
            nova_get_expr(T, ei)->as.async_op.operand = operand;
        }
        return ei;
    }

    /* ---- spawn expr ---- */
    case NOVA_TOKEN_SPAWN: {
        novai_advance(P);
        NovaExprIdx operand = novai_parse_expr(P, PREC_UNARY);
        NovaExprIdx ei = nova_ast_add_expr(T, NOVA_EXPR_SPAWN, loc);
        if (ei != NOVA_IDX_NONE) {
            nova_get_expr(T, ei)->as.async_op.operand = operand;
        }
        return ei;
    }

    /* ---- yield [expr] ---- */
    case NOVA_TOKEN_YIELD: {
        novai_advance(P);
        NovaExprIdx operand = NOVA_IDX_NONE;
        if (!novai_is_block_end(P) &&
            P->current.type != (NovaTokenType)')' &&
            P->current.type != (NovaTokenType)';' &&
            P->current.type != (NovaTokenType)',') {
            operand = novai_parse_expr(P, PREC_NONE);
        }
        NovaExprIdx ei = nova_ast_add_expr(T, NOVA_EXPR_YIELD, loc);
        if (ei != NOVA_IDX_NONE) {
            nova_get_expr(T, ei)->as.async_op.operand = operand;
        }
        return ei;
    }

    default:
        novai_error(P, "unexpected token %s in expression",
                    nova_token_name(P->current.type));
        novai_advance(P);
        return NOVA_IDX_NONE;
    }
}

/**
 * @brief Parse suffix operations: call, index, field, method
 */
static NovaExprIdx novai_parse_suffixed_expr(NovaParser *P) {
    NovaExprIdx expr = novai_parse_prefix(P);

    while (1) {
        NovaSourceLoc loc = P->current.loc;

        /* Field access: expr.name (keywords valid as field names) */
        if (novai_match(P, (NovaTokenType)'.')) {
            if (!novai_check_field_name(P)) {
                novai_error(P, "expected field name after '.'");
                return expr;
            }
            NovaExprIdx ei = nova_ast_add_expr(T, NOVA_EXPR_FIELD, loc);
            if (ei != NOVA_IDX_NONE) {
                nova_get_expr(T, ei)->as.field.object = expr;
                nova_get_expr(T, ei)->as.field.name =
                    novai_dup_string(P, P->current.value.string.data,
                                     P->current.value.string.len);
                nova_get_expr(T, ei)->as.field.name_len =
                    P->current.value.string.len;
            }
            novai_advance(P);
            expr = ei;
        }
        /* Method call: expr:name(args) */
        else if (novai_match(P, (NovaTokenType)':')) {
            if (!novai_check_field_name(P)) {
                novai_error(P, "expected method name after ':'");
                return expr;
            }
            NovaExprIdx method = nova_ast_add_expr(T, NOVA_EXPR_METHOD,
                                                   loc);
            if (method != NOVA_IDX_NONE) {
                nova_get_expr(T, method)->as.field.object = expr;
                nova_get_expr(T, method)->as.field.name =
                    novai_dup_string(P, P->current.value.string.data,
                                     P->current.value.string.len);
                nova_get_expr(T, method)->as.field.name_len =
                    P->current.value.string.len;
            }
            novai_advance(P);

            NovaExtraIdx args_start = NOVA_IDX_NONE;
            int arg_count = 0;
            if (novai_parse_args(P, &args_start, &arg_count) != 0) {
                return NOVA_IDX_NONE;
            }

            NovaExprIdx call = nova_ast_add_expr(T,
                NOVA_EXPR_METHOD_CALL, loc);
            if (call != NOVA_IDX_NONE) {
                nova_get_expr(T, call)->as.call.callee = method;
                nova_get_expr(T, call)->as.call.args_start = args_start;
                nova_get_expr(T, call)->as.call.arg_count = arg_count;
            }
            expr = call;
        }
        /* Index: expr[index_expr] */
        else if (novai_match(P, (NovaTokenType)'[')) {
            NovaExprIdx idx = novai_parse_expr(P, PREC_NONE);
            (void)novai_expect(P, (NovaTokenType)']', "after index");

            NovaExprIdx ei = nova_ast_add_expr(T, NOVA_EXPR_INDEX, loc);
            if (ei != NOVA_IDX_NONE) {
                nova_get_expr(T, ei)->as.index.object = expr;
                nova_get_expr(T, ei)->as.index.index = idx;
            }
            expr = ei;
        }
        /* Function call: expr(args) or expr "str" or expr {table} */
        else if (P->current.type == (NovaTokenType)'(' ||
                 P->current.type == NOVA_TOKEN_STRING ||
                 P->current.type == NOVA_TOKEN_LONG_STRING ||
                 P->current.type == (NovaTokenType)'{') {
            NovaExtraIdx args_start = NOVA_IDX_NONE;
            int arg_count = 0;
            if (novai_parse_args(P, &args_start, &arg_count) != 0) {
                return NOVA_IDX_NONE;
            }

            NovaExprIdx call = nova_ast_add_expr(T, NOVA_EXPR_CALL, loc);
            if (call != NOVA_IDX_NONE) {
                nova_get_expr(T, call)->as.call.callee = expr;
                nova_get_expr(T, call)->as.call.args_start = args_start;
                nova_get_expr(T, call)->as.call.arg_count = arg_count;
            }
            expr = call;
        }
        else {
            break;
        }
    }

    return expr;
}

/**
 * @brief Parse an expression with Pratt TDOP
 *
 * HOT PATH: Called recursively for every sub-expression
 */
static NovaExprIdx novai_parse_expr(NovaParser *P, int min_prec) {
    NovaExprIdx left = novai_parse_suffixed_expr(P);

    while (1) {
        int prec = novai_get_binop_prec(P->current.type);
        if (prec == PREC_NONE || prec < min_prec) {
            break;
        }

        NovaTokenType op_type = P->current.type;
        NovaSourceLoc op_loc = P->current.loc;
        novai_advance(P);

        int next_prec = novai_is_right_assoc(op_type) ? prec : prec + 1;
        NovaExprIdx right = novai_parse_expr(P, next_prec);

        NovaExprIdx binary = nova_ast_add_expr(T, NOVA_EXPR_BINARY,
                                               op_loc);
        if (binary != NOVA_IDX_NONE) {
            nova_get_expr(T, binary)->as.binary.op =
                novai_token_to_binop(op_type);
            nova_get_expr(T, binary)->as.binary.left = left;
            nova_get_expr(T, binary)->as.binary.right = right;
        }
        left = binary;
    }

    return left;
}

/**
 * @brief Parse a comma-separated list of expressions into extra pool
 */
static int novai_parse_exprlist(NovaParser *P, NovaExtraIdx *out_start,
                                int *out_count) {
    NovaExprIdx temp[NOVAI_TEMP_MAX];
    int n = 0;

    do {
        if (n >= NOVAI_TEMP_MAX) {
            novai_error(P, "too many expressions in list");
            return -1;
        }
        temp[n] = novai_parse_expr(P, PREC_NONE);
        n++;
    } while (novai_match(P, (NovaTokenType)','));

    *out_start = nova_ast_add_expr_list(T, n);
    for (int i = 0; i < n; i++) {
        nova_set_extra_expr(T, *out_start, i, temp[i]);
    }
    *out_count = n;
    return 0;
}

/* PLACEHOLDER: statement parsing will be inserted here */

/* ============================================================
 * STATEMENT PARSERS (ROW-BASED)
 * ============================================================ */

/**
 * @brief Parse: local name [, name ...] [= expr [, expr ...]]
 *        or:    local function name(...) ... end
 */
static NovaStmtIdx novai_parse_local(NovaParser *P) {
    NovaSourceLoc loc = P->previous.loc;

    /* dec [async] function name(...) ... end */
    int local_is_async = 0;
    if (novai_match(P, NOVA_TOKEN_ASYNC)) {
        local_is_async = 1;
        if (!novai_check(P, NOVA_TOKEN_FUNCTION)) {
            novai_error(P, "expected 'function' after 'dec async'");
            return NOVA_IDX_NONE;
        }
    }
    if (novai_match(P, NOVA_TOKEN_FUNCTION)) {
        if (!novai_check(P, NOVA_TOKEN_NAME)) {
            novai_error(P, "expected function name after 'dec function'");
            return NOVA_IDX_NONE;
        }
        NovaExprIdx name = nova_ast_add_expr(T, NOVA_EXPR_NAME,
                                             P->current.loc);
        if (name != NOVA_IDX_NONE) {
            nova_get_expr(T, name)->as.string.data =
                novai_dup_string(P, P->current.value.string.data,
                                 P->current.value.string.len);
            nova_get_expr(T, name)->as.string.len =
                P->current.value.string.len;
        }
        novai_advance(P);

        NovaParamIdx ps = NOVA_IDX_NONE;
        int pc = 0, va = 0;
        NovaBlockIdx body = NOVA_IDX_NONE;
        if (novai_parse_funcbody(P, &ps, &pc, &va, &body) != 0) {
            return NOVA_IDX_NONE;
        }

        NovaStmtIdx si = nova_ast_add_stmt(T, NOVA_STMT_LOCAL_FUNCTION,
                                           loc);
        if (si != NOVA_IDX_NONE) {
            NovaRowStmt *s = nova_get_stmt(T, si);
            s->as.func_stmt.name = name;
            s->as.func_stmt.param_start = ps;
            s->as.func_stmt.param_count = pc;
            s->as.func_stmt.is_variadic = va;
            s->as.func_stmt.is_async = local_is_async;
            s->as.func_stmt.body = body;
        }
        return si;
    }

    /* dec name [, name ...] */
    NovaRowNameRef temp_names[NOVAI_TEMP_MAX];
    int name_count = 0;

    do {
        if (!novai_check(P, NOVA_TOKEN_NAME)) {
            novai_error(P, "expected name in dec declaration");
            return NOVA_IDX_NONE;
        }
        if (name_count >= NOVAI_TEMP_MAX) {
            novai_error(P, "too many names in dec declaration");
            return NOVA_IDX_NONE;
        }
        temp_names[name_count].data = novai_dup_string(P,
            P->current.value.string.data,
            P->current.value.string.len);
        temp_names[name_count].len = P->current.value.string.len;
        name_count++;
        novai_advance(P);
    } while (novai_match(P, (NovaTokenType)','));

    /* Commit names to pool */
    NovaNameIdx ns = nova_ast_add_names(T, name_count);
    for (int i = 0; i < name_count; i++) {
        T->names[ns + (uint32_t)i] = temp_names[i];
    }

    /* Optional initializers */
    NovaExtraIdx vs = NOVA_IDX_NONE;
    int value_count = 0;
    if (novai_match(P, (NovaTokenType)'=')) {
        (void)novai_parse_exprlist(P, &vs, &value_count);
    }

    NovaStmtIdx si = nova_ast_add_stmt(T, NOVA_STMT_LOCAL, loc);
    if (si != NOVA_IDX_NONE) {
        NovaRowStmt *s = nova_get_stmt(T, si);
        s->as.local.names_start = ns;
        s->as.local.name_count = name_count;
        s->as.local.values_start = vs;
        s->as.local.value_count = value_count;
    }
    return si;
}

/**
 * @brief Parse: if cond then block {elseif cond then block} [else block] end
 */
static NovaStmtIdx novai_parse_if(NovaParser *P) {
    NovaSourceLoc loc = P->previous.loc;

    NovaRowIfBranch temp_br[NOVAI_TEMP_MAX];
    int count = 0;

    /* if condition then block */
    temp_br[0].loc = loc;
    temp_br[0].condition = novai_parse_expr(P, PREC_NONE);
    (void)novai_expect(P, NOVA_TOKEN_THEN, "after if condition");
    temp_br[0].body = novai_parse_block(P);
    count = 1;

    /* elseif ... then ... */
    while (novai_match(P, NOVA_TOKEN_ELSEIF)) {
        if (count >= NOVAI_TEMP_MAX) {
            novai_error(P, "too many if/elseif branches");
            break;
        }
        temp_br[count].loc = P->previous.loc;
        temp_br[count].condition = novai_parse_expr(P, PREC_NONE);
        (void)novai_expect(P, NOVA_TOKEN_THEN, "after elseif condition");
        temp_br[count].body = novai_parse_block(P);
        count++;
    }

    /* else */
    if (novai_match(P, NOVA_TOKEN_ELSE)) {
        if (count < NOVAI_TEMP_MAX) {
            temp_br[count].loc = P->previous.loc;
            temp_br[count].condition = NOVA_IDX_NONE;
            temp_br[count].body = novai_parse_block(P);
            count++;
        }
    }

    (void)novai_expect(P, NOVA_TOKEN_END, "to close if statement");

    /* Commit branches */
    NovaBranchIdx bs = nova_ast_add_branches(T, count);
    for (int i = 0; i < count; i++) {
        T->branches[bs + (uint32_t)i] = temp_br[i];
    }

    NovaStmtIdx si = nova_ast_add_stmt(T, NOVA_STMT_IF, loc);
    if (si != NOVA_IDX_NONE) {
        nova_get_stmt(T, si)->as.if_stmt.branch_start = bs;
        nova_get_stmt(T, si)->as.if_stmt.branch_count = count;
    }
    return si;
}

/**
 * @brief Parse: while condition do block end
 */
static NovaStmtIdx novai_parse_while(NovaParser *P) {
    NovaSourceLoc loc = P->previous.loc;

    NovaExprIdx cond = novai_parse_expr(P, PREC_NONE);
    (void)novai_expect(P, NOVA_TOKEN_DO, "after while condition");
    NovaBlockIdx body = novai_parse_block(P);
    (void)novai_expect(P, NOVA_TOKEN_END, "to close while loop");

    NovaStmtIdx si = nova_ast_add_stmt(T, NOVA_STMT_WHILE, loc);
    if (si != NOVA_IDX_NONE) {
        nova_get_stmt(T, si)->as.while_stmt.condition = cond;
        nova_get_stmt(T, si)->as.while_stmt.body = body;
    }
    return si;
}

/**
 * @brief Parse: repeat block until condition
 */
static NovaStmtIdx novai_parse_repeat(NovaParser *P) {
    NovaSourceLoc loc = P->previous.loc;

    NovaBlockIdx body = novai_parse_block(P);
    (void)novai_expect(P, NOVA_TOKEN_UNTIL, "to close repeat loop");
    NovaExprIdx cond = novai_parse_expr(P, PREC_NONE);

    NovaStmtIdx si = nova_ast_add_stmt(T, NOVA_STMT_REPEAT, loc);
    if (si != NOVA_IDX_NONE) {
        nova_get_stmt(T, si)->as.repeat_stmt.body = body;
        nova_get_stmt(T, si)->as.repeat_stmt.condition = cond;
    }
    return si;
}

/**
 * @brief Parse: for name = start, stop [, step] do block end
 *        or:    for names in exprs do block end
 */
static NovaStmtIdx novai_parse_for(NovaParser *P) {
    NovaSourceLoc loc = P->previous.loc;

    if (!novai_check(P, NOVA_TOKEN_NAME)) {
        novai_error(P, "expected name after 'for'");
        return NOVA_IDX_NONE;
    }

    const char *first_name = novai_dup_string(P,
        P->current.value.string.data,
        P->current.value.string.len);
    size_t first_name_len = P->current.value.string.len;
    novai_advance(P);

    /* Numeric for: for i = start, stop [, step] do */
    if (novai_match(P, (NovaTokenType)'=')) {
        NovaExprIdx start = novai_parse_expr(P, PREC_NONE);
        (void)novai_expect(P, (NovaTokenType)',', "after for start");
        NovaExprIdx stop = novai_parse_expr(P, PREC_NONE);

        NovaExprIdx step = NOVA_IDX_NONE;
        if (novai_match(P, (NovaTokenType)',')) {
            step = novai_parse_expr(P, PREC_NONE);
        }

        (void)novai_expect(P, NOVA_TOKEN_DO, "after for clause");
        NovaBlockIdx body = novai_parse_block(P);
        (void)novai_expect(P, NOVA_TOKEN_END, "to close for loop");

        NovaStmtIdx si = nova_ast_add_stmt(T, NOVA_STMT_FOR_NUMERIC,
                                           loc);
        if (si != NOVA_IDX_NONE) {
            NovaRowStmt *s = nova_get_stmt(T, si);
            s->as.for_numeric.name = first_name;
            s->as.for_numeric.name_len = first_name_len;
            s->as.for_numeric.start = start;
            s->as.for_numeric.stop = stop;
            s->as.for_numeric.step = step;
            s->as.for_numeric.body = body;
        }
        return si;
    }

    /* Generic for: for k [, v ...] in exprs do */
    NovaRowNameRef temp_names[NOVAI_TEMP_MAX];
    int name_count = 1;
    temp_names[0].data = first_name;
    temp_names[0].len = first_name_len;

    while (novai_match(P, (NovaTokenType)',')) {
        if (!novai_check(P, NOVA_TOKEN_NAME)) {
            novai_error(P, "expected name in for loop");
            return NOVA_IDX_NONE;
        }
        if (name_count >= NOVAI_TEMP_MAX) {
            novai_error(P, "too many names in for loop");
            return NOVA_IDX_NONE;
        }
        temp_names[name_count].data = novai_dup_string(P,
            P->current.value.string.data,
            P->current.value.string.len);
        temp_names[name_count].len = P->current.value.string.len;
        name_count++;
        novai_advance(P);
    }

    (void)novai_expect(P, NOVA_TOKEN_IN, "in generic for loop");

    /* Commit names */
    NovaNameIdx ns = nova_ast_add_names(T, name_count);
    for (int i = 0; i < name_count; i++) {
        T->names[ns + (uint32_t)i] = temp_names[i];
    }

    /* Parse iterators */
    NovaExtraIdx is = NOVA_IDX_NONE;
    int iter_count = 0;
    (void)novai_parse_exprlist(P, &is, &iter_count);

    (void)novai_expect(P, NOVA_TOKEN_DO, "after for-in clause");
    NovaBlockIdx body = novai_parse_block(P);
    (void)novai_expect(P, NOVA_TOKEN_END, "to close for loop");

    NovaStmtIdx si = nova_ast_add_stmt(T, NOVA_STMT_FOR_GENERIC, loc);
    if (si != NOVA_IDX_NONE) {
        NovaRowStmt *s = nova_get_stmt(T, si);
        s->as.for_generic.names_start = ns;
        s->as.for_generic.name_count = name_count;
        s->as.for_generic.iters_start = is;
        s->as.for_generic.iter_count = iter_count;
        s->as.for_generic.body = body;
    }
    return si;
}

/**
 * @brief Parse: function name[.field][:method](...) ... end
 */
static NovaStmtIdx novai_parse_function_stat(NovaParser *P) {
    NovaSourceLoc loc = P->previous.loc;

    if (!novai_check(P, NOVA_TOKEN_NAME)) {
        novai_error(P, "expected function name");
        return NOVA_IDX_NONE;
    }

    NovaExprIdx name = nova_ast_add_expr(T, NOVA_EXPR_NAME,
                                         P->current.loc);
    if (name != NOVA_IDX_NONE) {
        nova_get_expr(T, name)->as.string.data =
            novai_dup_string(P, P->current.value.string.data,
                             P->current.value.string.len);
        nova_get_expr(T, name)->as.string.len =
            P->current.value.string.len;
    }
    novai_advance(P);

    /* Handle dotted names: a.b.c (keywords valid as field names) */
    while (novai_match(P, (NovaTokenType)'.')) {
        if (!novai_check_field_name(P)) {
            novai_error(P, "expected name after '.'");
            return NOVA_IDX_NONE;
        }
        NovaExprIdx field = nova_ast_add_expr(T, NOVA_EXPR_FIELD,
                                              P->current.loc);
        if (field != NOVA_IDX_NONE) {
            nova_get_expr(T, field)->as.field.object = name;
            nova_get_expr(T, field)->as.field.name =
                novai_dup_string(P, P->current.value.string.data,
                                 P->current.value.string.len);
            nova_get_expr(T, field)->as.field.name_len =
                P->current.value.string.len;
        }
        novai_advance(P);
        name = field;
    }

    /* Handle method syntax: a:b (keywords valid as method names) */
    if (novai_match(P, (NovaTokenType)':')) {
        if (!novai_check_field_name(P)) {
            novai_error(P, "expected method name after ':'");
            return NOVA_IDX_NONE;
        }
        NovaExprIdx method = nova_ast_add_expr(T, NOVA_EXPR_METHOD,
                                               P->current.loc);
        if (method != NOVA_IDX_NONE) {
            nova_get_expr(T, method)->as.field.object = name;
            nova_get_expr(T, method)->as.field.name =
                novai_dup_string(P, P->current.value.string.data,
                                 P->current.value.string.len);
            nova_get_expr(T, method)->as.field.name_len =
                P->current.value.string.len;
        }
        novai_advance(P);
        name = method;
    }

    NovaParamIdx ps = NOVA_IDX_NONE;
    int pc = 0, va = 0;
    NovaBlockIdx body = NOVA_IDX_NONE;
    if (novai_parse_funcbody(P, &ps, &pc, &va, &body) != 0) {
        return NOVA_IDX_NONE;
    }

    NovaStmtIdx si = nova_ast_add_stmt(T, NOVA_STMT_FUNCTION, loc);
    if (si != NOVA_IDX_NONE) {
        NovaRowStmt *s = nova_get_stmt(T, si);
        s->as.func_stmt.name = name;
        s->as.func_stmt.param_start = ps;
        s->as.func_stmt.param_count = pc;
        s->as.func_stmt.is_variadic = va;
        s->as.func_stmt.is_async = 0;
        s->as.func_stmt.body = body;
    }
    return si;
}

/**
 * @brief Parse: do block end
 */
static NovaStmtIdx novai_parse_do(NovaParser *P) {
    NovaSourceLoc loc = P->previous.loc;
    NovaBlockIdx body = novai_parse_block(P);
    (void)novai_expect(P, NOVA_TOKEN_END, "to close do block");

    NovaStmtIdx si = nova_ast_add_stmt(T, NOVA_STMT_DO, loc);
    if (si != NOVA_IDX_NONE) {
        nova_get_stmt(T, si)->as.do_stmt.body = body;
    }
    return si;
}

/**
 * @brief Parse: return [expr [, expr ...]] [;]
 */
static NovaStmtIdx novai_parse_return(NovaParser *P) {
    NovaSourceLoc loc = P->previous.loc;

    NovaExtraIdx vs = NOVA_IDX_NONE;
    int value_count = 0;

    if (P->current.type != NOVA_TOKEN_END &&
        P->current.type != NOVA_TOKEN_ELSE &&
        P->current.type != NOVA_TOKEN_ELSEIF &&
        P->current.type != NOVA_TOKEN_UNTIL &&
        P->current.type != NOVA_TOKEN_EOF &&
        P->current.type != (NovaTokenType)')') {
        (void)novai_parse_exprlist(P, &vs, &value_count);
    }

    (void)novai_match(P, (NovaTokenType)';');

    NovaStmtIdx si = nova_ast_add_stmt(T, NOVA_STMT_RETURN, loc);
    if (si != NOVA_IDX_NONE) {
        nova_get_stmt(T, si)->as.return_stmt.values_start = vs;
        nova_get_stmt(T, si)->as.return_stmt.value_count = value_count;
    }
    return si;
}

/**
 * @brief Parse: goto label_name
 */
static NovaStmtIdx novai_parse_goto(NovaParser *P) {
    NovaSourceLoc loc = P->previous.loc;

    if (!novai_check(P, NOVA_TOKEN_NAME)) {
        novai_error(P, "expected label name after 'goto'");
        return NOVA_IDX_NONE;
    }

    NovaStmtIdx si = nova_ast_add_stmt(T, NOVA_STMT_GOTO, loc);
    if (si != NOVA_IDX_NONE) {
        nova_get_stmt(T, si)->as.goto_stmt.label =
            P->current.value.string.data;
        nova_get_stmt(T, si)->as.goto_stmt.label_len =
            P->current.value.string.len;
    }
    novai_advance(P);
    return si;
}

/**
 * @brief Parse: ::label_name::
 */
static NovaStmtIdx novai_parse_label(NovaParser *P) {
    NovaSourceLoc loc = P->previous.loc;

    if (!novai_check(P, NOVA_TOKEN_NAME)) {
        novai_error(P, "expected label name");
        return NOVA_IDX_NONE;
    }

    NovaStmtIdx si = nova_ast_add_stmt(T, NOVA_STMT_LABEL, loc);
    if (si != NOVA_IDX_NONE) {
        nova_get_stmt(T, si)->as.label_stmt.label =
            P->current.value.string.data;
        nova_get_stmt(T, si)->as.label_stmt.label_len =
            P->current.value.string.len;
    }
    novai_advance(P);
    (void)novai_expect(P, NOVA_TOKEN_DBCOLON, "to close label");
    return si;
}

/**
 * @brief Parse: import "module" [as alias]
 */
static NovaStmtIdx novai_parse_import(NovaParser *P) {
    NovaSourceLoc loc = P->previous.loc;

    if (!novai_check(P, NOVA_TOKEN_STRING)) {
        novai_error(P, "expected string after 'import'");
        return NOVA_IDX_NONE;
    }

    NovaStmtIdx si = nova_ast_add_stmt(T, NOVA_STMT_IMPORT, loc);
    if (si != NOVA_IDX_NONE) {
        nova_get_stmt(T, si)->as.import_stmt.module =
            P->current.value.string.data;
        nova_get_stmt(T, si)->as.import_stmt.module_len =
            P->current.value.string.len;
    }
    novai_advance(P);

    /* Optional: as alias */
    if (novai_check(P, NOVA_TOKEN_NAME) &&
        P->current.value.string.len == 2 &&
        memcmp(P->current.value.string.data, "as", 2) == 0) {
        novai_advance(P);
        if (novai_check(P, NOVA_TOKEN_NAME) && si != NOVA_IDX_NONE) {
            nova_get_stmt(T, si)->as.import_stmt.alias =
                P->current.value.string.data;
            nova_get_stmt(T, si)->as.import_stmt.alias_len =
                P->current.value.string.len;
            novai_advance(P);
        } else {
            novai_error(P, "expected alias name after 'as'");
        }
    }

    return si;
}

/**
 * @brief Parse: export expr
 */
static NovaStmtIdx novai_parse_export(NovaParser *P) {
    NovaSourceLoc loc = P->previous.loc;
    NovaExprIdx value = novai_parse_expr(P, PREC_NONE);

    NovaStmtIdx si = nova_ast_add_stmt(T, NOVA_STMT_EXPORT, loc);
    if (si != NOVA_IDX_NONE) {
        nova_get_stmt(T, si)->as.export_stmt.value = value;
    }
    return si;
}

/**
 * @brief Parse: const name = expr
 */
static NovaStmtIdx novai_parse_const(NovaParser *P) {
    NovaSourceLoc loc = P->previous.loc;

    if (!novai_check(P, NOVA_TOKEN_NAME)) {
        novai_error(P, "expected name after 'const'");
        return NOVA_IDX_NONE;
    }

    const char *name = novai_dup_string(P,
        P->current.value.string.data,
        P->current.value.string.len);
    size_t name_len = P->current.value.string.len;
    novai_advance(P);

    (void)novai_expect(P, (NovaTokenType)'=', "in const declaration");
    NovaExprIdx value = novai_parse_expr(P, PREC_NONE);

    NovaStmtIdx si = nova_ast_add_stmt(T, NOVA_STMT_CONST, loc);
    if (si != NOVA_IDX_NONE) {
        nova_get_stmt(T, si)->as.const_stmt.name = name;
        nova_get_stmt(T, si)->as.const_stmt.name_len = name_len;
        nova_get_stmt(T, si)->as.const_stmt.value = value;
    }
    return si;
}

/**
 * @brief Parse: enum Name MEMBER [= expr] ... end
 *
 * Syntax:
 *   enum Color
 *       RED
 *       GREEN
 *       BLUE
 *   end
 *
 *   enum HttpStatus
 *       OK = 200
 *       NOT_FOUND = 404
 *   end
 *
 * Each member is a NAME, optionally followed by '=' and an expression.
 * Members separated by newlines (no commas needed, but commas and
 * semicolons are tolerated as separators).
 */
static NovaStmtIdx novai_parse_enum(NovaParser *P) {
    NovaSourceLoc loc = P->previous.loc;

    if (!novai_check(P, NOVA_TOKEN_NAME)) {
        novai_error(P, "expected name after 'enum'");
        return NOVA_IDX_NONE;
    }

    const char *name = novai_dup_string(P,
        P->current.value.string.data,
        P->current.value.string.len);
    size_t name_len = P->current.value.string.len;
    novai_advance(P);

    /* Collect enum members */
    NovaRowNameRef temp_names[NOVAI_TEMP_MAX];
    NovaExprIdx    temp_values[NOVAI_TEMP_MAX];
    int member_count = 0;
    int has_values = 0;

    while (!novai_check(P, NOVA_TOKEN_END) &&
           !novai_check(P, NOVA_TOKEN_EOF)) {

        /* Skip optional separators */
        while (novai_match(P, (NovaTokenType)',') ||
               novai_match(P, (NovaTokenType)';')) {
            /* skip */
        }

        if (novai_check(P, NOVA_TOKEN_END)) break;

        if (!novai_check(P, NOVA_TOKEN_NAME)) {
            novai_error(P, "expected member name in enum");
            return NOVA_IDX_NONE;
        }

        if (member_count >= NOVAI_TEMP_MAX) {
            novai_error(P, "too many members in enum");
            return NOVA_IDX_NONE;
        }

        temp_names[member_count].data = novai_dup_string(P,
            P->current.value.string.data,
            P->current.value.string.len);
        temp_names[member_count].len = P->current.value.string.len;
        novai_advance(P);

        /* Optional explicit value */
        if (novai_match(P, (NovaTokenType)'=')) {
            temp_values[member_count] = novai_parse_expr(P, PREC_NONE);
            has_values = 1;
        } else {
            temp_values[member_count] = NOVA_IDX_NONE;
        }

        member_count++;
    }

    (void)novai_expect(P, NOVA_TOKEN_END, "to close 'enum'");

    /* Commit names to pool */
    NovaNameIdx ns = nova_ast_add_names(T, member_count);
    for (int i = 0; i < member_count; i++) {
        T->names[ns + (uint32_t)i] = temp_names[i];
    }

    /* Commit values to expr_extra pool */
    NovaExtraIdx vs = nova_ast_add_expr_list(T, member_count);
    for (int i = 0; i < member_count; i++) {
        nova_set_extra_expr(T, vs, i, temp_values[i]);
    }

    NovaStmtIdx si = nova_ast_add_stmt(T, NOVA_STMT_ENUM, loc);
    if (si != NOVA_IDX_NONE) {
        NovaRowStmt *s = nova_get_stmt(T, si);
        s->as.enum_stmt.name = name;
        s->as.enum_stmt.name_len = name_len;
        s->as.enum_stmt.members_start = ns;
        s->as.enum_stmt.member_count = member_count;
        s->as.enum_stmt.values_start = vs;
        s->as.enum_stmt.has_values = has_values;
        s->as.enum_stmt.typed = 0;
    }
    return si;
}

/**
 * @brief Parse: struct Name field [= default] ... end
 *
 * Syntax:
 *   struct Point
 *       x = 0
 *       y = 0
 *   end
 *
 *   struct Config
 *       host = "localhost"
 *       port = 8080
 *       debug = false
 *   end
 *
 * Each field is a NAME, optionally followed by '=' and a default value.
 * Fields without defaults get nil. The result is a constructor function
 * that accepts positional arguments to fill fields in declaration order.
 */
static NovaStmtIdx novai_parse_struct(NovaParser *P) {
    NovaSourceLoc loc = P->previous.loc;

    if (!novai_check(P, NOVA_TOKEN_NAME)) {
        novai_error(P, "expected name after 'struct'");
        return NOVA_IDX_NONE;
    }

    const char *name = novai_dup_string(P,
        P->current.value.string.data,
        P->current.value.string.len);
    size_t name_len = P->current.value.string.len;
    novai_advance(P);

    /* Collect struct fields */
    NovaRowNameRef temp_names[NOVAI_TEMP_MAX];
    NovaExprIdx    temp_defaults[NOVAI_TEMP_MAX];
    int field_count = 0;

    while (!novai_check(P, NOVA_TOKEN_END) &&
           !novai_check(P, NOVA_TOKEN_EOF)) {

        /* Skip optional separators */
        while (novai_match(P, (NovaTokenType)',') ||
               novai_match(P, (NovaTokenType)';')) {
            /* skip */
        }

        if (novai_check(P, NOVA_TOKEN_END)) break;

        if (!novai_check(P, NOVA_TOKEN_NAME)) {
            novai_error(P, "expected field name in struct");
            return NOVA_IDX_NONE;
        }

        if (field_count >= NOVAI_TEMP_MAX) {
            novai_error(P, "too many fields in struct");
            return NOVA_IDX_NONE;
        }

        temp_names[field_count].data = novai_dup_string(P,
            P->current.value.string.data,
            P->current.value.string.len);
        temp_names[field_count].len = P->current.value.string.len;
        novai_advance(P);

        /* Optional default value */
        if (novai_match(P, (NovaTokenType)'=')) {
            temp_defaults[field_count] = novai_parse_expr(P, PREC_NONE);
        } else {
            temp_defaults[field_count] = NOVA_IDX_NONE;
        }

        field_count++;
    }

    (void)novai_expect(P, NOVA_TOKEN_END, "to close 'struct'");

    /* Commit names to pool */
    NovaNameIdx ns = nova_ast_add_names(T, field_count);
    for (int i = 0; i < field_count; i++) {
        T->names[ns + (uint32_t)i] = temp_names[i];
    }

    /* Commit defaults to expr_extra pool */
    NovaExtraIdx ds = nova_ast_add_expr_list(T, field_count);
    for (int i = 0; i < field_count; i++) {
        nova_set_extra_expr(T, ds, i, temp_defaults[i]);
    }

    NovaStmtIdx si = nova_ast_add_stmt(T, NOVA_STMT_STRUCT, loc);
    if (si != NOVA_IDX_NONE) {
        NovaRowStmt *s = nova_get_stmt(T, si);
        s->as.struct_stmt.name = name;
        s->as.struct_stmt.name_len = name_len;
        s->as.struct_stmt.fields_start = ns;
        s->as.struct_stmt.field_count = field_count;
        s->as.struct_stmt.defaults_start = ds;
        s->as.struct_stmt.typed = 0;
    }
    return si;
}

/**
 * @brief Parse: typedec Name = basetype
 *
 * Syntax:
 *   typedec UserId = integer
 *   typedec Email  = string
 *
 * Creates a named type tag. At runtime, calling TypeName(value)
 * wraps the value in a tagged table { __type = "TypeName", value = v }
 * for nominal typing / domain tagging.
 */
static NovaStmtIdx novai_parse_typedec(NovaParser *P) {
    NovaSourceLoc loc = P->previous.loc;

    if (!novai_check(P, NOVA_TOKEN_NAME)) {
        novai_error(P, "expected name after 'typedec'");
        return NOVA_IDX_NONE;
    }

    const char *name = novai_dup_string(P,
        P->current.value.string.data,
        P->current.value.string.len);
    size_t name_len = P->current.value.string.len;
    novai_advance(P);

    (void)novai_expect(P, (NovaTokenType)'=', "in typedec declaration");

    if (!novai_check(P, NOVA_TOKEN_NAME)) {
        novai_error(P, "expected type name in typedec");
        return NOVA_IDX_NONE;
    }

    const char *base_type = novai_dup_string(P,
        P->current.value.string.data,
        P->current.value.string.len);
    size_t base_type_len = P->current.value.string.len;
    novai_advance(P);

    NovaStmtIdx si = nova_ast_add_stmt(T, NOVA_STMT_TYPEDEC, loc);
    if (si != NOVA_IDX_NONE) {
        NovaRowStmt *s = nova_get_stmt(T, si);
        s->as.typedec_stmt.name = name;
        s->as.typedec_stmt.name_len = name_len;
        s->as.typedec_stmt.base_type = base_type;
        s->as.typedec_stmt.base_type_len = base_type_len;
    }
    return si;
}

/**
 * @brief Parse: typedec enum Name { member [= value], ... }
 *
 * Creates a typed enum table with __type and __base metadata.
 * Uses curly-brace syntax with comma separators.
 */
static NovaStmtIdx novai_parse_typedec_enum(NovaParser *P) {
    NovaSourceLoc loc = P->previous.loc;

    /* 'enum' already consumed by caller; now expect Name { ... } */
    if (!novai_check(P, NOVA_TOKEN_NAME)) {
        novai_error(P, "expected name after 'typedec enum'");
        return NOVA_IDX_NONE;
    }

    const char *name = novai_dup_string(P,
        P->current.value.string.data,
        P->current.value.string.len);
    size_t name_len = P->current.value.string.len;
    novai_advance(P);

    (void)novai_expect(P, (NovaTokenType)'{', "to open typedec enum");

    /* Collect enum members */
    NovaRowNameRef temp_names[NOVAI_TEMP_MAX];
    NovaExprIdx    temp_values[NOVAI_TEMP_MAX];
    int member_count = 0;
    int has_values = 0;

    while (!novai_check(P, (NovaTokenType)'}') &&
           !novai_check(P, NOVA_TOKEN_EOF)) {

        while (novai_match(P, (NovaTokenType)',') ||
               novai_match(P, (NovaTokenType)';')) {
            /* skip */
        }

        if (novai_check(P, (NovaTokenType)'}')) break;

        if (!novai_check(P, NOVA_TOKEN_NAME)) {
            novai_error(P, "expected member name in typedec enum");
            return NOVA_IDX_NONE;
        }

        if (member_count >= NOVAI_TEMP_MAX) {
            novai_error(P, "too many members in typedec enum");
            return NOVA_IDX_NONE;
        }

        temp_names[member_count].data = novai_dup_string(P,
            P->current.value.string.data,
            P->current.value.string.len);
        temp_names[member_count].len = P->current.value.string.len;
        novai_advance(P);

        if (novai_match(P, (NovaTokenType)'=')) {
            temp_values[member_count] = novai_parse_expr(P, PREC_NONE);
            has_values = 1;
        } else {
            temp_values[member_count] = NOVA_IDX_NONE;
        }

        member_count++;
    }

    (void)novai_expect(P, (NovaTokenType)'}', "to close typedec enum");

    NovaNameIdx ns = nova_ast_add_names(T, member_count);
    for (int i = 0; i < member_count; i++) {
        T->names[ns + (uint32_t)i] = temp_names[i];
    }

    NovaExtraIdx vs = nova_ast_add_expr_list(T, member_count);
    for (int i = 0; i < member_count; i++) {
        nova_set_extra_expr(T, vs, i, temp_values[i]);
    }

    NovaStmtIdx si = nova_ast_add_stmt(T, NOVA_STMT_ENUM, loc);
    if (si != NOVA_IDX_NONE) {
        NovaRowStmt *s = nova_get_stmt(T, si);
        s->as.enum_stmt.name = name;
        s->as.enum_stmt.name_len = name_len;
        s->as.enum_stmt.members_start = ns;
        s->as.enum_stmt.member_count = member_count;
        s->as.enum_stmt.values_start = vs;
        s->as.enum_stmt.has_values = has_values;
        s->as.enum_stmt.typed = 1;
    }
    return si;
}

/**
 * @brief Parse: typedec struct Name { field [= default], ... }
 *
 * Creates a typed struct constructor using curly-brace syntax.
 * Instances include __base = "struct" metadata.
 */
static NovaStmtIdx novai_parse_typedec_struct(NovaParser *P) {
    NovaSourceLoc loc = P->previous.loc;

    if (!novai_check(P, NOVA_TOKEN_NAME)) {
        novai_error(P, "expected name after 'typedec struct'");
        return NOVA_IDX_NONE;
    }

    const char *name = novai_dup_string(P,
        P->current.value.string.data,
        P->current.value.string.len);
    size_t name_len = P->current.value.string.len;
    novai_advance(P);

    (void)novai_expect(P, (NovaTokenType)'{', "to open typedec struct");

    NovaRowNameRef temp_names[NOVAI_TEMP_MAX];
    NovaExprIdx    temp_defaults[NOVAI_TEMP_MAX];
    int field_count = 0;

    while (!novai_check(P, (NovaTokenType)'}') &&
           !novai_check(P, NOVA_TOKEN_EOF)) {

        while (novai_match(P, (NovaTokenType)',') ||
               novai_match(P, (NovaTokenType)';')) {
            /* skip */
        }

        if (novai_check(P, (NovaTokenType)'}')) break;

        if (!novai_check(P, NOVA_TOKEN_NAME)) {
            novai_error(P, "expected field name in typedec struct");
            return NOVA_IDX_NONE;
        }

        if (field_count >= NOVAI_TEMP_MAX) {
            novai_error(P, "too many fields in typedec struct");
            return NOVA_IDX_NONE;
        }

        temp_names[field_count].data = novai_dup_string(P,
            P->current.value.string.data,
            P->current.value.string.len);
        temp_names[field_count].len = P->current.value.string.len;
        novai_advance(P);

        if (novai_match(P, (NovaTokenType)'=')) {
            temp_defaults[field_count] = novai_parse_expr(P, PREC_NONE);
        } else {
            temp_defaults[field_count] = NOVA_IDX_NONE;
        }

        field_count++;
    }

    (void)novai_expect(P, (NovaTokenType)'}', "to close typedec struct");

    NovaNameIdx ns = nova_ast_add_names(T, field_count);
    for (int i = 0; i < field_count; i++) {
        T->names[ns + (uint32_t)i] = temp_names[i];
    }

    NovaExtraIdx ds = nova_ast_add_expr_list(T, field_count);
    for (int i = 0; i < field_count; i++) {
        nova_set_extra_expr(T, ds, i, temp_defaults[i]);
    }

    NovaStmtIdx si = nova_ast_add_stmt(T, NOVA_STMT_STRUCT, loc);
    if (si != NOVA_IDX_NONE) {
        NovaRowStmt *s = nova_get_stmt(T, si);
        s->as.struct_stmt.name = name;
        s->as.struct_stmt.name_len = name_len;
        s->as.struct_stmt.fields_start = ns;
        s->as.struct_stmt.field_count = field_count;
        s->as.struct_stmt.defaults_start = ds;
        s->as.struct_stmt.typed = 1;
    }
    return si;
}

/**
 * @brief Parse an expression statement or assignment
 */
static NovaStmtIdx novai_parse_expr_or_assign(NovaParser *P) {
    NovaSourceLoc loc = P->current.loc;
    NovaExprIdx first = novai_parse_suffixed_expr(P);

    /* Assignment: expr [, expr] = expr [, expr] */
    if (novai_check(P, (NovaTokenType)'=') ||
        novai_check(P, (NovaTokenType)',')) {

        NovaExprIdx temp_targets[NOVAI_TEMP_MAX];
        int target_count = 1;
        temp_targets[0] = first;

        while (novai_match(P, (NovaTokenType)',')) {
            if (target_count >= NOVAI_TEMP_MAX) {
                novai_error(P, "too many assignment targets");
                return NOVA_IDX_NONE;
            }
            temp_targets[target_count] =
                novai_parse_suffixed_expr(P);
            target_count++;
        }

        (void)novai_expect(P, (NovaTokenType)'=', "in assignment");

        /* Commit targets to extra pool */
        NovaExtraIdx ts = nova_ast_add_expr_list(T, target_count);
        for (int i = 0; i < target_count; i++) {
            nova_set_extra_expr(T, ts, i, temp_targets[i]);
        }

        /* Parse values */
        NovaExtraIdx vs = NOVA_IDX_NONE;
        int value_count = 0;
        (void)novai_parse_exprlist(P, &vs, &value_count);

        NovaStmtIdx si = nova_ast_add_stmt(T, NOVA_STMT_ASSIGN, loc);
        if (si != NOVA_IDX_NONE) {
            NovaRowStmt *s = nova_get_stmt(T, si);
            s->as.assign.targets_start = ts;
            s->as.assign.target_count = target_count;
            s->as.assign.values_start = vs;
            s->as.assign.value_count = value_count;
        }
        return si;
    }

    /* Expression statement */
    NovaStmtIdx si = nova_ast_add_stmt(T, NOVA_STMT_EXPR, loc);
    if (si != NOVA_IDX_NONE) {
        nova_get_stmt(T, si)->as.expr.expr = first;
    }
    return si;
}

/* ============================================================
 * MAIN STATEMENT DISPATCH
 * ============================================================ */

static NovaStmtIdx novai_parse_stmt(NovaParser *P) {
    if (P->panic_mode) {
        novai_synchronize(P);
    }

    switch ((int)P->current.type) {

    case NOVA_TOKEN_LOCAL:
        novai_advance(P);
        return novai_parse_local(P);

    case NOVA_TOKEN_IF:
        novai_advance(P);
        return novai_parse_if(P);

    case NOVA_TOKEN_WHILE:
        novai_advance(P);
        return novai_parse_while(P);

    case NOVA_TOKEN_REPEAT:
        novai_advance(P);
        return novai_parse_repeat(P);

    case NOVA_TOKEN_FOR:
        novai_advance(P);
        return novai_parse_for(P);

    case NOVA_TOKEN_FUNCTION:
        novai_advance(P);
        return novai_parse_function_stat(P);

    case NOVA_TOKEN_ASYNC:
        novai_advance(P);
        if (novai_check(P, NOVA_TOKEN_FUNCTION)) {
            novai_advance(P);
            {
                NovaStmtIdx fs = novai_parse_function_stat(P);
                if (fs != NOVA_IDX_NONE) {
                    nova_get_stmt(T, fs)->as.func_stmt.is_async = 1;
                }
                return fs;
            }
        }
        novai_error(P, "expected 'function' after 'async'");
        return NOVA_IDX_NONE;

    case NOVA_TOKEN_DO:
        novai_advance(P);
        return novai_parse_do(P);

    case NOVA_TOKEN_RETURN:
        novai_advance(P);
        return novai_parse_return(P);

    case NOVA_TOKEN_BREAK:
        novai_advance(P);
        return nova_ast_add_stmt(T, NOVA_STMT_BREAK, P->previous.loc);

    case NOVA_TOKEN_CONTINUE:
        novai_advance(P);
        return nova_ast_add_stmt(T, NOVA_STMT_CONTINUE, P->previous.loc);

    case NOVA_TOKEN_GOTO:
        novai_advance(P);
        return novai_parse_goto(P);

    case NOVA_TOKEN_DBCOLON:
        novai_advance(P);
        return novai_parse_label(P);

    case NOVA_TOKEN_IMPORT:
        novai_advance(P);
        return novai_parse_import(P);

    case NOVA_TOKEN_EXPORT:
        novai_advance(P);
        return novai_parse_export(P);

    case NOVA_TOKEN_CONST:
        novai_advance(P);
        return novai_parse_const(P);

    case NOVA_TOKEN_ENUM:
        novai_advance(P);
        return novai_parse_enum(P);

    case NOVA_TOKEN_STRUCT:
        novai_advance(P);
        return novai_parse_struct(P);

    case NOVA_TOKEN_TYPEDEC:
        novai_advance(P);
        if (novai_match(P, NOVA_TOKEN_ENUM)) {
            return novai_parse_typedec_enum(P);
        }
        if (novai_match(P, NOVA_TOKEN_STRUCT)) {
            return novai_parse_typedec_struct(P);
        }
        return novai_parse_typedec(P);

    case (NovaTokenType)';':
        novai_advance(P);
        return novai_parse_stmt(P);

    default:
        return novai_parse_expr_or_assign(P);
    }
}

/* ============================================================
 * BLOCK PARSING
 * ============================================================ */

static int novai_is_block_end(const NovaParser *P) {
    switch (P->current.type) {
        case NOVA_TOKEN_END:
        case NOVA_TOKEN_ELSE:
        case NOVA_TOKEN_ELSEIF:
        case NOVA_TOKEN_UNTIL:
        case NOVA_TOKEN_EOF:
            return 1;
        default:
            return 0;
    }
}

static NovaBlockIdx novai_parse_block(NovaParser *P) {
    NovaBlockIdx bi = nova_ast_add_block(T, P->current.loc);
    if (bi == NOVA_IDX_NONE) {
        novai_error(P, "out of memory");
        return NOVA_IDX_NONE;
    }

    while (!novai_is_block_end(P)) {
        NovaStmtIdx si = novai_parse_stmt(P);
        if (si != NOVA_IDX_NONE) {
            nova_ast_block_append(T, bi, si);
        }

        /* If we had a return, stop parsing this block */
        if (si != NOVA_IDX_NONE &&
            nova_get_stmt(T, si)->kind == NOVA_STMT_RETURN) {
            break;
        }
    }

    return bi;
}

/* ============================================================
 * PUBLIC API IMPLEMENTATION
 * ============================================================ */

/**
 * @brief Parse the token stream into a row-based AST table
 *
 * Populates P->table with all AST nodes. The table owns all
 * memory via its arena -- call nova_ast_table_destroy(&P->table)
 * to free everything.
 *
 * @param P            Parser state (already initialized via
 *                     nova_parser_init)
 * @param source_name  Source filename (not owned, must outlive table)
 * @return 0 on success, -1 on fatal error
 */
int nova_parse_row(NovaParser *P, const char *source_name) {
    if (P == NULL) {
        return -1;
    }

    /* Initialize the AST table */
    P->table = nova_ast_table_create(source_name);
    if (P->table.arena == NULL) {
        novai_error(P, "out of memory creating AST table");
        return -1;
    }

    /* Parse the top-level block */
    P->table.root = novai_parse_block(P);

    if (!novai_check(P, NOVA_TOKEN_EOF)) {
        novai_error(P, "unexpected token %s at top level",
                    nova_token_name(P->current.type));
    }

    return P->had_error ? -1 : 0;
}

/**
 * @brief Parse a single expression into the row-based table
 *
 * P->table must already be initialized (e.g. by a prior call
 * to nova_parse_row, or by manual nova_ast_table_create).
 *
 * @param P Parser state
 * @return Expression index, or NOVA_IDX_NONE on error
 */
NovaExprIdx nova_parse_row_expression(NovaParser *P) {
    if (P == NULL || P->table.arena == NULL) {
        return NOVA_IDX_NONE;
    }
    return novai_parse_expr(P, PREC_NONE);
}

#undef T

