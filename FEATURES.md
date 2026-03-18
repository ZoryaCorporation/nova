# Nova Feature Inventory

This document is the canonical record of Nova's features organized by version.
It serves as a structured capability reference — not a raw changelog, but a
clear inventory of what Nova can do and when each capability was introduced.

For detailed release notes see the [CHANGELOG](CHANGELOG.md) (coming soon).
For the language syntax reference see [nova_syntax/NOVA_GUIDE.md](nova_syntax/NOVA_GUIDE.md).

---

## v0.2.0 — Current Release

### Language

| Feature | Description |
|---------|-------------|
| **0-indexed arrays** | All arrays and `string.sub` / `string.find` are 0-indexed |
| **NaN-boxing** | All values packed into 64-bit doubles — integers, floats, booleans, nil, and heap objects share a single 8-byte slot |
| **Dec / global scoping** | Lexical scoping with `dec` keyword; globals stored in the VM environment table |
| **Closures** | Functions capture upvalues from enclosing scopes; upvalues migrate from stack to heap automatically on close |
| **Multiple return values** | Functions can return multiple values; destructuring assignment supported |
| **Varargs** | `function f(...)` with `...` expansion |
| **String interpolation** | Backtick strings with `${expr}` interpolation: `` `Hello, ${name}!` `` |
| **Metatables** | Full metamethod dispatch: `__index`, `__newindex`, `__add`, `__sub`, `__mul`, `__div`, `__mod`, `__unm`, `__eq`, `__lt`, `__le`, `__len`, `__concat`, `__tostring`, `__call` |
| **String method syntax** | All strings share a metatable; `s:upper()`, `s:find()`, `s:gsub()` etc. work natively |
| **goto / labels** | `goto label` and `::label::` for structured jumps |
| **Coroutines** | `coroutine.create`, `coroutine.resume`, `coroutine.yield`, `coroutine.wrap`, `coroutine.status` |
| **Async / await** | `async function`, `await`, `spawn` keywords built into the language — coroutine-backed, no callbacks |
| **Task system** | `task.run()`, `task.spawn()`, `task.sleep()`, `task.status()`, `task.wrap()` |
| **Error handling** | `error()`, `pcall()`, `xpcall()` with traceback support |
| **Modules** | `require()` with `package.path` search, `package.loaded` cache |
| **Preprocessor** | `#import <module>` for standard library modules; `#include "file"` for source inclusion |

### Standard Library

| Module | Import | Key Functions |
|--------|--------|---------------|
| **base** | (global) | `echo`, `print`, `printf`, `sprintf`, `fprintf`, `type`, `tostring`, `tonumber`, `error`, `assert`, `pcall`, `xpcall`, `pairs`, `ipairs`, `next`, `select`, `rawget`, `rawset`, `rawequal` |
| **math** | (global) | `math.abs`, `ceil`, `floor`, `sqrt`, `sin`, `cos`, `tan`, `atan`, `log`, `exp`, `max`, `min`, `random`, `randomseed`, `pi`, `huge`, `maxinteger` |
| **string** | (global) | `string.len`, `sub`, `upper`, `lower`, `rep`, `find`, `format`, `gsub`, `match`, `gmatch`, `byte`, `char`, `reverse` |
| **table** | (global) | `table.insert`, `remove`, `sort`, `concat`, `move`, `pack`, `unpack` |
| **io** | (global) | `io.open`, `close`, `read`, `write`, `lines`, `stdin`, `stdout`, `stderr` |
| **os** | (global) | `os.execute`, `capture`, `getenv`, `setenv`, `unsetenv`, `env`, `clock`, `time`, `date`, `difftime`, `cwd`, `chdir`, `sleep`, `platform`, `arch`, `hostname`, `homedir`, `tmpdir`, `pid`, `which`, `remove`, `rename`, `tmpname`, `setlocale`, `exit` |
| **coroutine** | (global) | `coroutine.create`, `resume`, `yield`, `wrap`, `status`, `running` |
| **fs** | (global) | `fs.read`, `write`, `append`, `lines`, `exists`, `isfile`, `isdir`, `islink`, `size`, `mtime`, `stat`, `list`, `walk`, `find`, `glob`, `mkdir`, `mkdirs`, `rmdir`, `remove`, `copy`, `move`, `join`, `basename`, `dirname`, `ext`, `stem`, `abspath`, `realpath`, `normalize`, `chmod`, `touch`, `tempdir` |
| **task** | (global) | `task.run`, `spawn`, `sleep`, `status`, `wrap` |
| **debug** | (global) | `debug.traceback`, `getinfo`, `getlocal`, `sethook` |
| **nlp** | (global) | `nlp.tokenize`, `stem`, `fuzzy`, `freq`, `tfidf`, `ngrams`, `is_stopword` |
| **tools** | (global) | `tools.cat`, `ls`, `tree`, `find`, `grep`, `head`, `tail`, `wc`, `pwd`, `run` |
| **net** | `#import net` | `net.get`, `post`, `put`, `delete`, `patch`, `head`, `request`, `url_encode`, `url_decode` |
| **sql** | `#import sql` | `sql.open`, `close`, `exec`, `query`, `insert`, `tables`, `columns`, `begin`, `commit`, `rollback`, `last_insert_id`, `changes`, `escape` |
| **data/json** | `#import json` | `json.decode`, `encode`, `load`, `save` |
| **data/csv** | `#import csv` | `csv.decode`, `encode`, `load`, `save` (TSV supported via delimiter option) |
| **data/nini** | `#import nini` | `nini.decode`, `encode`, `load`, `save` |
| **data/toml** | `#import toml` | `toml.decode`, `encode`, `load`, `save` |
| **data/yaml** | `#import yaml` | `yaml.decode`, `encode`, `load`, `save` |
| **data/html** | `#import html` | `html.decode`, `load` (parse HTML → table tree; text-only mode available) |

