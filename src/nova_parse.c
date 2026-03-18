/**
 * @file nova_parse.c
 * @brief Nova Language - Parser (Recursive Descent + Pratt TDOP)
 *
 * Architecture:
 *   - Statements: Classic recursive descent. Each statement type
 *     has its own parse function (novai_parse_if, novai_parse_while, etc.)
 *   - Expressions: Pratt (Top-Down Operator Precedence) parser.
 *     A single novai_parse_expr(min_prec) function handles ALL
 *     expression parsing with a prefix/infix dispatch table.
 *   - AST output: Both produce NovaExpr/NovaStmt/NovaBlock nodes.
 *
 * The parser reads from the preprocessor's expanded token buffer
 * via nova_pp_next_token(), so macros and conditionals are fully
 * resolved before we see a single token.
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
 *   - nova_parse.h (public API)
 *   - nova_ast.h (AST node types)
 *   - nova_pp.h (token source)
 */

#include "nova/nova_parse.h"
#include "nova/nova_error.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

/* ============================================================
 * PRATT PRECEDENCE LEVELS
 *
 * Nova's operator precedence (low to high), matching Lua 5.4:
 *
 *   1  or
 *   2  and
 *   3  <  >  <=  >=  ~=  ==
 *   4  |
 *   5  ~
 *   6  &
 *   7  <<  >>
 *   8  ..             (right-associative)
 *   9  +  -
 *  10  *  /  //  %
 *  11  unary: not  #  -  ~
 *  12  ^              (right-associative)
 *
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
    PREC_POSTFIX    = 13,  /* call, index, field */
    PREC_PRIMARY    = 14,
} NovaPrecedence;

/* ============================================================
 * TOKEN STREAM HELPERS
 * ============================================================ */

/**
 * @brief Advance to the next token from the PP stream
 */
static void novai_advance(NovaParser *P) {
    P->previous = P->current;
    (void)nova_pp_next_token(P->pp, &P->current);
}

/**
 * @brief Check if current token is of given type
 */
static inline int novai_check(const NovaParser *P, NovaTokenType type) {
    return P->current.type == type;
}

/**
 * @brief Check if current token is a NAME or a keyword usable as a field name
 *
 * Keywords are valid field/method names (e.g. coroutine.yield, t.end).
 * All keyword tokens preserve their string data from the lexer, so they
 * can be used as identifiers in field-access and method-call positions.
 *
 * @return 1 if usable as field name, 0 otherwise
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

/**
 * @brief Consume current token if it matches, otherwise do nothing
 *
 * @return 1 if consumed, 0 if no match
 */
static int novai_match(NovaParser *P, NovaTokenType type) {
    if (P->current.type == type) {
        novai_advance(P);
        return 1;
    }
    return 0;
}

/**
 * @brief Infer the best error code from a parser error message.
 */
static NovaErrorCode novai_infer_parse_code(const char *msg) {
    if (msg == NULL) { return NOVA_E1001; }
    if (strncmp(msg, "expected", 8) == 0)               return NOVA_E1014;
    if (strncmp(msg, "unexpected", 10) == 0)             return NOVA_E1001;
    if (strncmp(msg, "out of memory", 13) == 0)          return NOVA_E1017;
    return NOVA_E1001;
}

/**
 * @brief Report a parse error
 */
static void novai_error(NovaParser *P, const char *fmt, ...) {
    /* Don't cascade errors in panic mode */
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
    (void)snprintf(P->error_msg, sizeof(P->error_msg),
                   "%s:%d:%d: error: %s",
                   loc->filename ? loc->filename : "<input>",
                   loc->line, loc->column, msg);

    /* Emit rich colored diagnostic */
    nova_diag_report(NOVA_DIAG_ERROR, novai_infer_parse_code(msg),
                     loc->filename ? loc->filename : "<input>",
                     loc->line, loc->column,
                     "%s", msg);
}

/**
 * @brief Expect current token to be of given type; error if not
 *
 * Consumes the token on match.
 *
 * @return 1 on success, 0 on error
 */
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

/**
 * @brief Synchronize after an error (skip tokens to a safe point)
 *
 * Skips tokens until we find a statement boundary, allowing
 * the parser to recover and report more errors.
 */
