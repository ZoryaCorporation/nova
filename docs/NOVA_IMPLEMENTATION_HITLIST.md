# NOVA IMPLEMENTATION HITLIST

**Date**: February 14, 2026 (updated: session of Phase 11 completion)
**Status**: Active — Polish & Gaps Phase
**Codebase**: ~44,000+ lines across 28 `.c` + 20 `.h` files, ~5,700 lines tests
**Current**: 20/20 test suites green, 788/788 tests passing, 0 failures

---

## OVERVIEW

This document is the master implementation guide for bringing Nova from
"impressive prototype" to "fully operational scripting language." Items are
ordered by priority — each one builds on the previous. Every item includes
the exact files to touch, the pattern to follow, and acceptance criteria.

**Ground rules:**
- One item at a time, sequential execution
- Each item ends with a test suite that passes
- Build must stay `-Wall -Wextra -Werror -pedantic` clean
- No regressions in existing test suites

---

## STATUS KEY

- `[ ]` Not started
- `[~]` In progress
- `[x]` Complete

---

## PHASE 9: CORE LANGUAGE COMPLETENESS

### 9.1 [x] String Pattern Matching Engine

**Priority**: CRITICAL — blocks any serious text processing
**Effort**: Large (500-800 lines)
**Files**:
- `src/nova_lib_string.c` — main implementation
- `tests/test_string.n` — new test file

**What exists**: `find`, `gsub`, `match` do plain-text search only
(line 391: `/* Always do plain search for now (full patterns in Phase 9) */`).
`gmatch` is not in the registration table at all.

**Implementation plan**:

1. **Add a pattern engine** (new static functions in `nova_lib_string.c`):
   ```c
   /* Pattern matching state */
   typedef struct {
       const char *src;       /* Source string */
       size_t      src_len;
       const char *pat;       /* Pattern string */
       size_t      pat_len;
       const char *captures[32];  /* Capture start pointers */
       size_t      capture_len[32];
       int         num_captures;
       int         level;     /* Recursion depth */
   } MatchState;

   /* Core matching functions */
   static int match_class(int c, int cls);      /* %a, %d, %l, etc. */
   static const char *match_pattern(MatchState *ms,
                                     const char *s, const char *p);
   static const char *match_balance(MatchState *ms,
                                     const char *s, const char *p);
   static const char *start_capture(MatchState *ms,
                                     const char *s, const char *p);
   static const char *end_capture(MatchState *ms,
                                   const char *s, const char *p);
   ```

2. **Pattern classes to support**:
   - `.` — any character
   - `%a` — letters, `%d` — digits, `%l` — lowercase, `%u` — uppercase
   - `%w` — alphanumeric, `%s` — whitespace, `%p` — punctuation
   - `%c` — control chars, `%x` — hex digits
   - `%A`, `%D`, etc. — complement classes
   - `[set]` — character sets, `[^set]` — complement sets
   - `*` — 0 or more (greedy), `+` — 1 or more, `-` — 0 or more (lazy)
   - `?` — optional, `^` — anchor start, `$` — anchor end
   - `(...)` — captures, `%1`–`%9` — back-references

3. **Update existing functions**:
   - `nova_string_find` (line 364): Add pattern mode when `plain` is false
   - `nova_string_gsub` (line 668): Replace with pattern-aware substitution
   - `nova_string_match` (line 764): Replace with pattern-aware match

4. **Add `gmatch`**:
   ```c
   /* gmatch returns an iterator function. Requires C closure with
    * upvalues: source string, pattern, current position.
    * Implementation approach: store state in a userdata or table
    * on the stack, return a C function that reads from it.
    *
    * Simpler alternative: gmatch returns a table of all matches
    * (not standard Lua, but functional for Phase 9). Full iterator
    * version in Phase 10 when C closures have upvalue support. */
   static int nova_string_gmatch(NovaVM *vm);
   ```

5. **Register `gmatch`** in `nova_string_lib[]` (line 801):
   ```c
   {"gmatch", nova_string_gmatch},
   ```

**Test criteria** (`tests/test_string.n`):
```lua
-- Pattern classes
assert(string.find("hello 123", "%d+") == 6)
assert(string.match("hello 123", "%d+") == "123")
assert(string.gsub("hello world", "%w+", "X") == "X X")

-- Captures
local y, m, d = string.match("2026-02-12", "(%d+)-(%d+)-(%d+)")
assert(y == "2026")

-- Anchors
assert(string.find("hello", "^hello$") == 0)
assert(string.find("hello", "^world") == nil)

-- Character sets
assert(string.match("abc123", "[%a]+") == "abc")

-- gmatch
local words = string.gmatch("one two three", "%w+")
assert(#words == 3)
```

**Reference**: Lua 5.4 `lstrlib.c` pattern engine (~600 lines). Our version
can be simpler — skip `%f` frontier pattern and `%b` balance initially.

---

### 9.2 [x] Math Constants Fix

**Priority**: HIGH — trivial fix, currently broken
**Effort**: Small (15 lines)
**Files**:
- `src/nova_lib_math.c` lines 367-399
- `tests/test_math.n` — new test file

**What's broken**: `nova_open_math()` tries to set `math.pi` by calling
`nova_vm_set_field(vm, -1, "pi")` but `-1` is a relative index that
doesn't work correctly outside a C function call frame (no `cfunc_base`).
The math table was already popped and set as a global by `nova_lib_register_module`.

