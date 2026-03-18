# The Nova Language Guide

*From your first `echo` to coroutine pipelines — everything you need.*

**Author:** Anthony Taliento (Zorya Corporation)
**Nova Version:** 0.1.0
**Last Updated:** February 2026

---

## Table of Contents

1. [What is Nova?](#what-is-nova)
2. [Running Nova](#running-nova)
3. [Hello, World](#hello-world)
4. [Variables & Types](#variables--types)
5. [Strings](#strings)
6. [Tables (Arrays & Dictionaries)](#tables)
7. [Functions](#functions)
8. [Control Flow](#control-flow)
9. [The Print Family](#the-print-family)
10. [Modules & Imports](#modules--imports)
11. [Error Handling](#error-handling)
12. [Metatables & OOP](#metatables--oop)
13. [Closures & Upvalues](#closures--upvalues)
14. [Coroutines](#coroutines)
15. [The Standard Library](#the-standard-library)
16. [The Preprocessor](#the-preprocessor)
17. [Writing Idiomatic Nova](#writing-idiomatic-nova)

---

## What is Nova?

Nova is a dynamically-typed scripting language with **Lua-like syntax** and a
register-based bytecode VM. It was designed at Zorya Corporation to be fast,
embeddable, and pleasant to write.

**Key differences from Lua:**

| Feature | Lua | Nova |
|---------|-----|------|
| Array indexing | 1-based | **0-based** |
| String indexing | 1-based | **0-based** |
| Print family | `print()` only | `print`, `echo`, `printf`, `sprintf`, `fprintf` |
| String interpolation | None | `` `Hello ${name}` `` (backticks) |
| Preprocessor | None | `#define`, `#import`, `#include`, `#ifdef` |
| Modules | `require()` | `require()` **+** `#import` for stdlib |
| Integer type | Lua 5.3+ only | Always available (NaN-boxed) |

---

## Running Nova

```bash
# Build Nova
make

# Run a script
./bin/nova script.n

# Interactive shell (no arguments)
./bin/nova
```

---

## Hello, World

```nova
-- hello.n
echo "Hello, World!"
```

That's it. `echo` is an alias for `print` — plain text goes to stdout with
a newline. We use `echo` for simple messages because it reads like English:
*"echo this to the screen."*

---

## Variables & Types

Nova has five primitive types and two reference types:

```nova
-- Primitives
dec nothing = nil          -- nil: the absence of a value
dec alive   = true         -- boolean: true or false
dec count   = 42           -- integer: 64-bit signed
dec ratio   = 3.14159      -- number: 64-bit double
dec name    = "Nova"       -- string: immutable, interned

-- Reference types
dec items   = {1, 2, 3}    -- table: arrays and dictionaries
dec fn      = function(x)  -- function: first-class closures
    return x * 2
end
```

### Scope

Variables declared with `dec` are lexically scoped. Without `dec`, they
become global (stored in `_G`).

```nova
dec x = 10        -- dec to this block

function example()
    dec y = 20    -- dec to this function
    z = 30          -- GLOBAL (avoid this!)
end
```

**Rule of thumb:** Always use `dec` unless you have a specific reason not to.

### Type Checking

```nova
echo type(42)          -- "integer"
echo type(3.14)        -- "number"
echo type("hello")     -- "string"
echo type(true)        -- "boolean"
echo type(nil)         -- "nil"
echo type({})          -- "table"
echo type(print)       -- "function"
```

### Conversions

```nova
dec n = tonumber("42")      -- 42 (integer)
dec f = tonumber("3.14")    -- 3.14 (number)
dec s = tostring(42)        -- "42"
dec i = tointeger(3.99)     -- 3 (truncates)
```

---

## Strings

### Creation

```nova
dec a = "double quotes"
dec b = 'single quotes'
dec c = `interpolated: ${1 + 1}`   -- "interpolated: 2"
dec d = [[
    Multi-line strings use
    double square brackets.
]]
```

### Interpolation

Backtick strings evaluate `${expression}` inline:

```nova
dec name = "Nova"
dec ver  = "0.1.0"
echo `Welcome to ${name} v${ver}!`
-- Output: Welcome to Nova v0.1.0!

echo `2 + 2 = ${2 + 2}`
-- Output: 2 + 2 = 4
```

### Common Operations

```nova
dec s = "Hello, World!"

echo #s                             -- 13 (length)
echo string.sub(s, 0, 5)           -- "Hello," (0-indexed!)
echo string.upper(s)               -- "HELLO, WORLD!"
echo string.lower(s)               -- "hello, world!"
echo string.find(s, "World")       -- 7 (0-indexed position)
echo string.rep("Na", 4)           -- "NaNaNaNa"
echo "Hello" .. " " .. "Nova"      -- "Hello Nova" (concatenation)
```

> **IMPORTANT:** `string.sub(s, 0, 3)` returns characters 0, 1, 2, 3.
> This is NOT Lua's 1-based indexing. Nova is 0-based everywhere.

---

## Tables

Tables are Nova's only compound data structure. They serve as arrays,
dictionaries, objects, records — everything.

### Arrays (0-indexed!)

```nova
dec colors = {"red", "green", "blue"}

echo colors[0]    -- "red"
echo colors[1]    -- "green"
echo colors[2]    -- "blue"
echo #colors      -- 3

-- Iterate with numeric for
for i = 0, #colors - 1 do
    printf("  [%d] %s\n", i, colors[i])
end

-- Or with ipairs (still 0-indexed)
for i, color in ipairs(colors) do
    printf("  [%d] %s\n", i, color)
end
```

> **CRITICAL:** Nova arrays start at index **0**, not 1.
> `{10, 20, 30}` gives `t[0]=10, t[1]=20, t[2]=30`.
> The length operator `#t` returns the count (3), so the last
> valid index is always `#t - 1`.

### Dictionaries

```nova
dec player = {
    name  = "Zorya",
    hp    = 100,
    level = 1,
}

echo player.name          -- "Zorya" (dot syntax)
echo player["hp"]         -- 100 (bracket syntax)

player.level = 2          -- update
player.mana = 50          -- add new field
```

### Useful Table Operations

```nova
dec t = {10, 20, 30}

table.insert(t, 40)       -- append: {10, 20, 30, 40}
table.remove(t, 0)        -- remove first: {20, 30, 40}
table.sort(t)              -- sort in place

-- Iterate key-value pairs
for k, v in pairs(player) do
    printf("  %s = %s\n", tostring(k), tostring(v))
end
```

---

## Functions

### Basic Functions

```nova
function greet(name)
    echo `Hello, ${name}!`
end

greet("World")
```

### Functions with Returns

```nova
function add(a, b)
    return a + b
end

dec sum = add(3, 4)
echo sum    -- 7
```

### Multiple Returns

```nova
function divide(a, b)
    if b == 0 then
        return nil, "division by zero"
    end
    return a / b, nil
end

dec result, err = divide(10, 3)
if err then
    echo `Error: ${err}`
else
    printf("Result: %.2f\n", result)
end
```

### Local Functions & Closures

```nova
dec function make_counter(start)
    dec count = start or 0
    return function()
        count = count + 1
        return count
    end
end

dec counter = make_counter(10)
echo counter()    -- 11
echo counter()    -- 12
echo counter()    -- 13
```

---

## Control Flow

### if / elseif / else

```nova
dec score = 85

if score >= 90 then
    echo "A"
elseif score >= 80 then
    echo "B"
elseif score >= 70 then
    echo "C"
else
    echo "F"
end
```

### Numeric for

```nova
-- for var = start, stop [, step] do
for i = 0, 4 do
    echo i    -- 0, 1, 2, 3, 4
end

-- Count by 2
for i = 0, 10, 2 do
    echo i    -- 0, 2, 4, 6, 8, 10
end

-- Count down
for i = 5, 0, -1 do
    echo i    -- 5, 4, 3, 2, 1, 0
end
```

### while / repeat

```nova
dec i = 0
while i < 5 do
    echo i
    i = i + 1
end

-- repeat-until (runs at least once)
dec x = 0
repeat
    x = x + 1
until x >= 5
```

### Generic for

```nova
dec fruits = {"apple", "banana", "cherry"}
for i, fruit in ipairs(fruits) do
    echo `${i}: ${fruit}`
end

dec config = {host = "localhost", port = 8080}
for key, val in pairs(config) do
    echo `${key} = ${val}`
end
```

### break and continue

```nova
-- break exits the loop entirely
for i = 0, 100 do
    if i > 5 then break end
    echo i
end

-- continue skips to the next iteration
for i = 0, 9 do
    if i % 2 == 0 then continue end
    echo i    -- 1, 3, 5, 7, 9
end
```

---

## The Print Family

Nova has a deliberate print hierarchy. Think of it like this:

| Function | Purpose | When to Use |
|----------|---------|-------------|
| `echo` / `print` | Plain text + newline | Messages, labels, status |
| `printf(fmt, ...)` | Formatted to stdout | Numbers, alignment, data display |
| `sprintf(fmt, ...)` | Formatted to string | Building strings for later use |
| `fprintf(file, fmt, ...)` | Formatted to file | Logging, file output |

### The Idiomatic Pattern

```nova
function analyze_data(data)
    echo "Analyzing dataset..."                  -- status message

    dec total = 0
    dec count = #data
    for i = 0, count - 1 do
        total = total + data[i]
    end
    dec average = total / count

    printf("  Items:   %d\n", count)             -- formatted results
    printf("  Total:   %.2f\n", total)
    printf("  Average: %.2f\n", average)
end
```

The idea: **`echo` announces what you're doing** (human-readable status),
and **`printf` presents the results** (formatted data). This makes function
bodies read cleanly top-to-bottom.

### printf Format Specifiers

Nova follows C-style format specifiers:

```nova
printf("%d\n", 42)              -- integer: "42"
printf("%05d\n", 42)            -- zero-padded: "00042"
printf("%.2f\n", 3.14159)      -- float: "3.14"
printf("%10.2f\n", 3.14)       -- right-aligned: "      3.14"
printf("%-10s|\n", "left")     -- left-aligned: "left      |"
printf("%s has %d items\n", "cart", 5)
printf("hex: 0x%x\n", 255)     -- "hex: 0xff"
printf("%%\n")                  -- literal percent: "%"
```

### sprintf for String Building

```nova
dec label = sprintf("[%s] %04d", "INFO", 42)
echo label    -- "[INFO] 0042"

-- Useful for building table rows, messages, etc.
dec row = sprintf("%-20s %8.2f %6d", "Widget", 9.99, 150)
```

---

## Modules & Imports

### #import (Standard Library)

The preprocessor `#import` directive loads Nova's built-in modules:

```nova
#import data    -- enables json, csv, toml, ini, html, tsv, data
```

This makes functions like `json.encode()`, `csv.parse()` available and
defines preprocessor flags like `NOVA_HAS_JSON`.

### require() (User Modules)

```nova
-- lib/utils.n
dec M = {}

function M.clamp(x, lo, hi)
    if x < lo then return lo end
    if x > hi then return hi end
    return x
end

return M
```

```nova
-- main.n
dec utils = require("lib.utils")
echo utils.clamp(150, 0, 100)    -- 100
```

---

## Error Handling

### pcall (Protected Call)

```nova
dec ok, err = pcall(function()
    error("something went wrong")
end)

if not ok then
    echo `Caught error: ${err}`
end
```

### xpcall (with Error Handler)

```nova
dec function handler(err)
    return "HANDLED: " .. err
end

dec ok, msg = xpcall(function()
    error("oops")
end, handler)

echo msg    -- "HANDLED: script.n:XX: oops"
```

### assert

```nova
dec value = tonumber("not a number")
assert(value, "expected a valid number")    -- raises error
```

---

## Metatables & OOP

Metatables let you define custom behavior for tables — operator overloading,
prototype inheritance, and object-oriented patterns.

### Basic OOP Pattern

```nova
dec Animal = {}
Animal.__index = Animal

function Animal.new(name, sound)
    dec self = setmetatable({}, Animal)
    self.name = name
    self.sound = sound
    return self
end

function Animal:speak()
    printf("%s says %s!\n", self.name, self.sound)
end

dec cat = Animal.new("Cat", "meow")
dec dog = Animal.new("Dog", "woof")

cat:speak()    -- "Cat says meow!"
dog:speak()    -- "Dog says woof!"
```

### Operator Overloading

```nova
dec Vec2 = {}
Vec2.__index = Vec2

function Vec2.new(x, y)
    return setmetatable({x = x, y = y}, Vec2)
end

function Vec2.__add(a, b)
    return Vec2.new(a.x + b.x, a.y + b.y)
end

function Vec2.__tostring(v)
    return sprintf("(%g, %g)", v.x, v.y)
end

dec a = Vec2.new(1, 2)
dec b = Vec2.new(3, 4)
dec c = a + b
echo tostring(c)    -- "(4, 6)"
```

---

## Closures & Upvalues

Functions in Nova capture variables from their enclosing scope:

```nova
function make_adder(n)
    -- 'n' is captured as an upvalue
    return function(x)
        return x + n
    end
end

dec add5 = make_adder(5)
dec add10 = make_adder(10)

echo add5(3)     -- 8
echo add10(3)    -- 13
```

Upvalues stay alive even after their creating function returns. When
the stack frame closes, Nova migrates upvalues from stack to heap
automatically.

---

## Coroutines

Coroutines provide cooperative multitasking — functions that can
pause and resume.

```nova
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
-- Output: 0, 1, 2, 3, 4, 5
```

---

## The Standard Library

Nova comes with a rich set of built-in modules:

| Module | Purpose |
|--------|---------|
| `math` | Math functions (`sin`, `cos`, `sqrt`, `random`, `pi`, ...) |
| `string` | String manipulation (`sub`, `find`, `upper`, `lower`, `rep`, ...) |
| `table` | Table operations (`insert`, `remove`, `sort`, `concat`, ...) |
| `io` | File I/O (`open`, `read`, `write`, `close`, `lines`) |
| `os` | OS interface (`clock`, `time`, `getenv`, `execute`, ...) |
| `fs` | Filesystem (`exists`, `read`, `write`, `mkdir`, `ls`, `stat`, ...) |
| `nlp` | Natural language processing (`tokenize`, `stem`, `freq`, ...) |
| `math` | Math library (`sin`, `floor`, `random`, `pi`, ...) |
| `coroutine` | Coroutine control (`create`, `resume`, `yield`, `status`) |
| `debug` | Debug library (`traceback`, `getinfo`) |
| `package` | Module system (`path`, `loaded`, `searchers`) |

### With `#import data`:

| Module | Purpose |
|--------|---------|
| `json` | JSON encode/decode |
| `csv` | CSV parsing |
| `tsv` | TSV parsing |
| `ini` | INI file parsing |
| `toml` | TOML parsing |
| `html` | HTML entity encoding |

---

## The Preprocessor

Nova has a C-like preprocessor that runs before compilation:

```nova
#define MAX_SIZE 100
#define DEBUG 1

#ifdef DEBUG
    echo "Debug mode enabled"
#endif

#import data              -- load NDP standard modules
#include "helpers.m"      -- textual inclusion of macro file
```

### #define Macros

```nova
#define PI 3.14159
#define SQUARE(x) ((x) * (x))
#define LOG(msg) printf("[LOG] %s\n", msg)

dec area = PI * SQUARE(5)
LOG("computed area")
```

---

## Writing Idiomatic Nova

### The Golden Rules

1. **0-indexed everything.** Arrays, strings, iteration — all start at 0.
2. **`dec` by default.** Only omit it for intentional globals.
3. **`echo` for humans, `printf` for data.** Keep them separate.
4. **Small functions.** Each function does one thing.
5. **Guard early.** Validate inputs at the top, return early on error.
6. **Tables are your friend.** Use them for everything — config, records, lists.

### Clean Function Template

```nova
function process_records(records)
    echo "Processing records..."           -- announce intent

    if #records == 0 then                  -- guard clause
        echo "  No records to process."
        return 0
    end

    dec processed = 0                    -- logic
    for i = 0, #records - 1 do
        dec r = records[i]
        if r.active then
            processed = processed + 1
        end
    end

    printf("  Processed: %d/%d\n",         -- formatted results
           processed, #records)
    return processed
end
```

---

*ZORYA CORPORATION — Engineering Excellence, Democratized*
