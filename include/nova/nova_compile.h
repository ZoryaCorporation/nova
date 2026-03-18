/**
 * @file nova_compile.h
 * @brief Nova Language - Compiler API (Row AST -> Bytecode)
 *
 * The compiler walks the row-based AST (NovaASTTable) and emits
 * bytecode into function prototypes (NovaProto). It handles:
 *   - Scope management (block nesting, local variable allocation)
 *   - Register allocation (linear scan within each function)
 *   - Upvalue resolution (capturing variables from outer scopes)
 *   - Constant folding (number/string/boolean literal expressions)
 *   - Control flow compilation (jumps, loops, conditionals)
 *   - Closure creation (nested function prototypes)
 *   - async/await/spawn lowering to coroutine opcodes
 *
 * Architecture:
 *   NovaCompiler (top-level state)
 *   └── NovaFuncState (per-function compilation context)
 *       ├── NovaProto (the proto being built)
 *       ├── NovaLocal[] (local variable register assignments)
 *       ├── NovaScope[] (block scope stack)
 *       └── parent -> NovaFuncState (enclosing function)
 *
 * The compiler processes the row AST by index. Each compile_*
 * function takes an expression or statement index and emits
 * instructions into the current function's proto.
 *
 * @author Anthony Taliento
 * @date 2026-02-06
 * @version 0.2.0
 *
 * @copyright Copyright (c) 2026 Zorya Corporation
 * @license MIT
 *
 * ZORYA-C COMPLIANCE: v2.0.0
 */

#ifndef NOVA_COMPILE_H
#define NOVA_COMPILE_H

#include "nova_conf.h"
#include "nova_opcode.h"
#include "nova_proto.h"
#include "nova_ast_row.h"

#include <stdint.h>

/* ============================================================
 * EXPRESSION RESULT DESCRIPTOR
 *
 * When compiling an expression, the result can be:
 *   - Already in a register (NOVA_EXPR_REG)
 *   - A constant that hasn't been loaded yet (NOVA_EXPR_CONST)
 *   - A boolean test that set the condition flag (NOVA_EXPR_JMP)
 *   - A relocatable instruction (NOVA_EXPR_RELOC)
 *   - Nil / true / false literal (NOVA_EXPR_NIL etc.)
 *   - A pending function call result (NOVA_EXPR_CALL)
 *   - A vararg expansion (NOVA_EXPR_VARARG)
 *
 * The compiler tracks this to avoid unnecessary MOVE and LOADK
 * instructions when the result is already where it needs to be.
 * ============================================================ */

typedef enum {
    NOVA_EK_VOID    = 0,   /* Expression has no value (statement)       */
    NOVA_EK_NIL     = 1,   /* Literal nil                               */
    NOVA_EK_TRUE    = 2,   /* Literal true                              */
    NOVA_EK_FALSE   = 3,   /* Literal false                             */
    NOVA_EK_INTEGER = 4,   /* Integer literal (fits in sBx)             */
    NOVA_EK_NUMBER  = 5,   /* Number literal (in constant pool)         */
    NOVA_EK_STRING  = 6,   /* String literal (in constant pool)         */
    NOVA_EK_CONST   = 7,   /* Constant pool index (generic)             */
    NOVA_EK_REG     = 8,   /* Value already in a register               */
    NOVA_EK_LOCAL   = 9,   /* Local variable (register, non-temp)       */
    NOVA_EK_UPVAL   = 10,  /* Upvalue reference                         */
    NOVA_EK_GLOBAL  = 11,  /* Global variable (name in constant pool)   */
    NOVA_EK_INDEXED = 12,  /* Table index: table[key]                   */
    NOVA_EK_FIELD   = 13,  /* Table field: table.name                   */
    NOVA_EK_CALL    = 14,  /* Pending function call result              */
    NOVA_EK_VARARG  = 15,  /* Vararg expansion (...)                    */
    NOVA_EK_RELOC   = 16,  /* Relocatable: instruction needing A patch  */
    NOVA_EK_JMP     = 17   /* Boolean test with pending jumps           */
} NovaExprKind;