**Fix**: Set constants *before* the module table is popped. Or retrieve
the math global table and use `nova_table_raw_set` directly.

```c
int nova_open_math(NovaVM *vm) {
    if (vm == NULL) return -1;

    nova_lib_register_module(vm, "math", nova_math_lib);

    /* Get the registered math table back from globals */
    NovaValue math_tbl = nova_vm_get_global(vm, "math");
    if (math_tbl.type == NOVA_TYPE_TABLE) {
        NovaTable *t = math_tbl.as.table;

        /* Push constants directly into the table's hash */
        /* We need novai_string_new for keys — use set_global trick:
         * push the math table, push value, set_field, pop */
        nova_vm_push_value(vm, math_tbl);
        int tidx = nova_vm_get_top(vm) - 1;

        nova_vm_push_number(vm, 3.14159265358979323846);
        nova_vm_set_field(vm, tidx, "pi");

        nova_vm_push_number(vm, HUGE_VAL);
        nova_vm_set_field(vm, tidx, "huge");

        nova_vm_push_integer(vm, NOVA_INT_MAX);
        nova_vm_set_field(vm, tidx, "maxinteger");

        nova_vm_push_integer(vm, NOVA_INT_MIN);
        nova_vm_set_field(vm, tidx, "mininteger");

        nova_vm_set_top(vm, tidx);  /* Pop math table */
    }
    return 0;
}
```

**Test criteria** (`tests/test_math.n`):
```lua
-- Constants
assert(math.pi > 3.14 and math.pi < 3.15)
assert(math.huge > 1e300)
assert(math.maxinteger > 0)
assert(math.mininteger < 0)

-- Functions
assert(math.abs(-5) == 5)
assert(math.floor(3.7) == 3)
assert(math.ceil(3.2) == 4)
assert(math.max(1, 5, 3) == 5)
assert(math.min(1, 5, 3) == 1)
assert(math.sqrt(144) == 12)
assert(type(math.random()) == "number")
```

---

### 9.3 [x] `table.sort` with Custom Comparator

**Priority**: HIGH — needed for any real data processing
**Effort**: Small (40 lines)
**Files**:
- `src/nova_lib_table.c` lines 315-370
- `tests/test_table.n` — new test file

**What exists**: Insertion sort with hardcoded `<` comparison for
integers, numbers, strings. No support for a comparator function argument.

**Fix**: Check for optional 2nd argument (function). If present, call it
for each comparison instead of the hardcoded logic.

```c
static int nova_table_sort(NovaVM *vm) {
    /* Accept 1 or 2 args: table [, comparator_func] */
    int nargs = nova_vm_get_top(vm);
    if (nargs < 1) { /* error */ }

    NovaValue tv = nova_vm_get(vm, 0);
    /* ... validate table ... */

    int has_cmp = 0;
    NovaValue cmp_func = nova_value_nil();
    if (nargs >= 2) {
        cmp_func = nova_vm_get(vm, 1);
        if (cmp_func.type == NOVA_TYPE_CLOSURE ||
            cmp_func.type == NOVA_TYPE_CFUNCTION) {
            has_cmp = 1;
        }
    }

    /* In comparison loop, replace hardcoded < with: */
    if (has_cmp) {
        /* Push cmp_func, push prev, push key, call(2,1) */
        nova_vm_push_value(vm, cmp_func);
        nova_vm_push_value(vm, t->array[j - 1]);
        nova_vm_push_value(vm, key);
        nova_vm_call(vm, 2, 1);
        /* Result is boolean on stack */
        NovaValue result = nova_vm_get(vm, nova_vm_get_top(vm) - 1);
        should_swap = nova_value_is_truthy(result);
        nova_vm_pop(vm, 1);
    } else {
        /* existing default comparison */
    }
}
```

**Test criteria** (`tests/test_table.n`):
```lua
-- Default sort
local a = {5, 3, 1, 4, 2}
table.sort(a)
assert(a[0] == 1 and a[4] == 5)

-- Custom comparator (descending)
table.sort(a, function(x, y) return x > y end)
assert(a[0] == 5 and a[4] == 1)

-- String sort
local s = {"banana", "apple", "cherry"}
table.sort(s)
assert(s[0] == "apple" and s[2] == "cherry")
```

---

### 9.4 [x] `#require` Preprocessor Directive (Module System)

**Priority**: HIGH — clean module loading, replaces Lua's footgun `require()`
**Effort**: Medium (150-200 lines)
**Files**:
- `include/nova/nova_lex.h` — add `NOVA_TOKEN_PP_REQUIRE`
- `src/nova_lex.c` — recognize `#require` as a PP directive
- `include/nova/nova_pp.h` — add require tracking structures
- `src/nova_pp.c` — implement `novai_handle_require()`
- `src/nova_lib_package.c` — reuse existing search/load pipeline
- `tests/test_require.n` — new test file
- `tests/lib/test_module.n` — helper module for testing

**Design decision**: Runtime `require()` follows Lua semantics where it
can be called anywhere (inside functions, conditionally, in loops). This
is a footgun — modules should be declared at the top of a file, not
dynamically loaded mid-execution. Nova uses the preprocessor to enforce
clean structure.

