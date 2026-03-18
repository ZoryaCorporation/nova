/**
 * @file ntool_common.h
 * @brief Nova Tools - Shared Utility Library
 *
 * Common infrastructure for all Nova tool binaries: flag parsing,
 * glob matching, directory reading, path manipulation, formatting,
 * and environment detection.
 *
 * Built on the Zorya PAL (Platform Abstraction Layer) for
 * freestanding cross-platform support.
 *
 * @author Anthony Taliento
 * @date 2026-03-16
 * @version 0.2.0
 *
 * @copyright Copyright (c) 2026 Zorya Corporation
 * @license MIT
 *
 * ZORYA-C COMPLIANCE: v2.0.0
 */

#ifndef NTOOL_COMMON_H
#define NTOOL_COMMON_H

#include <zorya/pal.h>

#include <stdio.h>
#include <stdlib.h>

/* ============================================================
 * CONSTANTS
 * ============================================================ */

#define NTOOL_PATH_MAX      ZORYA_MAX_PATH
#define NTOOL_LINE_MAX      8192
#define NTOOL_DIR_MAX       4096
#define NTOOL_MAX_SUBJECTS  64
#define NTOOL_VERSION       "0.2.0"

/** Tree drawing characters (UTF-8) */
#define NTOOL_TREE_BRANCH   "\xe2\x94\x9c\xe2\x94\x80\xe2\x94\x80 "
#define NTOOL_TREE_CORNER   "\xe2\x94\x94\xe2\x94\x80\xe2\x94\x80 "
#define NTOOL_TREE_PIPE     "\xe2\x94\x82   "
#define NTOOL_TREE_SPACE    "    "

/* ============================================================
 * FLAG STRUCTURE
 * ============================================================ */

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
    const char *subjects[NTOOL_MAX_SUBJECTS];
    int         subject_count;
} NToolFlags;

/* ============================================================
 * FLAG PARSER
 * ============================================================ */

/**
 * @brief Parse tool arguments into an NToolFlags structure.
 *
 * Handles both short (-m) and long (--match) flags, with
 * value syntax via = or space separation.
 *
 * @param argc  Number of arguments (tool args only)
 * @param argv  Argument strings
 * @param flags Output flags structure (zeroed and filled)
 * @return 0 on success, -1 on error (message printed to stderr)
 */
int ntool_parse_flags(int argc, char **argv, NToolFlags *flags);

/* ============================================================
 * DIRECTORY READING
 * ============================================================ */

/**
 * @brief Directory entry for tool operations.
 *
 * Uses ZoryaDirEntry layout from PAL for portability.
 */
typedef ZoryaDirEntry NToolDirEntry;

/**
 * @brief Read all entries in a directory.
 *
 * Fills entries array, returns count. Skips "." and "..".
 * Uses Zorya PAL for cross-platform directory iteration.
 *
 * @param path        Directory path to read
 * @param entries     Output array of directory entries
 * @param max_entries Maximum entries to store
 * @param show_hidden Include dotfiles/hidden entries
 * @return Number of entries read
 */
int ntool_read_dir(const char *path, NToolDirEntry *entries,
                   int max_entries, int show_hidden);

/**
 * @brief qsort comparator: directories first, then alphabetical.
 */
int ntool_entry_cmp(const void *a, const void *b);

/* ============================================================
 * PATH / FILESYSTEM QUERIES
 * ============================================================ */

/**
 * @brief Check if a path is a directory.
 *
 * @param path Path to check
 * @return 1 if directory, 0 otherwise
 */
int ntool_is_directory(const char *path);

/**
 * @brief Get file size in bytes.
 *
 * @param path File path
 * @return Size in bytes, -1 on error
 */
long ntool_file_size(const char *path);

/**
 * @brief Get file modification time (epoch seconds).
 *
 * @param path File path
 * @return Modification time, 0 on error
 */
long ntool_file_mtime(const char *path);

/**
 * @brief Join two path segments.
 *
 * @param buf   Output buffer
 * @param bufsz Output buffer size
 * @param base  Base path segment
 * @param name  Name to append
 */
void ntool_join_path(char *buf, size_t bufsz,
                     const char *base, const char *name);

/* ============================================================
 * PATTERN MATCHING
 * ============================================================ */

/**
 * @brief Simple glob pattern matching (* and ? only).
 *
 * No character classes, no braces. Cross-platform, no headers needed.
 *
 * @param pattern Glob pattern
 * @param str     String to match
 * @return 1 if matches, 0 otherwise
 */
int ntool_glob_match(const char *pattern, const char *str);

/**
 * @brief Case-insensitive glob matching.
 */
int ntool_glob_match_ci(const char *pattern, const char *str);

/**
 * @brief Case-insensitive substring search.
 *
 * @param haystack String to search in
 * @param needle   Substring to find
 * @return Pointer to first match, or NULL
 */
const char *ntool_strcasestr(const char *haystack, const char *needle);

/* ============================================================
 * FORMATTING
 * ============================================================ */

/**
 * @brief Format a file size for human-readable display.
 *
 * @param size  Size in bytes
 * @param buf   Output buffer
 * @param bufsz Output buffer size
 */
void ntool_format_size(long size, char *buf, size_t bufsz);

/**
 * @brief Format a timestamp for display.
 *
 * @param mtime Modification time (epoch seconds)
 * @param buf   Output buffer
 * @param bufsz Output buffer size
 */
void ntool_format_time(long mtime, char *buf, size_t bufsz);

/* ============================================================
 * ENVIRONMENT DETECTION
 * ============================================================ */

/**
 * @brief Check if stdin has piped data (not a TTY).
 *
 * @return 1 if stdin has data, 0 if interactive
 */
int ntool_stdin_has_data(void);

/**
 * @brief Check if color output is enabled.
 *
 * Respects NO_COLOR env var and terminal detection.
 *
 * @return 1 if color enabled, 0 otherwise
 */
int ntool_color_enabled(void);

#endif /* NTOOL_COMMON_H */
