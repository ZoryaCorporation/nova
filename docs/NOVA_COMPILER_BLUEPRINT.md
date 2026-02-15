# NOVA COMPILER BLUEPRINT

**Purpose**: This document is the complete internal design reference for `nova_compile.c`.  
It exists so the compiler can be written in small, focused chunks without needing to re-derive the full architecture each time.

**Date**: February 6, 2026  
**Status**: Active implementation guide

---

## 1. FILE STRUCTURE (nova_compile.c)

The file is split into 4 logical parts, each written separately:

| Part | Lines (est.) | Contents |
|------|-------------|----------|
| **Part 1: Foundation** | ~350 | Includes, error reporting, scope management, register allocation, jump-list management, discharge helpers |
| **Part 2: Expressions** | ~550 | compile_expr() dispatch + all 23 expression type handlers |
| **Part 3: Statements** | ~500 | compile_stmt() dispatch + all 19 statement type handlers, compile_block() |
| **Part 4: Public API** | ~150 | nova_compile(), nova_compile_expression(), top-level FuncState setup |

---

## 2. INCLUDES AND FORWARD DECLARATIONS

```c
#include "nova/nova_compile.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
```

All internal (static) functions use the `novai_` prefix.  
Key forward declarations needed at top of file:

```c
static void novai_compile_expr(NovaFuncState *fs, NovaExprIdx idx, NovaExprDesc *e);
static void novai_compile_stmt(NovaFuncState *fs, NovaStmtIdx idx);
static void novai_compile_block(NovaFuncState *fs, NovaBlockIdx idx);
```

---

## 3. ERROR REPORTING

Simple macro + function. Sets `compiler->had_error = 1` and stores message.

```c
static void novai_error(NovaFuncState *fs, uint32_t line, const char *msg);
```

The `line` comes from either `NovaRowExpr.loc.line` or `NovaRowStmt.loc.line`.

Helper macro to get the AST table:
```c
#define AST(fs)  ((NovaASTTable *)(fs)->compiler->ast)
```

Note: `compiler->ast` is `const NovaASTTable*`, so we need a cast for the inline access helpers (which take non-const). We'll cast in the macro since we only read, never write.

---

## 4. REGISTER ALLOCATION

Simple linear allocator. `fs->free_reg` is the watermark.

```c
static uint8_t novai_alloc_reg(NovaFuncState *fs);      /* Returns fs->free_reg++ */
static void    novai_free_reg(NovaFuncState *fs, uint8_t reg);  /* fs->free_reg = reg if reg == free_reg-1 */
static void    novai_reserve_regs(NovaFuncState *fs, int count); /* free_reg += count */
static void    novai_check_stack(NovaFuncState *fs, uint8_t needed); /* Update max_stack */
```

**Rules:**
- `free_reg` starts at `active_locals` (locals occupy registers 0..N-1)
- Temps are allocated above locals: `free_reg, free_reg+1, ...`
- Free only the topmost temp (stack discipline)
- `max_stack` tracks high-water mark for the proto

---

## 5. SCOPE MANAGEMENT

```c
static void novai_enter_scope(NovaFuncState *fs, int is_loop);
static void novai_leave_scope(NovaFuncState *fs);
```

**Enter scope:**
- Push `scopes[scope_depth++]`
- Set `first_local = active_locals`
- Set `num_locals = 0`
- Set `has_upvalues = 0`
- Set `is_loop = is_loop`
- Set `break_list = NOVA_NO_JUMP`
- Set `continue_list = NOVA_NO_JUMP`

**Leave scope:**
- If `has_upvalues`, emit `CLOSE` for the first local register in scope
- For each local declared in this scope:
  - Call `nova_proto_close_local(proto, reg, current_pc)`
  - `active_locals--`
- `free_reg = active_locals` (reclaim temp registers)
- `scope_depth--`

---

## 6. LOCAL VARIABLE MANAGEMENT

