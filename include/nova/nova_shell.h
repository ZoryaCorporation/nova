/**
 * @file    nova_shell.h
 * @brief   Nova Interactive Tool Shell — Discovery-Based Dispatch
 *
 * The Nova shell discovers tool binaries at startup by scanning
 * known directories for executables prefixed with 'n'. Users type
 * unprefixed names (e.g., "cat" runs "ncat"). Third-party tools
 * are automatically discovered.
 *
 * @author  Anthony Taliento
 * @date    2026-03-16
 * @version 0.2.0
 *
 * @copyright Copyright (c) 2026 Zorya Corporation
 * @license MIT
 *
 * ZORYA-C COMPLIANCE: v2.0.0
 */

#ifndef NOVA_SHELL_H
#define NOVA_SHELL_H

/* ---- Constants ---- */

/** Maximum discoverable tools */
#define NOVA_SHELL_MAX_TOOLS   256

/** Maximum command-line length in the shell */
#define NOVA_SHELL_LINE_MAX    4096

/** Maximum tokens (arguments) per command */
#define NOVA_SHELL_MAX_ARGS    128

/** Maximum pipeline stages */
#define NOVA_SHELL_PIPE_MAX    16

/** Environment variable to override tool search path */
#define NOVA_TOOLS_DIR_ENV     "NOVA_TOOLS_DIR"

/** Local project tools directory */
#define NOVA_TOOLS_DIR_LOCAL   "./bin/tools"

/** System-installed tools directory */
#define NOVA_TOOLS_DIR_SYSTEM  "/usr/local/libexec/nova"

/** User tools directory (relative to $HOME) */
#define NOVA_TOOLS_DIR_USER    ".nova/tools"

/** Tool binary prefix (stripped for unprefixed names) */
#define NOVA_TOOL_PREFIX       "n"

/* ---- Types ---- */

/**
 * @brief A discovered tool binary.
 */
typedef struct {
    char name[64];       /**< Unprefixed name (e.g., "cat") */
    char path[4096];     /**< Full path to binary           */
} NovaShellTool;

/**
 * @brief Registry of discovered tool binaries.
 */
typedef struct {
    NovaShellTool tools[NOVA_SHELL_MAX_TOOLS];
    int           count;
} NovaShellToolRegistry;

/* ---- API ---- */

/**
 * @brief Discover tool binaries by scanning known directories.
 *
 * Search order:
 *   1. $NOVA_TOOLS_DIR (environment override)
 *   2. ./bin/tools/     (local project)
 *   3. ~/.nova/tools/   (user-installed)
 *   4. /usr/local/libexec/nova/ (system-installed)
 *
 * @param reg  Output registry (zeroed and filled)
 * @return Number of tools discovered
 */
int nova_shell_discover_tools(NovaShellToolRegistry *reg);

/**
 * @brief Find a tool by unprefixed name in the registry.
 *
 * @param reg   The tool registry
 * @param name  Unprefixed tool name (e.g., "cat")
 * @return Pointer to the tool entry, or NULL if not found
 */
const NovaShellTool *nova_shell_find_tool(const NovaShellToolRegistry *reg,
                                          const char *name);

/**
 * @brief Check if a name is a known tool (discovered or built-in).
 *
 * @param reg   Tool registry (may be NULL for legacy fallback)
 * @param name  Command name
 * @return 1 if known tool, 0 otherwise
 */
int nova_shell_is_tool(const NovaShellToolRegistry *reg, const char *name);

/**
 * @brief Dispatch a single tool command.
 *
 * Resolution order:
 *   1. Discovered tool binary → exec
 *   2. Built-in "task" command → nova_tool_task()
 *   3. Unknown → error
 *
 * @param reg   Tool registry
 * @param name  Tool name (unprefixed)
 * @param argc  Argument count
 * @param argv  Arguments
 * @return Exit status (0 = success)
 */
int nova_shell_dispatch(const NovaShellToolRegistry *reg,
                        const char *name, int argc, char **argv);

/**
 * @brief Run the interactive Nova tool shell.
 *
 * Discovers tools, presents a "nova$" prompt, dispatches commands,
 * supports pipeline chaining with |.
 *
 * @return 0 on normal exit
 */
int nova_shell_run(void);

#endif /* NOVA_SHELL_H */
