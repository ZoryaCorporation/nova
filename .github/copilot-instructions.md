# NOVA VM - Copilot Instructions

## Project Overview

**Nova** is a register-based bytecode virtual machine written in C99.
It implements a dynamically-typed scripting language with Lua-like syntax
and semantics — featuring a powerful preprocessor (#import, macros, includes),
string method syntax (s:upper(), s:find()), NINI config format, NDP data
processing, in-process CLI tools, a task runner, and the full C-style
print family (printf, sprintf, fprintf).

**Author**: Anthony Taliento (Zorya Corporation)
**Standard**: ZORYA-C v2.0.0
**Language**: C99 with strict warnings (-Wall -Wextra -Werror -pedantic)
**Platform**: Linux (Fedora 42) with GCC 15.2.1

---

## Architecture

- **NaN-boxing**: Values packed in 64-bit doubles (SIGN_BIT|QNAN for ints, QNAN|TAG for objects)
- **Register-based VM**: 32-bit instructions, computed-goto dispatch
- **DAGGER hash tables**: O(1) lookup with NXH64 hash
- **Weave string interning**: All strings interned for O(1) equality
- **Mark-and-sweep GC**: Tri-color marking, incremental collection, per-type metatables rooted in VM struct
- **Closures**: Upvalue migration (stack → heap on close)
- **Coroutines**: Symmetric transfer with independent stacks
- **Async/await**: Event loop with promise-based concurrency
- **Metatables**: Operator overloading, prototype inheritance, string method dispatch via OP_SELF
- **Modules**: `require()` with package.path search
- **NINI**: Native INI config format with task sections, variable interpolation, inline arrays
- **NDP**: Multi-format data processing (JSON, CSV, NINI, INI, TOML, HTML, YAML)
- **CLI Tools**: Built-in cat, ls, tree, find, grep, head, tail, wc, pwd + interactive tool shell

## Directory Structure

```
nova/
├── src/                    # VM and compiler implementation
│   ├── nova_vm.c           # Main VM (dispatch loop, GC, error handling)
│   ├── nova_compile.c      # Compiler (AST → bytecode)
│   ├── nova_lex.c          # Lexer/scanner
│   ├── nova_parse.c        # Parser (tokens → AST)
│   ├── nova_meta.c         # Metamethod dispatch (string metatables, __index, __eq, etc.)
│   ├── nova_gc.c           # Tri-color mark-sweep GC
│   ├── nova_nini.c         # NINI standalone codec
│   ├── nova_ndp.c          # NDP multi-format data processing
│   ├── nova_tools.c        # CLI tools (cat, ls, grep, etc.) + task runner
│   ├── nova_lib_tools.c    # In-process tools module (tools.cat, tools.grep, etc.)
│   ├── nova_lib_*.c        # Standard library modules
│   └── zorya/              # Vendored Zorya SDK
│       ├── nxh.c           # NXH64 hash function
│       ├── dagger.c        # DAGGER hash table
│       └── weave.c         # Weave string intern pool
├── include/
│   ├── nova/               # Nova headers
│   │   ├── nova_vm.h       # VM types and API
│   │   ├── nova_compile.h  # Compiler types
│   │   ├── nova_opcode.h   # Instruction set
│   │   ├── nova_meta.h     # Metamethod API
│   │   ├── nova_nini.h     # NINI codec API
│   │   └── ...
│   └── zorya/              # Vendored Zorya headers
│       ├── nxh.h           # Hash function
│       ├── dagger.h        # Hash table
│       ├── weave.h         # String interning
│       ├── pcm.h           # Performance macros
│       └── zorya_arena.h   # Arena allocator
├── tests/                  # Test suites (976+ tests, 26 suites)
├── examples/               # Example Nova scripts
├── docs/                   # Design documents and blueprints
├── taskfile.nini           # NINI-based build tasks
└── Makefile                # Build system
```

## Standard Library Modules

| Module | Description | Key Functions |
|--------|-------------|---------------|
| base | Global functions | echo, print, printf, sprintf, type, tostring, tonumber, error, assert, pcall, xpcall |
| math | Mathematics | abs, ceil, floor, sqrt, sin, cos, tan, log, exp, random, pi, huge |
| string | String ops | len, sub, upper, lower, rep, find, format, gsub, match, gmatch, byte, char, reverse |
| table | Table ops | insert, remove, sort, concat, move, pack, unpack |
| io | File I/O | open, close, read, write, lines |
| os | OS interface | execute, capture, getenv, setenv, clock, time, date, cwd, chdir, sleep, platform |
| fs | Filesystem | read, write, exists, isfile, isdir, list, walk, find, glob, mkdir, copy, move, stat |
| tools | In-process tools | cat, ls, tree, find, grep, head, tail, wc, pwd, run |
| coroutine | Coroutines | create, resume, yield, wrap, status |
| async | Async/await | run, spawn, sleep, status, wrap |
| debug | Debugging | traceback, getinfo, getlocal, sethook |
| net | HTTP client | get, post, put, delete, patch, head, request (`#import net`) |
| sql | SQLite3 | open, exec, query, close (`#import sql`) |
| nlp | Text processing | tokenize, stem, fuzzy, freq, tfidf, ngrams |
| data/nini | NINI codec | decode, encode, load, save (`#import nini`) |
| data/json | JSON codec | decode, encode, load, save (`#import json`) |
| data/csv | CSV codec | decode, encode, load, save (`#import csv`) |

## Output Functions

**`echo`** is Nova's preferred output function. It prints all arguments
separated by tabs, followed by a newline — identical to Lua's `print`.
`print` exists as a compatibility alias. Always prefer `echo` in Nova code.

```lua
echo("hello", "world")    -- hello    world
echo(42, true, nil)        -- 42    true    nil
```

For formatted output, use the C-style `printf` / `sprintf` / `fprintf` family.

## String Method Syntax

Strings support colon method calls via a shared metatable with `__index = string`:
```lua
dec s = "hello world"
s:upper()          -- "HELLO WORLD"
s:find("world")    -- 6
s:sub(0, 5)        -- "hello"
s:len()            -- 11
s:rep(3)           -- "hello worldhello worldhello world"
s:gsub("o", "0")   -- "hell0 w0rld"
```
The compiler emits `OP_SELF` for method calls. The string metatable is GC-rooted
via `vm->string_mt` in the NovaVM struct.

## NINI Format

Nova's native configuration format (lingua franca). Import with `#import nini`.

```ini
# Comments with # or ;
[section]
key = value              # Auto-typed: int, float, bool, nil, string
items[] = one            # Array push
items[] = two
tags = [a, b, c]         # Inline array
path = ${section.base}/sub  # Variable interpolation

[task:build]             # Task section → stored in __tasks.build
command = make
depends = [clean]
env.CC = gcc
```

## NINI Task Runner

Build tasks are defined in `taskfile.nini` and executed via `nova task`:
```bash
nova task              # List tasks
nova task build        # Run 'build' task
nova task clean build  # Run multiple tasks
nova task test         # Deps resolved automatically (test → build)
```

## CLI Tools

Available as `nova <tool>` subcommands or in the interactive shell (`nova` with no args):

| Tool | Usage | Description |
|------|-------|-------------|
| cat | `nova cat file.txt` | Print file contents |
| ls | `nova ls [dir]` | List directory |
| tree | `nova tree [dir]` | Directory tree |
| find | `nova find [dir] -m=*.c` | Find files by pattern |
| grep | `nova grep [file] -m=pattern` | Search text |
| head | `nova head file.txt` | First N lines |
| tail | `nova tail file.txt` | Last N lines |
| wc | `nova wc file.txt` | Line/word/char counts |
| task | `nova task [name]` | Run NINI taskfile tasks |

From scripts, use the `tools` module (zero subprocess overhead):
```lua
dec content = tools.cat("README.md")
dec entries = tools.ls("src")          -- Table of {name, type, size}
dec matches = tools.grep("TODO", "src/nova_vm.c")  -- Table of {file, num, text}
dec files   = tools.find(".", "*.n")   -- Table of paths
dec counts  = tools.wc("Makefile")     -- {lines, words, chars}
dec output  = tools.run("make clean")  -- Capture command stdout
```

## Coding Standards

Follow ZORYA-C v2.0.0. Key rules:

1. **NULL checks** before every pointer dereference
2. **Allocation checks** — every `malloc`/`calloc` must be checked
3. **Explicit casts** for type conversions
4. **Default case** in every switch statement
5. **`zorya_` prefix** for vendored code, `nova_`/`novai_` for Nova code
6. **`novai_` prefix** for internal (static) functions
7. **State pointer named `N`** for Nova VM state
8. **K&R brace style**, 4-space indentation, no tabs
9. **File headers** with @file, @brief, @author, @date, @copyright
10. **Function docs** with @brief, @param, @return

## Error Handling

```c
/* Nova uses setjmp/longjmp for error propagation */
novai_error(N, "error message: %s", detail);

/* Standard error codes for zorya vendored code */
typedef enum {
    ZORYA_OK = 0,
    ZORYA_ERROR_NULLPTR = -1,
    ZORYA_ERROR_NOMEM = -2,
    /* ... */
} zorya_error_t;
```

## Build Commands

```bash
make                # Build release
make DEBUG=1        # Build with debug symbols + sanitizers
make clean          # Clean artifacts
make test           # Run all 976+ tests across 26 suites
make trace          # Build with trace instrumentation
make lib            # Build libnova.a static library
make install        # Install to /usr/local
```

Or use the NINI task runner:
```bash
nova task build     # Same as 'make'
nova task test      # Build + test
nova task clean     # Same as 'make clean'
```

## Test Files

Tests use `.n` file extension (Nova scripts). Each test file is self-validating
and prints PASS/FAIL results. The test runner (`make test`) executes all
`tests/test_*.n` files and checks for failures.

---

**ZORYA CORPORATION - Engineering Excellence, Democratized**
