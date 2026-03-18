<!-- ============================================================
     NINI FORMAT SPECIFICATION v1.0.0
     Nova INI — The Native Tongue of Nova

     Author:  Anthony Taliento (Zorya Corporation)
     Date:    2026-02-18
     Status:  RATIFIED
     ============================================================ -->

# NINI Format Specification v1.0.0

**Nova INI (NINI)** is Nova's native structured data format. It takes the
simplicity of INI and levels it up with typed values, arrays, variable
interpolation, nested sections, multi-line strings, and first-class task
definitions — making it the universal config, data interchange, and task
runner format for the Nova ecosystem.

**File extension:** `.nini`
**MIME type:** `text/x-nini`
**Encoding:** UTF-8

---

## Table of Contents

1. [Design Goals](#1-design-goals)
2. [Comments](#2-comments)
3. [Key-Value Pairs](#3-key-value-pairs)
4. [Sections](#4-sections)
5. [Typed Values](#5-typed-values)
6. [Arrays](#6-arrays)
7. [Variable Interpolation](#7-variable-interpolation)
8. [Multi-Line Strings](#8-multi-line-strings)
9. [Include Directive](#9-include-directive)
10. [Task Definitions](#10-task-definitions)
11. [Reserved Prefixes](#11-reserved-prefixes)
12. [Format Detection](#12-format-detection)
13. [Conversion Rules](#13-conversion-rules)
14. [Grammar (EBNF)](#14-grammar-ebnf)
15. [Examples](#15-examples)

---

## 1. Design Goals

| Goal | Rationale |
|------|-----------|
| **Human-readable** | Should be writable by hand without looking up syntax |
| **Hyper-parseable** | Single-pass, line-at-a-time parsing with zero backtracking |
| **Typed by default** | Numbers, booleans, and nil are auto-detected (no everything-is-a-string) |
| **Nova-native** | Decodes directly into Nova tables with zero transformation |
| **Data interchange** | CSV, JSON, INI, TOML all normalize cleanly into NINI |
| **Task runner** | `[task:name]` sections define executable tasks with deps and env |
| **Composable** | `@include` merges other NINI files for modularity |

---

## 2. Comments

Lines starting with `#` or `;` are comments. Inline comments start with
`#` or `;` after whitespace, provided they are not inside a quoted string.

```nini
# This is a comment
; So is this

name = Nova    # inline comment
port = 8080    ; also works
```

---

## 3. Key-Value Pairs

The fundamental unit. Keys are unquoted identifiers. Values are everything
after the `=` sign, trimmed of surrounding whitespace.

```nini
key = value
host = localhost
port = 8080
```

**Key rules:**
- Keys are case-sensitive
- Keys may contain `a-z A-Z 0-9 _ - .`
- Keys MUST NOT contain `=`, `[`, `]`, `#`, `;`
- Duplicate keys within a section: last value wins (no error)

**Separator:** Only `=` is used (unlike INI which also allows `:`).
This avoids ambiguity with `task:name` section syntax.

---

## 4. Sections

Sections group keys into named tables. Section names are enclosed in
square brackets. Bare keys (before any section) belong to the root table.

```nini
# Root-level key
name = MyProject

[database]
host = localhost
port = 5432

[server]
host = 0.0.0.0
port = 3000
```

**Produces the Nova table:**
```lua
{
    name = "MyProject",
    database = { host = "localhost", port = 5432 },
    server   = { host = "0.0.0.0",  port = 3000 }
}
```

### 4.1 Nested Sections (Dot Notation)

Dots in section names create nested tables:

```nini
[database.pool]
min = 5
max = 20

[database.credentials]
user = admin
pass = secret
```

**Produces:**
```lua
{
    database = {
        pool = { min = 5, max = 20 },
        credentials = { user = "admin", pass = "secret" }
    }
}
```

---

## 5. Typed Values

Values are **auto-typed** by default (NINI's key enhancement over INI):

| Input | Nova Type | Value |
|-------|-----------|-------|
| `42` | integer | `42` |
| `-3.14` | number | `-3.14` |
| `true` / `yes` / `on` | boolean | `true` |
| `false` / `no` / `off` | boolean | `false` |
| `nil` / `null` / `none` | nil | `nil` |
| `"hello"` | string | `"hello"` (quotes stripped) |
| `hello` | string | `"hello"` (unquoted string) |

**Boolean and nil keywords are case-insensitive.**

To force a value to be a string (e.g., `"true"` as literal text), wrap it
in quotes:

```nini
is_active = true          # boolean true
label = "true"            # string "true"
count = 42                # integer 42
count_str = "42"          # string "42"
```

---

## 6. Arrays

NINI supports arrays via **repeated keys with `[]` suffix**:

```nini
[paths]
include[] = /usr/local/include
include[] = /opt/nova/include
include[] = ./include
```

**Produces:** `{ paths = { include = {"/usr/local/include", "/opt/nova/include", "./include"} } }`

### 6.1 Inline Arrays

For short arrays, use bracket syntax on a single line:

```nini
tags = [web, api, production]
ports = [8080, 8081, 8082]
flags = [true, false, true]
```

Values inside inline arrays follow the same auto-typing rules. Items are
separated by commas. Whitespace around items is trimmed.

---

## 7. Variable Interpolation

Reference other values with `${section.key}` or `${key}` (for root-level):

```nini
[database]
host = localhost
port = 5432
name = mydb

[deploy]
connection = ${database.host}:${database.port}/${database.name}
```

**Resolution rules:**
- `${key}` resolves from root table
- `${section.key}` resolves from named section
- `${section.sub.key}` follows dot chain
- Unresolved references remain as literal text `${...}`
- Interpolation happens at decode time, in file order

---

## 8. Multi-Line Strings

Triple-quoted strings span multiple lines:

```nini
[templates]
greeting = """
Hello, World!
Welcome to Nova.
This is a multi-line string.
"""
```

**Rules:**
- Opening `"""` must be on the key line (content starts on next line)
- Closing `"""` must be on its own line
- Leading/trailing newlines adjacent to `"""` are stripped
- Internal whitespace and newlines are preserved

---

## 9. Include Directive

Merge another NINI file into the current document:

```nini
@include defaults.nini
@include secrets.nini

[server]
port = ${defaults.port}
```

**Rules:**
- `@include` is a preprocessor directive, processed before parsing
- Included file's sections merge into the document (last value wins)
- Paths are relative to the including file
- Circular includes are detected and produce an error
- Maximum include depth: 16

---

## 10. Task Definitions

The killer feature. `[task:name]` sections define executable tasks for the
Nova task runner (`nova run <task>`):

```nini
[task:build]
description = Build the Nova VM
command = make clean && make
env.CC = gcc
env.CFLAGS = -O2 -Wall
dir = ./src

[task:test]
description = Run test suite
command = make test
depends = build

[task:deploy]
description = Deploy to production
command = ./scripts/deploy.sh ${server.host}
depends = [build, test]
env.NODE_ENV = production
```

### 10.1 Task Properties

| Key | Type | Required | Description |
|-----|------|----------|-------------|
| `command` | string | **YES** | Shell command to execute |
| `description` | string | no | Human-readable description |
| `depends` | string or array | no | Tasks to run first |
| `dir` | string | no | Working directory (default: file's directory) |
| `env.*` | string | no | Environment variables (`env.KEY = VALUE`) |
| `silent` | boolean | no | Suppress command echo (default: false) |
| `ignore_error` | boolean | no | Continue on non-zero exit (default: false) |
| `platforms` | array | no | Run only on listed platforms (`[linux, macos]`) |

### 10.2 Task Execution

```bash
nova run build              # Run the "build" task
nova run test               # Runs "build" first (dependency), then "test"
nova run deploy             # Runs build → test → deploy
nova run --list             # List all tasks with descriptions
nova run --dry              # Show what would execute without running
```

### 10.3 Dependency Resolution

- Dependencies form a DAG (Directed Acyclic Graph)
- Circular dependencies produce a clear error
- Each task runs at most once per invocation
- Execution order: depth-first, left-to-right

---

## 11. Reserved Prefixes

| Prefix | Meaning |
|--------|---------|
| `@include` | File inclusion directive |
| `@version` | NINI format version hint |
| `task:` | Task definition section |
| `env.*` | Environment variable in task context |

---

## 12. Format Detection

NINI files are detected by:
1. File extension: `.nini`
2. First line: `# nini` or `; nini` or `@version nini/1`
3. Presence of `[task:` section headers
4. Presence of `@include` directives
5. Presence of `[]` array syntax

In `data.detect()`, NINI is distinguished from plain INI by checking for
these NINI-specific features.

---

## 13. Conversion Rules

NINI serves as Nova's data normalization format. Conversion from other
formats follows these rules:

### CSV → NINI
```
# Source: users.csv
# name,age,email
# Alice,30,alice@example.com
# Bob,25,bob@example.com

@version nini/1

[row.0]
name = Alice
age = 30
email = alice@example.com

[row.1]
name = Bob
age = 25
email = bob@example.com

[meta]
columns = [name, age, email]
row_count = 2
source = users.csv
```

### JSON → NINI
Nested JSON objects map to dotted sections. Arrays use `[]` syntax.
Primitive values are expressed as typed values.

### INI → NINI
Direct superset — all valid INI is valid NINI. The only change is that
NINI uses `=` exclusively (not `:`), auto-types values, and adds
arrays, interpolation, and tasks.

---

## 14. Grammar (EBNF)

```ebnf
document     = { directive | section | keyvalue | comment | blank } ;
directive    = "@include" ws path newline
             | "@version" ws version newline ;
section      = "[" section_name "]" [ comment ] newline ;
section_name = ident { "." ident }
             | "task:" ident ;
keyvalue     = key ws "=" ws value [ comment ] newline ;
key          = ident [ "[]" ]
             | ident "." ident ;
value        = multiline | inline_array | interpolated_string ;
multiline    = '"""' newline { any_char } newline '"""' ;
inline_array = "[" value_list "]" ;
value_list   = typed_value { "," typed_value } ;
typed_value  = integer | float | boolean | nil_kw | string ;
interpolated_string = { char | "${" ref "}" } ;
ref          = ident { "." ident } ;
ident        = ( letter | "_" ) { letter | digit | "_" | "-" } ;
comment      = ( "#" | ";" ) { any_char } ;
blank        = { ws } newline ;
ws           = " " | "\t" ;
newline      = "\n" | "\r\n" ;
```

---

## 15. Examples

### 15.1 Application Config

```nini
# app.nini — Web application configuration
@version nini/1

name = MyApp
version = 2.1.0
debug = false

[server]
host = 0.0.0.0
port = 8080
workers = 4

[database]
host = localhost
port = 5432
name = myapp_prod
pool_min = 5
pool_max = 20

[logging]
level = info
file = /var/log/myapp.log
formats = [json, text]
```

### 15.2 Build Taskfile

```nini
# taskfile.nini — Project build tasks

[project]
name = Nova VM
version = 0.2.0
author = Zorya Corporation

[task:clean]
description = Remove build artifacts
command = rm -rf build/ bin/

[task:build]
description = Build Nova VM (release)
command = make -j$(nproc)
env.CC = gcc
env.CFLAGS = -O2 -Wall -Wextra -Werror -pedantic

[task:debug]
description = Build with debug symbols and sanitizers
command = make DEBUG=1
depends = clean
env.CC = gcc

[task:test]
description = Run full test suite
command = make test
depends = build

[task:bench]
description = Run performance benchmarks
command = ./bin/nova tests/bench_extended.n
depends = build

[task:release]
description = Full release pipeline
command = echo "Release complete!"
depends = [clean, build, test, bench]

[task:install]
description = Install to system
command = sudo make install
depends = build
```

### 15.3 Data Normalization

```nini
# Normalized from CSV sensor data
@version nini/1

[meta]
source = sensors.csv
columns = [timestamp, sensor_id, temperature, humidity]
row_count = 3

[row.0]
timestamp = 2026-02-18T10:00:00
sensor_id = S001
temperature = 22.5
humidity = 45

[row.1]
timestamp = 2026-02-18T10:05:00
sensor_id = S002
temperature = 23.1
humidity = 42

[row.2]
timestamp = 2026-02-18T10:10:00
sensor_id = S001
temperature = 22.8
humidity = 44
```

---

## Appendix A: Comparison

| Feature | INI | TOML | NINI |
|---------|-----|------|------|
| Sections | ✓ | ✓ | ✓ |
| Nested sections | ✗ | ✓ | ✓ (dot notation) |
| Auto-typed values | ✗ | ✓ | ✓ |
| Arrays | ✗ | ✓ | ✓ ([] and inline) |
| Variable interpolation | ✗ | ✗ | ✓ |
| Multi-line strings | ✗ | ✓ | ✓ |
| Includes | ✗ | ✗ | ✓ |
| Task definitions | ✗ | ✗ | ✓ |
| File extension | .ini | .toml | .nini |
| Complexity | Low | Medium | Low-Medium |

---

**ZORYA CORPORATION — Engineering Excellence, Democratized**
