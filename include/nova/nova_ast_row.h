/**
 * @file nova_ast_row.h
 * @brief Nova Language - Row-Based (Flat Array Indexed) AST
 *
 * Replaces the pointer-based AST (nova_ast.h) with a cache-friendly,
 * arena-backed, index-addressed design. All nodes live in typed pools
 * and reference each other via 32-bit indices instead of pointers.
 *
 * Architecture:
 *   - Three core pools: expressions, statements, blocks
 *   - Five auxiliary pools: expr-lists, table fields, params, if-branches, names
 *   - All backed by a single zorya_arena.h bump allocator
 *   - O(1) allocation, O(1) total free (just destroy the arena)
 *   - Excellent cache locality for tree walks
 *
 * Index Types:
 *   NovaExprIdx, NovaStmtIdx, NovaBlockIdx   (node references)
 *   NovaExtraIdx, NovaFieldIdx, NovaParamIdx  (aux pool references)
 *   NovaBranchIdx, NovaNameIdx                (aux pool references)
 *   All are uint32_t with sentinel NOVA_IDX_NONE = 0xFFFFFFFF.
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
 *   - nova_lex.h       (NovaSourceLoc, NovaTokenType)
 *   - nova_conf.h      (nova_number_t, nova_int_t)
 *   - nova_ast.h       (NovaExprType, NovaStmtType, NovaBinOp, NovaUnOp, NovaFieldKind)
 *   - zorya/zorya_arena.h (Arena)
 */

#ifndef NOVA_AST_ROW_H
#define NOVA_AST_ROW_H

#include "nova_ast.h"
#include "zorya/zorya_arena.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 * INDEX TYPES
 *
 * All typed as uint32_t. Sentinel NOVA_IDX_NONE replaces NULL.
 * Max ~4 billion nodes per pool (plenty for any source file).
 * ============================================================ */

/** Sentinel index — equivalent of NULL pointer */
#define NOVA_IDX_NONE ((uint32_t)0xFFFFFFFF)

/** Expression node index (into NovaASTTable.exprs) */
typedef uint32_t NovaExprIdx;

/** Statement node index (into NovaASTTable.stmts) */
typedef uint32_t NovaStmtIdx;

/** Block node index (into NovaASTTable.blocks) */
typedef uint32_t NovaBlockIdx;

/** Expression-list index (into NovaASTTable.expr_extra) */
typedef uint32_t NovaExtraIdx;

/** Table field index (into NovaASTTable.fields) */
typedef uint32_t NovaFieldIdx;

/** Parameter index (into NovaASTTable.params) */
typedef uint32_t NovaParamIdx;

/** If-branch index (into NovaASTTable.branches) */
typedef uint32_t NovaBranchIdx;

/** Name-ref index (into NovaASTTable.names) */
typedef uint32_t NovaNameIdx;

/* ============================================================
 * FORWARD DECLARATIONS
 * ============================================================ */

typedef struct NovaRowExpr     NovaRowExpr;
typedef struct NovaRowStmt     NovaRowStmt;
typedef struct NovaRowBlock    NovaRowBlock;
typedef struct NovaASTTable    NovaASTTable;

/* ============================================================
 * AUXILIARY ROW TYPES
 *
 * These are stored in their own typed pools and referenced
 * by index + count from the main expr/stmt nodes.
 * ============================================================ */

/**
 * @brief Table field (row version)
 *
 * Stored contiguously in NovaASTTable.fields[].
 * A table constructor references field_start + field_count.
 */
typedef struct {
    NovaFieldKind kind;         /**< LIST, RECORD, or BRACKET     */
    NovaExprIdx   key;          /**< NOVA_IDX_NONE for list fields */
    NovaExprIdx   value;        /**< Field value expression        */
    NovaSourceLoc loc;
} NovaRowTableField;

/**
 * @brief Function parameter (row version)
 *
 * Stored contiguously in NovaASTTable.params[].
 */
