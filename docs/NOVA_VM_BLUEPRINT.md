# Nova VM Blueprint

**Purpose**: Register-based bytecode virtual machine.
**Files**: `include/nova/nova_vm.h`, `src/nova_vm.c`
**Dependencies**: nova_opcode.h, nova_proto.h, nova_conf.h, nova_codegen.h, zorya/nxh.h

---

## 1. ARCHITECTURAL DECISIONS

### 1.1 Value Representation (NovaValue)

**Tagged union approach first** (not NaN-boxing yet):
- Simpler to debug, easier to get right
- NaN-boxing is a v0.2 optimization (NOVA_NAN_BOXING flag exists in conf.h)
- Tagged union: `{ NovaValueType type; union { nova_int_t integer; nova_number_t number; int boolean; void *pointer; } as; }`

NovaValueType enum: NIL=0, BOOL=1, INTEGER=2, NUMBER=3, STRING=4, TABLE=5, FUNCTION=6, CLOSURE=7, CFUNCTION=8, USERDATA=9, COROUTINE=10

### 1.2 String Object (NovaString)

For now, simple heap-allocated struct:
- `{ size_t length; uint64_t hash; char data[]; }` (flexible array member)
- Hash computed once at creation via NXH64
- Weave integration is a v0.2 enhancement

### 1.3 Table (NovaTable)

Minimal implementation for Phase 7:
- Array part: `NovaValue *array; uint32_t array_size;`
- Hash part: `{ NovaValue key; NovaValue value; } *hash; uint32_t hash_size; uint32_t hash_used;`
- DAGGER integration is v0.2
- Basic linear probing for hash part (simple, correct)

### 1.4 Closure (NovaClosure)

- `{ NovaProto *proto; NovaUpvalue **upvalues; uint8_t upvalue_count; }`
- C function closure: `{ nova_cfunc_t fn; NovaValue *upvalues; uint8_t upvalue_count; }`

### 1.5 Upvalue (NovaUpvalue)

Open/closed dual state:
- `{ NovaValue *location; NovaValue closed; NovaUpvalue *next; }`
- While local is on stack: location = &stack[reg]
- After local goes out of scope: location = &closed, copy value

---

## 2. CALL FRAME

```c
typedef struct {
    const NovaProto *proto;       /* Function being executed         */
    const NovaInstruction *ip;    /* Instruction pointer             */
    NovaValue *base;              /* Base of this frame's registers  */
    NovaClosure *closure;         /* Closure (for upvalue access)    */
    int num_results;              /* Expected results from caller    */
    uint8_t is_nova;              /* 1=Nova function, 0=C function   */
} NovaCallFrame;
```

Call stack: fixed array `NovaCallFrame frames[NOVA_MAX_CALL_DEPTH]`

---

## 3. VM STATE (NovaVM)

```c
typedef struct NovaVM {
    /* Register/value stack */
    NovaValue *stack;             /* Growable value stack             */
    NovaValue *stack_top;         /* First free slot                  */
    size_t     stack_size;        /* Allocated stack size             */
    
    /* Call frames */
    NovaCallFrame frames[NOVA_MAX_CALL_DEPTH];
    int           frame_count;
    
    /* Global environment */
    NovaTable *globals;
    
    /* Open upvalue list (linked, sorted by stack position) */
    NovaUpvalue *open_upvalues;
    
    /* GC roots (placeholder for Phase 8) */
    /* ... */
    
    /* Error state */
    int         status;           /* NOVA_VM_OK, etc. */
    char       *error_msg;        /* Heap-allocated error message     */
    
    /* Allocation stats */
    size_t bytes_allocated;
} NovaVM;
```

---

## 4. PUBLIC API

```c
/* Lifecycle */
NovaVM *nova_vm_create(void);
void    nova_vm_destroy(NovaVM *vm);

/* Execution */
int nova_vm_execute(NovaVM *vm, const NovaProto *proto);
int nova_vm_call(NovaVM *vm, int nargs, int nresults);

/* Stack operations (for C API layer) */
void        nova_vm_push_nil(NovaVM *vm);
void        nova_vm_push_bool(NovaVM *vm, int b);
void        nova_vm_push_integer(NovaVM *vm, nova_int_t val);
void        nova_vm_push_number(NovaVM *vm, nova_number_t val);
void        nova_vm_push_string(NovaVM *vm, const char *s, size_t len);
void        nova_vm_push_cfunction(NovaVM *vm, nova_cfunc_t fn);
NovaValue   nova_vm_get(NovaVM *vm, int idx);
int         nova_vm_get_top(NovaVM *vm);
void        nova_vm_set_top(NovaVM *vm, int idx);
void        nova_vm_pop(NovaVM *vm, int n);

/* Global table */
void nova_vm_set_global(NovaVM *vm, const char *name, NovaValue val);
NovaValue nova_vm_get_global(NovaVM *vm, const char *name);

/* Type queries */
const char *nova_vm_typename(NovaValueType type);

/* Error */
const char *nova_vm_error(const NovaVM *vm);
```

---

## 5. DISPATCH STRATEGY

### Computed Goto (GCC/Clang, NOVA_COMPUTED_GOTO=1)

```c
static void *dispatch_table[256] = {
    [NOVA_OP_MOVE]    = &&op_MOVE,
    [NOVA_OP_LOADK]   = &&op_LOADK,
    ...
};

#define DISPATCH() goto *dispatch_table[NOVA_GET_OPCODE(*ip)]
#define CASE(op)   op_##op:
#define NEXT()     ip++; DISPATCH()
```

### Switch Fallback (NOVA_COMPUTED_GOTO=0)

```c
#define DISPATCH() continue
#define CASE(op)   case NOVA_OP_##op:
#define NEXT()     ip++; continue
```

