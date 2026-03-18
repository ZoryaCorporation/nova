/**
 * @file nova_parse.h
 * @brief Nova Language - Parser API
 *
 * Recursive descent parser with Pratt (TDOP) expression parsing.
 * Consumes preprocessed tokens and produces an AST.
 *
 * Usage:
 *   NovaParser parser;
 *   nova_parser_init(&parser, pp);
 *   NovaAST *ast = nova_parse(&parser);
 *   // ... use ast ...
 *   nova_ast_free(ast);
 *   nova_parser_free(&parser);
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
 *   - nova_ast.h (AST node types)
 *   - nova_pp.h (preprocessor token stream)
 */

#ifndef NOVA_PARSE_H
#define NOVA_PARSE_H

#include "nova_ast.h"
#include "nova_ast_row.h"
#include "nova_pp.h"

/* ============================================================
 * PARSER STATE
 * ============================================================ */

/**
 * @brief Parser state
 *
 * Maintains the token stream and parsing context.
 * Reads preprocessed tokens from a NovaPP instance.
 */
typedef struct {
    NovaPP     *pp;             /**< Preprocessor (token source)  */

    NovaToken   current;        /**< Current token                */
    NovaToken   previous;       /**< Previous token (for locs)    */

    int         had_error;      /**< Was there a parse error?     */
    int         panic_mode;     /**< In error recovery mode?      */

    char        error_msg[512]; /**< Last error message           */
    int         error_count;    /**< Total parse errors           */

    NovaASTTable table;         /**< Row-based AST output         */
} NovaParser;

/* ============================================================
 * PARSER API
 * ============================================================ */

/**
 * @brief Initialize the parser
 *
 * @param P   Parser state to initialize
 * @param pp  Preprocessor with processed tokens ready
 * @return 0 on success, -1 on error
 *
 * @pre P != NULL && pp != NULL
 * @pre pp has been processed (nova_pp_process_* called)
 */
int nova_parser_init(NovaParser *P, NovaPP *pp);

/**
 * @brief Free parser resources
 *
 * Does NOT free the preprocessor or AST. Those are owned
 * by the caller.
 *
 * @param P Parser state (NULL is safe)
 */
void nova_parser_free(NovaParser *P);

/**
 * @brief Parse the token stream into an AST
 *
 * Parses the entire preprocessed token stream and returns
 * a complete AST. Returns NULL on fatal error.
 *
 * @param P Parser state
 * @return Complete AST, or NULL on fatal error
 *
 * @post Caller must call nova_ast_free() on the result
 */
NovaAST *nova_parse(NovaParser *P);

/**
 * @brief Parse a single expression
 *
 * Useful for REPL mode or expression evaluation.
 *
 * @param P Parser state
 * @return Expression AST node, or NULL on error
 */
NovaExpr *nova_parse_expression(NovaParser *P);

/**
 * @brief Get parser error message
 *
 * @param P Parser state
 * @return Error message string
 */
const char *nova_parser_error(const NovaParser *P);

/**
 * @brief Get parse error count
 */
int nova_parser_error_count(const NovaParser *P);

/* ============================================================
 * ROW-BASED PARSER API
 *
 * These functions emit into P->table (NovaASTTable) instead of
 * heap-allocating pointer-based AST nodes.
 * ============================================================ */

/**
 * @brief Parse the token stream into a row-based AST table
 *
 * Populates P->table. Call nova_ast_table_destroy(&P->table)
 * when done. Returns 0 on success, -1 on fatal error.
 *
 * @param P            Parser state
 * @param source_name  Source filename (not owned)
 * @return 0 on success, -1 on fatal error
 */
int nova_parse_row(NovaParser *P, const char *source_name);

/**
 * @brief Parse a single expression into the row-based table
 *
 * @param P Parser state (P->table must be initialized)
 * @return Expression index, or NOVA_IDX_NONE on error
 */
NovaExprIdx nova_parse_row_expression(NovaParser *P);

#endif /* NOVA_PARSE_H */
