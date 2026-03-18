/**
 * @file extension.ts
 * @brief Nova SDK — VS Code Extension Entry Point
 *
 * Provides:
 *   - Language Server Protocol (LSP) client for diagnostics, completion, hover
 *   - MCP server integration for AI tool usage
 *   - Run/compile/disassemble commands for .n files
 *   - NINI taskfile auto-detection and task provider
 *   - Nova terminal profile (interactive tool shell)
 *   - Error explanation command
 *   - Status bar integration
 *
 * @author Anthony Taliento (Zorya Corporation)
 * @copyright (c) 2026 Zorya Corporation
 */
import * as vscode from 'vscode';
export declare function activate(context: vscode.ExtensionContext): void;
export declare function deactivate(): Promise<void>;
//# sourceMappingURL=extension.d.ts.map