```c
static void    novai_add_local(NovaFuncState *fs, const char *name, uint32_t len);
static int     novai_resolve_local(NovaFuncState *fs, const char *name, uint32_t len);
static int     novai_resolve_upvalue(NovaFuncState *fs, const char *name, uint32_t len);
```

**novai_add_local:**
- `locals[local_count] = { name, len, free_reg, 0, 0, 0, current_pc }`
- `local_count++`, `active_locals++`
- Alloc register (free_reg++)
- `nova_proto_add_local(proto, name, reg, pc)` for debug info

**novai_resolve_local:**
- Walk `locals[]` backwards from `active_locals - 1` to `0`
- Compare name/len. Return register index if found, -1 if not.

**novai_resolve_upvalue (recursive):**
- If `fs->parent == NULL`: return -1 (not found = global)
- Try `novai_resolve_local(fs->parent, name, len)`:
  - If found: mark parent local as `is_captured = 1`, mark parent scope `has_upvalues = 1`
  - `nova_proto_add_upvalue(fs->proto, parent_reg, 1, name)` → returns upval index
- Else try `novai_resolve_upvalue(fs->parent, name, len)`:
  - If found: `nova_proto_add_upvalue(fs->proto, parent_upval_idx, 0, name)` → returns upval index
- Return -1 if neither found

---

## 7. JUMP LIST MANAGEMENT

Jump lists are linked through the sBx field of JMP instructions.  
`NOVA_NO_JUMP` (UINT32_MAX) = end sentinel.

```c
static uint32_t novai_emit_jmp(NovaFuncState *fs, uint32_t line);
static void     novai_patch_list(NovaFuncState *fs, uint32_t list, uint32_t target);
static void     novai_concat_jmp_list(NovaFuncState *fs, uint32_t *l1, uint32_t l2);
static void     novai_patch_to_here(NovaFuncState *fs, uint32_t list);
```

**novai_emit_jmp:**
- `nova_proto_emit_asbx(proto, NOVA_OP_JMP, 0, NOVA_NO_JUMP, line)` → returns PC
- The sBx is temporarily NOVA_NO_JUMP (will be patched later)

**novai_patch_list:**
- Walk the linked list: for each PC in list, read its sBx (the "next" link), then patch sBx to point to `target` (as offset: `target - pc - 1`)

**novai_concat_jmp_list:**
- Append `l2` to the end of `*l1`. If `*l1 == NOVA_NO_JUMP`, just set `*l1 = l2`.
- Otherwise walk `*l1` following sBx links to find the tail, then link tail to `l2`.

**novai_patch_to_here:**
- `novai_patch_list(fs, list, nova_proto_pc(proto))`

---

## 8. DISCHARGE (Expression → Register)

The discharge system moves an expression value into a concrete register.

```c
static void novai_discharge_to_reg(NovaFuncState *fs, NovaExprDesc *e, uint8_t reg);
static void novai_discharge_to_any(NovaFuncState *fs, NovaExprDesc *e);
static void novai_discharge_to_next(NovaFuncState *fs, NovaExprDesc *e);
static void novai_expr_free(NovaFuncState *fs, NovaExprDesc *e);
```

**novai_discharge_to_reg** (based on `e->kind`):