static void novai_synchronize(NovaParser *P) {
    P->panic_mode = 0;

    while (P->current.type != NOVA_TOKEN_EOF) {
        /* Previous token was a statement terminator */
        /* (In Lua/Nova, newlines don't terminate -- look for keywords) */

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
 * AST NODE CONSTRUCTORS AND DESTRUCTORS
 * ============================================================ */

NovaExpr *nova_expr_new(NovaExprType kind, NovaSourceLoc loc) {
    NovaExpr *e = (NovaExpr *)calloc(1, sizeof(NovaExpr));
    if (e != NULL) {
        e->kind = kind;
        e->loc = loc;
    }
    return e;
}

NovaStmt *nova_stmt_new(NovaStmtType kind, NovaSourceLoc loc) {
    NovaStmt *s = (NovaStmt *)calloc(1, sizeof(NovaStmt));
    if (s != NULL) {
        s->kind = kind;
        s->loc = loc;
        s->next = NULL;
    }
    return s;
}

NovaBlock *nova_block_new(NovaSourceLoc loc) {
    NovaBlock *b = (NovaBlock *)calloc(1, sizeof(NovaBlock));
    if (b != NULL) {
        b->loc = loc;
    }
    return b;
}

void nova_block_append(NovaBlock *block, NovaStmt *stmt) {
    if (block == NULL || stmt == NULL) {
        return;
    }
    stmt->next = NULL;
    if (block->last != NULL) {
        block->last->next = stmt;
    } else {
        block->first = stmt;
    }
    block->last = stmt;
    block->stmt_count++;
}

/* ---- Free functions ---- */

void nova_expr_free(NovaExpr *expr) {
    if (expr == NULL) {
        return;
    }

    switch (expr->kind) {
        case NOVA_EXPR_UNARY:
            nova_expr_free(expr->as.unary.operand);
            break;

        case NOVA_EXPR_BINARY:
            nova_expr_free(expr->as.binary.left);
            nova_expr_free(expr->as.binary.right);
            break;

        case NOVA_EXPR_AWAIT:
        case NOVA_EXPR_SPAWN:
        case NOVA_EXPR_YIELD:
            nova_expr_free(expr->as.async_op.operand);
            break;

        case NOVA_EXPR_CALL:
        case NOVA_EXPR_METHOD_CALL:
            nova_expr_free(expr->as.call.callee);
            for (int i = 0; i < expr->as.call.arg_count; i++) {
                nova_expr_free(expr->as.call.args[i]);
            }
            free(expr->as.call.args);
            break;

        case NOVA_EXPR_INDEX:
            nova_expr_free(expr->as.index.object);
            nova_expr_free(expr->as.index.index);
            break;

        case NOVA_EXPR_FIELD:
        case NOVA_EXPR_METHOD:
            nova_expr_free(expr->as.field.object);
            break;

        case NOVA_EXPR_TABLE:
            for (int i = 0; i < expr->as.table.field_count; i++) {
                nova_expr_free(expr->as.table.fields[i].key);
                nova_expr_free(expr->as.table.fields[i].value);
            }
            free(expr->as.table.fields);
            break;

        case NOVA_EXPR_FUNCTION:
            free(expr->as.function.params);
            nova_block_free(expr->as.function.body);
            break;

        case NOVA_EXPR_INTERP_STRING:
            for (int i = 0; i < expr->as.interp.part_count; i++) {
                nova_expr_free(expr->as.interp.parts[i]);
            }
            free(expr->as.interp.parts);
            break;

        default:
            /* Literals and names own nothing */
            break;
    }

    free(expr);
}

void nova_stmt_free(NovaStmt *stmt) {
    if (stmt == NULL) {
        return;
    }

    switch (stmt->kind) {
        case NOVA_STMT_EXPR:
            nova_expr_free(stmt->as.expr.expr);
            break;

        case NOVA_STMT_LOCAL:
            free(stmt->as.local.names);
            free(stmt->as.local.name_lens);
            for (int i = 0; i < stmt->as.local.value_count; i++) {
                nova_expr_free(stmt->as.local.values[i]);
            }
            free(stmt->as.local.values);
            break;

        case NOVA_STMT_ASSIGN:
            for (int i = 0; i < stmt->as.assign.target_count; i++) {
                nova_expr_free(stmt->as.assign.targets[i]);
            }
            free(stmt->as.assign.targets);
            for (int i = 0; i < stmt->as.assign.value_count; i++) {
                nova_expr_free(stmt->as.assign.values[i]);
            }
            free(stmt->as.assign.values);
            break;

        case NOVA_STMT_IF:
            for (int i = 0; i < stmt->as.if_stmt.branch_count; i++) {
                nova_expr_free(stmt->as.if_stmt.branches[i].condition);
                nova_block_free(stmt->as.if_stmt.branches[i].body);
            }
            free(stmt->as.if_stmt.branches);
            break;

        case NOVA_STMT_WHILE:
            nova_expr_free(stmt->as.while_stmt.condition);
            nova_block_free(stmt->as.while_stmt.body);
            break;

        case NOVA_STMT_REPEAT:
            nova_block_free(stmt->as.repeat_stmt.body);
            nova_expr_free(stmt->as.repeat_stmt.condition);
            break;

        case NOVA_STMT_FOR_NUMERIC:
            nova_expr_free(stmt->as.for_numeric.start);
            nova_expr_free(stmt->as.for_numeric.stop);
            nova_expr_free(stmt->as.for_numeric.step);
            nova_block_free(stmt->as.for_numeric.body);
            break;

        case NOVA_STMT_FOR_GENERIC:
            free(stmt->as.for_generic.names);
            free(stmt->as.for_generic.name_lens);
            for (int i = 0; i < stmt->as.for_generic.iter_count; i++) {
                nova_expr_free(stmt->as.for_generic.iterators[i]);
            }
            free(stmt->as.for_generic.iterators);
            nova_block_free(stmt->as.for_generic.body);
            break;

        case NOVA_STMT_DO:
            nova_block_free(stmt->as.do_stmt.body);
            break;

        case NOVA_STMT_RETURN:
            for (int i = 0; i < stmt->as.return_stmt.value_count; i++) {
                nova_expr_free(stmt->as.return_stmt.values[i]);
            }
            free(stmt->as.return_stmt.values);
            break;

        case NOVA_STMT_FUNCTION:
        case NOVA_STMT_LOCAL_FUNCTION:
            nova_expr_free(stmt->as.func_stmt.name);
            free(stmt->as.func_stmt.params);
            nova_block_free(stmt->as.func_stmt.body);
            break;

        case NOVA_STMT_IMPORT:
            /* String data not owned */
            break;

        case NOVA_STMT_EXPORT:
            nova_expr_free(stmt->as.export_stmt.value);
            break;

        case NOVA_STMT_CONST:
            nova_expr_free(stmt->as.const_stmt.value);
            break;

        default:
            break;
    }

    free(stmt);
}

void nova_block_free(NovaBlock *block) {
    if (block == NULL) {
        return;
    }
    NovaStmt *s = block->first;
    while (s != NULL) {
        NovaStmt *next = s->next;
        nova_stmt_free(s);
        s = next;
    }
    free(block);
}

void nova_ast_free(NovaAST *ast) {
    if (ast == NULL) {
        return;
    }
    nova_block_free(ast->body);
    free(ast);
}

/* ============================================================
 * FORWARD DECLARATIONS (mutual recursion)
 * ============================================================ */

static NovaExpr *novai_parse_expr(NovaParser *P, int min_prec);
static NovaStmt *novai_parse_stmt(NovaParser *P);
static NovaBlock *novai_parse_block(NovaParser *P);
static NovaExpr *novai_parse_suffixed_expr(NovaParser *P);
static int novai_is_block_end(const NovaParser *P);

/* ============================================================
 * PRATT EXPRESSION PARSER
 *
 * The heart of the expression parser. Uses a single function
 * with a precedence level parameter to handle all binary
 * operators. Prefix expressions (literals, unary, names, etc.)
 * are handled by novai_parse_prefix().
 * ============================================================ */

/**
 * @brief Get precedence of a binary operator token
 *
 * @return Precedence level (0 = not a binary operator)
 */
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

/**
 * @brief Is this operator right-associative?
 *
 * Right-associative operators parse with min_prec instead of
 * min_prec + 1, so they bind rightward: a ^ b ^ c = a ^ (b ^ c)
 */
static int novai_is_right_assoc(NovaTokenType type) {
    return (type == (NovaTokenType)'^' ||
            type == NOVA_TOKEN_DOTDOT);
}

/**
 * @brief Map a binary token to a NovaBinOp enum value
 */
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
        default:                    return NOVA_BINOP_ADD; /* unreachable */
    }
}

/* ============================================================
 * PREFIX EXPRESSION PARSING
 * ============================================================ */

/**
 * @brief Parse an argument list: (expr, expr, ...)
 *
 * @param P        Parser state
 * @param args     Output: argument expression array
 * @param count    Output: number of arguments
 * @return 0 on success, -1 on error
 */
static int novai_parse_args(NovaParser *P, NovaExpr ***args, int *count) {
    int cap = 8;
    int n = 0;

    *args = (NovaExpr **)calloc((size_t)cap, sizeof(NovaExpr *));
    if (*args == NULL) {
        novai_error(P, "out of memory");
        return -1;
    }

    /* Handle special Lua-style call syntax:
     *   f "string"   -> f("string")
     *   f { table }  -> f({table})
     *
     * For strings, we parse a full expression so that concatenation
     * and other operators work naturally:
     *   echo "hello " .. name   -> echo("hello " .. name)
     */
    if (P->current.type == NOVA_TOKEN_STRING ||
        P->current.type == NOVA_TOKEN_LONG_STRING) {
        (*args)[0] = novai_parse_expr(P, PREC_NONE);
        *count = 1;
        return 0;
    }

    if (P->current.type == (NovaTokenType)'{') {
        (*args)[0] = novai_parse_expr(P, PREC_NONE);
        *count = 1;
        return 0;
    }

    /* Standard parenthesized arguments */
    if (!novai_expect(P, (NovaTokenType)'(', "in function call")) {
        free(*args);
        *args = NULL;
        *count = 0;
        return -1;
    }

    if (!novai_check(P, (NovaTokenType)')')) {
        do {
            if (n >= cap) {
                cap *= 2;
                NovaExpr **new_args = (NovaExpr **)realloc(
                    *args, (size_t)cap * sizeof(NovaExpr *)
                );
                if (new_args == NULL) {
                    novai_error(P, "out of memory");
                    return -1;
                }
                *args = new_args;
            }

            (*args)[n] = novai_parse_expr(P, PREC_NONE);
            n++;
        } while (novai_match(P, (NovaTokenType)','));
    }

    (void)novai_expect(P, (NovaTokenType)')', "after arguments");
    *count = n;
    return 0;
}

/**
 * @brief Parse a table constructor: { fields }
 */