Both share the same handler bodies.

---

## 6. OPCODE HANDLER OUTLINE

### Data Movement
- MOVE: `R(A) = R(B)` — simple value copy
- LOADK: `R(A) = K(Bx)` — load constant from pool
- LOADNIL: `R(A..A+B) = nil` — nil a range
- LOADBOOL: `R(A) = (bool)B; if C then pc++`
- LOADINT: `R(A) = sBx` — small integer literal
- LOADKX: `R(A) = K(extra_arg)` — extended constant load

### Table Operations
- NEWTABLE: allocate new table with size hints
- GETTABLE/SETTABLE: generic index/newindex
- GETFIELD/SETFIELD: string-key fast path
- GETI/SETI: integer-key fast path
- SETLIST: bulk array init from registers
- SELF: method call setup

### Upvalue/Global
- GETUPVAL/SETUPVAL: access closure upvalues
- GETGLOBAL/SETGLOBAL: access global table by string key
- GETTABUP/SETTABUP: access upvalue table field

### Arithmetic
- ADD/SUB/MUL/DIV/IDIV/MOD/POW/UNM
- ADDI: immediate add
- ADDK/SUBK/MULK/DIVK/MODK: constant operand variants
- Integer fast path, number fallback, metamethod fallback

### Bitwise
- BAND/BOR/BXOR/BNOT/SHL/SHR — integer only

### String
- CONCAT: build from register range
- STRLEN: string length

### Comparison
- EQ/LT/LE + EQK/EQI/LTI/LEI/GTI/GEI
- All set/skip pattern: test, skip next instruction if false

### Logical
- NOT: boolean negation
- TEST: conditional skip on truthiness
- TESTSET: conditional move + skip

### Control Flow
- JMP: unconditional jump + close upvalues
- CALL: function call with variable args/results
- TAILCALL: tail call optimization
- RETURN/RETURN0/RETURN1: function return variants

### Loops
- FORPREP/FORLOOP: numeric for loop
- TFORCALL/TFORLOOP: generic for loop (iterator)

### Closures/Varargs
- CLOSURE: create closure from sub-proto
- VARARG: load varargs into registers
- VARARGPREP: adjust vararg storage
- CLOSE: close upvalues

### Async (Stubs for Phase 7)
- AWAIT/SPAWN/YIELD: set NOVA_VM_ERR_RUNTIME with "not yet implemented"

### Module (Stubs for Phase 7)
- IMPORT/EXPORT: same treatment

### Special
- NOP: no-op
- DEBUG: debug hook placeholder
- EXTRAARG: consumed by LOADKX

---

## 7. RK RESOLUTION

For opcodes with RK-mode operands (B or C can be register or constant):

```c
static inline NovaValue novai_rk(NovaValue *base, const NovaConstant *k,
                                  uint8_t rk) {
    if (NOVA_IS_RK_CONST(rk)) {
        return novai_const_to_value(&k[NOVA_RK_TO_CONST(rk)]);
    }
    return base[rk];
}
```

Need `novai_const_to_value()` to convert NovaConstant → NovaValue.

---

## 8. ERROR HANDLING

- VM sets `vm->status` and `vm->error_msg` on runtime errors
- Does NOT use longjmp/setjmp in v0.1 (keep it simple)
- Error in handler → set status, break out of dispatch loop
- `nova_vm_execute()` returns status code

Status codes:
- NOVA_VM_OK = 0
- NOVA_VM_ERR_RUNTIME = 1
- NOVA_VM_ERR_MEMORY = 2
- NOVA_VM_ERR_STACKOVERFLOW = 3
- NOVA_VM_ERR_TYPE = 4
- NOVA_VM_ERR_DIVZERO = 5
- NOVA_VM_ERR_NULLPTR = 6

---

## 9. REPL CONSIDERATIONS

Per user guidance: **keep REPL non-binding and simple**.
- REPL is NOT in nova_vm.c — it belongs in a separate `nova_repl.c` or in the main driver
- VM just provides `nova_vm_execute()` — REPL calls this in a loop
- No readline dependency, no complex state machine
- REPL support = "execute a proto, print result, repeat"
- Can be added in Phase 14 without touching VM code

---

## 10. FILE STRUCTURE ESTIMATE

- nova_vm.h: ~200 lines (types, enums, API)
- nova_vm.c: ~2000-2500 lines
  - Part 0: File header, includes (30 lines)
  - Part 1: NovaValue helpers + NovaString + NovaTable basics (200 lines)
  - Part 2: Stack management helpers (100 lines)
  - Part 3: Table implementation (create, get, set, next) (250 lines)
  - Part 4: Upvalue management (open, close, find) (100 lines)
  - Part 5: Closure creation (50 lines)
  - Part 6: RK resolution + constant conversion (50 lines)
  - Part 7: Arithmetic helpers (int/float dispatch) (200 lines)
  - Part 8: Comparison helpers (100 lines)
  - Part 9: CALL/RETURN mechanics (200 lines)
  - Part 10: Main dispatch loop (all opcode handlers) (800 lines)
  - Part 11: Public API (create, destroy, execute, stack ops) (300 lines)
  - Part 12: Error + type name utilities (50 lines)

---

## 11. KEY DESIGN PRINCIPLES

1. **Correctness first**: Get every opcode right before optimizing anything
2. **No GC yet**: malloc/free only. GC is Phase 8
3. **No metatables yet**: Table ops are direct only. Metamethods are Phase 9
4. **No string interning**: Strings are heap-allocated, compared by content
5. **Stubs over crashes**: Unimplemented features return errors, not segfaults
6. **REPL is separate**: VM is embeddable, REPL wraps it
7. **Computed goto where available**: But switch fallback works identically

---

**ZORYA CORPORATION - Engineering Excellence, Democratized**
