<!-- ============================================================
     NOVA BYTECODE SPECIFICATION
     Instruction Set Architecture and Binary Format

     Author:  Anthony Taliento (Zorya Corporation)
     Date:    2026-03-13
     Version: 0.2.0
     Status:  RATIFIED
     ============================================================ -->

# Nova Bytecode Specification

This document defines Nova's instruction set architecture (ISA) and the
`.no` binary bytecode format. It serves as the contract between the
compiler, optimizer, codegen, VM, disassembler, and debugger — every
component that reads or writes bytecode agrees on exactly what each
opcode number means and how instructions are encoded.

Opcode assignments are **permanent**. Once a number is assigned to an
opcode, it never changes meaning across versions. New opcodes are
appended, never inserted. Retired opcodes become reserved gaps. This
stability is what makes the `.no` binary format forward-compatible and
enables deterministic computed-goto dispatch tables.

For the virtual machine's execution model, value representation, and
garbage collector, see the companion [VM Specification](NOVA_VM_SPEC.md).

---

## Table of Contents

1. [Instruction Encoding](#1-instruction-encoding)
2. [Operand Modes](#2-operand-modes)
3. [Instruction Set Reference](#3-instruction-set-reference)
4. [Function Prototypes](#4-function-prototypes)
5. [The .no Binary Format](#5-the-no-binary-format)
6. [Opcode Space Map](#6-opcode-space-map)

---

## 1. Instruction Encoding

Every Nova instruction is a fixed-width 32-bit word stored in
little-endian byte order. The high 8 bits encode the opcode; the
remaining 24 bits carry operands. Fixed-width instructions simplify
fetch and decode — the VM reads exactly 4 bytes per cycle with no
length prefix or variable-length decoding.

### 1.1 Instruction Formats

The 24 operand bits are divided differently depending on the opcode's
format:

```
 31       24 23       16 15        8 7         0
 +----------+----------+----------+----------+
 | Opcode   |    A     |    B     |    C     |  ABC    Three 8-bit fields
 +----------+----------+----------+----------+
 | Opcode   |    A     |       Bx (16-bit)   |  ABx   8-bit + 16-bit unsigned
 +----------+----------+----------+----------+
 | Opcode   |    A     |       sBx (signed)  |  AsBx  8-bit + 16-bit signed
 +----------+----------+----------+----------+
 | Opcode   |          Ax (24-bit)           |  Ax    24-bit unsigned
 +----------+----------+----------+----------+
```

| Format | Fields | Typical use |
|--------|--------|-------------|
| **ABC** | A (8), B (8), C (8) | Arithmetic, table ops, comparisons |
| **ABx** | A (8), Bx (16 unsigned) | Constant loading, global access |
| **AsBx** | A (8), sBx (16 signed) | Jumps, loop control |
| **Ax** | Ax (24 unsigned) | Extended arguments |
| **NONE** | — | Zero-operand instructions (RETURN0, NOP) |

**Signed encoding (sBx):** The Bx field stores an unsigned 16-bit
value. To represent signed offsets, a bias of 32,767 (`0x7FFF`) is
added: `sBx = Bx - 32767`. This gives a range of −32,767 to +32,768,
sufficient for any jump within a single function.

**Field extraction** is pure bit manipulation — shift and mask, no
branches:

```c
#define NOVA_GET_OPCODE(i)  ((i) >> 24) & 0xFF)
#define NOVA_GET_A(i)       ((i) >> 16) & 0xFF)
#define NOVA_GET_B(i)       ((i) >> 8)  & 0xFF)
#define NOVA_GET_C(i)       ((i)        & 0xFF)
#define NOVA_GET_BX(i)      ((i)        & 0xFFFF)
#define NOVA_GET_SBX(i)     (NOVA_GET_BX(i) - 0x7FFF)
#define NOVA_GET_AX(i)      ((i)        & 0xFFFFFF)
```

### 1.2 Field Ranges

| Field | Bits | Unsigned range | Signed range |
|-------|------|----------------|--------------|
| Opcode | 8 | 0–255 | — |
| A | 8 | 0–255 | — |
| B | 8 | 0–255 | — |
| C | 8 | 0–255 | — |
| Bx | 16 | 0–65,535 | — |
| sBx | 16 | — | −32,767 to +32,768 |
| Ax | 24 | 0–16,777,215 | — |

---

## 2. Operand Modes

Each opcode's B and C fields are interpreted according to an operand
mode. The mode determines whether the field references a register, a
constant, or a raw integer.

| Mode | Code | Meaning | Example |
|------|------|---------|---------|
| UNUSED | 0 | Field not used | RETURN0 |
| REG | 1 | Register index | `R(B)` |
| CONST | 2 | Constant pool index | `K(Bx)` |
| RK | 3 | Register or constant | `RK(C)` — see below |
| INT | 4 | Raw integer value | `LOADBOOL` C field |
| JUMP | 5 | Jump offset (signed) | `JMP` sBx |

### 2.1 Register-or-Constant (RK) Encoding

Several instructions accept either a register reference or a constant
pool index in their B or C field. The 8-bit field is split:

- **0–127:** Register index. `R(field)`.
- **128–255:** Constant index. `K(field - 128)`.

The high bit acts as a discriminator. At runtime:

```c
if (field >= 128) {
    value = constants[field - 128];
} else {
    value = registers[field];
}
```

This allows three-operand instructions like `ADD R3, R1, K5` (add
register 1 and constant 5, store in register 3) to encode in a single
32-bit word. The RK encoding limits each field to 128 registers and
128 fast-path constants. For larger constant pools, `LOADK` uses the
16-bit Bx field (up to 65,536 constants), and `LOADKX` extends to 24
bits via `EXTRAARG`.

---

## 3. Instruction Set Reference

Nova v0.2.0 defines 46 opcodes organized into ten categories. The
opcode byte determines the instruction's identity; the format and
operand modes determine how its fields are decoded.

The notation used below:

- `R(x)` — Register at index x
- `K(x)` — Constant at pool index x
- `RK(x)` — Register or constant (RK encoding)
- `U(x)` — Upvalue at index x
- `G[k]` — Global variable with string key k
- `pc` — Program counter (instruction index)

### 3.1 Data Movement

These instructions load values into registers.

| Opcode | Hex | Format | Operation | Description |
|--------|-----|--------|-----------|-------------|
| MOVE | 0x00 | ABC | `R(A) = R(B)` | Copy register B to register A |
| LOADK | 0x01 | ABx | `R(A) = K(Bx)` | Load constant Bx into register A |
| LOADNIL | 0x02 | ABC | `R(A)..R(A+B) = nil` | Set B+1 consecutive registers to nil |
| LOADBOOL | 0x03 | ABC | `R(A) = (bool)B; if C then pc++` | Load boolean; optionally skip next instruction |
| LOADINT | 0x04 | AsBx | `R(A) = sBx` | Load small signed integer literal |
| LOADKX | 0x05 | A | `R(A) = K(extra)` | Load constant using next EXTRAARG instruction |

`LOADINT` avoids a constant pool lookup for small integers (−32,767 to
+32,768) — a fast path for loop counters and array indices. `LOADKX` is
the escape hatch for constant pools larger than 65,536 entries: the
24-bit Ax field of the following `EXTRAARG` instruction provides the
constant index.

### 3.2 Table Operations

These instructions create, read, and write tables.

| Opcode | Hex | Format | Operation | Description |
|--------|-----|--------|-----------|-------------|
| NEWTABLE | 0x10 | ABC | `R(A) = {}` | Create table (B=array hint, C=hash hint) |
| GETTABLE | 0x11 | ABC | `R(A) = R(B)[RK(C)]` | Generic table read |
| SETTABLE | 0x12 | ABC | `R(A)[RK(B)] = RK(C)` | Generic table write |
| GETFIELD | 0x13 | ABC | `R(A) = R(B)[K(C)]` | Read with string constant key |
| SETFIELD | 0x14 | ABC | `R(A)[K(B)] = RK(C)` | Write with string constant key |
| GETI | 0x15 | ABC | `R(A) = R(B)[C]` | Read with integer key |
| SETI | 0x16 | ABC | `R(A)[B] = RK(C)` | Write with integer key |
| SETLIST | 0x17 | ABC | `R(A)[(C-1)*FPF+i] = R(A+i)` | Batch-set array elements |
| SELF | 0x18 | ABC | `R(A+1)=R(B); R(A)=R(B)[RK(C)]` | Method lookup (self call) |

`NEWTABLE` preallocates array and hash parts according to the B and C
hints, avoiding early resizes when the compiler can predict table size.

`SELF` is the method call instruction: it saves the receiver in
`R(A+1)` (as the implicit first argument) and looks up the method name
in `R(A)`. This enables `obj:method(args)` syntax with a single
instruction instead of separate GETTABLE + MOVE.

`SETLIST` initializes table array elements in batches. The
fields-per-flush constant (50) determines how many registers are copied
per SETLIST instruction. For table constructors with more than 50
elements, the compiler emits multiple SETLIST instructions with
increasing C values.

### 3.3 Upvalue and Global Access

| Opcode | Hex | Format | Operation | Description |
|--------|-----|--------|-----------|-------------|
| GETUPVAL | 0x20 | ABC | `R(A) = U(B)` | Read upvalue B |
| SETUPVAL | 0x21 | ABC | `U(B) = R(A)` | Write upvalue B |
| GETGLOBAL | 0x22 | ABx | `R(A) = G[K(Bx)]` | Read global variable |
| SETGLOBAL | 0x23 | ABx | `G[K(Bx)] = R(A)` | Write global variable |
| GETTABUP | 0x24 | ABC | `R(A) = U(B)[RK(C)]` | Read field from upvalue table |
| SETTABUP | 0x25 | ABC | `U(A)[RK(B)] = RK(C)` | Write field to upvalue table |

`GETGLOBAL`/`SETGLOBAL` use the Bx field as a constant pool index to
the variable name string. The VM looks up or stores the value in
`vm->globals`, a hash table.

`GETTABUP`/`SETTABUP` combine upvalue access with table indexing in a
single instruction — an optimization for accessing module-level tables
from nested functions.

### 3.4 Arithmetic

| Opcode | Hex | Format | Operation | Description |
|--------|-----|--------|-----------|-------------|
| ADD | 0x30 | ABC | `R(A) = R(B) + R(C)` | Addition |
| SUB | 0x31 | ABC | `R(A) = R(B) - R(C)` | Subtraction |
| MUL | 0x32 | ABC | `R(A) = R(B) * R(C)` | Multiplication |
| DIV | 0x33 | ABC | `R(A) = R(B) / R(C)` | Float division |
| IDIV | 0x34 | ABC | `R(A) = R(B) // R(C)` | Floor (integer) division |
| MOD | 0x35 | ABC | `R(A) = R(B) % R(C)` | Modulo |
| POW | 0x36 | ABC | `R(A) = R(B) ^ R(C)` | Exponentiation |
| UNM | 0x37 | ABC | `R(A) = -R(B)` | Unary negation |
| ADDI | 0x38 | AsBx | `R(A) = R(A) + sBx` | Add signed immediate |
| ADDK | 0x39 | ABC | `R(A) = R(B) + K(C)` | Add constant |
| SUBK | 0x3A | ABC | `R(A) = R(B) - K(C)` | Subtract constant |
| MULK | 0x3B | ABC | `R(A) = R(B) * K(C)` | Multiply by constant |
| DIVK | 0x3C | ABC | `R(A) = R(B) / K(C)` | Divide by constant |
| MODK | 0x3D | ABC | `R(A) = R(B) % K(C)` | Modulo by constant |

The immediate and constant variants eliminate the RK branch on hot
paths. `ADDI` in particular is used for loop counter increments — it
modifies a register in place with a signed 16-bit immediate, requiring
no constant pool entry and no source register read.

All arithmetic instructions trigger metamethods (`__add`, `__sub`,
etc.) when operands don't support the operation natively.

### 3.5 Bitwise

| Opcode | Hex | Format | Operation | Description |
|--------|-----|--------|-----------|-------------|
| BAND | 0x40 | ABC | `R(A) = R(B) & R(C)` | Bitwise AND |
| BOR | 0x41 | ABC | `R(A) = R(B) \| R(C)` | Bitwise OR |
| BXOR | 0x42 | ABC | `R(A) = R(B) ~ R(C)` | Bitwise XOR |
| BNOT | 0x43 | ABC | `R(A) = ~R(B)` | Bitwise NOT |
| SHL | 0x44 | ABC | `R(A) = R(B) << R(C)` | Shift left |
| SHR | 0x45 | ABC | `R(A) = R(B) >> R(C)` | Shift right |

Bitwise operations require integer operands. Non-integer values raise
a type error or trigger the corresponding metamethod (`__band`,
`__bor`, etc.).

### 3.6 String

| Opcode | Hex | Format | Operation | Description |
|--------|-----|--------|-----------|-------------|
| CONCAT | 0x50 | ABC | `R(A) = R(A) .. ... .. R(A+B)` | Concatenate B+1 values |
| STRLEN | 0x51 | ABC | `R(A) = #R(B)` | String length |

`CONCAT` joins a contiguous range of registers. The compiler arranges
concatenation operands in adjacent registers so a single instruction
handles chains like `a .. b .. c .. d`.

### 3.7 Comparison

Comparison instructions are conditional: they test a condition and skip
the next instruction if the test fails. The skipped instruction is
typically a `JMP` to the branch target. The A field inverts the
condition (0 = test for true, 1 = test for false).

| Opcode | Hex | Format | Operation | Description |
|--------|-----|--------|-----------|-------------|
| EQ | 0x58 | ABC | `if (R(B) == R(C)) ~= A then pc++` | Equality test |
| LT | 0x59 | ABC | `if (R(B) < R(C)) ~= A then pc++` | Less-than test |
| LE | 0x5A | ABC | `if (R(B) <= R(C)) ~= A then pc++` | Less-or-equal test |
| EQK | 0x5B | ABC | `if (R(B) == K(C)) ~= A then pc++` | Equality with constant |
| EQI | 0x5C | AsBx | `if (R(A) == sBx) then pc++` | Equality with integer |
| LTI | 0x5D | AsBx | `if (R(A) < sBx) then pc++` | Less-than integer |
| LEI | 0x5E | AsBx | `if (R(A) <= sBx) then pc++` | Less-or-equal integer |
| GTI | 0x5F | AsBx | `if (R(A) > sBx) then pc++` | Greater-than integer |
| GEI | 0x60 | AsBx | `if (R(A) >= sBx) then pc++` | Greater-or-equal integer |

The integer immediate variants (`EQI`, `LTI`, `LEI`, `GTI`, `GEI`)
optimize comparisons against small integer constants — the single most
common comparison pattern in loops and conditionals. They avoid a
constant pool lookup entirely.

### 3.8 Logical

| Opcode | Hex | Format | Operation | Description |
|--------|-----|--------|-----------|-------------|
| NOT | 0x68 | ABC | `R(A) = not R(B)` | Logical negation |
| TEST | 0x69 | ABC | `if (bool)R(A) ~= C then pc++` | Boolean test |
| TESTSET | 0x6A | ABC | `if (bool)R(B) ~= C then R(A)=R(B) else pc++` | Conditional assignment |

`TEST` implements short-circuit evaluation for `and`/`or`: test the
value's truthiness (nil and false are falsy, everything else is truthy)
and skip the next instruction if the test fails.

`TESTSET` is the short-circuit assignment variant: `dec x = a or b`
compiles to a TESTSET that either assigns `a` to `x` (if truthy) or
falls through to evaluate `b`.

### 3.9 Control Flow

| Opcode | Hex | Format | Operation | Description |
|--------|-----|--------|-----------|-------------|
| JMP | 0x70 | AsBx | `pc += sBx; close upvalues >= A` | Unconditional jump |
| CALL | 0x71 | ABC | (see VM spec) | Function call |
| TAILCALL | 0x72 | ABC | (see VM spec) | Tail call (reuses frame) |
| RETURN | 0x73 | ABC | `return R(A), ..., R(A+B-2)` | Return values |
| RETURN0 | 0x74 | NONE | `return` | Return (no values) |
| RETURN1 | 0x75 | A | `return R(A)` | Return single value |

`JMP` also closes upvalues: if the A field is nonzero, all open
upvalues at register A or above are closed before the jump. This
handles block scoping — when control flow exits a block, locals declared
in that block must have their upvalues migrated to the heap.

`RETURN0` and `RETURN1` are specialized fast paths. `RETURN0` has no
operands at all — a single opcode byte. `RETURN1` reads one register.
These avoid the loop that generic `RETURN` uses to copy variable numbers
of return values.

The `CALL` and `TAILCALL` instructions are documented in detail in the
[VM Specification](NOVA_VM_SPEC.md#25-function-calls).

### 3.10 Loops

| Opcode | Hex | Format | Operation | Description |
|--------|-----|--------|-----------|-------------|
| FORPREP | 0x80 | AsBx | `R(A) -= R(A+2); pc += sBx` | Initialize numeric for-loop |
| FORLOOP | 0x81 | AsBx | (see below) | Numeric for-loop step |
| TFORCALL | 0x82 | ABC | (see below) | Generic for-loop iterator call |
| TFORLOOP | 0x83 | AsBx | (see below) | Generic for-loop continuation |

**Numeric for-loop** (`for i = start, stop, step`):

The compiler allocates four consecutive registers:
- `R(A)` — internal counter
- `R(A+1)` — limit
- `R(A+2)` — step
- `R(A+3)` — exposed loop variable (`i`)

`FORPREP` subtracts the step from the counter (undoing the first
iteration's add) and jumps forward to `FORLOOP`. `FORLOOP` adds the
step, checks whether the counter exceeds the limit, and either assigns
`R(A+3) = R(A)` and jumps back to the loop body, or falls through to
exit.

This two-instruction loop structure executes the step, bounds check,
and back-edge jump in a single instruction — the tightest possible loop
for numeric iteration.

**Generic for-loop** (`for k, v in iterator`):

`TFORCALL` calls the iterator function with its state arguments and
stores the results. `TFORLOOP` checks whether the first result is nil
(end of iteration) and either continues the loop or exits.

### 3.11 Closures and Varargs

| Opcode | Hex | Format | Operation | Description |
|--------|-----|--------|-----------|-------------|
| CLOSURE | 0x90 | ABx | `R(A) = closure(proto[Bx])` | Create closure from sub-prototype |
| VARARG | 0x91 | ABC | `R(A), ..., R(A+C-2) = vararg` | Load variadic arguments |
| VARARGPREP | 0x92 | A | — | Adjust vararg parameters |
| CLOSE | 0x93 | A | `close upvalues >= R(A)` | Close upvalues without jumping |

`CLOSURE` creates a new closure from the Bx-th sub-prototype of the
current function's prototype. Upvalues are resolved using the
sub-prototype's upvalue descriptors (see
[VM Specification](NOVA_VM_SPEC.md#52-upvalue-descriptors)):
`in_stack=1` captures a dec from the current frame, `in_stack=0`
copies an upvalue from the current closure.

`CLOSE` is emitted at block boundaries where upvalues must be closed
but no jump is needed (e.g., the end of a `do` block that falls
through). It performs the same upvalue closure as `JMP`'s A field but
without changing the program counter.

### 3.12 Async and Concurrency

| Opcode | Hex | Format | Operation | Description |
|--------|-----|--------|-----------|-------------|
| AWAIT | 0xA0 | ABC | `R(A) = await R(B)` | Await an async task |
| SPAWN | 0xA1 | ABC | `spawn R(A)(...)` | Spawn async task |
| YIELD | 0xA2 | ABC | `yield R(A), ..., R(A+B-1)` | Yield from coroutine/task |

These instructions interface with the VM's coroutine and async scheduler
(see [VM Specification](NOVA_VM_SPEC.md#8-async-concurrency)).

### 3.13 Module

| Opcode | Hex | Format | Operation | Description |
|--------|-----|--------|-----------|-------------|
| IMPORT | 0xB0 | ABx | `R(A) = import(K(Bx))` | Import module |
| EXPORT | 0xB1 | ABx | `export K(Bx) = R(A)` | Export value |

### 3.14 Special

| Opcode | Hex | Format | Operation | Description |
|--------|-----|--------|-----------|-------------|
| NOP | 0xF0 | NONE | — | No operation (padding/alignment) |
| DEBUG | 0xF1 | A | — | Debug breakpoint hook, level A |
| EXTRAARG | 0xFE | Ax | — | 24-bit extra argument for preceding instruction |
| SENTINEL | 0xFF | — | — | End-of-enum marker (never encoded) |

`EXTRAARG` extends the operand space of the preceding instruction. It
is emitted by `LOADKX` to provide a 24-bit constant index, supporting
pools of up to 16,777,215 constants.

---

## 4. Function Prototypes

A function prototype (`NovaProto`) is the compile-time representation
of a function. It holds everything needed to execute the function:
bytecode, constants, upvalue descriptors, and optional debug info. At
runtime, the `CLOSURE` instruction wraps a prototype in a closure
object that adds captured upvalue pointers.

### 4.1 Prototype Structure

```c
struct NovaProto {
    /* Bytecode */
    NovaInstruction *code;             /* Instruction array              */
    uint32_t         code_count;       /* Instruction count              */

    /* Constant pool */
    NovaConstant    *constants;        /* Constants array                */
    uint32_t         const_count;      /* Constant count                 */

    /* Sub-prototypes (nested functions) */
    struct NovaProto **protos;         /* Child prototypes               */
    uint32_t          proto_count;     /* Child count                    */

    /* Upvalue descriptors */
    NovaUpvalDesc   *upvalues;         /* Upvalue capture instructions   */
    uint8_t          upvalue_count;    /* Upvalue count (max 255)        */

    /* Debug info */
    NovaLocalInfo   *locals;           /* Local variable descriptors     */
    uint32_t         local_count;      /* Local descriptor count         */
    NovaLineInfo     lines;            /* pc → source line mapping       */
    const char      *source;           /* Source filename                 */

    /* Function metadata */
    uint8_t          num_params;       /* Fixed parameter count          */
    uint8_t          is_vararg;        /* 1 if function uses ...         */
    uint8_t          is_async;         /* 1 if async function            */
    uint8_t          max_stack;        /* Register window size           */
    uint32_t         line_defined;     /* Source line (start)            */
    uint32_t         last_line;        /* Source line (end)              */
};
```

### 4.2 Constant Pool

Each constant is a tagged value:

| Tag | Value | Encoding | Size |
|-----|-------|----------|------|
| `CONST_NIL` | 0 | No payload | 0 bytes |
| `CONST_BOOL` | 1 | 1 byte (0 or 1) | 1 byte |
| `CONST_INTEGER` | 2 | `int64_t` | 8 bytes |
| `CONST_NUMBER` | 3 | `double` (IEEE 754) | 8 bytes |
| `CONST_STRING` | 4 | Length-prefixed bytes | variable |

The compiler deduplicates constants: before adding a string constant to
the pool, it searches for an existing entry with the same content. This
keeps pool sizes small and reduces binary file size.

The maximum pool size is 65,536 entries (16-bit `Bx` field in `LOADK`).
With `LOADKX` and `EXTRAARG`, the effective limit extends to 16,777,215.

### 4.3 Upvalue Descriptors

Each descriptor tells the `CLOSURE` instruction how to capture a
variable:

```c
typedef struct {
    uint8_t index;      /* Register or upvalue index in parent     */
    uint8_t in_stack;   /* 1 = parent's dec, 0 = parent's upvalue */
} NovaUpvalDesc;
```

A chain of `in_stack=0` references allows a closure to capture
variables from any ancestor scope, with each link resolving through
the parent closure's own upvalue array.

### 4.4 Debug Information

**Line numbers:** A flat array mapping each instruction index (program
counter) to its source line number. `lines.line_numbers[pc]` gives the
source line for instruction `pc` in O(1).

**Local variables:** Each entry records a variable's name, the register
it occupies, and the PC range where it is active (start_pc to end_pc).
Used by the debugger and in error messages.

**Upvalue names:** String names for each upvalue, used in debug
tracebacks. Optional and may be NULL.

### 4.5 Sub-Prototypes

Function definitions nested inside a function are compiled as
sub-prototypes stored in the parent's `protos[]` array. The `CLOSURE`
instruction references sub-prototypes by index.

The prototype tree is depth-first: a script's top-level code is the
root prototype, and each function definition within it adds a child.
Nested functions add children to those children, forming a tree that
mirrors the source code's lexical nesting.

---

## 5. The .no Binary Format

Nova's bytecode serialization format (`.no` file extension) stores a
compiled prototype tree as a single portable binary. The format is
designed for fast deserialization: fields are naturally aligned, integers
are little-endian, and the structure directly mirrors the in-memory
prototype layout.

### 5.1 File Layout

```
Offset    Content
──────    ──────────────────────────────────
0x00      File Header (28 bytes)
0x1C      Root Prototype (variable length, recursive)
...       EOF Marker (4 bytes)
```

### 5.2 File Header

The header is 28 bytes:

```
Offset  Size  Field           Type       Endianness  Description
──────  ────  ─────           ────       ──────────  ────────────────────
0x00    4     Magic           uint32     BE (ASCII)  0x4E4F5641 ("NOVA")
0x04    1     Version Major   uint8      —           Format version major
0x05    1     Version Minor   uint8      —           Format version minor
0x06    2     Flags           uint16     LE          See flag table
0x08    4     Platform Tag    uint32     LE          0x00000000 (portable)
0x0C    8     Timestamp       uint64     LE          Unix time (informational)
0x14    8     Checksum        uint64     LE          NXH64 over payload
```

**Magic number:** The four bytes `0x4E, 0x4F, 0x56, 0x41` spell "NOVA"
in ASCII. This serves as a format identifier — tools can detect Nova
bytecode by reading the first four bytes.

**Version:** Major=1, Minor=0 in v0.2.0 of the runtime. Major version
mismatches are rejected (different instruction semantics). Minor version
mismatches are accepted (backward compatible).

**Flags:**

| Flag | Value | Description |
|------|-------|-------------|
| `NOVA_CODEGEN_FLAG_DEBUG` | `0x0002` | Debug info included |
| `NOVA_CODEGEN_FLAG_STRIP` | `0x0004` | Debug info stripped |

**Platform tag:** Always `0x00000000` (portable). The format contains
no platform-specific encodings.

**Timestamp:** Records `time(NULL)` at serialization time. Informational
only — not used for validation.

**Checksum:** NXH64 hash of the entire payload (everything after the
header) using the magic number as seed (`0x4E4F5641`). Computed after
serialization and patched into the header. Detects corruption and
truncation.

### 5.3 EOF Marker

The final 4 bytes of the file are `0xDEAD4E56` (big-endian). This
sentinel detects truncated files: if the last 4 bytes don't match, the
file is incomplete.

### 5.4 Prototype Serialization

Each prototype is serialized recursively in depth-first order:

```
[Metadata]       12 bytes
[Source Name]     2 + N bytes
[Instructions]   4 + (4 × code_count) bytes
[Constants]      4 + variable bytes
[Upvalues]       1 + (2 × upvalue_count) bytes
[Sub-prototypes] 4 + recursive proto data
[Debug Info]     variable (or minimized if stripped)
```

**Metadata (12 bytes):**
- `num_params` (uint8) — fixed parameter count
- `is_vararg` (uint8) — 1 if uses `...`
- `is_async` (uint8) — 1 if async function
- `max_stack` (uint8) — register window size
- `line_defined` (uint32 LE) — source line start
- `last_line` (uint32 LE) — source line end

**Source name:** uint16 LE length prefix, followed by that many bytes
of source filename. Zero-length means no source recorded.

**Instructions:** uint32 LE count, followed by `count` uint32 LE
instruction words.

**Constants:** uint32 LE count, followed by each constant as:

| Type | Serialized as |
|------|---------------|
| Nil | Tag byte (0) — no payload |
| Bool | Tag byte (1) + value byte (0 or 1) |
| Integer | Tag byte (2) + int64 LE |
| Number | Tag byte (3) + double as uint64 LE (memcpy) |
| String | Tag byte (4) + uint16 LE length + bytes |

**Upvalues:** uint8 count, followed by `count` pairs of (index: uint8,
in_stack: uint8).

**Sub-prototypes:** uint32 LE count, followed by `count` recursively
serialized prototypes (each with its own metadata, instructions,
constants, and so on).

**Debug information:** If the `STRIP` flag is set, three zero markers
are written (uint32 0, uint32 0, uint8 0) and no debug data is stored.
Otherwise:

- Line numbers: uint32 count + `count` uint32 LE values
- Locals: uint32 count + `count` × (uint16 name_len + name + uint32
  start_pc + uint32 end_pc + uint8 register)
- Upvalue names: uint8 count + `count` × (uint16 name_len + name)

### 5.5 Safety Limits

Deserialization enforces hard limits to prevent denial-of-service from
malformed files:

| Field | Maximum | Protection |
|-------|---------|------------|
| Instructions | 16,777,216 (16M) | Rejects oversized code arrays |
| Constants | 65,536 | Matches 16-bit LOADK limit |
| Sub-prototypes | 65,536 | Prevents unreasonable nesting |
| String length | 65,535 | uint16 length prefix cap |

---

## 6. Opcode Space Map

The full 256-byte opcode space, showing assigned and reserved ranges:

```
        0x00  0x10  0x20  0x30  0x40  0x50  0x60  0x70  0x80  0x90  0xA0  0xB0  0xC0  0xD0  0xE0  0xF0
  0x0_  MOV   TBL   UPV   ARITH BIT   STR   CMP── CTRL  LOOP  CLOS  ASYNC MOD   ···   ···   ···   SPEC
  0x01  LOADK GETBL SETUP SUB   BOR   STRLN ····· CALL  FORL  VARG  SPAWN EXP   ···   ···   ···   DEBG
  0x02  LDNIL SETTBL GETG MUL   BXOR  ····· ····· TCAL  TFORC VPREP YIELD ···   ···   ···   ···   ···
  0x03  LDBOO GETF  SETG  DIV   BNOT  ····· ····· RET   TFORL CLOSE ····· ···   ···   ···   ···   ···
  0x04  LDINT SETF  GTTUP IDIV  SHL   ····· ····· RET0  ····· ····· ····· ···   ···   ···   ···   ···
  0x05  LDKX  GETI  STTUP MOD   SHR   ····· ····· RET1  ····· ····· ····· ···   ···   ···   ···   ···
  0x06  ····· SETI  ····· POW   ····· ····· ····· ····· ····· ····· ····· ···   ···   ···   ···   ···
  0x07  ····· SETL  ····· UNM   ····· ····· ····· ····· ····· ····· ····· ···   ···   ···   ···   ···
  0x08  ····· SELF  ····· ADDI  ····· ····· NOT·· ····· ····· ····· ····· ···   ···   ···   ···   ···
  0x09  ····· ····· ····· ADDK  ····· ····· TEST  ····· ····· ····· ····· ···   ···   ···   ···   ···
  0x0A  ····· ····· ····· SUBK  ····· ····· TSETS ····· ····· ····· ····· ···   ···   ···   ···   ···
  0x0B  ····· ····· ····· MULK  ····· ····· ····· ····· ····· ····· ····· ···   ···   ···   ···   ···
  0x0C  ····· ····· ····· DIVK  ····· ····· ····· ····· ····· ····· ····· ···   ···   ···   ···   ···
  0x0D  ····· ····· ····· MODK  ····· ····· ····· ····· ····· ····· ····· ···   ···   ···   ···   ···
  0x0E  ····· ····· ····· ····· ····· ····· ····· ····· ····· ····· ····· ···   ···   ···   ···   XARG
  0x0F  ····· ····· ····· ····· ····· ····· ····· ····· ····· ····· ····· ···   ···   ···   ···   SENT
```

**Assigned:** 46 opcodes across 10 categories.
**Reserved:** 210 slots available for future expansion (0xC0–0xEF is
entirely open).

The gaps between categories are intentional. Each category occupies a
16-slot block with room for future additions without displacing adjacent
categories. This is critical for the stability contract: new arithmetic
instructions go in 0x3E–0x3F, new bitwise instructions go in 0x46–0x4F,
and so on.

---

*Nova is a Zorya Corporation project. MIT License.*
