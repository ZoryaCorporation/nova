<!-- ============================================================
     NOVA DATA PROCESSOR SPECIFICATION
     NDP — Unified Multi-Format Codec Architecture and Design

     Author:  Anthony Taliento (Zorya Corporation)
     Date:    2026-03-13
     Version: 0.2.0
     Status:  RATIFIED
     ============================================================ -->

# Nova Data Processor Specification

The Nova Data Processor (NDP) is a unified codec engine built into the
Nova runtime. It reads and writes eight structured data formats — JSON,
CSV, TSV, INI, TOML, YAML, HTML, and NINI — through a single consistent
API, producing standard Nova tables as the universal intermediate
representation. Any format can be decoded into a table, transformed in
Nova code, and re-encoded into any other format without conversion glue.

This document describes the internal architecture and design of NDP as
implemented in Nova v0.2.0. It covers the parser infrastructure, per-format
codec strategies, type inference rules, memory management, and the library
binding layer that exposes NDP to Nova scripts. For the NINI format itself
— Nova's native configuration language — see the companion
[NINI Specification](NINI_SPEC.md). For the virtual machine that hosts NDP,
see the [VM Specification](NOVA_VM_SPEC.md).

---

## Table of Contents

1. [Architecture Overview](#1-architecture-overview)
2. [Public API](#2-public-api)
3. [Format Detection](#3-format-detection)
4. [Parser Infrastructure](#4-parser-infrastructure)
5. [JSON Codec](#5-json-codec)
6. [CSV / TSV Codec](#6-csv--tsv-codec)
7. [INI Codec](#7-ini-codec)
8. [TOML Codec](#8-toml-codec)
9. [YAML Codec](#9-yaml-codec)
10. [HTML Codec](#10-html-codec)
11. [NINI Codec](#11-nini-codec)
12. [Type Inference](#12-type-inference)
13. [Encoding and Serialization](#13-encoding-and-serialization)
14. [File I/O Path](#14-file-io-path)
15. [Memory Management](#15-memory-management)
16. [Library Bindings](#16-library-bindings)
17. [Constants and Limits](#17-constants-and-limits)

---

## 1. Architecture Overview

NDP is implemented as a single C99 translation unit (`nova_ndp.c`, ~3,800
lines) organized into ten logical parts:

| Part | Responsibility | Approx. Lines |
|------|----------------|---------------|
| String builder | Growable output buffer (`NdpBuf`) | 50 |
| Shared helpers | Parser state, error reporting, character matching | 100 |
| Format detection | Heuristic auto-detection from content | 200 |
| JSON | RFC 8259 recursive-descent parser and encoder | 800 |
| CSV / TSV | RFC 4180 field parser with header support | 600 |
| INI | Section-based parser with type inference | 400 |
| TOML | v1.0 subset parser with arrays and tables | 600 |
| YAML | Indentation-based two-phase parser | 800 |
| HTML | Tag-soup tolerant tree builder | 700 |
| NINI bridge | Delegation to standalone `nova_nini.c` codec | 50 |
| Public dispatch | Top-level `ndp_decode` / `ndp_encode` routing | 20 |

Each format-specific codec is self-contained. A codec receives a unified
parser state and the VM stack, parses its input, and pushes a Nova table
onto the stack as the result. On the encoding side, a codec walks a table
at a given stack index and writes formatted output into an `NdpBuf`. The
public API dispatches to the appropriate codec based on the `NdpFormat`
enum value in the options struct.

The design is deliberately monolithic. All eight codecs share the same
parser helpers, error infrastructure, and string builder — no per-format
allocation strategies, no plugin registries, no dynamic dispatch tables.
This keeps the hot path simple and eliminates abstraction overhead.

### Design Principles

**Tables as the universal representation.** Every codec decodes into
Nova tables and encodes from Nova tables. JSON objects become keyed
tables, JSON arrays become integer-indexed tables, CSV rows become
arrays of keyed tables (when headers are present) or arrays of arrays
(when not), YAML mappings become tables, and so on. This means a
program can load a CSV file, add a computed column, and save the result
as JSON without ever thinking about format-specific structures.

**Type-preserving round-trips.** NDP infers types during decode —
integers stay integers, floats stay floats, booleans stay booleans —
and preserves those types during encode. A TOML config decoded and
immediately re-encoded produces semantically identical output.

**Graceful degradation over strict failure.** Several codecs (HTML in
particular, but also YAML and INI) tolerate malformed input rather than
aborting on the first structural error. Unmatched HTML closing tags are
silently skipped. INI files with bare values (no section header) are
assigned to a root-level table. This philosophy reflects Nova's role as
a practical scripting language — real-world data is messy.

---

## 2. Public API

### Core Functions

```c
int ndp_decode(NovaVM *vm, const char *text, size_t len,
               const NdpOptions *opts, char *errbuf);
```

Parses `text` (of length `len`) in the format specified by `opts->format`
and pushes a Nova table onto the VM stack. Returns `0` on success, `-1`
on error (with a diagnostic written to `errbuf`).

```c
int ndp_encode(NovaVM *vm, int idx, const NdpOptions *opts,
               NdpBuf *out, char *errbuf);
```

Serializes the table at stack index `idx` into the format specified by
`opts->format`, appending the output to `out`. Returns `0` on success,
`-1` on error.

```c
NdpFormat ndp_detect(const char *text, size_t len);
```

Examines the first bytes of `text` and returns a best-guess `NdpFormat`
value. Used by the `data.detect()` library function.

### Format Enum

```c
typedef enum {
    NDP_JSON,
    NDP_CSV,
    NDP_TSV,
    NDP_INI,
    NDP_TOML,
    NDP_HTML,
    NDP_YAML,
    NDP_NINI
} NdpFormat;
```

### Options Struct

All eight codecs share a single options struct. Each codec reads only
the fields relevant to its format; the rest are ignored.

```c
typedef struct {
    NdpFormat format;

    /* CSV / TSV */
    char      csv_delimiter;       /* default: ','          */
    char      csv_quote;           /* default: '"'          */
    int       csv_header;          /* default: 1 (true)     */

    /* JSON */
    int       json_strict;         /* default: 0            */

    /* Encoding */
    int       pretty;              /* default: 0            */
    int       indent;              /* default: 2 (spaces)   */

    /* INI */
    int       ini_typed;           /* default: 1 (true)     */

    /* HTML */
    int       html_text_only;      /* default: 0            */

    /* NINI */
    int       nini_interpolate;    /* default: 1 (true)     */
    int       nini_tasks_only;     /* default: 0            */
} NdpOptions;
```

The `ndp_options_init()` function fills all fields with sensible
defaults, ensuring forward compatibility when new options are added.

---

## 3. Format Detection

When a program calls `data.decode(text)` without specifying a format,
NDP runs a heuristic detection algorithm over the input. The algorithm
examines only the first few characters and, if necessary, the first ten
lines of the text — it never scans the entire input.

### Detection Algorithm

```
1. Skip any UTF-8 BOM (0xEF 0xBB 0xBF) and leading whitespace.

2. Inspect the first non-whitespace character:
   a. '<'  →  HTML
   b. '{'  →  JSON (object)
   c. '['  →  Ambiguous (could be JSON array, INI section, or TOML).
              Peek at the character after '[':
              - Another '['  →  TOML ([[array_of_tables]])
              - Otherwise    →  Continue to line scan

3. Scan the first 10 non-empty, non-comment lines:
   a. If a line matches [section_name] and another line contains '=' →  INI
   b. If a line matches [[double_bracket]]  →  TOML
   c. If a line matches key: value (colon, no equals sign)  →  YAML
   d. If a line starts with '- '  →  YAML (sequence)
   e. If lines contain tab characters but no commas  →  TSV
   f. If lines contain commas  →  CSV

4. If no pattern matches → default to JSON and let the parser
   report a syntax error if the guess is wrong.
```

The ten-line limit keeps detection fast — O(1) for the common cases
(JSON, HTML) and O(k) where k ≤ 10 for ambiguous inputs. The algorithm
resolves the `[[` ambiguity between JSON arrays of arrays and TOML
array-of-tables by checking for the double-bracket pattern.

---

## 4. Parser Infrastructure

All format-specific parsers share a common state structure and a set
of helper functions.

### Parser State

```c
typedef struct {
    const char *src;    /* input text                        */
    size_t      pos;    /* current byte offset               */
    int         line;   /* current line number (1-based)     */
    int         col;    /* current column (1-based)          */
    NovaVM     *vm;     /* VM pointer for stack operations   */
    char       *errbuf; /* destination for error messages    */
} NdpParser;
```

### Core Operations

**`peek()`** — Returns the character at the current position without
advancing. Returns `'\0'` at end of input.

**`advance()`** — Returns the current character and advances the position
by one byte, updating line and column counters. Newline characters
(`'\n'`) increment the line counter and reset the column to 1.

**`skip_whitespace()`** — Consumes spaces, tabs, carriage returns, and
newlines until a non-whitespace character or end of input is reached.

**`match(c)`** — If the current character equals `c`, advances past it
and returns true. Otherwise returns false without advancing.

**`consume_literal(s)`** — Attempts to match a string literal (e.g.,
`"true"`, `"null"`) at the current position. Advances past it on
match, reports an error on mismatch.

### Error Reporting

All errors include line and column information:

```
line 42 col 15: unterminated string literal
line 1 col 1: expected '{' or '[' at start of JSON
line 17 col 5: duplicate key 'name' in section [server]
```

Error messages are written into a 256-byte caller-provided buffer
(`errbuf`). The return value `-1` signals failure; the caller checks
`errbuf` for the diagnostic.

---

## 5. JSON Codec

NDP's JSON codec implements RFC 8259 via recursive descent. It is the
largest single codec in NDP and handles the full JSON specification
including Unicode escape sequences and surrogate pairs.

### Parser Structure

The parser is a direct recursive descent over the JSON grammar:

```
value   →  object | array | string | number | "true" | "false" | "null"
object  →  '{' (pair (',' pair)*)? '}'
pair    →  string ':' value
array   →  '[' (value (',' value)*)? ']'
string  →  '"' characters '"'
number  →  integer fraction? exponent?
```

Each grammar rule maps to a single C function. The parser operates
directly on the VM stack — as it recognizes structure, it pushes tables,
strings, numbers, and booleans onto the stack and assembles them into
the result table.

### Number Parsing

JSON numbers are parsed in two phases. First, the parser accumulates
the raw characters (digits, sign, decimal point, exponent indicator).
Then it routes the accumulated string to one of two C standard library
functions:

- If the number contains `.` or `e`/`E` → `strtod()` → Nova float
- Otherwise → `strtoll()` → Nova integer

This preserves the distinction between `42` (integer) and `42.0`
(float) through the decode-transform-encode pipeline.

### String Handling

JSON strings support the full escape repertoire:

| Escape | Character |
|--------|-----------|
| `\"` | Quotation mark |
| `\\` | Reverse solidus |
| `\/` | Solidus |
| `\b` | Backspace (U+0008) |
| `\f` | Form feed (U+000C) |
| `\n` | Line feed (U+000A) |
| `\r` | Carriage return (U+000D) |
| `\t` | Tab (U+0009) |
| `\uXXXX` | Unicode code point |

**Unicode and surrogate pairs.** The `\uXXXX` escape is fully
implemented. When the codec encounters a high surrogate (U+D800 through
U+DBFF), it expects a following `\uXXXX` low surrogate (U+DC00 through
U+DFFF) and combines them into a single Unicode code point:

```
codepoint = 0x10000 + ((high - 0xD800) << 10) + (low - 0xDC00)
```

The resulting code point is encoded as UTF-8 in the output string.

### Depth Limiting

The JSON parser enforces a maximum nesting depth of 100 levels. This
prevents stack exhaustion from deeply nested or adversarial input like
`[[[[[...` repeated thousands of times. The depth counter increments on
each entry to `parse_object()` or `parse_array()` and decrements on
exit.

### Encoding

The JSON encoder walks a Nova table and emits JSON text. Key behaviors:

- **Object vs. array detection.** A table is emitted as a JSON array if
  all keys are consecutive integers starting at 0; otherwise it is
  emitted as a JSON object.
- **Circular reference detection.** The encoder tracks visited tables
  during traversal and reports an error if a cycle is detected.
- **Special float values.** IEEE 754 infinity and NaN cannot be
  represented in JSON. The encoder emits `null` for these values, per
  common practice.
- **Nil omission.** Table entries with nil values are silently omitted
  during encoding — JSON has no `nil` type.

---

## 6. CSV / TSV Codec

The CSV codec implements RFC 4180 field parsing with extensions for
type inference and configurable delimiters. TSV is handled by the
same codec with the delimiter set to `'\t'`.

### Field Parsing

Each field falls into one of two categories:

**Quoted fields** begin with the configured quote character (default
`"`) and end with a matching quote. Within a quoted field, a doubled
quote (`""`) represents a literal quote character. Quoted fields may
contain newlines, delimiters, and any other characters.

**Unquoted fields** extend from the current position to the next
delimiter or end of line. Leading and trailing whitespace is preserved.

### Header Mode

When `csv_header` is true (the default), the first row is treated as
column names. Each subsequent row is decoded as a keyed table:

```
Input:  name,age,city
        Alice,30,NYC

Result: { [0] = { name = "Alice", age = 30, city = "NYC" } }
```

When `csv_header` is false, rows are decoded as integer-indexed arrays:

```
Input:  Alice,30,NYC

Result: { [0] = { [0] = "Alice", [1] = 30, [2] = "NYC" } }
```

### Type Inference

Every field value passes through type inference (see
[Section 12](#12-type-inference)). Empty fields decode to `nil`. This
means a CSV file with numeric columns produces integer or float values
in the resulting table, not strings — enabling arithmetic operations
without explicit conversion.

### Memory

CSV header names are stored in a dynamically allocated array that grows
by 2x doubling. This array is freed after decoding completes; the
header strings themselves are interned in the VM's string pool and
persist as long as they are referenced.

---

## 7. INI Codec

The INI codec handles standard INI files with section headers, key-value
pairs, and optional type inference.

### Parsing Strategy

The parser processes input line by line:

1. Skip blank lines and comment lines (starting with `#` or `;`).
2. If the line matches `[section_name]`, create or select a sub-table
   under the root table with the given name.
3. If the line matches `key = value`, strip optional surrounding
   quotes from the value, apply type inference if enabled, and store
   the result in the current section table.

Key-value pairs appearing before any section header are stored directly
in the root-level table.

### Type Inference

When `ini_typed` is true (the default), values pass through NDP's
unified type inference rules (see [Section 12](#12-type-inference)).
When false, all values are stored as strings.

### Boolean Patterns

The INI codec recognizes an extended set of boolean literals, matched
case-insensitively:

| True | False |
|------|-------|
| `true` | `false` |
| `yes` | `no` |
| `on` | `off` |

### Stack Recovery

A subtle implementation detail: after calling `set_field` to store a
key-value pair, the VM pops the value from the stack. The INI parser
must re-push the current section table after each field insertion to
maintain the correct stack layout for subsequent operations. This
pattern is shared with the TOML and YAML codecs.

---

## 8. TOML Codec

The TOML codec implements a practical subset of TOML v1.0, covering the
structures most commonly found in configuration files.

### Supported Features

| Feature | Status |
|---------|--------|
| Key-value pairs | Supported |
| Standard tables `[name]` | Supported |
| Array of tables `[[name]]` | Supported |
| Inline arrays `[1, 2, 3]` | Supported |
| Strings (basic `"..."`) | Supported |
| Integers | Supported |
| Floats | Supported |
| Booleans | Supported |
| Comments (`#`) | Supported |
| Digit separators (`1_000_000`) | Supported |
| Special floats (`inf`, `nan`) | Supported |

### Unsupported Features

The following TOML v1.0 features are not implemented:

- Multi-line basic strings (`"""..."""`)
- Literal strings (`'...'`)
- Dotted keys (`a.b.c = value`)
- Inline tables (`{key = value}`)
- Datetime types (parsed as strings)

These omissions are deliberate — they represent features rarely
encountered in the configuration files Nova targets, and adding them
would increase codec complexity without proportional benefit.

### Number Parsing

TOML numbers support underscore digit separators, which the parser
strips before passing the value to `strtoll()` or `strtod()`:

```toml
population = 1_000_000    # → integer 1000000
pi         = 3.141_592    # → float 3.141592
```

Special float values `inf` and `nan` (case-insensitive, with optional
`+`/`-` sign) are recognized and converted to the corresponding IEEE
754 values.

### Array-of-Tables Detection

The encoder determines whether a table should be emitted as a TOML
`[[array_of_tables]]` or a standard `[table]` by inspecting its
structure: if all keys are consecutive integers and all values are
tables, it is emitted as an array of tables.

---

## 9. YAML Codec

The YAML codec implements a practical indentation-based parser suitable
for configuration files and data interchange. It is not a full YAML 1.2
implementation — it handles the subset of YAML that appears in
practice: mappings, sequences, scalars, and nested structures.

### Two-Phase Parsing

The YAML parser uses a novel two-phase strategy that avoids the
complexity of a traditional YAML state machine:

**Phase 1: Line splitting.** The parser splits the input into an array
of lines, recording each line's indentation level (number of leading
spaces). Comment lines (`# ...`) and blank lines are stripped during
this phase.

**Phase 2: Pair-wise parsing.** The parser walks the line array,
interpreting structure based on indentation deltas between consecutive
lines:

- A line at the same indent as the previous line is a sibling key-value
  pair in the current mapping.
- A line at deeper indent than the previous line begins a nested block
  (either a sub-mapping or a sub-sequence).
- A line at shallower indent closes one or more nested blocks and
  continues at the appropriate ancestor level.

This two-phase approach converts the YAML indentation problem into an
array indexing problem, which is simpler to implement correctly and
easier to debug than a single-pass indentation tracker.

### Mappings

```yaml
key: value
nested:
  inner: data
```

Mappings parse as Nova tables with string keys. Nested mappings create
sub-tables at deeper indentation levels.

### Sequences

```yaml
items:
  - alpha
  - beta
  - gamma
```

Sequences parse as integer-indexed Nova tables (0-based). The `- `
prefix is stripped and the scalar value is type-inferred.

### Scalars

Scalar values are type-inferred through NDP's standard rules (see
[Section 12](#12-type-inference)). Quoted strings (single or double)
preserve their contents literally. The YAML keywords `null`, `true`,
and `false` are recognized as typed values.

### Memory

The line-splitting phase allocates a dynamic array of line records
with an initial capacity of 64 entries, growing by 2x doubling as
needed. This array is freed after parsing completes.

---

## 10. HTML Codec

The HTML codec is a read-only tag-soup parser that builds a tree
representation of HTML documents. It is designed for scraping, content
extraction, and document analysis — not for round-tripping HTML through
encode/decode cycles.

### Tree Structure

The parser produces a nested table tree where each element has:

```lua
{
    tag      = "div",              -- element name (lowercase)
    attrs    = { class = "main" }, -- attribute table (string keys)
    children = { ... }            -- array of child elements/text
}
```

Text content between tags is represented as string entries in the
parent's `children` array.

### Parsing Strategy

The HTML parser is deliberately tolerant of malformed markup:

- **Void elements** (`br`, `hr`, `img`, `input`, `meta`, `link`, and
  others) are self-closing — no end tag is expected or required.
- **Unmatched closing tags** are silently ignored rather than treated
  as errors.
- **Missing closing tags** at end of input are implicitly closed.
- **Attribute parsing** handles both quoted (`class="main"`) and
  unquoted (`checked`) attribute values.

This tolerance is essential for processing real-world HTML, which
frequently violates strict parsing rules.

### Entity Decoding

The HTML codec decodes both named and numeric character entities:

**Named entities:**

| Entity | Character |
|--------|-----------|
| `&amp;` | `&` |
| `&lt;` | `<` |
| `&gt;` | `>` |
| `&quot;` | `"` |
| `&apos;` | `'` |
| `&nbsp;` | (non-breaking space) |

**Numeric entities:**

- Decimal: `&#NNN;` → Unicode code point NNN
- Hexadecimal: `&#xHHH;` → Unicode code point 0xHHH

Numeric entities are converted to their UTF-8 encoding. The codec
handles the full Unicode range, producing multi-byte UTF-8 sequences
for code points above U+007F.

### Depth Limiting

Like the JSON codec, the HTML parser enforces a maximum nesting depth
of 100 levels to prevent stack exhaustion from pathological input.

### Text-Only Mode

When `html_text_only` is set in the options, the codec skips tree
construction entirely and instead concatenates all text content into
a single string. This is the efficient path for use cases like
extracting readable text from a web page.

---

## 11. NINI Codec

NINI (Nova INI) is Nova's native configuration format. The NDP codec
delegates NINI parsing entirely to the standalone `nova_nini.c`
implementation (~1,000 lines), converting between `NdpOptions` and
`NiniOptions` at the boundary.

For the full NINI format specification — including variable interpolation,
typed values, inline arrays, task sections, multi-line strings, and dotted
section names — see the [NINI Specification](NINI_SPEC.md).

### Bridge Architecture

The NDP ↔ NINI bridge is minimal:

1. Convert `NdpOptions` fields (`nini_interpolate`, `nini_tasks_only`)
   into a `NiniOptions` struct.
2. Call `nova_nini_decode()` or `nova_nini_encode()` with the converted
   options.
3. The standalone codec operates directly on the VM stack, pushing or
   reading tables in the same manner as the other NDP codecs.

This delegation keeps the NINI codec available both through NDP
(`data.decode(text, "nini")`) and through its own module
(`nini.decode(text)`), sharing the same parsing implementation.

### Key Features (Summary)

- **Variable interpolation:** `${section.key}` references resolved
  against the root table, with up to 8 levels of nested traversal.
- **Type inference:** Same rules as INI, plus hex/octal are base-10 only.
- **Inline arrays:** `key = [a, b, c]` with quote-aware splitting.
- **Array push:** `key[] = value` appends to an existing array.
- **Task sections:** `[task:name]` stored in a `__tasks` sub-table.
- **Dotted sections:** `[a.b.c]` creates nested sub-tables.
- **Multi-line strings:** Triple-quoted `"""..."""` blocks.
- **Environment dot notation:** `env.KEY = value` creates a nested
  `env` sub-table.
- **Comment styles:** `#` and `;`, both full-line and inline.

### GC Safety

The NINI codec maintains GC safety through careful stack discipline:

1. The root table is created and held on the VM stack throughout parsing.
2. Each new sub-table is immediately attached to its parent via
   `nova_table_set_str()`, making it reachable from the root.
3. String values are interned in the Weave pool and are therefore
   immortal for the duration of the VM's lifetime.
4. The root table remains on the stack until the caller pops it.

This ensures that no allocated object can be collected between
allocations — a requirement for correct interaction with Nova's
incremental garbage collector.

---

## 12. Type Inference

Several codecs (CSV, INI, NINI, TOML, YAML) share NDP's unified type
inference rules. When a codec encounters a bare value — a string not
explicitly typed by the format's own grammar — it attempts to interpret
it as a more specific type.

### Inference Rules (Evaluated in Order)

| Test | Result | Example |
|------|--------|---------|
| Empty string | `nil` | (empty field in CSV) |
| `true`, `yes`, `on` (case-insensitive) | Boolean `true` | `debug = true` |
| `false`, `no`, `off` (case-insensitive) | Boolean `false` | `verbose = off` |
| `nil`, `null`, `none` (case-insensitive) | `nil` | `value = null` |
| Parses as integer via `strtoll(s, &end, 10)` with no remainder | Integer | `port = 8080` |
| Parses as float via `strtod(s, &end)` with no remainder | Float | `pi = 3.14159` |
| Quoted with `"..."` or `'...'` | String (quotes stripped) | `name = "Nova"` |
| Everything else | String (bare) | `host = localhost` |

### Design Rationale

The integer-before-float ordering ensures that `42` is stored as a
Nova integer (not `42.0`), preserving type fidelity through
encode-decode cycles. The `strtoll` check uses base 10 exclusively —
hexadecimal (`0xFF`) and octal (`0o77`) prefixes are interpreted as
strings, not numbers. This avoids surprising implicit conversions in
configuration files.

The extended boolean set (`yes`/`no`, `on`/`off`) matches common
configuration file conventions. These keywords are matched
case-insensitively via `strcasecmp`.

---

## 13. Encoding and Serialization

Each codec implements an encoder that walks a Nova table and emits
formatted text into an `NdpBuf` string builder.

### String Builder (NdpBuf)

```c
typedef struct {
    char  *data;   /* heap-allocated buffer  */
    size_t len;    /* current content length */
    size_t cap;    /* allocated capacity     */
} NdpBuf;
```

The builder starts with an initial capacity of 256 bytes and grows by
2x doubling when the buffer is full. This amortized O(1) append
strategy minimizes allocation overhead for large outputs.

### Per-Format Encoding Notes

**JSON.** Objects emit `{key:value}` pairs; arrays emit `[value,value]`.
Compact output by default. Table traversal detects arrays (consecutive
integer keys from 0) versus objects (any non-integer or non-consecutive
keys). Special float values (`inf`, `nan`) are emitted as `null`.
Circular references are detected and reported as errors.

**CSV.** The encoder emits a header row (from the keys of the first
table entry) followed by data rows. Fields containing the delimiter,
the quote character, or newlines are automatically quoted.

**INI.** Root-level key-value pairs are emitted first, followed by
section blocks. Nested tables become sections.

**TOML.** Tables become `[section]` headers; arrays of tables become
`[[section]]`. Leaf values are emitted with appropriate TOML syntax
(strings quoted, numbers bare, booleans lowercase).

**YAML.** The encoder emits `key: value` pairs with indentation for
nested structures. Sequences use `- ` prefix notation.

**NINI.** Delegation to `nova_nini_encode()`. Supports triple-quoted
multi-line strings, inline arrays, task sections, and environment
dot notation. See the [NINI Specification](NINI_SPEC.md).

**HTML.** Encoding is not supported — the HTML codec is read-only.

---

## 14. File I/O Path

The `load` and `save` functions provide file-level conveniences that
combine I/O with codec operations.

### Load Path

```
1. Open file, seek to end, measure length.
2. Validate: file size ≤ 100 MiB.
3. Allocate buffer (file size + 1 byte for NUL terminator).
4. Read entire file into buffer in a single fread().
5. Call ndp_decode(vm, buffer, len, opts, errbuf).
6. Free buffer.
7. Return decoded table on VM stack.
```

### Save Path

```
1. Call ndp_encode(vm, idx, opts, &out_buf, errbuf).
2. Write out_buf contents to file via ndp_write_file().
3. Free out_buf.
```

### File Size Limit

The 100 MiB file size limit is a safety guard against accidental
loading of very large files (e.g., multi-gigabyte log files). The
limit is checked via `fseek`/`ftell` before any memory is allocated.
Files exceeding this limit produce an error without allocating the
buffer.

---

## 15. Memory Management

NDP's memory strategy varies by component:

| Component | Strategy | Details |
|-----------|----------|---------|
| `NdpBuf` (string builder) | 2x doubling | Starts at 256 bytes |
| CSV header names | 2x doubling | Dynamic array, freed after decode |
| YAML line records | 2x doubling | Initial capacity 64, freed after decode |
| Error messages | Stack-allocated | 256-byte fixed buffers |
| Name buffers | Stack-allocated | 256-byte fixed buffers for section/key/tag names |
| File I/O buffers | Single allocation | Sized to file length, freed after decode |

All heap allocations are checked for `NULL` returns. Allocation failure
produces a descriptive error message (`"out of memory"`) and returns
`-1` to the caller. No codec silently continues after a failed
allocation.

### VM Stack Integration

Codecs build their result tables directly on the VM stack using Nova's
table API (`nova_vm_push_table`, `nova_vm_push_string`,
`nova_table_set_str`, etc.). This means intermediate values are visible
to the garbage collector at all times — no decoded data exists in a
GC-invisible shadow heap. The root table remains on the stack throughout
parsing, and sub-tables are attached to their parents immediately after
creation.

---

## 16. Library Bindings

NDP is exposed to Nova scripts through the `data` standard library
module and eight format-specific modules.

### Format-Specific Modules

Each format is available through a dedicated import:

```lua
#import json     -- or: dec json = require("data.json")
#import csv
#import toml
#import yaml
#import html
#import nini
```

Every format module provides four functions:

| Function | Signature | Description |
|----------|-----------|-------------|
| `decode` | `(text [, opts])` → table | Parse string → table |
| `encode` | `(table [, opts])` → string | Serialize table → string |
| `load` | `(filename [, opts])` → table | Read file → parse → table |
| `save` | `(filename, table [, opts])` | Serialize → write file |

The `opts` parameter is an optional Nova table whose keys map to the
corresponding `NdpOptions` fields:

```lua
dec rows = csv.decode(text, { header = true, delimiter = "\t" })
dec pretty = json.encode(data, { pretty = true, indent = 4 })
```

### Unified Data Module

The `data` module provides format-agnostic access:

```lua
dec data = require("data")

dec result = data.decode(text, "json")
dec output = data.encode(value, "csv")
dec format = data.detect(text)    -- returns "json", "csv", etc.
```

### Registration Mechanism

The library bindings use a C preprocessor macro (`DEFINE_FORMAT_ALL`)
to generate the four functions (decode, encode, load, save) for each
format from a single macro invocation. This ensures that all formats
have identical function signatures, error handling behavior, and
options parsing logic.

```c
DEFINE_FORMAT_ALL(json, NDP_JSON, "json")
DEFINE_FORMAT_ALL(csv,  NDP_CSV,  "csv")
DEFINE_FORMAT_ALL(toml, NDP_TOML, "toml")
DEFINE_FORMAT_ALL(yaml, NDP_YAML, "yaml")
DEFINE_FORMAT_ALL(html, NDP_HTML, "html")
DEFINE_FORMAT_ALL(nini, NDP_NINI, "nini")
```

Each macro expansion produces a C function that:
1. Extracts the text or filename argument from the VM stack.
2. Parses the optional options table into an `NdpOptions` struct.
3. Calls `ndp_decode` or `ndp_encode` with the appropriate format.
4. Pushes the result or reports an error.

---

## 17. Constants and Limits

| Constant | Value | Purpose |
|----------|-------|---------|
| NDP buffer initial capacity | 256 bytes | String builder starting size |
| NDP buffer growth factor | 2x | Exponential doubling |
| Format detection line limit | 10 lines | Heuristic scan depth |
| JSON/HTML max nesting depth | 100 levels | Stack exhaustion prevention |
| File size limit | 100 MiB | Memory safety guard |
| Error buffer size | 256 bytes | Diagnostic message capacity |
| Name buffer size | 256 bytes | Section, key, and tag names |
| YAML line array initial capacity | 64 entries | Line record storage |
| NINI interpolation depth | 8 levels | Nested `${...}` resolution |
| NINI task max depth | 32 levels | Cycle detection in task deps |

---

*See also: [NINI Specification](NINI_SPEC.md) · [VM Specification](NOVA_VM_SPEC.md) · [Bytecode Specification](NOVA_BYTECODE_SPEC.md)*
