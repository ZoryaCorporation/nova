/**
 * @file nova_ast_row.c
 * @brief Nova Language - Row-Based AST Implementation
 *
 * Arena-backed pool allocation for the flat-array indexed AST.
 * All pools grow via the backing arena — no individual frees,
 * everything is released when the arena is destroyed.
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
 *   - nova_ast_row.h
 *   - zorya/zorya_arena.h
 *
 * THREAD SAFETY:
 *   Not thread-safe. One NovaASTTable per thread.
 */

#include "nova/nova_ast_row.h"
#include <string.h>

/* ============================================================
 * INTERNAL: POOL GROWTH HELPER
 *
 * When a pool is full, allocate a new (2x) array from the arena
 * and memcpy the old data. The old array is "leaked" in the arena
 * (it will be freed when the arena is destroyed). This is the
 * standard arena growth pattern — simple and cache-friendly.
 * ============================================================ */

/**
 * @brief Grow a pool array to at least new_cap elements
 *
 * @param arena    Backing arena
 * @param old_data Pointer to current array (may be NULL)
 * @param old_cap  Current capacity (element count)
 * @param new_cap  Desired capacity (element count)
 * @param elem_sz  Size of one element in bytes
 * @return New array pointer, or NULL on failure
 *
 * @pre arena != NULL
 * @pre new_cap > old_cap
 */
static void *novai_pool_grow(Arena *arena,
                             void *old_data,
                             uint32_t old_cap,
                             uint32_t new_cap,
                             size_t elem_sz) {
    if (arena == NULL) {
        return NULL;
    }

    void *new_data = arena_alloc(arena, (size_t)new_cap * elem_sz);
    if (new_data == NULL) {
        return NULL;
    }

    /* Zero-initialize the entire new array */
    memset(new_data, 0, (size_t)new_cap * elem_sz);

    /* Copy old data if present */
    if (old_data != NULL && old_cap > 0) {
        memcpy(new_data, old_data, (size_t)old_cap * elem_sz);
    }

    return new_data;
}

/**
 * @brief Ensure a pool has room for at least one more element
 *
 * Doubles capacity if full. Returns 0 on success, -1 on failure.
 *
 * @param arena    Backing arena
 * @param data_ptr Pointer to the pool array pointer (in/out)
 * @param count    Current element count
 * @param cap_ptr  Pointer to capacity (in/out)
 * @param elem_sz  Size of one element in bytes
 * @return 0 on success, -1 on failure
 */
static int novai_pool_ensure(Arena *arena,
                             void **data_ptr,
                             uint32_t count,
                             uint32_t *cap_ptr,
                             size_t elem_sz) {
    if (count < *cap_ptr) {
        return 0; /* Room available */
    }

    uint32_t new_cap = (*cap_ptr == 0) ? 16 : (*cap_ptr) * 2;
    void *new_data = novai_pool_grow(arena, *data_ptr,
                                     *cap_ptr, new_cap, elem_sz);
    if (new_data == NULL) {
        return -1;
    }

    *data_ptr = new_data;
    *cap_ptr = new_cap;
    return 0;
}

/**
 * @brief Ensure a pool can hold `needed` more elements
 *
 * @param arena    Backing arena
 * @param data_ptr Pointer to the pool array pointer (in/out)
 * @param count    Current element count
 * @param cap_ptr  Pointer to capacity (in/out)
 * @param needed   Number of additional elements needed
 * @param elem_sz  Size of one element in bytes
 * @return 0 on success, -1 on failure
 */
static int novai_pool_ensure_n(Arena *arena,
                               void **data_ptr,
                               uint32_t count,
                               uint32_t *cap_ptr,
                               uint32_t needed,
                               size_t elem_sz) {
    uint32_t required = count + needed;
    if (required <= *cap_ptr) {
        return 0;
    }

    uint32_t new_cap = *cap_ptr;
    if (new_cap == 0) {
        new_cap = 16;
    }
    while (new_cap < required) {
        new_cap *= 2;
    }

    void *new_data = novai_pool_grow(arena, *data_ptr,
                                     *cap_ptr, new_cap, elem_sz);
    if (new_data == NULL) {
        return -1;
    }

    *data_ptr = new_data;
    *cap_ptr = new_cap;
    return 0;
}