**File header hierarchy** (enforced ordering):
```nova
-- ============================================
-- 1. #include — textual inclusion (.m files)
--    Macros, type aliases, enumerations.
--    Like C headers.
-- ============================================
#include "constants.m"
#include "error_codes.m"

-- ============================================
-- 2. #import — built-in module activation
--    Registers stdlib module globals.
--    Data format codecs, future: networking, etc.
-- ============================================
#import data
#import json

-- ============================================
-- 3. #require — user module loading
--    Compiles & caches external .n files.
--    Keeps dependencies out of function bodies.
-- ============================================
#require utils                  -- binds to local `utils`
#require lib/parser             -- binds to local `parser`
#require lib/parser as P        -- explicit alias

-- ============================================
-- Code starts here, all dependencies resolved
-- ============================================
```

**Syntax**:
```
#require <module_path>              -- auto-bind to last segment
#require <module_path> as <name>    -- explicit binding name
```

**Semantics**:
1. Resolved at preprocess time (before parsing)
2. Module path searched via `package.path` (same search logic as old `require`)
3. Module is compiled and executed once, result cached in `package.loaded`
4. The module's return value is bound to a **local variable** in the requiring file
5. Subsequent `#require` of the same module returns the cached value
6. Circular dependencies detected at preprocess time → compile error

**Implementation plan**:

1. **Add token** to `nova_lex.h`:
   ```c
   NOVA_TOKEN_PP_REQUIRE,      /* #require          */
   ```

2. **Lexer recognition** in `nova_lex.c` PP directive table:
   ```c
   {"require", NOVA_TOKEN_PP_REQUIRE},
   ```

3. **PP handler** — the core logic. This is where it differs from
   `#import` (which just sets flags). `#require` must:
   - Parse the module path (bare name or quoted string)
   - Parse optional `as <name>` alias
   - Resolve the file path using existing `novai_search_path()`
   - Emit synthetic tokens that the parser sees as:
     ```
     local <binding_name> = __require__("<resolved_path>")
     ```
   - The `__require__` is a C function that does the actual
     load/compile/cache (reusing `nova_base_require` from
     `nova_lib_package.c`)

   ```c
   static int novai_handle_require(NovaPP *pp, const NovaSourceLoc *loc) {
       /* 1. Parse module path: #require lib/parser */
       /*    Accept NAME or NAME/NAME/... or "quoted/path" */

       /* 2. Parse optional: as NAME */

       /* 3. Determine binding name:
        *    - With 'as': use the alias
        *    - Without: use last path segment
        *      "lib/parser" → "parser"
        *      "utils" → "utils"
        */

       /* 4. Check for circular requires */

       /* 5. Emit synthetic tokens into the PP output stream:
        *    TOKEN_LOCAL  <binding>  TOKEN_ASSIGN
        *    TOKEN_NAME("__require__")
        *    TOKEN_LPAREN  TOKEN_STRING("<path>")  TOKEN_RPAREN
        *    TOKEN_NEWLINE
        */
   }
   ```

4. **Register `__require__` global** — in `nova_open_package()`:
   ```c
   /* Keep the existing require() temporarily for backwards compat,
    * but __require__ is the PP-emitted version */
   nova_vm_set_global(vm, "__require__",
                      nova_value_cfunction(nova_base_require));
   ```

5. **Deprecate runtime `require()`**: Keep it working but emit a
   compiler warning: "require() is deprecated, use #require instead"

**On `#export`**: DEFERRED. Not needed for correctness — modules export
by returning a table (`return { fn1 = fn1, fn2 = fn2 }`). This is
simple, explicit, and requires zero compiler support. `#export` can be
added later for static analysis / IDE autocomplete if desired.

**Test criteria** (`tests/test_require.n`):
```nova
-- tests/lib/test_module.n (helper file):
-- local M = {}
-- M.greeting = "hello from module"
-- function M.add(a, b) return a + b end
-- return M

#require lib/test_module

check("require loads module", test_module ~= nil)
check("require module field", test_module.greeting == "hello from module")
check("require module func", test_module.add(3, 4) == 7)

-- With alias
#require lib/test_module as tm
check("require as alias", tm.greeting == "hello from module")

-- Cached (same object)
#require lib/test_module as tm2
check("require cached", tm == tm2)
```

---

### 9.4b [x] `load()` Runtime String Compilation

**Priority**: MEDIUM — useful for metaprogramming, separate from `#require`
**Effort**: Small (60 lines)
**Files**:
- `src/nova_lib_base.c` — add `nova_base_load`
- Register in `nova_base_lib[]` (line 1369)

**Rationale**: `load()` compiles a string into a callable function at
runtime. This is distinct from module loading — it's for dynamic code
generation, template engines, REPL internals, etc. It stays as a runtime
function because the source code literally doesn't exist until runtime.

`dofile()` and `loadfile()` are NOT implemented — their use cases are
covered by `#require` (modules) and `load()` (dynamic strings).

```c
static int nova_base_load(NovaVM *vm) {
    /* Get source string argument */
    /* Compile it through lex → pp → parse → compile → optimize */
    /* Push the resulting closure on success */
    /* Push nil + error string on failure */
}
```

**Register**:
```c
{"load", nova_base_load},
```

**Test criteria**:
```nova
local f = load("return 42")
assert(f() == 42)

local f2, err = load("invalid !!!")
assert(f2 == nil)
assert(type(err) == "string")

-- Dynamic code gen
local code = sprintf("return %d + %d", 10, 20)
assert(load(code)() == 30)
```

---

### 9.5 [ ] File Handle Methods (Userdata Metatables)