/**
 * Compiled expression descriptor.
 *
 * Tracks where an expression's value currently lives so the
 * compiler can decide how to get it into the target register.
 */
typedef struct {
    NovaExprKind kind;
    uint32_t line;         /* Source line for debug info             */

    union {
        uint8_t   reg;        /* EXPR_REG, EXPR_LOCAL: register index    */
        uint8_t   upval;      /* EXPR_UPVAL: upvalue index               */
        uint32_t  const_idx;  /* EXPR_CONST, EXPR_STRING, EXPR_NUMBER,
                                 EXPR_GLOBAL: constant pool index        */
        nova_int_t integer;   /* EXPR_INTEGER: small integer literal     */
        uint32_t  pc;         /* EXPR_RELOC: instruction to patch        */
        struct {
            uint8_t  table;   /* EXPR_INDEXED/FIELD: table register      */
            uint16_t key;     /* EXPR_INDEXED: key reg/RK
                                 EXPR_FIELD: constant pool index         */
        } index;
        struct {
            uint32_t true_list;   /* EXPR_JMP: pending true-jumps        */
            uint32_t false_list;  /* EXPR_JMP: pending false-jumps       */
        } jmp;
    } u;

} NovaExprDesc;

/* ============================================================
 * LOCAL VARIABLE (compile-time)
 *
 * Tracks a local variable during compilation. Separate from
 * NovaLocalInfo (which is debug info stored in the proto).
 * ============================================================ */

typedef struct {
    const char *name;       /* Variable name                          */
    uint32_t    name_len;   /* Name length                            */
    uint8_t     reg;        /* Assigned register                      */
    uint8_t     is_captured;/* 1 if captured by a closure (upvalue)   */
    uint8_t     is_const;   /* 1 if declared with 'const'             */
    uint8_t     is_active;  /* 1 if past declaration point            */
    uint32_t    start_pc;   /* PC where variable becomes active       */
} NovaCompLocal;

/* ============================================================
 * BLOCK SCOPE
 *
 * Tracks a lexical scope (do/end, if/then, while/do, etc.).
 * When a scope closes, locals declared in it are freed and
 * any captured locals cause CLOSE instructions.
 * ============================================================ */

typedef struct {
    uint8_t  first_local;    /* Index of first local in this scope     */
    uint8_t  num_locals;     /* Number of locals declared in scope     */
    uint8_t  has_upvalues;   /* 1 if any local was captured            */
    uint8_t  is_loop;        /* 1 if this is a loop scope (for break)  */
    uint32_t break_list;     /* Linked list of break-jump PCs          */
    uint32_t continue_list;  /* Linked list of continue-jump PCs       */
} NovaScope;

/* ============================================================
 * JUMP LIST
 *
 * The compiler uses linked lists of pending jumps that need
 * back-patching. Each entry is an instruction PC. The "next"
 * link is stored in the sBx field of the jump instruction
 * itself (pointing to the previous entry), with UINT32_MAX
 * as the end sentinel.
 * ============================================================ */

#define NOVA_NO_JUMP UINT32_MAX

/* ============================================================
 * GOTO / LABEL DESCRIPTORS
 *
 * Labels are function-scoped. Goto statements may reference
 * labels defined later (forward jump) or earlier (backward).
 * Pending gotos are resolved when the label is encountered,
 * or at end-of-function if still unresolved (error).
 * ============================================================ */

/** Maximum pending gotos per function */
#define NOVA_MAX_GOTOS  64

/** Maximum labels per function */
#define NOVA_MAX_LABELS 64

/**
 * @brief Descriptor for a pending goto statement.
 */
typedef struct {
    const char *name;           /**< Label name (points into AST)     */
    size_t      name_len;       /**< Label name length                */
    uint32_t    pc;             /**< PC of the JMP instruction        */
    uint32_t    line;           /**< Source line (for error reporting) */
    uint8_t     active_locals;  /**< Number of active locals at goto  */
    uint8_t     scope_depth;    /**< Scope depth at goto              */
} NovaGotoDesc;

/**
 * @brief Descriptor for a resolved label.
 */
