/**
 * @file nova_ast.h
 * @brief Nova Language - Abstract Syntax Tree Node Definitions
 *
 * Defines all AST node types for the Nova language. The parser
 * produces a tree of these nodes which is then passed to the
 * optimizer and code generator.
 *
 * Node hierarchy:
 *   NovaASTNode (tagged union)
 *     - NovaExpr (expressions: literals, binary, unary, call, index, ...)
 *     - NovaStmt (statements: local, assign, if, while, for, return, ...)
 *     - NovaBlock (sequence of statements)
 *
 * Memory management:
 *   All AST nodes are allocated via malloc and freed by
 *   nova_ast_free(). String data in nodes points back into the
 *   lexer's source buffer (not owned by the AST).
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
 *   - nova_lex.h (NovaSourceLoc, NovaTokenType)
 *   - nova_conf.h (nova_number_t, nova_int_t)
 */

#ifndef NOVA_AST_H
#define NOVA_AST_H

#include "nova_lex.h"
#include "nova_conf.h"

#include <stddef.h>

/* ============================================================
 * FORWARD DECLARATIONS
 * ============================================================ */

typedef struct NovaExpr   NovaExpr;
typedef struct NovaStmt   NovaStmt;
typedef struct NovaBlock  NovaBlock;

/* ============================================================
 * EXPRESSION NODE TYPES
 * ============================================================ */

typedef enum {
    /* Literals */
    NOVA_EXPR_NIL = 0,
    NOVA_EXPR_TRUE,
    NOVA_EXPR_FALSE,
    NOVA_EXPR_INTEGER,          /**< Integer literal              */
    NOVA_EXPR_NUMBER,           /**< Float literal                */
    NOVA_EXPR_STRING,           /**< String literal               */
    NOVA_EXPR_VARARG,           /**< ... (vararg expression)      */

    /* Variables and access */
    NOVA_EXPR_NAME,             /**< Identifier (variable name)   */
    NOVA_EXPR_INDEX,            /**< a[b]  (bracket index)        */
    NOVA_EXPR_FIELD,            /**< a.b   (dot field access)     */
    NOVA_EXPR_METHOD,           /**< a:b() (method call syntax)   */

    /* Operators */
    NOVA_EXPR_UNARY,            /**< -x, not x, #x, ~x           */
    NOVA_EXPR_BINARY,           /**< a + b, a .. b, a and b, ...  */

    /* Calls */
    NOVA_EXPR_CALL,             /**< f(args)                      */
    NOVA_EXPR_METHOD_CALL,      /**< obj:method(args)             */

    /* Constructors */
    NOVA_EXPR_TABLE,            /**< { fields }                   */
    NOVA_EXPR_FUNCTION,         /**< function(...) body end        */

    /* Interpolated string */
    NOVA_EXPR_INTERP_STRING,    /**< `hello ${name}`              */

    /* Async/coroutine expressions */
    NOVA_EXPR_AWAIT,            /**< await expr                   */
    NOVA_EXPR_SPAWN,            /**< spawn expr (create task)     */
    NOVA_EXPR_YIELD,            /**< yield [expr] (coroutine)     */

    NOVA_EXPR_COUNT
} NovaExprType;

/* ============================================================
 * BINARY / UNARY OPERATOR KINDS
 * ============================================================ */

typedef enum {
    /* Arithmetic */
    NOVA_BINOP_ADD = 0,         /**< +                            */
    NOVA_BINOP_SUB,             /**< -                            */
    NOVA_BINOP_MUL,             /**< *                            */
    NOVA_BINOP_DIV,             /**< /                            */
    NOVA_BINOP_IDIV,            /**< //                           */
    NOVA_BINOP_MOD,             /**< %                            */
    NOVA_BINOP_POW,             /**< ^                            */

    /* String */
    NOVA_BINOP_CONCAT,          /**< ..                           */

    /* Comparison */
    NOVA_BINOP_EQ,              /**< ==                           */
    NOVA_BINOP_NEQ,             /**< ~=  or  !=                   */
    NOVA_BINOP_LT,              /**< <                            */
    NOVA_BINOP_LE,              /**< <=                           */
    NOVA_BINOP_GT,              /**< >                            */
    NOVA_BINOP_GE,              /**< >=                           */

    /* Logical */
    NOVA_BINOP_AND,             /**< and                          */
    NOVA_BINOP_OR,              /**< or                           */

    /* Bitwise */
    NOVA_BINOP_BAND,            /**< &                            */
    NOVA_BINOP_BOR,             /**< |                            */
    NOVA_BINOP_BXOR,            /**< ~                            */
    NOVA_BINOP_SHL,             /**< <<                           */
    NOVA_BINOP_SHR,             /**< >>                           */

    NOVA_BINOP_COUNT
} NovaBinOp;