/* ============================================================
 * PUBLIC API: CREATE / DESTROY
 * ============================================================ */

/**
 * @brief Create a new AST table with arena backing
 *
 * @param source_name  Source filename (not owned, must outlive table)
 * @return Initialized table, or table with arena==NULL on failure
 *
 * @post All pools are allocated with initial capacity
 *
 * COMPLEXITY: O(1)
 */
NovaASTTable nova_ast_table_create(const char *source_name) {
    NovaASTTable t;
    memset(&t, 0, sizeof(t));
    t.root = NOVA_IDX_NONE;
    t.source_name = source_name;

    Arena *arena = arena_create(0); /* 64KB default chunk */
    if (arena == NULL) {
        return t;
    }
    t.arena = arena;

    /* Pre-allocate core pools */
    t.exprs = (NovaRowExpr *)arena_alloc(
        arena, (size_t)NOVA_POOL_INIT_EXPRS * sizeof(NovaRowExpr));
    t.stmts = (NovaRowStmt *)arena_alloc(
        arena, (size_t)NOVA_POOL_INIT_STMTS * sizeof(NovaRowStmt));
    t.blocks = (NovaRowBlock *)arena_alloc(
        arena, (size_t)NOVA_POOL_INIT_BLOCKS * sizeof(NovaRowBlock));

    if (t.exprs == NULL || t.stmts == NULL || t.blocks == NULL) {
        arena_destroy(arena);
        t.arena = NULL;
        return t;
    }

    memset(t.exprs, 0,
           (size_t)NOVA_POOL_INIT_EXPRS * sizeof(NovaRowExpr));
    memset(t.stmts, 0,
           (size_t)NOVA_POOL_INIT_STMTS * sizeof(NovaRowStmt));
    memset(t.blocks, 0,
           (size_t)NOVA_POOL_INIT_BLOCKS * sizeof(NovaRowBlock));

    t.expr_cap  = NOVA_POOL_INIT_EXPRS;
    t.stmt_cap  = NOVA_POOL_INIT_STMTS;
    t.block_cap = NOVA_POOL_INIT_BLOCKS;

    /* Pre-allocate auxiliary pools */
    t.expr_extra = (NovaExprIdx *)arena_alloc(
        arena, (size_t)NOVA_POOL_INIT_EXTRA * sizeof(NovaExprIdx));
    t.fields = (NovaRowTableField *)arena_alloc(
        arena, (size_t)NOVA_POOL_INIT_FIELDS * sizeof(NovaRowTableField));
    t.params = (NovaRowParam *)arena_alloc(
        arena, (size_t)NOVA_POOL_INIT_PARAMS * sizeof(NovaRowParam));
    t.branches = (NovaRowIfBranch *)arena_alloc(
        arena, (size_t)NOVA_POOL_INIT_BRANCHES * sizeof(NovaRowIfBranch));
    t.names = (NovaRowNameRef *)arena_alloc(
        arena, (size_t)NOVA_POOL_INIT_NAMES * sizeof(NovaRowNameRef));

    if (t.expr_extra == NULL || t.fields == NULL ||
        t.params == NULL || t.branches == NULL || t.names == NULL) {
        arena_destroy(arena);
        t.arena = NULL;
        return t;
    }

    t.extra_cap   = NOVA_POOL_INIT_EXTRA;
    t.field_cap   = NOVA_POOL_INIT_FIELDS;
    t.param_cap   = NOVA_POOL_INIT_PARAMS;
    t.branch_cap  = NOVA_POOL_INIT_BRANCHES;
    t.name_cap    = NOVA_POOL_INIT_NAMES;

    return t;
}

/**
 * @brief Destroy an AST table and free all memory
 *
 * @param table  Table to destroy (NULL arena is safe)
 *
 * COMPLEXITY: O(1) — arena_destroy frees all chunks at once
 */