**Priority**: HIGH — `file:read()` / `file:write()` syntax doesn't work
**Effort**: Large (200-300 lines)
**Files**:
- `src/nova_lib_io.c` — major rework of file handles
- `src/nova_vm.c` — may need userdata metatable support
- `include/nova/nova_vm.h` — NovaUserdata type if not already there
- `tests/test_io.n` — new test file

**What exists**: `io.open` returns an integer slot index (line 78:
`static NovaFileHandle nova_io_files[NOVA_IO_MAX_FILES]`). This can't
support `file:read()` because integers don't have metatables.

**Current file handle comment** (line 31):
`/* Full handle-as-object with methods requires metatables (Phase 9). */`

**Implementation plan**:

1. **Create file userdata**: Instead of returning integer slot IDs, return
   a table with a `__handle` field (integer slot) and a metatable providing
   `read`, `write`, `close`, `seek`, `flush`, `lines` methods.

   ```c
   /* Create a file handle table with methods metatable */
   static void nova_io_push_handle(NovaVM *vm, int slot) {
       nova_vm_push_table(vm);
       int tidx = nova_vm_get_top(vm) - 1;

       /* Store slot ID */
       nova_vm_push_integer(vm, slot);
       nova_vm_set_field(vm, tidx, "__handle");

       /* Set metatable with __index = file methods table */
       nova_vm_push_table(vm);  /* metatable */
       int mtidx = nova_vm_get_top(vm) - 1;

       /* Build methods table */
       nova_vm_push_table(vm);  /* __index table */
       int midx = nova_vm_get_top(vm) - 1;

       /* Register file methods */
       nova_vm_push_cfunction(vm, nova_file_read);
       nova_vm_set_field(vm, midx, "read");
       nova_vm_push_cfunction(vm, nova_file_write);
       nova_vm_set_field(vm, midx, "write");
       /* ... close, seek, flush, lines ... */

       /* Set __index on metatable */
       nova_vm_set_field(vm, mtidx, "__index");
       /* Pop and set metatable */
       /* ... */
   }
   ```

2. **File methods**: Each method extracts `self.__handle` to find the slot:
   ```c
   static int nova_file_read(NovaVM *vm) {
       NovaValue self = nova_vm_get(vm, 0);  /* self (the file table) */
       /* Get __handle from self */
       /* Use the slot to do io.read logic */
   }
   ```

3. **Update `io.open`**: Return the handle table instead of integer.

**Test criteria** (`tests/test_io.n`):
```lua
-- Write and read back
local f = io.open("/tmp/nova_test.txt", "w")
f:write("hello nova\n")
f:close()

local f2 = io.open("/tmp/nova_test.txt", "r")
local line = f2:read("*l")
assert(line == "hello nova")
f2:close()

os.remove("/tmp/nova_test.txt")
```

---

### 9.6 [x] `coroutine.wrap` — Return Iterator Function

**Priority**: MEDIUM — useful for `for` loops with coroutines
**Effort**: Medium (60 lines)
**Files**:
- `src/nova_lib_coroutine.c` lines 251-304
- `tests/test_coroutine.n` — extend existing

**What exists** (line 291):
`/* Actually, let's implement wrap differently: return the coroutine ... */`
Currently returns the raw coroutine, not an iterator function.

**Implementation approach**: Since we don't have C closures with upvalues
yet, use a table-based wrapper:

```c
static int nova_co_wrap(NovaVM *vm) {
    /* 1. Create the coroutine from the function */
    /* 2. Create a wrapper table with { co = <coroutine> } */
    /* 3. Set a metatable with __call = wrap_resume_func */
    /* 4. wrap_resume_func extracts self.co and calls coroutine.resume */
    /* 5. If resume returns false (dead), raise error */
}
```

By using `__call` metamethod, the wrapper table becomes callable like
a function: `for val in coroutine.wrap(gen) do ... end`

**Test criteria**:
```lua
local function gen()
    coroutine.yield(1)
    coroutine.yield(2)
    coroutine.yield(3)
end

local iter = coroutine.wrap(gen)
assert(iter() == 1)
assert(iter() == 2)
assert(iter() == 3)
```

---

### 9.7 [x] `continue` Statement

**Priority**: MEDIUM — common expectation from all modern languages
**Effort**: Medium (80 lines across lexer, parser, compiler)
**Files**:
- `src/nova_lex.c` — add `TOKEN_CONTINUE` keyword
- `include/nova/nova_lex.h` — add to token enum
- `src/nova_parse_row.c` — parse `continue` as a statement
- `src/nova_compile.c` — emit jump to loop header
- `tests/test_stress.n` — extend existing

**Implementation plan**:

1. **Lexer**: Add `continue` to keyword table (next to `break`):
   ```c
   {"continue", TOKEN_CONTINUE},
   ```

2. **Parser**: Handle `continue` like `break` but targeting loop start:
   ```c
   case TOKEN_CONTINUE: {
       if (!cs->loop_depth) {
           parse_error(P, "cannot use 'continue' outside a loop");
       }
       /* Emit a pending jump that will be patched to loop header */
       nova_ast_row_add(P->ast, NOVA_AST_CONTINUE, ...);
       advance(P);
       break;
   }
   ```

3. **Compiler**: Emit `NOVA_OP_JMP` targeting the loop condition check:
   - `while` loop: jump to condition evaluation
   - `for` loop: jump to FORLOOP/TFORLOOP
   - `repeat` loop: jump to condition evaluation

   This requires the compiler to track the "continue target" address
   for each loop scope, similar to how `break` tracks the loop exit.