typedef enum {
    NOVA_UNOP_NEGATE = 0,       /**< -x                           */
    NOVA_UNOP_NOT,              /**< not x                        */
    NOVA_UNOP_LEN,              /**< #x                           */
    NOVA_UNOP_BNOT,             /**< ~x                           */

    NOVA_UNOP_COUNT
} NovaUnOp;

/* ============================================================
 * TABLE FIELD KINDS (for table constructors)
 * ============================================================ */

typedef enum {
    NOVA_FIELD_LIST = 0,        /**< expr (positional: {1, 2, 3}) */
    NOVA_FIELD_RECORD,          /**< name = expr                  */
    NOVA_FIELD_BRACKET,         /**< [expr] = expr                */
} NovaFieldKind;

typedef struct {
    NovaFieldKind kind;
    NovaExpr     *key;          /**< NULL for list fields         */
    NovaExpr     *value;        /**< Field value expression       */
    NovaSourceLoc loc;
} NovaTableField;

/* ============================================================
 * FUNCTION PARAMETER
 * ============================================================ */

typedef struct {
    const char *name;           /**< Parameter name (into source) */
    size_t      name_len;       /**< Name length                  */
    NovaSourceLoc loc;
} NovaParam;

/* ============================================================
 * EXPRESSION NODE
 * ============================================================ */

/**
 * @brief Expression AST node
 *
 * Tagged union: check 'kind' then access the appropriate
 * member of the union.
 */
struct NovaExpr {
    NovaExprType   kind;        /**< Expression type tag          */
    NovaSourceLoc  loc;         /**< Source location              */

    union {
        /* NOVA_EXPR_INTEGER */
        nova_int_t integer;

        /* NOVA_EXPR_NUMBER */
        nova_number_t number;

        /* NOVA_EXPR_STRING, NOVA_EXPR_NAME */
        struct {
            const char *data;   /**< String data (not owned)      */
            size_t      len;    /**< String length                */
        } string;

        /* NOVA_EXPR_UNARY */
        struct {
            NovaUnOp    op;
            NovaExpr   *operand;
        } unary;

        /* NOVA_EXPR_BINARY */
        struct {
            NovaBinOp   op;
            NovaExpr   *left;
            NovaExpr   *right;
        } binary;

        /* NOVA_EXPR_CALL, NOVA_EXPR_METHOD_CALL */
        struct {
            NovaExpr   *callee;     /**< Function expression      */
            NovaExpr  **args;       /**< Argument array           */
            int         arg_count;  /**< Number of arguments      */
        } call;

        /* NOVA_EXPR_INDEX: a[b] */
        struct {
            NovaExpr   *object;     /**< Table/array being indexed*/
            NovaExpr   *index;      /**< Index expression         */
        } index;

        /* NOVA_EXPR_FIELD: a.b  /  NOVA_EXPR_METHOD: a:b */
        struct {
            NovaExpr   *object;     /**< Object being accessed    */
            const char *name;       /**< Field/method name        */
            size_t      name_len;
        } field;

        /* NOVA_EXPR_TABLE */
        struct {
            NovaTableField *fields;
            int             field_count;
        } table;

        /* NOVA_EXPR_FUNCTION (anonymous function literal) */
        struct {
            NovaParam  *params;     /**< Parameter list           */
            int         param_count;
            int         is_variadic;/**< Has ... parameter?       */
            int         is_async;   /**< async function?          */
            NovaBlock  *body;       /**< Function body            */
        } function;

