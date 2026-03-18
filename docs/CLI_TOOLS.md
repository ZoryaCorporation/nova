<!-- ============================================================
     NOVA CLI TOOLS SPECIFICATION
     Built-In Tool Suite — Architecture and Reference

     Author:  Anthony Taliento (Zorya Corporation)
     Date:    2026-03-13
     Version: 0.2.0
     Status:  RATIFIED
     ============================================================ -->

# Nova CLI Tools Specification

Nova ships with a suite of Unix-style tools built directly into the
runtime. These tools are not external programs invoked through a shell —
they are C functions compiled into the Nova binary, callable both as
command-line subcommands and as in-process library functions from Nova
scripts. The result is a self-contained toolchain with zero subprocess
overhead, no shell dependencies, and no external installs.

These tools are part of Nova's greater mission to provide reliable tools
for developers, engineers, students, and learners of all kinds. We will
continue to expand, develop and enhance this tool suite in future versions.
It is anticipated that there will be many feature requests, however rest
assured that this list will continue to grow. We have a massive number of
unix-style tools to implement, and  entirely new tools are also on the
horizon. These tools are free of cost, available to everyone, everywhere.

This document describes the architecture and implementation of Nova's
tool suite as of v0.2.0: the dispatch system, each tool's behavior and
algorithms, the interactive shell, the NINI task runner, and the
in-process library API. For the formats that the task runner's
`taskfile.nini` uses, see the [NINI Specification](NINI_SPEC.md). For
the data processing tools, see the
[NDP Specification](NDP_SPEC.md).

---

## Table of Contents

