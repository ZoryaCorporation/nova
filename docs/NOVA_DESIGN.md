# NOVA Language Design Document

**Project**: Nova Programming Language  
**Version**: 0.1.0 (Genesis)  
**Author**: Anthony Taliento  
**Organization**: Zorya Corporation  
**Date**: February 5, 2026  
**Status**: Active Development  
**License**: MIT

---

## 1. VISION

Nova is a lightweight, embeddable scripting language built from scratch by Zorya Corporation. It preserves Lua's elegant syntax and philosophy -- small, fast, embeddable -- while addressing real-world shortcomings that have plagued Lua for decades:

- **No preprocessor** → Nova has `#include`, `#define`, macros, conditionals
- **No cross-platform bytecode** → Nova's `.no` format is portable by design
- **Painful C API** → Nova's C API is clean, typed, and intuitive
- **Primitive string handling** → Nova uses Weave (industrial-strength strings)
- **Slow hash tables** → Nova uses DAGGER (O(1) Robin Hood + Cuckoo)
- **No standard module system** → Nova has first-class `import`/`export`
- **1-indexed arrays** → Nova supports 0-indexed (configurable, default 0!)

Nova is NOT a Lua fork. It shares Lua's spirit, not its code.

---

## 2. FILE EXTENSIONS

| Extension | Purpose | Analogy |
|-----------|---------|---------|
| `.n`      | Nova source file | `.c` / `.lua` |
| `.m`      | Nova macro/header file | `.h` |
| `.no`     | Nova Object (compiled bytecode) | `.o` / `.luac` |
| `.lua`    | Legacy Lua source (compatibility mode) | - |

### File Relationships

```
  math.m          -- Macro definitions, type hints, constants
     |
     v
  app.n           -- Source file, #include "math.m"
     |
     v
  [Lexer → PP → Parser → AST → Compiler]
     |
     v
  app.no          -- Portable binary object
     |
     v
  [Nova VM]       -- Executes .no OR .n directly
```

---

## 3. ARCHITECTURE

### 3.1 Pipeline

```
Source (.n/.m)
    |
    v
 [LEXER]         -- Tokenization (nova_lex.c)
    |
    v
 [PREPROCESSOR]  -- #include, #define, macros (nova_pp.c)
    |
    v
 [PARSER]        -- Token stream → Row AST (nova_parse_row.c)
    |
    v
 [ROW AST]       -- Flat array-indexed syntax tree (nova_ast_row.c)
    |
    v
 [COMPILER]      -- Row AST → Bytecode (nova_compile.c)
    |
    v
 [OPTIMIZER]     -- Constant fold, dead code, peephole (nova_opt.c)
    |
    v
 [CODEGEN]       -- Bytecode emission (nova_codegen.c)
    |
    v
 Bytecode (.no)
    |
    v
 [VM]            -- Register-based execution (nova_vm.c)
```

Lua collapses Parser→Compiler into a single pass with no AST. We deliberately
add an AST stage for:
- Optimization passes (constant folding, dead code elimination)
- Better error messages with source location tracking
- Future: type inference, lint, static analysis
- Macro expansion operates on token streams, not raw text

### 3.2 Row-Based AST Architecture

The AST uses a **flat array-indexed** design instead of heap-allocated pointer
trees. All nodes live in typed pools backed by `zorya_arena`, yielding:

- **Cache locality**: Sequential pool access instead of pointer chasing
- **O(1) bulk destroy**: Free the arena, done (no tree walk)
- **Compact indices**: `uint32_t` indices (4 bytes) vs pointers (8 bytes on x64)
- **Deterministic layout**: Nodes allocated in parse order

**Pool Architecture**:

```
NovaASTTable
├── exprs[]        -- NovaRowExpr pool (12 tagged union variants)
├── stmts[]        -- NovaRowStmt pool (16 variants, next-linked)
├── blocks[]       -- NovaRowBlock pool (first_stmt index)
├── extras[]       -- NovaExtraIdx pool (overflow child lists)
├── fields[]       -- NovaRowTableField pool (table constructors)
├── params[]       -- NovaRowParam pool (function parameters)
├── branches[]     -- NovaRowIfBranch pool (if/elseif chains)
├── names[]        -- NovaRowNameRef pool (variable name refs)
└── arena          -- zorya_arena backing all allocations
```

**Index Types**: Each pool uses a distinct `uint32_t` typedef to prevent
cross-pool index confusion at the API level. `NOVA_IDX_NONE` (0xFFFFFFFF)
serves as the universal null sentinel.

**Statement Linking**: Statements within a block form an index-based linked
list via `stmt.next`, eliminating the need for dynamic child arrays. A
`NovaRowBlock` stores only `first_stmt` -- walk the chain to traverse.

**Auxiliary Pools**: Variable-length data (function params, table fields,
if/elseif branches, multi-name references) use dedicated pools with
reservation functions that return contiguous index ranges.

### 3.3 Component Overview

| Component | File(s) | Zorya Asset Used | Status |
|-----------|---------|-----------------|--------|
| Lexer | `nova_lex.c` (25KB obj) | PCM (branch hints, bit ops) | **DONE** |
| Preprocessor | `nova_pp.c` (34KB obj) | DAGGER (macro table), Weave (string ops) | **DONE** |
| Parser (pointer) | `nova_parse.c` (32KB obj) | PCM | **DONE** (legacy) |
| Parser (row) | `nova_parse_row.c` (29KB obj) | PCM | **DONE** (primary) |
| AST (pointer) | `nova_ast.h` | Arena allocator | **DONE** (legacy) |
| AST (row) | `nova_ast_row.h/.c` (8KB obj) | zorya_arena (pool backing) | **DONE** |
| Compiler | `nova_compile.c` (2,632 lines) | Row AST, proto emission | **DONE** |
| Optimizer | `nova_opt.c` (1,296 lines) | 7 passes: peephole, const fold, dead code, etc. | **DONE** |
| Code Generator | `nova_codegen.c` (1,274 lines) | NXH64 (integrity checksum) | **DONE** |
| Proto system | `nova_proto.c` (684 lines) | Instruction/constant pool management | **DONE** |
| VM | `nova_vm.c` (2,891 lines) | DAGGER (globals/env), NXH, PCM (computed goto) | **DONE** |
| Metamethods | `nova_meta.c` (1,137 lines) | 13 metamethods dispatched | **DONE** |
| Std Libraries | `nova_lib_*.c` (7 files, ~4,400 lines) | 81 functions across 7 modules | **DONE** |
| GC | `nova_gc.c` | PCM (bit ops for mark bits) | **NOT STARTED** |
| C API | `nova_api.c` | All of the above | **PARTIAL** (headers) |
| Error Catalog | `nova_error_catalog.c` | Diagnostic codes + explanations | **NOT STARTED** |

---

## 4. LANGUAGE SYNTAX

Nova preserves Lua's clean syntax with targeted enhancements.

### 4.1 printf - First-Class Formatted Output

One of Lua's biggest ergonomic failures: no built-in printf. Nova fixes this.

```lua
-- printf: format and print to stdout (C printf semantics)
local name = "Nova"
local version = 0.1
local hp = 100

printf("Welcome to %s v%.1f\n", name, version)
printf("Player HP: %d/%d\n", hp, hp)
printf("Hex: 0x%08X  Oct: %o\n", 255, 255)
printf("Padded: [%-20s]\n", "left-aligned")
printf("Precision: %.6f\n", 3.14159265)

-- sprintf: format and return string (no print)
local msg = sprintf("[%s] Level %d", name, 42)

-- fprintf: format and print to file
local f = io.open("log.txt", "w")
fprintf(f, "Timestamp: %d\n", os.time())
f:close()

-- The Lua way (ugly, still supported for compat):
print(string.format("x = %d", 42))  -- works but why would you
```

Supported format specifiers: `%d`, `%i`, `%u`, `%f`, `%e`, `%g`, `%x`, `%X`,
`%o`, `%s`, `%c`, `%%`, with width, precision, and flag modifiers (`-`, `+`,
`0`, `#`, space).

### 4.2 Familiar Lua Patterns (Preserved)

```lua
-- Variables and types
local x = 42
local name = "Nova"
local t = { "alpha", "bravo", "charlie" }

-- Functions
function greet(name)
    print("Hello, " .. name)
end

-- First-class functions
local add = function(a, b) return a + b end

-- Tables (the universal data structure)
local player = {
    name = "Nova",
    hp = 100,
    items = { "sword", "shield" },
}

-- Control flow
if x > 0 then
    print("positive")
elseif x < 0 then
    print("negative")
else
    print("zero")
end

-- Loops
for i = 0, 9 do        -- NOTE: 0-indexed by default!
    print(i)
end

for key, value in pairs(t) do
    print(key, value)
end

while condition do
    -- body
end

repeat
    -- body
until condition

-- Multiple returns
function divide(a, b)
    if b == 0 then return nil, "division by zero" end
    return a / b
end

-- Coroutines
local co = coroutine.create(function()
    for i = 0, 9 do
        coroutine.yield(i)
    end
end)

-- Metatables and OOP
local Vector = {}
Vector.__index = Vector

function Vector.new(x, y)
    return setmetatable({ x = x, y = y }, Vector)
end

function Vector:magnitude()
    return math.sqrt(self.x^2 + self.y^2)
end
```