static NovaExpr *novai_parse_table(NovaParser *P) {
    NovaSourceLoc loc = P->current.loc;
    novai_advance(P); /* consume '{' */

    int cap = 8;
    int count = 0;
    NovaTableField *fields = (NovaTableField *)calloc(
        (size_t)cap, sizeof(NovaTableField)
    );
    if (fields == NULL) {
        novai_error(P, "out of memory");
        return NULL;
    }

    while (!novai_check(P, (NovaTokenType)'}') &&
           !novai_check(P, NOVA_TOKEN_EOF)) {

        if (count >= cap) {
            cap *= 2;
            NovaTableField *nf = (NovaTableField *)realloc(
                fields, (size_t)cap * sizeof(NovaTableField)
            );
            if (nf == NULL) {
                novai_error(P, "out of memory");
                free(fields);
                return NULL;
            }
            fields = nf;
        }

        NovaTableField *f = &fields[count];
        memset(f, 0, sizeof(NovaTableField));
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
        /* name = expr  (check: NAME followed by '=') */
        else if (P->current.type == NOVA_TOKEN_NAME) {
            /* Lookahead: is the next token '=' ? */
            NovaToken save = P->current;
            novai_advance(P);
            if (novai_check(P, (NovaTokenType)'=')) {
                /* Record-style field */
                f->kind = NOVA_FIELD_RECORD;
                f->key = nova_expr_new(NOVA_EXPR_STRING, save.loc);
                if (f->key != NULL) {
                    f->key->as.string.data = save.value.string.data;
                    f->key->as.string.len = save.value.string.len;
                }
                novai_advance(P); /* consume '=' */
                f->value = novai_parse_expr(P, PREC_NONE);
            } else {
                /* Push back: it's just an expression.
                 * We consumed one token too many. The "save" was the
                 * NAME and P->current is the token after it.
                 * We need to reconstruct the expression.
                 * Since we can't push back, parse the rest as a
                 * suffixed expression starting from save. */

                /* Build a name expression from the saved token */
                f->kind = NOVA_FIELD_LIST;
                NovaExpr *name_expr = nova_expr_new(NOVA_EXPR_NAME, save.loc);
                if (name_expr != NULL) {
                    name_expr->as.string.data = save.value.string.data;
                    name_expr->as.string.len = save.value.string.len;
                }
                /* If there are suffix ops (call, index, field), we'd need
                 * to handle them. For now, treat as simple expression. */
                f->value = name_expr;
            }
        }
        /* Positional: just an expression */
        else {
            f->kind = NOVA_FIELD_LIST;
            f->value = novai_parse_expr(P, PREC_NONE);
        }

        count++;

        /* Field separators: ',' or ';' */
        if (!novai_match(P, (NovaTokenType)',') &&
            !novai_match(P, (NovaTokenType)';')) {
            break;
        }
    }

    (void)novai_expect(P, (NovaTokenType)'}', "to close table");

    NovaExpr *expr = nova_expr_new(NOVA_EXPR_TABLE, loc);
    if (expr != NULL) {
        expr->as.table.fields = fields;
        expr->as.table.field_count = count;
    } else {
        free(fields);
    }
    return expr;
}

/**
 * @brief Parse a function body: (params) block end
 *
 * Used for both `function() end` expressions and statements.
 */
static int novai_parse_funcbody(NovaParser *P,
                                NovaParam **out_params,
                                int *out_param_count,
                                int *out_variadic,
                                NovaBlock **out_body) {
    /* Parameters */
    (void)novai_expect(P, (NovaTokenType)'(', "in function definition");

    int cap = 8;
    int count = 0;
    int variadic = 0;
    NovaParam *params = (NovaParam *)calloc((size_t)cap, sizeof(NovaParam));
    if (params == NULL) {
        novai_error(P, "out of memory");
        return -1;
    }

    if (!novai_check(P, (NovaTokenType)')')) {
        do {
            if (novai_match(P, NOVA_TOKEN_DOTDOTDOT)) {
                variadic = 1;
                break;
            }
            if (!novai_check(P, NOVA_TOKEN_NAME)) {
                novai_error(P, "expected parameter name");
                free(params);
                return -1;
            }
            if (count >= cap) {
                cap *= 2;
                NovaParam *np = (NovaParam *)realloc(
                    params, (size_t)cap * sizeof(NovaParam)
                );
                if (np == NULL) {
                    free(params);
                    return -1;
                }
                params = np;
            }
            params[count].name = P->current.value.string.data;
            params[count].name_len = P->current.value.string.len;
            params[count].loc = P->current.loc;
            count++;
            novai_advance(P);
        } while (novai_match(P, (NovaTokenType)','));
    }

    (void)novai_expect(P, (NovaTokenType)')', "after parameters");

    /* Body */
    NovaBlock *body = novai_parse_block(P);

    (void)novai_expect(P, NOVA_TOKEN_END, "to close function body");

    *out_params = params;
    *out_param_count = count;
    *out_variadic = variadic;
    *out_body = body;
    return 0;
}

/**
 * @brief Parse a prefix expression (the "nud" in Pratt terms)
 *
 * Handles: literals, names, unary operators, parenthesized
 * expressions, table constructors, function literals.
 */
