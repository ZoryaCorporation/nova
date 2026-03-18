<!-- ============================================================
     NOVA TYPE DECLARATION SPECIFICATION
     Enums, Structs, and Type Aliases — Design, Compilation, and
     Runtime Semantics

     Author:  Anthony Taliento (Zorya Corporation)
     Date:    2026-03-15
     Version: 0.2.0
     Status:  RATIFIED
     ============================================================ -->

# Nova Type Declaration Specification

Nova v0.2.0 introduces three declaration forms — `enum`, `struct`, and
`typedec` — that let programs define user data structures as first-class
runtime values. These constructs are implemented entirely at compile
time: the parser recognizes the declaration syntax, the compiler emits
standard bytecode (table construction, closure creation, global
assignment), and the runtime needs no new opcodes or VM machinery.
The type system remains fully dynamic — declarations don't enforce static
types — but they produce values with embedded metadata (`__type`,
`__base`) that enable runtime introspection, serialization, and
disciplined domain modeling.

This document describes the internal design: how the lexer, parser, AST,
and compiler cooperate to implement type declarations, what bytecode
they produce, and what invariants hold at runtime. It is written for
engineers and computer scientists interested in how a dynamically-typed
language can provide structured data modeling without static type
checking, and what that design enables for systems-level programming.

For source-level usage and examples, see the
[Nova Language Guide](../nova_syntax/NOVA_GUIDE.md). For the VM's
execution model, see the [VM Specification](NOVA_VM_SPEC.md).

---

## Table of Contents

