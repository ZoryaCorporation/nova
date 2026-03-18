/**
 * @file nova_task.h
 * @brief Nova Language - NINI Task Runner Interface
 *
 * NINI-based universal build orchestrator. Reads taskfile.nini,
 * decodes [task:*] sections, resolves dependencies, and executes
 * shell commands with environment management.
 *
 * @author Anthony Taliento
 * @date 2026-02-18
 * @version 0.2.0
 *
 * @copyright Copyright (c) 2026 Zorya Corporation
 * @license MIT
 *
 * ZORYA-C COMPLIANCE: v2.0.0
 */

#ifndef NOVA_TASK_H
#define NOVA_TASK_H

#include "nova/nova_tools.h"   /* NovaToolFlags */

/**
 * @brief Task runner entry point.
 *
 * Creates a temporary VM for NINI decoding, reads the taskfile,
 * applies global config, and dispatches to the named task(s).
 *
 * @param flags  Parsed command-line flags
 * @return 0 on success, non-zero on failure
 */
int nova_tool_task(const NovaToolFlags *flags);

#endif /* NOVA_TASK_H */