### 4.3 Nova Enhancements

#### Preprocessor (New)

```c
-- File: math_utils.m (Nova macro/header file)

#ifndef MATH_UTILS_M
#define MATH_UTILS_M

#define PI          3.14159265358979
#define TAU         (PI * 2)
#define DEG2RAD(d)  ((d) * PI / 180.0)
#define RAD2DEG(r)  ((r) * 180.0 / PI)

-- Constants visible to includers
EPSILON = 1e-10

-- Function declarations (for documentation / static checking)
-- These are optional hints, not enforced types
--@param radius number
--@return number
function circle_area(radius) end

#endif
```

```lua
-- File: game.n (Nova source file)

#include "math_utils.m"

#define MAX_ENTITIES 1024
#define DEBUG 1

#ifdef DEBUG
    #define LOG(msg)  print("[DEBUG] " .. msg)
#else
    #define LOG(msg)
#endif

local entities = {}

function spawn_entity(x, y)
    if #entities >= MAX_ENTITIES then
        LOG("Entity limit reached!")
        return nil
    end

    local angle = DEG2RAD(45)
    -- ...
end
```

#### Module System (Enhanced)

```lua
-- File: vector.n

local Vector = {}
Vector.__index = Vector

function Vector.new(x, y)
    return setmetatable({ x = x or 0, y = y or 0 }, Vector)
end

function Vector:dot(other)
    return self.x * other.x + self.y * other.y
end

-- Explicit export
return Vector
```

```lua
-- File: game.n

local Vector = require("vector")    -- Standard require (Lua compatible)

-- Or Nova-style import (planned)
-- import Vector from "vector"

local v = Vector.new(3, 4)
```

#### String Interpolation (New)

```lua
local name = "Nova"
local version = "0.1.0"

-- Backtick strings support interpolation
local msg = `Welcome to ${name} v${version}!`

-- Multi-line strings (Lua-compatible long strings preserved)
local text = [[
This is a
multi-line string
]]

-- Backtick multi-line with interpolation
local html = `
<h1>${title}</h1>
<p>${body}</p>
`
```

#### Integer Division and Bitwise (Clean)