| Kind | Emit |
|------|------|
| `NOVA_EK_NIL` | `LOADNIL reg, 0, 0` |
| `NOVA_EK_TRUE` | `LOADBOOL reg, 1, 0` |
| `NOVA_EK_FALSE` | `LOADBOOL reg, 0, 0` |
| `NOVA_EK_INTEGER` | If fits sBx: `LOADINT reg, value`. Else: add to const pool, `LOADK reg, idx` |
| `NOVA_EK_NUMBER` | Add to const pool → `LOADK reg, idx` |
| `NOVA_EK_STRING` | find_or_add_string → `LOADK reg, idx` |
| `NOVA_EK_CONST` | `LOADK reg, const_idx` |
| `NOVA_EK_REG` | If `e->u.reg != reg`: `MOVE reg, e->u.reg` |
| `NOVA_EK_LOCAL` | If `e->u.reg != reg`: `MOVE reg, e->u.reg` |
| `NOVA_EK_UPVAL` | `GETUPVAL reg, e->u.upval` |
| `NOVA_EK_GLOBAL` | `GETGLOBAL reg, e->u.const_idx` |
| `NOVA_EK_INDEXED` | `GETTABLE reg, e->u.index.table, e->u.index.key` |
| `NOVA_EK_FIELD` | `GETFIELD reg, e->u.index.table, e->u.index.key` |
| `NOVA_EK_CALL` | If reg != A of the CALL instruction: `MOVE reg, A` |
| `NOVA_EK_VARARG` | `VARARG reg, 2` (single result) |
| `NOVA_EK_RELOC` | Patch the A field of the instruction at `e->u.pc` to `reg` |
| `NOVA_EK_JMP` | (complex: discharge boolean test to register, not needed in Part 1) |
| `NOVA_EK_VOID` | (error or no-op) |

After discharge, set `e->kind = NOVA_EK_REG; e->u.reg = reg;`

**novai_discharge_to_any:** If e is already in a reg, do nothing. Otherwise alloc a temp reg and discharge there.

**novai_discharge_to_next:** Discharge to `free_reg`, then alloc that reg.

**novai_expr_free:** If expression used a temp register (kind == NOVA_EK_REG and reg >= active_locals), free it.

---

## 9. EXPRESSION COMPILATION (Part 2)

Main dispatch function:

```c
static void novai_compile_expr(NovaFuncState *fs, NovaExprIdx idx, NovaExprDesc *e)
```

Gets `NovaRowExpr *expr = nova_get_expr(AST(fs), idx)`, switches on `expr->kind`:

### Literal Expressions (trivial, set e->kind directly):

| AST Kind | ExprDesc Result |
|----------|----------------|
| `NOVA_EXPR_NIL` | `e->kind = NOVA_EK_NIL` |
| `NOVA_EXPR_TRUE` | `e->kind = NOVA_EK_TRUE` |
| `NOVA_EXPR_FALSE` | `e->kind = NOVA_EK_FALSE` |
| `NOVA_EXPR_INTEGER` | `e->kind = NOVA_EK_INTEGER; e->u.integer = expr->as.integer.value` |
| `NOVA_EXPR_NUMBER` | `e->kind = NOVA_EK_NUMBER; e->u.const_idx = nova_proto_add_number(proto, val)` |
| `NOVA_EXPR_STRING` | `e->kind = NOVA_EK_STRING; e->u.const_idx = nova_proto_find_or_add_string(...)` |
| `NOVA_EXPR_VARARG` | `e->kind = NOVA_EK_VARARG` |

### Name Resolution:

`NOVA_EXPR_NAME`:
1. Try `novai_resolve_local(fs, name, len)` → `NOVA_EK_LOCAL, reg`
2. Try `novai_resolve_upvalue(fs, name, len)` → `NOVA_EK_UPVAL, upval_idx`
3. Else: global → `NOVA_EK_GLOBAL, const_idx = find_or_add_string(name)`

### Unary Operators:

`NOVA_EXPR_UNARY`: Compile operand first, discharge to reg, then emit:
- `NOVA_UNOP_NEGATE` → `UNM A, B`
- `NOVA_UNOP_NOT` → `NOT A, B`
- `NOVA_UNOP_LEN` → `STRLEN A, B`
- `NOVA_UNOP_BNOT` → `BNOT A, B`
Result: `NOVA_EK_RELOC` with pc of the emitted instruction

### Binary Operators:

`NOVA_EXPR_BINARY`: 
1. Compile left operand, discharge to reg
2. Compile right operand, discharge to reg
3. Free right temp, free left temp
4. Map `NovaBinOp` to `NovaOpcode`:

