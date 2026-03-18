<!-- ============================================================
     ZORYA-C STANDARD DOCUMENT
     Coding Standard for High-Performance C Systems Development

     Author:  Anthony Taliento (Zorya Corporation)
     Date:    2026-03-13 (When this frontmatter was added)
     Version: 2.0.0
     Status:  LIVING STANDARD
     ============================================================ -->



# ZORYA-C CODING STANDARD
## High-Performance Reliability Without Dogma

**Version**: 2.0.0
**Last Updated**: March 11, 2026
**Maintainer**: Anthony Taliento, Zorya Corporation
**Status**: LIVING STANDARD — Evolves with project needs
**License**: Apache License 2.0 — free to use, adapt, and redistribute

---

## EXECUTIVE SUMMARY

ZORYA-C is a C coding standard for high-performance, safety-critical systems development. It is **inspired by MISRA-C** but adapted for the real-world demands of language runtimes, embedded systems, CLI tools, and high-performance C projects.

**Key Principles**:
1. **Safety Critical**: Zero undefined behavior, always
2. **Performance Pragmatic**: No overhead in hot paths
3. **Explicit Intent**: Code documents its own reasoning
4. **Audit-First**: Every exception is findable and justified

**Certification Status**: We do **NOT** claim MISRA-C compliance. ZORYA-C is its own public, reviewable standard. MISRA-C certification is expensive and time-consuming; we adopt its engineering discipline without legal liability.

> **Adopting ZORYA-C**: This standard is project-agnostic. Replace the example
> prefixes (`NOVA_`, `nova_`) with your project's prefix. The principles are
> universal; the naming is yours.

---

## TABLE OF CONTENTS

