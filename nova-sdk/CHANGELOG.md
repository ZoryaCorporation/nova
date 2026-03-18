# Changelog

## [0.2.0] - 2026-03-04

### Added
- Complete TextMate grammar for Nova (.n, .m) covering all language features
  - Preprocessor directives (#import, #define, #ifdef, #ifndef, #if, #else, #endif, #undef, #pragma, #error, #warning)
  - async/await/spawn keywords
  - C-style block comments (/* ... */)
  - All standard library modules (fs, tools, net, sql, nlp, data codecs, etc.)
  - echo as Nova's preferred output function
  - fprintf in the format function family
  - String interpolation with ${expr} in backtick strings
- TextMate grammar for NINI (.nini, .ni)
  - Sections, task sections, key-value pairs
  - Typed values (auto-detect int, float, bool, nil)
  - Inline arrays, array push syntax
  - Variable interpolation ${section.key}
  - @include directive
  - Multi-line strings (triple quotes)
- File icons: N (source), NO (compiled), M (header), T (taskfile), NINI (config)
- Nova Phosphor dark color theme (phosphor green CRT aesthetic)
- Nova Phosphor High Contrast color theme
- Nova icon theme for file explorer
- Extension runtime with commands:
  - Run, Compile, Disassemble, Show AST, Explain Error, Show Version
  - Run Task (with taskfile.nini auto-detection)
  - Open Nova Terminal (interactive tool shell)
- Task provider (auto-detect taskfile.nini, register tasks in VS Code)
- Terminal profile provider (Nova shell)
- Status bar integration
- 40+ snippets for Nova and NINI
- Language configurations with proper indentation, folding, brackets
- Problem matchers for Nova compiler and runtime errors
- Settings for executable path, optimization level, trace channels, etc.

## [0.1.0] - 2026-02-15

### Added
- Initial basic syntax highlighting for .n files
- Green N file icons (dark/light variants)
- Basic language configuration
