/**
 * @file settingsPanel.ts
 * @brief Nova SDK — Settings Webview Panel
 *
 * A real GUI settings page for the Nova SDK extension.
 * Groups settings into logical sections:
 *   - Runtime:  Executable path, optimization, debug
 *   - LSP:     Language Server configuration
 *   - MCP:     Model Context Protocol server
 *   - Terminal: Shell integration
 *   - Taskfile: Task runner configuration
 *   - Tracing:  Debug tracing channels
 *
 * @author Anthony Taliento (Zorya Corporation)
 * @copyright (c) 2026 Zorya Corporation
 */
import * as vscode from 'vscode';
export declare class NovaSettingsPanel {
    static readonly viewType = "nova.settings";
    private static currentPanel;
    private readonly _panel;
    private readonly _context;
    private _disposables;
    private constructor();
    static show(context: vscode.ExtensionContext): void;
    dispose(): void;
    private _getCurrentSettings;
    private _handleMessage;
    private _getHtml;
}
//# sourceMappingURL=settingsPanel.d.ts.map