**Test criteria**:
```lua
-- Skip even numbers
local sum = 0
for i = 0, 9 do
    if i % 2 == 0 then continue end
    sum = sum + i
end
assert(sum == 25)  -- 1+3+5+7+9

-- Continue in while
local s = ""
local i = 0
while i < 5 do
    i = i + 1
    if i == 3 then continue end
    s = s .. tostring(i)
end
assert(s == "1245")
```

---

## PHASE 10: STANDARD LIBRARY HARDENING

### 10.1 [x] Comprehensive stdlib Test Suite

**Priority**: HIGH — we need tests before we can safely change anything
**Effort**: Medium (400 lines of test code)
**Files**: All new test files

Create dedicated test files for each standard library module:

| Test File | Module | Target # Tests |
|-----------|--------|----------------|
| `tests/test_string.n` | `string.*` | 40+ |
| `tests/test_table.n` | `table.*` | 30+ |
| `tests/test_math.n` | `math.*` | 30+ |
| `tests/test_io.n` | `io.*` | 20+ |
| `tests/test_os.n` | `os.*` | 15+ |
| `tests/test_package.n` | `require` / `package.*` | 15+ |
| `tests/test_control.n` | Control flow constructs | 30+ |

Each test file follows the established pattern:
```lua
local pass = 0
local fail = 0

local function check(name, cond)
    if cond then
        pass = pass + 1
        printf("  PASS: %s\n", name)
    else
        fail = fail + 1
        printf("  FAIL: %s\n", name)
    end
end

-- tests here...

printf("MODULE TESTS: %d passed, %d failed\n", pass, fail)
if fail > 0 then error("SOME TESTS FAILED") end
```

**Test coverage targets by module**:

**`test_string.n`**: len, sub (negative indices, out of bounds), upper,
lower, rep (with separator), reverse, byte (range), char (multiple),
find (plain + patterns once 9.1 done), format (%d, %s, %f, %x, %02d,
%10s, %%), gsub (plain + patterns), match (plain + patterns),
concatenation edge cases, empty strings.

**`test_table.n`**: insert (at end, at position), remove (end, position,
return value), concat (separator, range), sort (default, custom comparator
once 9.3 done), pack/unpack, move, length operator, mixed array/hash,
constructor syntax `{1, 2, key="val"}`.

**`test_math.n`**: All trig functions (sin, cos, tan, asin, acos, atan),
exp, log, pow, floor, ceil, abs, fmod, sqrt, min, max, random/randomseed,
type, tointeger, constants (pi, huge, maxinteger, mininteger).

**`test_io.n`**: open/close, read formats (*l, *n, *a, N bytes), write,
lines iterator, tmpfile, flush, stderr, file methods once 9.5 done.

**`test_os.n`**: clock (returns number), time (returns integer), date
(format string), getenv (existing var, missing var), execute (exit code),
remove/rename (files), tmpname.

**`test_package.n`**: require (load .n file, cached on second require),
package.loaded, package.path, error on missing module.

**`test_control.n`**: if/elseif/else, while, repeat-until, numeric for
(positive step, negative step, zero iterations), generic for (pairs,
ipairs), break (nested), goto (forward, backward, scope rules), do-end
block scoping, return (multiple values), tail calls, closures/upvalues,
variadic functions (...), nested function definitions.

---

### 10.2 [x] `table.move` Full Implementation

**Priority**: MEDIUM
**Effort**: Small (30 lines)
**Files**: `src/nova_lib_table.c` lines 380-420

**What exists**: Current implementation only works for same-table moves
and has bounds issues. Doesn't support the `a2` destination table.

**Fix**: Support 5th argument (destination table), handle overlapping
regions correctly (copy forward or backward as needed).

---

### 10.3 [x] Fix `nova_open_math` Stack Discipline

**Priority**: LOW (fixed by 9.2 above, but document the general problem)

The `nova_lib_register_module` function pops the module table from the
stack and sets it as a global. After that call, the table is no longer
on the stack, so relative indices like `-1` don't point to it anymore.

**Pattern to follow for any module that needs post-registration setup**:
```c
/* Register the module */
nova_lib_register_module(vm, "modulename", module_lib);

/* Get it back from globals to add more fields */
NovaValue mod = nova_vm_get_global(vm, "modulename");
nova_vm_push_value(vm, mod);
int tidx = nova_vm_get_top(vm) - 1;

/* Add fields */
nova_vm_push_number(vm, 3.14);
nova_vm_set_field(vm, tidx, "constant_name");

/* Clean up stack */
nova_vm_set_top(vm, tidx);
```

---

## PHASE 10.5: ABSTRACTION LAYER (Engine Serviceability)

**Goal**: Make Nova's internals *swappable* — NaN-boxing, DAGGER, Weave,
or any future backend change should be bolt-on, not open-heart surgery.

**Philosophy**: Right now, ~520 call sites across 12+ files directly
access `.type`, `.as.*`, `->data`, `->length`, `->hash`, table probe
loops, etc. Swapping ANY internal representation means touching all of
them. This phase introduces macro/function boundaries so that consumers
never see implementation details. After this, changing the value
representation means editing ~30 macro definitions in ONE header file.

### 10.5a [x] Value Accessor Macros

**Effort**: Small (50 lines in header)
**Files**: `include/nova/nova_vm.h`