```lua
-- Integer division
local q = 7 // 2      -- 3 (floor division, Lua 5.3+)

-- Bitwise operators (Lua 5.3+ syntax)
local flags = 0xFF & mask
local shifted = value << 4
local toggled = flags ~ 0x01
`
```

#### 0-Indexed Arrays (Default)

```lua
-- Nova defaults to 0-indexed
local arr = { "zero", "one", "two" }
print(arr[0])    -- "zero"
print(#arr)      -- 3

-- ipairs starts at 0
for i, v in ipairs(arr) do
    print(i, v)  -- 0 "zero", 1 "one", 2 "two"
end

-- Numeric for: 0-based range
for i = 0, #arr - 1 do
    print(arr[i])
end
```

#### Async / Await / Spawn / Yield (New)

Nova extends Lua's coroutine model with first-class concurrency keywords.
These are syntactic sugar over a cooperative scheduler -- no OS threads, no
preemption, no data races. The programmer controls when context switches happen.

```lua
-- async function declaration
async function fetch_data(url)
    local response = await http.get(url)
    return response.body
end

-- await suspends until the async operation completes
local data = await fetch_data("https://api.example.com/users")

-- spawn launches a task without blocking the caller
spawn process_background(data)

-- yield in async context: cooperative multitasking
async function producer(channel)
    for i = 0, 99 do
        yield channel, i    -- yield a value to the channel
    end
end

-- async anonymous functions
local handler = async function(event)
    local result = await process(event)
    return result
end

-- async methods
function Server:handle(request)
    local body = await self:parse_body(request)
    local result = await self:dispatch(body)
    return result
end
```

**Semantics**:
- `async function` marks a function as returning a task/future
- `await expr` suspends the current task until `expr` resolves
- `spawn expr` creates a detached task (fire-and-forget)
- `yield` within async context yields control to the scheduler
- All concurrency is cooperative -- `await` and `yield` are the only
  preemption points

**Implementation**: Built on Nova's coroutine infrastructure. `async` functions
are compiled to coroutine constructors. `await` compiles to
`coroutine.resume()` with automatic error propagation. `spawn` creates a new
coroutine and registers it with the task scheduler.

---

## 5. BYTECODE FORMAT (.no)

### 5.1 Design Goals

1. **Cross-platform**: Explicit endianness (little-endian canonical), no native sizes
2. **Versioned**: Header contains format version for forward compatibility
3. **Streamable**: Can begin execution before full file is loaded
4. **Debug-optional**: Debug symbols in separate section, strippable
5. **Compact**: Variable-length encoding for small values

### 5.2 File Layout

```
+----------------------------------+
| Magic Number (4 bytes)           |  "NOVA" = 0x4E4F5641
+----------------------------------+
| Format Version (2 bytes)         |  Major.Minor
+----------------------------------+
| Flags (2 bytes)                  |  Endian, debug, strip info
+----------------------------------+
| Platform Tag (4 bytes)           |  0x00000000 = portable
+----------------------------------+
| Timestamp (8 bytes)              |  Unix epoch, compilation time
+----------------------------------+
| NXH Checksum (8 bytes)           |  NXH64 of all following data
+----------------------------------+
| String Table                     |  Interned strings
|   Count (4 bytes)                |
|   [len:u16, data:u8[]]...        | Variable-length UTF-8 strings
+----------------------------------+
| Constant Pool                    |  Numbers, booleans, nil
|   Count (4 bytes)                |
|   [type:u8, data:var]...         |
+----------------------------------+
| Function Prototypes              |  Compiled functions
|   Count (4 bytes)                |
|   [FunctionProto]...             |
+----------------------------------+
| Debug Section (optional)         |  Line maps, local names
|   [DebugInfo]...                 |
+----------------------------------+
| EOF Marker (4 bytes)             |  0xDEADNOVA
+----------------------------------+
```

### 5.3 Instruction Encoding

32-bit fixed-width instructions (like Lua 5.x):

```
  31       24 23       16 15        8 7         0
  +----------+----------+----------+----------+
  | Opcode   |    A     |    B     |    C     |  ABC format
  +----------+----------+----------+----------+
  | Opcode   |    A     |       Bx (16-bit)   |  ABx format
  +----------+----------+----------+----------+
  | Opcode   |    A     |       sBx (signed)  |  AsBx format
  +----------+----------+----------+----------+
  | Opcode   |          Ax (24-bit)            |  Ax format
  +----------+----------+----------+----------+
```

**Opcode**: 8 bits = 256 possible instructions  
**A register**: 8 bits = 256 registers  
**B, C fields**: 8 bits each or combined into 16-bit Bx/sBx

### 5.4 Core Opcodes (Preliminary)

```
-- Data Movement
NOVA_OP_MOVE        R(A) = R(B)
NOVA_OP_LOADK       R(A) = K(Bx)           -- Load constant
NOVA_OP_LOADNIL     R(A) = nil
NOVA_OP_LOADBOOL    R(A) = (bool)B; if C: pc++
NOVA_OP_LOADINT     R(A) = sBx             -- Load small integer

-- Table Operations
NOVA_OP_NEWTABLE    R(A) = {} (size hints: B=array, C=hash)
NOVA_OP_GETTABLE    R(A) = R(B)[R(C)]
NOVA_OP_SETTABLE    R(A)[R(B)] = R(C)
NOVA_OP_GETFIELD    R(A) = R(B)[K(C)]      -- Optimized string key
NOVA_OP_SETFIELD    R(A)[K(B)] = R(C)

-- Upvalue / Global / Environment
NOVA_OP_GETUPVAL    R(A) = U(B)
NOVA_OP_SETUPVAL    U(B) = R(A)
NOVA_OP_GETGLOBAL   R(A) = G[K(Bx)]
NOVA_OP_SETGLOBAL   G[K(Bx)] = R(A)

-- Arithmetic
NOVA_OP_ADD         R(A) = R(B) + R(C)
NOVA_OP_SUB         R(A) = R(B) - R(C)
NOVA_OP_MUL         R(A) = R(B) * R(C)
NOVA_OP_DIV         R(A) = R(B) / R(C)
NOVA_OP_IDIV        R(A) = R(B) // R(C)    -- Integer division
NOVA_OP_MOD         R(A) = R(B) % R(C)
NOVA_OP_POW         R(A) = R(B) ^ R(C)
NOVA_OP_UNM         R(A) = -R(B)

-- Bitwise
NOVA_OP_BAND        R(A) = R(B) & R(C)
NOVA_OP_BOR         R(A) = R(B) | R(C)
NOVA_OP_BXOR        R(A) = R(B) ~ R(C)
NOVA_OP_BNOT        R(A) = ~R(B)
NOVA_OP_SHL         R(A) = R(B) << R(C)
NOVA_OP_SHR         R(A) = R(B) >> R(C)

-- String
NOVA_OP_CONCAT      R(A) = R(B) .. R(C)
NOVA_OP_STRLEN      R(A) = #R(B)

-- Comparison (sets condition flag, optionally skips)
NOVA_OP_EQ          if (R(B) == R(C)) ~= A then pc++
NOVA_OP_LT          if (R(B) <  R(C)) ~= A then pc++
NOVA_OP_LE          if (R(B) <= R(C)) ~= A then pc++

-- Logical
NOVA_OP_NOT         R(A) = not R(B)
NOVA_OP_TEST        if (bool)R(A) ~= C then pc++
NOVA_OP_TESTSET     if (bool)R(B) ~= C then R(A)=R(B) else pc++

-- Control Flow
NOVA_OP_JMP         pc += sBx
NOVA_OP_CALL        R(A)(R(A+1), ..., R(A+B-1));  results in R(A)..R(A+C-2)
NOVA_OP_TAILCALL    return R(A)(R(A+1), ..., R(A+B-1))
NOVA_OP_RETURN      return R(A), ..., R(A+B-2)

-- Loops
NOVA_OP_FORPREP     R(A) -= R(A+2); pc += sBx
NOVA_OP_FORLOOP     R(A) += R(A+2); if R(A) <= R(A+1) then pc += sBx; R(A+3) = R(A)
NOVA_OP_TFORCALL    R(A+3),...= R(A)(R(A+1), R(A+2))
NOVA_OP_TFORLOOP    if R(A+1) ~= nil then R(A) = R(A+1) else pc++

-- Closures
NOVA_OP_CLOSURE     R(A) = closure(proto[Bx])
NOVA_OP_VARARG      R(A), ..., R(A+B-2) = vararg

-- Special
NOVA_OP_NOP         No operation
NOVA_OP_DEBUG       Debug breakpoint hook
```

---

## 6. OBJECT MODEL

### 6.1 Value Representation

Nova uses NaN-boxing for compact 64-bit value representation:

```
Doubles:  Normal IEEE 754 doubles (when not a NaN)
Encoded:  Use quiet NaN space to encode other types

  63    52 51 50 49 48 47                          0
  +-------+--+--+--+--+-----------------------------+
  |  NaN  | Q| S|  Tag |       Payload (48 bits)    |
  +-------+--+--+--+--+-----------------------------+

  When bits 62:52 = 0x7FF (NaN exponent) and bit 51 (quiet) = 1:
    Tag 0b00 = Pointer (48-bit address)
    Tag 0b01 = Integer (48-bit signed)
    Tag 0b10 = Boolean / Nil / Special
    Tag 0b11 = Reserved (future: lightweight userdata)
```

This gives us:
- **Double**: Full 64-bit IEEE 754 doubles (no overhead)
- **Pointer**: 48-bit pointers (covers all current architectures)
- **Integer**: 48-bit signed integers (-140T to +140T)
- **Boolean/Nil**: Encoded in tag + payload
- **Strings**: Pointer to Weave/Tablet interned string
- **Tables**: Pointer to DAGGER-backed table object
- **Functions**: Pointer to closure/function proto
- **Userdata**: Pointer to user-allocated data

### 6.2 Type Tags

```c
typedef enum {
    NOVA_TYPE_NIL       = 0,
    NOVA_TYPE_BOOL      = 1,
    NOVA_TYPE_INTEGER   = 2,
    NOVA_TYPE_NUMBER    = 3,   /* IEEE 754 double */
    NOVA_TYPE_STRING    = 4,   /* Weave/Tablet interned */
    NOVA_TYPE_TABLE     = 5,   /* DAGGER-backed */
    NOVA_TYPE_FUNCTION  = 6,   /* Closure or C function */
    NOVA_TYPE_USERDATA  = 7,   /* User-managed pointer */
    NOVA_TYPE_THREAD    = 8,   /* Coroutine */
    NOVA_TYPE_CFUNCTION = 9,   /* Raw C function pointer */

    NOVA_TYPE_COUNT     = 10
} NovaType;
```

### 6.3 String Interning

All strings in Nova are interned via Weave's Tablet:

- **Short strings** (<=40 bytes): Always interned, pointer comparison for equality
- **Long strings**: Lazily interned on first comparison, hashed via NXH64
- **Concatenation**: Uses Weave's Cord for deferred concatenation (huge perf win for `..` chains)

### 6.4 Tables

Nova tables are backed by DAGGER:

- **Array part**: Contiguous C array for integer keys [0..n]
- **Hash part**: DAGGER table for string/mixed keys
- **Robin Hood + Cuckoo**: O(1) average lookup, bounded worst case
- **Growth**: Power-of-2 sizing with 75% load factor threshold

---

## 7. PREPROCESSOR

### 7.1 Design Philosophy

Nova's preprocessor operates on **tokens**, not raw text. This means:
- Macros are hygienic by default (no accidental name capture)
- Error messages point to original source locations
- Macro expansion is type-aware (not blind text substitution)

### 7.2 Directives

```
#include "file.m"           Include macro/header file
#include <stdlib>           Include standard library header
#define NAME value          Object-like macro
#define NAME(a,b) expr      Function-like macro
#undef NAME                 Remove macro definition
#ifdef NAME                 Conditional: defined?
#ifndef NAME                Conditional: not defined?
#if expr                    Conditional: expression
#elif expr                  Else-if
#else                       Else
#endif                      End conditional
#pragma name value          Implementation hints
#error "message"            Compilation error
#warning "message"          Compilation warning
#line N "file"              Set line/file for diagnostics
```

### 7.3 Macro Expansion

```lua
-- Object-like macros
#define VERSION "0.1.0"
#define MAX_HEALTH 100

-- Function-like macros
#define MIN(a, b)   ((a) < (b) and (a) or (b))
#define MAX(a, b)   ((a) > (b) and (a) or (b))
#define CLAMP(x, lo, hi)  MIN(MAX(x, lo), hi)

-- Stringification
#define STR(x) #x
#define TOSTR(x) STR(x)

-- Concatenation (token pasting)
#define PASTE(a, b) a ## b
-- PASTE(my, _var) => my_var

-- Variadic macros
#define LOG(fmt, ...) print(string.format(fmt, __VA_ARGS__))
```

### 7.4 Predefined Macros

```
__NOVA__            Always defined (1)
__NOVA_VERSION__    Version string "0.1.0"
__NOVA_MAJOR__      Major version (0)
__NOVA_MINOR__      Minor version (1)
__NOVA_PATCH__      Patch version (0)
__FILE__            Current source filename
__LINE__            Current line number
__DATE__            Compilation date "Feb 05 2026"
__TIME__            Compilation time "14:30:00"
__FUNC__            Current function name (in function context)
__COUNTER__         Unique incrementing integer
```

---

## 8. VIRTUAL MACHINE

### 8.1 Design

- **Register-based**: Like Lua 5.x, not stack-based
- **256 registers per frame**: 8-bit register addressing
- **32-bit fixed-width instructions**: Simple decode, cache-friendly
- **Computed goto dispatch** (GCC/Clang) with switch fallback
- **Incremental generational GC**: Low-latency, tunable

### 8.2 Execution Modes

```
nova script.n           -- Lex → PP → Parse → Compile → Execute (JIT pipeline)
nova script.no          -- Load bytecode → Execute
nova -c script.n        -- Compile to .no only
nova -e "print(42)"     -- Execute string
nova                    -- Interactive REPL
nova -i script.n        -- Execute then enter REPL
```

### 8.3 Garbage Collector

Tri-color incremental mark-sweep with generational optimization:

- **White**: Unreached objects (candidates for collection)
- **Gray**: Reached but children not yet scanned
- **Black**: Fully scanned, all children reached

Generational:
- **Young generation**: Collected frequently (most objects die young)
- **Old generation**: Collected rarely (survivors promoted)
- **Incremental steps**: GC work interleaved with mutator execution
- **Tunable**: `collectgarbage("setpause", N)` etc.

---

## 9. C API

### 9.1 Design Principles

1. **Explicit over implicit**: No hidden state mutations
2. **Type-safe wrappers**: `nova_get_string()` not `nova_tostring(-1)`
3. **Error handling**: Return codes + optional error callbacks
4. **Zero-copy where possible**: String references, not copies
5. **Embeddable**: Minimal footprint, no global state

### 9.2 Core API Shape

```c
/* State lifecycle */
NovaState *nova_open(void);
NovaState *nova_open_with(const NovaConfig *config);
void       nova_close(NovaState *N);

/* Execution */
nova_status_t nova_load_file(NovaState *N, const char *path);
nova_status_t nova_load_string(NovaState *N, const char *source);
nova_status_t nova_load_bytecode(NovaState *N, const void *data, size_t len);
nova_status_t nova_call(NovaState *N, int nargs, int nresults);
nova_status_t nova_pcall(NovaState *N, int nargs, int nresults);

/* Value operations */
void        nova_push_nil(NovaState *N);
void        nova_push_bool(NovaState *N, int b);
void        nova_push_integer(NovaState *N, nova_int_t value);
void        nova_push_number(NovaState *N, nova_number_t value);
void        nova_push_string(NovaState *N, const char *s);
void        nova_push_lstring(NovaState *N, const char *s, size_t len);
void        nova_push_cfunction(NovaState *N, nova_cfunc_t fn);

NovaType    nova_type(NovaState *N, int idx);
const char *nova_typename(NovaState *N, int idx);
int         nova_is_nil(NovaState *N, int idx);
int         nova_is_bool(NovaState *N, int idx);
int         nova_is_number(NovaState *N, int idx);
int         nova_is_string(NovaState *N, int idx);
int         nova_is_table(NovaState *N, int idx);
int         nova_is_function(NovaState *N, int idx);

nova_int_t    nova_to_integer(NovaState *N, int idx);
nova_number_t nova_to_number(NovaState *N, int idx);
const char   *nova_to_string(NovaState *N, int idx, size_t *len);
int           nova_to_bool(NovaState *N, int idx);

/* Table operations */
void        nova_new_table(NovaState *N);
void        nova_new_table_sized(NovaState *N, int narr, int nhash);
void        nova_get_table(NovaState *N, int idx);
void        nova_set_table(NovaState *N, int idx);
void        nova_get_field(NovaState *N, int idx, const char *key);
void        nova_set_field(NovaState *N, int idx, const char *key);
int         nova_next(NovaState *N, int idx);
size_t      nova_raw_len(NovaState *N, int idx);

/* Global table */
void        nova_get_global(NovaState *N, const char *name);
void        nova_set_global(NovaState *N, const char *name);

/* Stack management */
int         nova_get_top(NovaState *N);
void        nova_set_top(NovaState *N, int idx);
void        nova_pop(NovaState *N, int n);
void        nova_push_value(NovaState *N, int idx);
void        nova_remove(NovaState *N, int idx);
void        nova_insert(NovaState *N, int idx);
void        nova_replace(NovaState *N, int idx);
int         nova_check_stack(NovaState *N, int n);

/* Metatables */
int         nova_get_metatable(NovaState *N, int idx);
int         nova_set_metatable(NovaState *N, int idx);

/* Error handling */
int         nova_error(NovaState *N);
int         nova_error_fmt(NovaState *N, const char *fmt, ...);
void        nova_set_error_handler(NovaState *N, nova_error_fn handler, void *ctx);

/* Modules */
void        nova_open_libs(NovaState *N);
void        nova_open_lib(NovaState *N, const char *name, const NovaReg *lib);

/* GC control */
int         nova_gc(NovaState *N, int what, int data);
```

### 9.3 Embedding Example

```c
#include <nova/nova.h>

int main(void) {
    NovaState *N = nova_open();
    if (N == NULL) {
        fprintf(stderr, "Failed to create Nova state\n");
        return 1;
    }

    nova_open_libs(N);

    nova_status_t status = nova_load_file(N, "app.n");
    if (status != NOVA_OK) {
        fprintf(stderr, "Error: %s\n", nova_to_string(N, -1, NULL));
        nova_close(N);
        return 1;
    }

    status = nova_pcall(N, 0, 0);
    if (status != NOVA_OK) {
        fprintf(stderr, "Runtime error: %s\n", nova_to_string(N, -1, NULL));
    }

    nova_close(N);
    return 0;
}
```

---

## 10. STANDARD LIBRARY

Batteries included, but all optional (embeddable = minimal by default).

| Module | Description | Backing |
|--------|-------------|---------|
| `base` | `print`, `printf`, `sprintf`, `type`, `tostring`, `tonumber`, `error`, `pcall`, `assert` | Core |
| `string` | String manipulation, patterns, formatting | Weave |
| `table` | Table operations, sort, insert, remove | DAGGER |
| `math` | Math functions, constants, random | libm |
| `io` | File I/O, stdin/stdout/stderr | POSIX/C stdio |
| `os` | OS functions, clock, time, env | POSIX |
| `coroutine` | Coroutine creation, resume, yield | Core |
| `debug` | Debug hooks, traceback, info | Core |
| `package` | Module loading, require, search paths | Core |
| `bit` | Bitwise operations (also available as operators) | PCM |
| `json` | JSON encode/decode (new) | Weave+DAGGER |
| `regex` | Regular expressions (new, beyond Lua patterns) | Weave |
| `hash` | Hashing utilities (new) | NXH |
| `net` | Basic networking (planned) | POSIX sockets |
| `csv` | CSV/TSV read/write (new) | Weave+DAGGER |
| `task` | Task runner / build orchestration (new) | Core+io |

---

## 11. DATA PROCESSING

Nova treats structured data as a first-class concern. Where other scripting
languages require `import json` or npm packages for basic data wrangling, Nova
ships with built-in, zero-dependency support for the formats that dominate
real-world scripting: JSON, CSV, and TSV.

This is the "glue" that makes Nova viable as a daily-driver scripting language.
If you can't parse a config file or munge a data export without reaching for
external dependencies, the language is a toy. Nova is not a toy.

### 11.1 JSON (Built-In)

```lua
local json = require("json")

-- Decode: string → Nova table
local data = json.decode('{"name": "Nova", "version": 0.1}')
print(data.name)       -- "Nova"
print(data.version)    -- 0.1

-- Encode: Nova table → string
local str = json.encode({ name = "Nova", tags = { "fast", "embeddable" } })
print(str)  -- {"name":"Nova","tags":["fast","embeddable"]}

-- Pretty print
local pretty = json.encode(data, { indent = 2 })

-- Streaming decode for large files
local stream = json.stream(io.open("huge.json"))
for item in stream:items() do
    process(item)
end

-- File convenience
local config = json.load("config.json")         -- file → table
json.save("output.json", results, { indent = 2 }) -- table → file
```

**Implementation**: JSON decode is a hand-written recursive descent parser
(no generated code, no dependencies). Objects decode to DAGGER-backed Nova
tables. Strings are Weave-interned. Numbers use Nova's standard NaN-boxing.
Encode walks the table and emits via Weave Cord for O(1) concatenation.

**Conformance**: RFC 8259 compliant. Rejects duplicate keys by default
(configurable). Handles UTF-8 escape sequences. Limits nesting depth to 512
(configurable) to prevent stack overflow on malicious input.

### 11.2 CSV / TSV (Built-In)

```lua
local csv = require("csv")

-- Read CSV with automatic header detection
local data = csv.load("employees.csv")
for _, row in ipairs(data) do
    printf("%s earns $%s\n", row.name, row.salary)
end

-- Read TSV (tab-separated)
local tsv_data = csv.load("data.tsv", { delimiter = "\t" })

-- Read with explicit options
local data = csv.load("data.csv", {
    delimiter = ",",
    has_header = true,
    skip_blank = true,
    trim = true,
    types = { "string", "int", "float", "string" },  -- auto-convert
})

-- Write CSV
csv.save("output.csv", {
    headers = { "name", "score", "grade" },
    rows = {
        { "Alice", 95, "A" },
        { "Bob",   87, "B" },
    },
})

-- Streaming for large files (does not load entire file into memory)
for row in csv.rows("huge_dataset.csv") do
    if tonumber(row.score) > 90 then
        process(row)
    end
end

-- Build CSV from table data
local result = csv.encode(table_data, { headers = true })
print(result)
```

**Implementation**: Zero-copy streaming parser. Each row is a Nova table
with string keys (from header) or integer keys (no header). RFC 4180
compliant: handles quoted fields, embedded commas, embedded newlines,
escaped quotes. DAGGER-backed row tables for O(1) field access by name.

### 11.3 Design Philosophy

The data processing libraries follow the Ritchie principle: **do one thing
really well, give people the tools, don't be burdensome**.

- **No magic**: `csv.load()` returns a plain table. You already know tables.
- **Streaming by default**: Large files never blow up memory
- **Typed when asked**: Auto-type-conversion is opt-in, not forced
- **Consistent API**: `load()` / `save()` / `encode()` / `decode()` across
  all formats
- **Backed by Zorya SDK**: Weave for strings, DAGGER for tables, NXH for
  any hashing -- so the "built-in" libraries get the same industrial-strength
  backing as the language core

---

## 12. TASK RUNNER

Nova includes a built-in task runner for build orchestration, project
automation, and CI/CD scripting. Where other ecosystems need Make + shell +
jq + Python glue scripts, Nova handles it all in one language.

### 12.1 Task Files

Task definitions live in `.task.n` files (or a top-level `Taskfile.n`).
They're regular Nova code with a `task` declaration syntax:

```lua
-- Taskfile.n

#define VERSION "1.0.0"

task "build" {
    description = "Compile the project",
    depends = { "clean" },

    run = function()
        local files = glob("src/*.c")
        for _, f in ipairs(files) do
            exec("gcc", "-c", "-o", f:replace(".c", ".o"), f)
        end
        exec("gcc", "-o", "myapp", glob("src/*.o"))
        printf("Build complete: %s\n", VERSION)
    end,
}

task "clean" {
    description = "Remove build artifacts",
    run = function()
        rm(glob("src/*.o"))
        rm("myapp")
    end,
}

task "test" {
    description = "Run test suite",
    depends = { "build" },
    run = function()
        local results = exec("./myapp", "--test")
        if results.exit_code ~= 0 then
            error("Tests failed!")
        end
    end,
}

task "deploy" {
    description = "Deploy to production",
    depends = { "test" },
    env = { DEPLOY_ENV = "production" },
    run = function()
        local config = json.load("deploy.json")
        exec("rsync", "-avz", "./dist/", config.remote_path)
        printf("Deployed %s to %s\n", VERSION, config.remote_path)
    end,
}
```

### 12.2 Running Tasks

```bash
# Run a task
nova task build

# Run with arguments
nova task deploy --env=staging

# List available tasks
nova task --list

# Run default task (first task, or one marked default)
nova task
```

### 12.3 Built-In Task Utilities

The task runner exposes utility functions that are only available inside task
contexts (not in general Nova scripts):

| Function | Description |
|----------|-------------|
| `exec(cmd, ...)` | Run external command, return `{output, exit_code}` |
| `glob(pattern)` | File glob, returns array of matching paths |
| `rm(path_or_list)` | Remove files/directories |
| `mkdir(path)` | Create directory (recursive) |
| `cp(src, dst)` | Copy file or directory |
| `mv(src, dst)` | Move/rename |
| `exists(path)` | Check if path exists |
| `read(path)` | Read file contents as string |
| `write(path, data)` | Write string to file |
| `env(name)` | Get environment variable |
| `cd(path)` | Change working directory (scoped to task) |
| `printf(fmt, ...)` | Formatted output (always available) |

### 12.4 Design Rationale

The task runner completes Nova's vision as a Swiss Army knife. Consider
the typical project automation stack:

| Need | Traditional | Nova |
|------|-------------|------|
| Build orchestration | Makefile | `Taskfile.n` |
| JSON config processing | jq / Python | `json.load()` |
| CSV data munging | awk / Python | `csv.load()` |
| File manipulation | shell scripts | `glob()`, `cp()`, `rm()` |
| String formatting | printf / sed | `printf()`, string interpolation |
| Conditional logic | shell `if`/`test` | Real `if`/`else` with tables |

One language. One file. No dependencies. That's the pitch.

---

## 13. ZORYA SDK INTEGRATION

### 11.1 Asset Mapping

| Nova Need | Zorya Asset | Integration |
|-----------|------------|-------------|
| String interning | Weave Tablet | Direct: all Nova strings are Weave objects |
| String building | Weave Cord | Cord for `..` concatenation chains |
| Mutable strings | Weave | String buffer operations |
| Hash function | NXH64 | All hashing: strings, tables, bytecode |
| Global table | DAGGER | Environment and module tables |
| Macro table | DAGGER | PP macro name → expansion mapping |
| Performance macros | PCM | Throughout: LIKELY, PREFETCH, ROTL, etc. |
| Memory pools | zorya_arena | AST node allocation, GC arenas |

### 13.2 Build Integration

Nova includes the Zorya SDK source directly (no dynamic linking):

```makefile
# Nova links against Zorya SDK objects
ZORYA_OBJECTS = nxh.o dagger.o weave.o zorya_arena.o pcm.o
NOVA_OBJECTS  = nova_lex.o nova_pp.o nova_parse.o nova_parse_row.o \
                nova_ast_row.o nova_compile.o nova_opt.o nova_codegen.o \
                nova_vm.o nova_gc.o nova_api.o nova_state.o \
                nova_error.o nova_error_catalog.o \
                nova_lib_base.o nova_lib_string.o nova_lib_table.o \
                nova_lib_math.o nova_lib_io.o nova_lib_os.o \
                nova_lib_json.o nova_lib_csv.o nova_lib_task.o

nova: $(NOVA_OBJECTS) $(ZORYA_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^ -lm
```

---

## 14. MEMORY MANAGEMENT

### 14.1 Allocation Strategy

- **Arena allocator** for AST nodes (bulk alloc, bulk free)
- **GC-managed heap** for runtime objects (strings, tables, closures)
- **Custom allocator hook**: User can provide their own `malloc`/`free`

```c
/* User-provided allocator */
typedef void *(*nova_alloc_fn)(void *ud, void *ptr, size_t osize, size_t nsize);

NovaConfig config = {
    .allocator = my_alloc_fn,
    .alloc_userdata = my_context,
};
NovaState *N = nova_open_with(&config);
```

### 14.2 GC Tuning

```lua
-- From Nova code
collectgarbage("setpause", 200)      -- Pause between cycles (%)
collectgarbage("setstepmul", 400)    -- Step multiplier (speed)
collectgarbage("generational", 20, 100)  -- Generational: minor/major ratios
```

---

## 15. ERROR HANDLING

Nova's error system is designed to be the single highest-impact quality-of-life
feature in the language. Inspired by Rust's compiler diagnostics and GCC's
caret-style output, Nova errors are not just informative -- they're
**instructive**. Every error teaches the programmer something.

### 15.1 Error Format

All Nova diagnostics follow a consistent format:

```
file:line:col: severity[CODE]: message
  line | source code
       | ^^^^^ annotation
```

### 15.2 Compile Errors

```
game.n:15:23: error[NOVA-E0100]: undefined macro 'UNDEFINED_MACRO'
  15 |     local x = UNDEFINED_MACRO
     |               ^~~~~~~~~~~~~~~
     = note: macros must be #define'd before use
     = hint: did you forget to #include a .m file?

game.n:20:5: error[NOVA-E0201]: expected 'end' to close 'function' at line 10
  10 | function update(dt)
     | -------- opened here
  ...
  20 |     })
     |     ^ expected 'end', found ')'
     = note: every 'function' must have a matching 'end'

