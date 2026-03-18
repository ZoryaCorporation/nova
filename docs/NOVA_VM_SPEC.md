<!-- ============================================================
     NOVA VM SPECIFICATION
     The Nova Virtual Machine — Architecture and Internal Design

     Author:  Anthony Taliento (Zorya Corporation)
     Date:    2026-03-13
     Version: 0.2.0
     Status:  RATIFIED
     ============================================================ -->

# Nova VM Specification

Nova is a register-based bytecode virtual machine implemented in C99.
It executes dynamically-typed programs compiled from a Lua-like source
language through a pipeline of lexing, preprocessing, parsing,
compilation, and optional optimization. This document describes the
internal architecture of the virtual machine itself — its value
representation, execution engine, memory management, and concurrency
model — as implemented in Nova v0.2.0.

This specification is written for engineers, language implementors, and
computer scientists interested in how Nova works under the hood. It
assumes familiarity with concepts like NaN-boxing, garbage collection,
and bytecode interpretation. For the instruction set and binary format,
see the companion [Bytecode Specification](NOVA_BYTECODE_SPEC.md).

---

## Table of Contents

1. [Value Representation](#1-value-representation)
2. [The Execution Engine](#2-the-execution-engine)
3. [Object System](#3-object-system)
4. [Garbage Collector](#4-garbage-collector)
5. [Closures and Upvalues](#5-closures-and-upvalues)
6. [Metatables and Metamethods](#6-metatables-and-metamethods)
7. [Coroutines](#7-coroutines)
8. [Async Concurrency](#8-async-concurrency)
9. [Error Handling](#9-error-handling)
10. [String Interning](#10-string-interning)
11. [Hash Tables](#11-hash-tables)
12. [VM Configuration](#12-vm-configuration)

---

## 1. Value Representation

Every runtime value in Nova occupies exactly 64 bits. Nova supports two
compile-time representation modes selected by the `NOVA_NAN_BOXING`
flag: a NaN-boxed encoding for 64-bit platforms and a portable tagged
union fallback.

### 1.1 NaN-Boxing (Default on x86-64 and ARM64)

NaN-boxing exploits the structure of IEEE 754 double-precision
floating-point numbers. A double is 64 bits wide. The IEEE standard
reserves a range of bit patterns for "Not a Number" (NaN) values, but
hardware only uses a small fraction of that space. Nova commandeers the
unused NaN bit patterns to encode non-double values — booleans,
integers, object pointers, and nil — directly inside a 64-bit word,
with no indirection and no type tag field.

The key insight is that any 64-bit pattern where bits 62:52 are all
ones and bit 51 is set constitutes a quiet NaN. That leaves 51 bits of
payload that the hardware ignores. Nova uses 4 of those bits as a type
tag and the remaining 48 bits as a value payload — enough to hold a
pointer on current 64-bit architectures.

**Bit layout:**

```
 63  62       52 51 50    48 47                             0
+----+-----------+--+-------+--------------------------------+
| S  | Exponent  | Q| Tag   |           Payload              |
+----+-----------+--+-------+--------------------------------+
  1      11       1    3                  48
```

**Quiet NaN anchor (QNAN):** `0x7FFC000000000000` — bits 62:52 all set,
bit 51 (quiet) set, bits 50:48 clear. Any value whose upper bits match
QNAN is a tagged non-double value.

**Encoding rules:**

| Type | Encoding | Size |
|------|----------|------|
| Double | Raw IEEE 754 bits (any pattern where `(v & QNAN) != QNAN`) | 64 bits |
| Nil | `QNAN \| TAG_NIL` | — |
| Boolean | `QNAN \| TAG_BOOL \| (0 or 1)` | 1 bit payload |
| Integer | `SIGN_BIT \| QNAN \| (value & 0xFFFFFFFFFFFF)` | 48 bits payload |
| Object | `QNAN \| TAG_OBJ \| (pointer & 0xFFFFFFFFFFFF)` | 48-bit pointer |
| C Function | `QNAN \| TAG_CFUNC \| (pointer & 0xFFFFFFFFFFFF)` | 48-bit pointer |

**Tag constants:**

| Tag | Value | Bits 50:48 |
|-----|-------|------------|
| `TAG_NIL` | `0x0001000000000000` | 001 |
| `TAG_BOOL` | `0x0002000000000000` | 010 |
| `TAG_OBJ` | `0x0003000000000000` | 011 |
| `TAG_CFUNC` | `0x0004000000000000` | 100 |

**The sign bit trick:** Integers are distinguished by setting bit 63
(the sign bit) alongside the QNAN bits. Since a quiet NaN with the sign
bit set is otherwise unused, this gives integers their own tag space.
The lower 48 bits hold the integer value with sign extension to recover
a full `int64_t`.

**Object sub-typing:** All heap-allocated objects (strings, tables,
closures, coroutines, userdata) share a single tag (`TAG_OBJ`). The
48-bit payload is a pointer to a `NovaGCHeader`, whose `gc_type` field
distinguishes the actual object type. This costs one pointer dereference
for sub-type queries but keeps the tag space compact.

**Why NaN-boxing matters:** A NaN-boxed value is 8 bytes. A tagged
union would be 16 bytes (8 for the union, 4 for the type tag, 4 for
alignment padding). Halving value size means the register file and value
stack consume half the cache lines, which directly impacts dispatch loop
performance. On x86-64, NaN-boxed dispatch runs approximately 30% faster
than the tagged-union fallback in benchmarks.

### 1.2 Tagged Union (Portable Fallback)

When `NOVA_NAN_BOXING` is 0, `NovaValue` is a 16-byte struct:

```c
typedef struct NovaValue {
    NovaValueType type;    /* Enum tag (4 bytes) */
    union {
        int           boolean;
        nova_int_t    integer;
        nova_number_t number;
        NovaString   *string;
        NovaTable    *table;
        NovaClosure  *closure;
        nova_cfunc_t  cfunc;
        void         *userdata;
        NovaCoroutine *coroutine;
    } as;
} NovaValue;
```

The type system is identical. The same abstraction macros
(`nova_is_string`, `nova_as_table`, etc.) resolve to either NaN-box bit
manipulation or struct field access at compile time. Consumer code never
touches the representation directly.

### 1.3 Type System

Nova has ten runtime types:

| Type | Tag | Heap-Allocated | Description |
|------|-----|----------------|-------------|
| Nil | 0 | No | Absence of value |
| Boolean | 1 | No | `true` or `false` |
| Integer | 2 | No | Signed 64-bit (`int64_t`) |
| Number | 3 | No | IEEE 754 double |
| String | 4 | Yes | Immutable, interned byte sequence |
| Table | 5 | Yes | Associative array (array + hash parts) |
| Function | 6 | Yes | Nova closure (code + captured variables) |
| C Function | 7 | No* | Host function pointer |
| Userdata | 8 | Yes | Opaque C pointer with optional metatable |
| Thread | 9 | Yes | Coroutine (independent execution context) |

\* C functions are encoded as raw pointers in NaN-boxed mode —
no heap allocation required.

**Truthiness:** Nil and `false` are falsy. Every other value — including
the integer zero, the empty string, and empty tables — is truthy.

---

## 2. The Execution Engine

Nova is a register-based virtual machine. Where stack-based VMs
(like the JVM or CPython) push and pop operands on an implicit
evaluation stack, Nova's instructions name their operands explicitly as
register indices. An instruction like `ADD R3, R1, R2` reads from
registers 1 and 2 and writes to register 3 — no pushes, no pops. This
reduces instruction count (fewer dup/swap/rot operations), improves data
locality, and maps naturally to the underlying CPU's register file.

### 2.1 Instruction Format

Every instruction is a 32-bit word. The high 8 bits are the opcode, and
the remaining 24 bits carry operands in one of four formats:

```
31       24 23       16 15        8 7         0
+----------+----------+----------+----------+
| Opcode   |    A     |    B     |    C     |  ABC   (3 × 8-bit)
+----------+----------+----------+----------+
| Opcode   |    A     |       Bx (16-bit)   |  ABx   (8 + 16-bit)
+----------+----------+----------+----------+
| Opcode   |    A     |       sBx (signed)  |  AsBx  (8 + signed 16)
+----------+----------+----------+----------+
| Opcode   |          Ax (24-bit)           |  Ax    (24-bit)
+----------+----------+----------+----------+
```

The A field typically names the destination register. B and C name
source registers, constant indices, or immediate values depending on the
opcode. The complete instruction set is documented in the
[Bytecode Specification](NOVA_BYTECODE_SPEC.md).

### 2.2 Register Window

Each call frame owns a contiguous window of the VM's value stack. A
function's `max_stack` field (computed by the compiler) determines how
many registers the frame needs. Frame registers are addressed 0 through
`max_stack - 1` relative to the frame's `base` pointer:

```
VM stack (NovaValue[]):
┌────────┬────────┬────────┬────────┬────────┬────────┬────────┐
│ R0     │ R1     │ R2     │ R3     │ R0     │ R1     │ R2     │
│ caller │ caller │ caller │ caller │callee  │callee  │callee  │
└────────┴────────┴────────┴────────┴────────┴────────┴────────┘
         ↑ caller base               ↑ callee base
```

The limit is 125 usable registers per frame. This is derived from the
8-bit B/C fields: the upper half (128–255) is reserved for
register-or-constant (RK) encoding, and a further 3 slots are reserved
for internal use, leaving 125 for user code. In practice most functions
need far fewer — a typical function uses 10–30 registers.

### 2.3 The Constant Pool and RK Encoding

Each function prototype carries a constant pool — an array of
compile-time values (numbers, strings, booleans, nil). Instructions
reference constants by index.

For instructions with 16-bit `Bx` fields (like `LOADK`), the constant
index can address up to 65,536 entries. But many instructions use 8-bit
B or C fields, which could only address 256 constants — or 256
registers. Nova's RK encoding shares the 8-bit space between both:

- Indices 0–127: register references
- Indices 128–255: constant pool references (subtract 128 to get the
  pool index)

The high bit acts as a discriminator. At dispatch time, a single
comparison determines whether to read from the register file or the
constant pool. This keeps three-operand instructions (like `ADD R3, R1,
K5`) in a compact 32-bit word without needing a separate `ADDK` opcode
for every combination — though Nova does provide dedicated constant
variants (`ADDK`, `SUBK`, `MULK`, `DIVK`, `MODK`) for the most common
patterns, eliminating the RK branch entirely on hot paths.

### 2.4 Computed Goto Dispatch

The dispatch loop is the innermost loop of the virtual machine — the
code that reads an instruction, decodes the opcode, and jumps to the
handler. Its performance determines the VM's throughput for
compute-bound programs.

Nova uses GCC's computed goto extension (`&&label` / `goto *ptr`) to
build a direct-threaded dispatch loop. A static array of 256 label
addresses — one per possible opcode — is constructed at startup:

```c
static const void *dispatch_table[256] = {
    [NOVA_OP_MOVE]    = &&op_MOVE,
    [NOVA_OP_LOADK]   = &&op_LOADK,
    [NOVA_OP_ADD]     = &&op_ADD,
    /* ... one entry per opcode ... */
};
```

Each handler ends with a `DISPATCH()` macro that fetches the next
instruction and jumps directly to its handler — no switch statement, no
branch prediction against a single indirect jump target. The CPU's
branch predictor sees a different jump site for each opcode, enabling
it to build per-opcode prediction histories.

The `DISPATCH()` macro also prefetches the next instruction's cache
line using `PREFETCH_NEXT_OP(ip)`, hiding memory latency on sequential
instruction streams. On platforms without computed goto support (MSVC),
the dispatch loop falls back to a conventional `switch` statement.

In benchmarks, computed goto dispatch is approximately 30% faster than
the switch fallback.

### 2.5 Function Calls

The `CALL` instruction (`OP_CALL A B C`) invokes the value in register
A with the arguments in registers A+1 through A+B-1, expecting C-1
results.

**Call sequence:**

1. **Resolve callee.** The value at `R(A)` must be a closure, a C
   function, or an object with a `__call` metamethod. If it's none of
   these, a runtime error is raised.

2. **Check call depth.** The frame count must be below
   `NOVA_MAX_CALL_DEPTH` (200). Stack overflow raises a runtime error
   rather than corrupting the C stack.

3. **Ensure stack space.** The callee's `max_stack` (plus a safety
   margin of 10 slots) is guaranteed available, growing the stack if
   necessary. After a grow, all frame `base` pointers are recalculated
   — the `REFRESH_BASE` macro handles this in the dispatch loop.

4. **Push frame.** A new `NovaCallFrame` is pushed onto the frame
   array. Its `base` is set to `&stack[A+1]` — the arguments are
   already in place. The caller's instruction pointer is saved.

5. **Initialize registers.** Registers above the argument count are
   nil-filled so the GC never sees stale pointers.

6. **Transfer control.** The dispatch loop jumps to the callee's first
   instruction. For C functions, the function pointer is called directly
   and results are copied back to the caller's register window.

**Return sequence (`OP_RETURN A B`):**

1. Close any open upvalues pointing into the current frame
   (`novai_close_upvalues`).
2. Copy return values from `R(A)` through `R(A+B-2)` to the caller's
   return receptor (at `frame->base - 1`).
3. Free any saved varargs.
4. Pop the frame. Restore the caller's `base`, instruction pointer,
   prototype, code array, and constant pool.
5. Adjust `stack_top` to reflect the caller's register window.

Nova also provides `RETURN0` (no values) and `RETURN1` (single value)
as specialized fast-path variants of `RETURN`, eliminating the loop
overhead for the common cases.

**Tail calls** (`OP_TAILCALL`) reuse the current frame's stack slot
rather than pushing a new frame, preventing stack growth in recursive
patterns. Arguments are shifted down to overwrite the current frame's
registers, and the callee inherits the current frame position.

### 2.6 Global Variables

Globals are stored in a single `NovaTable` rooted at `vm->globals`.
The `GETGLOBAL` instruction takes a constant pool index (the variable
name as a string), looks it up in the globals table, and writes the
result to the destination register. `SETGLOBAL` does the reverse. Both
involve a hash table lookup — O(1) amortized — making global access
slightly more expensive than dec (register) access, which is free.

This is intentional: it encourages developers to use locals and creates
a clear performance gradient that aligns with good programming practice.

---

## 3. Object System

Every heap-allocated object begins with a `NovaGCHeader`:

```c
typedef struct NovaGCHeader {
    struct NovaGCHeader *gc_next;    /* All-objects singly-linked list */
    struct NovaGCHeader *gc_gray;    /* Gray list for mark traversal   */
    uint8_t              gc_type;    /* NovaValueType tag              */
    uint8_t              gc_color;   /* WHITE0, WHITE1, GRAY, BLACK   */
    uint8_t              gc_flags;   /* Object-specific flags          */
    uint8_t              _gc_pad;    /* Alignment padding              */
    uint32_t             gc_size;    /* Allocation size in bytes       */
} NovaGCHeader;  /* 24 bytes on 64-bit */
```

The header is always the first field of every object struct, so a
pointer to any object can be safely cast to `NovaGCHeader*` for generic
GC traversal. All objects are linked through `gc_next` into a single
chain rooted at `vm->all_objects`. The `gc_gray` pointer threads objects
into a separate gray list during the mark phase (see Section 4).

### 3.1 Strings

A `NovaString` is an immutable byte sequence with a precomputed NXH64
hash. All strings are interned in a global pool (see Section 10),
guaranteeing that two strings with the same content share the same
pointer. This makes string equality a pointer comparison — O(1)
regardless of length.

### 3.2 Tables

A `NovaTable` is a hybrid data structure with two parts:

- **Array part:** A dense `NovaValue[]` for consecutive integer keys
  starting at 0. Indexed in O(1). Initial capacity is 4 slots, growing
  geometrically.

- **Hash part:** A hash map for string keys and non-consecutive integer
  keys. Uses linear probing with a 75% load factor. Initial capacity is
  4 slots, growing by 2× on rehash.

Tables also carry an optional metatable pointer for operator overloading
and prototype inheritance (see Section 6).

### 3.3 Closures

A `NovaClosure` pairs a function prototype (`NovaProto*`) with an array
of captured upvalues. The prototype is shared among all closures created
from the same function definition; upvalue arrays are per-closure. See
Section 5 for the capture mechanism.

### 3.4 Coroutines

A `NovaCoroutine` is an independent execution context with its own value
stack, call frame array, and upvalue chain. See Section 7.

---

## 4. Garbage Collector

Nova uses a tri-color incremental mark-and-sweep garbage collector. It
runs interleaved with program execution — a small amount of GC work is
performed after each allocation, so the program never pauses for a
complete heap traversal. This is the same fundamental design used by
Lua 5.1+, adapted for Nova's object model.

### 4.1 Tri-Color Abstraction

The GC assigns each object one of four colors:

| Color | Meaning |
|-------|---------|
| **White** (current) | Not yet reached by the current mark phase. Newly allocated objects receive this color. |
| **White** (other) | Not yet reached, from the previous cycle. Objects still wearing this color after marking are dead. |
| **Gray** | Reached by the marker, but children not yet scanned. Queued for traversal. |
| **Black** | Fully scanned. This object and all its direct children have been visited. |

The two white colors (WHITE0 and WHITE1) implement a flip-flop: at the
start of each GC cycle, the "current white" toggles. New allocations
receive the current white (alive by definition). Objects not visited
during marking retain the previous cycle's white (dead). This
eliminates the need to walk the entire object list at cycle start just
to reset colors.

### 4.2 GC Phases

The collector operates as a three-phase state machine:

```
    ┌──────────────────────────────────────────────┐
    │                                              │
    ▼                                              │
  PAUSE ──────► MARK ──────► SWEEP ────────────────┘
  (idle)     (trace roots    (free dead
              and gray        objects,
              objects)        reset whites)
```

**PAUSE → MARK:** Triggered when `bytes_allocated` exceeds
`gc_threshold`. The current white flips. All root objects are shaded
gray and pushed onto the gray list.

**MARK:** Processes gray objects in batches of 40 per step. Each step
pops a gray object, inspects its type, and marks all referenced
children gray (if they are white). The object itself becomes black.
Mark work is measured in bytes traversed — each object's `gc_size` is
counted against the step budget.

**SWEEP:** Walks the `all_objects` chain in batches of 40. Objects
wearing the dead white are unlinked and freed. Survivors are re-colored
to the current white. The sweep position is saved between steps for
exact resumption.

**SWEEP → PAUSE:** When the sweep pointer reaches the end of the list,
the collector returns to PAUSE. A new threshold is computed:

```
gc_threshold = gc_estimate × gc_pause / 100
```

With the default `gc_pause` of 200, the heap must approximately double
before the next cycle begins.

### 4.3 Root Set

The marker begins each cycle by shading these roots gray:

1. **Value stack** — The register windows of all active call frames.
   The scan respects each frame's `base + max_stack` extent, not just
   `stack_top`, because C function calls temporarily lower `stack_top`
   while the caller's registers are still live.

2. **Call frame closures** — The closure pointer in each active frame.

3. **Call frame varargs** — Saved variadic arguments for each frame.

4. **Global table** — `vm->globals` and all its contents.

5. **Open upvalues** — `vm->open_upvalues`, the chain of upvalues still
   pointing at stack slots.

6. **Running coroutine** — `vm->running_coroutine`.

7. **Saved stacks** — When coroutine A resumes B, A's stack is saved
   to a `NovaSavedStackRef` linked through `vm->saved_stacks`. The GC
   traverses every saved stack's register windows, closures, and
   varargs. This handles arbitrarily deep nesting (A resumes B
   resumes C).

8. **Async task queue** — `vm->task_queue[0..task_count-1]`, the
   spawned coroutines managed by the async event loop.

9. **Per-type metatables** — `vm->string_mt`, the shared string
   metatable that enables method syntax on strings.

### 4.4 Write Barrier

When the marker is running, mutator operations that create new
references from a black (fully-scanned) object to a white (unscanned)
object could cause the collector to miss the white object — a
dangling-pointer bug. The write barrier prevents this.

Nova uses a "barrier forward" strategy: when a black parent gains a
new child reference during the mark phase, the parent is re-shaded gray
and pushed back onto the gray list for re-traversal:

```c
if (gc_phase == MARK && parent->gc_color == BLACK) {
    parent->gc_color = GRAY;
    parent->gc_gray  = vm->gray_list;
    vm->gray_list    = parent;
}
```

The barrier is inlined (`NOVA_GC_BARRIER_INLINE`) at all seven table
write sites in the dispatch loop, saving approximately 5 nanoseconds
per call versus a function-call barrier in hot paths.

### 4.5 Incremental Pacing

GC work is driven by allocation pressure. Each allocation adds its size
to `gc_debt`. When the debt is positive and the total allocation exceeds
the threshold, `nova_gc_step()` runs a work budget proportional to the
debt:

```
work_target = gc_debt × gc_step_mul / 100
```

With the default `gc_step_mul` of 200, the GC does 2× the work relative
to the allocation that triggered it. The minimum step size is 1 KB to
avoid thrashing on small allocations.

This pacing model means programs that allocate rapidly get more
aggressive collection, while programs that allocate sparingly are rarely
interrupted. A full collection (`nova_gc_full_collect`) can be triggered
explicitly to run the complete cycle synchronously.

### 4.6 Object Traversal

Each object type defines what the marker traverses:

| Object | Children Traversed |
|--------|--------------------|
| String | None (leaf node) |
| Table | Array values, hash keys and values, metatable |
| Closure | Upvalue array |
| Upvalue (open) | Stack slot (via pointer) |
| Upvalue (closed) | Closed value (owned copy) |
| Coroutine | Body closure, stack values, frame closures, open upvalues, pending arguments |

### 4.7 Memory Lifecycle

All heap allocations go through `nova_gc_check(vm, size)`, which
accumulates debt and triggers a step if needed. The VM tracks total
`bytes_allocated` for threshold comparison and `gc_estimate` as a
running approximation of live bytes (refined after each sweep). At VM
destruction, `nova_gc_shutdown()` walks the `all_objects` chain and
frees every object unconditionally — no marking required.

---

## 5. Closures and Upvalues

When a function references a dec variable from an enclosing scope,
the compiler emits an upvalue capture. At runtime, the `CLOSURE`
instruction creates a `NovaClosure` with an upvalue array whose entries
track the captured variables.

### 5.1 Open and Closed Upvalues

An upvalue is "open" when the variable it captures is still on the
stack — the upvalue holds a pointer directly into the enclosing frame's
register window. Multiple closures capturing the same variable share
the same `NovaUpvalue` object, so mutations are visible to all.

Open upvalues are threaded into a sorted chain at `vm->open_upvalues`,
ordered by descending stack position. This ordering allows efficient
batch closure: when a frame returns, `novai_close_upvalues()` walks the
chain and closes all upvalues pointing at or above the frame's base.

"Closing" an upvalue means copying the stack value into the upvalue
object's own `closed` field and redirecting the upvalue's `location`
pointer from the stack slot to `&self->closed`. After closing, the
variable lives on the heap and survives the enclosing frame's
destruction. This is the mechanism that makes closures work in the
presence of stack-frame lifetimes.

### 5.2 Upvalue Descriptors

Each function prototype carries an array of `NovaUpvalDesc` structs
that tell the `CLOSURE` instruction how to resolve each upvalue:

```c
typedef struct {
    uint8_t index;     /* Parent's register or upvalue index  */
    uint8_t in_stack;  /* 1 = parent's dec, 0 = parent's upvalue */
} NovaUpvalDesc;
```

If `in_stack` is 1, the captured variable is a dec in the immediately
enclosing function — look up register `index` in the parent frame. If
`in_stack` is 0, the captured variable is itself an upvalue of the
parent — follow the parent closure's upvalue chain at position `index`.
This handles arbitrary nesting depth: a closure can capture a variable
from any ancestor scope through a chain of upvalue-of-upvalue
references.

---

## 6. Metatables and Metamethods

Any table can have a metatable — another table whose string-keyed
entries define overloaded behavior. When the VM encounters an operation
on a value that doesn't natively support it (like adding two tables, or
indexing a non-table), it checks the operand's metatable for a handler.

### 6.1 Metamethod Protocol

Metamethod lookup follows a fixed protocol:

1. Check the value's metatable (or the per-type metatable for non-table
   types).
2. Look up the metamethod name (e.g., `"__add"`, `"__index"`) as a
   string key.
3. If found and callable, invoke it with the operands as arguments.
4. If not found, raise a type error.

The VM tracks `meta_stop_frame` to prevent infinite metamethod recursion
— if a metamethod triggers the same metamethod, the stop frame prevents
re-entry.

### 6.2 String Metatables

Strings have a shared metatable stored at `vm->string_mt`. This table's
`__index` entry points to the string standard library, enabling method
syntax:

```lua
dec s = "hello world"
s:upper()           -- Dispatches via __index on string_mt
s:find("world")     -- Same mechanism
```

The `SELF` instruction (`OP_SELF A B C`) implements method calls:
it stores the receiver in `R(A+1)` and the looked-up method in `R(A)`,
ready for a subsequent `CALL`. For strings, the `SELF` handler walks the
metatable chain to find the method function.

### 6.3 Supported Metamethods

| Metamethod | Triggered by |
|------------|-------------|
| `__add`, `__sub`, `__mul`, `__div`, `__mod`, `__pow`, `__idiv` | Arithmetic operators |
| `__band`, `__bor`, `__bxor`, `__bnot`, `__shl`, `__shr` | Bitwise operators |
| `__unm` | Unary minus |
| `__eq`, `__lt`, `__le` | Comparison operators |
| `__concat` | String concatenation (`..`) |
| `__len` | Length operator (`#`) |
| `__index` | Table read (field not found) |
| `__newindex` | Table write (new field) |
| `__call` | Function call on non-function |
| `__tostring` | String coercion |

---

## 7. Coroutines

A coroutine is an independent execution context — a thread of
computation with its own stack, call frames, and upvalue chain, but
sharing the VM's globals and GC. Coroutines enable cooperative
multitasking: a running coroutine can yield control at any point,
suspending its state, and be resumed later from exactly where it left
off.

### 7.1 Coroutine Structure

```c
struct NovaCoroutine {
    NovaGCHeader   gc;                               /* GC header          */
    NovaValue      stack[NOVA_COROUTINE_STACK_SIZE];  /* Own stack (256)    */
    NovaValue     *stack_top;
    NovaCallFrame  frames[NOVA_COROUTINE_MAX_FRAMES]; /* Own frames (64)   */
    int            frame_count;
    NovaClosure   *body;                              /* Entry function     */
    NovaUpvalue   *open_upvalues;                     /* Own upvalue chain  */
    NovaCoStatus   status;                            /* SUSPENDED/RUNNING/DEAD/NORMAL */
    /* ... pending args, error state ... */
};
```

Each coroutine has a 256-slot stack and up to 64 call frames —
intentionally smaller than the main VM (1024 slots, 200 frames) to keep
coroutines lightweight. They are GC-managed objects; the collector
traverses their stacks and frame closures as part of the mark phase.

### 7.2 Resume and Yield

**`coroutine.resume(co, args...)`:**

1. The called coroutine's status must be SUSPENDED.
2. The caller's stack and frames are saved to a `NovaSavedStackRef` and
   linked into `vm->saved_stacks`.
3. The VM switches to the coroutine's stack, frames, and instruction
   pointer.
4. Resume arguments are placed in the coroutine's registers at the point
   where it last yielded (or as the body function's parameters on first
   resume).

**`coroutine.yield(values...)`:**

1. The yield instruction (`OP_YIELD`) saves the coroutine's state and
   sets its status to SUSPENDED.
2. The VM restores the calling context from `vm->saved_stacks`.
3. Yielded values become the return values of the `resume()` call in
   the caller.

This symmetric transfer means no C-stack recursion occurs during
resume/yield — the VM simply swaps which execution context its dispatch
loop operates on.

### 7.3 Coroutine States

| Status | Meaning |
|--------|---------|
| SUSPENDED | Created but not yet started, or yielded |
| RUNNING | Currently executing |
| DEAD | Body function returned or raised an unhandled error |
| NORMAL | Has resumed another coroutine and is waiting |

---

## 8. Async Concurrency

Nova provides an `async/await` model built on top of coroutines. An
async function is a regular function marked with the `is_async` flag in
its prototype. When called, it does not execute immediately — instead,
it creates a coroutine and enqueues it in the VM's task queue.

### 8.1 Task Queue and Event Loop

The VM maintains a round-robin task queue:

```c
NovaCoroutine **task_queue;     /* Array of spawned tasks */
int             task_count;     /* Active task count      */
int             task_capacity;  /* Allocated slots        */
```

The `OP_SPAWN` instruction registers a coroutine as an async task.
The `async.run()` library function starts the event loop, which:

1. Resumes each task in round-robin order.
2. Tasks run until they yield (`OP_YIELD`) or complete.
3. Completed tasks (status DEAD) are removed from the queue.
4. The loop terminates when all tasks are dead.

### 8.2 Await

The `OP_AWAIT` instruction yields the current task and marks it as
waiting on another task's result. When the awaited task completes, the
event loop resumes the waiting task with the result values.

This model provides concurrency (multiple logical threads of execution)
without parallelism (only one task runs at a time). It is well-suited
for I/O-bound workloads where tasks spend most of their time waiting for
external events.

---

## 9. Error Handling

Nova uses `setjmp`/`longjmp` for error propagation — the same mechanism
used by Lua and many C-based interpreters. When an error occurs, control
transfers immediately to the nearest protected call boundary, unwinding
all intervening frames.

### 9.1 Error Representation

```c
struct NovaErrorJmp {
    jmp_buf              buf;        /* setjmp buffer              */
    struct NovaErrorJmp *previous;   /* Chain for nested pcalls    */
    int                  status;     /* Error code                 */
    int                  frame_count;/* Frame depth at entry       */
    NovaValue           *stack_top;  /* Stack position at entry    */
};
```

Error jumps form a linked list through `vm->error_jmp`. Each `pcall`
pushes a new entry; each successful return or caught error pops it.

### 9.2 Protected Calls (pcall / xpcall)

`nova_vm_pcall()` establishes a recovery point:

1. **Save state:** Frame count, stack top, error jump chain, status,
   metamethod stop frame, and C-function base pointer.
2. **Set jump point:** `setjmp(ej.buf)`. If this returns 0, the call
   proceeds normally.
3. **Normal path:** Execute the call via `nova_vm_call()`. On success,
   restore the error jump chain and return `NOVA_VM_OK`.
4. **Error path:** If `longjmp` fires (setjmp returns non-zero),
   restore all saved state, close upvalues for unwound frames, free
   their varargs, push the error message to the caller's return slot,
   and return the error code.

This is how Nova implements structured error handling without
exceptions: `pcall()` catches any error raised during execution,
`xpcall()` adds a message handler function that can augment the error
before it propagates.

### 9.3 Error Codes

| Code | Constant | Meaning |
|------|----------|---------|
| 0 | `NOVA_VM_OK` | No error |
| 1 | `NOVA_VM_ERR_RUNTIME` | General runtime error |
| 2 | `NOVA_VM_ERR_MEMORY` | Allocation failure |
| 3 | `NOVA_VM_ERR_STACKOVERFLOW` | Call depth exceeded |
| 4 | `NOVA_VM_ERR_TYPE` | Type mismatch |
| 5 | `NOVA_VM_ERR_DIVZERO` | Division by zero |
| 6 | `NOVA_VM_ERR_NULLPTR` | Null pointer dereference |
| 7 | `NOVA_VM_YIELD` | Execution suspended (not an error) |

---

## 10. String Interning

Every string value in Nova passes through the string intern pool. If a
string with the same bytes already exists, the existing pointer is
returned. If not, a new string is allocated, hashed, and inserted into
the pool.

### 10.1 Weave String Library

The intern pool is built on the Weave string library from the Zorya SDK.
A `Tablet` is a hash set of unique strings, backed by a DAGGER hash
table (see Section 11). The interning API:

```c
const Weave *tablet_intern(Tablet *T, const char *s);
const Weave *tablet_intern_len(Tablet *T, const char *s, size_t len);
```

Interned strings are owned by the Tablet. They are immutable and carry
precomputed hashes. Each `Weave` object stores its data, length, and
capacity in a flexible array layout:

```
[uint64_t len] [uint64_t cap] [uint8_t flags] [data ... NUL]
```

### 10.2 Hash Function (NXH64)

Strings are hashed using NXH64, a custom 64-bit non-cryptographic hash
function from the Zorya SDK. It processes input in 32-byte blocks using
four parallel accumulators with prime-derived mixing constants:

```c
acc += input × PRIME_VOID;
acc = rotate_left(acc, 31);
acc *= PRIME_NEXUS;
```

The mixing primes are derived from mathematical constants (golden ratio
derivatives) chosen for maximum bit avalanche. The finalization step
applies multiple XOR-shift-multiply rounds to ensure full bit diffusion.

NXH64 achieves approximately 10 GB/s throughput on x86-64. All reads
are unaligned-safe (byte-by-byte composition), making it portable to
architectures with strict alignment requirements.

The string hash seed is fixed at `0x4E6F7661` (ASCII "Nova"), ensuring
deterministic behavior across runs.

### 10.3 Why Interning Matters

In a typical program, the same short strings (variable names, method
names, field keys) appear thousands of times. Without interning, every
table lookup would require `memcmp` — O(n) in string length. With
interning, equality is pointer comparison — O(1) unconditionally. Table
lookups compare hashes first (O(1)) and pointers second (O(1)), making
the hash table's inner loop branch-free for the common case.

Nova interns all strings up to `NOVA_SHORT_STRING_LIMIT` (40 bytes)
unconditionally. Longer strings may be interned on demand.

---

## 11. Hash Tables

Nova's hash tables — used for the globals table, table objects, and
the string intern pool — are backed by DAGGER (Dual-Action Guaranteed
Grab Engine for Records), a hybrid hash table from the Zorya SDK.

### 11.1 Robin Hood Hashing with Cuckoo Fallback

DAGGER uses Robin Hood hashing as its primary collision resolution
strategy. In Robin Hood hashing, each entry tracks its probe sequence
length (PSL) — the distance from its ideal hash slot to its actual
position. During insertion, if the new entry's PSL exceeds an existing
entry's PSL, the entries are swapped, keeping the variance of PSLs low.
This has a practical effect: lookups can terminate early when they
encounter an entry whose PSL is lower than the current probe count,
because Robin Hood invariants guarantee no matching entry can exist
further along.

When an entry's PSL exceeds a threshold (16), DAGGER falls back to
Cuckoo hashing: the entry is hashed with a secondary hash function
(`nxh64_alt`) and placed at the alternate location. This bounds worst-
case probe length at the cost of a second hash computation on rare
inserts.

### 11.2 Table Entry

Each DAGGER entry is 40 bytes:

```c
typedef struct {
    uint64_t    hash_primary;    /* Primary NXH64 hash         */
    uint64_t    hash_alternate;  /* Secondary hash (Cuckoo)    */
    const void *key;             /* Key pointer                */
    void       *value;           /* Value pointer               */
    uint32_t    key_len;         /* Key length in bytes        */
    uint8_t     psl;             /* Probe sequence length      */
    uint8_t     occupied;        /* 1 if slot is in use        */
    uint8_t     in_cuckoo;       /* 1 if at alternate position */
    uint8_t     _pad;            /* Alignment                  */
} DaggerEntry;
```

### 11.3 Load Factor and Growth

DAGGER resizes at 75% load factor, doubling capacity each time. The
minimum capacity is 16 slots; the default initial capacity is 64. On
resize, all entries are rehashed and placed at their ideal primary
positions (resetting PSLs to minimum). The growth factor of 2 keeps
capacities at powers of two, allowing hash-to-slot conversion with a
bitwise AND instead of expensive modulo division.

### 11.4 Hot Path Optimization

DAGGER provides inlined hot-path functions through the Zorya PCM
(Performance Critical Macros) system:

- `dagger_hot_probe1()` — Single-slot probe (covers 75%+ of lookups)
- `dagger_hot_get()` — Full Robin Hood search, inlined
- `dagger_hot_get_str()` — String key shortcut (inlines `strlen`)
- `dagger_hot_get_int()` — Integer key shortcut (hashes `uint64_t`
  directly)

These eliminate function call overhead for the VM's most frequent
operation: table field access.

---

## 12. VM Configuration

The following compile-time constants define the VM's limits and tuning
parameters. They are set in `nova_conf.h` and can be overridden at
build time.

### 12.1 Numeric Types

| Constant | Default | Description |
|----------|---------|-------------|
| `NOVA_NUMBER_TYPE` | `double` | IEEE 754 64-bit floating point |
| `NOVA_INTEGER_TYPE` | `int64_t` | Signed 64-bit integer |
| `NOVA_UNSIGNED_TYPE` | `uint64_t` | Unsigned 64-bit integer |

### 12.2 VM Limits

| Constant | Value | Description |
|----------|-------|-------------|
| `NOVA_MAX_REGISTERS` | 125 | Usable registers per frame |
| `NOVA_MAX_CALL_DEPTH` | 200 | Maximum nested call frames |
| `NOVA_MAX_UPVALUES` | 64 | Upvalues per closure |
| `NOVA_MAX_CONSTANTS` | 65,536 | Constants per function |
| `NOVA_MAX_LOCALS` | 120 | Local variables (debug info) |
| `NOVA_INITIAL_STACK_SIZE` | 1,024 | Initial stack slots |
| `NOVA_MAX_STACK_SIZE` | 1,000,000 | Hard stack limit |
| `NOVA_COROUTINE_STACK_SIZE` | 256 | Coroutine stack slots |
| `NOVA_COROUTINE_MAX_FRAMES` | 64 | Coroutine call frames |

### 12.3 String Limits

| Constant | Value | Description |
|----------|-------|-------------|
| `NOVA_SHORT_STRING_LIMIT` | 40 bytes | Always-intern threshold |
| `NOVA_MAX_STRING_LENGTH` | 256 MB | Maximum string size |

### 12.4 Garbage Collector

| Constant | Value | Description |
|----------|-------|-------------|
| `NOVA_GC_INITIAL_THRESHOLD` | 128 KB | First collection trigger |
| `NOVA_GC_PAUSE` | 200% | Heap must double between cycles |
| `NOVA_GC_STEP_MULTIPLIER` | 200–400% | Work per step relative to debt |
| `NOVA_GC_MIN_STEP_SIZE` | 1 KB | Minimum work per step |
| `NOVA_GC_MARK_BATCH` | 40 | Gray objects per mark step |
| `NOVA_GC_SWEEP_BATCH` | 40 | Objects per sweep step |
| `NOVA_GC_MIN_THRESHOLD` | 64 KB | Minimum threshold floor |

### 12.5 Preprocessor

| Constant | Value | Description |
|----------|-------|-------------|
| `NOVA_PP_MAX_INCLUDE_DEPTH` | 32 | `#import` nesting limit |
| `NOVA_PP_MAX_MACRO_DEPTH` | 64 | Macro expansion depth |
| `NOVA_PP_MAX_MACRO_PARAMS` | 32 | Parameters per macro |
| `NOVA_PP_MAX_IF_DEPTH` | 64 | `#if`/`#ifdef` nesting |

---

*Nova is a Zorya Corporation project. MIT License.*