Add type-checking and extraction macros:
```c
/* Type checks (return int 0/1) */
#define nova_is_nil(v)       ((v).type == NOVA_TYPE_NIL)
#define nova_is_bool(v)      ((v).type == NOVA_TYPE_BOOL)
#define nova_is_integer(v)   ((v).type == NOVA_TYPE_INTEGER)
#define nova_is_number(v)    ((v).type == NOVA_TYPE_NUMBER)
#define nova_is_string(v)    ((v).type == NOVA_TYPE_STRING)
#define nova_is_table(v)     ((v).type == NOVA_TYPE_TABLE)
#define nova_is_function(v)  ((v).type == NOVA_TYPE_FUNCTION)
#define nova_is_cfunction(v) ((v).type == NOVA_TYPE_CFUNCTION)
#define nova_is_thread(v)    ((v).type == NOVA_TYPE_THREAD)
#define nova_is_numeric(v)   (nova_is_integer(v) || nova_is_number(v))
#define nova_typeof(v)       ((v).type)

/* Value extraction (caller must check type first) */
#define nova_as_bool(v)      ((v).as.boolean)
#define nova_as_integer(v)   ((v).as.integer)
#define nova_as_number(v)    ((v).as.number)
#define nova_as_string(v)    ((v).as.string)
#define nova_as_table(v)     ((v).as.table)
#define nova_as_closure(v)   ((v).as.closure)
#define nova_as_cfunction(v) ((v).as.cfunc)
#define nova_as_coroutine(v) ((v).as.coroutine)
#define nova_as_userdata(v)  ((v).as.userdata)
```

When NaN-boxing arrives, only these macros change. The 520+ call sites
remain untouched.

### 10.5b [x] String Accessor Macros

**Effort**: Tiny (5 lines in header)
**Files**: `include/nova/nova_vm.h`

```c
#define nova_str_data(s)   ((s)->data)
#define nova_str_len(s)    ((s)->length)
#define nova_str_hash(s)   ((s)->hash)
```

When Weave strings replace NovaString, only these macros change.

### 10.5c [x] Table Public API

**Effort**: Small (change `static` to non-static, add declarations)
**Files**: `include/nova/nova_vm.h`, `src/nova_vm.c`

Promote these from `static` in nova_vm.c to public API:
- `nova_table_get_str(t, key)` → lookup by NovaString*
- `nova_table_set_str(vm, t, key, val)` → insert by NovaString*
- `nova_table_get_int(t, key)` → lookup by integer
- `nova_table_set_int(vm, t, key, val)` → insert by integer
- `nova_table_new(vm)` → create empty table
- `nova_table_free(vm, t)` → destroy table
- `nova_table_next(t, &idx, &key, &val)` → iteration
- `nova_string_new(vm, s, len)` → create string
- `nova_string_equal(a, b)` → compare strings

Move `NOVAI_STRING_SEED` to the header to eliminate duplication.

When DAGGER replaces the hash table: reimplement these ~8 functions,
nothing else changes. All 12+ consumer files remain untouched.

### 10.5d [ ] Migrate VM Core (nova_vm.c)

**Effort**: Medium (mechanical search-and-replace)
**Files**: `src/nova_vm.c`

Replace all `.type ==` with `nova_is_*()`, all `.as.*` with `nova_as_*()`,
all `->data`/`->length`/`->hash` with `nova_str_*()`.

### 10.5e [ ] Migrate Metamethods (nova_meta.c)

**Effort**: Small
**Files**: `src/nova_meta.c`

Remove duplicated `NOVAI_STRING_SEED`. Replace direct hash probing with
`nova_table_get_str()`. Replace direct `.type`/`.as.` with macros.

### 10.5f [ ] Migrate GC (nova_gc.c)

**Effort**: Small
**Files**: `src/nova_gc.c`

GC needs to walk table internals for marking — this is the ONE consumer
that legitimately needs to see inside tables. Use accessor macros where
possible, but `nova_table_mark()` helper function handles the rest.

### 10.5g [ ] Migrate Standard Library Files

**Effort**: Medium (largest file is nova_lib_base.c)
**Files**: `src/nova_lib_base.c`, `src/nova_lib_string.c`,
`src/nova_lib_table.c`, `src/nova_lib_data.c`, `src/nova_lib_io.c`,
`src/nova_lib_os.c`, `src/nova_lib_math.c`, `src/nova_lib_coroutine.c`,
`src/nova_lib_package.c`

Replace ALL direct struct access with macros and API functions.
This is where the bulk of duplicated hash probing lives — it all
gets replaced with `nova_table_get/set` calls.

### 10.5h [ ] Full Test Suite Validation

**Effort**: Tiny (run `make test`)
**Files**: None (validation only)

All 18 test suites (696 tests) must pass after every migration step.
Each file is migrated and tested individually before moving on.

---

## PHASE 11: DEVELOPER EXPERIENCE

### 11.1 [x] `debug` Library

**Status**: COMPLETE
**Priority**: HIGH — debugging is currently blind
**Effort**: Large (300 lines)
**Files**:
- `src/nova_lib_debug.c` — new file (CREATED)
- `include/nova/nova_lib.h` — `nova_open_debug` declared
- `src/nova_lib_base.c` — added to `nova_open_libs()`

**Implemented functions**: `debug.traceback()`, `debug.getinfo()`,
`debug.getlocal()`, `debug.sethook()` (stub), `debug.gethook()` (stub).
`nova_vm_traceback()` added to `nova_vm.c` for C-level traceback generation.

**Functions to implement**:

| Function | Description |
|----------|-------------|
| `debug.traceback([msg] [,level])` | Generate stack trace string |
| `debug.getinfo(f [,what])` | Get function info (name, source, line) |
| `debug.sethook(hook, mask [,count])` | Set debug hook (call/return/line) |
| `debug.getlocal(level, idx)` | Get local variable name+value |
| `debug.setlocal(level, idx, val)` | Set local variable |
| `debug.getupvalue(f, idx)` | Get upvalue name+value |
| `debug.setupvalue(f, idx, val)` | Set upvalue |

**Minimum viable**: `traceback` and `getinfo` are the critical ones.
Hook support can be deferred.

**Implementation reference** — traceback walks the call frame stack:
```c
static int nova_debug_traceback(NovaVM *vm) {
    /* Walk vm->call_frames from current back to base */
    /* For each frame, emit: "  source:line: in function 'name'" */
    /* If frame has a NovaProto, use proto->source and
     * proto->debug_lines[pc - proto->code] for line number */
}
```

The VM already has `vm->call_frames` and `vm->frame_count`. Each
`NovaCallFrame` has a `proto` (with source/line info) and `ip` (current
instruction pointer). This gives us everything for a basic traceback.

---

### 11.2 [x] Stack Traces on Error

**Status**: COMPLETE
**Priority**: HIGH — companion to 11.1
**Effort**: Small (30 lines)
**Files**:
- `src/nova_vm.c` — `novai_error()` auto-prepends `source:line:` to runtime errors
- `src/nova.c` — auto-prints `nova_vm_traceback()` after unhandled errors
- `src/nova_compile.c` — `NovaExprDesc.line` field added; `novai_discharge_to_reg`
  now uses expression line info instead of hardcoded 0
- `include/nova/nova_compile.h` — `line` field added to `NovaExprDesc`

**Result**: All runtime errors show `source:line:` prefix and full stack
traceback. Works correctly at all nesting depths (top-level through
deeply nested functions). Compiler bug fixed where `novai_discharge_to_reg`
emitted line 0 for all instructions (GETFIELD, GETTABLE, etc.).

---

### 11.3 [ ] User-Facing Language Reference

**Priority**: MEDIUM — currently no docs for end users
**Effort**: Medium (documentation only)
**Files**:
- `docs/NOVA_LANGUAGE_REFERENCE.md` — new file
- `docs/NOVA_STDLIB_REFERENCE.md` — new file