game.n:33:12: error[NOVA-E0301]: cannot call nil value
  32 |     local handler = get_handler("click")
  33 |     handler()
     |     ~~~~~~~^^
     |     |
     |     this is nil (returned from get_handler)
     = note: get_handler returns nil when no handler is registered
     = hint: check the return value before calling:
             if handler then handler() end
```

### 15.3 Warnings

```
game.n:8:11: warning[NOVA-W0001]: unused variable 'temp'
   8 |     local temp = compute()
     |           ^^^^ defined here but never used
     = hint: prefix with _ to suppress: local _temp = compute()

game.n:45:5: warning[NOVA-W0020]: unreachable code after return
  44 |     return result
  45 |     print("done")
     |     ^^^^^^^^^^^^^ this code will never execute
```

### 15.4 Runtime Errors

```
nova: game.n:42: error[NOVA-R0010]: attempt to index a nil value
  42 |     local hp = player.health
     |                ~~~~~~^~~~~~~
     |                |
     |                'player' is nil (global)
stack traceback:
    game.n:42: in function 'update'
    main.n:10: in function 'game_loop'
    main.n:55: in main chunk

     = hint: ensure 'player' is initialized before accessing fields
```

### 15.5 Error Code System

Every diagnostic has a unique numeric code in the format `NOVA-Xnnnn`:

| Prefix | Category | Range |
|--------|----------|-------|
| `NOVA-E0xxx` | Lexer errors | 0001-0099 |
| `NOVA-E01xx` | Preprocessor errors | 0100-0199 |
| `NOVA-E02xx` | Parser / syntax errors | 0200-0299 |
| `NOVA-E03xx` | Compiler / semantic errors | 0300-0399 |
| `NOVA-E04xx` | Type errors | 0400-0499 |
| `NOVA-E05xx` | Module / import errors | 0500-0599 |
| `NOVA-W0xxx` | Warnings | 0001-0999 |
| `NOVA-R0xxx` | Runtime errors | 0001-0999 |

Error codes are:
- **Stable**: Once assigned, a code never changes meaning
- **Searchable**: `nova --explain NOVA-E0201` prints a detailed explanation
- **Machine-readable**: Tools, IDEs, and AI assistants can parse them
- **Documented**: Every code has an entry in the error catalog

### 15.6 Debug Mode (--debug)

Running with `nova --debug` or `nova --explain NOVA-E0201` activates
extended error explanations -- tutorial-quality output that explains
*why* something is wrong and *how* to fix it:

```
$ nova --explain NOVA-E0201

