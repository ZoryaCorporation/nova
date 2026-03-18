# Nova Syntax — Examples, Guides & Templates

Welcome to the **Nova Syntax** folder — your playground for learning, writing,
and mastering Nova.

## What's Here

```
nova_syntax/
├── NOVA_GUIDE.md           # The Nova Language Guide (beginner → expert)
├── COPILOT_GUIDE.md        # AI Assistant Guide for writing Nova
├── README.md               # You are here
│
├── 01_basics/              # Beginner: variables, types, functions
│   ├── hello_nova.n        #   Your very first Nova program
│   ├── variables.n         #   Types, locals, globals, scope
│   └── functions.n         #   Defining and calling functions
│
├── 02_collections/         # Beginner: tables, arrays, iteration
│   ├── arrays.n            #   0-indexed arrays and iteration
│   ├── dictionaries.n      #   Key-value tables
│   └── nested_tables.n     #   Tables inside tables
│
├── 03_control_flow/        # Intermediate: loops, conditionals, patterns
│   ├── conditionals.n      #   if/elseif/else patterns
│   ├── loops.n             #   for, while, repeat, break, continue
│   └── error_handling.n    #   pcall, xpcall, error recovery
│
├── 04_real_programs/       # Intermediate: complete mini-programs
│   ├── todo_list.n         #   Task manager with tables
│   ├── word_counter.n      #   File word-frequency analyzer
│   └── contact_book.n      #   Searchable contact directory
│
├── 05_advanced/            # Advanced: closures, metatables, async, coroutines
│   ├── closures.n          #   Closures and upvalues
│   ├── oop_with_meta.n     #   OOP via metatables
│   ├── type_declarations.n #   Enums, structs & type aliases
│   ├── async.n             #   Async/await and the task system
│   └── pipelines.n         #   Coroutine-based data pipelines
│
└── 06_templates/           # Ready-to-use project templates
    ├── cli_tool.n          #   Command-line tool skeleton
    ├── data_processor.n    #   CSV/JSON ETL pipeline
    └── text_analyzer.n     #   NLP analysis template
```

## The Nova Philosophy

Nova looks like Lua but thinks differently:

- **0-indexed** — Arrays start at 0, `string.sub` starts at 0
- **`echo`** for plain messages, **`printf`** for formatted output
- **Clean function bodies** — echo your intent at the top, logic in the middle,
  formatted output at the bottom
- **`#import`** for standard modules, **`require()`** for user modules
- **Backtick interpolation** — `` `Hello, ${name}!` ``

## Quick Start

```bash
# Run any example
./bin/nova nova_syntax/01_basics/hello_nova.n

# Run them all
for f in nova_syntax/*/*.n; do echo "--- $f ---"; ./bin/nova "$f"; done
```

---

**ZORYA CORPORATION — Engineering Excellence, Democratized**