typedef struct {
    const char   *name;         /**< Parameter name (into source)  */
    size_t        name_len;     /**< Name length                   */
    NovaSourceLoc loc;
} NovaRowParam;

/**
 * @brief If/elseif/else branch (row version)
 *
 * Stored contiguously in NovaASTTable.branches[].
 */
typedef struct {
    NovaExprIdx   condition;    /**< NOVA_IDX_NONE for else branch */
    NovaBlockIdx  body;         /**< Block index for branch body   */
    NovaSourceLoc loc;
} NovaRowIfBranch;

/**
 * @brief Name reference (for local/for-generic name lists)
 *
 * Stored contiguously in NovaASTTable.names[].
 */
typedef struct {
    const char *data;           /**< Name string (into source)     */
    size_t      len;            /**< Name length                   */
} NovaRowNameRef;

/* ============================================================
 * EXPRESSION NODE (ROW VERSION)
 *
 * Tagged union identical in spirit to NovaExpr, but all child
 * references are indices. Variable-length data (args, parts)
 * uses extra-pool start + count.
 * ============================================================ */

struct NovaRowExpr {
    NovaExprType   kind;        /**< Expression type tag           */
    NovaSourceLoc  loc;         /**< Source location               */

    union {
        /* NOVA_EXPR_INTEGER */
        struct {
            nova_int_t value;
        } integer;

        /* NOVA_EXPR_NUMBER */
        struct {
            nova_number_t value;
        } number;

        /* NOVA_EXPR_STRING, NOVA_EXPR_NAME */
        struct {
            const char *data;       /**< String data (not owned)   */
            size_t      len;        /**< String length             */
        } string;

        /* NOVA_EXPR_UNARY */
        struct {
            NovaUnOp    op;
            NovaExprIdx operand;
        } unary;

        /* NOVA_EXPR_BINARY */
        struct {
            NovaBinOp   op;
            NovaExprIdx left;
            NovaExprIdx right;
        } binary;

        /* NOVA_EXPR_CALL, NOVA_EXPR_METHOD_CALL */
        struct {
            NovaExprIdx  callee;        /**< Function expression   */
            NovaExtraIdx args_start;    /**< Start in expr_extra[] */
            int          arg_count;     /**< Number of arguments   */
        } call;

        /* NOVA_EXPR_INDEX: a[b] */
        struct {
            NovaExprIdx object;
            NovaExprIdx index;
        } index;

        /* NOVA_EXPR_FIELD: a.b  /  NOVA_EXPR_METHOD: a:b */
        struct {
            NovaExprIdx object;
            const char *name;
            size_t      name_len;
        } field;

        /* NOVA_EXPR_TABLE */
        struct {
            NovaFieldIdx field_start;   /**< Start in fields[]     */
            int          field_count;
        } table;

        /* NOVA_EXPR_FUNCTION (anonymous function literal) */
        struct {
            NovaParamIdx param_start;   /**< Start in params[]     */
            int          param_count;
            int          is_variadic;
            int          is_async;
            NovaBlockIdx body;
        } function;

        /* NOVA_EXPR_AWAIT / NOVA_EXPR_SPAWN / NOVA_EXPR_YIELD */
        struct {
            NovaExprIdx operand;
        } async_op;

        /* NOVA_EXPR_INTERP_STRING */
        struct {
            NovaExtraIdx parts_start;   /**< Start in expr_extra[] */
            int          part_count;
        } interp;

    } as; /**< Union payload — access via nova_get_expr(t, i)->as.xxx */
};

/* ============================================================
 * STATEMENT NODE (ROW VERSION)
 *
 * Tagged union identical in spirit to NovaStmt. The `next` field
 * forms an index-based linked list within blocks.
 * ============================================================ */