error[NOVA-E0201]: expected 'end' to close block

Nova uses keyword-delimited blocks:

    function ... end
    if ... then ... end
    while ... do ... end
    for ... do ... end
    repeat ... until ...

Every opening keyword must have a matching closing keyword. This error
means Nova found a block opener (like 'function') but never found its
matching 'end'.

Common causes:
  1. Missing 'end' keyword
  2. Extra closing bracket or parenthesis
  3. Mismatched nesting (an inner block stole an outer 'end')

Example fix:

    -- Wrong:                     -- Right:
    function foo()                function foo()
        if bar then                   if bar then
            baz()                         baz()
                                      end      -- <-- was missing
    end                           end
```

### 15.7 Error Implementation Architecture

```
NovaErrorSystem
├── nova_error.h          -- Error codes, severity enum, NovaError struct
├── nova_error.c          -- Error formatting, caret rendering
├── nova_error_catalog.c  -- Static catalog: code → explanation text
└── nova_diag.h           -- Diagnostic builder API

NovaError {
    code:       uint16_t       -- NOVA-E0201 → 0x0201
    severity:   NovaSeverity   -- ERROR, WARNING, NOTE, HINT
    file:       const char *   -- Source file path
    line:       uint32_t       -- 1-based line number
    col:        uint32_t       -- 1-based column
    end_col:    uint32_t       -- End column (for underline span)
    message:    const char *   -- Primary message
    notes:      NovaNote[]     -- Secondary annotations
}
```

The diagnostic builder API supports chaining:

```c
nova_diag_error(N, NOVA_E0201)
    .at(file, line, col)
    .span(start_col, end_col)
    .message("expected 'end' to close '%s' at line %d", keyword, open_line)
    .note_at(open_file, open_line, open_col, "block opened here")
    .hint("add 'end' after line %d", last_stmt_line)
    .emit();