        /* NOVA_EXPR_AWAIT / NOVA_EXPR_SPAWN / NOVA_EXPR_YIELD */
        struct {
            NovaExpr   *operand;    /**< Expression being awaited/spawned/yielded */
        } async_op;

        /* NOVA_EXPR_INTERP_STRING */
        struct {
            NovaExpr  **parts;      /**< Alternating string/expr  */
            int         part_count;
        } interp;

    } as; /**< Union payload - access via expr->as.xxx */
};

/* ============================================================
 * STATEMENT NODE TYPES
 * ============================================================ */

typedef enum {
    /* Expressions as statements */
    NOVA_STMT_EXPR = 0,         /**< Expression statement (call)  */

    /* Variable operations */
    NOVA_STMT_LOCAL,            /**< local name [= expr]          */
    NOVA_STMT_ASSIGN,           /**< var = expr                   */

    /* Control flow */
    NOVA_STMT_IF,               /**< if ... then ... end          */
    NOVA_STMT_WHILE,            /**< while ... do ... end         */
    NOVA_STMT_REPEAT,           /**< repeat ... until ...         */
    NOVA_STMT_FOR_NUMERIC,      /**< for i = a, b [, c] do       */
    NOVA_STMT_FOR_GENERIC,      /**< for k, v in iter do          */
    NOVA_STMT_DO,               /**< do ... end                   */
    NOVA_STMT_BREAK,            /**< break                        */
    NOVA_STMT_CONTINUE,         /**< continue                     */
    NOVA_STMT_GOTO,             /**< goto label                   */
    NOVA_STMT_LABEL,            /**< ::label::                    */
    NOVA_STMT_RETURN,           /**< return [exprs]               */

    /* Functions */
    NOVA_STMT_FUNCTION,         /**< function name() ... end      */
    NOVA_STMT_LOCAL_FUNCTION,   /**< local function name() ... end*/

    /* Nova extensions */
    NOVA_STMT_IMPORT,           /**< import "module"              */
    NOVA_STMT_EXPORT,           /**< export name                  */
    NOVA_STMT_CONST,            /**< const name = expr            */
    NOVA_STMT_ENUM,             /**< enum Name ... end            */
    NOVA_STMT_STRUCT,           /**< struct Name ... end          */
    NOVA_STMT_TYPEDEC,          /**< typedec Name = type          */

    NOVA_STMT_COUNT
} NovaStmtType;

/* ============================================================
 * IF-BRANCH (for if/elseif/else chains)
 * ============================================================ */

typedef struct {
    NovaExpr  *condition;       /**< NULL for else branch         */
    NovaBlock *body;
    NovaSourceLoc loc;
} NovaIfBranch;

/* ============================================================
 * STATEMENT NODE
 * ============================================================ */

/**
 * @brief Statement AST node
 */
struct NovaStmt {
    NovaStmtType   kind;        /**< Statement type tag           */
    NovaSourceLoc  loc;         /**< Source location              */
    NovaStmt      *next;        /**< Linked list in block         */

    union {
        /* NOVA_STMT_EXPR */
        struct {
            NovaExpr *expr;
        } expr;

        /* NOVA_STMT_LOCAL */
        struct {
            const char **names;     /**< Name list (not owned)    */
            size_t      *name_lens;
            int          name_count;
            NovaExpr   **values;    /**< Initializers (can be NULL)*/
            int          value_count;
        } local;

        /* NOVA_STMT_ASSIGN */
        struct {
            NovaExpr  **targets;    /**< LHS expressions          */
            int         target_count;
            NovaExpr  **values;     /**< RHS expressions          */
            int         value_count;
        } assign;

        /* NOVA_STMT_IF */
        struct {
            NovaIfBranch *branches;
            int           branch_count;
        } if_stmt;

        /* NOVA_STMT_WHILE */
        struct {
            NovaExpr  *condition;
            NovaBlock *body;
        } while_stmt;

        /* NOVA_STMT_REPEAT */
        struct {
            NovaBlock *body;
            NovaExpr  *condition;   /**< until condition          */
        } repeat_stmt;