### VM & Runtime

| Feature | Description |
|---------|-------------|
| **Register-based VM** | 32-bit fixed-width instructions; computed-goto dispatch (no interpreter overhead) |
| **Mark-and-sweep GC** | Tri-color incremental collection; per-type metatables rooted in the VM struct |
| **DAGGER hash tables** | Open-addressing hash tables with NXH64 hash; O(1) average lookup |
| **Weave string interning** | All strings interned at allocation; equality is pointer comparison |
| **NaN-boxing value repr** | 8 bytes per value for all types including heap object pointers |
| **Upvalue migration** | Upvalues live on the stack until their scope closes, then migrate to heap |
| **Per-type metatables** | String, number, boolean, and nil types each have a shared metatable slot in the VM |

### Compiler

| Feature | Description |
|---------|-------------|
| **Single-pass compiler** | Tokens → bytecode in one pass; no intermediate AST for most constructs |
| **Constant folding** | Arithmetic on literal values resolved at compile time |
| **Register allocation** | Linear register allocation with reuse |
| **Preprocessor** | `#import`, `#include`, macro-like directives resolved before compilation |
| **OP_SELF** | Dedicated opcode for method calls (`obj:method()`) with single table lookup |

### Tooling

| Tool | How to use | Description |
|------|-----------|-------------|
| **nova** | `nova script.n` | Run a Nova script |
| **nova task** | `nova task [name]` | NINI task runner |
| **nova cat** | `nova cat file` | Print file contents |
| **nova ls** | `nova ls [dir]` | List directory |
| **nova tree** | `nova tree [dir]` | Directory tree |
| **nova find** | `nova find [dir] -m=*.n` | Find files by pattern |
| **nova grep** | `nova grep [file] -m=pattern` | Search text |
| **nova head** | `nova head file` | First N lines |
| **nova tail** | `nova tail file` | Last N lines |
| **nova wc** | `nova wc file` | Line/word/char counts |
| **Interactive shell** | `nova` (no args) | Interactive tool shell |

### NINI Configuration Format

Nova's native configuration format. Used for `taskfile.nini` and general
application config. Full spec: [docs/NINI_SPEC.md](docs/NINI_SPEC.md).

| Feature | Example |
|---------|---------|
| Auto-typed scalar values | `port = 8080`, `debug = true`, `name = hello` |
| Array push syntax | `items[] = one` |
| Inline arrays | `tags = [a, b, c]` |
| Variable interpolation | `path = ${section.key}/sub` |
| Task sections | `[task:build]` → stored in `__tasks.build` |
| Task dependencies | `depends = [clean, test]` |
| Comments | `# comment` or `; comment` |

### NDP — Nova Data Processing

Nova's unified multi-format data processing engine. Each format is a separate
import — the underlying codec is the same NDP engine. All codecs produce and
consume standard Nova tables, so data flows freely between formats.

| Format | Import | Read | Write |
|--------|--------|------|-------|
| JSON | `#import json` | `json.decode` / `json.load` | `json.encode` / `json.save` |
| CSV / TSV | `#import csv` | `csv.decode` / `csv.load` | `csv.encode` / `csv.save` |
| INI | `#import nini` | `nini.decode` / `nini.load` | `nini.encode` / `nini.save` |
| NINI | `#import nini` | `nini.decode` / `nini.load` | `nini.encode` / `nini.save` |
| TOML | `#import toml` | `toml.decode` / `toml.load` | `toml.encode` / `toml.save` |
| YAML | `#import yaml` | `yaml.decode` / `yaml.load` | `yaml.encode` / `yaml.save` |
| HTML | `#import html` | `html.decode` / `html.load` | — (read-only; text-only mode available) |
| SQLite | `#import sql` | `sql.query` | `sql.exec` / `sql.insert` |



---

*This document is updated with each release. Last updated: v0.2.0*
