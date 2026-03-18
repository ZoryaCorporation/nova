# Nova Copilot Guide — For AI Coding Assistants

*How to write correct, idiomatic Nova code when assisting developers.*

**Purpose:** This document teaches AI coding assistants (GitHub Copilot, etc.)
the key differences between Nova and Lua so they generate correct code on the
first try.

---

## CRITICAL: Nova is NOT Lua

Nova has Lua-like syntax but **different semantics**. If you see `.n` files
and think "oh, Lua" — stop. Read this first. The biggest mistakes AI assistants
make are:

### 1. INDEXING IS 0-BASED (NOT 1-BASED)

This is the #1 source of AI-generated bugs in Nova.

```nova
-- WRONG (Lua-style 1-based)
dec items = {"a", "b", "c"}
for i = 1, #items do          -- WRONG: skips items[0], reads past end
    echo items[i]
end

-- CORRECT (Nova 0-based)
dec items = {"a", "b", "c"}
for i = 0, #items - 1 do      -- CORRECT: 0 to length-1
    echo items[i]
end
```

**Rules:**
- `{10, 20, 30}` creates `t[0]=10, t[1]=20, t[2]=30`
- `#t` returns the count (3), NOT the last index
- Valid indices are `0` to `#t - 1`
- `string.sub(s, 0, 3)` returns characters at positions 0,1,2,3
- `ipairs()` iterates from index 0, yields `0, val[0]` first
- `table.insert(t, val)` appends at `#t` (after last element)
- `table.remove(t, 0)` removes the first element

### 2. PRINT FAMILY

Nova has multiple print functions. Use the right one:

```nova
-- echo/print: plain text + newline (alias for each other)
echo "Starting process..."
print "Done!"

-- printf: C-style formatted output to stdout
printf("Found %d items in %.2f seconds\n", count, elapsed)

-- sprintf: formatted string (returns string, no output)
dec msg = sprintf("[%s] Error %d", "WARN", 404)

-- fprintf: formatted output to file handle
fprintf(io.stderr, "Error: %s\n", message)
```

**Never** use `printf` without `\n` at the end — it doesn't auto-append newlines.

### 3. STRING INTERPOLATION

Nova supports backtick template strings:

```nova
dec name = "World"
echo `Hello, ${name}!`           -- "Hello, World!"
echo `Sum: ${2 + 2}`             -- "Sum: 4"
echo `Type: ${type(42)}`         -- "Type: integer"
```

Only backtick strings support `${}`. Double/single quoted strings do not.

### 4. NO SEMICOLONS

Nova does not use semicolons. Statements are newline-separated:

```nova
-- CORRECT
dec x = 10
dec y = 20
echo x + y

-- WRONG (semicolons are a syntax error)
dec x = 10;
```

### 5. PREPROCESSOR

Nova has a C-like preprocessor. It runs before compilation:

```nova
#define MAX 100               -- constant replacement
#define SQUARE(x) ((x) * (x))  -- parameterized macro
#import data                  -- load NDP (json, csv, etc.)
#include "macros.m"           -- textual file inclusion

#ifdef DEBUG
    echo "debug mode"
#endif
```

Macro files conventionally use `.m` extension. Script files use `.n`.

---

## Idiomatic Nova Style

### Function Structure

The preferred Nova function pattern:

```nova
function process_batch(items, threshold)
    echo "Processing batch..."              -- 1. Announce intent

    if items == nil or #items == 0 then     -- 2. Guard clauses
        echo "  Nothing to process."
        return nil
    end

    dec results = {}                      -- 3. Core logic
    dec count = 0
    for i = 0, #items - 1 do
        if items[i].value >= threshold then
            results[count] = items[i]
            count = count + 1
        end
    end

    printf("  Filtered: %d/%d items\n",     -- 4. Formatted output
           count, #items)
    printf("  Threshold: %.2f\n", threshold)
    return results
end
```

**Pattern:** `echo` (status) → guards → logic → `printf` (results)

### Variable Naming

```nova
-- dec variables: snake_case
dec user_count = 0
dec max_retries = 3

-- functions: snake_case
function calculate_average(data) end
dec function load_config(path) end

-- constants: UPPER_SNAKE via #define
#define MAX_ITEMS 1000
#define DEFAULT_PORT 8080

-- module tables: PascalCase
dec HttpClient = {}
HttpClient.__index = HttpClient

-- boolean variables: is_/has_/can_ prefix
dec is_valid = true
dec has_errors = false
```

### Table Construction

```nova
-- Array (0-indexed)
dec names = {"Alice", "Bob", "Charlie"}
-- Access: names[0], names[1], names[2]
-- Length: #names == 3

-- Dictionary
dec config = {
    host = "localhost",
    port = 8080,
    debug = false,
}

-- Mixed (avoid when possible, but legal)
dec mixed = {
    "first",              -- [0] = "first"
    "second",             -- [1] = "second"
    name = "mixed",       -- hash part
}
```

### Iteration Patterns

```nova
-- Numeric array iteration (MOST COMMON)
for i = 0, #arr - 1 do
    dec item = arr[i]
    -- ...
end

-- ipairs (0-based in Nova)
for i, val in ipairs(arr) do
    -- i starts at 0
end

-- Dictionary iteration
for key, val in pairs(dict) do
    -- unordered
end

-- Counted iteration
for i = 0, 9 do
    -- 0 through 9 inclusive
end

-- Step iteration
for i = 0, 100, 10 do
    -- 0, 10, 20, ..., 100
end
```

---

## Standard Library Quick Reference

### Always Available