struct NovaRowStmt {
    NovaStmtType   kind;        /**< Statement type tag            */
    NovaSourceLoc  loc;         /**< Source location               */
    NovaStmtIdx    next;        /**< Next stmt in block (linked list) */

    union {
        /* NOVA_STMT_EXPR */
        struct {
            NovaExprIdx expr;
        } expr;

        /* NOVA_STMT_LOCAL */
        struct {
            NovaNameIdx  names_start;   /**< Start in names[]      */
            int          name_count;
            NovaExtraIdx values_start;  /**< Start in expr_extra[] */
            int          value_count;
        } local;

        /* NOVA_STMT_ASSIGN */
        struct {
            NovaExtraIdx targets_start; /**< Start in expr_extra[] */
            int          target_count;
            NovaExtraIdx values_start;  /**< Start in expr_extra[] */
            int          value_count;
        } assign;

        /* NOVA_STMT_IF */
        struct {
            NovaBranchIdx branch_start; /**< Start in branches[]   */
            int           branch_count;
        } if_stmt;

        /* NOVA_STMT_WHILE */
        struct {
            NovaExprIdx  condition;
            NovaBlockIdx body;
        } while_stmt;

        /* NOVA_STMT_REPEAT */
        struct {
            NovaBlockIdx body;
            NovaExprIdx  condition;
        } repeat_stmt;

        /* NOVA_STMT_FOR_NUMERIC */
        struct {
            const char  *name;
            size_t       name_len;
            NovaExprIdx  start;
            NovaExprIdx  stop;
            NovaExprIdx  step;      /**< NOVA_IDX_NONE if omitted */
            NovaBlockIdx body;
        } for_numeric;

        /* NOVA_STMT_FOR_GENERIC */
        struct {
            NovaNameIdx  names_start;
            int          name_count;
            NovaExtraIdx iters_start;   /**< Start in expr_extra[] */
            int          iter_count;
            NovaBlockIdx body;
        } for_generic;

        /* NOVA_STMT_DO */
        struct {
            NovaBlockIdx body;
        } do_stmt;

        /* NOVA_STMT_RETURN */
        struct {
            NovaExtraIdx values_start;  /**< Start in expr_extra[] */
            int          value_count;
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
            NovaExprIdx  name;          /**< Name expression       */
            NovaParamIdx param_start;
            int          param_count;
            int          is_variadic;
            int          is_async;
            NovaBlockIdx body;
        } func_stmt;

        /* NOVA_STMT_IMPORT */
        struct {
            const char *module;
            size_t      module_len;
            const char *alias;          /**< NULL if no alias      */
            size_t      alias_len;
        } import_stmt;

        /* NOVA_STMT_EXPORT */
        struct {
            NovaExprIdx value;
        } export_stmt;

        /* NOVA_STMT_CONST */
        struct {
            const char *name;
            size_t      name_len;
            NovaExprIdx value;
        } const_stmt;

        /* NOVA_STMT_ENUM */
        struct {
            const char  *name;          /**< Enum type name            */
            size_t       name_len;
            NovaNameIdx  members_start; /**< Start in names[]          */
            int          member_count;
            NovaExtraIdx values_start;  /**< Start in expr_extra[]     */
            int          has_values;    /**< 1 if any explicit = val   */
            int          typed;         /**< 1 if typedec enum { }     */
        } enum_stmt;

        /* NOVA_STMT_STRUCT */
        struct {
            const char  *name;          /**< Struct type name          */
            size_t       name_len;
            NovaNameIdx  fields_start;  /**< Field names in names[]    */
            int          field_count;
            NovaExtraIdx defaults_start;/**< Defaults in expr_extra[]  */
            int          typed;         /**< 1 if typedec struct { }   */
        } struct_stmt;

        /* NOVA_STMT_TYPEDEC */
        struct {
            const char  *name;          /**< Type alias name           */
            size_t       name_len;
            const char  *base_type;     /**< Base type name            */
            size_t       base_type_len;
        } typedec_stmt;

