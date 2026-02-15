# Nova Language

**A lightweight, embeddable scripting language by Zorya Corporation.**

Nova preserves Lua's elegant syntax and philosophy while adding the features
that real-world projects demand: a preprocessor, cross-platform bytecode,
and a clean C API. Built from scratch -- not a fork.

## Key Features

- **Preprocessor**: `#include`, `#define`, function-like macros, conditionals
- **Portable Bytecode**: `.no` files work on any platform (not tied to endianness or word size)
- **Clean C API**: Type-safe, explicit, embeddable
- **0-Indexed Arrays**: Sane default (configurable)
- **Coroutines**: Stackful cooperative multitasking (create, resume, yield, wrap)
- **Garbage Collector**: Tri-color incremental mark-and-sweep
- **Metatables**: Full Lua-compatible metamethod system
- **Powered by Zorya SDK**: NXH (hashing), DAGGER (hash tables), Weave (strings), PCM (macros)

## File Extensions

| Extension | Purpose |
|-----------|---------|
| `.n`  | Nova source file |
| `.m`  | Nova macro/header file |
| `.no` | Nova Object (compiled bytecode) |
| `.lua`| Legacy Lua source (compatibility) |

## Quick Start

```bash
# Build
cd nova && make

# Run a script
./bin/nova examples/hello.n

# Compile to bytecode
./bin/nova -c examples/hello.n    # produces hello.no

# Run bytecode
./bin/nova examples/hello.no
```

## Example

```lua
#include "math_utils.m"

#define MAX_ENTITIES 1024

local entities = {}

function spawn(x, y)
    if #entities >= MAX_ENTITIES then
        return nil, "limit reached"
    end
    local e = { x = x, y = y, angle = DEG2RAD(45) }
    entities[#entities] = e
    return e
end

for i = 0, 9 do
    spawn(i * 10, 0)
end

print("Spawned " .. tostring(#entities) .. " entities")
```

## Building

Requires GCC (or Clang) and the Zorya C SDK (included in parent directory).

```bash
make            # Release build
make DEBUG=1    # Debug build (AddressSanitizer + assertions)
make test       # Run tests
make lib        # Build libnova.a for embedding
make install    # Install to /usr/local
```

## Embedding

```c
#include <nova/nova.h>

int main(void) {
    NovaState *N = nova_open();
    nova_open_libs(N);
    
    if (nova_load_file(N, "app.n") == NOVA_OK) {
        nova_pcall(N, 0, 0);
    }
    
    nova_close(N);
    return 0;
}
```

## Architecture

```
Source (.n/.m) → Lexer → Preprocessor → Parser → AST → Compiler → Bytecode (.no) → VM
```

See [docs/NOVA_DESIGN.md](docs/NOVA_DESIGN.md) for the complete design document.

## License

MIT License - Copyright (c) 2026 Zorya Corporation

## Credits

- **Anthony Taliento** - Language design and implementation
- **Zorya Corporation** - Engineering infrastructure
- Inspired by the elegance of Lua (PUC-Rio)