**Contents for `NOVA_LANGUAGE_REFERENCE.md`**:
1. Types (nil, boolean, integer, number, string, table, function, thread)
2. Variables (local, global, scoping rules)
3. Operators (arithmetic, comparison, logical, bitwise, string)
4. Control flow (if, while, repeat, for, break, continue, goto, return)
5. Functions (declaration, multiple returns, varargs, closures)
6. Tables (constructors, indexing, 0-based arrays, metatables)
7. String interpolation (backtick syntax)
8. Preprocessor (#include, #import, #require, #define, #ifdef)
9. Module system (#require vs return pattern)
10. Coroutines
11. Error handling (pcall, xpcall, error)

**Contents for `NOVA_STDLIB_REFERENCE.md`**:
- One section per module: signature, description, examples
- Auto-generate from the registration tables where possible

---

## PHASE 12: ADVANCED FEATURES

### 12.1 [x] NaN-Boxing Value Representation

**Status**: COMPLETE (Feb 10, 2026)
**Result**: `NovaValue` reduced from 16 bytes to 8 bytes (uint64_t).
All 10.5a-c accessor macros have dual implementations (NaN-boxed and
tagged-union) controlled by `#ifdef NOVA_NAN_BOXING`.
Integers packed via SIGN_BIT|QNAN, objects via QNAN|type tags.
21/21 benchmark checks passing.

---

### 12.2 [x] DAGGER Hash Table Integration

**Status**: COMPLETE (Feb 11, 2026)
**Result**: NovaTable hash part replaced with DaggerTable from
`zorya_c_program/src/dagger.c`. O(1) lookup with NXH64 hashing.
String interning integrated via Weave (12.3).

---

### 12.3 [x] Weave String Integration

**Status**: COMPLETE (Feb 11, 2026)
**Result**: NovaString backed by Weave intern pool from
`zorya_c_program/src/zorya_weave.c`. String equality is O(1)
pointer comparison. All strings interned at creation.
24/24 intern benchmark checks passing.

---

### 12.4 [x] Async/Await Runtime

**Status**: COMPLETE (Feb 14, 2026)
**Result**: Full cooperative async/await built on coroutine infrastructure.
New files: `nova_async.c` (task scheduler), `nova_lib_async.c` ("task" stdlib).
OP_CALL intercepts `is_async` functions to create coroutines with stashed args.
OP_AWAIT does blocking resume with cooperative tick for spawned tasks.
OP_SPAWN enqueues to VM task queue. GC-safe with root marking for
task queue and pending_args. Coroutine-local error handler (setjmp)
prevents longjmp from bypassing cleanup.
25/25 async test checks across 18 tests.

---

## PHASE 13: ECOSYSTEM & TOOLING

### 13.1 [ ] VS Code Syntax Highlighting Update

Update the existing Nova VS Code extension to cover new keywords
(`continue`) and patterns.

### 13.2 [ ] Error Code Documentation

Generate user-facing documentation from the error catalog in
`nova_error.c`. Each error code (E1xxx compile, E2xxx runtime,
E3xxx I/O, W1xxx warnings) should have a page with:
- What it means
- Common causes
- How to fix it
- Example code that triggers it

### 13.3 [ ] Example Programs

Build a collection of real-world example programs:
- CLI tool (argument parsing, file processing)
- Data pipeline (CSV → JSON transform)
- Config file processor
- Simple HTTP client (once networking exists)
- Game of Life / Mandelbrot (showcase computation)

---

## EXECUTION ORDER

Optimized dependency chain — each step builds on the previous:

```
COMPLETED:
  9.1  String pattern matching ......... DONE
  9.2  Math constants fix ............... DONE
  9.3  table.sort comparator ........... DONE
  9.4  #require directive .............. DONE
  9.4b load() string compilation ....... DONE
  9.6  coroutine.wrap .................. DONE
  9.7  continue statement .............. DONE
  10.1 Comprehensive test suite ........ DONE (20 suites, 740+ checks)
  10.2 table.move full impl ............ DONE
  10.3 Math stack discipline fix ....... DONE
  10.5a Value accessor macros .......... DONE
  10.5b String accessor macros ......... DONE
  10.5c Table public API ............... DONE
  12.1 NaN-boxing ...................... DONE (Feb 10)
  12.2 DAGGER hash tables .............. DONE (Feb 11)
  12.3 Weave strings ................... DONE (Feb 11)
  12.4 Async/await ..................... DONE (Feb 14)

  FAILING TESTS: ALL FIXED
    - test_math: FIXED — NaN-box-safe 48-bit integer limits
    - test_io: FIXED — nova_is_cfunction SIGN_BIT discriminant
    - test_metamethods: FIXED — same NaN-boxing fix
    - test_stress: FIXED — same NaN-boxing fix
    (Root cause: nova_is_cfunction macro lacked SIGN_BIT in mask,
     causing every NaN-boxed integer to match as a cfunction)

  11.1 debug library ................... DONE
  11.2 Stack traces on error ........... DONE

REMAINING (roughly by priority):
  9.5  File handle methods ............. ~250 lines (blocks io tests)
  10.5d Migrate VM core ................ mechanical, medium effort
  10.5e Migrate metamethods ............ small
  10.5f Migrate GC ..................... small
  10.5g Migrate stdlib files ........... medium (largest is lib_base)
  10.5h Full test suite validation ..... run `make test`
  11.3 Language reference docs ......... docs only
  13.1 VS Code extension update ........ syntax highlighting
  13.2 Error code docs ................. auto-generate
  13.3 Example programs ................ showcase real programs

  END-TO-END VALIDATION (new):
    - Write a full program using .m macro files
    - Compile to .no serialized bytecode
    - Execute .no file and validate output
```

---

## NOVA FILE HEADER CONVENTION

All `.n` source files should follow this directive ordering:

```nova
-- ============================================
-- File header comments
-- ============================================

-- 1. Textual inclusion (macros, constants, enums)
#include "constants.m"

-- 2. Built-in module activation
#import data

-- 3. User module loading
#require lib/utils
#require lib/parser as P

-- 4. Code begins
-- ============================================
```

**Rationale**:
- `#include` first because macros may be needed by later directives
- `#import` next because built-in modules are resolved instantly
- `#require` last because it may trigger compilation of other files
- All three are preprocessor directives, resolved before parsing
- No `require()` calls inside function bodies or conditionally
- No `#export` needed — modules export via `return { ... }`

This convention is enforced by style, not by the compiler (for now).
Future `zorya-lint` could warn on out-of-order directives.

---

## CURRENT TEST SUITE INVENTORY

| Suite | File | Tests | Status |
|-------|------|-------|--------|
| Base | `tests/tier1_test.n` | ~25 | PASS |
| Stress | `tests/test_stress.n` | 84 | PASS |
| 0-Index | `tests/test_0index.n` | 53 | PASS |
| Interpolation | `tests/test_interpolation.n` | 16 | PASS |
| Metamethods | `tests/test_metamethods.n` | 12 | PASS |
| Coroutine | `tests/test_coroutine.n` | 10 | PASS |
| Xpcall | `tests/test_xpcall.n` | 8 | PASS |
| GC | `tests/test_gc.n` | 10 | PASS |
| Data | `tests/test_data.n` | 101 | PASS |
| Data Real | `tests/test_data_real.n` | 98 | PASS |
| String | `tests/test_string.n` | 74 | PASS |
| Require | `tests/test_require.n` | 21 | PASS |
| Control | `tests/test_control.n` | 42 | PASS |
| Table | `tests/test_table.n` | 66 | PASS |
| Math | `tests/test_math.n` | 66 | PASS |
| IO | `tests/test_io.n` | 17 | PASS |
| OS | `tests/test_os.n` | 25 | PASS |
| Package | `tests/test_package.n` | 15 | PASS |
| Intern Bench | `tests/test_bench_intern.n` | 24 | PASS |
| NaN-box Bench | `tests/test_bench_nanbox.n` | 21 | PASS |
| Async | `tests/test_async.n` | 25 | PASS |
| **TOTAL** | | **788** | **20 PASS, 0 FAIL** |

---

## POST-COMPLETION BENCHMARKS

After each phase, re-run the performance benchmarks:
```bash
./bin/nova tests/bench_extended.n
```

Track regressions — string patterns and debug hooks can impact hot
paths if not carefully implemented.

---

**ZORYA CORPORATION — Engineering Excellence, Democratized**
