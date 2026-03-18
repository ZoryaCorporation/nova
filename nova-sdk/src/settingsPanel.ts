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

export class NovaSettingsPanel {
    public static readonly viewType = 'nova.settings';
    private static currentPanel: NovaSettingsPanel | undefined;

    private readonly _panel: vscode.WebviewPanel;
    private readonly _context: vscode.ExtensionContext;
    private _disposables: vscode.Disposable[] = [];

    private constructor(
        panel: vscode.WebviewPanel,
        context: vscode.ExtensionContext
    ) {
        this._panel = panel;
        this._context = context;

        this._panel.webview.html = this._getHtml();

        /* Handle messages from webview */
        this._panel.webview.onDidReceiveMessage(
            (msg) => this._handleMessage(msg),
            null,
            this._disposables
        );

        /* Handle panel disposal */
        this._panel.onDidDispose(() => this.dispose(), null, this._disposables);

        /* Refresh when settings change externally */
        vscode.workspace.onDidChangeConfiguration(
            (e) => {
                if (e.affectsConfiguration('nova')) {
                    this._panel.webview.postMessage({
                        type: 'refresh',
                        settings: this._getCurrentSettings(),
                    });
                }
            },
            null,
            this._disposables
        );
    }

    public static show(context: vscode.ExtensionContext): void {
        if (NovaSettingsPanel.currentPanel) {
            NovaSettingsPanel.currentPanel._panel.reveal(vscode.ViewColumn.One);
            return;
        }

        const panel = vscode.window.createWebviewPanel(
            NovaSettingsPanel.viewType,
            'Nova SDK Settings',
            vscode.ViewColumn.One,
            {
                enableScripts: true,
                retainContextWhenHidden: true,
                localResourceRoots: [],
            }
        );

        NovaSettingsPanel.currentPanel = new NovaSettingsPanel(panel, context);
    }

    public dispose(): void {
        NovaSettingsPanel.currentPanel = undefined;
        this._panel.dispose();
        while (this._disposables.length) {
            const d = this._disposables.pop();
            if (d) { d.dispose(); }
        }
    }

    private _getCurrentSettings(): Record<string, unknown> {
        const cfg = vscode.workspace.getConfiguration('nova');
        return {
            executablePath: cfg.get<string>('executablePath', 'nova'),
            sdkServerPath: cfg.get<string>('sdkServerPath', ''),
            runArgs: (cfg.get<string[]>('runArgs', []) || []).join(' '),
            optimizationLevel: cfg.get<string>('optimizationLevel', '-O1'),
            stripDebugInfo: cfg.get<boolean>('stripDebugInfo', false),

            lspEnabled: cfg.get<boolean>('lsp.enabled', true),
            lspPath: cfg.get<string>('lsp.path', ''),
            lspTrace: cfg.get<string>('lsp.trace', 'off'),

            mcpEnabled: cfg.get<boolean>('mcp.enabled', true),
            mcpPath: cfg.get<string>('mcp.path', ''),
            mcpSandbox: cfg.get<boolean>('mcp.sandbox', true),

            terminalShellIntegration: cfg.get<boolean>('terminal.shellIntegration', true),

            taskfileAutoDetect: cfg.get<boolean>('taskfile.autoDetect', true),
            taskfilePath: cfg.get<string>('taskfile.path', 'taskfile.nini'),

            traceEnabled: cfg.get<boolean>('trace.enabled', false),
            traceChannels: cfg.get<string>('trace.channels', 'all'),
        };
    }

    private async _handleMessage(msg: { type: string; key?: string; value?: unknown }): Promise<void> {
        switch (msg.type) {
            case 'getSettings':
                this._panel.webview.postMessage({
                    type: 'refresh',
                    settings: this._getCurrentSettings(),
                });
                break;

            case 'updateSetting':
                if (msg.key && msg.value !== undefined) {
                    const cfg = vscode.workspace.getConfiguration('nova');
                    let val: unknown = msg.value;

                    /* Handle array settings */
                    if (msg.key === 'runArgs') {
                        val = (msg.value as string).split(/\s+/).filter(Boolean);
                    }

                    await cfg.update(msg.key, val, vscode.ConfigurationTarget.Global);
                }
                break;

            case 'browseFile': {
                const result = await vscode.window.showOpenDialog({
                    canSelectFiles: true,
                    canSelectFolders: false,
                    canSelectMany: false,
                    title: `Select ${msg.key || 'file'}`,
                });
                if (result && result[0]) {
                    this._panel.webview.postMessage({
                        type: 'filePicked',
                        key: msg.key,
                        path: result[0].fsPath,
                    });
                }
                break;
            }

            case 'openExternal':
                if (msg.value && typeof msg.value === 'string') {
                    vscode.env.openExternal(vscode.Uri.parse(msg.value));
                }
                break;

            case 'restartLsp':
                await vscode.commands.executeCommand('workbench.action.restartExtensionHost');
                break;

            case 'runCommand':
                if (msg.value && typeof msg.value === 'string') {
                    await vscode.commands.executeCommand(msg.value);
                }
                break;
        }
    }