| BinOp | Opcode |
|-------|--------|
| `ADD` | `NOVA_OP_ADD` |
| `SUB` | `NOVA_OP_SUB` |
| `MUL` | `NOVA_OP_MUL` |
| `DIV` | `NOVA_OP_DIV` |
| `IDIV` | `NOVA_OP_IDIV` |
| `MOD` | `NOVA_OP_MOD` |
| `POW` | `NOVA_OP_POW` |
| `CONCAT` | `NOVA_OP_CONCAT` (special: chain handling) |
| `BAND` | `NOVA_OP_BAND` |
| `BOR` | `NOVA_OP_BOR` |
| `BXOR` | `NOVA_OP_BXOR` |
| `SHL` | `NOVA_OP_SHL` |
| `SHR` | `NOVA_OP_SHR` |

Result: `NOVA_EK_RELOC` with pc of emitted instruction.

**Comparison operators** (`EQ`, `NEQ`, `LT`, `LE`, `GT`, `GE`):
- Emit comparison instruction (e.g. `EQ A=1, B=left, C=right`)
- Emit JMP (the "skip" for when test fails)
- Result: `NOVA_EK_JMP` with true_list/false_list

**Logical operators** (`AND`, `OR`):
- Short-circuit evaluation:
  - `AND`: compile left, if false jump to end (result = left), else compile right
  - `OR`: compile left, if true jump to end (result = left), else compile right
- Result: carried through as `NOVA_EK_JMP`

### Call Expression:

`NOVA_EXPR_CALL` / `NOVA_EXPR_METHOD_CALL`:
1. Compile callee into a register (say R(base))
2. For METHOD_CALL: emit `SELF base, obj_reg, method_name_RK`
3. Compile each argument into consecutive registers: R(base+1), R(base+2), ...
4. Emit `CALL base, nargs+1, 2` (2 = single result expected)
5. Result: `NOVA_EK_CALL; e->u.reg = base` (the result is in R(base))

For multi-return position (last arg in call, return value), set C=0 to get all results.

### Index / Field:

`NOVA_EXPR_INDEX`:
- Compile object, compile index key 
- `e->kind = NOVA_EK_INDEXED; e->u.index = { table_reg, key_rk }`

`NOVA_EXPR_FIELD`:
- Compile object
- Add field name to constant pool
- `e->kind = NOVA_EK_FIELD; e->u.index = { table_reg, name_const_idx }`

`NOVA_EXPR_METHOD`:
- Same as field but used in method call context

### Table Constructor:

`NOVA_EXPR_TABLE`:
1. Emit `NEWTABLE reg, array_hint, hash_hint`
2. For each field in `fields[field_start .. field_start + field_count)`:
   - Compile field value (and key if RECORD/BRACKET)
   - For LIST fields: batch with SETLIST
   - For RECORD fields: `SETFIELD`
   - For BRACKET fields: `SETTABLE`
3. Result: `NOVA_EK_REG` in the allocated register

### Function Literal:

`NOVA_EXPR_FUNCTION`:
1. Create new child proto: `nova_proto_create()`
2. Create new `NovaFuncState` for the child
3. Enter scope, add parameters as locals
4. If `is_async`: set `child_fs.is_async = 1`
5. Compile body block
6. Emit `RETURN0` at end
7. Leave scope, close child FuncState
8. Add child proto to parent: `nova_proto_add_child()`
9. Emit `CLOSURE reg, child_proto_idx` in parent
10. Result: `NOVA_EK_RELOC`

### Async Expressions:

`NOVA_EXPR_AWAIT`:
- Compile operand to reg
- Emit `AWAIT result_reg, operand_reg`
- Result: `NOVA_EK_RELOC`

`NOVA_EXPR_SPAWN`:
- Compile operand to reg
- Emit `SPAWN result_reg, operand_reg`
- Result: `NOVA_EK_RELOC`

`NOVA_EXPR_YIELD`:
- Compile operand (if present) to reg
- Emit `YIELD reg, nvalues, nresults`
- Result: `NOVA_EK_CALL` (results arrive in registers like a call)