        /* NOVA_STMT_FOR_NUMERIC: for i = start, stop [, step] do */
        struct {
            const char *name;
            size_t      name_len;
            NovaExpr   *start;
            NovaExpr   *stop;
            NovaExpr   *step;       /**< NULL if omitted          */
            NovaBlock  *body;
        } for_numeric;

        /* NOVA_STMT_FOR_GENERIC: for k, v in iterator do */
        struct {
            const char **names;
            size_t      *name_lens;
            int          name_count;
            NovaExpr   **iterators; /**< iter expressions         */
            int          iter_count;
            NovaBlock   *body;
        } for_generic;

        /* NOVA_STMT_DO */
        struct {
            NovaBlock *body;
        } do_stmt;

        /* NOVA_STMT_RETURN */
        struct {
            NovaExpr  **values;
            int         value_count;
        } return_stmt;

        /* NOVA_STMT_GOTO */
        struct {
            const char *label;
            size_t      label_len;
        } goto_stmt;

        /* NOVA_STMT_LABEL */
        struct {
            const char *label;
            size_t      label_len;
        } label_stmt;

        /* NOVA_STMT_FUNCTION / NOVA_STMT_LOCAL_FUNCTION */
        struct {
            NovaExpr   *name;       /**< Name expression (can have dots/colons) */
            NovaParam  *params;
            int         param_count;
            int         is_variadic;
            int         is_async;   /**< async function?          */
            NovaBlock  *body;
        } func_stmt;

        /* NOVA_STMT_IMPORT */
        struct {
            const char *module;     /**< Module path string       */
            size_t      module_len;
            const char *alias;      /**< AS name (NULL if none)   */
            size_t      alias_len;
        } import_stmt;

        /* NOVA_STMT_EXPORT */
        struct {
            NovaExpr *value;        /**< Exported expression/name */
        } export_stmt;

        /* NOVA_STMT_CONST */
        struct {
            const char *name;
            size_t      name_len;
            NovaExpr   *value;
        } const_stmt;

        /* NOVA_STMT_BREAK: no extra data needed */

    } as; /**< Union payload */
};

/* ============================================================
 * BLOCK (sequence of statements)
 * ============================================================ */

/**
 * @brief Block of statements (scope for locals)
 *
 * Blocks form the body of functions, if/while/for/do constructs.
 */
struct NovaBlock {
    NovaStmt     *first;        /**< Head of statement linked list */
    NovaStmt     *last;         /**< Tail for O(1) append          */
    int           stmt_count;   /**< Number of statements          */
    NovaSourceLoc loc;          /**< Location of block start       */
};

/* ============================================================
 * AST TOP-LEVEL (a parsed file / chunk)
 * ============================================================ */

/**
 * @brief Top-level AST for a parsed Nova source
 */
typedef struct {
    NovaBlock    *body;         /**< Top-level block               */
    const char   *source_name; /**< Source filename (not owned)    */
} NovaAST;

/* ============================================================
 * AST MEMORY MANAGEMENT
 * ============================================================ */

/**
 * @brief Free an expression tree recursively
 *
 * @param expr Expression to free (NULL is safe)
 */
void nova_expr_free(NovaExpr *expr);

/**
 * @brief Free a statement and its children
 *
 * @param stmt Statement to free (NULL is safe)
 */
void nova_stmt_free(NovaStmt *stmt);

/**
 * @brief Free a block and all contained statements
 *
 * @param block Block to free (NULL is safe)
 */
void nova_block_free(NovaBlock *block);

/**
 * @brief Free an entire AST
 *
 * @param ast AST to free (NULL is safe)
 */
void nova_ast_free(NovaAST *ast);

/* ============================================================
 * AST CONSTRUCTION HELPERS
 * ============================================================ */

/** Allocate and zero-initialize an expression node */
NovaExpr  *nova_expr_new(NovaExprType kind, NovaSourceLoc loc);

/** Allocate and zero-initialize a statement node */
NovaStmt  *nova_stmt_new(NovaStmtType kind, NovaSourceLoc loc);

/** Allocate an empty block */
NovaBlock *nova_block_new(NovaSourceLoc loc);

/** Append a statement to a block */
void nova_block_append(NovaBlock *block, NovaStmt *stmt);

#endif /* NOVA_AST_H */