1. [Architecture](#1-architecture)
2. [Flag Parsing](#2-flag-parsing)
3. [cat](#3-cat)
4. [ls](#4-ls)
5. [tree](#5-tree)
6. [find](#6-find)
7. [grep](#7-grep)
8. [head](#8-head)
9. [tail](#9-tail)
10. [wc](#10-wc)
11. [pwd](#11-pwd)
12. [run](#12-run)
13. [Task Runner](#13-task-runner)
14. [Interactive Shell](#14-interactive-shell)
15. [In-Process Library API](#15-in-process-library-api)
16. [Glob Matching](#16-glob-matching)
17. [Cross-Platform Layer](#17-cross-platform-layer)
18. [Constants and Limits](#18-constants-and-limits)

---

## 1. Architecture

The tool suite is distributed across a modular architecture with focused
files, each handling a single responsibility:

### Core (linked into `nova` binary)

| File | Lines | Responsibility |
|------|-------|----------------|
| `nova_tools.c` | ~1,250 | Flag parser, tool implementations, registry & dispatch |
| `nova_task.c` | ~850 | NINI task runner (`nova task`) |
| `nova_shell.c` | ~500 | Interactive shell, discovery, pipeline engine |
| `nova_lib_tools.c` | ~810 | In-process library bindings (`tools.cat()`, etc.) |

### Standalone Binaries (`bin/tools/`)

| Binary | Source | Description |
|--------|--------|-------------|
| `ncat` | `tools/ncat.c` | Concatenate and print files |
| `nls` | `tools/nls.c` | List directory contents |
| `ntree` | `tools/ntree.c` | Directory tree visualization |
| `nfind` | `tools/nfind.c` | Find files by pattern |
| `ngrep` | `tools/ngrep.c` | Search text in files |
| `nhead` | `tools/nhead.c` | First N lines of a file |
| `ntail` | `tools/ntail.c` | Last N lines of a file |
| `nwc` | `tools/nwc.c` | Word, line, character count |
| `nwrite` | `tools/nwrite.c` | Write stdin to file |
| `necho` | `tools/necho.c` | Print text to stdout |
| `npwd` | `tools/npwd.c` | Print working directory |

### Shared Library

| File | Lines | Responsibility |
|------|-------|----------------|
| `tools/shared/ntool_common.c` | ~310 | Shared utilities (glob, dir, color, flags) |
| `tools/shared/ntool_common.h` | ~235 | Shared API header |

All standalone tool binaries link against `ntool_common` and the Zorya
PAL. They do **not** link against `libnova.a` — they are self-contained
C99 programs with no VM dependency.

### Dispatch Model

When Nova is invoked as `nova <tool> [args...]`, the main entry point
checks the first argument against a static tool name table:

```c
static const char *novai_tool_names[] = {
    "cat", "ls", "tree", "find", "grep",
    "head", "tail", "wc", "write", "echo",
    "pwd", "task", NULL
};
```

If the argument matches a known tool name, `nova_tool_dispatch()` is
called instead of the compiler/VM pipeline. The dispatcher:

1. Calls `nova_tool_parse_flags()` to extract flags and positional
   arguments from `argv`.
2. Routes to the appropriate tool function (e.g., `nova_tool_cat()`,
   `nova_tool_grep()`).
3. Returns an exit code (0 = success, non-zero = error).

This design means tool invocations never start the compiler, never
allocate a VM, and never load the standard library. The overhead is
effectively that of a direct C function call.

### Dual Interface

Every tool is available through two interfaces:

| Interface | Invocation | Output | Overhead |
|-----------|-----------|--------|----------|
| CLI | `nova cat file.txt` | Printed to stdout/stderr | Process startup only |
| Library | `tools.cat("file.txt")` | Returned as Nova value | Zero (in-process) |

The CLI and library implementations share the same core algorithms but
differ in how they produce output. CLI tools print to file handles
(stdout, stderr, or a redirected output file). Library tools build Nova
values — strings, tables, or integers — and push them onto the VM stack.

---

## 2. Flag Parsing

All tools share a unified flag parsing system through the
`NovaToolFlags` structure:

```c
typedef struct {
    const char *match;         /* -m, --match=PATTERN        */
    int  ignore_case;          /* -i, --ignore-case          */
    int  invert;               /* -v, --invert               */
    int  recursive;            /* -r, --recursive            */
    const char *output;        /* -o, --output=FILE          */
    int  limit;                /* -l, --limit=N  (0 = off)   */
    int  depth;                /* -d, --depth=N  (-1 = unlimited) */
    int  verbose;              /* -V, --verbose              */
    int  show_all;             /* -a, --all                  */
    int  show_numbers;         /* -n, --number               */
    int  show_long;            /* -L, --long                 */
    int  append_mode;          /* -A, --append               */
    int  count_lines;          /* wc: --lines                */
    int  count_words;          /* wc: --words                */
    int  count_chars;          /* wc: --chars                */
    int  dry_run;              /* --dry                      */
    const char *subjects[64];  /* positional arguments       */
    int  subject_count;
} NovaToolFlags;
```

The parser handles both short and long flag forms. Values are specified
either with `=` (`-m=*.c`, `--match=TODO`) or as the next argument
(`-m *.c`). Positional arguments (file paths, directory paths) are
collected into the `subjects` array, up to a maximum of 64.

Each tool reads only the flags relevant to its operation; the rest are
ignored. This one-struct-fits-all approach keeps the parsing code simple
and consistent across tools.

---

## 3. cat

**CLI:** `nova cat <file> [files...] [-n] [-o output] [-A]`
**Library:** `tools.cat(path)` → string

### Implementation

The `cat` tool reads files line by line using `fgets()` into a
fixed-size buffer (`NOVAI_LINE_MAX` = 8,192 bytes). Each line is
written to the output stream as it is read — there is no intermediate
buffering of the entire file.

When line numbering is enabled (`-n`), each line is prefixed with a
right-aligned 6-digit line number (`%6d`). The line counter is
maintained across multiple files, so numbering continues sequentially
when concatenating.

### Output Redirection

The `-o` flag redirects output to a file instead of stdout. Combined
with `-A` (append mode), `cat` opens the output file with `"a"` instead
of `"w"`, enabling log-style concatenation.

### Stdin

When no file arguments are given and stdin is not a terminal (detected
via `isatty()`), `cat` reads from stdin. This enables pipeline usage:

```sh
echo "hello" | nova cat
```

### Library Behavior

The library function `tools.cat(path)` reads the entire file into
memory using `fseek`/`ftell` to determine the size, `malloc` to
allocate a buffer, and a single `fread` to load the contents. It
returns the contents as a Nova string. If the file cannot be opened,
it raises a VM error with the system error message.

---

## 4. ls

**CLI:** `nova ls [dir] [-a] [-L] [-m=pattern] [-i] [-v]`
**Library:** `tools.ls([dir])` → table of `{name, type, size}`

### Implementation

The `ls` tool reads a directory using the platform's directory
enumeration API (`opendir`/`readdir` on POSIX,
`FindFirstFileA`/`FindNextFileA` on Windows). Each entry is collected
into an array of `NovaiDirEntry` structs (up to `NOVAI_DIR_MAX` = 4,096
entries) containing name, type, and metadata.

### Sorting

Entries are sorted via `qsort()` with a two-tier comparator:

1. **Directories first.** All directories sort before all files.
2. **Alphabetical within tier.** Within the directory group and within
   the file group, entries are sorted alphabetically by name.

### Long Listing

The `-L` flag enables detailed output showing modification time,
human-readable file size, and name:

```
2026-03-13 14:30   23.5K  nova_vm.c
2026-03-13 14:30    1.2M  libnova.a
2026-03-13 14:30     dir  src/
```

File sizes are formatted with unit suffixes:

| Range | Format | Example |
|-------|--------|---------|
| < 1 KB | `NB` | `512B` |
| < 1 MB | `N.NK` | `23.5K` |
| < 1 GB | `N.NM` | `1.2M` |
| ≥ 1 GB | `N.NG` | `2.3G` |

Timestamps use `strftime` with the format `%Y-%m-%d %H:%M`.

### Filtering

When the `-m` flag is set, only entries whose name matches the glob
pattern are shown. The `-i` flag makes the match case-insensitive,
and `-v` inverts the match (showing entries that do *not* match).

### Hidden Files

Dotfiles (entries starting with `.`) are hidden by default. The `-a`
flag includes them.

### Library Behavior

`tools.ls([dir])` returns a Nova table where each entry is a sub-table
with three fields: `name` (string), `type` (`"file"` or `"dir"`), and
`size` (integer, bytes). Dotfiles (`.` and `..`) are always excluded.

---

## 5. tree

**CLI:** `nova tree [dir] [-d=N] [-a] [-m=pattern]`
**Library:** `tools.tree([dir, depth])` → string

### Implementation

The `tree` tool performs a depth-first recursive traversal of the
directory hierarchy, building an indented tree display with Unicode
box-drawing characters.

### Box-Drawing Characters

```
├── file.c          TREE_BRANCH  (non-last child)
└── file.h          TREE_CORNER  (last child)
│   ├── inner.c     TREE_PIPE    (continuing parent vertical)
    └── leaf.c      TREE_SPACE   (parent was last child)
```

The tree characters are full UTF-8 sequences:

| Name | Bytes | Visual |
|------|-------|--------|
| `TREE_BRANCH` | `E2 94 9C E2 94 80 E2 94 80 20` | `├── ` |
| `TREE_CORNER` | `E2 94 94 E2 94 80 E2 94 80 20` | `└── ` |
| `TREE_PIPE` | `E2 94 82 20 20 20` | `│   ` |
| `TREE_SPACE` | `20 20 20 20` | `    ` |

### Prefix Building

At each recursion level, the tool builds a prefix string by
concatenating the appropriate box characters from all ancestor levels.
Whether an ancestor was the last child in its directory determines
whether a `TREE_PIPE` (continuing vertical line) or `TREE_SPACE`
(blank) is used.

### Output

The final line of output is a summary:

```
12 directories, 47 files
```

### Depth Limiting

The `-d` flag limits recursion depth. A negative value (the default)
means unlimited depth.

### Library Behavior

`tools.tree([dir, depth])` returns the entire tree as a single string
with embedded newlines, including the summary line.

---

## 6. find

**CLI:** `nova find [dir] -m=<pattern> [-d=N] [-l=N] [-o=file]`
**Library:** `tools.find(dir, pattern [, depth])` → table of paths

### Implementation

The `find` tool performs a recursive depth-first traversal of the
directory hierarchy, testing each filename against a glob pattern using
`novai_glob_match()` (see [Section 16](#16-glob-matching)).

The pattern is matched against the filename only, not the full path.
Matching files are reported with their path relative to the starting
directory.

### Limits

The `-l` flag stops output after N matches. The `-d` flag limits
directory traversal depth.

### Library Behavior

`tools.find(dir, pattern [, depth])` returns a flat Nova table of
matching file paths (strings). The paths are relative to the given
directory.

---

## 7. grep

**CLI:** `nova grep <file|dir> -m=<pattern> [-r] [-n] [-i] [-v] [-l=N]`
**Library:** `tools.grep(pattern, path...)` → table of `{file, num, text}`

### Implementation

The `grep` tool searches files for lines containing a given text
pattern. The matching algorithm is plain text substring search using
`strstr()` (or `novai_strcasestr()` when case-insensitive mode is
enabled). Nova's grep does **not** support regular expressions — it is
a fast literal text searcher.

### Line Processing

Files are read line by line with `fgets()` into a buffer of
`NOVAI_LINE_MAX` (8,192) bytes. Trailing newlines are stripped before
display. Each matching line is printed with optional context:

- **Single file:** `line_content` (or `line_num:line_content` with `-n`)
- **Multi-file or directory:** `filepath:line_num:line_content`

### Recursive Search

The `-r` flag enables recursive directory search. When a directory is
given as the target (without `-r`), grep still searches it recursively
by default. The `novai_grep_dir()` function walks the directory tree
via `opendir`/`readdir`, calling `novai_grep_file()` for each regular
file.

### Flags

| Flag | Effect |
|------|--------|
| `-i` | Case-insensitive matching |
| `-v` | Invert match (show non-matching lines) |
| `-n` | Show line numbers |
| `-l=N` | Stop after N matches |

### Library Behavior

`tools.grep(pattern, path...)` returns a Nova table of match records.
Each record is a sub-table with three fields: `file` (string), `num`
(integer, 1-based line number), and `text` (string, the matching line
with trailing whitespace trimmed).

---

## 8. head

**CLI:** `nova head <file> [--lines=N] [-n]`
**Library:** `tools.head(path [, n])` → string

### Implementation

Reads up to N lines (default: 10) from the beginning of a file using
`fgets()`. When multiple files are specified, each file's output is
preceded by a header:

```
==> filename <==
```

Line numbering (`-n`) prefixes each line with its number.

### Stdin

Like `cat`, `head` reads from stdin when no file arguments are given
and stdin is not a terminal.

### Library Behavior

`tools.head(path [, n])` returns the first N lines as a single string.

---

## 9. tail

**CLI:** `nova tail <file> [--lines=N] [-n]`
**Library:** `tools.tail(path [, n])` → string

### Implementation

The `tail` tool uses a buffered algorithm rather than seeking from the
end of the file. It reads the entire file line by line, storing each
line (via `strdup()`) in a dynamically allocated pointer array. The
array starts with a capacity of 256 entries and grows by 2x doubling
via `realloc()`.

After reading all lines, it computes the starting index:

```c
start = line_count - max_lines;
if (start < 0) start = 0;
```

Then it prints lines from `start` to `line_count - 1` and frees all
line strings and the array.

This approach is simple and correct for any file, though it buffers the
entire file in memory. For the file sizes Nova typically processes
(configuration files, source code, logs), this is practical.

### Multi-File Output

Like `head`, multiple files get `==> filename <==` separators.

### Library Behavior

`tools.tail(path [, n])` returns the last N lines as a single string.

---

## 10. wc

**CLI:** `nova wc <file> [--lines] [--words] [--chars]`
**Library:** `tools.wc(path)` → table `{lines, words, chars}`

### Implementation

The `wc` tool performs a single-pass character-by-character scan using
`fgetc()`:

```
for each character c:
    chars++
    if c == '\n':  lines++
    if c is whitespace:  in_word = 0
    else if not in_word: words++; in_word = 1
```

The word counter uses a two-state machine: transitions from whitespace
to non-whitespace increment the word count.

### Output Format

By default, all three metrics are printed in 7-character right-aligned
fields:

```
    127     892    6543  Makefile
```

The `--lines`, `--words`, and `--chars` flags selectively show only
the requested metrics. When multiple files are given, a `total` line
is appended with the cumulative counts.

### Library Behavior

`tools.wc(path)` returns a single Nova table with three integer fields:
`lines`, `words`, and `chars`.

---

## 11. pwd

**CLI:** `nova pwd`
**Library:** `tools.pwd()` → string

Returns the current working directory via `getcwd()` (POSIX) or
`_getcwd()` (Windows) into a stack buffer of `NOVAI_PATH_MAX`
(4,096) bytes.

---

## 12. run

**Library only:** `tools.run(command)` → string, integer

Executes a shell command via `popen(command, "r")` and captures its
stdout into a dynamically growing string buffer (`NovaiStrBuf`, initial
capacity 4,096 bytes, 2x doubling). The exit code is extracted via
`pclose()` and `WEXITSTATUS` (POSIX).

Returns two values on the VM stack: the captured output (string, with
trailing newline stripped) and the exit code (integer). This enables
patterns like:

```lua
dec output, code = tools.run("make -n")
if code != 0 then
    error("build simulation failed: " .. output)
end
```

`tools.run` is not available as a CLI subcommand — it exists only in
the library API because its purpose is programmatic command execution.

---

## 13. Task Runner

The task runner is a NINI-powered build orchestrator invoked via
`nova task [name...]`. It reads task definitions from a `taskfile.nini`
file, resolves dependencies, manages environment variables, and
executes commands.

### Taskfile Discovery

When `nova task` is invoked, the runner searches for `taskfile.nini`
starting in the current directory and walking up to 16 parent
directories. Once found, it changes to the taskfile's directory so
that all relative paths in the file resolve correctly.

### Task Schema

Tasks are defined as NINI task sections:

```ini
[task:build]
command     = make                  # single command
cmds[]      = step1                 # or: multi-command sequence
cmds[]      = step2
description = Build the project     # shown in task listing
depends     = [clean, check]        # run these first
dir         = /path/to/workdir      # change directory before running
shell       = /bin/bash             # shell to use (default: sh)
silent      = false                 # suppress command echo
ignore_error = true                 # continue on failure
platforms   = [linux, macos]        # skip on other platforms
timeout     = 60                    # command timeout in seconds
pre         = setup_task            # run before main command
post        = cleanup_task          # run after main command (on success)
env.CC      = gcc                   # set environment variable
env.CFLAGS  = -O2
dotenv      = .env                  # load KEY=VALUE file
```

### Execution Model

For each task, the runner:

1. **Checks depth.** If recursion exceeds 32 levels, the task is
   rejected as a cycle.
2. **Checks the visited set.** A static array of up to 128 task names
   tracks which tasks have already been executed in this run. Diamond
   dependencies (A→B→D, A→C→D) execute D only once.
3. **Checks the platform filter.** If `platforms` is set and the
   current platform is not in the list, the task is silently skipped.
4. **Loads the `.env` file** (if specified). Parses KEY=VALUE lines,
   strips quotes, and sets environment variables.
5. **Sets environment variables.** Task-level `env.KEY = value` entries
   are applied with backup for post-task restoration.
6. **Changes directory** (if `dir` is set) with backup.
7. **Runs dependency tasks** in declaration order, recursively.
8. **Runs the pre-hook** (if set).
9. **Executes the command** (or iterates `cmds[]` if present). Each
   command is executed via `system()`. Non-zero exit codes stop
   execution unless `ignore_error` is set.
10. **Runs the post-hook** (if the command succeeded).
11. **Restores** the original directory and environment variables.

### Dependency Resolution

Dependencies form a directed acyclic graph. The runner uses depth-first
traversal with two safeguards:

- **Cycle detection:** A recursion depth limit of 32 levels. Any
  task chain exceeding this depth is reported as a potential cycle.
- **Diamond deduplication:** The visited-set array (128 entries)
  prevents re-executing a task that was already run during this
  invocation, even if it appears in multiple dependency chains.

### Color Output

When the terminal supports color (detected at startup), the task
runner uses ANSI escape sequences for visual feedback:

| Event | Symbol | Color |
|-------|--------|-------|
| Task start | `▸` | Hunter green, bold |
| Command echo | `$` | Muted gray |
| Task success | `✓` | Green |
| Task failure | `✗` | Red |

### Task Listing

`nova task` with no arguments prints a formatted list of all defined
tasks with their descriptions (if present).

---

## 14. Interactive Shell

When Nova is invoked with no arguments (`nova`), it enters an
interactive tool shell — a minimal command-line environment for running
tools without the `nova` prefix. The shell is implemented in
`nova_shell.c` with a discovery-based dispatch system.

### Tool Discovery

On startup, the shell scans for tool binaries in these locations:

1. `$NOVA_TOOLS_DIR` (if set)
2. `<nova-binary-dir>/tools/`
3. `~/.nova/tools/`
4. `/usr/local/lib/nova/tools/`

Any executable file starting with the `n` prefix is registered.
Discovery is lazy — the scan happens once at shell initialization.

### Prompt

```
nova$
```

The prompt uses ANSI color when available: `nova` in green, `$` in
emerald.

### Tokenization

User input is tokenized by a custom splitter that handles:

- **Quoted strings:** Both single and double quotes. Quotes can appear
  mid-token: `-m="*.c"` is a single token.
- **In-place modification:** The tokenizer modifies the input buffer
  directly, replacing delimiters with NUL bytes.
- **Maximum tokens:** 128 per command line.

### Pipeline Support

The shell supports Unix-style pipes between tools:

```
nova$ cat README.md | grep TODO
nova$ ls src/ | grep .c
```

Pipes are implemented via `tmpfile()` and `dup2()`:

1. Split the command line on unquoted `|` characters (up to 16 stages).
2. For each stage, save the current stdin/stdout via `dup()`.
3. Redirect stdin from the previous stage's output file.
4. Redirect stdout to a new temp file.
5. Execute the tool.
6. Restore stdin/stdout via `dup2()`.
7. The output temp file becomes the next stage's input.

Stdin is set to unbuffered mode (`setvbuf(stdin, NULL, _IONBF, 0)`)
to ensure `dup2()` redirection works correctly with the C standard
library's buffered I/O.

### Built-In Commands

The shell recognizes several built-in commands that are not piped
through the tool dispatcher:

| Command | Action |
|---------|--------|
| `cd [dir]` | Change directory. Supports `~` expansion (checks `HOME`, then `USERPROFILE`). |
| `exit` / `quit` | Leave the shell. |
| `help` | Print the list of available tools. |
| `clear` | Clear the terminal screen. |
| `version` | Print the Nova version string. |

---

## 15. In-Process Library API

The `tools` module exposes all tools as Nova functions, callable from
scripts via `require("tools")` or direct access:

```lua
dec content  = tools.cat("file.txt")           -- string
dec entries  = tools.ls("src/")                 -- table[{name, type, size}]
dec tree_str = tools.tree(".", 3)               -- string
dec files    = tools.find("src/", "*.c")        -- table[paths]
dec matches  = tools.grep("TODO", "src/vm.c")   -- table[{file, num, text}]
dec top      = tools.head("log.txt", 20)        -- string
dec bottom   = tools.tail("log.txt", 20)        -- string
dec counts   = tools.wc("Makefile")             -- {lines, words, chars}
dec cwd      = tools.pwd()                      -- string
dec out, rc  = tools.run("make test")           -- string, integer
```

### String Buffer

Several library tool functions (head, tail, tree, run) accumulate
output into a dynamic string buffer:

```c
typedef struct {
    char  *data;
    size_t len;
    size_t cap;
} NovaiStrBuf;
```

Initial capacity is 4,096 bytes, growing by 2x doubling. The buffer
is converted to a Nova string and pushed onto the VM stack when the
tool function returns.

### Error Handling

Library tool functions report errors by raising VM errors
(`nova_vm_raise_error`), which can be caught with Nova's `pcall` or
`xpcall`. Error messages include the tool name and the system error
string:

```
tools.cat: cannot open 'missing.txt': No such file or directory
```

### Return Values

| Tool | Returns | Stack Effect |
|------|---------|-------------|
| `cat` | string | 1 value |
| `ls` | table | 1 value |
| `tree` | string | 1 value |
| `find` | table | 1 value |
| `grep` | table | 1 value |
| `head` | string | 1 value |
| `tail` | string | 1 value |
| `wc` | table | 1 value |
| `pwd` | string | 1 value |
| `run` | string, integer | 2 values |

---

## 16. Glob Matching

The `find` and `ls` tools use a simple glob matching algorithm
implemented in `novai_glob_match()`.

### Supported Wildcards

| Pattern | Meaning |
|---------|---------|
| `*` | Match zero or more characters |
| `?` | Match exactly one character |

### Not Supported

Character classes (`[abc]`), brace expansion (`{a,b}`), and POSIX
character classes (`[:alpha:]`) are not implemented. The glob matcher
handles the two wildcards that cover the vast majority of file-matching
use cases.

### Algorithm

The matcher uses a simple backtracking algorithm:

1. Walk the pattern and string in parallel.
2. On `?`, advance both pointers (one character consumed).
3. On `*`, record the pattern position and the current string position
   as a "star checkpoint."
4. On a literal character match, advance both pointers.
5. On a mismatch, if a star checkpoint exists, backtrack: advance the
   string pointer from the checkpoint and retry.
6. After the pattern is exhausted, skip any trailing `*` characters.
7. Match succeeds if both pattern and string are fully consumed.

### Case-Insensitive Matching

`novai_glob_match_ci()` creates lowercase copies of both the pattern
and the string into 256-byte stack buffers, then calls the standard
`novai_glob_match()` on the copies.

---

## 17. Cross-Platform Layer

The tool suite abstracts platform differences through inline wrapper
functions:

| Function | POSIX | Windows |
|----------|-------|---------|
| `novai_isatty()` | `isatty()` | `_isatty()` |
| `novai_fileno()` | `fileno()` | `_fileno()` |
| `novai_getcwd()` | `getcwd()` | `_getcwd()` |
| `novai_chdir()` | `chdir()` | `_chdir()` |
| `novai_dup()` | `dup()` | `_dup()` |
| `novai_dup2()` | `dup2()` | `_dup2()` |
| `novai_fdclose()` | `close()` | `_close()` |
| `novai_is_directory()` | `stat()` + `S_ISDIR()` | `GetFileAttributesA()` |
| `novai_file_size()` | `stat().st_size` | `GetFileAttributesEx()` |
| `novai_file_mtime()` | `stat().st_mtime` | `_stat().st_mtime` |

Directory enumeration uses `opendir`/`readdir`/`closedir` on POSIX
and `FindFirstFileA`/`FindNextFileA`/`FindClose` on Windows. File
metadata is gathered via `stat()` on POSIX and the Win32 API on
Windows.

---

## 18. Constants and Limits

| Constant | Value | Purpose |
|----------|-------|---------|
| `NOVAI_PATH_MAX` | 4,096 bytes | Maximum file path length |
| `NOVAI_LINE_MAX` | 8,192 bytes | Maximum single line buffer |
| `NOVAI_DIR_MAX` | 4,096 entries | Maximum directory entries per read |
| `NOVAI_SHELL_LINE_MAX` | 4,096 bytes | Shell command input buffer |
| `NOVAI_SHELL_MAX_ARGS` | 128 tokens | Maximum tokens per shell command |
| `NOVAI_PIPE_MAX_STAGES` | 16 stages | Maximum pipeline depth |
| `NOVAI_TASK_MAX_DEPTH` | 32 levels | Task dependency recursion limit |
| `NOVAI_TASK_MAX_VISITED` | 128 tasks | Deduplication set size |
| `NOVAI_TASK_MAX_SEARCH` | 16 levels | Parent directory search for taskfile |
| `NOVA_TOOL_MAX_SUBJECTS` | 64 entries | Maximum positional arguments |
| `NOVAI_TL_BUF_INIT` | 4,096 bytes | Library string buffer initial size |
| Tail line array initial capacity | 256 entries | Dynamic, grows 2x |
| Glob case-insensitive buffer | 256 bytes | Stack-allocated copy |

---

*See also: [NDP Specification](NDP_SPEC.md) · [NINI Specification](NINI_SPEC.md) · [VM Specification](NOVA_VM_SPEC.md)*