### Interpolated String:

`NOVA_EXPR_INTERP_STRING`:
1. Compile each part into consecutive registers
2. Emit `CONCAT base, nparts` to concatenate them all
3. Result: `NOVA_EK_RELOC`

---

## 10. STATEMENT COMPILATION (Part 3)

Main dispatch function:

```c
static void novai_compile_stmt(NovaFuncState *fs, NovaStmtIdx idx)
```

Gets `NovaRowStmt *stmt = nova_get_stmt(AST(fs), idx)`, switches on `stmt->kind`:

### NOVA_STMT_EXPR
- Compile expression (for side effects like function calls)
- Discard result (free any temp registers)

### NOVA_STMT_LOCAL
- For each name in `names[names_start .. +name_count)`:
  - If matching value exists: compile value, discharge to next reg
  - Else: emit `LOADNIL` for the register
  - `novai_add_local(fs, name, len)`

### NOVA_STMT_ASSIGN
- Compile all RHS values first, into temp registers
- For each target:
  - If target is a local: `MOVE local_reg, value_reg`
  - If target is an upvalue: `SETUPVAL upval_idx, value_reg`
  - If target is a global: `SETGLOBAL const_idx, value_reg`
  - If target is an index: `SETTABLE`
  - If target is a field: `SETFIELD`

### NOVA_STMT_IF
- For each branch in `branches[branch_start .. +branch_count)`:
  - If branch has condition (if/elseif):
    - Compile condition → test → `JMP` to next branch on false
  - Compile branch body block
  - At end of body: `JMP` to after-if (patch later)
  - If no condition (else): just compile body
- Patch all end-of-body jumps to here

### NOVA_STMT_WHILE
- Enter loop scope
- `loop_start = current_pc`
- Compile condition → test
- `exit_jmp = emit JMP` (for when condition false)
- Compile body block
- Emit `JMP` back to `loop_start`
- Patch `exit_jmp` to here
- Patch `break_list` to here
- Patch `continue_list` to loop_start
- Leave scope

### NOVA_STMT_REPEAT
- Enter loop scope
- `loop_start = current_pc`
- Compile body block
- Compile condition → test
- If false: `JMP` back to `loop_start`
- Patch break/continue lists
- Leave scope

### NOVA_STMT_FOR_NUMERIC
- Enter scope
- Compile start/stop/step expressions into 3 consecutive registers (R(A), R(A+1), R(A+2))
- If no step: emit default step (LOADINT 1)
- Emit `FORPREP A, jmp_to_test`
- `loop_body_start = pc`
- Add loop variable as local in R(A+3)
- Compile body
- `test_pc = pc`
- Emit `FORLOOP A, jmp_back_to_body`
- Patch FORPREP to jump to test_pc
- Leave scope

### NOVA_STMT_FOR_GENERIC
- Enter scope
- Compile iterator expressions into 3+ registers
- Emit `JMP` to TFORCALL
- `loop_start = pc`
- Add loop variables as locals
- Compile body
- Emit `TFORCALL A, nresults`
- Emit `TFORLOOP A, jmp_back_to_loop_start`
- Patch initial JMP
- Leave scope

### NOVA_STMT_DO
- Enter scope
- Compile body block
- Leave scope

### NOVA_STMT_BREAK
- Must be inside a loop scope. Find nearest loop scope.
- Emit `JMP` and add its PC to the loop scope's `break_list`.

### NOVA_STMT_GOTO / NOVA_STMT_LABEL
- (Phase 2 feature -- emit error or stub for now)

### NOVA_STMT_RETURN
- Compile all return values into consecutive registers starting at `free_reg`
- If 0 values: `RETURN0`
- If 1 value and already in reg: `RETURN1 reg`
- Else: `RETURN base, count+1`

### NOVA_STMT_FUNCTION
- Desugar: `function a.b.c()` → `a.b.c = function() end`
- Compile the function expression (creates closure)
- Assign to the target (global, field, etc.)