        /* NOVA_STMT_BREAK: no extra data */

    } as; /**< Union payload */
};

/* ============================================================
 * BLOCK NODE (ROW VERSION)
 * ============================================================ */

/**
 * @brief Block of statements (row version)
 *
 * First/last form a linked list via NovaRowStmt.next indices.
 */
struct NovaRowBlock {
    NovaStmtIdx   first;        /**< Head of stmt linked list      */
    NovaStmtIdx   last;         /**< Tail for O(1) append          */
    int           stmt_count;   /**< Number of statements          */
    NovaSourceLoc loc;          /**< Location of block start       */
};

/* ============================================================
 * POOL GROWTH DEFAULTS
 * ============================================================ */

#define NOVA_POOL_INIT_EXPRS       256
#define NOVA_POOL_INIT_STMTS       128
#define NOVA_POOL_INIT_BLOCKS       64
#define NOVA_POOL_INIT_EXTRA       512
#define NOVA_POOL_INIT_FIELDS      128
#define NOVA_POOL_INIT_PARAMS       64
#define NOVA_POOL_INIT_BRANCHES     32
#define NOVA_POOL_INIT_NAMES        64

/* ============================================================
 * AST TABLE (top-level container)
 *
 * Holds all pools + arena. One per parsed file/chunk.
 * ============================================================ */

struct NovaASTTable {
    /* ---- Core node pools ---- */
    NovaRowExpr      *exprs;
    uint32_t          expr_count;
    uint32_t          expr_cap;

    NovaRowStmt      *stmts;
    uint32_t          stmt_count;
    uint32_t          stmt_cap;

    NovaRowBlock     *blocks;
    uint32_t          block_count;
    uint32_t          block_cap;

    /* ---- Auxiliary pools ---- */

    /** Flat array of NovaExprIdx for variable-length expr lists
     *  (call args, return values, interp parts, etc.)            */
    NovaExprIdx      *expr_extra;
    uint32_t          extra_count;
    uint32_t          extra_cap;

    /** Table constructor fields */
    NovaRowTableField *fields;
    uint32_t           field_count;
    uint32_t           field_cap;

    /** Function parameters */
    NovaRowParam     *params;
    uint32_t          param_count;
    uint32_t          param_cap;

    /** If/elseif/else branches */
    NovaRowIfBranch  *branches;
    uint32_t          branch_count;
    uint32_t          branch_cap;

    /** Name references (local names, for-generic names) */
    NovaRowNameRef   *names;
    uint32_t          name_count;
    uint32_t          name_cap;

    /* ---- Backing memory ---- */
    Arena            *arena;         /**< All pools allocated here  */

    /* ---- Root node ---- */
    NovaBlockIdx      root;          /**< Top-level block           */
    const char       *source_name;   /**< Source filename (not owned) */
};

/* ============================================================
 * API FUNCTION DECLARATIONS
 *
 * Implementations will go in nova_ast_row.c
 * ============================================================ */

/**
 * @brief Create a new AST table with arena backing
 *
 * @param source_name  Source filename (not owned, must outlive table)
 * @return Initialized table, or table with arena==NULL on failure
 *
 * @post All pools are allocated with initial capacity
 */
NovaASTTable nova_ast_table_create(const char *source_name);

/**
 * @brief Destroy an AST table and free all memory
 *
 * O(1) — just destroys the backing arena.
 *
 * @param table  Table to destroy (NULL arena is safe)
 */
void nova_ast_table_destroy(NovaASTTable *table);

/**
 * @brief Add an expression node to the table
 *
 * @param table  AST table (must not be NULL)
 * @param kind   Expression type
 * @param loc    Source location
 * @return Index of the new expression, or NOVA_IDX_NONE on failure
 */
NovaExprIdx nova_ast_add_expr(NovaASTTable *table,
                              NovaExprType kind,
                              NovaSourceLoc loc);