    private _getHtml(): string {
        const settings = this._getCurrentSettings();
        const settingsJson = JSON.stringify(settings)
            .replace(/</g, '\\u003c');

        return `<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<meta http-equiv="Content-Security-Policy"
    content="default-src 'none'; style-src 'unsafe-inline'; script-src 'unsafe-inline';">
<title>Nova SDK Settings</title>
<style>
    :root {
        --nova-green: #4A8C5C;
        --nova-green-bright: #6DB880;
        --nova-green-dim: #2D5E3A;
        --nova-green-muted: #162218;
        --nova-bg: #0c0c0c;
        --nova-card: #141414;
        --nova-border: #252525;
        --nova-text: #a8a8a8;
        --nova-text-dim: #606060;
        --nova-text-bright: #d4d4d4;
        --nova-error: #cf6679;
        --nova-warn: #c8b060;
    }

    * { box-sizing: border-box; margin: 0; padding: 0; }

    body {
        font-family: var(--vscode-font-family, 'Segoe UI', Tahoma, sans-serif);
        font-size: 13px;
        color: var(--nova-text);
        background: var(--nova-bg);
        padding: 0;
        line-height: 1.5;
    }

    /* ---- Header ---- */
    .header {
        background: var(--nova-card);
        border-bottom: 2px solid var(--nova-green-dim);
        padding: 24px 32px;
        display: flex;
        align-items: center;
        gap: 16px;
    }
    .header-icon {
        display: flex;
        align-items: center;
        justify-content: center;
        flex-shrink: 0;
    }
    .header-icon svg {
        width: 48px;
        height: 48px;
    }
    .header-text h1 {
        font-size: 20px;
        color: var(--nova-text-bright);
        font-weight: 600;
        letter-spacing: 0.5px;
    }
    .header-text p {
        color: var(--nova-text-dim);
        font-size: 12px;
        margin-top: 2px;
    }
    .header-version {
        margin-left: auto;
        background: var(--nova-green-muted);
        color: var(--nova-green);
        padding: 4px 12px;
        border-radius: 12px;
        font-size: 11px;
        font-weight: 600;
        border: 1px solid var(--nova-green-dim);
        font-family: 'Consolas', monospace;
    }

    /* ---- Nav ---- */
    .nav {
        display: flex;
        gap: 0;
        background: var(--nova-card);
        border-bottom: 1px solid var(--nova-border);
        padding: 0 32px;
        overflow-x: auto;
    }
    .nav-tab {
        padding: 10px 20px;
        color: var(--nova-text-dim);
        cursor: pointer;
        border-bottom: 2px solid transparent;
        font-size: 12px;
        font-weight: 500;
        text-transform: uppercase;
        letter-spacing: 0.8px;
        white-space: nowrap;
        transition: all 0.15s ease;
        user-select: none;
    }
    .nav-tab:hover {
        color: var(--nova-text-bright);
        background: #1a1a1a;
    }
    .nav-tab.active {
        color: var(--nova-green);
        border-bottom-color: var(--nova-green);
    }

    /* ---- Content ---- */
    .content {
        max-width: 720px;
        margin: 0 auto;
        padding: 24px 32px 48px;
    }

    .section {
        display: none;
    }
    .section.active {
        display: block;
    }

    .section-title {
        font-size: 15px;
        font-weight: 600;
        color: var(--nova-green);
        margin-bottom: 4px;
        letter-spacing: 0.3px;
    }
    .section-desc {
        color: var(--nova-text-dim);
        font-size: 12px;
        margin-bottom: 20px;
        line-height: 1.6;
    }

    /* ---- Settings Items ---- */
    .setting {
        margin-bottom: 20px;
        padding: 16px;
        background: var(--nova-card);
        border: 1px solid var(--nova-border);
        border-radius: 8px;
        transition: border-color 0.15s ease;
    }
    .setting:hover {
        border-color: var(--nova-green-dim);
    }
    .setting-header {
        display: flex;
        align-items: center;
        justify-content: space-between;
        margin-bottom: 6px;
    }
    .setting-label {
        font-size: 13px;
        font-weight: 600;
        color: var(--nova-text-bright);
    }
    .setting-key {
        font-family: 'Consolas', 'Courier New', monospace;
        font-size: 10px;
        color: var(--nova-text-dim);
        background: #1a1a1a;
        padding: 2px 6px;
        border-radius: 4px;
    }
    .setting-desc {
        font-size: 12px;
        color: var(--nova-text-dim);
        margin-bottom: 10px;
        line-height: 1.5;
    }

    /* ---- Inputs ---- */
    input[type="text"], select {
        width: 100%;
        padding: 8px 12px;
        background: var(--nova-bg);
        border: 1px solid var(--nova-border);
        border-radius: 6px;
        color: var(--nova-text-bright);
        font-family: 'Consolas', 'Courier New', monospace;
        font-size: 13px;
        outline: none;
        transition: border-color 0.15s ease;
    }
    input[type="text"]:focus, select:focus {
        border-color: var(--nova-green);
        box-shadow: 0 0 0 1px var(--nova-green-dim);
    }
    input[type="text"]::placeholder {
        color: var(--nova-text-dim);
    }

    select {
        cursor: pointer;
        appearance: none;
        -webkit-appearance: none;
        background-image: url("data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' width='12' height='12' viewBox='0 0 12 12'%3E%3Cpath fill='%234A8C5C' d='M3 5l3 3 3-3z'/%3E%3C/svg%3E");
        background-repeat: no-repeat;
        background-position: right 10px center;
        padding-right: 28px;
    }

    /* ---- Toggle ---- */
    .toggle-row {
        display: flex;
        align-items: center;
        justify-content: space-between;
    }
    .toggle-row .setting-left {
        flex: 1;
    }
    .toggle {
        position: relative;
        width: 44px;
        height: 24px;
        flex-shrink: 0;
        margin-left: 16px;
    }
    .toggle input {
        opacity: 0;
        width: 0;
        height: 0;
    }
    .toggle-slider {
        position: absolute;
        cursor: pointer;
        inset: 0;
        background: var(--nova-border);
        border-radius: 12px;
        transition: all 0.2s ease;
    }
    .toggle-slider:before {
        content: '';
        position: absolute;
        width: 18px;
        height: 18px;
        left: 3px;
        top: 3px;
        background: var(--nova-text-dim);
        border-radius: 50%;
        transition: all 0.2s ease;
    }
    .toggle input:checked + .toggle-slider {
        background: var(--nova-green-dim);
    }
    .toggle input:checked + .toggle-slider:before {
        transform: translateX(20px);
        background: var(--nova-green);
    }

    /* ---- File picker ---- */
    .input-with-btn {
        display: flex;
        gap: 8px;
    }
    .input-with-btn input {
        flex: 1;
    }
    .btn {
        padding: 8px 16px;
        background: var(--nova-green-dim);
        color: var(--nova-text-bright);
        border: 1px solid var(--nova-green-dim);
        border-radius: 6px;
        cursor: pointer;
        font-size: 12px;
        font-weight: 600;
        white-space: nowrap;
        transition: all 0.15s ease;
    }
    .btn:hover {
        background: var(--nova-green);
        color: #000;
    }
    .btn-outline {
        background: transparent;
        border-color: var(--nova-border);
        color: var(--nova-text-dim);
    }
    .btn-outline:hover {
        border-color: var(--nova-green);
        color: var(--nova-green);
        background: var(--nova-green-muted);
    }

    /* ---- Info box ---- */
    .info-box {
        padding: 12px 16px;
        background: #181818;
        border: 1px solid var(--nova-border);
        border-radius: 8px;
        font-size: 12px;
        color: var(--nova-text);
        line-height: 1.6;
        margin-bottom: 20px;
    }
    .info-box code {
        font-family: 'Consolas', monospace;
        color: var(--nova-green-bright);
        font-size: 12px;
    }
    .info-box .label {
        color: var(--nova-text-bright);
        font-weight: 600;
    }

    /* ---- Status indicator ---- */
    .status-dot {
        display: inline-block;
        width: 8px;
        height: 8px;
        border-radius: 50%;
        margin-right: 6px;
    }
    .status-dot.on { background: var(--nova-green); }
    .status-dot.off { background: var(--nova-text-dim); }
    .status-dot.warn { background: var(--nova-warn); }

    /* ---- Footer ---- */
    .footer {
        text-align: center;
        padding: 20px;
        border-top: 1px solid var(--nova-border);
        margin-top: 32px;
    }
    .footer-text {
        color: var(--nova-text-dim);
        font-size: 11px;
    }
    .footer-text a {
        color: var(--nova-green);
        text-decoration: none;
    }
    .footer-text a:hover {
        text-decoration: underline;
    }

    /* ---- Responsive ---- */
    @media (max-width: 600px) {
        .header { padding: 16px; }
        .nav { padding: 0 16px; }
        .content { padding: 16px; }
    }
</style>
</head>
<body>

<!-- Header -->
<div class="header">
    <div class="header-icon">
        <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 48 48">
            <rect x="2" y="2" width="44" height="44" rx="10" fill="#1a1a1a" stroke="#2D5E3A" stroke-width="1.5"/>
            <text x="24" y="34" font-family="'Consolas','Courier New',monospace"
                  font-size="28" font-weight="900" fill="#4A8C5C"
                  text-anchor="middle" dominant-baseline="auto">N</text>
        </svg>
    </div>
    <div class="header-text">
        <h1>Nova SDK Settings</h1>
        <p>Configure your Nova development environment</p>
    </div>
    <span class="header-version">v0.2.0</span>
</div>

<!-- Navigation Tabs -->
<div class="nav">
    <div class="nav-tab active" data-tab="runtime">Runtime</div>
    <div class="nav-tab" data-tab="lsp">Language Server</div>
    <div class="nav-tab" data-tab="mcp">MCP Server</div>
    <div class="nav-tab" data-tab="terminal">Terminal</div>
    <div class="nav-tab" data-tab="taskfile">Tasks</div>
    <div class="nav-tab" data-tab="tracing">Tracing</div>
    <div class="nav-tab" data-tab="customize">Customize</div>
</div>

<!-- Content Sections -->
<div class="content">

    <!-- ============ RUNTIME ============ -->
    <div class="section active" id="section-runtime">
        <div class="section-title">Runtime Configuration</div>
        <div class="section-desc">
            Configure how Nova scripts are compiled and executed.
        </div>

        <div class="setting">
            <div class="setting-header">
                <span class="setting-label">Nova Executable Path</span>
                <span class="setting-key">nova.executablePath</span>
            </div>
            <div class="setting-desc">
                Path to the Nova binary. If Nova is on your PATH, just <code>nova</code> works.
            </div>
            <div class="input-with-btn">
                <input type="text" id="executablePath" placeholder="nova"
                       data-key="executablePath">
                <button class="btn" onclick="browseFile('executablePath')">Browse</button>
            </div>
        </div>

        <div class="setting">
            <div class="setting-header">
                <span class="setting-label">SDK Server Path</span>
                <span class="setting-key">nova.sdkServerPath</span>
            </div>
            <div class="setting-desc">
                Path to the unified <code>nova-sdk-server</code> binary (provides both LSP and MCP).
                Leave empty to auto-detect from PATH or adjacent to the Nova executable.
            </div>
            <div class="input-with-btn">
                <input type="text" id="sdkServerPath" placeholder="(auto-detect)"
                       data-key="sdkServerPath">
                <button class="btn" onclick="browseFile('sdkServerPath')">Browse</button>
            </div>
        </div>

        <div class="setting">
            <div class="setting-header">
                <span class="setting-label">Additional Run Arguments</span>
                <span class="setting-key">nova.runArgs</span>
            </div>
            <div class="setting-desc">
                Extra arguments passed to <code>nova</code> when running scripts (space-separated).
            </div>
            <input type="text" id="runArgs" placeholder="--gc-stress --verbose"
                   data-key="runArgs">
        </div>

        <div class="setting">
            <div class="setting-header">
                <span class="setting-label">Optimization Level</span>
                <span class="setting-key">nova.optimizationLevel</span>
            </div>
            <div class="setting-desc">
                Optimization level for compilation. -O0 disables optimizations (useful for debugging).
            </div>
            <select id="optimizationLevel" data-key="optimizationLevel">
                <option value="-O0">-O0 (No optimization)</option>
                <option value="-O1">-O1 (Standard)</option>
            </select>
        </div>

        <div class="setting">
            <div class="toggle-row">
                <div class="setting-left">
                    <div class="setting-header">
                        <span class="setting-label">Strip Debug Info</span>
                        <span class="setting-key">nova.stripDebugInfo</span>
                    </div>
                    <div class="setting-desc">
                        Remove debug information from compiled output (-s flag).
                    </div>
                </div>
                <label class="toggle">
                    <input type="checkbox" id="stripDebugInfo" data-key="stripDebugInfo">
                    <span class="toggle-slider"></span>
                </label>
            </div>
        </div>
    </div>

    <!-- ============ LSP ============ -->
    <div class="section" id="section-lsp">
        <div class="section-title">Language Server Protocol</div>
        <div class="section-desc">
            The Nova Language Server provides real-time diagnostics, code completion,
            and hover information. Now powered by the unified <code>nova-sdk-server --lsp</code>.
        </div>

        <div class="info-box">
            <span class="label">Status:</span>
            <span class="status-dot" id="lsp-status-dot"></span>
            <span id="lsp-status-text">Checking...</span>
        </div>

        <div class="setting">
            <div class="toggle-row">
                <div class="setting-left">
                    <div class="setting-header">
                        <span class="setting-label">Enable Language Server</span>
                        <span class="setting-key">nova.lsp.enabled</span>
                    </div>
                    <div class="setting-desc">
                        Start the Nova LSP server for diagnostics, completion, and hover.
                    </div>
                </div>
                <label class="toggle">
                    <input type="checkbox" id="lspEnabled" data-key="lsp.enabled">
                    <span class="toggle-slider"></span>
                </label>
            </div>
        </div>

        <div class="setting">
            <div class="setting-header">
                <span class="setting-label">LSP Binary Path <span style="color:#ff9800;font-size:11px">(deprecated)</span></span>
                <span class="setting-key">nova.lsp.path</span>
            </div>
            <div class="setting-desc">
                <em>Deprecated.</em> Use <code>nova.sdkServerPath</code> in the Runtime tab instead.
                The unified <code>nova-sdk-server</code> binary handles both LSP and MCP.
            </div>
            <div class="input-with-btn">
                <input type="text" id="lspPath" placeholder="(use sdkServerPath)"
                       data-key="lsp.path">
                <button class="btn" onclick="browseFile('lspPath')">Browse</button>
            </div>
        </div>

        <div class="setting">
            <div class="setting-header">
                <span class="setting-label">LSP Trace Level</span>
                <span class="setting-key">nova.lsp.trace</span>
            </div>
            <div class="setting-desc">
                Trace communication between VS Code and the Nova Language Server.
                Useful for debugging LSP issues.
            </div>
            <select id="lspTrace" data-key="lsp.trace">
                <option value="off">Off</option>
                <option value="messages">Messages</option>
                <option value="verbose">Verbose</option>
            </select>
        </div>
    </div>

    <!-- ============ MCP ============ -->
    <div class="section" id="section-mcp">
        <div class="section-title">Model Context Protocol Server</div>
        <div class="section-desc">
            The Nova MCP Server exposes 12 tools to AI assistants via the MCP protocol.
            Now powered by the unified <code>nova-sdk-server --mcp</code> with a shared
            SQLite-backed memory system.
        </div>

        <div class="info-box">
            <span class="label">12 Tools:</span> 7 Nova-smart (eval, check, disassemble,
            explain-error, describe-api, project-info, run-tests) + 5 universal
            (workspace-tree, memory-store, memory-query, memory-forget, analyze)<br>
            <span class="label">Memory DB:</span> <code>.nova/sdk.db</code> (SQLite3 + FTS5)
        </div>

        <div class="setting">
            <div class="toggle-row">
                <div class="setting-left">
                    <div class="setting-header">
                        <span class="setting-label">Enable MCP Server</span>
                        <span class="setting-key">nova.mcp.enabled</span>
                    </div>
                    <div class="setting-desc">
                        Register the Nova MCP Server with VS Code for AI tool integration.
                        Writes configuration to <code>.vscode/mcp.json</code>.
                    </div>
                </div>
                <label class="toggle">
                    <input type="checkbox" id="mcpEnabled" data-key="mcp.enabled">
                    <span class="toggle-slider"></span>
                </label>
            </div>
        </div>

        <div class="setting">
            <div class="setting-header">
                <span class="setting-label">MCP Binary Path <span style="color:#ff9800;font-size:11px">(deprecated)</span></span>
                <span class="setting-key">nova.mcp.path</span>
            </div>
            <div class="setting-desc">
                <em>Deprecated.</em> Use <code>nova.sdkServerPath</code> in the Runtime tab instead.
                The unified <code>nova-sdk-server</code> binary handles both LSP and MCP.
            </div>
            <div class="input-with-btn">
                <input type="text" id="mcpPath" placeholder="(use sdkServerPath)"
                       data-key="mcp.path">
                <button class="btn" onclick="browseFile('mcpPath')">Browse</button>
            </div>
        </div>

        <div class="setting">
            <div class="toggle-row">
                <div class="setting-left">
                    <div class="setting-header">
                        <span class="setting-label">Sandbox Mode</span>
                        <span class="setting-key">nova.mcp.sandbox</span>
                    </div>
                    <div class="setting-desc">
                        Restrict MCP tool execution to safe operations. Disabling sandbox
                        allows filesystem, network, and OS access from AI tools.
                        <strong>Recommended: ON</strong>
                    </div>
                </div>
                <label class="toggle">
                    <input type="checkbox" id="mcpSandbox" data-key="mcp.sandbox">
                    <span class="toggle-slider"></span>
                </label>
            </div>
        </div>
    </div>

    <!-- ============ TERMINAL ============ -->
    <div class="section" id="section-terminal">
        <div class="section-title">Terminal Integration</div>
        <div class="section-desc">
            Nova includes an interactive tool shell with built-in commands
            (cat, ls, tree, find, grep, head, tail, wc, task).
        </div>

        <div class="setting">
            <div class="toggle-row">
                <div class="setting-left">
                    <div class="setting-header">
                        <span class="setting-label">Shell Integration</span>
                        <span class="setting-key">nova.terminal.shellIntegration</span>
                    </div>
                    <div class="setting-desc">
                        Launch Nova's interactive tool shell as the terminal profile.
                        When off, the Nova terminal opens a regular bash shell.
                    </div>
                </div>
                <label class="toggle">
                    <input type="checkbox" id="terminalShellIntegration"
                           data-key="terminal.shellIntegration">
                    <span class="toggle-slider"></span>
                </label>
            </div>
        </div>

        <div class="info-box">
            <span class="label">Tip:</span> Use the status bar <code>Nova</code> button
            or run <strong>Nova: Open Terminal</strong> (Ctrl+Shift+P) to launch the
            interactive shell. Built-in tools run in-process with zero subprocess overhead.
        </div>
    </div>

    <!-- ============ TASKFILE ============ -->
    <div class="section" id="section-taskfile">
        <div class="section-title">Task Runner</div>
        <div class="section-desc">
            Nova uses NINI taskfiles for build automation. Tasks defined in
            <code>[task:name]</code> sections are auto-detected and integrated
            with VS Code's task system.
        </div>

        <div class="setting">
            <div class="toggle-row">
                <div class="setting-left">
                    <div class="setting-header">
                        <span class="setting-label">Auto-Detect Tasks</span>
                        <span class="setting-key">nova.taskfile.autoDetect</span>
                    </div>
                    <div class="setting-desc">
                        Automatically scan for <code>taskfile.nini</code> in the workspace
                        and register discovered tasks with VS Code.
                    </div>
                </div>
                <label class="toggle">
                    <input type="checkbox" id="taskfileAutoDetect"
                           data-key="taskfile.autoDetect">
                    <span class="toggle-slider"></span>
                </label>
            </div>
        </div>

        <div class="setting">
            <div class="setting-header">
                <span class="setting-label">Taskfile Path</span>
                <span class="setting-key">nova.taskfile.path</span>
            </div>
            <div class="setting-desc">
                Relative path to the NINI taskfile from workspace root.
            </div>
            <input type="text" id="taskfilePath" placeholder="taskfile.nini"
                   data-key="taskfile.path">
        </div>
    </div>

    <!-- ============ TRACING ============ -->
    <div class="section" id="section-tracing">
        <div class="section-title">Debug Tracing</div>
        <div class="section-desc">
            Enable runtime tracing to inspect VM execution, calls, stack operations,
            GC cycles, and module loading. Useful for debugging Nova scripts and the VM.
        </div>

        <div class="setting">
            <div class="toggle-row">
                <div class="setting-left">
                    <div class="setting-header">
                        <span class="setting-label">Enable Tracing</span>
                        <span class="setting-key">nova.trace.enabled</span>
                    </div>
                    <div class="setting-desc">
                        Pass <code>--trace</code> flag when running Nova scripts.
                        Warning: generates verbose output.
                    </div>
                </div>
                <label class="toggle">
                    <input type="checkbox" id="traceEnabled" data-key="trace.enabled">
                    <span class="toggle-slider"></span>
                </label>
            </div>
        </div>

        <div class="setting">
            <div class="setting-header">
                <span class="setting-label">Trace Channels</span>
                <span class="setting-key">nova.trace.channels</span>
            </div>
            <div class="setting-desc">
                Which trace channels to enable when tracing is on.
            </div>
            <select id="traceChannels" data-key="trace.channels">
                <option value="all">All Channels</option>
                <option value="vm">VM (dispatch loop)</option>
                <option value="call">Call (function calls)</option>
                <option value="stack">Stack (push/pop operations)</option>
                <option value="gc">GC (garbage collection)</option>
                <option value="module">Module (require/import)</option>
            </select>
        </div>
    </div>

    <!-- ============ CUSTOMIZE ============ -->
    <div class="section" id="section-customize">
        <div class="section-title">Appearance &amp; Customization</div>
        <div class="section-desc">
            Personalize your Nova development experience with custom themes,
            syntax colors, and file icons.
        </div>

        <div class="setting">
            <div class="setting-header">
                <span class="setting-label">Theme &amp; Syntax Colors</span>
            </div>
            <div class="setting-desc">
                Choose from 10 built-in syntax profiles (Phosphor, Cyberpunk, Monokai, Dracula, Nord...)
                or create your own color scheme. Works with any VS Code theme and applies to all languages.
            </div>
            <button class="btn" onclick="runCommand('nova.customizeTheme')">
                Open Theme Customizer
            </button>
        </div>

        <div class="setting">
            <div class="setting-header">
                <span class="setting-label">File Icons</span>
            </div>
            <div class="setting-desc">
                Add custom SVG icons for any file extension. Icons are integrated into
                the Nova icon theme and appear in the Explorer, tabs, and breadcrumbs.
            </div>
            <button class="btn" onclick="runCommand('nova.customizeIcons')">
                Open Icon Customizer
            </button>
        </div>

        <div class="setting">
            <div class="setting-header">
                <span class="setting-label">Color Theme</span>
            </div>
            <div class="setting-desc">
                Switch between Nova's built-in color themes or open VS Code's theme picker.
            </div>
            <button class="btn btn-outline" onclick="runCommand('workbench.action.selectTheme')">
                Change Color Theme
            </button>
        </div>

        <div class="setting">
            <div class="setting-header">
                <span class="setting-label">Icon Theme</span>
            </div>
            <div class="setting-desc">
                Switch file icon themes. Select "Nova Icons" for the full Nova experience.
            </div>
            <button class="btn btn-outline" onclick="runCommand('workbench.action.selectIconTheme')">
                Change Icon Theme
            </button>
        </div>

        <div class="setting">
            <div class="setting-header">
                <span class="setting-label">Recognized Languages</span>
            </div>
            <div class="setting-desc">
                The Nova SDK extension registers the following languages and file associations.
            </div>
            <div style="font-size:12px; line-height:1.8; margin-top:8px;">
                <div style="display:grid; grid-template-columns:auto 1fr; gap:4px 16px; align-items:baseline;">
                    <span style="color:var(--nova-text-bright);font-weight:600;">Nova</span>
                    <span style="color:var(--nova-text-dim); font-family:Consolas,monospace;">.n, .nova</span>
                    <span style="color:var(--nova-text-bright);font-weight:600;">Nova Header</span>
                    <span style="color:var(--nova-text-dim); font-family:Consolas,monospace;">.m</span>
                    <span style="color:var(--nova-text-bright);font-weight:600;">Nova Object</span>
                    <span style="color:var(--nova-text-dim); font-family:Consolas,monospace;">.no</span>
                    <span style="color:var(--nova-text-bright);font-weight:600;">NINI Config</span>
                    <span style="color:var(--nova-text-dim); font-family:Consolas,monospace;">.nini, .ni</span>
                    <span style="color:var(--nova-text-bright);font-weight:600;">NINI Taskfile</span>
                    <span style="color:var(--nova-text-dim); font-family:Consolas,monospace;">taskfile.nini</span>
                    <span style="color:var(--nova-text-bright);font-weight:600;">Nova Doc</span>
                    <span style="color:var(--nova-text-dim); font-family:Consolas,monospace;">.nd</span>
                </div>
            </div>
        </div>
    </div>

    <!-- Footer -->
    <div class="footer">
        <div class="footer-text">
            Nova SDK v0.2.0 &middot; Zorya Corporation &middot;
            <a href="#" onclick="openExternal('https://github.com/zorya-corporation/nova')">GitHub</a> &middot;
            <a href="#" onclick="openExternal('https://github.com/zorya-corporation/nova/issues')">Issues</a>
        </div>
    </div>
</div>

<script>
(function() {
    const vscode = acquireVsCodeApi();
    let settings = ${settingsJson};

    /* ---- Tab switching ---- */
    document.querySelectorAll('.nav-tab').forEach(tab => {
        tab.addEventListener('click', () => {
            document.querySelectorAll('.nav-tab').forEach(t => t.classList.remove('active'));
            document.querySelectorAll('.section').forEach(s => s.classList.remove('active'));
            tab.classList.add('active');
            const section = document.getElementById('section-' + tab.dataset.tab);
            if (section) section.classList.add('active');
        });
    });

    /* ---- Apply settings to UI ---- */
    function applySettings(s) {
        settings = s;

        // Text inputs
        setVal('executablePath', s.executablePath);
        setVal('sdkServerPath', s.sdkServerPath);
        setVal('runArgs', s.runArgs);
        setVal('lspPath', s.lspPath);
        setVal('mcpPath', s.mcpPath);
        setVal('taskfilePath', s.taskfilePath);

        // Selects
        setVal('optimizationLevel', s.optimizationLevel);
        setVal('lspTrace', s.lspTrace);
        setVal('traceChannels', s.traceChannels);

        // Toggles
        setCheck('stripDebugInfo', s.stripDebugInfo);
        setCheck('lspEnabled', s.lspEnabled);
        setCheck('mcpEnabled', s.mcpEnabled);
        setCheck('mcpSandbox', s.mcpSandbox);
        setCheck('terminalShellIntegration', s.terminalShellIntegration);
        setCheck('taskfileAutoDetect', s.taskfileAutoDetect);
        setCheck('traceEnabled', s.traceEnabled);

        // LSP status
        const dot = document.getElementById('lsp-status-dot');
        const txt = document.getElementById('lsp-status-text');
        if (dot && txt) {
            if (s.lspEnabled) {
                dot.className = 'status-dot on';
                txt.textContent = s.lspPath ? 'Enabled (' + s.lspPath + ')' : 'Enabled (auto-detect)';
            } else {
                dot.className = 'status-dot off';
                txt.textContent = 'Disabled';
            }
        }
    }

    function setVal(id, val) {
        const el = document.getElementById(id);
        if (el) el.value = val || '';
    }

    function setCheck(id, val) {
        const el = document.getElementById(id);
        if (el) el.checked = !!val;
    }

    /* ---- Event handlers ---- */
    // Text inputs — debounced
    let timers = {};
    document.querySelectorAll('input[type="text"]').forEach(input => {
        input.addEventListener('input', () => {
            const key = input.dataset.key;
            if (!key) return;
            clearTimeout(timers[key]);
            timers[key] = setTimeout(() => {
                vscode.postMessage({ type: 'updateSetting', key, value: input.value });
            }, 500);
        });
    });

    // Selects — immediate
    document.querySelectorAll('select').forEach(select => {
        select.addEventListener('change', () => {
            const key = select.dataset.key;
            if (!key) return;
            vscode.postMessage({ type: 'updateSetting', key, value: select.value });
        });
    });

    // Toggles — immediate
    document.querySelectorAll('.toggle input').forEach(toggle => {
        toggle.addEventListener('change', () => {
            const key = toggle.dataset.key;
            if (!key) return;
            vscode.postMessage({ type: 'updateSetting', key, value: toggle.checked });
        });
    });

    /* ---- File picker ---- */
    window.browseFile = function(inputId) {
        vscode.postMessage({ type: 'browseFile', key: inputId });
    };

    /* ---- External links ---- */
    window.openExternal = function(url) {
        vscode.postMessage({ type: 'openExternal', value: url });
    };

    /* ---- Run VS Code commands ---- */
    window.runCommand = function(cmd) {
        vscode.postMessage({ type: 'runCommand', value: cmd });
    };

    /* ---- Receive messages from extension ---- */
    window.addEventListener('message', (event) => {
        const msg = event.data;
        switch (msg.type) {
            case 'refresh':
                applySettings(msg.settings);
                break;
            case 'filePicked':
                const el = document.getElementById(msg.key);
                if (el) {
                    el.value = msg.path;
                    const key = el.dataset.key;
                    if (key) {
                        vscode.postMessage({ type: 'updateSetting', key, value: msg.path });
                    }
                }
                break;
        }
    });

    /* ---- Initial load ---- */
    applySettings(settings);
})();
</script>
</body>
</html>`;
    }
}