```

### 15.8 Design Principles

1. **Every error is a teaching moment**. If a beginner hits it, they should
   be able to fix it without Googling.
2. **Caret precision**. Point at the exact character, not just the line.
   Underline the full span of the problem.
3. **Context, not just location**. Show related lines (where a block was
   opened, where a variable was defined). Use `...` to skip irrelevant lines.
4. **Machine-friendly**. Error codes let IDEs, linters, and AI assistants
   provide automated fixes. `NOVA-E0201` is greppable, linkable, indexable.
5. **Progressive detail**. Default output is concise. `--debug` adds
   explanations. `--explain CODE` gives the full tutorial.

---

## 16. DEVELOPMENT ROADMAP

### Phase 1: Foundation -- COMPLETE
- [x] Design document
- [x] Project skeleton, build system
- [x] Lexer -- `nova_lex.c` (1,465 lines, 25KB obj, 29 keywords)
- [x] Pointer-based parser -- `nova_parse.c` (2,118 lines, 32KB obj)
- [x] Pointer-based AST -- `nova_ast.h` (511 lines)
- [x] async/await/spawn/yield keywords in lexer and parser

### Phase 2: Preprocessor -- COMPLETE
- [x] `#define` / `#undef` (object and function-like macros)
- [x] `#include` (file inclusion with cycle detection)
- [x] `#ifdef` / `#ifndef` / `#if` / `#else` / `#elif` / `#endif`
- [x] Function-like macros (variadic, hygienic expansion)
- [x] Predefined macros (`__FILE__`, `__LINE__`, `__DATE__`, etc.)
- [x] Token-based preprocessing (no text rewriting)
- [x] Full preprocessor -- `nova_pp.c` (2,184 lines, 34KB obj)

### Phase 3: Row-Based AST Migration -- COMPLETE
- [x] Flat array-indexed AST design (8 typed pools, arena-backed)
- [x] Row AST header -- `nova_ast_row.h` (609 lines, 8 index types)
- [x] Row AST implementation -- `nova_ast_row.c` (524 lines, 8KB obj)
- [x] Row-based parser -- `nova_parse_row.c` (1,626 lines, 29KB obj)
- [x] All 5 object files compile clean: 0 warnings, 128KB total

### Phase 4: Compiler -- COMPLETE
- [x] Row AST → Bytecode compiler (`nova_compile.c`, 2,632 lines)
- [x] Scope resolution and upvalue analysis (full lexical scoping, recursive upvalue walk)
- [x] Constant folding (deferred to optimizer -- see Phase 5)
- [x] Register allocation (linear stack-based allocator)
- [x] Instruction emission (32-bit fixed-width, ABC/ABx/AsBx/Ax formats)
- [x] Prototype table construction (constants, upvalues, sub-protos, debug info)
- [x] goto/label compilation (forward/backward jumps, scope validation)
- [x] break/continue (jump list chaining per loop scope)
- [x] 18 statement types compiled (EXPR, LOCAL, ASSIGN, IF, WHILE, REPEAT, FOR_NUMERIC, FOR_GENERIC, DO, BREAK, GOTO, LABEL, RETURN, FUNCTION, LOCAL_FUNCTION, IMPORT, EXPORT, CONST)
- [ ] Import/export resolution (stubs -- opcodes exist, runtime not wired)
- [ ] async function compilation (coroutine constructor emission)

### Phase 5: Optimizer -- COMPLETE
- [x] Peephole optimization (`novai_pass_peephole` -- LOADK+ARITH→ARITHk, MOVE elimination)
- [x] Dead code elimination (`novai_pass_dead_code` -- NOP-ify unreachable after RETURN)
- [x] Constant propagation (`novai_pass_const_fold` -- evaluate constant arithmetic at compile time)
- [x] Tail call detection (`novai_pass_tailcall` -- CALL+RETURN→TAILCALL)
- [x] Branch folding (`novai_pass_jump_opt` -- jump chain collapse, jump-to-next elimination)
- [x] Return specialization (`novai_pass_return_spec` -- RETURN→RETURN0/RETURN1)
- [x] NOP compaction (`novai_pass_squeeze` -- compact NOPs, adjust all jump targets)
- [x] 7 optimization passes, `nova_opt.c` (1,296 lines)

### Phase 6: Code Generator + .no Format -- COMPLETE
- [x] Bytecode serialization to `.no` format (`nova_codegen.c`, 1,274 lines)
- [x] Cross-platform header (magic `NOVA`, version, flags, platform, timestamp, NXH64 checksum)
- [x] String table emission (interned string pool)
- [x] Constant pool emission (typed constants)
- [x] Debug section (line maps, local names, strippable via `NOVA_CODEGEN_FLAG_STRIP`)
- [x] Bytecode loading and verification (`nova_codegen_load`, `nova_codegen_undump`)
- [x] `nova -c script.n` compilation mode
- [x] Memory dump/undump API (`nova_codegen_dump`, `nova_codegen_undump`)

### Phase 7: Virtual Machine -- NEAR COMPLETE
- [x] Register-based execution engine (`nova_vm.c`, 2,891 lines)
- [x] Computed goto dispatch (GCC `&&label` via PCM, fallback switch)
- [x] 256 registers per call frame
- [x] Call frame stack management
- [x] Upvalue capture and closure creation (open upvalue chain, `CLOSE` opcode)
- [x] Metatable dispatch -- 13 metamethods via `nova_meta.c` (1,137 lines):
      `__index`, `__newindex`, `__add`, `__sub`, `__mul`, `__div`, `__mod`,
      `__pow`, `__unm`, `__eq`, `__lt`, `__le`, `__len`, `__tostring`, `__call`
- [x] pcall error handling with setjmp/longjmp stack unwinding (`NovaErrorJmp` chain)
- [x] 77 opcodes dispatched (arithmetic, bitwise, comparison, table, string, control, loops, closures)
- [ ] Coroutine support (YIELD/AWAIT/SPAWN opcodes exist, runtime not implemented)
- [ ] async/await/spawn runtime (task scheduler)
- [ ] xpcall (pcall done, xpcall not yet)

### Phase 8: Garbage Collector -- NOT STARTED (headers defined)
- [x] GC object header infrastructure (`NovaGCObject` with `gc_next`, `color`, `generation` in `nova_object.h`)
- [x] GC API stubs declared (`novai_gc_alloc`, `novai_gc_step`, `novai_gc_collect`, `novai_gc_barrier`)
- [x] `bytes_allocated` tracking field on `NovaVM`
- [ ] Tri-color incremental mark-sweep implementation
- [ ] Generational mode (minor/major collections)
- [ ] Write barrier (forward barrier for incremental correctness)
- [ ] Finalization (`__gc` metamethod)
- [ ] Weak tables (weak keys, weak values, ephemeron handling)
- [ ] GC tuning API (pause, stepmul, generational ratios)
- [ ] Custom allocator hook support

### Phase 9: Runtime Integration -- MOSTLY COMPLETE
- [x] Table implementation (DAGGER-backed, array+hash hybrid)
- [x] String interning (basic implementation in VM)
- [x] Metatables and operator overloading (13 metamethods, full dispatch)
- [x] Module system -- `require()` with `package.loaded`, `package.path`, `package.preload`
- [ ] NaN-boxing value representation (code exists in `nova_object.h`, tagged union currently active)
- [ ] Coroutine infrastructure (not implemented)
- [ ] Weave Tablet integration for short/long dual strategy interning

### Phase 10: Standard Library -- MOSTLY COMPLETE
- [x] Base library -- 23 functions: `print`, `printf`, `sprintf`, `fprintf`, `type`, `tostring`, `tonumber`, `tointeger`, `error`, `assert`, `pcall`, `select`, `rawget`, `rawset`, `rawlen`, `rawequal`, `setmetatable`, `getmetatable`, `ipairs`, `pairs`, `next`, `unpack`, `collectgarbage`
- [x] String library -- 12 functions: `len`, `sub`, `upper`, `lower`, `rep`, `reverse`, `byte`, `char`, `find`, `format`, `gsub`, `match`
- [x] Table library -- 6 functions: `insert`, `remove`, `concat`, `sort`, `move`, `pack`
- [x] Math library -- 20 functions + constants: `abs`, `ceil`, `floor`, `sqrt`, `sin`, `cos`, `tan`, `asin`, `acos`, `atan`, `exp`, `log`, `pow`, `fmod`, `max`, `min`, `random`, `randomseed`, `tointeger`, `type`, `pi`
- [x] I/O library -- 9 functions: `open`, `close`, `read`, `write`, `lines`, `type`, `tmpfile`, `flush`, `popen`
- [x] OS library -- 11 functions: `clock`, `time`, `date`, `difftime`, `getenv`, `execute`, `exit`, `remove`, `rename`, `tmpname`, `setlocale`
- [x] Package library -- `require()`, `package.loaded`, `package.path`, `package.preload`
- [ ] Coroutine library (create, resume, yield, wrap, status)
- [ ] Bit library (PCM-backed bitwise operations -- operators exist, no library wrapper)
- [ ] Debug library (hooks, traceback, getinfo, getlocal)

