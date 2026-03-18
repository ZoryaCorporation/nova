<!-- ============================================================
     NOVA DOC FORMAT SPECIFICATION v1.0.0
     .nd — Structured Documents for Humans, Machines, and AI

     Author:  Anthony Taliento (Zorya Corporation)
     Date:    2026-03-04
     Status:  RATIFIED
     Parent:  NINI Specification v1.0.0
     ============================================================ -->

# Nova Doc Specification v1.0.0

**Nova Doc (`.nd`)** is a document format derived from NINI, designed to
serve three audiences simultaneously: humans who read it, machines that
parse it, and AI models that consume it as context. It inherits NINI's
key=value syntax, typed values, arrays, and interpolation, then adds
**text blocks**, **typed sections**, **tagging**, and **frontmatter** to
make it suitable for structured documentation, analysis output, memory
records, and tool interchange.

**File extension:** `.nd`
**MIME type:** `text/x-nova-doc`
**Encoding:** UTF-8
**Magic line:** `@nova-doc 1` (first non-blank, non-comment line)

---

## Table of Contents

1.  [Design Goals](#1-design-goals)
2.  [Relationship to NINI](#2-relationship-to-nini)
3.  [Format Overview](#3-format-overview)
4.  [Frontmatter](#4-frontmatter)
5.  [Typed Sections](#5-typed-sections)
6.  [Text Blocks](#6-text-blocks)
7.  [Checklists](#7-checklists)
8.  [Tags](#8-tags)
9.  [Priority / Severity](#9-priority--severity)
10. [Timestamps](#10-timestamps)
11. [References and Links](#11-references-and-links)
12. [Includes](#12-includes)
13. [Format Detection](#13-format-detection)
14. [Grammar (EBNF)](#14-grammar-ebnf)
15. [Token Efficiency Analysis](#15-token-efficiency-analysis)
16. [Reserved Section Types](#16-reserved-section-types)
17. [Use Cases](#17-use-cases)
18. [Examples](#18-examples)

---

## 1. Design Goals

| Goal | Rationale |
|------|-----------|
| **Human-readable** | Clean visual structure, no bracket/brace noise |
| **Machine-parseable** | Strict grammar, zero ambiguity, single-pass parse |
| **AI-friendly** | Minimal structural tokens, natural-language text blocks |
| **Token-efficient** | ~40% fewer tokens than equivalent JSON for the same data |
| **NINI-compatible** | Superset — every NINI feature works in `.nd` |
| **Self-describing** | Typed sections declare their schema implicitly |
| **Composable** | `@include` for modularity, `@ref` for cross-document links |
| **Bidirectional** | Both humans and AI can author `.nd` documents |

### 1.1 The Three-Audience Problem

Most formats optimize for one audience:

- **JSON** — great for machines, painful for humans, expensive for AI
  (30-40% structural tokens: `{}`, `""`, `,`, `:`)
- **Markdown** — great for humans, unparseable for machines, ambiguous
  for AI (no typed fields, no schema)
- **YAML** — attempts all three, fails at all three (whitespace
  sensitivity, implicit typing surprises, security issues)

Nova Doc addresses this by separating **structured data** (key=value,
typed, parseable) from **prose content** (text blocks, natural language,
readable). Machines parse the structure. Humans and AI read the prose.
Both can access both.

---

## 2. Relationship to NINI

Nova Doc is a **strict superset** of NINI v1.0.0. Every valid NINI
document is a valid `.nd` document. The extensions are:

| Feature | NINI | Nova Doc |
|---------|------|----------|
| Key = value | ✓ | ✓ |
| Typed values | ✓ | ✓ |
| Arrays | ✓ | ✓ |
| Variable interpolation | ✓ | ✓ |
| Multi-line strings (`"""`) | ✓ | ✓ |
| Sections `[name]` | ✓ | ✓ |
| Task sections `[task:name]` | ✓ | ✓ |
| `@include` | ✓ | ✓ |
| **Text blocks `<< >>`** | ✗ | **✓** |
| **Typed sections `[type:name]`** | task only | **any type** |
| **Tags `+tag`** | ✗ | **✓** |
| **Frontmatter `@key`** | version only | **✓** |
| **Priority `!priority`** | ✗ | **✓** |
| **Checklists `[ ]` `[x]` `[-]`** | ✗ | **✓** |

The parser distinguishes NINI from Nova Doc by the magic line:
- `@version nini/1` → NINI mode (text blocks are a parse error)
- `@nova-doc 1` → Nova Doc mode (all extensions enabled)
- Neither → auto-detect (text blocks trigger Nova Doc mode)

---

## 3. Format Overview

A Nova Doc document consists of:

```nd
@nova-doc 1

# Comments start with # or ;

# Frontmatter — document-level metadata
@type = analysis
@created = 2026-03-04T14:00:00Z
@author = nova-mcp/analyze
@tags = [vm, core, performance]

# Sections — structured data grouped by typed headers
[file:src/nova_vm.c]
lines = 2847
functions = 47
complexity = high

# Text blocks — prose content for humans and AI
summary = <<
The core VM execution engine. Contains the main
dispatch loop with computed-goto across 80+ opcodes.
>>

# Subsections — hierarchical organization
[function:nova_vm_execute]
line = 120
params = [NovaVM *N]
returns = NovaStatus

description = <<
Main execution loop. Fetches and dispatches instructions
via computed-goto. GC safepoints at backward jumps and
function call boundaries.
>>
```

---

## 4. Frontmatter

Lines starting with `@` before the first section define document-level
metadata. Unlike NINI's root-level keys, frontmatter keys use the `@`
prefix to distinguish them as metadata rather than content.

```nd
@nova-doc 1
@type = analysis
@created = 2026-03-04T14:00:00Z
@source = nova-mcp/analyze
@target = src/nova_vm.c
@version = 0.2.0
@tags = [vm, core, dispatch]
```

### 4.1 Reserved Frontmatter Keys

| Key | Type | Description |
|-----|------|-------------|
| `@nova-doc` | integer | Format version (required, must be first) |
| `@type` | string | Document type (analysis, memory, tree, report, log) |
| `@created` | string | ISO 8601 timestamp |
| `@updated` | string | ISO 8601 timestamp (last modification) |
| `@source` | string | What generated this document |
| `@target` | string | What this document is about |
| `@author` | string | Human or tool author |
| `@version` | string | Version of the subject (not the format) |
| `@tags` | array | Document-level tags for categorization |
| `@id` | string | Unique identifier (for memory records) |
| `@expires` | string | ISO 8601 timestamp (for TTL on memory records) |
| `@priority` | string | low, normal, high, critical |

Custom frontmatter keys are allowed. Unknown `@` keys are preserved
without error.

---

## 5. Typed Sections

NINI restricts typed sections to `[task:name]`. Nova Doc generalizes
this: `[type:name]` where `type` is any identifier. The type tells the
parser (and the reader) what schema the section follows.

```nd
[file:src/nova_vm.c]        # A file analysis
[function:nova_vm_execute]   # A function within that file
[memory:build-command]       # A memory record
[finding:gc-safepoint]       # An analysis finding
[task:build]                 # A task definition (same as NINI)
[metric:dispatch-speed]      # A performance metric
```

### 5.1 Section Type Rules

- Types are lowercase identifiers: `a-z`, `0-9`, `_`, `-`
- Names follow the same rules as NINI section names (including dots)
- Unknown types are preserved without error (forward-compatible)
- Typed and untyped sections can coexist in the same document

### 5.2 Section Nesting

Sections are **flat with implied hierarchy** based on document order.
A `[function:...]` section appearing after a `[file:...]` section is
implicitly a child of that file. Parsers MAY build a tree based on
type relationships (file → function, analysis → finding) but MUST NOT
require it — flat iteration is always valid.

Explicit nesting via dots is also supported:

```nd
[file:src/nova_vm.c]
lines = 2847

[file:src/nova_vm.c.function:nova_vm_execute]
line = 120
```

However, the implicit ordering style is preferred for readability:

```nd
[file:src/nova_vm.c]
lines = 2847

[function:nova_vm_execute]
line = 120
```

### 5.3 Repeated Section Types

Multiple sections can share the same type with different names.
This is the standard way to express lists of structured items:

```nd
[function:nova_vm_create]
line = 45
returns = NovaVM *

[function:nova_vm_execute]
line = 120
returns = NovaStatus

[function:nova_vm_destroy]
line = 910
returns = void
```

---

## 6. Text Blocks

The core extension. Text blocks use `<<` and `>>` delimiters to contain
multi-line natural-language prose, keeping it cleanly separated from
structured key=value data.

```nd
summary = <<
This is a text block. It can span multiple lines.
Whitespace and newlines are preserved exactly as written.
No escaping is needed for any characters except >>.
>>
```

### 6.1 Text Block Rules

1. **Opening:** `key = <<` — the `<<` must be at the end of the line
   (after optional whitespace). Content starts on the next line.
2. **Closing:** `>>` alone on its own line (with optional leading
   whitespace). The `>>` line is not included in the content.
3. **Content:** Everything between opening and closing is literal text.
   No interpolation, no escaping, no comment stripping.
4. **Leading newline:** The newline after `<<` is stripped (content
   starts on the first content line, not a blank line).
5. **Trailing newline:** The newline before `>>` is stripped.
6. **Indentation:** Preserved as-is. If the document is indented,
   the content preserves that indentation.

### 6.2 Why `<<` / `>>` Instead of `"""`

NINI already uses `"""` for multi-line strings. Text blocks are
semantically different:

| Feature | `"""` Multi-Line String | `<<` `>>` Text Block |
|---------|------------------------|---------------------|
| Purpose | Structured string value | Prose / documentation |
| Interpolation | Yes (`${...}` resolved) | No (literal content) |
| Comment stripping | Yes (inline `#` stripped) | No (everything preserved) |
| Intended audience | Machine consumption | Human and AI consumption |
| Typical length | 1-5 lines | 1-50+ lines |
| Contains code examples | Awkward | Natural |

Text blocks are designed for content that will be **read**, not
**processed**. This makes them ideal for:
- Analysis summaries
- Documentation prose
- Error explanations
- Code examples (no escape issues)
- Memory record values

### 6.3 Nested Delimiters

If the text content itself contains `>>` on a line by itself (rare),
escape it with a leading backslash: `\>>`. The backslash is stripped
during parsing. No other escaping is needed — `<<` inside a text block
is literal and does not nest.

---

## 7. Checklists

Checklists bring task tracking directly into the document format.
Each checklist item is a key=value pair where the value ends with a
**status marker**: `[ ]`, `[x]`, or `[-]`.

```nd
[tracker:sdk-release]
created = 2026-03-04
+release +v0.2.0

build_libnova = Build static library [x]
run_tests = Execute full test suite [x]
update_changelog = Write changelog entry [ ]
bump_version = Update VERSION file [ ]
tag_release = Create git tag [ ]
publish = Push to package registry [-]
```

### 7.1 Status Markers

| Marker | Meaning | Display |
|--------|---------|---------|
| `[x]` | **Complete** — finished, passed, done | ✓ |
| `[-]` | **Incomplete** — failed, blocked, skipped | ✗ |
| `[ ]` | **Not started** — pending, todo | ○ |

Markers MUST appear at the end of the value, after optional whitespace.
The marker is stripped from the value during parsing and stored as a
separate `status` field on the key-value entry.

### 7.2 Parsing Rules

1. **Detection:** A value ending in `[ ]`, `[x]`, or `[-]` (with
   optional surrounding whitespace) is a checklist item.
2. **Value extraction:** Everything before the marker is the display
   text (trimmed). `build_libnova = Build static library [x]` parses
   as `{key: "build_libnova", value: "Build static library", status: "complete"}`.
3. **Bare markers:** A key with only a marker and no text is valid:
   `step_one = [x]` → `{key: "step_one", value: "", status: "complete"}`.
4. **Not a conflict:** Inline arrays use `[a, b, c]` syntax with
   commas. The checklist markers `[ ]`, `[x]`, `[-]` contain no
   commas and are always at the end — no ambiguity.

### 7.3 Nested Checklists

Indented keys under a parent key create sub-tasks. The parser tracks
indentation level to build a hierarchy:

```nd
[tracker:nova-sdk]
+project +milestone

phase_1 = Core infrastructure [x]
    build_system = Set up Makefile [x]
    shared_json = JSON parser + builder [x]
    shared_jsonrpc = JSON-RPC transport [x]

phase_2 = Server implementation [x]
    lsp_server = LSP diagnostics server [x]
    mcp_server = MCP tool server [x]
    mcp_tools = 7 Nova-smart tools [x]

phase_3 = Format & tooling [-]
    nova_doc_spec = Nova Doc specification [x]
    nd_parser = .nd parser implementation [ ]
    memory_system = SQLite memory store [ ]
    analyze_tool = Code analysis tool [ ]
    workspace_tree = Directory snapshot tool [ ]
```

**Nesting rules:**
- Indentation is 4 spaces per level (consistent with Nova code style)
- A parent item's status can be auto-computed from children:
  all `[x]` → parent is `[x]`, any `[-]` → parent is `[-]`,
  otherwise `[ ]`
- Auto-computation is optional — explicit parent status overrides
- Maximum nesting depth: 8 levels

### 7.4 Aggregate Status

A section-level `@status` directive provides a quick summary:

```nd
[tracker:release-v0.2.0]
@status = 4/6    # 4 of 6 items complete
@progress = 67   # percentage (auto-computed or explicit)
+release

build = Build release binary [x]
test = Run test suite [x]
docs = Update documentation [x]
changelog = Write changelog [x]
tag = Create git tag [ ]
publish = Push release [-]
```

`@status` and `@progress` are computed by tools when generating or
updating tracker sections. They are hints for quick scanning — the
actual status is always determined by the individual markers.

### 7.5 CLI Display

When rendered by `nova` CLI tools, checklist items display with
visual indicators:

```
[tracker:release-v0.2.0]  (4/6 — 67%)
  ✓ build          Build release binary
  ✓ test           Run test suite
  ✓ docs           Update documentation
  ✓ changelog      Write changelog
  ○ tag            Create git tag
  ✗ publish        Push release
```

### 7.6 MCP Integration

When an MCP tool returns a tracker section, the AI receives structured
progress data with zero ambiguity:

- **Project kickoff:** Tool generates a tracker with all `[ ]` items
- **Progress updates:** Tool updates markers as tasks complete
- **Status queries:** AI can request just the tracker to see what's
  done, what's pending, and what's blocked — without reading the
  entire project history

This eliminates the "flying blind" problem where every new message
requires the AI to rediscover project state.

---

## 8. Tags

Tags are lightweight labels attached to sections or the entire document.
They use the `+tag` syntax, appearing on the key line after the value
or on their own line within a section.

```nd
[finding:slow-dispatch]
severity = high
+performance +vm +hotspot

description = <<
The dispatch loop is not using computed goto on this
platform, falling back to a switch statement.
>>
```

### 8.1 Tag Rules

- Tags start with `+` followed by an identifier: `+tag_name`
- Multiple tags on one line separated by whitespace
- Tags on a standalone line apply to the current section
- Tags in frontmatter (`@tags = [...]`) apply to the document
- Tags are metadata — they do not affect the section's key=value data
- Tags are searchable and filterable by tools

### 8.2 Tag vs. Key

Use tags for **categorization and filtering**. Use keys for
**structured data**:

```nd
# Good — severity is data, tags are categories
[finding:null-deref]
severity = critical
+bug +safety +pointer
file = src/nova_vm.c
line = 342

# Bad — using keys for categories
[finding:null-deref]
severity = critical
category1 = bug
category2 = safety
category3 = pointer
```

---

## 9. Priority / Severity

Sections can declare priority using the `!` prefix on a standalone line
or as an inline marker after the section header:

```nd
[finding:buffer-overflow] !critical
file = src/nova_nini.c
line = 128

[finding:unused-variable] !low
file = src/nova_gc.c
line = 45
```

### 9.1 Priority Levels

| Marker | Meaning |
|--------|---------|
| `!critical` | Must address immediately |
| `!high` | Important, address soon |
| `!normal` | Standard priority (default if omitted) |
| `!low` | Minor, address when convenient |
| `!info` | Informational, no action needed |

Priority is syntactic sugar for `priority = critical` but more visible
at a glance — both when scanning a file and when an AI tokenizes it.

---

## 10. Timestamps

Nova Doc uses ISO 8601 for all timestamps. Auto-typing detects the
common forms:

```nd
@created = 2026-03-04T14:00:00Z          # Full UTC
@updated = 2026-03-04                     # Date only
last_seen = 2026-03-04T14:00:00-05:00     # With timezone
```

Timestamps are stored as strings (not a separate type) but the auto-type
system recognizes the ISO 8601 pattern and tags them as `datetime` in the
parsed representation, enabling tool-side filtering by date range.

---

## 11. References and Links

Cross-references to files, sections, or other `.nd` documents use the
`@ref` syntax within text blocks and values:

```nd
description = <<
This function is called by @ref(function:nova_vm_execute)
and documented in @ref(docs/NOVA_VM_BLUEPRINT.md).
See @ref(finding:gc-safepoint) for related analysis.
>>
```

### 11.1 Reference Types

| Syntax | Resolves To |
|--------|-------------|
| `@ref(type:name)` | Section in current document |
| `@ref(file.nd#type:name)` | Section in another .nd document |
| `@ref(path/to/file.c)` | Source file (by path) |
| `@ref(path/file.c:L120)` | Source file at line number |
| `@ref(path/file.c:L120-L150)` | Source file line range |

References are resolved by tools at read time and are purely
informational in the document itself — an unresolved `@ref` is not an
error. It's a hint that aids navigation.

---

## 12. Includes

Same as NINI — `@include` merges another document:

```nd
@include base_config.nd
@include project_conventions.nd

[analysis:current]
# inherits all included sections
```

Include rules follow NINI v1.0.0 Section 9.

---

## 13. Format Detection

Nova Doc files are detected by:

1. **File extension:** `.nd`
2. **Magic line:** `@nova-doc 1` as the first non-blank, non-comment line
3. **Presence of text blocks:** `<<` / `>>` triggers Nova Doc mode
4. **Presence of typed sections** beyond `[task:*]`
5. **Presence of checklist markers:** `[x]`, `[-]`, `[ ]` at end of values

### 13.1 Parser Mode Selection

```
File extension is .nd         → Nova Doc mode
First directive is @nova-doc  → Nova Doc mode
First directive is @version   → NINI mode
Has << >> blocks              → Nova Doc mode (auto-detect)
Otherwise                     → NINI mode
```

---

## 14. Grammar (EBNF)

Extends NINI v1.0.0 Section 14:

```ebnf
document     = magic_line { frontmatter } { body_element } ;

magic_line   = "@nova-doc" ws integer newline ;

frontmatter  = "@" key ws "=" ws value newline
             | comment
             | blank ;

body_element = section | keyvalue | checklist_item | tagline
             | priority_line | comment | blank | directive ;

directive    = "@include" ws path newline ;

section      = "[" section_id "]" [ ws priority ] [ comment ] newline ;
section_id   = typed_id | plain_id ;
typed_id     = type ":" name ;
plain_id     = name { "." name } ;
type         = ident ;
name         = ident { "." ident } ;

keyvalue     = key ws "=" ws value [ comment ] newline ;
key          = ident [ "[]" ]
             | ident "." ident ;

value        = text_block | multiline | inline_array
             | checklist_value | interpolated_string ;

checklist_item = { ws } key ws "=" ws checklist_value [ comment ] newline ;
checklist_value = { any_text } ws status_marker ;
status_marker = "[x]" | "[-]" | "[ ]" ;

text_block   = "<<" newline { text_line } ">>" newline ;
text_line    = { any_char_except_newline } newline ;

multiline    = '"""' newline { any_char } newline '"""' ;

inline_array = "[" value_list "]" ;
value_list   = typed_value { "," typed_value } ;
typed_value  = integer | float | boolean | nil_kw | datetime
             | quoted_string | unquoted_string ;

datetime     = digit{4} "-" digit{2} "-" digit{2}
               [ "T" digit{2} ":" digit{2} ":" digit{2}
                 [ timezone ] ] ;
timezone     = "Z" | ( "+" | "-" ) digit{2} ":" digit{2} ;

interpolated_string = { char | "${" ref "}" } ;
ref          = ident { "." ident } ;

tagline      = { tag } newline ;
tag          = "+" ident ;

priority     = "!" priority_level ;
priority_level = "critical" | "high" | "normal" | "low" | "info" ;

ident        = ( letter | "_" ) { letter | digit | "_" | "-" } ;
comment      = ( "#" | ";" ) { any_char } newline ;
blank        = { ws } newline ;
ws           = " " | "\t" ;
newline      = "\n" | "\r\n" ;
```

---

## 15. Token Efficiency Analysis

Comparing equivalent structured data across formats. Using GPT-style
BPE tokenization (cl100k_base) as a reference tokenizer.

### 15.1 Benchmark: File Analysis Record

**JSON (38 tokens):**
```json
{"file":"nova_vm.c","lines":2847,"functions":47,"public":12,"static":35,"summary":"Core VM dispatch loop and execution engine."}
```

**YAML (28 tokens):**
```yaml
file: nova_vm.c
lines: 2847
functions: 47
public: 12
static: 35
summary: Core VM dispatch loop and execution engine.
```

**Nova Doc (24 tokens):**
```nd
[file:nova_vm.c]
lines = 2847
functions = 47
public = 12
static = 35

summary = <<
Core VM dispatch loop and execution engine.
>>
```

**Markdown (variable, ~30-40 tokens depending on structure):**
```markdown
## nova_vm.c
- Lines: 2847
- Functions: 47 (12 public, 35 static)

Core VM dispatch loop and execution engine.
```

### 15.2 Why Nova Doc Wins for AI

1. **No bracket/brace noise.** JSON's `{}`, `""`, `,` each consume a
   token and carry zero semantic information for the model.

2. **Key = value is natural language.** Transformers process `lines = 2847`
   as a factual statement, similar to how facts appear in training data.
   `"lines": 2847` requires the model to parse JSON syntax first.

3. **Text blocks are raw prose.** The `<<`/`>>` delimiters cost 2 tokens
   total. JSON string escaping inside values (newlines, quotes) costs
   1 token per escape.

4. **Section headers are labels.** `[file:nova_vm.c]` is one semantic
   unit. `{"file": "nova_vm.c", "type": "analysis"}` is spread across
   8 tokens.

5. **No closing delimiters.** JSON needs `}` for every `{`, `]` for
   every `[`. Nova Doc sections end implicitly at the next section.

### 15.3 Token Budget Impact

For a typical analysis document with 10 file sections, 50 function
entries, and prose summaries:

| Format | Estimated Tokens | Overhead |
|--------|-----------------|----------|
| JSON | ~2,400 | Baseline |
| YAML | ~1,700 | -29% |
| Markdown | ~1,900 | -21% |
| **Nova Doc** | **~1,450** | **-40%** |

At scale (memory database with 500 records, full project analysis),
a 40% reduction means fitting 40% more context into the same window.

---

## 16. Reserved Section Types

These types have defined schemas for tool interoperability:

### 16.1 `[file:path]` — File Analysis

```nd
[file:src/nova_vm.c]
lines = 2847
size = 89234
modified = 2026-03-04T12:00:00Z
language = c
functions = 47
public = 12
static = 35
structs = [NovaVM, NovaCallFrame]
includes = [nova_gc.h, nova_meta.h]
+core +vm

summary = <<
Core VM dispatch loop and execution engine.
>>
```

| Key | Type | Required | Description |
|-----|------|----------|-------------|
| lines | integer | yes | Total line count |
| size | integer | no | File size in bytes |
| modified | datetime | no | Last modification time |
| language | string | no | Programming language |
| functions | integer | no | Total function count |
| public | integer | no | Public function count |
| static | integer | no | Static/internal function count |
| structs | array | no | Struct/type names defined |
| includes | array | no | Files included/imported |
| summary | text | yes | Human/AI-readable summary |

### 16.2 `[function:name]` — Function Analysis

```nd
[function:nova_vm_execute]
line = 120
end = 890
params = [NovaVM *N]
returns = NovaStatus
complexity = high
calls = [novai_dispatch, nova_gc_step]
+hotspot +dispatch

description = <<
Main execution loop. Fetches instructions from the current
call frame, decodes via computed-goto, dispatches to opcode
handlers.
>>
```

| Key | Type | Required | Description |
|-----|------|----------|-------------|
| line | integer | yes | Start line |
| end | integer | no | End line |
| params | array | no | Parameter list |
| returns | string | no | Return type |
| complexity | string | no | low, medium, high |
| calls | array | no | Functions called |
| description | text | yes | What the function does |

### 16.3 `[memory:key]` — Memory Record

```nd
[memory:build-command]
category = convention
created = 2026-03-04T14:00:00Z
source = user
+build +make

value = <<
Build with 'make' for release, 'make DEBUG=1' for debug
with sanitizers. 'make lib' builds libnova.a. 'make test'
runs all 328 tests across 26 suites.
>>
```

| Key | Type | Required | Description |
|-----|------|----------|-------------|
| category | string | yes | convention, decision, finding, context, task |
| created | datetime | yes | When this was recorded |
| source | string | yes | Who/what created it (user, tool, analysis) |
| expires | datetime | no | TTL for auto-cleanup |
| value | text | yes | The memory content |

### 16.4 `[finding:id]` — Analysis Finding

```nd
[finding:gc-safepoint-gap] !high
file = src/nova_vm.c
line = 567
+bug +gc +safety

description = <<
The tight loop at line 567 lacks a GC safepoint. If the
loop iterates more than ~10K times, GC pressure can cause
OOM before collection runs.
>>

recommendation = <<
Insert nova_gc_check(N) at the loop's backward jump
to ensure collection can run during long iterations.
>>
```

| Key | Type | Required | Description |
|-----|------|----------|-------------|
| file | string | yes | Source file path |
| line | integer | no | Line number |
| description | text | yes | What was found |
| recommendation | text | no | Suggested fix |

### 16.5 `[tree:path]` — Directory Entry

```nd
[tree:src]
type = directory
children = 47
+source

[tree:src/nova_vm.c]
type = file
size = 89234
lines = 2847
+core

[tree:src/nova_gc.c]
type = file
size = 12456
lines = 340
+gc
```

| Key | Type | Required | Description |
|-----|------|----------|-------------|
| type | string | yes | file or directory |
| size | integer | no | Size in bytes (files) |
| lines | integer | no | Line count (files) |
| children | integer | no | Entry count (directories) |

### 16.6 `[metric:name]` — Performance / Stats

```nd
[metric:compile-time]
value = 0.342
unit = seconds
sample_size = 100
+performance +compiler

description = <<
Average compilation time for the standard test suite
measured over 100 runs on the reference hardware.
>>
```

### 16.7 `[log:timestamp]` — Log Entry

```nd
[log:2026-03-04T14:23:00Z]
level = info
source = nova-mcp/analyze
+session

message = <<
Completed full project analysis. 47 source files,
12 findings (2 high, 4 normal, 6 low). Results stored
in .nova/analysis.nd.
>>
```

### 16.8 `[tracker:name]` — Task / Progress Tracker

```nd
[tracker:sdk-v0.2.0]
created = 2026-03-04
updated = 2026-03-04T19:00:00Z
@status = 5/8
@progress = 62
+release +sdk

build_lsp = Build LSP server [x]
build_mcp = Build MCP server [x]
mcp_tools = Implement 7 smart tools [x]
nd_spec = Write Nova Doc spec [x]
nd_parser = Implement .nd parser [ ]
memory_db = SQLite memory system [ ]
analyze = Code analysis tool [ ]
tree_tool = Workspace tree snapshot [x]
```

| Key | Type | Required | Description |
|-----|------|----------|-------------|
| created | datetime | yes | When the tracker was created |
| updated | datetime | no | Last modification time |
| @status | string | no | Completion fraction (e.g., "5/8") |
| @progress | integer | no | Completion percentage (0-100) |
| *task_key* | checklist | yes | One or more `key = description [status]` entries |

**Nested trackers** use indentation for sub-tasks:

```nd
[tracker:release-pipeline]
created = 2026-03-04
@status = 3/6
+release +ci

build = Build phase [x]
    compile = Compile all targets [x]
    link = Link binaries [x]
    package = Create archive [x]

test = Test phase [-]
    unit = Unit tests [x]
    integration = Integration tests [-]
    bench = Performance benchmarks [ ]

deploy = Deploy phase [ ]
    staging = Deploy to staging [ ]
    production = Deploy to production [ ]
```

Trackers are the primary format for AI-to-human and AI-to-AI progress
communication. They provide at-a-glance project state without requiring
the reader to parse logs, git history, or conversation context.

---

## 17. Use Cases

### 17.1 MCP Tool Output

All MCP tools that produce structured results emit Nova Doc:

```nd
@nova-doc 1
@type = analysis
@source = nova-mcp/analyze
@target = src/nova_vm.c
@created = 2026-03-04T14:00:00Z

[file:src/nova_vm.c]
lines = 2847
functions = 47
public = 12
static = 35
includes = [nova_gc.h, nova_meta.h, nova_opcode.h]
+core +vm

summary = <<
Core VM dispatch loop and execution engine. The main
nova_vm_execute function uses computed-goto dispatch
across 80+ opcodes. GC safepoints at function calls
and backward jumps.
>>

[function:nova_vm_create]
line = 45
end = 89
params = []
returns = NovaVM *
complexity = low

description = <<
Allocates and initializes a new VM instance. Sets up the
global table, string metatable, and initial stack.
>>

[function:nova_vm_execute]
line = 120
end = 890
params = [NovaVM *N]
returns = NovaStatus
complexity = high
+hotspot +dispatch

description = <<
Main execution loop. Fetches instructions from current
call frame, decodes opcode, dispatches via computed-goto.
Contains GC safepoints at backward jumps and function
call boundaries.
>>

[finding:dispatch-size] !low
file = src/nova_vm.c
line = 120
+refactor

description = <<
The dispatch loop spans 770 lines. While computed-goto
requires handlers to be in the same function scope,
consider extracting complex opcode logic into inline
helper functions for readability.
>>
```

### 17.2 Memory Database Export

When the memory system dumps records, each record becomes a section:

```nd
@nova-doc 1
@type = memory-export
@created = 2026-03-04T15:00:00Z
@source = nova-mcp/memory

[memory:naming-convention]
category = convention
created = 2026-03-01
source = codebase
+style +naming

value = <<
Public API uses nova_ prefix. Internal/static functions
use novai_ prefix. Zorya vendored code uses zorya_ prefix.
VM state pointer is always named N.
>>

[memory:build-commands]
category = convention
created = 2026-03-02
source = user
+build +make

value = <<
Build with 'make' for release, 'make DEBUG=1' for debug.
'make lib' builds libnova.a. 'make test' runs all 328
tests across 26 suites.
>>

[memory:gc-invariant]
category = finding
created = 2026-03-04
source = analysis
+gc +safety

value = <<
All objects reachable from the VM root set must be
marked before any sweep phase. The tri-color invariant
requires that no black object points to a white object
at the end of marking.
>>
```

### 17.3 Workspace Tree Snapshot

```nd
@nova-doc 1
@type = tree
@target = /home/user/project
@created = 2026-03-04T14:00:00Z

[tree:.]
type = directory
children = 8

[tree:src]
type = directory
children = 47

[tree:src/nova_vm.c]
type = file
size = 89234
lines = 2847
+core

[tree:src/nova_gc.c]
type = file
size = 12456
lines = 340
+gc

[tree:src/nova_compile.c]
type = file
size = 45678
lines = 1234
+compiler

[tree:tests]
type = directory
children = 26

[tree:docs]
type = directory
children = 12

[tree:Makefile]
type = file
size = 3456
lines = 89
+build
```

### 17.4 Session Log

```nd
@nova-doc 1
@type = session-log
@created = 2026-03-04T13:00:00Z

[log:2026-03-04T13:00:00Z]
level = info
source = user
message = <<
Starting MCP tool redesign. Goal: replace 12 hollow
tools with Nova-specific smart tools.
>>

[log:2026-03-04T13:30:00Z]
level = info
source = nova-mcp/analyze
message = <<
Analyzed existing tools. 12 of 14 duplicate native
Copilot capabilities. Recommended replacement set:
nova_eval, nova_check, nova_disassemble, nova_explain_error,
nova_describe_api, nova_project_info, nova_run_tests.
>>

[log:2026-03-04T14:45:00Z]
level = info
source = user
message = <<
Build succeeded. All 7 tools compiled clean. Binary
size: 350K (was 326K). libnova.a linkage verified.
>>
```

---

## 18. Examples

### 18.1 Minimal Document

```nd
@nova-doc 1
@type = note

[note:reminder]
value = <<
Remember to update the changelog before release.
>>
```

### 18.2 Multi-File Analysis

```nd
@nova-doc 1
@type = analysis
@source = nova-mcp/analyze
@target = src/
@created = 2026-03-04T14:00:00Z
@tags = [full-scan, automated]

[meta]
total_files = 47
total_lines = 46823
languages = [c]

summary = <<
Full source analysis of the Nova VM core. 47 C source
files comprising ~47K lines. The codebase follows
ZORYA-C v2.0.0 conventions consistently. Key hotspots
identified in VM dispatch and GC marking.
>>

[file:src/nova_vm.c]
lines = 2847
functions = 47
+core +vm

summary = <<
VM execution engine and dispatch loop.
>>

[file:src/nova_compile.c]
lines = 1234
functions = 28
+compiler

summary = <<
Compiler front-end. AST to bytecode translation.
>>

[file:src/nova_gc.c]
lines = 340
functions = 12
+gc

summary = <<
Tri-color mark-sweep garbage collector.
>>

[finding:dispatch-hotspot] !normal
file = src/nova_vm.c
line = 120
+performance

description = <<
At 770 lines the dispatch function is the largest in
the codebase but this is by design — computed-goto
requires all handlers in one function scope.
>>

[finding:gc-barrier] !high
file = src/nova_gc.c
line = 89
+correctness

description = <<
Write barrier not triggered when setting metatable
fields via __newindex. Could allow black→white
references during incremental marking.
>>
```

---

## Appendix A: Format Comparison

| Feature | JSON | YAML | TOML | Markdown | NINI | **Nova Doc** |
|---------|------|------|------|----------|------|-------------|
| Machine-parseable | ✓✓✓ | ✓✓ | ✓✓ | ✗ | ✓✓✓ | ✓✓✓ |
| Human-readable | ✓ | ✓✓ | ✓✓ | ✓✓✓ | ✓✓ | ✓✓✓ |
| AI-efficient | ✗ | ✓ | ✓ | ✓✓ | ✓✓ | ✓✓✓ |
| Typed values | ✓ | ✓ | ✓ | ✗ | ✓ | ✓ |
| Prose content | ✗ | ✗ | ✗ | ✓✓✓ | ✗ | ✓✓✓ |
| Checklists | ✗ | ✗ | ✗ | ✓ | ✗ | ✓✓✓ |
| Schema-implied | ✗ | ✗ | ✗ | ✗ | ✗ | ✓ |
| Token overhead | ~40% | ~15% | ~15% | ~5% | ~8% | ~8% |
| Strict grammar | ✓ | ✗ | ✓ | ✗ | ✓ | ✓ |

---

## Appendix B: Design Decisions

### B.1 Why Not Extend Markdown?

Markdown is designed for rendering to HTML, not for data interchange.
Adding structured fields to markdown (e.g., YAML frontmatter) creates
a two-parser problem and still leaves the body unparseable. Nova Doc
is one grammar, one parser, both structure and prose.

### B.2 Why Not Use TOML?

TOML is excellent for configuration but has no concept of prose content,
no document metadata, and its inline table syntax is verbose. Nova Doc
inherits NINI's simpler syntax while adding what TOML lacks.

### B.3 Why `<<` / `>>` Instead of Heredoc?

Heredocs (`<<EOF ... EOF`) require choosing a delimiter that doesn't
appear in the content. `<<` / `>>` are fixed delimiters — simpler to
parse, simpler to type, and the escape case (`\>>`) is rare enough
to be acceptable. The angle-bracket mnemonic also reads naturally as
"begin content" / "end content."

### B.4 Why Tags Instead of Nested Arrays?

Tags (`+vm +core +performance`) are scanned as single tokens by an LLM.
An equivalent JSON array `"tags": ["vm", "core", "performance"]` costs
9 tokens for the same information. Tags also enable grep-style
filtering without parsing: `grep '+critical' analysis.nd`.

---

## Appendix C: File Extension Summary

| Extension | Format | Purpose |
|-----------|--------|---------|
| `.nini` → `.ni` | NINI | Configuration, data interchange, taskfiles |
| `.nd` | Nova Doc | Documentation, analysis, memory, tool output |
| `.n` | Nova | Nova source code |

---

**ZORYA CORPORATION — Engineering Excellence, Democratized**