static NovaExpr *novai_parse_prefix(NovaParser *P) {
    NovaSourceLoc loc = P->current.loc;

    switch ((int)P->current.type) {

    /* ---- Literals ---- */
    case NOVA_TOKEN_NIL:
        novai_advance(P);
        return nova_expr_new(NOVA_EXPR_NIL, loc);

    case NOVA_TOKEN_TRUE:
        novai_advance(P);
        return nova_expr_new(NOVA_EXPR_TRUE, loc);

    case NOVA_TOKEN_FALSE:
        novai_advance(P);
        return nova_expr_new(NOVA_EXPR_FALSE, loc);

    case NOVA_TOKEN_INTEGER: {
        NovaExpr *e = nova_expr_new(NOVA_EXPR_INTEGER, loc);
        if (e != NULL) {
            e->as.integer = P->current.value.integer;
        }
        novai_advance(P);
        return e;
    }

    case NOVA_TOKEN_NUMBER: {
        NovaExpr *e = nova_expr_new(NOVA_EXPR_NUMBER, loc);
        if (e != NULL) {
            e->as.number = P->current.value.number;
        }
        novai_advance(P);
        return e;
    }

    case NOVA_TOKEN_STRING:
    case NOVA_TOKEN_LONG_STRING: {
        NovaExpr *e = nova_expr_new(NOVA_EXPR_STRING, loc);
        if (e != NULL) {
            e->as.string.data = P->current.value.string.data;
            e->as.string.len = P->current.value.string.len;
        }
        novai_advance(P);
        return e;
    }

    case NOVA_TOKEN_INTERP_START: {
        /*
         * Parse backtick interpolated string with multi-part support.
         * Token stream: INTERP_START, INTERP_SEGMENT/expr*..., INTERP_END
         */
        novai_advance(P); /* consume INTERP_START */

        /* Collect parts into temp array */
        NovaExpr *parts[64];
        int nparts = 0;

        while (!novai_check(P, NOVA_TOKEN_INTERP_END) &&
               !novai_check(P, NOVA_TOKEN_EOF)) {

            if (novai_check(P, NOVA_TOKEN_INTERP_SEGMENT)) {
                if (P->current.value.string.len > 0 && nparts < 64) {
                    NovaExpr *seg = nova_expr_new(NOVA_EXPR_STRING, loc);
                    if (seg != NULL) {
                        seg->as.string.data = P->current.value.string.data;
                        seg->as.string.len = P->current.value.string.len;
                    }
                    parts[nparts++] = seg;
                }
                novai_advance(P);
            }

            if (!novai_check(P, NOVA_TOKEN_INTERP_END) &&
                !novai_check(P, NOVA_TOKEN_INTERP_SEGMENT) &&
                !novai_check(P, NOVA_TOKEN_EOF)) {
                if (nparts < 64) {
                    parts[nparts++] = novai_parse_expr(P, PREC_NONE);
                }
            }
        }

        novai_match(P, NOVA_TOKEN_INTERP_END);

        NovaExpr *e = nova_expr_new(NOVA_EXPR_INTERP_STRING, loc);
        if (e != NULL) {
            int count = nparts > 0 ? nparts : 1;
            e->as.interp.parts = (NovaExpr **)calloc((size_t)count, sizeof(NovaExpr *));
            if (e->as.interp.parts != NULL) {
                if (nparts > 0) {
                    for (int i = 0; i < nparts; i++) {
                        e->as.interp.parts[i] = parts[i];
                    }
                    e->as.interp.part_count = nparts;
                } else {
                    NovaExpr *empty = nova_expr_new(NOVA_EXPR_STRING, loc);
                    if (empty != NULL) {
                        empty->as.string.data = "";
                        empty->as.string.len = 0;
                    }
                    e->as.interp.parts[0] = empty;
                    e->as.interp.part_count = 1;
                }
            }
        }
        return e;
    }

    case NOVA_TOKEN_DOTDOTDOT:
        novai_advance(P);
        return nova_expr_new(NOVA_EXPR_VARARG, loc);

    /* ---- Identifier (variable name) ---- */
    case NOVA_TOKEN_NAME: {
        NovaExpr *e = nova_expr_new(NOVA_EXPR_NAME, loc);
        if (e != NULL) {
            e->as.string.data = P->current.value.string.data;
            e->as.string.len = P->current.value.string.len;
        }
        novai_advance(P);
        return e;
    }

    /* ---- Parenthesized expression ---- */
    case (NovaTokenType)'(': {
        novai_advance(P); /* consume '(' */
        NovaExpr *e = novai_parse_expr(P, PREC_NONE);
        (void)novai_expect(P, (NovaTokenType)')', "after expression");
        return e;
    }

    /* ---- Table constructor ---- */
    case (NovaTokenType)'{':
        return novai_parse_table(P);

    /* ---- Unary operators ---- */
    case (NovaTokenType)'-': {
        novai_advance(P);
        NovaExpr *operand = novai_parse_expr(P, PREC_UNARY);
        NovaExpr *e = nova_expr_new(NOVA_EXPR_UNARY, loc);
        if (e != NULL) {
            e->as.unary.op = NOVA_UNOP_NEGATE;
            e->as.unary.operand = operand;
        }
        return e;
    }

    case NOVA_TOKEN_NOT: {
        novai_advance(P);
        NovaExpr *operand = novai_parse_expr(P, PREC_UNARY);
        NovaExpr *e = nova_expr_new(NOVA_EXPR_UNARY, loc);
        if (e != NULL) {
            e->as.unary.op = NOVA_UNOP_NOT;
            e->as.unary.operand = operand;
        }
        return e;
    }

    case (NovaTokenType)'#': {
        novai_advance(P);
        NovaExpr *operand = novai_parse_expr(P, PREC_UNARY);
        NovaExpr *e = nova_expr_new(NOVA_EXPR_UNARY, loc);
        if (e != NULL) {
            e->as.unary.op = NOVA_UNOP_LEN;
            e->as.unary.operand = operand;
        }
        return e;
    }

    case (NovaTokenType)'~': {
        novai_advance(P);
        NovaExpr *operand = novai_parse_expr(P, PREC_UNARY);
        NovaExpr *e = nova_expr_new(NOVA_EXPR_UNARY, loc);
        if (e != NULL) {
            e->as.unary.op = NOVA_UNOP_BNOT;
            e->as.unary.operand = operand;
        }
        return e;
    }

    /* ---- Function literal: function(...) ... end ---- */
    case NOVA_TOKEN_FUNCTION: {
        novai_advance(P);
        NovaParam *params = NULL;
        int param_count = 0;
        int variadic = 0;
        NovaBlock *body = NULL;

        if (novai_parse_funcbody(P, &params, &param_count,
                                 &variadic, &body) != 0) {
            return NULL;
        }

        NovaExpr *e = nova_expr_new(NOVA_EXPR_FUNCTION, loc);
        if (e != NULL) {
            e->as.function.params = params;
            e->as.function.param_count = param_count;
            e->as.function.is_variadic = variadic;
            e->as.function.is_async = 0;
            e->as.function.body = body;
        } else {
            free(params);
            nova_block_free(body);
        }
        return e;
    }

    /* ---- async function(...) ... end ---- */
    case NOVA_TOKEN_ASYNC: {
        novai_advance(P);
        if (!novai_check(P, NOVA_TOKEN_FUNCTION)) {
            novai_error(P, "expected 'function' after 'async'");
            return NULL;
        }
        novai_advance(P); /* consume 'function' */
        NovaParam *params = NULL;
        int param_count = 0;
        int variadic = 0;
        NovaBlock *body = NULL;

        if (novai_parse_funcbody(P, &params, &param_count,
                                 &variadic, &body) != 0) {
            return NULL;
        }

        NovaExpr *e = nova_expr_new(NOVA_EXPR_FUNCTION, loc);
        if (e != NULL) {
            e->as.function.params = params;
            e->as.function.param_count = param_count;
            e->as.function.is_variadic = variadic;
            e->as.function.is_async = 1;
            e->as.function.body = body;
        } else {
            free(params);
            nova_block_free(body);
        }
        return e;
    }

    /* ---- await expr ---- */
    case NOVA_TOKEN_AWAIT: {
        novai_advance(P);
        NovaExpr *operand = novai_parse_expr(P, PREC_UNARY);
        NovaExpr *e = nova_expr_new(NOVA_EXPR_AWAIT, loc);
        if (e != NULL) {
            e->as.async_op.operand = operand;
        }
        return e;
    }

    /* ---- spawn expr ---- */
    case NOVA_TOKEN_SPAWN: {
        novai_advance(P);
        NovaExpr *operand = novai_parse_expr(P, PREC_UNARY);
        NovaExpr *e = nova_expr_new(NOVA_EXPR_SPAWN, loc);
        if (e != NULL) {
            e->as.async_op.operand = operand;
        }
        return e;
    }

    /* ---- yield [expr] ---- */
    case NOVA_TOKEN_YIELD: {
        novai_advance(P);
        NovaExpr *operand = NULL;
        /* yield can be bare (no expression) or with a value */
        if (!novai_is_block_end(P) &&
            P->current.type != (NovaTokenType)')' &&
            P->current.type != (NovaTokenType)';' &&
            P->current.type != (NovaTokenType)',') {
            operand = novai_parse_expr(P, PREC_NONE);
        }
        NovaExpr *e = nova_expr_new(NOVA_EXPR_YIELD, loc);
        if (e != NULL) {
            e->as.async_op.operand = operand;
        }
        return e;
    }

    default:
        novai_error(P, "unexpected token %s in expression",
                    nova_token_name(P->current.type));
        novai_advance(P);
        return NULL;
    }
}

/**
 * @brief Parse suffix operations: call, index, field, method
 *
 * Handles the postfix chain: f(x).y[z]:w(a)
 */