### Phase 11: Data Processing Libraries
- [ ] JSON library (hand-written recursive descent, RFC 8259)
- [ ] JSON streaming decoder for large files
- [ ] CSV/TSV library (zero-copy streaming, RFC 4180)
- [ ] Typed column auto-conversion (opt-in)
- [ ] `load()` / `save()` / `encode()` / `decode()` consistent API

### Phase 12: Rich Error System
- [ ] Error code catalog (`nova_error_catalog.c`)
- [ ] Caret-style diagnostic renderer (`nova_error.c`)
- [ ] Multi-line span annotations with context
- [ ] `--explain CODE` tutorial-quality output
- [ ] `--debug` extended error mode
- [ ] Warning system with suppression (`--nowarn=NOVA-W0001`)
- [ ] Machine-readable JSON error output (`--error-format=json`)

### Phase 13: Task Runner
- [ ] `Taskfile.n` loading and validation
- [ ] Task dependency resolution (topological sort)
- [ ] Task utility functions (exec, glob, rm, cp, mv, etc.)
- [ ] `nova task` CLI subcommand
- [ ] Environment variable injection
- [ ] Task listing and help output

### Phase 14: C API + Polish
- [ ] Full C API implementation (`nova_api.c`)
- [ ] REPL with line editing and history
- [ ] Performance benchmarking suite
- [ ] Fuzz testing (AFL/libFuzzer on parser and JSON decoder)
- [ ] API documentation and examples
- [ ] Lua compatibility layer (optional `require("compat")`)

### Progress Summary

| Component | File(s) | Lines | Obj Size | Status |
|-----------|---------|-------|----------|--------|
| Lexer | `nova_lex.c` | 1,465 | 25KB | **DONE** |
| Preprocessor | `nova_pp.c` | 2,233 | 34KB | **DONE** |
| Parser (legacy) | `nova_parse.c` | 2,117 | 32KB | **DONE** |
| AST (legacy) | `nova_ast.h` | 511 | -- | **DONE** |
| Row AST | `nova_ast_row.h/.c` | 1,133 | 8KB | **DONE** |
| Row Parser | `nova_parse_row.c` | 1,667 | 29KB | **DONE** |
| Compiler | `nova_compile.c` | 2,632 | -- | **DONE** |
| Optimizer | `nova_opt.c` | 1,296 | -- | **DONE** |
| Codegen / .no | `nova_codegen.c` | 1,274 | -- | **DONE** |
| Proto system | `nova_proto.c` | 684 | -- | **DONE** |
| Opcode metadata | `nova_opcode.c` | 352 | -- | **DONE** |
| VM | `nova_vm.c` | 2,891 | -- | **DONE** |
| Metamethods | `nova_meta.c` | 1,137 | -- | **DONE** |
| Base lib | `nova_lib_base.c` | 1,311 | -- | **DONE** |
| String lib | `nova_lib_string.c` | 806 | -- | **DONE** |
| Table lib | `nova_lib_table.c` | 483 | -- | **DONE** |
| Math lib | `nova_lib_math.c` | 399 | -- | **DONE** |
| I/O lib | `nova_lib_io.c` | 578 | -- | **DONE** |
| OS lib | `nova_lib_os.c` | 386 | -- | **DONE** |
| Package lib | `nova_lib_package.c` | 453 | -- | **DONE** |
| CLI / Runner | `nova.c` | 1,003 | -- | **DONE** |
| C API | `nova_api.h` / `nova.h` | 733 | -- | **PARTIAL** (headers only) |
| GC | (headers only) | ~200 | -- | **NOT STARTED** |
| Error system | (inline in VM) | -- | -- | **PARTIAL** |
| **Total** | **20 source files** | **~25,000+** | -- | **Core pipeline complete** |

---

## 17. NAMING CONVENTIONS

Following ZORYA-C v2.0.0:

| Category | Convention | Example |
|----------|-----------|---------|
| Public functions | `nova_verb_noun()` | `nova_push_string()` |
| Internal functions | `novai_verb_noun()` | `novai_parse_expr()` |
| Types | `NovaPascalCase` | `NovaState`, `NovaValue` |
| Macros | `NOVA_UPPER_CASE` | `NOVA_MAX_REGISTERS` |
| Opcodes | `NOVA_OP_NAME` | `NOVA_OP_ADD` |
| Error codes | `NOVA_ERROR_NAME` | `NOVA_ERROR_SYNTAX` |
| Config | `NOVA_CONFIG_NAME` | `NOVA_CONFIG_GC_PAUSE` |
| State pointer | `N` | `nova_push_nil(N)` |