1. [Design Philosophy](#1-design-philosophy)
2. [Lexical Integration](#2-lexical-integration)
3. [Abstract Syntax Tree](#3-abstract-syntax-tree)
4. [Parsing](#4-parsing)
5. [Compilation: Enum](#5-compilation-enum)
6. [Compilation: Struct](#6-compilation-struct)
7. [Compilation: Typedec (Type Alias)](#7-compilation-typedec-type-alias)
8. [Compound Forms: typedec enum and typedec struct](#8-compound-forms-typedec-enum-and-typedec-struct)
9. [Runtime Representation](#9-runtime-representation)
10. [Practical Implications for Systems Development](#10-practical-implications-for-systems-development)
11. [Limits and Constants](#11-limits-and-constants)

---

## 1. Design Philosophy

Most dynamically-typed languages offer a single compound type — the
associative table, the dictionary, the object — and rely on convention
to distinguish different shapes of data. A table used as an enum looks
identical to a table used as a record. The programmer must remember
which is which, because the language cannot tell them apart.

Nova takes a different approach. Rather than adding a static type system
(which would fundamentally change the language's character), Nova
provides **declaration forms** that compile down to standard tables and
closures but embed type metadata into the values themselves. An enum
table carries `__type = "Direction"` and `__base = "enum"`. A struct
instance carries `__type = "Vec3"` and `__base = "struct"`. This
metadata costs two table entries per value. It buys runtime type
queries, serialization that preserves type identity, and error messages
that name the type involved.

The design principle is: **zero new runtime machinery, maximum
structural leverage.** The VM needs no new opcodes, no new value tags,
no new GC traversal logic. The compiler translates declarations into
the same `NEWTABLE`, `SETTABLE`, `CLOSURE`, and `SETGLOBAL` instructions
it already emits for tables and functions. The type system remains
dynamic. The discipline is opt-in. But the discipline is available, and
it changes what you can build.

### 1.1 Why Not Static Types?

Nova's value representation (NaN-boxed 64-bit words — see [VM Spec §1](NOVA_VM_SPEC.md#1-value-representation)) packs type tags into quiet NaN
bit patterns. Adding compile-time type checking would not change this
representation — runtime values would still be dynamically tagged. A
static type system would add complexity to the compiler (type inference,
unification, error reporting) without a corresponding performance
benefit, because the VM would still box and unbox values the same way.

Instead, type declarations provide **structural documentation that the
runtime can inspect**. This is the pragmatic middle ground: the
programmer gets named types with metadata, the compiler stays simple,
and the VM stays fast.

### 1.2 Module-Scope Declarations

Type declarations are designed to live at module scope — the top level
of a source file, alongside global function definitions. This mirrors
how data structures work in systems languages: types define the shape of
a program's data, independent of any particular function's control flow.
While declarations work syntactically inside function bodies (the parser
enforces no scope restriction), the idiomatic pattern places them at the
top of the file, where they serve as the program's data architecture.

---

## 2. Lexical Integration

The three keywords — `enum`, `struct`, and `typedec` — are registered
in the lexer's sorted keyword table alongside Nova's 30+ other reserved
words:

```c
static const NovaKeyword nova_keywords[] = {
    /* ... */
    { "enum",     NOVA_TOKEN_ENUM },
    /* ... */
    { "struct",   NOVA_TOKEN_STRUCT },
    /* ... */
    { "typedec",  NOVA_TOKEN_TYPEDEC },
    /* ... */
};
```

The keyword table is binary-searched during identifier scanning. When
the lexer reads a token like `enum`, it first scans it as a potential
`NOVA_TOKEN_NAME`, then checks the keyword table and promotes the token
type to `NOVA_TOKEN_ENUM`. This is the same mechanism used for `if`,
`while`, `function`, and all other keywords — no special tokenization
is required.

The compound forms (`typedec enum { }` and `typedec struct { }`) use
the brace characters `{` and `}` as delimiters. These are tokenized as
single-character tokens — `(NovaTokenType)'{'` and `(NovaTokenType)'}'`
— the same tokens used for string interpolation (`${expr}`). The lexer
tracks an `interp_brace_depth` counter to distinguish interpolation
braces from declaration braces, but the parser sees both as the same
token type and disambiguates through context.

---

## 3. Abstract Syntax Tree

The parser produces a row-based (flat-array) AST. Each statement is a
`NovaRowStmt` discriminated by its `NovaStmtType` tag. Three statement
types handle type declarations:

```c
typedef enum {
    /* ... 18 existing statement types ... */
    NOVA_STMT_ENUM,      /* enum Name ... end  |  typedec enum Name { } */
    NOVA_STMT_STRUCT,    /* struct Name ... end | typedec struct Name { } */
    NOVA_STMT_TYPEDEC,   /* typedec Name = type */
    NOVA_STMT_COUNT
} NovaStmtType;
```

### 3.1 Enum AST Node

```c
struct {
    const char  *name;           /* Enum type name            */
    size_t       name_len;
    NovaNameIdx  members_start;  /* Index into names[] pool   */
    int          member_count;
    NovaExtraIdx values_start;   /* Index into expr_extra[]   */
    int          has_values;     /* 1 if any member has = val */
    int          typed;          /* 1 if typedec enum { }     */
} enum_stmt;
```

Member names are stored in the AST's shared `names[]` pool —
a flat array of `NovaRowNameRef` (pointer + length). Member values are
stored in the `expr_extra[]` pool as expression indices
(`NovaExprIdx`), where `NOVA_IDX_NONE` indicates an auto-numbered
member with no explicit value. This pool-based layout avoids per-node
heap allocations and keeps the AST cache-friendly.

The `typed` flag discriminates between `enum ... end` (bare form,
`typed = 0`) and `typedec enum Name { }` (compound form, `typed = 1`).
Both parse to `NOVA_STMT_ENUM` — they differ only in this flag, which
the compiler checks when emitting metadata.

### 3.2 Struct AST Node

```c
struct {
    const char  *name;            /* Struct type name          */
    size_t       name_len;
    NovaNameIdx  fields_start;    /* Field names in names[]    */
    int          field_count;
    NovaExtraIdx defaults_start;  /* Defaults in expr_extra[]  */
    int          typed;           /* 1 if typedec struct { }   */
} struct_stmt;
```

Structurally parallel to enum. Fields replace members, defaults replace
values. A field with no explicit default has `NOVA_IDX_NONE` at its
`defaults_start` position, meaning the parameter will pass through
as-is (which may be `nil` if not supplied).

### 3.3 Typedec AST Node

```c
struct {
    const char *name;        /* Type alias name   */
    size_t      name_len;
    const char *base_type;   /* Base type name    */
    size_t      base_type_len;
} typedec_stmt;
```

The simplest form. No pools, no expression lists — just two interned
strings: the alias name and the base type name.

---

## 4. Parsing

### 4.1 Statement Dispatch

The parser's main statement dispatch recognizes three token types and
routes to five parsing functions:

```c
case NOVA_TOKEN_ENUM:
    novai_advance(P);
    return novai_parse_enum(P);

case NOVA_TOKEN_STRUCT:
    novai_advance(P);
    return novai_parse_struct(P);

case NOVA_TOKEN_TYPEDEC:
    novai_advance(P);
    if (novai_match(P, NOVA_TOKEN_ENUM)) {
        return novai_parse_typedec_enum(P);
    }
    if (novai_match(P, NOVA_TOKEN_STRUCT)) {
        return novai_parse_typedec_struct(P);
    }
    return novai_parse_typedec(P);
```

The `typedec` case uses one-token lookahead: after consuming `typedec`,
it checks whether the next token is `enum` or `struct`. If so, it
delegates to the compound parser. Otherwise, it falls through to the
simple type-alias parser. This is LL(1) — no backtracking required.

### 4.2 Bare Enum Parsing (`enum ... end`)

```
enum Name
    MEMBER1 [= expr]
    MEMBER2 [= expr]
    ...
end
```

The parser:

1. Consumes the name token after `enum`.
2. Enters a loop collecting member names (must be `NOVA_TOKEN_NAME`).
3. For each member, optionally parses `= expr` for an explicit value.
4. Accepts commas and semicolons as optional separators.
5. Expects `end` to close the block.
6. Commits all names and values to the AST pools.
7. Creates a `NOVA_STMT_ENUM` node with `typed = 0`.

Members are collected into stack-local arrays (`temp_names[]`,
`temp_values[]`) before committing to the AST pools. This avoids
fragmented pool allocations and allows the parser to reject malformed
declarations without corrupting pool state. The maximum is
`NOVAI_TEMP_MAX` members per declaration.

### 4.3 Bare Struct Parsing (`struct ... end`)

Identical structure to enum parsing, with fields instead of members and
defaults instead of values. Produces `NOVA_STMT_STRUCT` with
`typed = 0`.

### 4.4 Compound Enum Parsing (`typedec enum Name { }`)

```
typedec enum Name {
    MEMBER1 [= expr],
    MEMBER2 [= expr],
    ...
}
```

When the `typedec` dispatcher matches `enum`, it advances past the
`enum` token and delegates to `novai_parse_typedec_enum()`. This
function is structurally identical to `novai_parse_enum()` with two
differences:

1. **Delimiters:** Expects `{` to open and `}` to close, instead of an
   implicit start and `end`.
2. **Typed flag:** Sets `typed = 1` on the resulting AST node.

Both produce the same `NOVA_STMT_ENUM` statement type. The compiler
handles the distinction.

### 4.5 Compound Struct Parsing (`typedec struct Name { }`)

Same structural mirror: `{`/`}` delimiters, `typed = 1`, same AST node
type as bare struct.

### 4.6 Type Alias Parsing (`typedec Name = type`)

The simplest parser: consumes the name, expects `=`, consumes the base
type name. No loops, no pools, no expressions — just two string
captures. Produces `NOVA_STMT_TYPEDEC`.

---

## 5. Compilation: Enum

The enum compiler (`novai_compile_stmt_enum`) translates a
`NOVA_STMT_ENUM` node into a sequence of table-manipulation
instructions. The result at runtime is a global table variable whose
keys are member names and whose values are integers, strings, or other
constants.

### 5.1 Bytecode Sequence

For an enum with N members, the compiler emits:

```
NEWTABLE   R(t), 0, N          ; Create table with N hash slots
; For each member:
SETTABLE   R(t), RK(key), RK(val)   ; Set table[memberName] = value
; If typed:
SETTABLE   R(t), RK("__type"), RK("EnumName")
SETTABLE   R(t), RK("__base"), RK("enum")
; Store globally:
SETGLOBAL  R(t), K("EnumName")
```

### 5.2 Auto-Increment Counter

The compiler maintains a running counter (`auto_val`, initialized to 0)
that provides values for members without explicit `= expr` assignments.
For each member:

- **Explicit value:** The expression is compiled normally. If it's an
  integer literal, the counter is updated to `literal_value + 1`,
  allowing subsequent auto-members to continue from the explicit value.
  If it's a non-integer expression (string, function call, etc.), the
  counter simply increments by 1.

- **Auto value:** The counter's current value is added to the constant
  pool as an integer constant and used as the value. The counter
  increments.

This produces C-like auto-numbering:

```nova
typedec enum Priority {
    LOW,           -- 0 (auto)
    MEDIUM,        -- 1 (auto)
    HIGH = 10,     -- 10 (explicit, counter resets to 11)
    CRITICAL       -- 11 (auto, continues from 10)
}
```

### 5.3 Constant Pool Pressure

Every member name and value is a constant pool entry. Member names are
string constants (resolved via `nova_proto_find_or_add_string`, which
deduplicates). Auto-numbered values are integer constants. In the RK
encoding used by `SETTABLE`, the 8-bit B and C fields allow at most 128
constant indices (indices 128–255 map to constants 0–127). This means a
single enum is limited to approximately 60 members in the worst case
(each member needs a name constant + a value constant = 2 slots, plus
metadata constants for the typed form).

For enums exceeding this limit, the compiler raises error E1013
("enum member name constant overflow"). In practice, enums with more
than 60 members should be split into related groups, which is better
design regardless of the technical constraint.

### 5.4 Hex Literal Support

The lexer supports hexadecimal integer literals (`0x00`, `0xFF`,
`0xDEAD`). These are parsed as standard `NOVA_TOKEN_INT` values with the
integer representation stored in the token. The parser and compiler
handle them identically to decimal integers — no special code paths.
This makes hex-valued enums natural for systems programming:

```nova
typedec enum OpcodeGroup {
    DATA_MOVE  = 0x00,
    TABLE_OPS  = 0x10,
    ARITHMETIC = 0x30,
    CONTROL    = 0x70,
    CLOSURES   = 0x90
}
```

At runtime, `OpcodeGroup.ARITHMETIC` is simply the integer `48`. The
hex notation is purely a source convenience.

---

## 6. Compilation: Struct

The struct compiler (`novai_compile_stmt_struct`) is more complex than
the enum compiler because structs create **constructor functions** — not
static tables, but closures that build tables on demand.

### 6.1 Constructor Compilation Strategy

A struct declaration:

```nova
typedec struct Vec3 {
    x = 0,
    y = 0,
    z = 0
}
```

compiles to the equivalent of:

```nova
function Vec3(x, y, z)
    dec t = {}
    t.x = (x ~= nil) and x or 0
    t.y = (y ~= nil) and y or 0
    t.z = (z ~= nil) and z or 0
    t.__type = "Vec3"
    t.__base = "struct"   -- only if typed = 1
    return t
end
```

The compiler achieves this by creating a **child function prototype** —
the same mechanism used for nested function definitions and closures.
The child proto is compiled inline, added to the parent's child array,
and instantiated via `CLOSURE`.

### 6.2 Child Prototype Setup

The compiler creates a new `NovaProto` and initializes it with:

- `num_params` = field count (each field becomes a positional parameter)
- `is_vararg` = 0 (fixed arity)
- `source` = current source filename
- `line_defined` = declaration line number

A child `NovaFuncState` is pushed onto the compiler's function state
stack, establishing a new scope for code generation.

### 6.3 Default Value Codegen

For each field with a default value, the compiler emits a conditional
branch sequence:

```
TEST       R(param), 0, 1      ; if param is truthy, skip to direct-set
JMP        +offset              ; jump to direct-set
; Default path:
LOADK      R(temp), K(default)  ; load default value
SETTABLE   R(table), RK(key), R(temp)  ; set field to default
JMP        +offset              ; skip direct-set
; Direct-set:
SETTABLE   R(table), RK(key), R(param)  ; set field from argument
```

This is a TEST-JUMP pattern: `OP_TEST` checks whether the parameter
register is truthy (non-nil and non-false). If truthy, the argument
was provided and is used directly. If falsy (nil — meaning the argument
was omitted), the default value is loaded and used instead.

Fields without defaults skip the conditional entirely:

```
SETTABLE   R(table), RK(key), R(param)
```

Here, if the argument was omitted, the parameter register contains
`nil`, which is exactly what gets stored.

### 6.4 Metadata Injection

After all fields are set, the compiler emits:

```
; Always (both bare and typed):
SETTABLE   R(table), RK("__type"), RK("StructName")

; Typed only (typed = 1):
SETTABLE   R(table), RK("__base"), RK("struct")
```

The `__type` field is always present — even bare structs carry it,
enabling basic type queries on any struct instance. The `__base` field
is only emitted for the `typedec struct` compound form, distinguishing
typed structs from bare ones.

### 6.5 Return and CLOSURE

The child proto ends with `RETURN1 R(table)`, returning the
constructed table. Control returns to the parent scope, which emits:

```
CLOSURE    R(dest), child_index   ; Instantiate constructor closure
SETGLOBAL  R(dest), K("StructName")  ; Store as global
```

At runtime, calling `Vec3(1, 2, 3)` invokes this closure, which
constructs and returns a fresh table with the specified fields. Each
call creates an independent table — struct instances share no mutable
state.

---

## 7. Compilation: Typedec (Type Alias)

The type alias form (`typedec UserId = integer`) compiles to a
single-parameter constructor closure, structurally similar to the struct
constructor but simpler:

### 7.1 Bytecode Sequence

The child prototype emits:

```
NEWTABLE   R(t), 0, 3             ; Create table with 3 hash slots
SETTABLE   R(t), RK("__type"), RK("UserId")    ; Type metadata
SETTABLE   R(t), RK("__base"), RK("integer")   ; Base type name
SETTABLE   R(t), RK("value"),  R(0)             ; Wrapped value
RETURN1    R(t)
```

The parent emits:

```
CLOSURE    R(dest), child_index
SETGLOBAL  R(dest), K("UserId")
```

### 7.2 Wrapper Semantics

A typedec alias wraps a single value in a table with type metadata. At
runtime:

```nova
typedec UserId = integer

dec id = UserId(42)
-- id.value   == 42
-- id.__type  == "UserId"
-- id.__base  == "integer"
```

This is intentionally minimal. The wrapper doesn't validate that the
argument matches the base type (Nova is dynamically typed). The base
type name is documentation, not enforcement — it tells serializers,
debuggers, and human readers what kind of value to expect.

### 7.3 When to Use Typedec vs. Struct

Both produce constructor closures that return tagged tables. The
difference is in the number of fields:

- **Typedec:** One field (`value`), one base type name. Use for
  semantic wrappers around primitives — `UserId`, `Email`, `Score`.
- **Struct:** N fields with named accessors and optional defaults. Use
  for composite records — `Vec3`, `Config`, `HttpRequest`.

The distinction is conceptual, not mechanical. The compiler generates
the same category of bytecode for both.

---

## 8. Compound Forms: typedec enum and typedec struct

The compound forms (`typedec enum Name { }` and `typedec struct Name
{ }`) are syntactic variants that combine the `typedec` keyword with
enum or struct semantics. They exist to provide a C-style declaration
experience with full type metadata.

### 8.1 Syntactic Unification

Internally, compound forms produce the same AST nodes as their bare
counterparts — `NOVA_STMT_ENUM` and `NOVA_STMT_STRUCT`. The only
difference is the `typed` flag, which triggers additional metadata
emission during compilation. This design keeps the compiler simple:
there is one enum compiler and one struct compiler, each with a
conditional branch for the typed case.

The brace delimiters (`{ }`) are an alternative to the `... end` block
syntax used by bare forms. The parser handles both identically — the
member/field collection loop is the same, only the terminator check
differs.

### 8.2 Metadata Comparison

| Declaration Form | `__type` | `__base` | Runtime Type |
|-----------------|----------|----------|--------------|
| `enum ... end` | Set (enum name) | Not set | `"table"` |
| `struct ... end` | Set (struct name) | Not set | `"function"` (constructor) |
| `typedec Name = type` | Set (alias name) | Set (type name) | `"function"` (constructor) |
| `typedec enum Name { }` | Set (enum name) | `"enum"` | `"table"` |
| `typedec struct Name { }` | Set (struct name) | `"struct"` | `"function"` (constructor) |

The `__base` field is the key differentiator. Bare enums and structs
carry `__type` for basic identification, but only the typed forms carry
`__base`, which classifies the structure's *kind*. This enables
generic code that handles "any enum" or "any struct" without knowing the
specific type:

```nova
dec function is_enum(t)
    return type(t) == "table" and t.__base == "enum"
end

dec function is_struct_instance(t)
    return type(t) == "table" and t.__base == "struct"
end
```

---

## 9. Runtime Representation

Type declarations produce standard runtime values — no special object
types, no VM extensions. Understanding the runtime representation
clarifies what you can do with declared types.

### 9.1 Enum at Runtime

An enum is a `NovaTable` stored as a global variable. Its hash part
contains string keys (member names) mapping to constant values
(integers, strings, etc.). For a typed enum, two additional entries
exist: `__type` (the enum name) and `__base` (the string `"enum"`).

The table is not frozen or made read-only. Nova does not enforce enum
immutability at the VM level — you *can* assign new keys to an enum
table at runtime. This is consistent with Nova's dynamically-typed
philosophy: the language provides structure, not constraints. Static
analysis tools and the MCP server can warn about enum mutations; the
runtime does not prevent them.

**Memory cost:** One `NovaTable` allocation (40 bytes header + hash
array). For a 10-member typed enum, the hash part holds 12 entries
(10 members + `__type` + `__base`). At 40 bytes per `DaggerEntry`, the
hash array is approximately 768 bytes after rounding to a power-of-two
capacity (16 slots × 40 bytes = 640 bytes, plus the table header).
Total: approximately 700 bytes per enum.

### 9.2 Struct at Runtime

A struct declaration creates a `NovaClosure` stored as a global
variable. The closure wraps a child `NovaProto` (the constructor
function). Calling the closure returns a new `NovaTable` with the
struct's fields, `__type`, and optionally `__base`.

**Constructor cost:** One `NovaClosure` allocation (header + upvalue
array, typically 40–64 bytes) plus the shared `NovaProto` (which
carries bytecode, constant pool, and debug info — roughly 200–500
bytes depending on field count and default expressions).

**Instance cost:** Each call to the constructor allocates one
`NovaTable`. For a 5-field typed struct, the hash part holds 7 entries
(5 fields + `__type` + `__base`). At minimum capacity (8 slots), the
hash array is 320 bytes plus the table header. Each instance is
independent — no shared state, no prototype chain, no hidden classes.

### 9.3 Typedec Alias at Runtime

A type alias is a `NovaClosure` (like struct) that wraps a single value.
Each call allocates a `NovaTable` with 3 entries (`__type`, `__base`,
`value`). Minimum capacity is 4 hash slots = 160 bytes plus header.

### 9.4 GC Implications

All runtime values created by type declarations are standard GC-managed
objects. The collector traverses enum tables, struct constructor
closures, and struct instances through its normal mark phase — no
special GC support is needed (see [VM Spec §4](NOVA_VM_SPEC.md#4-garbage-collector)).

Constructor closures are rooted through the global table. Struct
instances are rooted through whatever variable or table holds them. When
all references to a struct instance are dropped, the GC collects it in
the next sweep. Enum tables, being globals, persist for the program's
lifetime unless explicitly removed from `_G`.

---

## 10. Practical Implications for Systems Development

Type declarations transform what Nova can express. Before them, a Nova
program's data model was implicit — buried in table constructors and
ad-hoc field assignments scattered across function bodies. With type
declarations, the data model is explicit, inspectable, and
self-documenting.

### 10.1 Protocol and Wire Format Modeling

Systems programs exchange structured data — network packets, IPC
messages, configuration records. Traditional scripting languages force
you to build these structures inline:

```nova
-- Without declarations: shape is implicit
dec msg = {}
msg.type = 3
msg.payload = data
msg.timestamp = os.clock()
```

With type declarations, the shape is defined once, centrally, and every
instance is self-describing:

```nova
typedec enum MessageType {
    HANDSHAKE,
    DATA,
    ACK,
    ERROR
}

typedec struct Message {
    type      = 0,
    payload   = nil,
    timestamp = 0
}

dec msg = Message(MessageType.DATA, data, os.clock())
-- msg.__type == "Message"
-- msg.__base == "struct"
```

A serializer can inspect `msg.__type` to determine how to encode it.
A deserializer can look up the constructor by name to rebuild it. The
enum values provide stable, named constants instead of magic numbers.

### 10.2 State Machines and Finite Automata

Enums provide the vocabulary for state machines — a fundamental
construct in systems software (connection handlers, protocol parsers,
device drivers). The combination of auto-numbering and hex literals
makes them natural for both high-level states and low-level bit fields:

```nova
-- High-level: connection lifecycle
typedec enum ConnState {
    IDLE,
    CONNECTING,
    CONNECTED,
    CLOSING
}

-- Low-level: hardware register flags
typedec enum GpioFlags {
    INPUT     = 0x00,
    OUTPUT    = 0x01,
    PULL_UP   = 0x02,
    PULL_DOWN = 0x04,
    OPEN_DRAIN = 0x08
}
```

### 10.3 Domain-Driven Design

Typedec aliases provide the lightest possible boundary between
primitive types and domain concepts. In systems code, a `uint16_t` might
be a port number, a process ID, a file descriptor, or a signal number.
They're all integers, but confusing one for another is a bug:

```nova
typedec PortNumber = integer
typedec ProcessId  = integer
typedec Signal     = integer
```

Nova won't prevent you from passing a `PortNumber` where a `ProcessId`
is expected (dynamic typing). But a function can inspect its argument's
`__type` at the boundary and raise a clear error:

```nova
dec function kill_process(pid, sig)
    if pid.__type != "ProcessId" then
        error("expected ProcessId, got " .. tostring(pid.__type))
    end
    -- ...
end
```

This is defensive programming, not static typing. It works at system
boundaries — the places where bugs actually occur.

### 10.4 Enum-Struct Composition

The most powerful pattern combines enums and structs: enums define the
vocabulary, structs define the shape. Together they model domains with
the expressiveness of a systems language:

```nova
typedec enum EventKind {
    CONNECT,
    DISCONNECT,
    MESSAGE,
    ERROR
}

typedec struct Event {
    kind      = 0,
    timestamp = 0,
    source    = "unknown",
    data      = nil
}
```

This pattern — variants for classification, records for data — is the
foundation of event systems, command patterns, protocol handlers, and
message buses. Nova can now express all of these with named, typed,
self-describing values.

### 10.5 What This Doesn't Do

Type declarations are not:

- **Static type checking.** The compiler does not verify that a struct
  argument matches its field type. Arguments are positional, not typed.
- **Sealed or frozen.** Enum tables and struct instances can be mutated
  at runtime. There is no `const` or `readonly` modifier on tables.
- **Pattern-matchable.** Nova does not provide `match` or `switch` over
  type-declared values (though it could be built as a library function
  over `__type` inspection).
- **Inheritable.** Structs do not have prototype chains. Use
  metatables (see [VM Spec §6](NOVA_VM_SPEC.md#6-metatables-and-metamethods))
  for inheritance.

These are deliberate omissions. Each could be added incrementally
without changing the existing compilation model, because the runtime
representation (tables and closures) is flexible enough to support any
of these extensions. The current design provides the highest leverage
for the lowest implementation cost.

---

## 11. Limits and Constants

| Parameter | Value | Source |
|-----------|-------|--------|
| Max members per enum | ~60 | RK constant limit (128 constants, 2 per member) |
| Max fields per struct | ~60 | Same RK constant limit |
| Max member/field name length | Unlimited | Interned via Weave |
| Member name format | `NOVA_TOKEN_NAME` | Alphanumeric + underscore |
| Default value expressions | Any expression | Compiled by standard expression compiler |
| Auto-increment initial value | 0 | Matches C enum convention |
| Auto-increment after explicit | `explicit + 1` | Only for integer literals |
| Temp buffer size | `NOVAI_TEMP_MAX` | Compile-time constant (parser) |
| `__type` key | Always present | All enum/struct/typedec forms |
| `__base` key | Typed + typedec only | `"enum"`, `"struct"`, or base type name |

---

*Nova is a Zorya Corporation project. MIT License.*