static NovaExpr *novai_parse_suffixed_expr(NovaParser *P) {
    NovaExpr *expr = novai_parse_prefix(P);

    while (1) {
        NovaSourceLoc loc = P->current.loc;

        /* Field access: expr.name (keywords valid as field names) */
        if (novai_match(P, (NovaTokenType)'.')) {
            if (!novai_check_field_name(P)) {
                novai_error(P, "expected field name after '.'");
                return expr;
            }
            NovaExpr *e = nova_expr_new(NOVA_EXPR_FIELD, loc);
            if (e != NULL) {
                e->as.field.object = expr;
                e->as.field.name = P->current.value.string.data;
                e->as.field.name_len = P->current.value.string.len;
            }
            novai_advance(P);
            expr = e;
        }
        /* Method call: expr:name(args) */
        else if (novai_match(P, (NovaTokenType)':')) {
            if (!novai_check_field_name(P)) {
                novai_error(P, "expected method name after ':'");
                return expr;
            }

            NovaExpr *method = nova_expr_new(NOVA_EXPR_METHOD, loc);
            if (method != NULL) {
                method->as.field.object = expr;
                method->as.field.name = P->current.value.string.data;
                method->as.field.name_len = P->current.value.string.len;
            }
            novai_advance(P);

            /* Now parse the call arguments */
            NovaExpr **call_args = NULL;
            int call_count = 0;
            if (novai_parse_args(P, &call_args, &call_count) != 0) {
                nova_expr_free(method);
                return NULL;
            }

            NovaExpr *call = nova_expr_new(NOVA_EXPR_METHOD_CALL, loc);
            if (call != NULL) {
                call->as.call.callee = method;
                call->as.call.args = call_args;
                call->as.call.arg_count = call_count;
            }
            expr = call;
        }
        /* Index: expr[index_expr] */
        else if (novai_match(P, (NovaTokenType)'[')) {
            NovaExpr *idx = novai_parse_expr(P, PREC_NONE);
            (void)novai_expect(P, (NovaTokenType)']', "after index");

            NovaExpr *e = nova_expr_new(NOVA_EXPR_INDEX, loc);
            if (e != NULL) {
                e->as.index.object = expr;
                e->as.index.index = idx;
            }
            expr = e;
        }
        /* Function call: expr(args)  or  expr "str"  or  expr {table} */
        else if (P->current.type == (NovaTokenType)'(' ||
                 P->current.type == NOVA_TOKEN_STRING ||
                 P->current.type == NOVA_TOKEN_LONG_STRING ||
                 P->current.type == (NovaTokenType)'{') {
            NovaExpr **call_args = NULL;
            int call_count = 0;
            if (novai_parse_args(P, &call_args, &call_count) != 0) {
                nova_expr_free(expr);
                return NULL;
            }

            NovaExpr *call = nova_expr_new(NOVA_EXPR_CALL, loc);
            if (call != NULL) {
                call->as.call.callee = expr;
                call->as.call.args = call_args;
                call->as.call.arg_count = call_count;
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
 * @param P        Parser state
 * @param min_prec Minimum binding power (precedence)
 * @return Expression AST node
 *
 * HOT PATH: Called recursively for every sub-expression
 */
static NovaExpr *novai_parse_expr(NovaParser *P, int min_prec) {
    /* Parse the prefix (nud) -- includes suffixed expressions */
    NovaExpr *left = novai_parse_suffixed_expr(P);

    /* Parse infix operators (led) while they bind tighter */
    while (1) {
        int prec = novai_get_binop_prec(P->current.type);
        if (prec == PREC_NONE || prec < min_prec) {
            break;
        }

        NovaTokenType op_type = P->current.type;
        NovaSourceLoc op_loc = P->current.loc;
        novai_advance(P); /* consume operator */

        /* Right-hand side: parse with same prec for right-assoc,
         * prec + 1 for left-assoc */
        int next_prec = novai_is_right_assoc(op_type) ? prec : prec + 1;
        NovaExpr *right = novai_parse_expr(P, next_prec);

        NovaExpr *binary = nova_expr_new(NOVA_EXPR_BINARY, op_loc);
        if (binary != NULL) {
            binary->as.binary.op = novai_token_to_binop(op_type);
            binary->as.binary.left = left;
            binary->as.binary.right = right;
        }
        left = binary;
    }

    return left;
}

/* ============================================================
 * EXPRESSION LIST PARSING
 *
 * Parses comma-separated expression lists used in local
 * declarations, assignments, return statements, etc.
 * ============================================================ */

/**
 * @brief Parse a comma-separated list of expressions
 *
 * @param P      Parser state
 * @param exprs  Output: expression array
 * @param count  Output: number of expressions
 * @return 0 on success, -1 on error
 */
static int novai_parse_exprlist(NovaParser *P, NovaExpr ***exprs,
                                int *count) {
    int cap = 4;
    int n = 0;

    *exprs = (NovaExpr **)calloc((size_t)cap, sizeof(NovaExpr *));
    if (*exprs == NULL) {
        novai_error(P, "out of memory");
        return -1;
    }

    do {
        if (n >= cap) {
            cap *= 2;
            NovaExpr **ne = (NovaExpr **)realloc(
                *exprs, (size_t)cap * sizeof(NovaExpr *)
            );
            if (ne == NULL) {
                novai_error(P, "out of memory");
                return -1;
            }
            *exprs = ne;
        }
        (*exprs)[n] = novai_parse_expr(P, PREC_NONE);
        n++;
    } while (novai_match(P, (NovaTokenType)','));

    *count = n;
    return 0;
}

/* ============================================================
 * STATEMENT PARSERS (Recursive Descent)
 * ============================================================ */

/**
 * @brief Parse: local name [, name ...] [= expr [, expr ...]]
 *        or:    local function name(...) ... end
 */
static NovaStmt *novai_parse_local(NovaParser *P) {
    NovaSourceLoc loc = P->previous.loc;

    /* dec function name(...) ... end
     * local async function name(...) ... end */
    int local_is_async = 0;
    if (novai_match(P, NOVA_TOKEN_ASYNC)) {
        local_is_async = 1;
        if (!novai_check(P, NOVA_TOKEN_FUNCTION)) {
            novai_error(P,
                "expected 'function' after 'dec async'");
            return NULL;
        }
    }
    if (novai_match(P, NOVA_TOKEN_FUNCTION)) {
        if (!novai_check(P, NOVA_TOKEN_NAME)) {
            novai_error(P, "expected function name after 'dec function'");
            return NULL;
        }

        NovaExpr *name = nova_expr_new(NOVA_EXPR_NAME, P->current.loc);
        if (name != NULL) {
            name->as.string.data = P->current.value.string.data;
            name->as.string.len = P->current.value.string.len;
        }
        novai_advance(P);

        NovaParam *params = NULL;
        int param_count = 0;
        int variadic = 0;
        NovaBlock *body = NULL;

        if (novai_parse_funcbody(P, &params, &param_count,
                                 &variadic, &body) != 0) {
            nova_expr_free(name);
            return NULL;
        }

        NovaStmt *s = nova_stmt_new(NOVA_STMT_LOCAL_FUNCTION, loc);
        if (s != NULL) {
            s->as.func_stmt.name = name;
            s->as.func_stmt.params = params;
            s->as.func_stmt.param_count = param_count;
            s->as.func_stmt.is_variadic = variadic;
            s->as.func_stmt.is_async = local_is_async;
            s->as.func_stmt.body = body;
        }
        return s;
    }

    /* dec name [, name ...] */
    int cap = 4;
    int name_count = 0;
    const char **names = (const char **)calloc((size_t)cap,
                                               sizeof(const char *));
    size_t *name_lens = (size_t *)calloc((size_t)cap, sizeof(size_t));
    if (names == NULL || name_lens == NULL) {
        free(names);
        free(name_lens);
        novai_error(P, "out of memory");
        return NULL;
    }

    do {
        if (!novai_check(P, NOVA_TOKEN_NAME)) {
            novai_error(P, "expected name in dec declaration");
            free(names);
            free(name_lens);
            return NULL;
        }
        if (name_count >= cap) {
            cap *= 2;
            const char **nn = (const char **)realloc(
                names, (size_t)cap * sizeof(const char *)
            );
            if (nn == NULL) {
                free(names);
                free(name_lens);
                return NULL;
            }
            names = nn;
            size_t *nl = (size_t *)realloc(
                name_lens, (size_t)cap * sizeof(size_t)
            );
            if (nl == NULL) {
                free(names);
                free(name_lens);
                return NULL;
            }
            name_lens = nl;
        }
        names[name_count] = P->current.value.string.data;
        name_lens[name_count] = P->current.value.string.len;
        name_count++;
        novai_advance(P);
    } while (novai_match(P, (NovaTokenType)','));

    /* Optional initializers: = expr [, expr ...] */
    NovaExpr **values = NULL;
    int value_count = 0;

    if (novai_match(P, (NovaTokenType)'=')) {
        (void)novai_parse_exprlist(P, &values, &value_count);
    }

    NovaStmt *s = nova_stmt_new(NOVA_STMT_LOCAL, loc);
    if (s != NULL) {
        s->as.local.names = names;
        s->as.local.name_lens = name_lens;
        s->as.local.name_count = name_count;
        s->as.local.values = values;
        s->as.local.value_count = value_count;
    } else {
        free(names);
        free(name_lens);
    }
    return s;
}

/**
 * @brief Parse: if cond then block {elseif cond then block} [else block] end
 */
static NovaStmt *novai_parse_if(NovaParser *P) {
    NovaSourceLoc loc = P->previous.loc;

    int cap = 4;
    int count = 0;
    NovaIfBranch *branches = (NovaIfBranch *)calloc(
        (size_t)cap, sizeof(NovaIfBranch)
    );
    if (branches == NULL) {
        novai_error(P, "out of memory");
        return NULL;
    }

    /* if condition then block */
    branches[0].loc = loc;
    branches[0].condition = novai_parse_expr(P, PREC_NONE);
    (void)novai_expect(P, NOVA_TOKEN_THEN, "after if condition");
    branches[0].body = novai_parse_block(P);
    count = 1;

    /* elseif ... then ... */
    while (novai_match(P, NOVA_TOKEN_ELSEIF)) {
        if (count >= cap) {
            cap *= 2;
            NovaIfBranch *nb = (NovaIfBranch *)realloc(
                branches, (size_t)cap * sizeof(NovaIfBranch)
            );
            if (nb == NULL) {
                novai_error(P, "out of memory");
                break;
            }
            branches = nb;
        }
        memset(&branches[count], 0, sizeof(NovaIfBranch));
        branches[count].loc = P->previous.loc;
        branches[count].condition = novai_parse_expr(P, PREC_NONE);
        (void)novai_expect(P, NOVA_TOKEN_THEN, "after elseif condition");
        branches[count].body = novai_parse_block(P);
        count++;
    }

    /* else ... */
    if (novai_match(P, NOVA_TOKEN_ELSE)) {
        if (count >= cap) {
            cap *= 2;
            NovaIfBranch *nb = (NovaIfBranch *)realloc(
                branches, (size_t)cap * sizeof(NovaIfBranch)
            );
            if (nb == NULL) {
                novai_error(P, "out of memory");
            } else {
                branches = nb;
            }
        }
        if (count < cap) {
            memset(&branches[count], 0, sizeof(NovaIfBranch));
            branches[count].loc = P->previous.loc;
            branches[count].condition = NULL; /* else = no condition */
            branches[count].body = novai_parse_block(P);
            count++;
        }
    }

    (void)novai_expect(P, NOVA_TOKEN_END, "to close if statement");

    NovaStmt *s = nova_stmt_new(NOVA_STMT_IF, loc);
    if (s != NULL) {
        s->as.if_stmt.branches = branches;
        s->as.if_stmt.branch_count = count;
    } else {
        free(branches);
    }
    return s;
}

/**
 * @brief Parse: while condition do block end
 */
static NovaStmt *novai_parse_while(NovaParser *P) {
    NovaSourceLoc loc = P->previous.loc;

    NovaExpr *cond = novai_parse_expr(P, PREC_NONE);
    (void)novai_expect(P, NOVA_TOKEN_DO, "after while condition");
    NovaBlock *body = novai_parse_block(P);
    (void)novai_expect(P, NOVA_TOKEN_END, "to close while loop");

    NovaStmt *s = nova_stmt_new(NOVA_STMT_WHILE, loc);
    if (s != NULL) {
        s->as.while_stmt.condition = cond;
        s->as.while_stmt.body = body;
    }
    return s;
}

/**
 * @brief Parse: repeat block until condition
 */
static NovaStmt *novai_parse_repeat(NovaParser *P) {
    NovaSourceLoc loc = P->previous.loc;

    NovaBlock *body = novai_parse_block(P);
    (void)novai_expect(P, NOVA_TOKEN_UNTIL, "to close repeat loop");
    NovaExpr *cond = novai_parse_expr(P, PREC_NONE);

    NovaStmt *s = nova_stmt_new(NOVA_STMT_REPEAT, loc);
    if (s != NULL) {
        s->as.repeat_stmt.body = body;
        s->as.repeat_stmt.condition = cond;
    }
    return s;
}

/**
 * @brief Parse: for name = start, stop [, step] do block end
 *        or:    for names in exprs do block end
 */
static NovaStmt *novai_parse_for(NovaParser *P) {
    NovaSourceLoc loc = P->previous.loc;

    if (!novai_check(P, NOVA_TOKEN_NAME)) {
        novai_error(P, "expected name after 'for'");
        return NULL;
    }

    /* Collect first name */
    const char *first_name = P->current.value.string.data;
    size_t first_name_len = P->current.value.string.len;
    novai_advance(P);

    /* Numeric for: for i = start, stop [, step] do */
    if (novai_match(P, (NovaTokenType)'=')) {
        NovaExpr *start = novai_parse_expr(P, PREC_NONE);
        (void)novai_expect(P, (NovaTokenType)',', "after for start");
        NovaExpr *stop = novai_parse_expr(P, PREC_NONE);

        NovaExpr *step = NULL;
        if (novai_match(P, (NovaTokenType)',')) {
            step = novai_parse_expr(P, PREC_NONE);
        }

        (void)novai_expect(P, NOVA_TOKEN_DO, "after for clause");
        NovaBlock *body = novai_parse_block(P);
        (void)novai_expect(P, NOVA_TOKEN_END, "to close for loop");

        NovaStmt *s = nova_stmt_new(NOVA_STMT_FOR_NUMERIC, loc);
        if (s != NULL) {
            s->as.for_numeric.name = first_name;
            s->as.for_numeric.name_len = first_name_len;
            s->as.for_numeric.start = start;
            s->as.for_numeric.stop = stop;
            s->as.for_numeric.step = step;
            s->as.for_numeric.body = body;
        }
        return s;
    }

    /* Generic for: for k [, v ...] in exprs do */
    int cap = 4;
    int name_count = 1;
    const char **names = (const char **)calloc((size_t)cap,
                                               sizeof(const char *));
    size_t *name_lens = (size_t *)calloc((size_t)cap, sizeof(size_t));
    if (names == NULL || name_lens == NULL) {
        free(names);
        free(name_lens);
        return NULL;
    }

    names[0] = first_name;
    name_lens[0] = first_name_len;

    while (novai_match(P, (NovaTokenType)',')) {
        if (!novai_check(P, NOVA_TOKEN_NAME)) {
            novai_error(P, "expected name in for loop");
            free(names);
            free(name_lens);
            return NULL;
        }
        if (name_count >= cap) {
            cap *= 2;
            const char **nn = (const char **)realloc(
                names, (size_t)cap * sizeof(const char *)
            );
            if (nn == NULL) {
                free(names);
                free(name_lens);
                return NULL;
            }
            names = nn;
            size_t *nl = (size_t *)realloc(
                name_lens, (size_t)cap * sizeof(size_t)
            );
            if (nl == NULL) {
                free(names);
                free(name_lens);
                return NULL;
            }
            name_lens = nl;
        }
        names[name_count] = P->current.value.string.data;
        name_lens[name_count] = P->current.value.string.len;
        name_count++;
        novai_advance(P);
    }

    (void)novai_expect(P, NOVA_TOKEN_IN, "in generic for loop");

    NovaExpr **iters = NULL;
    int iter_count = 0;
    (void)novai_parse_exprlist(P, &iters, &iter_count);

    (void)novai_expect(P, NOVA_TOKEN_DO, "after for-in clause");
    NovaBlock *body = novai_parse_block(P);
    (void)novai_expect(P, NOVA_TOKEN_END, "to close for loop");

    NovaStmt *s = nova_stmt_new(NOVA_STMT_FOR_GENERIC, loc);
    if (s != NULL) {
        s->as.for_generic.names = names;
        s->as.for_generic.name_lens = name_lens;
        s->as.for_generic.name_count = name_count;
        s->as.for_generic.iterators = iters;
        s->as.for_generic.iter_count = iter_count;
        s->as.for_generic.body = body;
    } else {
        free(names);
        free(name_lens);
    }
    return s;
}

/**
 * @brief Parse: function name[.field][.field][:method](...) ... end
 */
static NovaStmt *novai_parse_function_stat(NovaParser *P) {
    NovaSourceLoc loc = P->previous.loc;

    /* Parse the name: name[.name][:name] */
    if (!novai_check(P, NOVA_TOKEN_NAME)) {
        novai_error(P, "expected function name");
        return NULL;
    }

    NovaExpr *name = nova_expr_new(NOVA_EXPR_NAME, P->current.loc);
    if (name != NULL) {
        name->as.string.data = P->current.value.string.data;
        name->as.string.len = P->current.value.string.len;
    }
    novai_advance(P);

    /* Handle dotted names: a.b.c */
    while (novai_match(P, (NovaTokenType)'.')) {
        if (!novai_check(P, NOVA_TOKEN_NAME)) {
            novai_error(P, "expected name after '.'");
            nova_expr_free(name);
            return NULL;
        }
        NovaExpr *field = nova_expr_new(NOVA_EXPR_FIELD, P->current.loc);
        if (field != NULL) {
            field->as.field.object = name;
            field->as.field.name = P->current.value.string.data;
            field->as.field.name_len = P->current.value.string.len;
        }
        novai_advance(P);
        name = field;
    }

    /* Handle method syntax: a:b */
    if (novai_match(P, (NovaTokenType)':')) {
        if (!novai_check(P, NOVA_TOKEN_NAME)) {
            novai_error(P, "expected method name after ':'");
            nova_expr_free(name);
            return NULL;
        }
        NovaExpr *method = nova_expr_new(NOVA_EXPR_METHOD, P->current.loc);
        if (method != NULL) {
            method->as.field.object = name;
            method->as.field.name = P->current.value.string.data;
            method->as.field.name_len = P->current.value.string.len;
        }
        novai_advance(P);
        name = method;
    }

    NovaParam *params = NULL;
    int param_count = 0;
    int variadic = 0;
    NovaBlock *body = NULL;

    if (novai_parse_funcbody(P, &params, &param_count,
                             &variadic, &body) != 0) {
        nova_expr_free(name);
        return NULL;
    }

    NovaStmt *s = nova_stmt_new(NOVA_STMT_FUNCTION, loc);
    if (s != NULL) {
        s->as.func_stmt.name = name;
        s->as.func_stmt.params = params;
        s->as.func_stmt.param_count = param_count;
        s->as.func_stmt.is_variadic = variadic;
        s->as.func_stmt.body = body;
    }
    return s;
}

/**
 * @brief Parse: do block end
 */
static NovaStmt *novai_parse_do(NovaParser *P) {
    NovaSourceLoc loc = P->previous.loc;

    NovaBlock *body = novai_parse_block(P);
    (void)novai_expect(P, NOVA_TOKEN_END, "to close do block");

    NovaStmt *s = nova_stmt_new(NOVA_STMT_DO, loc);
    if (s != NULL) {
        s->as.do_stmt.body = body;
    }
    return s;
}

/**
 * @brief Parse: return [expr [, expr ...]] [;]
 */
static NovaStmt *novai_parse_return(NovaParser *P) {
    NovaSourceLoc loc = P->previous.loc;

    NovaExpr **values = NULL;
    int value_count = 0;

    /* Check if there are return values */
    if (P->current.type != NOVA_TOKEN_END &&
        P->current.type != NOVA_TOKEN_ELSE &&
        P->current.type != NOVA_TOKEN_ELSEIF &&
        P->current.type != NOVA_TOKEN_UNTIL &&
        P->current.type != NOVA_TOKEN_EOF &&
        P->current.type != (NovaTokenType)')') {
        (void)novai_parse_exprlist(P, &values, &value_count);
    }

    /* Optional trailing semicolon */
    (void)novai_match(P, (NovaTokenType)';');

    NovaStmt *s = nova_stmt_new(NOVA_STMT_RETURN, loc);
    if (s != NULL) {
        s->as.return_stmt.values = values;
        s->as.return_stmt.value_count = value_count;
    }
    return s;
}

/**
 * @brief Parse: goto label_name
 */
static NovaStmt *novai_parse_goto(NovaParser *P) {
    NovaSourceLoc loc = P->previous.loc;

    if (!novai_check(P, NOVA_TOKEN_NAME)) {
        novai_error(P, "expected label name after 'goto'");
        return NULL;
    }

    NovaStmt *s = nova_stmt_new(NOVA_STMT_GOTO, loc);
    if (s != NULL) {
        s->as.goto_stmt.label = P->current.value.string.data;
        s->as.goto_stmt.label_len = P->current.value.string.len;
    }
    novai_advance(P);
    return s;
}

/**
 * @brief Parse: ::label_name::
 */
static NovaStmt *novai_parse_label(NovaParser *P) {
    NovaSourceLoc loc = P->previous.loc;

    if (!novai_check(P, NOVA_TOKEN_NAME)) {
        novai_error(P, "expected label name");
        return NULL;
    }

    NovaStmt *s = nova_stmt_new(NOVA_STMT_LABEL, loc);
    if (s != NULL) {
        s->as.label_stmt.label = P->current.value.string.data;
        s->as.label_stmt.label_len = P->current.value.string.len;
    }
    novai_advance(P);

    (void)novai_expect(P, NOVA_TOKEN_DBCOLON, "to close label");
    return s;
}

/**
 * @brief Parse: import "module" [as alias]
 */
static NovaStmt *novai_parse_import(NovaParser *P) {
    NovaSourceLoc loc = P->previous.loc;

    if (!novai_check(P, NOVA_TOKEN_STRING)) {
        novai_error(P, "expected string after 'import'");
        return NULL;
    }

    NovaStmt *s = nova_stmt_new(NOVA_STMT_IMPORT, loc);
    if (s != NULL) {
        s->as.import_stmt.module = P->current.value.string.data;
        s->as.import_stmt.module_len = P->current.value.string.len;
    }
    novai_advance(P);

    /* Optional: as alias */
    if (novai_check(P, NOVA_TOKEN_NAME) &&
        P->current.value.string.len == 2 &&
        memcmp(P->current.value.string.data, "as", 2) == 0) {
        novai_advance(P); /* consume 'as' */
        if (novai_check(P, NOVA_TOKEN_NAME) && s != NULL) {
            s->as.import_stmt.alias = P->current.value.string.data;
            s->as.import_stmt.alias_len = P->current.value.string.len;
            novai_advance(P);
        } else {
            novai_error(P, "expected alias name after 'as'");
        }
    }

    return s;
}

/**
 * @brief Parse: export name | export function name() ... end
 */
static NovaStmt *novai_parse_export(NovaParser *P) {
    NovaSourceLoc loc = P->previous.loc;

    NovaExpr *value = novai_parse_expr(P, PREC_NONE);

    NovaStmt *s = nova_stmt_new(NOVA_STMT_EXPORT, loc);
    if (s != NULL) {
        s->as.export_stmt.value = value;
    }
    return s;
}

/**
 * @brief Parse: const name = expr
 */
static NovaStmt *novai_parse_const(NovaParser *P) {
    NovaSourceLoc loc = P->previous.loc;

    if (!novai_check(P, NOVA_TOKEN_NAME)) {
        novai_error(P, "expected name after 'const'");
        return NULL;
    }

    const char *name = P->current.value.string.data;
    size_t name_len = P->current.value.string.len;
    novai_advance(P);

    (void)novai_expect(P, (NovaTokenType)'=', "in const declaration");
    NovaExpr *value = novai_parse_expr(P, PREC_NONE);

    NovaStmt *s = nova_stmt_new(NOVA_STMT_CONST, loc);
    if (s != NULL) {
        s->as.const_stmt.name = name;
        s->as.const_stmt.name_len = name_len;
        s->as.const_stmt.value = value;
    }
    return s;
}

/**
 * @brief Parse an expression statement or assignment
 *
 * If we parse an expression and see '=' or ',' after it,
 * it's an assignment: var1 [, var2 ...] = expr1 [, expr2 ...]
 * Otherwise it's an expression statement (must be a call).
 */
static NovaStmt *novai_parse_expr_or_assign(NovaParser *P) {
    NovaSourceLoc loc = P->current.loc;

    NovaExpr *first = novai_parse_suffixed_expr(P);

    /* Assignment: expr [, expr] = expr [, expr] */
    if (novai_check(P, (NovaTokenType)'=') ||
        novai_check(P, (NovaTokenType)',')) {

        int cap = 4;
        int target_count = 1;
        NovaExpr **targets = (NovaExpr **)calloc((size_t)cap,
                                                  sizeof(NovaExpr *));
        if (targets == NULL) {
            nova_expr_free(first);
            return NULL;
        }
        targets[0] = first;

        /* Collect additional targets: var1, var2, var3 */
        while (novai_match(P, (NovaTokenType)',')) {
            if (target_count >= cap) {
                cap *= 2;
                NovaExpr **nt = (NovaExpr **)realloc(
                    targets, (size_t)cap * sizeof(NovaExpr *)
                );
                if (nt == NULL) {
                    free(targets);
                    return NULL;
                }
                targets = nt;
            }
            targets[target_count] = novai_parse_suffixed_expr(P);
            target_count++;
        }

        (void)novai_expect(P, (NovaTokenType)'=', "in assignment");

        NovaExpr **values = NULL;
        int value_count = 0;
        (void)novai_parse_exprlist(P, &values, &value_count);

        NovaStmt *s = nova_stmt_new(NOVA_STMT_ASSIGN, loc);
        if (s != NULL) {
            s->as.assign.targets = targets;
            s->as.assign.target_count = target_count;
            s->as.assign.values = values;
            s->as.assign.value_count = value_count;
        } else {
            free(targets);
        }
        return s;
    }

    /* Expression statement */
    NovaStmt *s = nova_stmt_new(NOVA_STMT_EXPR, loc);
    if (s != NULL) {
        s->as.expr.expr = first;
    }
    return s;
}

/* ============================================================
 * MAIN STATEMENT DISPATCH
 * ============================================================ */

/**
 * @brief Parse a single statement
 *
 * Dispatches to the appropriate statement parser based on the
 * current token. This is the top-level recursive descent entry.
 */
static NovaStmt *novai_parse_stmt(NovaParser *P) {
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

    /* async function name() ... end */
    case NOVA_TOKEN_ASYNC:
        novai_advance(P);
        if (novai_check(P, NOVA_TOKEN_FUNCTION)) {
            novai_advance(P);
            {
                NovaStmt *fs = novai_parse_function_stat(P);
                if (fs != NULL) {
                    fs->as.func_stmt.is_async = 1;
                }
                return fs;
            }
        }
        novai_error(P, "expected 'function' after 'async'");
        return NULL;

    case NOVA_TOKEN_DO:
        novai_advance(P);
        return novai_parse_do(P);

    case NOVA_TOKEN_RETURN:
        novai_advance(P);
        return novai_parse_return(P);

    case NOVA_TOKEN_BREAK:
        novai_advance(P);
        return nova_stmt_new(NOVA_STMT_BREAK, P->previous.loc);

    case NOVA_TOKEN_GOTO:
        novai_advance(P);
        return novai_parse_goto(P);

    case NOVA_TOKEN_DBCOLON:
        novai_advance(P);
        return novai_parse_label(P);

    /* Nova extensions */
    case NOVA_TOKEN_IMPORT:
        novai_advance(P);
        return novai_parse_import(P);

    case NOVA_TOKEN_EXPORT:
        novai_advance(P);
        return novai_parse_export(P);

    case NOVA_TOKEN_CONST:
        novai_advance(P);
        return novai_parse_const(P);

    /* Semicolons as empty statements (Lua compatibility) */
    case (NovaTokenType)';':
        novai_advance(P);
        return novai_parse_stmt(P); /* skip and try next */

    default:
        return novai_parse_expr_or_assign(P);
    }
}

/* ============================================================
 * BLOCK PARSING
 * ============================================================ */

/**
 * @brief Check if current token closes a block
 *
 * Block-closing tokens are: end, else, elseif, until, EOF
 */
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

/**
 * @brief Parse a block of statements
 *
 * Parses statements until a block-closing token is found.
 */
static NovaBlock *novai_parse_block(NovaParser *P) {
    NovaBlock *block = nova_block_new(P->current.loc);
    if (block == NULL) {
        novai_error(P, "out of memory");
        return NULL;
    }

    while (!novai_is_block_end(P)) {
        NovaStmt *s = novai_parse_stmt(P);
        if (s != NULL) {
            nova_block_append(block, s);
        }

        /* If we had a return, stop parsing this block */
        if (s != NULL && s->kind == NOVA_STMT_RETURN) {
            break;
        }
    }

    return block;
}

/* ============================================================
 * PUBLIC API IMPLEMENTATION
 * ============================================================ */

int nova_parser_init(NovaParser *P, NovaPP *pp) {
    if (P == NULL || pp == NULL) {
        return -1;
    }

    memset(P, 0, sizeof(NovaParser));
    P->pp = pp;
    P->had_error = 0;
    P->panic_mode = 0;
    P->error_count = 0;
    P->error_msg[0] = '\0';

    /* Prime the parser with the first token */
    novai_advance(P);

    return 0;
}

void nova_parser_free(NovaParser *P) {
    if (P == NULL) {
        return;
    }
    /* Parser doesn't own pp or any allocated state beyond error_msg */
    P->pp = NULL;
}

NovaAST *nova_parse(NovaParser *P) {
    if (P == NULL) {
        return NULL;
    }

    NovaAST *ast = (NovaAST *)calloc(1, sizeof(NovaAST));
    if (ast == NULL) {
        novai_error(P, "out of memory allocating AST");
        return NULL;
    }

    ast->body = novai_parse_block(P);

    if (!novai_check(P, NOVA_TOKEN_EOF)) {
        novai_error(P, "unexpected token %s at top level",
                    nova_token_name(P->current.type));
    }

    if (P->had_error) {
        /* Still return the partial AST -- caller can check error_count */
    }

    return ast;
}

NovaExpr *nova_parse_expression(NovaParser *P) {
    if (P == NULL) {
        return NULL;
    }
    return novai_parse_expr(P, PREC_NONE);
}

const char *nova_parser_error(const NovaParser *P) {
    if (P == NULL) {
        return "parser is NULL";
    }
    return P->error_msg;
}

int nova_parser_error_count(const NovaParser *P) {
    if (P == NULL) {
        return 0;
    }
    return P->error_count;
}