The state pointer is `N` (following ZORYA-C-200's convention of single-letter state pointers).

---

## 18. COMPARISON WITH LUA

| Feature | Lua 5.4 | Nova 0.1 |
|---------|---------|----------|
| Preprocessor | None | Full (#include, #define, macros) |
| Array indexing | 1-based | 0-based (default) |
| Bytecode portability | Platform-dependent | Cross-platform (.no) |
| String library | Basic patterns | Weave (industrial-strength) |
| Hash tables | Custom open-addressing | DAGGER (Robin Hood + Cuckoo) |
| C API style | Stack-index gymnastics | Clean typed interface |
| String interning | Simple hash | NXH64 + Weave Tablet |
| Module system | `require` + search paths | `require` + explicit imports |
| String interpolation | None | Backtick templates |
| Build system | None | Preprocessor + .no objects |
| GC | Incremental generational | Incremental generational (improved) |
| Coroutines | Yes | Yes + async/await/spawn |
| Metatables | Yes | Yes |
| Multiple returns | Yes | Yes |
| Closures | Yes | Yes |
| Embeddable | Yes (primary goal) | Yes (primary goal) |
| AST representation | None (single-pass) | Row-based flat arrays (cache-optimal) |
| JSON support | Requires library | Built-in `json` module |
| CSV/TSV support | None | Built-in `csv` module |
| Error diagnostics | Basic line numbers | GCC-style carets + error codes |
| Error explanations | None | `--explain NOVA-Exxxx` tutorials |
| printf | `string.format()` | First-class `printf()`, `sprintf()` |
| Task runner | None | Built-in `nova task` |
| Concurrency keywords | None | `async`/`await`/`spawn`/`yield` |

---

## 19. IMPLEMENTATION PROGRESS LOG

This section tracks the actual implementation progress, milestones reached,
and decisions made during development.

### 19.1 Milestone Timeline

| Date | Milestone | Details |
|------|-----------|---------|
| 2026-02-05 | Design document created | Complete language specification drafted |
| 2026-02-05 | Phases 1-3 complete | Lexer, preprocessor, row AST, parser all working |
| 2026-02-06 | Compiler + Optimizer + Codegen | Full compilation pipeline: Row AST -> bytecode -> .no files |
| 2026-02-06 | VM + Standard libraries | Register-based VM with computed goto, 77 opcodes, 7 stdlib modules |
| 2026-02-07 | Metamethods (13 total) | `__index`, `__newindex`, `__add`-`__pow`, `__unm`, `__eq`/`__lt`/`__le`, `__len`, `__tostring`, `__call` |
| 2026-02-07 | pcall / error handling | `setjmp`/`longjmp` based protected calls with `NovaErrorJmp` chain |
| 2026-02-08 | goto/labels | Forward and backward gotos, scope validation, unresolved goto detection |
| 2026-02-08 | Preprocessor double-free fix | Empty string tokens now always duplicated in `novai_output_push` |
| 2026-02-08 | CONCAT register allocation fix | `NOVA_EK_CALL` handling in `novai_discharge_to_any` |
| 2026-02-09 | Garbage collector complete | Tri-color incremental mark-sweep (~690 lines), all alloc functions wired |
| 2026-02-09 | GC: 5 critical bugs fixed | Gray list corruption, upvalue type tag, enum clash, double-free at shutdown, sweep list corruption |
| 2026-02-09 | GC: white-flip ordering fix | `nova_gc_check` moved before `malloc` to prevent premature collection of newly-allocated objects |
| 2026-02-09 | GC: sweep pointer-to-pointer fix | `gc_sweep_pos` changed from `NovaGCHeader*` to `NovaGCHeader**` to correctly maintain all_objects chain during incremental sweep |
| 2026-02-09 | Compiler: for-loop register fix | Hidden locals `(for state)/(for limit)/(for step)` prevent body locals from overlapping loop internals |
| 2026-02-09 | collectgarbage() fully implemented | collect, stop, restart, count, step, setpause, setstepmul, isrunning |
| 2026-02-09 | GC stress test (10 tests) | String stress (10K), table stress (5K), nested tables, closures, metatables, heavy alloc (50K) |

### 19.2 Current Status Summary (2026-02-09)

**What Works Today:**
- Full pipeline: `.n` source -> lex -> preprocess -> parse -> compile -> optimize -> execute
- Compile to `.no` bytecode and load/execute from `.no` files
- Interactive REPL (`nova` with no arguments)
- 18 statement types, full expression compilation
- 77 VM opcodes with computed goto dispatch
- 7 optimization passes (peephole, const fold, dead code, tailcall, etc.)
- 13 metamethods with full table-based dispatch
- pcall with proper stack unwinding (setjmp/longjmp)
- goto/labels with forward/backward jumps and scope checking
- 7 standard library modules: base (23 funcs), string (12), table (6), math (20), io (9), os (11), package
- `require()` module loading with search paths
- `printf` / `sprintf` / `fprintf` as first-class builtins
- **Garbage collector**: Incremental tri-color mark-sweep with two-white flip-flop
  - All allocation functions wired (string, table, closure, upvalue)
  - Write barriers at all mutation sites (table set, SETUPVAL, close_upvalues, setmetatable)
  - `collectgarbage()` fully implemented (collect, stop, restart, count, step, setpause, setstepmul, isrunning)
  - Stress-tested: 10K string iterations, 5K table iterations, 50K heavy allocations

**What Does NOT Work Yet:**
- Coroutines (`coroutine.create`, `resume`, `yield` -- opcodes exist, no runtime)
- async/await/spawn (keywords parsed, no runtime scheduler)
- xpcall (pcall done, xpcall not yet)
- NaN-boxing (code written in headers, tagged union is currently active)
- C embedding API (headers designed, implementation not written)
- Rich error diagnostics (no caret-style errors, no error catalog)
- Data processing libraries (json, csv -- not started)
- Task runner (not started)
- Debug library (not started)
- String interpolation with backticks (not implemented)
- 0-indexed arrays (currently 1-indexed like standard Lua)
- Memory leaks in parser/AST (arena cleanup not wired to VM lifecycle)

### 19.3 Phase Completion Matrix

```
Phase  1: Foundation              [##########] 100%  COMPLETE
Phase  2: Preprocessor            [##########] 100%  COMPLETE
Phase  3: Row-Based AST           [##########] 100%  COMPLETE
Phase  4: Compiler                [########--]  85%  import/export + async remaining
Phase  5: Optimizer               [##########] 100%  COMPLETE
Phase  6: Code Generator + .no    [##########] 100%  COMPLETE
Phase  7: Virtual Machine         [########--]  85%  coroutines + async remaining
Phase  8: Garbage Collector       [##########] 100%  COMPLETE
Phase  9: Runtime Integration     [######----]  65%  coroutines + NaN-boxing remaining
Phase 10: Standard Library        [########--]  80%  coroutine/bit/debug libs remaining
Phase 11: Data Processing         [----------]   0%  NOT STARTED
Phase 12: Rich Error System       [----------]   0%  NOT STARTED
Phase 13: Task Runner             [----------]   0%  NOT STARTED
Phase 14: C API + Polish          [#---------]  10%  headers only
```

**Overall: ~65% of the full design completed. Core language pipeline is functional with automatic memory management.**

### 19.4 Known Bugs and Technical Debt

| Issue | Severity | Location | Notes |
|-------|----------|----------|-------|
| Parser/AST arenas not freed through VM | MEDIUM | `nova_parse_row.c` | Valgrind reports leaks on every run |
| `novai_const_to_value` creates new string per LOADK | MEDIUM | `nova_vm.c:693` | String interning would deduplicate |
| `novai_error` doesn't support format strings | LOW | `nova_compile.c:91` | Compile errors use static messages only |
good| Tagged union (16B) instead of NaN-boxing (8B) | LOW | `nova_object.h` | 2x memory overhead per value |
| `tmpnam` warning from linker | LOW | `nova_lib_os.c` | Use `mkstemp` instead |

---

## 20. NEXT STEPS -- DEVELOPMENT PLAN

### Tier 1: Critical Path (Must-Have for Usable Language)

These items block real-world usage. Without GC, Nova cannot run long-lived
programs. Without coroutines, a major language feature is missing.

**1. ~~Garbage Collector (`nova_gc.c`)~~** -- DONE (2026-02-09)
- Tri-color incremental mark-sweep: ~690 lines in `nova_gc.c`
- Root set: stack, globals, call frame closures/varargs, open upvalues
- Write barriers at all mutation sites
- Two-white flip-flop: `gc_current_white ^= 1` each cycle
- `gc_sweep_pos` uses `NovaGCHeader**` (pointer-to-pointer) for correct incremental sweep
- `nova_gc_check` called BEFORE allocation to prevent premature collection
- `collectgarbage()` supports: collect, stop, restart, count, step, setpause, setstepmul, isrunning
- All 3 test suites pass (tier1, metamethods, gc_stress -- 10 GC tests including 50K heavy alloc)

**2. ~~Coroutine Runtime~~ (`nova_coroutine.c`, `nova_lib_coroutine.c`)** -- DONE (2026-02-08)
- Stackful cooperative coroutines with independent stacks (256 slots) and frame arrays (64 max)
- `coroutine.create(f)` -> new thread, `coroutine.resume(co, ...)` -> enter/continue
- `coroutine.yield(...)` -> suspend, `coroutine.status(co)` -> state query
- `coroutine.isyieldable()`, `coroutine.running()`, `coroutine.wrap(f)` (placeholder)
- NovaSavedState VM swap mechanism for zero-copy context switching
- GC integration: mark/propagate/free for NOVA_TYPE_THREAD objects
- Parser fix: keywords valid as field names (e.g. `coroutine.yield`)
- CALL handler yield path: advances IP, repositions yield values
- 10 tests pass: create, resume, yield, value passing, loops, GC interaction
- ~750 lines across nova_coroutine.c + nova_lib_coroutine.c

**3. ~~xpcall~~** -- DONE (2026-02-08)
- `xpcall(f, handler, ...)` -> true, results... OR false, handler(err)
- Error handler called with raw error message before stack unwind
- Falls back to raw error if handler itself fails
- Works inside coroutines
- 8 tests pass: success, error, transform, multi-return, nil handler, coroutine
- ~90 lines in nova_lib_base.c

### Tier 2: High Value (Major Feature Completions)

**4. String Interpolation (Backtick Strings)** -- DONE
- Lexer emits INTERP_START / INTERP_SEGMENT / INTERP_END token stream
- Parser collects literal segments and sub-expressions into multi-part INTERP_STRING
- Compiler chains binary CONCATs; single-expression parts coerced to string via "" .. expr
- Supports: variables, arithmetic, function calls, table fields, nested braces, multi-line
- 16 tests pass in test_interpolation.n
- Changes: nova_lex.h (+3 tokens, +3 state fields), nova_lex.c (interp mode), nova_pp.c (string dup),
  nova_parse_row.c + nova_parse.c (INTERP_START handler), nova_compile.c (CONCAT chaining)

**5. 0-Indexed Arrays** -- DONE
- All arrays and string positions are now 0-based throughout the language
- Implementation: Changed 25+ locations across 9 files (nova_vm.c, nova_meta.c,
  nova_lib_base.c, nova_lib_table.c, nova_lib_string.c, nova_compile.c, nova.c)
- Table constructor `{a, b, c}` stores at indices 0, 1, 2
- `#t` returns element count (unchanged semantics)
- `ipairs` iterates 0, 1, 2, ... (was 1, 2, 3, ...)
- `unpack(t)` defaults to `unpack(t, 0, #t-1)` (was 1..#t)
- `select(0, ...)` returns first vararg (was `select(1, ...)`)
- `string.sub(s, 0, 0)` returns first char (was `sub(s, 1, 1)`)
- `string.find` returns 0-based start/end positions
- `string.byte` defaults to index 0
- 0-based append idiom: `t[#t] = val` (was `t[#t + 1] = val`)
- Tests: 53 dedicated 0-index tests + all 7 suites pass

**6. Debug Library**
- `debug.traceback()` -- format stack trace string
- `debug.getinfo(f)` -- function metadata (name, source, line)
- `debug.getlocal(level, idx)` -- inspect local variables
- Essential for development-time debugging

**7. C Embedding API (`nova_api.c`)**
- Implement the full API declared in `nova.h` (498 lines of interface)
- Stack-based operations for C -> Nova interop
- Critical for embedding Nova in other applications

### Tier 3: Ecosystem (Nice-to-Have, Post-Core)

**8. Rich Error System**
- Error code catalog (`nova_error_catalog.c`)
- Caret-style diagnostic renderer
- `--explain` and `--debug` modes
- Machine-readable JSON error output

**9. JSON Library (`nova_lib_json.c`)**
- Hand-written recursive descent decoder
- Table encoder with pretty-print option
- Streaming decoder for large files
- `json.load()` / `json.save()` / `json.encode()` / `json.decode()`

**10. CSV Library (`nova_lib_csv.c`)**
- Zero-copy streaming parser (RFC 4180)
- Header detection, typed columns (opt-in)
- `csv.load()` / `csv.save()` / `csv.rows()`

**11. Task Runner**
- `Taskfile.n` loading and validation
- Dependency resolution (topological sort)
- `nova task` CLI subcommand
- Built-in utility functions (exec, glob, rm, cp, mv)

**12. NaN-Boxing Activation**
- Switch from tagged union (16 bytes) to NaN-boxing (8 bytes)
- Half the memory per value
- Better cache utilization
- Code already exists in `nova_object.h`, needs VM integration testing

### Recommended Order

```
GC  -->  Coroutines  -->  xpcall  -->  Debug lib
 |
 +-->  C API  -->  String interpolation  -->  0-indexed arrays
 |
 +-->  JSON lib  -->  CSV lib  -->  Error system  -->  Task runner
```

GC is the single most impactful item -- it unblocks everything else and
makes Nova viable for programs that run longer than a few seconds.

---

**ZORYA CORPORATION - Engineering Excellence, Democratized**
