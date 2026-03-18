# Nova SDK — VS Code Extension

**The complete development kit for the Nova programming language.**

Nova SDK makes Nova a first-class citizen in VS Code with syntax highlighting,
file icons, color themes, task integration, a dedicated terminal, and rich
code snippets — by Zorya Corporation.

---

## Features

### Syntax Highlighting
- **Nova** (`.n`, `.m`) — Full TextMate grammar covering all keywords, operators,
  preprocessor directives (`#import`, `#define`, `#ifdef`), async/await/spawn,
  string interpolation with `${expr}`, C-style format functions, all standard
  library modules, metamethods, and more.
- **NINI** (`.nini`, `.ni`) — Syntax highlighting for Nova's native config format
  including sections, `[task:name]` definitions, typed values, inline arrays,
  variable interpolation (`${section.key}`), and comments.
- **Taskfile** (`taskfile.nini`) — Recognized as a distinct language with its own icon.

### File Icons
| Extension | Icon | Description |
|-----------|------|-------------|
| `.n`      | **N** (green) | Nova source file |
| `.no`     | **NO** (green) | Compiled Nova object |
| `.m`      | **M** (green) | Nova header/macro file |
| `.nini`   | **NINI** (green) | NINI config file |
| `taskfile.nini` | **T** (green) | NINI taskfile |

### Color Themes
- **Nova Phosphor** — Dark theme inspired by old-school phosphor green terminals.
  Monochrome green palette with carefully differentiated shades for keywords,
  strings, functions, comments, and operators.
- **Nova Phosphor (High Contrast)** — High contrast variant with pure black
  background and brighter greens.

### Commands
| Command | Keybinding | Description |
|---------|------------|-------------|
| `Nova: Run Current File` | `Ctrl+Shift+N` | Execute the active `.n` file |
| `Nova: Compile Current File` | — | Compile to `.no` bytecode |
| `Nova: Run Task` | — | Pick and run a task from `taskfile.nini` |
| `Nova: Open Terminal` | — | Launch Nova's interactive tool shell |
| `Nova: Disassemble Current File` | — | Show bytecode disassembly |
| `Nova: Show AST` | — | Dump the abstract syntax tree |
| `Nova: Explain Error Code` | — | Look up error code details |
| `Nova: Show Version` | — | Display Nova version info |

### Task Integration
The extension auto-detects `taskfile.nini` in your workspace and registers
all `[task:name]` sections as VS Code tasks. Tasks appear in the task runner
(`Ctrl+Shift+B`) with proper grouping (build/test/clean).

### Nova Terminal
A dedicated terminal profile that launches Nova's interactive tool shell.
Access it from the terminal dropdown or the status bar icon.

### Snippets
40+ snippets for Nova and NINI covering:
- Functions, control flow, loops, error handling
- Classes (metatable OOP pattern), coroutines, async
- `echo`, `printf`, file I/O, JSON encode/decode
- Preprocessor directives
- NINI sections, tasks, taskfile templates

### Settings
| Setting | Default | Description |
|---------|---------|-------------|
| `nova.executablePath` | `"nova"` | Path to the Nova binary |
| `nova.runArgs` | `[]` | Extra arguments for script execution |
| `nova.optimizationLevel` | `"-O1"` | Compilation optimization level |
| `nova.stripDebugInfo` | `false` | Strip debug info on compile |
| `nova.taskfile.autoDetect` | `true` | Auto-detect taskfile.nini |
| `nova.taskfile.path` | `"taskfile.nini"` | Taskfile location |
| `nova.trace.enabled` | `false` | Enable VM tracing |
| `nova.trace.channels` | `"all"` | Trace channels to enable |
| `nova.terminal.shellIntegration` | `true` | Use Nova shell as terminal |

---

## Quick Start

1. Install the extension
2. Open a workspace containing `.n` files
3. Press `Ctrl+Shift+N` to run the active file
4. Select **Nova Phosphor** from the color theme picker
5. Enable **Nova File Icons** from the icon theme picker

---

## Requirements

- [Nova](https://github.com/zorya-corporation/nova) v0.2.0+ installed and on PATH
- VS Code 1.85.0+

---

## Building from Source

```bash
cd nova-sdk
npm install
npm run compile
```

To test in a development instance:
- Press `F5` in VS Code (with this folder open) to launch the Extension Host.

To package:
```bash
npm run package    # Creates nova-sdk-0.2.0.vsix
```

---

## Roadmap

- [ ] LSP server (diagnostics, go-to-definition, completions, hover)
- [ ] MCP server for GitHub Copilot integration (Nova tools, NDP, taskrunner)
- [ ] Debugger adapter (breakpoints, step-through, variable inspection)
- [ ] Webview settings UI (Nova home base in VS Code)
- [ ] Customizable theme generator
- [ ] Code lens for test results
- [ ] Notebook support for Nova scripts

---

**Zorya Corporation** — Engineering Excellence, Democratized.