typedef struct {
    const char *name;           /**< Label name (points into AST)     */
    size_t      name_len;       /**< Label name length                */
    uint32_t    pc;             /**< PC of the label target           */
    uint8_t     active_locals;  /**< Number of active locals at label */
    uint8_t     scope_depth;    /**< Scope depth at label             */
} NovaLabelDesc;

/* ============================================================
 * FUNCTION STATE
 *
 * Per-function compilation context. Nested functions push
 * a new NovaFuncState that links to its parent.
 * ============================================================ */

/** Maximum block nesting depth */
#define NOVA_MAX_SCOPE_DEPTH 64

typedef struct NovaFuncState {
    NovaProto       *proto;         /* The proto being built              */
    struct NovaFuncState *parent;   /* Enclosing function (NULL = top)    */
    struct NovaCompiler  *compiler; /* Back-pointer to top-level state    */

    /* Register allocator */
    uint8_t          free_reg;      /* Next free register                 */
    uint8_t          active_locals; /* Number of active local variables   */

    /* Locals */
    NovaCompLocal    locals[NOVA_MAX_LOCALS];
    uint16_t         local_count;   /* Total locals allocated (incl. dead)*/

    /* Scope stack */
    NovaScope        scopes[NOVA_MAX_SCOPE_DEPTH];
    uint8_t          scope_depth;   /* Current nesting depth              */

    /* Pending jump lists for boolean expressions */
    uint32_t         last_target;   /* PC of last jump target emitted     */

    /* Goto / label tracking */
    NovaGotoDesc     gotos[NOVA_MAX_GOTOS];
    uint16_t         goto_count;    /* Number of pending gotos            */
    NovaLabelDesc    labels[NOVA_MAX_LABELS];
    uint16_t         label_count;   /* Number of defined labels           */

    /* Metadata */
    uint8_t          is_async;      /* 1 if compiling async function      */

} NovaFuncState;

/* ============================================================
 * TOP-LEVEL COMPILER STATE
 * ============================================================ */

typedef struct NovaCompiler {
    const NovaASTTable *ast;        /* The row AST to compile             */
    NovaFuncState      *current_fs; /* Currently compiling function       */

    /* Error state */
    int                 had_error;  /* 1 if any error was encountered     */
    const char         *error_msg;  /* Last error message (static string) */
    uint32_t            error_line; /* Line of last error                 */

    /* Source info */
    const char         *source;     /* Source filename                    */

} NovaCompiler;

/* ============================================================
 * COMPILER API
 * ============================================================ */

/**
 * @brief Compile a row AST into a function prototype.
 *
 * This is the main entry point. Takes a fully parsed row AST
 * and produces a NovaProto representing the top-level chunk.
 * The proto contains the bytecode, constants, sub-protos for
 * nested functions, and debug info.
 *
 * @param ast     Row AST table from the parser (must not be NULL)
 * @param source  Source filename for debug info (must not be NULL)
 *
 * @return Top-level function prototype, or NULL on error.
 *         Caller owns the proto and must call nova_proto_destroy.
 *
 * @pre ast != NULL && source != NULL
 * @post On success, returned proto is fully compiled and ready
 *       for optimization, codegen, or direct VM execution.
 *
 * COMPLEXITY: O(n) where n = total AST nodes
 * THREAD SAFETY: Not thread-safe
 *
 * @example
 *   NovaProto *main = nova_compile(&parser.table, "script.n");
 *   if (main == NULL) {
 *       fprintf(stderr, "compilation failed\n");
 *   } else {
 *       nova_proto_dump(main, 0);
 *       nova_proto_destroy(main);
 *   }
 */
NovaProto *nova_compile(const NovaASTTable *ast, const char *source);

/**
 * @brief Compile a single expression (for REPL / eval).
 *
 * Wraps the expression in a function that returns its value.
 *
 * @param ast     Row AST table
 * @param expr    Expression index to compile
 * @param source  Source filename
 *
 * @return Function prototype that returns the expression value,
 *         or NULL on error.
 */
NovaProto *nova_compile_expression(const NovaASTTable *ast,
                                   NovaExprIdx expr,
                                   const char *source);

#endif /* NOVA_COMPILE_H */