1. [Philosophy & Relationship to MISRA-C](#1-philosophy--relationship-to-misra-c)
2. [Rule Categories & Numbering](#2-rule-categories--numbering)
3. [Safety-Critical Rules (001-099)](#3-safety-critical-rules-001-099)
4. [Performance-Adapted Rules (100-199)](#4-performance-adapted-rules-100-199)
5. [Runtime-Specific Rules (200-299)](#5-runtime-specific-rules-200-299)
6. [Platform-Agnostic Rules (300-399)](#6-platform-agnostic-rules-300-399)
7. [Documentation Standards (400-499)](#7-documentation-standards-400-499)
8. [Enforcement & Tooling](#8-enforcement--tooling)
9. [Exception Handling Process](#9-exception-handling-process)
10. [Migration Guide](#10-migration-guide)

---

# 1. PHILOSOPHY & RELATIONSHIP TO MISRA-C

## 1.1 The Zorya Vision: Engineering Excellence for All

At Zorya Corporation, we believe that world-class C system development practices should not be locked behind expensive certifications or exclusive to automotive and aerospace industries. ZORYA-C represents our mission to **democratize engineering excellence** - bringing MISRA-C's rigorous discipline to every C developer, from embedded firmware engineers to language runtime architects.

**Our Core Philosophy**:

1. **Universal Standards, Zero Compromise** - Safety-critical discipline benefits ALL systems, not just those with regulatory requirements. Every segmentation fault prevented, every buffer overflow caught, every undefined behavior eliminated improves software quality regardless of domain.

2. **Performance Without Recklessness** - High performance and reliability are not opposing forces. Through careful analysis, measured optimization, and explicit documentation, we achieve both. Every performance exception is justified with data; every micro-optimization is traceable.

3. **Teaching Through Doing** - ZORYA-C is not just a rulebook — it is an **educational framework**. Each rule includes rationale, examples, and enforcement guidance. We empower developers to understand *why* rules exist, not just follow them blindly.

4. **Pragmatism Over Dogma** - We respect MISRA-C's foundations but refuse artificial constraints. When MISRA-C forbids `goto` entirely, we study Linux kernel’s cleanup patterns and adapt intelligently. When MISRA-C bans function-like macros, we create the PCM (Performance-Critical Macro) framework to allow them responsibly.

5. **Community Empowerment** - C systems development has a steep learning curve. ZORYA-C serves as a **mentor in code form** — guiding newcomers from basic pointer safety to advanced optimization techniques, from simple memory management to garbage collector design.

## 1.2 Why MISRA-C is Our Foundation

MISRA-C represents **40+ years of hard-earned lessons** from safety-critical systems where failure means death. When an antilock braking system fails at 70mph, the consequences are catastrophic. This discipline produced rules like:

- **No undefined behavior** - Because undefined behavior in production is non-negotiable
- **Explicit null checks** - Because segmentation faults destroy uptime
- **Switch default cases** - Because invalid enum values happen in real systems
- **No implicit conversions** - Because sign extension bugs are silent killers

These rules were forged in fire. We adopt them wholesale in our Safety-Critical category (ZORYA-C-001 through ZORYA-C-099) because **they represent wisdom worth preserving**.

## 1.3 Where We Diverge: Performance Realities

MISRA-C emerged from automotive embedded systems with different constraints than modern language runtimes:

**MISRA-C Context**: Microcontrollers with 64KB RAM, single-threaded, real-time constraints measured in microseconds, code reviewed line-by-line by certification authorities.

**Runtime Context**: Multi-core CPUs, gigabytes of RAM, garbage collectors running millions of operations/second, JIT compilers generating machine code dynamically, performance measured in nanoseconds per operation.

**Our Adaptations**:

- **goto for Cleanup** - MISRA forbids; we allow with documentation. Linux kernel uses this pattern universally for resource cleanup. When you have 5 initialization steps that can fail, `goto cleanup_N` is clearer than nested `if` statements.

- **Performance-Critical Macros (PCM)** - MISRA forbids function-like macros; we allow with justification. When `NOVA_UPVALUE_INDEX(i)` is called 10 million times/second in the VM loop, the 5-nanosecond function call overhead compounds to 50ms/second - real performance loss for zero safety gain.

- **Direct Field Access in Hot Paths** - MISRA prefers accessor functions; we prefer direct access where profiled. In a garbage collector mark phase running 1000 times/second, accessor overhead is measurable waste.

- **Aggressive Inlining** - MISRA limits inlining; we encourage it strategically. Modern compilers inline effectively, and hot path functions benefit enormously.

## 1.4 Our Mission: Elevating the Entire C Ecosystem

ZORYA-C is not tied to any single project. It is our **gift to the C community** - a modern, pragmatic, performance-aware coding standard that:

**For Beginners**:
- Teaches pointer safety from day one
- Explains *why* null checks matter (with crash examples)
- Introduces memory management discipline early
- Builds intuition for undefined behavior risks

**For Intermediate Developers**:
- Demonstrates hot path optimization techniques
- Shows when to use macros vs inline functions (with profiling data)
- Teaches platform-agnostic CPU feature detection
- Guides through multi-threaded safety considerations

**For Expert Developers**:
- Provides framework for documenting performance exceptions
- Enables measured trade-offs between safety and speed
- Establishes audit trails for every deviation
- Creates vocabulary for discussing optimization strategies

**For Teams**:
- Offers consistent, enforceable standards
- Integrates with modern CI/CD pipelines
- Supports automated compliance checking
- Enables code review with clear rationale references

## 1.5 Teaching Advanced Concepts Through Standards

ZORYA-C embeds **systems programming education** into its structure:

**Memory Safety** - ZORYA-C-006 (No Use After Free) teaches you to null pointers after free, but the rationale explains *why*: use-after-free is exploitable, creates security vulnerabilities, and manifests as Heisenbugs. The rule becomes a lesson.

**Performance Analysis** - ZORYA-C-121 (PCM) requires you to measure performance impact before using macros. This teaches profiling discipline - you learn `perf`, `valgrind`, `cachegrind`. The exception process becomes a performance engineering course.

**Platform Portability** - ZORYA-C-301 (CPUID Wrapper) shows you how to write code that adapts at runtime to CPU capabilities. You learn about SIMD instruction sets, ARM vs x86 differences, and feature detection APIs. The rule becomes a portability guide. Zorya Also provides a PAL (Platform Abstraction Layer) library for freestanding C projects that all are welcome to use in their projects.

**Concurrency Safety** - Future ZORYA-C rules on thread safety will teach lock-free algorithms, memory ordering, and atomic operations - not just forbid dangerous patterns.

## 1.6 Our Promise: No Hidden Costs, No Lock-In

**Transparency**: Every ZORYA-C rule is public, version-controlled, and open to scrutiny. We document our reasoning, cite our sources, and invite criticism. Our mission is to provide a clear, teachable standard and tools to developers, engineers, and students worldwide.

**Affordability**: ZORYA-C is free. No certification fees, no vendor lock-in, no expensive audits. Compliance checking uses open-source tools (clang-tidy, cppcheck, gcc).

**Adaptability**: Your project has unique constraints? Fork ZORYA-C. Modify rules. Document exceptions. We provide the framework; you own the decisions.

**Evolution**: ZORYA-C is a living standard. As compiler technology improves, as CPU architectures evolve, as new safety research emerges - we update. Version-controlled evolution with clear changelogs.

## 1.7 Join the Movement

ZORYA-C represents a **philosophy shift** in C development:

- From "C is dangerous" to "C is powerful when disciplined"
- From "safety or performance" to "safety AND performance"
- From "expensive certification" to "transparent excellence"
- From "follow rules blindly" to "understand principles deeply"

We invite you to:
- **Use ZORYA-C** in your projects (any domain, any scale)
- **Teach ZORYA-C** to newcomers (embed learning in code review)
- **Contribute to ZORYA-C** (propose rules, share lessons learned)
- **Challenge ZORYA-C** (find weaknesses, suggest improvements)

**Our goal**: Make C system development accessible, safe, and performant for the next generation of engineers.

## 1.8 Why "ZORYA"?

Zorya, in Slavic mythology, represents the **guardian of cosmic order** - watching over the universe, preventing chaos. This embodies our mission:

- **Guardianship** - Protecting codebases from undefined behavior
- **Order** - Bringing discipline to complex systems
- **Vigilance** - Continuous monitoring through static analysis
- **Enlightenment** - Teaching through transparency

ZORYA-C is our **watchful guardian** for your C systems.

---

**REMEMBER**: Excellence is not expensive. Excellence is disciplined. Excellence is teachable. Excellence is ZORYA-C.

---

# 2. RULE CATEGORIES & NUMBERING

Rules are numbered by category for easy reference:

| Range | Category | Description |
|-------|----------|-------------|
| 001-099 | Safety Critical | MISRA-C rules adopted wholesale (non-negotiable) |
| 100-199 | Performance Adapted | MISRA-C rules modified for runtime performance |
| 200-299 | Runtime Specific | Original rules for language runtime development |
| 300-399 | Platform Agnostic | Portability and cross-platform compatibility |
| 400-499 | Documentation | Code quality and maintainability standards |

**Rule Format**:
```
ZORYA-C-XXX: <Rule Title>

RULE: <Concise statement of requirement>

RATIONALE: <Why this rule exists>

EXAMPLES:
    [COMPLIANT]: <code example>
    [VIOLATION]: <code example>

EXCEPTIONS: <When rule can be waived (if ever)>

ENFORCEMENT: <How to check compliance>
```

---

# 3. SAFETY-CRITICAL RULES (001-099)

These rules are adopted from MISRA-C **without modification**. Violations are **never acceptable**.

## ZORYA-C-001: No Implicit Function Declarations

**RULE**: All functions must be declared before use. No implicit declarations.

**RATIONALE**: Implicit declarations cause type mismatches, stack corruption, and undefined behavior. Compilers guess return types (often wrong).

**EXAMPLES**:
```c
// [COMPLIANT]
NOVA_API int nova_gettop(NovaVM *N);  // Declared in header

int main(void) {
        NovaVM *N = nova_open(NULL, NULL);
        int top = nova_gettop(N);  // Safe - declaration exists
}

// [VIOLATION]
int main(void) {
        int top = nova_gettop(N);  // Compiler guesses return type!
}
```

**ENFORCEMENT**: Compile with `-Werror=implicit-function-declaration`

---

## ZORYA-C-002: Null Pointer Checks Before Dereference

**RULE**: All pointer parameters and return values must be checked for NULL before dereferencing.

**RATIONALE**: Null pointer dereferences cause segmentation faults. Check before use, always.

**EXAMPLES**:
```c
// [COMPLIANT]
int nova_process_value(NovaVM *N, TValue *value) {
        if (N == NULL) return NOVA_ERRMEM;
        if (value == NULL) return NOVA_ERRMEM;

        // Safe to dereference now
        return value->tt_;
}

// [VIOLATION]
int nova_process_value(NovaVM *N, TValue *value) {
        return value->tt_;  // CRASH if value is NULL!
}
```

**EXCEPTIONS**: None. Always check.

**ENFORCEMENT**: Static analysis (clang-analyzer, cppcheck)

---

## ZORYA-C-003: All Switch Statements Have Default Case

**RULE**: Every switch statement must include a default case, even if "impossible".

**RATIONALE**: Enums can be cast from integers. Invalid values must be handled gracefully.

**EXAMPLES**:
```c
// [COMPLIANT]
switch (type) {
        case NOVA_TNIL: return "nil";
        case NOVA_TNUMBER: return "number";
        case NOVA_TSTRING: return "string";
        default:
                return "unknown";  // Handle invalid type
}

// [VIOLATION]
switch (type) {
        case NOVA_TNIL: return "nil";
        case NOVA_TNUMBER: return "number";
        // Missing default - undefined behavior for invalid type!
}
```

**ENFORCEMENT**: Compile with `-Wswitch-default`

---

## ZORYA-C-004: Explicit Unused Parameter Annotation

**RULE**: Unused function parameters must be explicitly annotated with `(void)cast`.

**RATIONALE**: Makes intent clear - parameter unused by design, not oversight.

**EXAMPLES**:
```c
// [COMPLIANT]
int nova_stub_function(NovaVM *N) {
        (void)N;  // Explicit: L intentionally unused
        return NOVA_OK;
}

// [VIOLATION]
int nova_stub_function(NovaVM *N) {
        return NOVA_OK;  // Compiler warning - unused parameter
}
```

**ENFORCEMENT**: Compile with `-Wunused-parameter`

---

## ZORYA-C-005: No Implicit Type Conversions

**RULE**: All type conversions must be explicit with cast operators.

**RATIONALE**: Implicit conversions hide truncation, sign loss, and precision loss.

**EXAMPLES**:
```c
// [COMPLIANT]
uint32_t flags = 0xFF;
uint8_t byte = (uint8_t)flags;  // Explicit truncation

// [VIOLATION]
uint32_t flags = 0xFF;
uint8_t byte = flags;  // Implicit truncation!
```

**ENFORCEMENT**: Compile with `-Wconversion`

---

## ZORYA-C-006: No Use After Free

**RULE**: Pointers must be set to NULL after `free()`. Never access freed memory.

**RATIONALE**: Use-after-free is undefined behavior and security vulnerability.

**EXAMPLES**:
```c
// [COMPLIANT]
void cleanup(void *ptr) {
        if (ptr != NULL) {
                free(ptr);
                ptr = NULL;  // Prevent use-after-free
        }
}

// [VIOLATION]
void cleanup(void *ptr) {
        free(ptr);
        // ptr still points to freed memory!
}
```

**ENFORCEMENT**: Dynamic analysis (valgrind, ASan)

---

## ZORYA-C-007: All Memory Allocations Checked

**RULE**: Every `malloc()`, `calloc()`, `realloc()` must check for NULL return.

**RATIONALE**: Allocation can fail. Dereferencing NULL pointer crashes.

**EXAMPLES**:
```c
// [COMPLIANT]
void *buffer = malloc(size);
if (buffer == NULL) {
        return NOVA_ERRMEM;
}

// [VIOLATION]
void *buffer = malloc(size);
memset(buffer, 0, size);  // CRASH if allocation failed!
```

**ENFORCEMENT**: Static analysis (scan-build)

---

## ZORYA-C-008: No Array Index Out of Bounds

**RULE**: Array indices must be validated before access.

**RATIONALE**: Buffer overruns cause crashes and security vulnerabilities.

**EXAMPLES**:
```c
// [COMPLIANT]
int get_element(int *array, size_t size, size_t index) {
        if (index >= size) return -1;  // Bounds check
        return array[index];
}

// [VIOLATION]
int get_element(int *array, size_t size, size_t index) {
        return array[index];  // No bounds check!
}
```

**ENFORCEMENT**: Static analysis + dynamic checks (ASan)

---

## ZORYA-C-009: No Signed Integer Overflow

**RULE**: All arithmetic operations on signed integers must be checked for overflow.

**RATIONALE**: Signed overflow is undefined behavior per C standard.

**EXAMPLES**:
```c
// [COMPLIANT]
#include <limits.h>

int safe_add(int a, int b, int *result) {
        if (a > 0 && b > INT_MAX - a) return NOVA_ERRRUN;  // Overflow check
        if (a < 0 && b < INT_MIN - a) return NOVA_ERRRUN;  // Underflow check
        *result = a + b;
        return NOVA_OK;
}

// [VIOLATION]
int unsafe_add(int a, int b) {
        return a + b;  // Undefined behavior if overflow!
}
```

**ENFORCEMENT**: Compile with `-ftrapv` or `-fsanitize=undefined`

---

## ZORYA-C-010: No Uninitialized Variables

**RULE**: All variables must be initialized before use.

**RATIONALE**: Uninitialized variables contain garbage values - undefined behavior.

**EXAMPLES**:
```c
// [COMPLIANT]
int status = NOVA_OK;  // Initialized at declaration
int count = 0;

// [VIOLATION]
int status;  // Garbage value
if (status == NOVA_OK) { ... }  // Undefined behavior!
```

**ENFORCEMENT**: Compile with `-Wuninitialized`

---

# 4. PERFORMANCE-ADAPTED RULES (100-199)

These rules are **modified from MISRA-C** to balance safety with runtime performance requirements.

## ZORYA-C-120: Macro Naming (VERBOSE_ONLY)

**RULE**: All macros must use FULL WORDS. No abbreviations except universally understood terms (CPU, GPU, SIMD, OPCODE, INDEX, etc.).

**RATIONALE**: Ambiguous abbreviations cause confusion. `NUM` could mean Number, Numeric, NUMA, Numeral. Clear naming is non-negotiable.

**EXAMPLES**:
```c
// [COMPLIANT]
#define NOVA_NUMBER_OPCODES ((int)(NOVA_OP_FUTURE_7) + 1)
#define NOVA_CPU_FEATURE_MASK 0xFFFF
#define NOVA_SIMD_WIDTH_256 256
#define NOVA_OPCODE_ADD 0x01  // OPCODE universally understood

// [VIOLATION]
#define NOVA_NUM_OPCODES ...     // NUM ambiguous
#define NOVA_PROC_COUNT ...      // PROC = Processor? Process? Procedure?
#define NOVA_MAX_LEN ...         // LEN = Length? Lens? Lenient?
#define NOVA_OP_ADD 0x01         // OP too short without context
```

**UNIVERSALLY UNDERSTOOD ABBREVIATIONS** (Allowed):
- CPU, GPU, TPU (hardware)
- SIMD, AVX, SSE, NEON (instruction sets)
- RAM, ROM, VRAM (memory)
- API, ABI, SDK (interfaces)
- JIT, AOT, VM (compilation)
- OPCODE, BYTECODE (instructions)
- INDEX, OFFSET, SIZE, COUNT (measurements)
- UTF, ASCII, UNICODE (encodings)

**ENFORCEMENT**: Code review + grep audit

---

## ZORYA-C-121: Performance-Critical Macros (PCM)

**RULE**: Function-like macros are **FORBIDDEN** except for calculated constants. All PCMs must be documented with header.

**RATIONALE**: Function-like macros bypass type checking and debugger. Only allow when performance-critical and no inline function alternative exists.

**PCM DOCUMENTATION FORMAT**:
```c
/*
** PCM: NOVA_UPVALUE_INDEX
** Purpose: Calculate pseudo-index for upvalue access in VM stack
** Rationale: Called millions of times/sec in hot loop. Inline function
**            adds 2-3 CPU cycles (5-10ns) due to ABI calling convention.
**            Macro has zero overhead (compile-time constant folding).
** Performance Impact: 10,000,000 calls/sec × 5ns = 50ms saved per second
** Alternative Rejected: static inline adds function call overhead
** Written: 2025-11-05
*/
#define NOVA_UPVALUE_INDEX(i) (NOVA_REGISTRYINDEX - (i))
```

**EXAMPLES**:
```c
// [COMPLIANT] - Calculated constant with PCM header
/* PCM: NOVA_UPVALUE_INDEX ... (see above) */
#define NOVA_UPVALUE_INDEX(i) (NOVA_REGISTRYINDEX - (i))

// [COMPLIANT] - Platform token (not a function)
#define NOVA_API extern

// [VIOLATION] - Function-like macro without PCM header
#define GET_VALUE(o) ((o)->value_)  // Use inline function instead

// [VIOLATION] - Macro when inline function viable
#define MAX(a,b) ((a)>(b)?(a):(b))  // Use inline function
```

**ENFORCEMENT**:
1. Search codebase: `grep -r "#define.*(" include/ src/`
2. Each match must have PCM header OR be removed
3. PCM documentation reviewed during major releases

---

## ZORYA-C-122: Calculated Constants Only

**RULE**: Function-like macros **FORBIDDEN** except:
1. Calculated constants (index calculations, bit operations)
2. Platform-specific token replacement (NOVA_API, export macros)
3. Header guards (#ifndef NOVA_H_)

All other cases must use inline functions.

**RATIONALE**: Inline functions provide type safety, debuggability, and similar performance on modern compilers.

**EXAMPLES**:
```c
// [COMPLIANT] - Calculated constant
#define NOVA_UPVALUE_INDEX(i) (NOVA_REGISTRYINDEX - (i))

// [COMPLIANT] - Platform token
#define NOVA_API extern

// [COMPLIANT] - Header guard
#ifndef NOVA_H_
#define NOVA_H_

// [VIOLATION] - Use inline function
#define GET_TYPE(o) ((o)->tt_)

// CORRECT REPLACEMENT
static inline int nova_gettype(const TValue *o) {
        return o->tt_;
}
```

**ENFORCEMENT**: Code review + PCM audit

---

## ZORYA-C-150: goto for Cleanup Only

**RULE**: goto statements **ALLOWED** for error cleanup and resource deallocation. goto statements **FORBIDDEN** for control flow.

**RATIONALE**: Linux kernel, SQLite, and high-quality C codebases use goto for cleanup. MISRA-C's blanket restriction causes real-world performance problems (automotive GPS sluggishness). We adopt pragmatic policy: goto for necessity (cleanup), not convenience (flow).

**ALLOWED PATTERNS**:
1. Error cleanup with multiple resources
2. Single exit point for complex functions
3. Breaking out of nested loops (rare - document justification)

**FORBIDDEN PATTERNS**:
1. Backward jumps (goto label_above_current_line)
2. Jumping into loops or blocks
3. Multiple gotos creating spaghetti logic
4. goto when if/else/switch/break/continue works

**EXAMPLES**:
```c
// [COMPLIANT] - Cleanup pattern
int nova_initialize_engines(NovaVM *N) {
        void *buffer = NULL;
        int status;

        buffer = malloc(BUFFER_SIZE);
        if (buffer == NULL) {
                status = NOVA_ERRMEM;
                goto cleanup_none;
        }

        status = nova_init_crypto(N);
        if (status != NOVA_OK) {
                goto cleanup_buffer;
        }

        status = nova_init_network(N);
        if (status != NOVA_OK) {
                goto cleanup_crypto;
        }

        return NOVA_OK;

cleanup_crypto:
        nova_shutdown_crypto(N);
cleanup_buffer:
        free(buffer);
cleanup_none:
        return status;
}

// [VIOLATION] - Control flow spaghetti
void bad_function(int x) {
        if (x > 10) goto middle;
        printf("Start\n");

middle:
        if (x < 5) goto end;
        printf("Middle\n");
        goto start;  // Backward jump!

end:
        printf("End\n");
        return;

start:
        printf("Back to start?\n");
}
```

**DOCUMENTATION REQUIREMENT**:
```c
/* goto used for cleanup - ZORYA-C-150 compliant */
goto cleanup_resources;
```

**ENFORCEMENT**: Code review + grep for "goto"

---

## ZORYA-C-151: Direct Field Access in Hot Paths

**RULE**: In performance-critical code paths (VM loops, GC, JIT), direct struct field access is **PREFERRED** over accessor functions.

**RATIONALE**: Accessor functions add overhead (ABI, stack frame). In hot paths called millions of times per second, this compounds.

**EXAMPLES**:
```c
// [COMPLIANT] - Hot path (VM execution loop)
static inline void vm_execute_opcode(TValue *o) {
        int type = o->tt_;           // Direct access
        Value val = o->value_;       // Direct access
        GCObject *gc = val.gc;       // Direct access
}

// [ACCEPTABLE] - Cold path (initialization)
int nova_initialize_value(TValue *o) {
        int type = nova_gettype(o);   // Accessor OK in cold path
        return NOVA_OK;
}

// [VIOLATION] - Hot path using accessor (unnecessary overhead)
static inline void vm_execute_opcode(TValue *o) {
        int type = nova_gettype(o);   // Adds function call overhead!
}
```

**CLASSIFICATION**:
- **Hot Path**: VM loop, GC mark/sweep, JIT code generation, string interning
- **Cold Path**: Initialization, error handling, debugging, configuration

**ENFORCEMENT**: Performance profiling + code review

---

## ZORYA-C-152: Aggressive Inline Functions

**RULE**: Functions called in hot paths should be `static inline` unless:
1. Function > 50 lines (inline bloat risk)
2. Function has complex control flow (inline impractical)
3. Function address needed (callbacks, function pointers)

**RATIONALE**: Modern compilers inline effectively. Inline eliminates call overhead in hot paths.

**EXAMPLES**:
```c
// [COMPLIANT] - Hot path function inlined
static inline int nova_gettag(const TValue *o) {
        return o->tt_;
}

// [COMPLIANT] - Large function NOT inlined
int nova_complex_operation(NovaVM *N) {
        // 200 lines of code...
        // Too large to inline
}

// [COMPLIANT] - Function address needed
int nova_callback(NovaVM *N) {
        // Used as function pointer - cannot inline
}

// [VIOLATION] - Hot path function not inlined
int nova_gettag(const TValue *o) {  // Missing static inline!
        return o->tt_;
}
```

**ENFORCEMENT**: Performance profiling + compiler optimization reports

---

# 5. RUNTIME-SPECIFIC RULES (200-299)

These are **original rules** specific to language runtime development.

## ZORYA-C-200: Consistent State Pointer Naming

**RULE**: The primary runtime state pointer must use a consistent single-letter name throughout the project (`N`, `L`, etc.) — not `state`, `vm`, `runtime`, or other verbose names.

**RATIONALE**: Consistency across the codebase. Every developer immediately recognizes the state pointer. Nova uses `N`; Lua-heritage projects may use `L`.

**EXAMPLES**:
```c
// [COMPLIANT]
void nova_function(NovaVM *N) { }
int nova_operation(NovaVM *N, int arg) { }

// [VIOLATION]
void nova_function(NovaVM *state) { }      // Verbose — use N
void nova_function(NovaVM *runtime) { }    // Verbose — use N
void nova_function(NovaVM *vm) { }         // Verbose — use N
```

**EXCEPTIONS**: When multiple state pointers exist, use `N1`, `N2`, or descriptive names like `parent_N`, `child_N`.

**ENFORCEMENT**: Code review + grep audit

---

## ZORYA-C-201: Engine Init Returns Status Code

**RULE**: All engine initialization functions must:
1. Return `int` status code (NOVA_OK or error)
2. Take `NovaVM *N` as first parameter
3. Follow naming pattern: `nova_initialize_<engine>_engine()`

**RATIONALE**: Consistent error handling. Enables graceful degradation if engine fails to initialize.

**EXAMPLES**:
```c
// [COMPLIANT]
NOVA_API int nova_initialize_crypto_engine(NovaVM *N) {
        if (N == NULL) return NOVA_ERRMEM;

        // Initialize crypto subsystem

        return NOVA_OK;
}

// [VIOLATION] - Returns void (can't signal error)
void nova_initialize_crypto_engine(NovaVM *N) { }

// [VIOLATION] - Wrong naming pattern
int init_crypto(NovaVM *N) { }

// [VIOLATION] - State not first parameter
int nova_initialize_crypto_engine(int flags, NovaVM *N) { }
```

**ENFORCEMENT**: Code review + static analysis

---

## ZORYA-C-202: Hot Path Functions Documented

**RULE**: Functions in performance-critical paths must be marked with `/* HOT */` comment and profiling data.

**RATIONALE**: Makes optimization priorities clear. Helps future maintainers know where performance matters.

**EXAMPLES**:
```c
/* HOT: Called 10M times/sec in VM loop - 40% of runtime */
static inline int vm_execute_add(NovaVM *N, TValue *ra, TValue *rb) {
        // Implementation
}

/* COLD: Called once at startup */
int nova_initialize_vm(NovaVM *N) {
        // Implementation
}
```

**ENFORCEMENT**: Performance profiling required before marking HOT

---

## ZORYA-C-203: Consistent Project Naming

**RULE**: All references to "lua", "Lua", "LUA" in type names, function names, macros, and variables are **FORBIDDEN** except in migration compatibility layers.

**RATIONALE**: Prevents confusion between upstream and your project. Use your project prefix consistently.

**EXAMPLES**:
```c
// [COMPLIANT]
NovaVM *N;
#define NOVA_OK 0
int nova_gettop(NovaVM *N);

// [VIOLATION]
NovaVM *N;              // Use NovaVM
#define LUA_OK 0           // Use NOVA_OK
#define NOVALIB_API ...     // Use NOVALIB_API
int nova_gettop(...);       // Use nova_gettop
```

**EXCEPTIONS**: Migration shim layer (deprecated, to be removed).

**ENFORCEMENT**: Grep audit: `grep -r "lua\|LUA" include/ src/` must return zero matches

---

## ZORYA-C-204: Single Type Definition Location

**RULE**: Each type must have ONE canonical definition location. Duplicate type definitions across headers are **FORBIDDEN**.

**RATIONALE**: Prevents definition conflicts, ensures single source of truth.

**EXAMPLES**:
```c
// [COMPLIANT]
// In nova_vm.h (CANONICAL)
typedef struct NovaHardwareProfile {
        // Definition
} NovaHardwareProfile;

// In hcc.h (USER)
#include "nova_vm.h"  // Use canonical definition

// [VIOLATION]
// In nova_vm.h
typedef struct NovaHardwareProfile { ... } NovaHardwareProfile;

// In hcc.h (DUPLICATE - FORBIDDEN!)
typedef struct NovaHardwareProfile { ... } NovaHardwareProfile;
```

**ENFORCEMENT**: Compile with `-Werror=duplicate-decl-specifier`

---

## ZORYA-C-205: Platform Detection Runtime Not Compile-Time

**RULE**: Hardware capabilities (SIMD, CPU features) must be detected at **RUNTIME** when possible, not compile-time.

**RATIONALE**: Build once, run anywhere. One binary adapts to multiple CPU architectures.

**EXAMPLES**:
```c
// [COMPLIANT] - Runtime detection
uint32_t nova_detect_cpu_features(void) {
        uint32_t features = 0;
        #if defined(__x86_64__) || defined(_M_X64)
                unsigned int eax, ebx, ecx, edx;
                if (__get_cpuid(1, &eax, &ebx, &ecx, &edx)) {
                        if (edx & (1 << 26)) features |= NOVA_CPU_SSE2;
                        if (ecx & (1 << 28)) features |= NOVA_CPU_AVX;
                }
        #endif
        return features;
}

// [VIOLATION] - Compile-time only
#ifdef __AVX2__
        #define USE_AVX2 1  // Binary only works on AVX2 CPUs!
#endif
```

**EXCEPTIONS**: Architecture-specific code (x86 vs ARM) requires compile-time detection.

**ENFORCEMENT**: Code review

---

# 6. PLATFORM-AGNOSTIC RULES (300-399)

Rules ensuring cross-platform compatibility.

## ZORYA-C-300: Use Boolean Macros for Platform Detection

**RULE**: Platform detection must use boolean macros (0 or 1), not `#ifdef`.

**RATIONALE**: Boolean macros allow `#if` conditionals (clearer than `#ifdef`).

**EXAMPLES**:
```c
// [COMPLIANT]
#if NOVA_OS_LINUX
        // Linux-specific code
#elif NOVA_OS_WINDOWS
        // Windows-specific code
#endif

// [VIOLATION]
#ifdef NOVA_OS_LINUX  // Always defined (0 or 1)!
```

**ENFORCEMENT**: Code review

---

## ZORYA-C-301: Platform-Agnostic CPUID Wrapper

**RULE**: CPUID detection must use unified wrapper function supporting GCC, Clang, and MSVC intrinsics.

**RATIONALE**: Different compilers have different CPUID APIs. Wrapper abstracts differences.

**PATTERN**:
```c
static uint32_t nova_detect_cpu_features(void) {
        uint32_t features = 0;

        #if defined(__x86_64__) || defined(_M_X64)
                #if defined(__GNUC__) || defined(__clang__)
                        /* GCC/Clang: __get_cpuid() */
                #elif defined(_MSC_VER)
                        /* MSVC: __cpuid() */
                #endif
        #elif defined(__aarch64__) || defined(__arm__)
                /* ARM: getauxval() */
        #endif

        return features;
}
```

**ENFORCEMENT**: Code review + platform testing

---

## ZORYA-C-302: No Hardcoded Endianness

**RULE**: Endianness must be detected at runtime or compile-time, never assumed.

**RATIONALE**: Code must work on both little-endian (x86, ARM) and big-endian (SPARC, POWER) systems.

**EXAMPLES**:
```c
// [COMPLIANT]
static inline bool is_little_endian(void) {
        uint32_t test = 1;
        return *(uint8_t *)&test == 1;
}

// [VIOLATION]
#define IS_LITTLE_ENDIAN 1  // Assumes x86!
```

**ENFORCEMENT**: Testing on multiple architectures

---

# 7. DOCUMENTATION STANDARDS (400-499)

Rules for code quality and maintainability.

## ZORYA-C-400: File Header Required

**RULE**: Every source file (.c, .h) must have Zorya Corporation standard header.

**TEMPLATE**:
```c
/*
** ═══════════════════════════════════════════════════════════════════════════
** ZORYA CORPORATION
** ═══════════════════════════════════════════════════════════════════════════
** Project: <Your Project>
** File: <filename>
** Purpose: <one-line description>
** Component: <subsystem name>
**
** MISSION: <detailed mission statement>
**
** EDUCATIONAL PHILOSOPHY:
** <What this file teaches/demonstrates>
**
** ACADEMIC REFERENCES:
** - <Paper/technique references>
**
** COMMUNITY TRIBUTE:
** Built upon Lua 5.4.8 by Roberto Ierusalimschy, Waldemar Celes,
** Luiz Henrique de Figueiredo (PUC-Rio, Brazil)
** ═══════════════════════════════════════════════════════════════════════════
*/
```

**ENFORCEMENT**: Pre-commit hook checks file headers

---

## ZORYA-C-401: Function Documentation

**RULE**: Public API functions require documentation comment with:
- Purpose
- Parameters
- Return value
- Complexity (if non-trivial)
- Thread safety

**TEMPLATE**:
```c
/*
** ─────────────────────────────────────────────────────────────────────────
** Function: nova_functionname
** Purpose: <what it does>
**
** Parameters:
**   N       - NovaVM pointer (non-NULL)
**   param1  - <description>
**
** Returns: <return value description>
**
** Complexity: O(<complexity>)
** Thread-Safe: <yes/no/conditional>
** Side-Effects: <list any state changes>
** ─────────────────────────────────────────────────────────────────────────
*/
```

**ENFORCEMENT**: Code review

---

## ZORYA-C-402: No TODO Without Tracking

**RULE**: TODO/FIXME comments must reference issue tracker or be removed.

**EXAMPLES**:
```c
// [COMPLIANT]
// TODO(issue-#42): Optimize memory allocation

// [VIOLATION]
// TODO: Fix this later  // No tracking!
```

**ENFORCEMENT**: Pre-commit hook rejects untracked TODOs

---

# 8. ENFORCEMENT & TOOLING

## 8.1 Static Analysis Tools

### Recommended Tools:
1. **clang-tidy** - LLVM-based static analyzer
2. **cppcheck** - Lightweight C/C++ checker
3. **scan-build** - Clang static analyzer frontend
4. **gcc -Wall -Wextra -Werror** - Compiler warnings as errors

### clang-tidy Configuration
```yaml
# .clang-tidy
Checks: >
    bugprone-*,
    clang-analyzer-*,
    readability-*,
    modernize-*,
    performance-*,
    -modernize-use-trailing-return-type,
    -readability-identifier-length

WarningsAsErrors: '*'

CheckOptions:
    - key: readability-identifier-naming.FunctionCase
        value: lower_case
    - key: readability-identifier-naming.VariableCase
        value: lower_case
    - key: readability-identifier-naming.TypedefCase
        value: CamelCase
```

### cppcheck Configuration
```bash
cppcheck --enable=all \
                 --error-exitcode=1 \
                 --suppress=missingInclude \
                 --inline-suppr \
                 --std=c99 \
                 --language=c \
                 src/ include/
```

### GCC Flags
```makefile
CFLAGS += -Wall -Wextra -Werror
CFLAGS += -Wimplicit-function-declaration
CFLAGS += -Wuninitialized
CFLAGS += -Wunused-parameter
CFLAGS += -Wswitch-default
CFLAGS += -Wconversion
CFLAGS += -Wformat=2
CFLAGS += -Wstrict-prototypes
CFLAGS += -Wmissing-prototypes
CFLAGS += -Wold-style-definition
```

---

## 8.2 Dynamic Analysis Tools

1. **Valgrind** - Memory leak detection
2. **AddressSanitizer (ASan)** - Buffer overflows, use-after-free
3. **UndefinedBehaviorSanitizer (UBSan)** - Undefined behavior detection
4. **ThreadSanitizer (TSan)** - Race condition detection

### ASan + UBSan Build
```makefile
CFLAGS += -fsanitize=address -fsanitize=undefined
CFLAGS += -fno-omit-frame-pointer
```

---

## 8.3 Pre-Commit Hooks

```bash
#!/bin/bash
# .git/hooks/pre-commit

# Check file headers
python3 tools/check_headers.py || exit 1

# Run clang-tidy
clang-tidy --quiet $(git diff --cached --name-only | grep '\.[ch]$') || exit 1

# Check for Lua naming
if git diff --cached | grep -E 'lua_|LUA_|LUALIB'; then
        echo "ERROR: Lua naming found (ZORYA-C-203)"
        exit 1
fi

# Check for untracked TODOs
if git diff --cached | grep -E 'TODO|FIXME' | grep -v 'issue-#'; then
        echo "ERROR: Untracked TODO/FIXME found (ZORYA-C-402)"
        exit 1
fi
```

---

## 8.4 CI/CD Integration

```yaml
# .github/workflows/zorya-c-compliance.yml
name: ZORYA-C Compliance

on: [push, pull_request]

jobs:
    static-analysis:
        runs-on: ubuntu-latest
        steps:
            - uses: actions/checkout@v2

            - name: Install tools
                run: |
                    sudo apt-get install clang-tidy cppcheck

            - name: Run clang-tidy
                run: clang-tidy src/*.c include/*.h

            - name: Run cppcheck
                run: cppcheck --enable=all --error-exitcode=1 src/ include/

            - name: Check Lua naming
                run: |
                    if grep -r "lua_\|LUA_\|LUALIB" src/ include/; then
                        echo "Lua naming found - violates ZORYA-C-203"
                        exit 1
                    fi
```

---

# 9. EXCEPTION HANDLING PROCESS

## 9.1 When Exceptions Are Allowed

Rules can be waived **ONLY** when:
1. Physical impossibility (no alternative exists)
2. External library compatibility requirement
3. Performance critical with measured justification

## 9.2 Exception Documentation

**REQUIRED ANNOTATION**:
```c
/* ZORYA-C-XXX EXCEPTION
 * Rule: <rule number and name>
 * Reason: <why exception needed>
 * Alternative Rejected: <why standard approach won't work>
 * Approval: <reviewer name and date>
 * Review Date: <when to reassess>
 */
```

**EXAMPLE**:
```c
/* ZORYA-C-122 EXCEPTION
 * Rule: Calculated Constants Only (function-like macros forbidden)
 * Reason: Legacy Lua bytecode compatibility requires exact bit manipulation
 * Alternative Rejected: Inline function changes ABI, breaks compatibility
 * Approval: Anthony Taliento, 2025-11-05
 * Review Date: 2026-01-01 (reassess after Lua migration complete)
 */
#define LUA_COMPAT_MASK(x) ((x) & 0xFF)
```

## 9.3 Exception Review

All exceptions must be reviewed:
- **Quarterly**: Assess if exception still needed
- **Before release**: Ensure no workarounds exist
- **After migration**: Remove compatibility exceptions

---

# 10. MIGRATION GUIDE

## 10.1 Automated Refactoring Tools

### Tool 1: Lua Naming Eliminator
```bash
#!/bin/bash
# tools/eliminate_lua_naming.sh

# Replace lua_State with NovaVM
find src/ include/ -type f -name '*.[ch]' -exec sed -i 's/lua_State/NovaVM/g' {} +

# Replace upstream constants with your prefix
find src/ include/ -type f -name '*.[ch]' -exec sed -i 's/LUA_OK/NOVA_OK/g' {} +
# Replace LUA_ERRRUN, LUA_MULTRET, etc. with your project constants
# find src/ include/ -type f -name '*.[ch]' -exec sed -i 's/LUA_ERRRUN/NOVA_ERRRUN/g' {} +

# Replace NOVALIB_API with NOVALIB_API
find src/ include/ -type f -name '*.[ch]' -exec sed -i 's/NOVALIB_API/NOVALIB_API/g' {} +

echo "Lua naming elimination complete"
```

### Tool 2: Function-Like Macro Finder
```bash
#!/bin/bash
# tools/find_function_macros.sh

echo "Function-like macros requiring PCM headers or removal:"
echo "=========================================================="

grep -rn "#define.*(" include/ src/ | \
    grep -v "^.*:.*#ifndef" | \
    grep -v "^.*:.*#ifdef" | \
    grep -v "^.*:.*#define NOVA_.*_H_" | \
    while read line; do
        file=$(echo "$line" | cut -d: -f1)
        linenum=$(echo "$line" | cut -d: -f2)
        content=$(echo "$line" | cut -d: -f3-)

        # Check if PCM header exists above
        if ! grep -B10 "^$content" "$file" | grep -q "PCM:"; then
            echo "$file:$linenum: Missing PCM header"
            echo "  $content"
            echo ""
        fi
    done
```

### Tool 3: Null Check Inserter
```python
#!/usr/bin/env python3
# tools/insert_null_checks.py

import re
import sys

def insert_null_checks(filename):
        with open(filename, 'r') as f:
                lines = f.readlines()

        # Find function definitions
        func_pattern = re.compile(r'^\w+\s+\w+\((.*)\)\s*{')

        modified = []
        i = 0
        while i < len(lines):
                match = func_pattern.match(lines[i])
                if match:
                        params = match.group(1)
                        # Extract pointer parameters
                        pointers = re.findall(r'(\w+)\s+\*(\w+)', params)

                        modified.append(lines[i])
                        i += 1

                        # Insert null checks
                        for ptype, pname in pointers:
                                check = f"    if ({pname} == NULL) return NOVA_ERRMEM;\n"
                                modified.append(check)
                else:
                        modified.append(lines[i])
                        i += 1

        with open(filename, 'w') as f:
                f.writelines(modified)

if __name__ == '__main__':
        for filename in sys.argv[1:]:
                insert_null_checks(filename)
                print(f"Inserted null checks in {filename}")
```

### Tool 4: Unused Parameter Annotator
```bash
#!/bin/bash
# tools/annotate_unused_params.sh

# Compile with -Wunused-parameter to find unused params
gcc -Wunused-parameter -c src/*.c 2>&1 | \
    grep "unused parameter" | \
    while read line; do
        file=$(echo "$line" | cut -d: -f1)
        param=$(echo "$line" | grep -oP "parameter '\K[^']+")

        # Insert (void)param; annotation
        sed -i "s/\(.*$param.*\)/\1\n    (void)$param;/" "$file"
    done

echo "Unused parameter annotations complete"
```

---

## 10.2 Common Violations & Fixes

### Violation 1: Implicit Function Declaration
```c
// [BEFORE]
int main(void) {
        int result = nova_operation();  // Implicit declaration
}

// [AFTER]
#include "nova_vm.h"  // Declares nova_operation()

int main(void) {
        int result = nova_operation();
}
```

### Violation 2: No Null Check
```c
// [BEFORE]
void process(void *ptr) {
        memset(ptr, 0, 100);  // CRASH if ptr NULL!
}

// [AFTER]
void process(void *ptr) {
        if (ptr == NULL) return;
        memset(ptr, 0, 100);
}
```

### Violation 3: Function-Like Macro
```c
// [BEFORE]
#define GET_TYPE(o) ((o)->tt_)

// [AFTER]
static inline int nova_gettype(const TValue *o) {
        return o->tt_;
}
```

### Violation 4: Lua Naming
```c
// [BEFORE]
NovaVM *N;
#define LUA_OK 0
NOVALIB_API int luaL_loadfile(NovaVM *N);

// [AFTER]
NovaVM *N;
#define NOVA_OK 0
NOVALIB_API int novaL_loadfile(NovaVM *N);
```

### Violation 5: goto for Control Flow
```c
// [BEFORE]
void bad_flow(int x) {
        if (x > 10) goto middle;
        printf("start\n");
middle:
        printf("middle\n");
}

// [AFTER]
void good_flow(int x) {
        if (x <= 10) {
                printf("start\n");
        }
        printf("middle\n");
}
```

---

## 10.3 Migration Checklist

Before claiming ZORYA-C compliance:

- [ ] All files have Zorya Corporation headers (ZORYA-C-400)
- [ ] No Lua naming in codebase (ZORYA-C-203)
- [ ] All function-like macros removed or have PCM headers (ZORYA-C-121)
- [ ] All pointers checked before dereference (ZORYA-C-002)
- [ ] All switch statements have default case (ZORYA-C-003)
- [ ] All unused parameters annotated (ZORYA-C-004)
- [ ] goto only used for cleanup (ZORYA-C-150)
- [ ] clang-tidy passes with zero warnings
- [ ] cppcheck passes with zero errors
- [ ] ASan + UBSan runtime passes
- [ ] All public functions documented (ZORYA-C-401)
- [ ] No untracked TODOs (ZORYA-C-402)

---

# APPENDIX A: RULE QUICK REFERENCE

| Rule | Category | Description | Severity |
|------|----------|-------------|----------|
| ZORYA-C-001 | Safety | No implicit function declarations | CRITICAL |
| ZORYA-C-002 | Safety | Null pointer checks before dereference | CRITICAL |
| ZORYA-C-003 | Safety | Switch statements have default case | CRITICAL |
| ZORYA-C-004 | Safety | Explicit unused parameter annotation | WARNING |
| ZORYA-C-005 | Safety | No implicit type conversions | WARNING |
| ZORYA-C-006 | Safety | No use after free | CRITICAL |
| ZORYA-C-007 | Safety | All memory allocations checked | CRITICAL |
| ZORYA-C-008 | Safety | No array index out of bounds | CRITICAL |
| ZORYA-C-009 | Safety | No signed integer overflow | CRITICAL |
| ZORYA-C-010 | Safety | No uninitialized variables | CRITICAL |
| ZORYA-C-120 | Performance | Macro naming (verbose only) | WARNING |
| ZORYA-C-121 | Performance | Performance-critical macros (PCM) | WARNING |
| ZORYA-C-122 | Performance | Calculated constants only | WARNING |
| ZORYA-C-150 | Performance | goto for cleanup only | WARNING |
| ZORYA-C-151 | Performance | Direct field access in hot paths | INFO |
| ZORYA-C-152 | Performance | Aggressive inline functions | INFO |
| ZORYA-C-200 | Runtime | State pointer named 'L' | INFO |
| ZORYA-C-201 | Runtime | Engine init returns status code | WARNING |
| ZORYA-C-202 | Runtime | Hot path functions documented | INFO |
| ZORYA-C-203 | Runtime | Consistent project naming | CRITICAL |
| ZORYA-C-204 | Runtime | Single type definition location | CRITICAL |
| ZORYA-C-205 | Runtime | Platform detection runtime not compile-time | INFO |
| ZORYA-C-300 | Platform | Boolean macros for platform detection | INFO |
| ZORYA-C-301 | Platform | Platform-agnostic CPUID wrapper | WARNING |
| ZORYA-C-302 | Platform | No hardcoded endianness | WARNING |
| ZORYA-C-400 | Documentation | File header required | WARNING |
| ZORYA-C-401 | Documentation | Function documentation | INFO |
| ZORYA-C-402 | Documentation | No TODO without tracking | WARNING |

---

# APPENDIX B: ZORYA-C vs MISRA-C Summary

| Aspect | MISRA-C | ZORYA-C |
|--------|---------|---------|
| **Certification** | Required for safety-critical | Not required (intentional) |
| **goto statements** | Restricted | Allowed for cleanup |
| **Function-like macros** | Forbidden | Allowed if documented (PCM) |
| **Direct struct access** | Discouraged | Preferred in hot paths |
| **Inline functions** | Limited | Encouraged aggressively |
| **Calculated macros** | Forbidden | Allowed if parenthesized |
| **Runtime detection** | Not specified | Required for portability |
| **Lua naming** | Not applicable | Forbidden |
| **Hot path optimization** | Not specified | Explicit rules |
| **Exception process** | Rigid | Documented and reviewable |

---

**END OF ZORYA-C STANDARD v1.0.0**

---

## Version History

| Version | Date | Changes | Author |
|---------|------|---------|--------|
| 2.0.0 | 2026-03-11 | Project-agnostic update, PCM date format, state pointer flexibility, streamlined file headers | Anthony Taliento |
| 1.0.0 | 2025-11-05 | Initial ZORYA-C Standard | Anthony Taliento |

---

## Acknowledgments

ZORYA-C is inspired by:
- **MISRA-C** (Motor Industry Software Reliability Association)
- **Linux Kernel Coding Style**
- **SQLite Code Style**


We honor their contributions to C code quality while adapting for language runtime development.

---

**ZORYA CORPORATION - Engineering Excellence Without Compromise**

