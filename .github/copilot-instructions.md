# NOVA VM - Copilot Instructions

## Project Overview

**Nova** is a register-based bytecode virtual machine written in C99.
It implements a dynamically-typed scripting language with Lua-like syntax and semantics.

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
- **Mark-and-sweep GC**: Tri-color marking, incremental collection
- **Closures**: Upvalue migration (stack → heap on close)
- **Coroutines**: Symmetric transfer with independent stacks
- **Async/await**: Event loop with promise-based concurrency
- **Metatables**: Operator overloading and prototype inheritance
- **Modules**: `require()` with package.path search

## Directory Structure

```
nova/
├── src/                  # VM and compiler implementation
│   ├── nova_vm.c         # Main VM (dispatch loop, GC, error handling)
│   ├── nova_compile.c    # Compiler (AST → bytecode)
│   ├── nova_lex.c        # Lexer/scanner
│   ├── nova_parse.c      # Parser (tokens → AST)
│   ├── nova_lib_*.c      # Standard library modules
│   └── zorya/            # Vendored Zorya SDK
│       ├── nxh.c         # NXH64 hash function
│       ├── dagger.c      # DAGGER hash table
│       └── weave.c       # Weave string intern pool
├── include/
│   ├── nova/             # Nova headers
│   │   ├── nova_vm.h     # VM types and API
│   │   ├── nova_compile.h # Compiler types
│   │   ├── nova_opcodes.h # Instruction set
│   │   └── ...
│   └── zorya/            # Vendored Zorya headers
│       ├── nxh.h         # Hash function
│       ├── dagger.h      # Hash table
│       ├── weave.h       # String interning
│       ├── pcm.h         # Performance macros
│       └── zorya_arena.h # Arena allocator
├── tests/                # Test suites (788 tests, 20 suites)
├── examples/             # Example Nova scripts
├── docs/                 # Design documents and blueprints
└── Makefile              # Build system
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
make test           # Run all 788 tests
```

## Test Files

Tests use `.n` file extension (Nova scripts). Each test file is self-validating
and prints PASS/FAIL results. The test runner (`make test`) executes all
`tests/test_*.n` files and checks for failures.

---

**ZORYA CORPORATION - Engineering Excellence, Democratized**