### NOVA_STMT_LOCAL_FUNCTION
- Add local name first (allows self-recursion)
- Compile function expression into that local's register
- Mark local as active

### NOVA_STMT_IMPORT
- Emit `IMPORT reg, module_name_const_idx`
- If alias: add alias as local bound to reg
- Else: add module name as local

### NOVA_STMT_EXPORT
- Compile value expression
- Emit `EXPORT name_const_idx, value_reg`

### NOVA_STMT_CONST
- Same as local but mark `is_const = 1` on the local

---

## 11. BLOCK COMPILATION

```c
static void novai_compile_block(NovaFuncState *fs, NovaBlockIdx idx)
```

- Get block: `nova_get_block(AST(fs), idx)`
- Walk the statement linked list: `stmt = first; while stmt != NOVA_IDX_NONE`
- For each: `novai_compile_stmt(fs, stmt_idx)`, then `stmt_idx = stmt->next`

---

## 12. PUBLIC API (Part 4)

### nova_compile()

1. Create top-level `NovaProto` with `nova_proto_create()`
2. Set `proto->source = source`, `proto->is_vararg = 1` (top-level is vararg)
3. Create `NovaCompiler` struct on stack
4. Create `NovaFuncState` struct on stack
5. Link: `fs.proto = proto`, `fs.compiler = &compiler`, `fs.parent = NULL`
6. Enter top-level scope
7. Compile root block: `novai_compile_block(&fs, ast->root)`
8. Emit `RETURN0` at end
9. Leave scope
10. Set `proto->max_stack = fs.free_reg` (high-water mark)
11. If `compiler.had_error`: destroy proto, return NULL
12. Return proto

### nova_compile_expression()

1. Like nova_compile() but:
   - Compile the single expression
   - Emit `RETURN1` with the result register
   - Return the proto

---

## 13. BINOP → OPCODE MAPPING TABLE

Used in Part 2 for binary expression compilation:

```c
static const NovaOpcode novai_binop_to_opcode[] = {
    [NOVA_BINOP_ADD]    = NOVA_OP_ADD,
    [NOVA_BINOP_SUB]    = NOVA_OP_SUB,
    [NOVA_BINOP_MUL]    = NOVA_OP_MUL,
    [NOVA_BINOP_DIV]    = NOVA_OP_DIV,
    [NOVA_BINOP_IDIV]   = NOVA_OP_IDIV,
    [NOVA_BINOP_MOD]    = NOVA_OP_MOD,
    [NOVA_BINOP_POW]    = NOVA_OP_POW,
    [NOVA_BINOP_CONCAT] = NOVA_OP_CONCAT,
    [NOVA_BINOP_BAND]   = NOVA_OP_BAND,
    [NOVA_BINOP_BOR]    = NOVA_OP_BOR,
    [NOVA_BINOP_BXOR]   = NOVA_OP_BXOR,
    [NOVA_BINOP_SHL]    = NOVA_OP_SHL,
    [NOVA_BINOP_SHR]    = NOVA_OP_SHR,
};
```

Comparison ops and logical ops are handled separately (they produce jumps, not register values).

---

## 14. AST TYPE REFERENCE (quick lookup)

### NovaExprType (23 variants from nova_ast.h):
```
NOVA_EXPR_NIL, NOVA_EXPR_TRUE, NOVA_EXPR_FALSE,
NOVA_EXPR_INTEGER, NOVA_EXPR_NUMBER, NOVA_EXPR_STRING,
NOVA_EXPR_VARARG, NOVA_EXPR_NAME,
NOVA_EXPR_INDEX, NOVA_EXPR_FIELD, NOVA_EXPR_METHOD,
NOVA_EXPR_UNARY, NOVA_EXPR_BINARY,
NOVA_EXPR_CALL, NOVA_EXPR_METHOD_CALL,
NOVA_EXPR_TABLE, NOVA_EXPR_FUNCTION,
NOVA_EXPR_INTERP_STRING,
NOVA_EXPR_AWAIT, NOVA_EXPR_SPAWN, NOVA_EXPR_YIELD,
NOVA_EXPR_COUNT
```

