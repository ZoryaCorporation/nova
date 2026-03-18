<!-- ============================================================
     NOVA — README
     A fast, freestanding scripting language built for real work.

     Author:  Anthony Taliento (Zorya Corporation)
     Version: 0.2.0
     ============================================================ -->

# Nova

**A fast, freestanding scripting language built for real work.**

Nova is a register-based bytecode virtual machine and scripting language
written from scratch in C99. Where most scripting languages leave the hard
problems to third-party packages — data formats, async I/O, text analysis,
build tooling — Nova ships them as first-class capabilities in a single
binary. Multi-format data processing, async/await, NLP primitives,
Unix-style CLI tools, a native config format, a task runner, HTTP, SQLite,
and a rich standard library are all built in, with zero mandatory external
dependencies.

Not a fork. Not a wrapper. Built from the ground up.

```lua
#define VERSION "0.2.0"

dec colors = { "crimson", "gold", "indigo" }

for i = 0, #colors - 1 do
    printf("  %d: %s\n", i, colors[i])
end

echo `Nova v${VERSION} — ${#colors} colors loaded.`
```

---

## Quick Start

```bash
git clone https://github.com/zorya-corporation/nova.git
cd nova && make

./bin/nova hello.n            # run a script
./bin/nova -c hello.n         # compile to bytecode → hello.no
./bin/nova hello.no           # run compiled bytecode

./bin/nova                    # interactive tool shell
make test                     # 32 suites, 1300+ assertions
```

---

## What Nova Does

| Capability | Description | Docs |
|------------|-------------|------|
| **Multi-format data** | Parse and emit JSON, CSV/TSV, TOML, YAML, INI, NINI, HTML natively | [NDP Spec →](docs/NDP_SPEC.md) |
| **Async / await** | First-class `async function`, `await`, `spawn` — coroutine-backed, no callbacks | [Async Guide →](nova_syntax/05_advanced/async.n) |
| **Built-in CLI tools** | `cat`, `grep`, `find`, `ls`, `tree`, `wc`, `head`, `tail` — in-process, zero subprocess overhead | [CLI Tools →](docs/CLI_TOOLS.md) |
| **NINI + task runner** | Native config format with interpolation, typed values, and `nova task` | [NINI Spec →](docs/NINI_SPEC.md) |
| **NLP primitives** | Tokenize, stem, TF-IDF, n-grams, fuzzy match — built-in, no external model | [Features →](FEATURES.md) |
| **SQLite** | Full SQL via `#import sql` — vendored, no install required | [Features →](FEATURES.md) |
| **HTTP client** | `net.get`, `post`, `request` via `#import net` (libcurl, optional) | [Features →](FEATURES.md) |
| **Rich filesystem** | 34-function `fs` module — stat, glob, walk, join, chmod, touch and more | [Features →](FEATURES.md) |
| **Coroutines** | Stackful symmetric coroutines, cooperative scheduling | [Language Guide →](nova_syntax/NOVA_GUIDE.md) |
| **Metatables** | Full operator overloading, prototype OOP, string method dispatch | [Language Guide →](nova_syntax/NOVA_GUIDE.md) |
| **Preprocessor** | `#define`, `#ifdef`, `#include`, `#import` — resolved before compilation | [Language Guide →](nova_syntax/NOVA_GUIDE.md) |
| **Bytecode** | Portable `.no` files, register-based 32-bit instructions | [Features →](FEATURES.md) |
| **Embedding** | Clean C API, `libnova.a` static library | [Features →](FEATURES.md) |
| **Zero deps** | SQLite vendored, libcurl optional — core builds with only `-lm` | — |

---

## The Language

Nova's syntax will feel familiar if you've used any scripting language in the
Lua family, but it makes deliberate departures where clarity and modern
convenience win out — 0-indexed arrays, string interpolation, a C-style
print family, and a full preprocessor. The sections below introduce the core
concepts; the [Language Guide](nova_syntax/NOVA_GUIDE.md) covers everything
in depth.

### Variables and Types

Nova is dynamically typed with seven value types: `nil`, `boolean`, `integer`,
`number` (float), `string`, `table`, and `function`.

```lua
dec name   = "Nova"                      -- string
dec age    = 2                            -- integer
dec pi     = 3.14159                      -- number (float)
dec active = true                         -- boolean
dec empty  = nil                          -- nil
dec items  = {1, 2, 3}                   -- table (0-indexed array)
dec player = {name="Zorya", hp=100}      -- table (dictionary)
```

### Tables

Tables are Nova's single compound type — arrays and dictionaries in one
structure. Arrays are **0-indexed**.

```lua
dec fruits = {"apple", "banana", "cherry"}
echo fruits[0]           -- apple
echo #fruits             -- 3

for i = 0, #fruits - 1 do
    printf("  %d: %s\n", i, fruits[i])
end

dec config = {host = "localhost", port = 8080}
echo config.host         -- localhost
```

### Strings

Strings support backtick interpolation, colon method syntax, and the full
C-style print family.

```lua
dec lang = "Nova"
dec ver  = 2

-- Interpolation with expressions
echo `${lang} version ${ver}`              -- Nova version 2
echo `${lang:upper()} v${ver * 100}`       -- NOVA v200

-- Colon method syntax (via string metatable)
dec s = "hello world"
echo s:upper()                             -- HELLO WORLD
echo s:find("world")                       -- 6
echo s:sub(0, 5)                           -- hello

-- The print family
echo("status update")                      -- plain output (preferred)
printf("%-12s %8.2f\n", "Total:", 9.99)    -- formatted to stdout
dec msg = sprintf("[%04d] %s", 42, "ok") -- formatted to string
fprintf(file, "log: %s\n", msg)            -- formatted to file
```

### Functions

```lua
function greet(name)
    echo `Hello, ${name}!`
end

-- Multiple returns
function divide(a, b)
    if b == 0 then return nil, "division by zero" end
    return a / b, a % b
end

dec q, r = divide(17, 5)    -- q=3, r=2

-- Closures
function make_counter()
    dec n = 0
    return function()
        n = n + 1
        return n
    end
end
```

### Metatables and OOP

Metatables provide operator overloading, prototype-based inheritance, and
method dispatch.

```lua
dec Vec2 = {}
Vec2.__index = Vec2

function Vec2.new(x, y)
    return setmetatable({x = x, y = y}, Vec2)
end

function Vec2.__add(a, b)
    return Vec2.new(a.x + b.x, a.y + b.y)
end

function Vec2:length()
    return math.sqrt(self.x * self.x + self.y * self.y)
end

dec v = Vec2.new(3, 4)
echo v:length()          -- 5.0

dec w = v + Vec2.new(1, 1)
echo `(${w.x}, ${w.y})`  -- (4, 5)
```

### Coroutines

Stackful symmetric coroutines for cooperative multitasking.

```lua
dec function counter(max)
    for i = 0, max do
        coroutine.yield(i)
    end
end

dec co = coroutine.create(counter)
while true do
    dec ok, val = coroutine.resume(co, 5)
    if not ok or coroutine.status(co) == "dead" then break end
    echo val
end
```

### Error Handling

```lua
dec ok, err = pcall(function()
    error("something broke")
end)
if not ok then echo `Caught: ${err}` end

-- xpcall with custom handler
dec ok, msg = xpcall(function()
    error("oops")
end, function(e) return "HANDLED: " .. e end)
```

### Preprocessor

A full token-based preprocessor runs before compilation — `#define` with
function-like macros, conditional compilation, textual includes, and module
imports.

```lua
#define MAX_HP  100
#define SQUARE(x) ((x) * (x))

#ifdef DEBUG
    echo "Debug mode"
#endif

#include "helpers.m"   -- textual inclusion
#import  json           -- load stdlib module
```

→ [Full language guide](nova_syntax/NOVA_GUIDE.md) &nbsp;|&nbsp; [Examples and tutorials](nova_syntax/)

---

## Standard Library

Nova's standard library is designed to make common tasks trivial and uncommon
tasks possible — without reaching for external packages. Core modules are
available the moment the VM starts; extension modules load on demand with a
single `#import` directive.

### Core Modules

| Module | Key Functions |
|--------|--------------|
| `math` | `abs`, `ceil`, `floor`, `sqrt`, `sin`, `cos`, `tan`, `log`, `exp`, `random`, `pi`, `huge` |
| `string` | `len`, `sub`, `upper`, `lower`, `find`, `gsub`, `match`, `gmatch`, `format`, `rep`, `reverse`, `byte`, `char` |
| `table` | `insert`, `remove`, `sort`, `concat`, `move`, `pack`, `unpack` |
| `io` | `open`, `close`, `read`, `write`, `lines` |
| `os` | `execute`, `capture`, `getenv`, `setenv`, `clock`, `time`, `date`, `cwd`, `chdir`, `sleep`, `platform` |
| `fs` | `read`, `write`, `exists`, `isfile`, `isdir`, `list`, `walk`, `find`, `glob`, `mkdir`, `copy`, `move`, `stat` |
| `coroutine` | `create`, `resume`, `yield`, `wrap`, `status` |
| `async` | `run`, `spawn`, `sleep`, `status`, `wrap` |
| `debug` | `traceback`, `getinfo`, `getlocal`, `sethook` |
| `tools` | `cat`, `ls`, `tree`, `find`, `grep`, `head`, `tail`, `wc`, `pwd`, `run` |

### Extension Modules

Load with `#import`. Each data module provides `encode`, `decode`, `load`, and
`save` for its format.

| Module | Import | Description |
|--------|--------|-------------|
| `json` | `#import json` | JSON codec |
| `csv` | `#import csv` | CSV with headers and custom delimiters |
| `toml` | `#import toml` | TOML codec |
| `yaml` | `#import yaml` | YAML codec |
| `ini` | `#import ini` | INI file parsing |
| `html` | `#import html` | HTML entity encoding and decoding |
| `nini` | `#import nini` | Nova's native config format |
| `net` | `#import net` | HTTP client — `get`, `post`, `put`, `delete`, `request` (libcurl) |
| `sql` | `#import sql` | SQLite3 — `open`, `exec`, `query`, `close` (vendored) |
| `nlp` | built-in | `tokenize`, `stem`, `fuzzy`, `freq`, `tfidf`, `ngrams` |

→ [Features reference](FEATURES.md)

---

## Data Processing

One of Nova's defining features is its built-in NDP (Nova Data Processor) — a
unified multi-format codec that handles eight structured data formats out of
the box. Every codec speaks the same language: standard Nova tables in,
standard Nova tables out. This means data flows freely between JSON, CSV,
TOML, YAML, and the rest without conversion glue, adapter libraries, or
format-specific quirks.

```lua
#import json
#import csv
#import toml

-- JSON
dec data = json.decode('{"name":"Nova","version":2}')
printf("name: %s\n", data.name)

-- CSV → JSON
dec rows = csv.decode("name,age\nAlice,30\nBob,25")
echo json.encode(rows[0])          -- {"name":"Alice","age":"30"}

-- TOML
dec config = toml.decode([[
[database]
host = "localhost"
port = 5432
]])
printf("db: %s:%d\n", config.database.host, config.database.port)

-- Cross-format: load YAML, save as JSON
#import yaml
dec parsed = yaml.load("config.yaml")
json.save("config.json", parsed)
```

→ [NDP Specification](docs/NDP_SPEC.md)

---

## Async / Await

`async` and `await` are first-class keywords, not library functions. Async
functions are coroutines under the hood — no event loop boilerplate, no
callback nesting, no promises API to learn.

```lua
async function fetch_record(id)
    dec raw  = await get_from_db(id)
    dec rich = await enrich(raw)
    return rich
end

async function main()
    -- spawn fires without blocking
    dec _ = spawn fetch_record(1)
    dec _ = spawn fetch_record(2)

    -- await blocks until resolved
    dec result = await fetch_record(3)
    printf("record: %d\n", result.id)
end

task.run(main)
```

→ [Async / await guide](nova_syntax/05_advanced/async.n)

---

## Built-In CLI Tools

Nova ships with a suite of Unix-style tools that run in-process inside the
VM. There is no subprocess spawning, no shell dependency, and no need to parse
stdout — every tool returns structured Nova tables and strings directly.
The same tools are available both as `nova <tool>` subcommands from your
terminal and as the `tools` module inside any script, with zero overhead
either way.

```bash
# At the command line
nova cat   README.md
nova grep  "TODO" src/
nova find  . -m=*.n
nova ls    src/
nova tree  src/ --depth=2
nova wc    Makefile
nova head  LICENSE
nova tail  build.log
nova task  build
```

```lua
-- Inside a Nova script — same tools, zero subprocess overhead
dec src     = tools.cat("src/nova_vm.c")
dec entries = tools.ls("src/")           -- {name, type, size}
dec matches = tools.grep("TODO", "*.c")  -- {file, num, text}
dec files   = tools.find(".", "*.n")     -- list of paths
dec counts  = tools.wc("Makefile")       -- {lines, words, chars}
dec out     = tools.run("make --dry-run") -- capture stdout
```

→ [CLI Tools reference](docs/CLI_TOOLS.md)

---

## NINI — Config Format and Task Runner

NINI (Nova INI, `.ni`) is Nova's native configuration format — the project's
lingua franca for config, data interchange, and build automation. It takes
the simplicity of INI and extends it with auto-typed values, variable
interpolation, inline arrays, and task sections. Where JSON is noisy and YAML
is fragile, NINI is readable, parseable, and unambiguous.

```ini
# project.ni
[project]
name    = nova
version = 0.2.0
debug   = false

[paths]
base = /opt/${project.name}
bin  = ${paths.base}/bin

[build]
flags = [-Wall, -Wextra, -Werror]

[task:build]
command = make
env.CC  = gcc

[task:test]
command = make test
depends = [build]
```

```lua
#import nini
dec conf = nini.load("project.ni")
printf("building %s %s\n", conf.project.name, conf.project.version)
printf("output:  %s\n", conf.paths.bin)
```

```bash
# Task runner
nova task              # list all tasks
nova task build        # run a task
nova task clean build  # run multiple (deps resolved automatically)
```

→ [NINI specification](docs/NINI_SPEC.md)

---

## NLP

Text analysis is a first-class citizen in Nova. The built-in `nlp` module
provides tokenization, stemming, frequency analysis, TF-IDF scoring, n-gram
extraction, and fuzzy matching — all implemented in C inside the VM, with
no model downloads, no external services, and no imports needed.

```lua
dec text = "The quick brown fox jumps over the lazy dog"

dec tokens = nlp.tokenize(text)        -- word array
dec stems  = {}
for i = 0, #tokens - 1 do
    stems[i] = nlp.stem(tokens[i])
end

dec freq = nlp.freq(tokens)            -- word frequency table
dec top  = nlp.tfidf(tokens, corpus)   -- TF-IDF scores
dec bi   = nlp.ngrams(tokens, 2)       -- bigrams

-- Fuzzy matching
dec score = nlp.fuzzy("colour", "color")
```

---

## Network and Database

```lua
#import net
dec body = net.get("https://api.example.com/data")
dec resp = net.post("https://api.example.com/submit", json.encode(payload))

#import sql
dec db = sql.open("app.db")
sql.exec(db, "CREATE TABLE IF NOT EXISTS users (id INTEGER, name TEXT)")
dec rows = sql.query(db, "SELECT * FROM users")
sql.close(db)
```

`net` requires libcurl (build with `NOVA_NO_NET=1` to omit). `sql` uses
vendored SQLite3 — always available.

---

## Architecture

Nova's implementation is built on a small number of carefully chosen
primitives, each designed to minimize overhead and maximize throughput.

```
Source (.n/.m) → Lexer → Preprocessor → Parser → AST → Compiler → Bytecode (.no) → VM
```

| Component | Implementation |
|-----------|---------------|
| **Values** | NaN-boxed 64-bit doubles — integers, objects, booleans packed via tag bits |
| **VM** | Register-based, 32-bit instructions, computed-goto dispatch |
| **Hash tables** | DAGGER — open-addressing with NXH64 hash, O(1) average |
| **Strings** | Weave intern pool — all strings interned, O(1) equality |
| **GC** | Tri-color incremental mark-and-sweep |
| **Closures** | Upvalue migration — stack to heap on close |
| **Metatables** | Per-type operator overloading, full prototype chains |

### Value Representation

Every value in Nova occupies exactly 8 bytes. The VM uses NaN-boxing — a
technique that exploits unused bit patterns in IEEE 754 quiet NaNs to encode
object pointers, integers, booleans, and nil alongside ordinary floating-point
numbers. No value is ever heap-allocated just to exist; the VM can determine a
value's type with a single mask-and-compare instruction.

### Execution Engine

The VM executes a register-based instruction set encoded as 32-bit words.
Register machines avoid the push/pop traffic of a stack architecture and tend
to produce shorter instruction sequences for the same program. Dispatch uses
GCC/Clang computed gotos (`&&label`), which give the branch predictor a
per-opcode history rather than funneling every instruction through one central
`switch`.

### Strings and Hash Tables

All strings pass through the Weave intern pool on creation. Once interned, a
string is unique in the system — equality is a pointer comparison, and hashing
is done once at intern time. DAGGER hash tables pair this with NXH64, a custom
64-bit hash function, using open addressing with linear probing for
cache-friendly O(1) lookups across globals, module tables, and metatable
chains.

### Memory Management

Nova's garbage collector uses tri-color marking (white/gray/black) with
incremental collection. Rather than stopping the world for a full mark-sweep
cycle, the GC interleaves small amounts of marking work with normal VM
execution, keeping pause times low. Each object type roots its metatable in
the VM struct, which keeps traversal tight and avoids the overhead of a
separate metatable registry.

---

## Building

Nova builds from source with a C99 compiler and `make` — nothing else. There
is no `./configure` step, no CMake, no package manager dependency. The
Makefile auto-detects your platform and compiler.

```bash
make                   # Release build (-O2)
make DEBUG=1           # Debug — ASan + UBSan + assertions
make NOVA_NO_NET=1     # Build without libcurl (disables net module)
make test              # Run full test suite
make lib               # Build libnova.a static library
make install           # Install to /usr/local
make clean             # Clean build artifacts
```

| Flag | Effect |
|------|--------|
| `DEBUG=1` | Enables `-g`, `-O0`, AddressSanitizer, UBSan, assertions |
| `NOVA_NO_NET=1` | No libcurl — zero external dependencies (just `-lm`) |
| `TRACE=1` | Instruction-level trace output |

### Platform Support

Nova's Platform Abstraction Layer (PAL) provides native support for Linux
(GCC/Clang), macOS (Clang/GCC), Windows (MSYS2/MinGW), and BSD. The Makefile
auto-detects your platform — no `./configure` step needed.

---

## Debug and Development

Nova exposes every stage of its compilation pipeline through command-line
flags, making it straightforward to inspect how source code transforms at
each step.

```bash
./bin/nova --lex   file.n    # Dump token stream
./bin/nova --parse file.n    # Parse and report errors
./bin/nova --ast   file.n    # Print full AST
./bin/nova --dis   file.n    # Disassemble bytecode
```

---

## File Extensions

| Extension | Purpose |
|-----------|---------|
| `.n` | Nova source file |
| `.m` | Macro / header file (textual inclusion via `#include`) |
| `.no` | Nova Object — compiled bytecode |
| `.ni` | NINI data file — Nova's native config format |
| `.nd` | Nova Document — document format derived from NINI |

---

## Documentation

Nova's documentation is organized into guides for learning, references for
looking things up, and specifications for the formats Nova defines.

**Guides**

| Document | Description |
|----------|-------------|
| [Language Guide](nova_syntax/NOVA_GUIDE.md) | Complete language reference with worked examples |
| [Examples and Tutorials](nova_syntax/) | Progressive walkthroughs from basics to advanced topics |

**References**

| Document | Description |
|----------|-------------|
| [Features](FEATURES.md) | Full standard library and feature inventory by version |

**Specifications**

| Document | Description |
|----------|-------------|
| [VM Specification](docs/NOVA_VM_SPEC.md) | Virtual machine architecture — NaN-boxing, GC, closures, dispatch |
| [Bytecode Specification](docs/NOVA_BYTECODE_SPEC.md) | Instruction set, opcode reference, `.no` binary format |
| [NDP Specification](docs/NDP_SPEC.md) | Nova Data Processor — codec architecture, type inference, format detection |
| [CLI Tools Specification](docs/CLI_TOOLS.md) | Tool suite — dispatch, interactive shell, task runner, glob matching |
| [NINI Spec](docs/NINI_SPEC.md) | NINI configuration format — ratified v1.0.0 |
| [Nova Doc Spec](docs/NOVA_DOC_SPEC.md) | Nova Doc (`.nd`) structured document format — v1.0.0 |
| [ZORYA-C Standard](docs/ZORYA_C_STANDARD.md) | C coding standard used throughout the codebase |

---

## Project Structure

```
nova/
├── src/                    VM and compiler (C99)
│   ├── nova_vm.c           Dispatch loop, GC, runtime
│   ├── nova_compile.c      Compiler (AST → bytecode)
│   ├── nova_lex.c          Lexer / scanner
│   ├── nova_parse.c        Parser (recursive descent + Pratt TDOP)
│   ├── nova_meta.c         Metamethod dispatch pipeline
│   ├── nova_gc.c           Tri-color mark-and-sweep GC
│   ├── nova_pp.c           Token-based preprocessor
│   ├── nova_opt.c          Bytecode optimizer
│   ├── nova_nini.c         NINI codec
│   ├── nova_ndp.c          NDP multi-format data processor
│   ├── nova_tools.c        CLI tool dispatch and flag parser
│   ├── nova_shell.c        Interactive tool shell (discovery-based)
│   ├── nova_task.c         NINI task runner
│   ├── nova_lib_*.c        Standard library modules
│   ├── tools/              Standalone tool sources (11 binaries)
│   └── zorya/              Vendored Zorya SDK
│       ├── nxh.c           NXH64 hash function
│       ├── dagger.c        DAGGER hash table
│       ├── weave.c         Weave string intern pool
│       └── sqlite3.c       Vendored SQLite
├── include/
│   ├── nova/               Nova headers (22 files)
│   └── zorya/              Zorya SDK headers (7 files)
├── tests/                  32 test suites, 1300+ assertions
├── examples/               Example programs
├── nova_syntax/            Language guide and tutorials
├── docs/                   Reference documentation
├── taskfile.nini           NINI build tasks
└── Makefile                Build system
```

---

## License

MIT License — see [LICENSE](LICENSE) for details.

Components in `src/zorya/` and `include/zorya/` are provided under the
Apache License 2.0. SQLite (`src/zorya/sqlite3.c`) is
[public domain](https://sqlite.org/copyright.html).

## Credits

- **Anthony Taliento** — Language design and implementation
- **Zorya Corporation** — Engineering infrastructure and SDK

---

*Nova is a Zorya Corporation project.*

*ZORYA CORPORATION — Engineering Excellence, Democratized*
