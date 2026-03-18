/**
 * @file nova_tools.h
 * @brief Nova Language - CLI Tools Interface
 *
 * Compiled C implementations of common CLI tools (cat, ls, tree,
 * find, grep, head, tail, wc, write). Built on cross-platform
 * primitives — no POSIX-only headers required.
 *
 * Command structure:  nova [tool] [subject...] [flags]
 * Universal flags:    -m/--match, -i/--ignore-case, -v/--invert,
 *                     -r/--recursive, -o/--output, -l/--limit,
 *                     -d/--depth, -V/--verbose
 *
 * @author Anthony Taliento
 * @date 2026-02-17
 * @version 0.2.0
 *
 * @copyright Copyright (c) 2026 Zorya Corporation
 * @license MIT
 *
 * ZORYA-C COMPLIANCE: v2.0.0
 */

#ifndef NOVA_TOOLS_H
#define NOVA_TOOLS_H

#include <stdio.h>

/* ============================================================
 * TOOL FLAG STRUCTURE
 * ============================================================ */

/**
 * @brief Maximum number of positional arguments (subjects).
 */
#define NOVA_TOOL_MAX_SUBJECTS 64

/**
 * @brief Parsed flags shared across all tools.
 *
 * Universal flags have consistent meaning everywhere.
 * Tool-specific flags are only checked by relevant tools.
 */
typedef struct {
    /* === Universal flags === */
    const char *match;          /**< -m, --match: filter/search pattern  */
    int         ignore_case;    /**< -i, --ignore-case                   */
    int         invert;         /**< -v, --invert: invert filter         */
    int         recursive;      /**< -r, --recursive                     */
    const char *output;         /**< -o, --output: output file path      */
    int         limit;          /**< -l, --limit: max results (0=none)   */
    int         depth;          /**< -d, --depth: max dir depth (-1=inf) */
    int         verbose;        /**< -V, --verbose                       */

    /* === Tool-specific flags === */
    int         show_all;       /**< -a, --all: show hidden/dotfiles     */
    int         show_numbers;   /**< -n, --number: prefix line numbers   */
    int         show_long;      /**< -L, --long: detailed listing        */
    int         append_mode;    /**< -A, --append: append to output      */
    int         count_lines;    /**< --lines: wc lines only              */
    int         count_words;    /**< --words: wc words only              */
    int         count_chars;    /**< --chars: wc chars only              */
    int         dry_run;        /**< --dry: show commands without running */

    /* === Positional arguments === */
    const char *subjects[NOVA_TOOL_MAX_SUBJECTS];
    int         subject_count;
} NovaToolFlags;

/* ============================================================
 * FLAG PARSER
 * ============================================================ */

/**
 * @brief Parse tool arguments into a NovaToolFlags structure.
 *
 * Handles both short (-m) and long (--match) flags, with
 * value syntax via = or space separation.
 *
 * @param argc  Number of arguments (tool args only, not "nova")
 * @param argv  Argument strings
 * @param flags Output flags structure (zeroed and filled)
 * @return 0 on success, -1 on error (message printed to stderr)
 */
int nova_tool_parse_flags(int argc, char **argv, NovaToolFlags *flags);

/* ============================================================
 * TOOL DISPATCH
 * ============================================================ */

/**
 * @brief Check if a string names a known tool subcommand.
 *
 * @param name  The subcommand name to check
 * @return 1 if known tool, 0 otherwise
 */
int nova_tool_is_tool(const char *name);

/**
 * @brief Dispatch a tool subcommand.
 *
 * Called from nova.c when the first argument matches a tool name.
 * Parses remaining args as tool flags, runs the tool.
 *
 * @param tool  Tool name (e.g. "cat", "grep", "tree")
 * @param argc  Argument count (excluding tool name)
 * @param argv  Arguments (excluding tool name)
 * @return 0 on success, non-zero on error
 */
int nova_tool_dispatch(const char *tool, int argc, char **argv);

/**
 * @brief Print a list of available tools (for nova --help).
 */
void nova_tool_print_help(void);

/* ============================================================
 * INDIVIDUAL TOOL FUNCTIONS
 * ============================================================ */

/** @brief cat: Concatenate and print files. */
int nova_tool_cat(const NovaToolFlags *flags);

/** @brief ls: List directory contents. */
int nova_tool_ls(const NovaToolFlags *flags);

/** @brief tree: Directory tree visualization. */
int nova_tool_tree(const NovaToolFlags *flags);

/** @brief find: Find files by name pattern. */
int nova_tool_find(const NovaToolFlags *flags);

/** @brief grep: Search text in files. */
int nova_tool_grep(const NovaToolFlags *flags);

/** @brief head: First N lines of a file. */
int nova_tool_head(const NovaToolFlags *flags);

/** @brief tail: Last N lines of a file. */
int nova_tool_tail(const NovaToolFlags *flags);

/** @brief wc: Word, line, and character count. */
int nova_tool_wc(const NovaToolFlags *flags);

/** @brief write: Write stdin to file. */
int nova_tool_write(const NovaToolFlags *flags);

/** @brief echo: Print arguments to stdout. */
int nova_tool_echo(const NovaToolFlags *flags);

/** @brief pwd: Print working directory. */
int nova_tool_pwd(const NovaToolFlags *flags);

/** @brief task: Run NINI taskfile tasks. */
int nova_tool_task(const NovaToolFlags *flags);

#endif /* NOVA_TOOLS_H */