| Function | Signature | Returns |
|----------|-----------|---------|
| `print` / `echo` | `(...)` | nil |
| `printf` | `(fmt, ...)` | nil |
| `sprintf` | `(fmt, ...)` | string |
| `fprintf` | `(file, fmt, ...)` | nil |
| `type` | `(val)` | string |
| `tostring` | `(val)` | string |
| `tonumber` | `(val)` | number or nil |
| `tointeger` | `(val)` | integer or nil |
| `error` | `(msg)` | never returns |
| `assert` | `(val [, msg])` | val or error |
| `pcall` | `(fn, ...)` | ok, result_or_err |
| `xpcall` | `(fn, handler, ...)` | ok, result_or_err |
| `pairs` | `(table)` | iterator |
| `ipairs` | `(table)` | iterator (0-based!) |
| `select` | `(n, ...)` | selected values |
| `unpack` | `(table [, i [, j]])` | multiple values |
| `setmetatable` | `(table, mt)` | table |
| `getmetatable` | `(table)` | metatable or nil |
| `rawget` | `(table, key)` | value |
| `rawset` | `(table, key, val)` | table |
| `rawlen` | `(table)` | integer |
| `rawequal` | `(a, b)` | boolean |
| `collectgarbage` | `([opt])` | varies |

### math.*

`abs`, `ceil`, `floor`, `sqrt`, `sin`, `cos`, `tan`, `asin`, `acos`,
`atan`, `exp`, `log`, `max`, `min`, `random`, `randomseed`, `pi`,
`huge`, `maxinteger`, `mininteger`

### string.*

`sub(s, i, j)` — 0-indexed substring
`find(s, pattern)` — returns 0-based position
`upper(s)`, `lower(s)` — case conversion
`rep(s, n)` — repeat string
`byte(s, i)` — character code at 0-based position
`char(n)` — code to character
`len(s)` — string length
`format(fmt, ...)` — like sprintf
`reverse(s)` — reversed string

### table.*

`insert(t, val)` — append
`insert(t, pos, val)` — insert at pos
`remove(t, pos)` — remove at pos
`sort(t [, cmp])` — in-place sort
`concat(t [, sep])` — join to string

### io.*

`open(path, mode)`, `close(f)`, `read(f, fmt)`, `write(f, ...)`, `lines(f)`

### os.*

`clock()`, `time()`, `date()`, `getenv(name)`, `execute(cmd)`, `tmpname()`,
`rename(old, new)`, `remove(path)`, `exit(code)`

### fs.*

`exists(path)`, `read(path)`, `write(path, data)`, `append(path, data)`,
`mkdir(path)`, `rmdir(path)`, `ls(path)`, `stat(path)`, `isdir(path)`,
`isfile(path)`, `copy(src, dst)`, `move(src, dst)`, ...

### nlp.*

`tokenize(text [, opts])`, `stem(word)`, `stems(text)`,
`is_stopword(word)`, `stopwords()`, `distance(a, b)`, `similarity(a, b)`,
`fuzzy(haystack, needle [, threshold])`, `freq(text [, limit])`,
`tfidf(docs, term)`, `ngrams(text, n [, limit])`,
`kwic(text, keyword [, width])`, `sentences(text)`,
`summarize(text [, n])`, `normalize(text)`, `chartype(c)`,
`wordcount(text)`, `unique(text)`

### With `#import data`:

`json.encode(val)`, `json.decode(str)`,
`csv.parse(str)`, `csv.encode(data)`,
`ini.parse(str)`, `ini.encode(data)`,
`toml.parse(str)`, `html.encode(str)`, `html.decode(str)`

---

## Common Mistakes to Avoid

### 1. Off-by-one in loops

```nova
-- WRONG: starts at 1, misses first element
for i = 1, #arr do ... end

-- CORRECT
for i = 0, #arr - 1 do ... end
```

### 2. Using `#t` as last index

```nova
-- WRONG: #t is the COUNT, not last index
echo arr[#arr]       -- out of bounds!

-- CORRECT
echo arr[#arr - 1]   -- last element
```

### 3. Forgetting \n in printf

```nova
-- WRONG: output runs together
printf("hello")
printf("world")
-- Output: "helloworld"

-- CORRECT
printf("hello\n")
printf("world\n")
```

### 4. Using string.sub with 1-based index

```nova
-- WRONG (Lua habit)
dec first_char = string.sub(s, 1, 1)  -- this is the SECOND char!

-- CORRECT
dec first_char = string.sub(s, 0, 0)  -- first character
```

### 5. Creating arrays with explicit key 1

```nova
-- WRONG (Lua-style)
dec t = {[1] = "a", [2] = "b"}  -- gap at index 0!

-- CORRECT
dec t = {[0] = "a", [1] = "b"}  -- or just {"a", "b"}
```

### 6. Assuming require returns at index 1

```nova
-- If a module returns an array, it's 0-indexed
dec data = require("mymodule")
echo data[0]    -- first element, NOT data[1]
```

---

## Code Generation Checklist

Before outputting Nova code, verify:

- [ ] All array loops use `for i = 0, #arr - 1 do`
- [ ] All `string.sub` calls use 0-based positions
- [ ] All table literals use implicit 0-based indexing or explicit `[0] = ...`
- [ ] `printf` calls end with `\n`
- [ ] No semicolons anywhere
- [ ] `dec` on all variable declarations
- [ ] `echo`/`print` for messages, `printf` for formatted data
- [ ] `#import data` appears before using `json`, `csv`, etc.
- [ ] File extension is `.n` (not `.lua`)
- [ ] No use of Lua-specific APIs (`table.getn`, `string.gmatch`, `io.read("*a")`)

---

*ZORYA CORPORATION — Engineering Excellence, Democratized*