void nova_ast_table_destroy(NovaASTTable *table) {
    if (table == NULL) {
        return;
    }
    if (table->arena != NULL) {
        arena_destroy(table->arena);
    }
    memset(table, 0, sizeof(*table));
    table->root = NOVA_IDX_NONE;
}

/* ============================================================
 * PUBLIC API: ADD NODE FUNCTIONS
 * ============================================================ */

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
                              NovaSourceLoc loc) {
    if (table == NULL || table->arena == NULL) {
        return NOVA_IDX_NONE;
    }

    if (novai_pool_ensure(table->arena,
                          (void **)&table->exprs,
                          table->expr_count,
                          &table->expr_cap,
                          sizeof(NovaRowExpr)) != 0) {
        return NOVA_IDX_NONE;
    }

    uint32_t idx = table->expr_count;
    table->expr_count++;

    NovaRowExpr *e = &table->exprs[idx];
    memset(e, 0, sizeof(*e));
    e->kind = kind;
    e->loc  = loc;

    return idx;
}

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
                              NovaSourceLoc loc) {
    if (table == NULL || table->arena == NULL) {
        return NOVA_IDX_NONE;
    }

    if (novai_pool_ensure(table->arena,
                          (void **)&table->stmts,
                          table->stmt_count,
                          &table->stmt_cap,
                          sizeof(NovaRowStmt)) != 0) {
        return NOVA_IDX_NONE;
    }

    uint32_t idx = table->stmt_count;
    table->stmt_count++;

    NovaRowStmt *s = &table->stmts[idx];
    memset(s, 0, sizeof(*s));
    s->kind = kind;
    s->loc  = loc;
    s->next = NOVA_IDX_NONE;

    return idx;
}

/**
 * @brief Add a block node to the table
 *
 * @param table  AST table (must not be NULL)
 * @param loc    Source location
 * @return Index of the new block, or NOVA_IDX_NONE on failure
 */
NovaBlockIdx nova_ast_add_block(NovaASTTable *table,
                                NovaSourceLoc loc) {
    if (table == NULL || table->arena == NULL) {
        return NOVA_IDX_NONE;
    }

    if (novai_pool_ensure(table->arena,
                          (void **)&table->blocks,
                          table->block_count,
                          &table->block_cap,
                          sizeof(NovaRowBlock)) != 0) {
        return NOVA_IDX_NONE;
    }

    uint32_t idx = table->block_count;
    table->block_count++;

    NovaRowBlock *b = &table->blocks[idx];
    b->first      = NOVA_IDX_NONE;
    b->last       = NOVA_IDX_NONE;
    b->stmt_count = 0;
    b->loc        = loc;

    return idx;
}

/**
 * @brief Append a statement to a block (by index)
 *
 * Links stmt into the block's linked list via NovaRowStmt.next.
 *
 * @param table  AST table
 * @param block  Block index
 * @param stmt   Statement index to append
 *
 * @pre block != NOVA_IDX_NONE && stmt != NOVA_IDX_NONE
 */
void nova_ast_block_append(NovaASTTable *table,
                           NovaBlockIdx block,
                           NovaStmtIdx stmt) {
    if (table == NULL || block == NOVA_IDX_NONE ||
        stmt == NOVA_IDX_NONE) {
        return;
    }

    NovaRowBlock *b = &table->blocks[block];
    NovaRowStmt  *s = &table->stmts[stmt];
    s->next = NOVA_IDX_NONE; /* Ensure tail is capped */

    if (b->first == NOVA_IDX_NONE) {
        b->first = stmt;
        b->last  = stmt;
    } else {
        table->stmts[b->last].next = stmt;
        b->last = stmt;
    }
    b->stmt_count++;
}

/* ============================================================
 * PUBLIC API: AUXILIARY POOL RESERVATIONS
 * ============================================================ */

/**
 * @brief Reserve a contiguous range of expression indices in the extra pool
 *
 * @param table  AST table
 * @param count  Number of slots to reserve
 * @return Start index in expr_extra[], or NOVA_IDX_NONE on failure
 */