/**
 * @brief Add a statement node to the table
 *
 * @param table  AST table (must not be NULL)
 * @param kind   Statement type
 * @param loc    Source location
 * @return Index of the new statement, or NOVA_IDX_NONE on failure
 */
NovaStmtIdx nova_ast_add_stmt(NovaASTTable *table,
                              NovaStmtType kind,
                              NovaSourceLoc loc);

/**
 * @brief Add a block node to the table
 *
 * @param table  AST table (must not be NULL)
 * @param loc    Source location
 * @return Index of the new block, or NOVA_IDX_NONE on failure
 */
NovaBlockIdx nova_ast_add_block(NovaASTTable *table,
                                NovaSourceLoc loc);

/**
 * @brief Append a statement to a block (by index)
 *
 * @param table  AST table
 * @param block  Block index
 * @param stmt   Statement index to append
 */
void nova_ast_block_append(NovaASTTable *table,
                           NovaBlockIdx block,
                           NovaStmtIdx stmt);

/**
 * @brief Reserve a contiguous range of expression indices in the extra pool
 *
 * Used for call arguments, return values, interpolation parts, etc.
 *
 * @param table  AST table
 * @param count  Number of slots to reserve
 * @return Start index in expr_extra[], or NOVA_IDX_NONE on failure
 */
NovaExtraIdx nova_ast_add_expr_list(NovaASTTable *table, int count);

/**
 * @brief Reserve a contiguous range of table field slots
 *
 * @param table  AST table
 * @param count  Number of fields to reserve
 * @return Start index in fields[], or NOVA_IDX_NONE on failure
 */
NovaFieldIdx nova_ast_add_fields(NovaASTTable *table, int count);

/**
 * @brief Reserve a contiguous range of parameter slots
 *
 * @param table  AST table
 * @param count  Number of params to reserve
 * @return Start index in params[], or NOVA_IDX_NONE on failure
 */
NovaParamIdx nova_ast_add_params(NovaASTTable *table, int count);

/**
 * @brief Reserve a contiguous range of if-branch slots
 *
 * @param table  AST table
 * @param count  Number of branches to reserve
 * @return Start index in branches[], or NOVA_IDX_NONE on failure
 */
NovaBranchIdx nova_ast_add_branches(NovaASTTable *table, int count);

/**
 * @brief Reserve a contiguous range of name-ref slots
 *
 * @param table  AST table
 * @param count  Number of names to reserve
 * @return Start index in names[], or NOVA_IDX_NONE on failure
 */
NovaNameIdx nova_ast_add_names(NovaASTTable *table, int count);

/* ============================================================
 * INLINE ACCESS HELPERS
 * ============================================================ */

/** Get expression by index (no bounds check — trust the indices) */
static inline NovaRowExpr *nova_get_expr(NovaASTTable *table,
                                         NovaExprIdx idx) {
    return &table->exprs[idx];
}

/** Get statement by index */
static inline NovaRowStmt *nova_get_stmt(NovaASTTable *table,
                                         NovaStmtIdx idx) {
    return &table->stmts[idx];
}

/** Get block by index */
static inline NovaRowBlock *nova_get_block(NovaASTTable *table,
                                           NovaBlockIdx idx) {
    return &table->blocks[idx];
}

/** Get expression index from extra pool */
static inline NovaExprIdx nova_get_extra_expr(NovaASTTable *table,
                                              NovaExtraIdx start,
                                              int offset) {
    return table->expr_extra[start + (uint32_t)offset];
}

/** Set expression index in extra pool */
static inline void nova_set_extra_expr(NovaASTTable *table,
                                       NovaExtraIdx start,
                                       int offset,
                                       NovaExprIdx value) {
    table->expr_extra[start + (uint32_t)offset] = value;
}

#ifdef __cplusplus
}
#endif

#endif /* NOVA_AST_ROW_H */