### NovaStmtType (19 variants from nova_ast.h):
```
NOVA_STMT_EXPR, NOVA_STMT_LOCAL, NOVA_STMT_ASSIGN,
NOVA_STMT_IF, NOVA_STMT_WHILE, NOVA_STMT_REPEAT,
NOVA_STMT_FOR_NUMERIC, NOVA_STMT_FOR_GENERIC,
NOVA_STMT_DO, NOVA_STMT_BREAK,
NOVA_STMT_GOTO, NOVA_STMT_LABEL,
NOVA_STMT_RETURN,
NOVA_STMT_FUNCTION, NOVA_STMT_LOCAL_FUNCTION,
NOVA_STMT_IMPORT, NOVA_STMT_EXPORT, NOVA_STMT_CONST,
NOVA_STMT_COUNT
```

### Row AST access patterns:
```c
NovaRowExpr  *expr = nova_get_expr(AST(fs), expr_idx);
NovaRowStmt  *stmt = nova_get_stmt(AST(fs), stmt_idx);
NovaRowBlock *blk  = nova_get_block(AST(fs), block_idx);
NovaExprIdx   arg  = nova_get_extra_expr(AST(fs), start, offset);
NovaRowTableField *fld = &AST(fs)->fields[field_idx];
NovaRowParam      *prm = &AST(fs)->params[param_idx];
NovaRowIfBranch   *br  = &AST(fs)->branches[branch_idx];
NovaRowNameRef    *nm  = &AST(fs)->names[name_idx];
```

### NovaUnOp → opcode:
```
NOVA_UNOP_NEGATE → NOVA_OP_UNM
NOVA_UNOP_NOT    → NOVA_OP_NOT
NOVA_UNOP_LEN    → NOVA_OP_STRLEN
NOVA_UNOP_BNOT   → NOVA_OP_BNOT
```

---

## 15. KEY CONSTANTS AND LIMITS

```c
NOVA_MAX_LOCALS        = 200    (from nova_conf.h)
NOVA_MAX_SCOPE_DEPTH   = 64     (from nova_compile.h)
NOVA_MAX_REGISTERS     = 250    (from nova_conf.h)
NOVA_MAX_UPVALUES      = 64     (from nova_conf.h)
NOVA_RK_CONST_BASE     = 250    (from nova_opcode.h)
NOVA_MAX_RK_CONST      = 5      (255 - 250)
NOVA_SBX_BIAS          = 0x7FFF (from nova_opcode.h)
NOVA_NO_JUMP           = UINT32_MAX (from nova_compile.h)
NOVA_IDX_NONE          = 0xFFFFFFFF (from nova_ast_row.h)
```

---

## 16. IMPLEMENTATION NOTES

1. **Const cast for AST access**: The `nova_get_expr/stmt/block` helpers take non-const `NovaASTTable*`, but our compiler has `const NovaASTTable*`. Since we only read, cast away const in the `AST()` macro.

2. **Line numbers**: Always pass `expr->loc.line` or `stmt->loc.line` to emission functions. The `line` field in NovaSourceLoc is `int` (from nova_lex.h), cast to `uint32_t` for proto API.

3. **CONCAT special case**: String concatenation chains `a .. b .. c` should emit `CONCAT base, count` covering the full range of registers, not individual `CONCAT` per pair.

4. **Multi-return positions**: The last expression in a function call or return statement can be "open" (returning all values). We handle this by setting the C field = 0 in CALL/VARARG instructions.

5. **Goto/Labels**: Stubbed out in initial implementation. Full goto support requires a label table and forward-reference resolution pass, which we'll add later.

6. **Error recovery**: On error, set `fs->compiler->had_error = 1` and continue compiling to catch more errors. The final nova_compile() checks `had_error` and returns NULL if set.

---

**END OF BLUEPRINT**