NovaExtraIdx nova_ast_add_expr_list(NovaASTTable *table, int count) {
    if (table == NULL || table->arena == NULL || count <= 0) {
        return NOVA_IDX_NONE;
    }

    if (novai_pool_ensure_n(table->arena,
                            (void **)&table->expr_extra,
                            table->extra_count,
                            &table->extra_cap,
                            (uint32_t)count,
                            sizeof(NovaExprIdx)) != 0) {
        return NOVA_IDX_NONE;
    }

    uint32_t start = table->extra_count;
    table->extra_count += (uint32_t)count;

    /* Initialize all slots to NOVA_IDX_NONE */
    for (int i = 0; i < count; i++) {
        table->expr_extra[start + (uint32_t)i] = NOVA_IDX_NONE;
    }

    return start;
}

/**
 * @brief Reserve a contiguous range of table field slots
 *
 * @param table  AST table
 * @param count  Number of fields to reserve
 * @return Start index in fields[], or NOVA_IDX_NONE on failure
 */
NovaFieldIdx nova_ast_add_fields(NovaASTTable *table, int count) {
    if (table == NULL || table->arena == NULL || count <= 0) {
        return NOVA_IDX_NONE;
    }

    if (novai_pool_ensure_n(table->arena,
                            (void **)&table->fields,
                            table->field_count,
                            &table->field_cap,
                            (uint32_t)count,
                            sizeof(NovaRowTableField)) != 0) {
        return NOVA_IDX_NONE;
    }

    uint32_t start = table->field_count;
    table->field_count += (uint32_t)count;
    return start;
}

/**
 * @brief Reserve a contiguous range of parameter slots
 *
 * @param table  AST table
 * @param count  Number of params to reserve
 * @return Start index in params[], or NOVA_IDX_NONE on failure
 */
NovaParamIdx nova_ast_add_params(NovaASTTable *table, int count) {
    if (table == NULL || table->arena == NULL || count <= 0) {
        return NOVA_IDX_NONE;
    }

    if (novai_pool_ensure_n(table->arena,
                            (void **)&table->params,
                            table->param_count,
                            &table->param_cap,
                            (uint32_t)count,
                            sizeof(NovaRowParam)) != 0) {
        return NOVA_IDX_NONE;
    }

    uint32_t start = table->param_count;
    table->param_count += (uint32_t)count;
    return start;
}

/**
 * @brief Reserve a contiguous range of if-branch slots
 *
 * @param table  AST table
 * @param count  Number of branches to reserve
 * @return Start index in branches[], or NOVA_IDX_NONE on failure
 */
NovaBranchIdx nova_ast_add_branches(NovaASTTable *table, int count) {
    if (table == NULL || table->arena == NULL || count <= 0) {
        return NOVA_IDX_NONE;
    }

    if (novai_pool_ensure_n(table->arena,
                            (void **)&table->branches,
                            table->branch_count,
                            &table->branch_cap,
                            (uint32_t)count,
                            sizeof(NovaRowIfBranch)) != 0) {
        return NOVA_IDX_NONE;
    }

    uint32_t start = table->branch_count;
    table->branch_count += (uint32_t)count;
    return start;
}

/**
 * @brief Reserve a contiguous range of name-ref slots
 *
 * @param table  AST table
 * @param count  Number of names to reserve
 * @return Start index in names[], or NOVA_IDX_NONE on failure
 */
NovaNameIdx nova_ast_add_names(NovaASTTable *table, int count) {
    if (table == NULL || table->arena == NULL || count <= 0) {
        return NOVA_IDX_NONE;
    }

    if (novai_pool_ensure_n(table->arena,
                            (void **)&table->names,
                            table->name_count,
                            &table->name_cap,
                            (uint32_t)count,
                            sizeof(NovaRowNameRef)) != 0) {
        return NOVA_IDX_NONE;
    }

    uint32_t start = table->name_count;
    table->name_count += (uint32_t)count;
    return start;
}
