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
import * as path from 'path';
import * as fs from 'fs';
import * as cp from 'child_process';
import {
    LanguageClient,
    LanguageClientOptions,
    ServerOptions,
    TransportKind,
} from 'vscode-languageclient/node';
import { NovaSettingsPanel } from './settingsPanel';
import { ThemeCustomizerPanel } from './themeCustomizer';
import { IconCustomizerPanel } from './iconCustomizer';
import { NovaToolboxProvider } from './toolboxProvider';

/* ================================================================
 * Configuration helpers
 * ================================================================ */

function getNovaPath(): string {
    return vscode.workspace.getConfiguration('nova').get<string>('executablePath', 'nova');
}

function getOptLevel(): string {
    return vscode.workspace.getConfiguration('nova').get<string>('optimizationLevel', '-O1');
}

function getStripFlag(): boolean {
    return vscode.workspace.getConfiguration('nova').get<boolean>('stripDebugInfo', false);
}

function getRunArgs(): string[] {
    return vscode.workspace.getConfiguration('nova').get<string[]>('runArgs', []);
}

function getTaskfilePath(): string {
    return vscode.workspace.getConfiguration('nova').get<string>('taskfile.path', 'taskfile.nini');
}

/** Shell-quote a string with single quotes (safe for paths/patterns with spaces) */
function sq(s: string): string {
    return `'${s.replace(/'/g, "'\\''")}'`;
}

/**
 * Get or create a reusable Nova tool terminal.
 * Reuses the most recently created "Nova Tools" terminal if it's still alive.
 */
let _novaToolTerminal: vscode.Terminal | undefined;

function getNovaToolTerminal(): vscode.Terminal {
    /* Check if the existing terminal is still alive */
    if (_novaToolTerminal) {
        const alive = vscode.window.terminals.includes(_novaToolTerminal);
        if (alive) {
            _novaToolTerminal.show();
            return _novaToolTerminal;
        }
        _novaToolTerminal = undefined;
    }

    /* Create a new one */
    const wsRoot = vscode.workspace.workspaceFolders?.[0]?.uri.fsPath;
    _novaToolTerminal = vscode.window.createTerminal({
        name: 'Nova Tools',
        cwd: wsRoot,
    });
    _novaToolTerminal.show();
    return _novaToolTerminal;
}

/* ================================================================
 * Status bar
 * ================================================================ */

let statusBarItem: vscode.StatusBarItem;

function createStatusBar(context: vscode.ExtensionContext): void {
    statusBarItem = vscode.window.createStatusBarItem(
        vscode.StatusBarAlignment.Left, 50
    );
    statusBarItem.text = '$(terminal) Nova';
    statusBarItem.tooltip = 'Nova SDK — Click to open Nova terminal';
    statusBarItem.command = 'nova.openTerminal';
    statusBarItem.show();
    context.subscriptions.push(statusBarItem);
}

/* ================================================================
 * Commands
 * ================================================================ */

function registerCommands(context: vscode.ExtensionContext): void {
    /* --- Run current file --- */
    context.subscriptions.push(
        vscode.commands.registerCommand('nova.runFile', () => {
            const editor = vscode.window.activeTextEditor;
            if (!editor) {
                vscode.window.showWarningMessage('No active Nova file to run.');
                return;
            }

            const filePath = editor.document.fileName;
            const ext = path.extname(filePath);
            if (ext !== '.n' && ext !== '.no' && ext !== '.m' && ext !== '.nova') {
                vscode.window.showWarningMessage('Active file is not a Nova source file.');
                return;
            }

            const nova = getNovaPath();
            const args = [...getRunArgs(), filePath];
            const terminal = vscode.window.createTerminal({
                name: `Nova: ${path.basename(filePath)}`,
                cwd: path.dirname(filePath),
            });
            terminal.show();
            terminal.sendText(`${nova} ${args.map(a => `"${a}"`).join(' ')}`);
        })
    );

    /* --- Compile current file --- */
    context.subscriptions.push(
        vscode.commands.registerCommand('nova.compileFile', () => {
            const editor = vscode.window.activeTextEditor;
            if (!editor) {
                vscode.window.showWarningMessage('No active Nova file to compile.');
                return;
            }

            const filePath = editor.document.fileName;
            const nova = getNovaPath();
            const optLevel = getOptLevel();
            const args = ['-c', filePath, optLevel];
            if (getStripFlag()) {
                args.push('-s');
            }

            const terminal = vscode.window.createTerminal({
                name: `Nova Compile: ${path.basename(filePath)}`,
                cwd: path.dirname(filePath),
            });
            terminal.show();
            terminal.sendText(`${nova} ${args.join(' ')}`);
        })
    );

    /* --- Disassemble current file --- */
    context.subscriptions.push(
        vscode.commands.registerCommand('nova.disassemble', () => {
            const editor = vscode.window.activeTextEditor;
            if (!editor) { return; }

            const filePath = editor.document.fileName;
            const nova = getNovaPath();

            const terminal = vscode.window.createTerminal({
                name: `Nova Disasm: ${path.basename(filePath)}`,
                cwd: path.dirname(filePath),
            });
            terminal.show();
            terminal.sendText(`${nova} --dis "${filePath}"`);
        })
    );

    /* --- Show AST --- */
    context.subscriptions.push(
        vscode.commands.registerCommand('nova.showAST', () => {
            const editor = vscode.window.activeTextEditor;
            if (!editor) { return; }

            const filePath = editor.document.fileName;
            const nova = getNovaPath();

            const terminal = vscode.window.createTerminal({
                name: `Nova AST: ${path.basename(filePath)}`,
                cwd: path.dirname(filePath),
            });
            terminal.show();
            terminal.sendText(`${nova} --ast "${filePath}"`);
        })
    );

    /* --- Explain error code --- */
    context.subscriptions.push(
        vscode.commands.registerCommand('nova.explainError', async () => {
            const code = await vscode.window.showInputBox({
                prompt: 'Enter Nova error code (e.g., E2001)',
                placeHolder: 'E2001',
                validateInput: (v) => /^E\d{4}$/.test(v) ? null : 'Format: E followed by 4 digits',
            });
            if (!code) { return; }

            const nova = getNovaPath();
            const terminal = vscode.window.createTerminal({ name: `Nova: ${code}` });
            terminal.show();
            terminal.sendText(`${nova} --explain ${code}`);
        })
    );

    /* --- Show version --- */
    context.subscriptions.push(
        vscode.commands.registerCommand('nova.showVersion', () => {
            const nova = getNovaPath();
            const terminal = vscode.window.createTerminal({ name: 'Nova Version' });
            terminal.show();
            terminal.sendText(`${nova} -V`);
        })
    );

    /* --- Open Nova terminal (interactive tool shell) --- */
    context.subscriptions.push(
        vscode.commands.registerCommand('nova.openTerminal', () => {
            const nova = getNovaPath();
            const useShell = vscode.workspace.getConfiguration('nova')
                .get<boolean>('terminal.shellIntegration', true);

            const workspaceFolder = vscode.workspace.workspaceFolders?.[0];
            const cwd = workspaceFolder?.uri.fsPath;

            if (useShell) {
                /* Launch Nova's interactive tool shell */
                const terminal = vscode.window.createTerminal({
                    name: 'Nova',
                    shellPath: nova,
                    shellArgs: [],
                    cwd,
                });
                terminal.show();
            } else {
                /* Just open a bash terminal with nova on PATH */
                const terminal = vscode.window.createTerminal({
                    name: 'Nova',
                    cwd,
                });
                terminal.show();
            }
        })
    );

    /* --- Open settings panel --- */
    context.subscriptions.push(
        vscode.commands.registerCommand('nova.openSettings', () => {
            NovaSettingsPanel.show(context);
        })
    );

    /* --- Theme customizer --- */
    context.subscriptions.push(
        vscode.commands.registerCommand('nova.customizeTheme', () => {
            ThemeCustomizerPanel.show(context.extensionUri);
        })
    );

    /* --- Icon customizer --- */
    context.subscriptions.push(
        vscode.commands.registerCommand('nova.customizeIcons', () => {
            IconCustomizerPanel.show(context.extensionUri);
        })
    );

    /* --- Run task --- */
    context.subscriptions.push(
        vscode.commands.registerCommand('nova.runTask', async () => {
            const nova = getNovaPath();
            const workspaceFolder = vscode.workspace.workspaceFolders?.[0];
            if (!workspaceFolder) {
                vscode.window.showWarningMessage('No workspace folder open.');
                return;
            }

            const taskfilePath = path.join(workspaceFolder.uri.fsPath, getTaskfilePath());
            if (!fs.existsSync(taskfilePath)) {
                vscode.window.showWarningMessage(`No taskfile found at ${taskfilePath}`);
                return;
            }

            /* Parse task names from taskfile */
            const content = fs.readFileSync(taskfilePath, 'utf-8');
            const taskRegex = /^\[task:([^\]]+)\]/gm;
            const tasks: string[] = [];
            let match;
            while ((match = taskRegex.exec(content)) !== null) {
                tasks.push(match[1]);
            }

            if (tasks.length === 0) {
                vscode.window.showInformationMessage('No tasks found in taskfile.');
                return;
            }

            const selected = await vscode.window.showQuickPick(tasks, {
                placeHolder: 'Select a Nova task to run',
                title: 'Nova Task Runner',
            });

            if (!selected) { return; }

            const terminal = vscode.window.createTerminal({
                name: `Nova Task: ${selected}`,
                cwd: workspaceFolder.uri.fsPath,
            });
            terminal.show();
            terminal.sendText(`${nova} task ${selected}`);
        })
    );
}

/* ================================================================
 * Task Provider — Auto-detect taskfile.nini
 * ================================================================ */

class NovaTaskProvider implements vscode.TaskProvider {
    static readonly type = 'nova';

    private taskPromise: Thenable<vscode.Task[]> | undefined;

    provideTasks(): Thenable<vscode.Task[]> {
        if (!this.taskPromise) {
            this.taskPromise = this.detectTasks();
        }
        return this.taskPromise;
    }

    resolveTask(task: vscode.Task): vscode.Task | undefined {
        const definition = task.definition;
        if (definition.type === NovaTaskProvider.type && definition.task) {
            return this.createTask(definition.task, definition.file);
        }
        return undefined;
    }

    private async detectTasks(): Promise<vscode.Task[]> {
        const tasks: vscode.Task[] = [];
        const workspaceFolders = vscode.workspace.workspaceFolders;
        if (!workspaceFolders) { return tasks; }

        for (const folder of workspaceFolders) {
            const taskfilePath = path.join(folder.uri.fsPath, getTaskfilePath());
            if (!fs.existsSync(taskfilePath)) { continue; }

            const content = fs.readFileSync(taskfilePath, 'utf-8');
            const taskRegex = /^\[task:([^\]]+)\]\s*\n(?:([^[]*?)(?=\n\[|\n*$))/gm;
            let match;

            /* Simpler regex to just get task names */
            const nameRegex = /^\[task:([^\]]+)\]/gm;
            while ((match = nameRegex.exec(content)) !== null) {
                const taskName = match[1];

                /* Try to extract description */
                const descRegex = new RegExp(
                    `\\[task:${taskName}\\][\\s\\S]*?description\\s*=\\s*(.+)`,
                    'm'
                );
                const descMatch = content.match(descRegex);
                const description = descMatch ? descMatch[1].trim() : taskName;

                const task = this.createTask(taskName, taskfilePath, folder, description);
                if (task) {
                    tasks.push(task);
                }
            }
        }

        return tasks;
    }

    private createTask(
        taskName: string,
        taskfile?: string,
        folder?: vscode.WorkspaceFolder,
        description?: string
    ): vscode.Task {
        const nova = getNovaPath();
        const definition: vscode.TaskDefinition = {
            type: NovaTaskProvider.type,
            task: taskName,
            file: taskfile,
        };

        const exec = new vscode.ShellExecution(`${nova} task ${taskName}`, {
            cwd: folder?.uri.fsPath,
        });

        const task = new vscode.Task(
            definition,
            folder ?? vscode.TaskScope.Workspace,
            description || taskName,
            'Nova',
            exec,
            '$nova'
        );

        /* Map known task types to VS Code task groups */
        if (taskName === 'build' || taskName === 'debug') {
            task.group = vscode.TaskGroup.Build;
        } else if (taskName === 'test') {
            task.group = vscode.TaskGroup.Test;
        } else if (taskName === 'clean') {
            task.group = vscode.TaskGroup.Clean;
        }

        task.presentationOptions = {
            reveal: vscode.TaskRevealKind.Always,
            panel: vscode.TaskPanelKind.Shared,
        };

        return task;
    }
}

/* ================================================================
 * LSP Client
 * ================================================================ */

let lspClient: LanguageClient | undefined;

/** Cached resolved path for nova-sdk-server (shared by LSP + MCP) */
let resolvedSdkServer: string | null | undefined;

/**
 * @brief Resolve the path to the nova-sdk-server binary.
 *
 * Search order:
 *   1. Explicit config (nova.sdkServerPath)
 *   2. Adjacent to nova executable (e.g., /usr/local/bin/nova-sdk-server)
 *   3. Bundled in extension (./bin/nova-sdk-server)
 *   4. Common install locations
 *   5. PATH lookup
 *
 * Falls back to legacy binary names (nova-lsp / nova-mcp) for
 * backward compatibility during migration.
 *
 * @param context  Extension context for bundled path
 * @return Resolved path, or null if not found
 */
function resolveSdkServer(
    context: vscode.ExtensionContext
): string | null {
    /* Return cached result if already resolved */
    if (resolvedSdkServer !== undefined) {
        return resolvedSdkServer;
    }

    const binName = 'nova-sdk-server';

    /* 1. Explicit config */
    const explicit = vscode.workspace.getConfiguration('nova')
        .get<string>('sdkServerPath', '');
    if (explicit && fs.existsSync(explicit)) {
        resolvedSdkServer = explicit;
        return explicit;
    }

    /* 2. Adjacent to nova executable */
    const novaPath = getNovaPath();
    const novaResolved = novaPath === 'nova'
        ? (() => { try { return cp.execSync('which nova 2>/dev/null', { encoding: 'utf-8' }).trim(); } catch { return ''; } })()
        : novaPath;
    if (novaResolved) {
        const adjacent = path.join(path.dirname(novaResolved), binName);
        if (fs.existsSync(adjacent)) {
            resolvedSdkServer = adjacent;
            return adjacent;
        }
    }

    /* 3. Try common install locations */
    const tryPaths = [
        path.join('/usr/local/bin', binName),
        path.join('/usr/bin', binName),
        path.join(context.extensionPath, 'bin', binName),
    ];

    const home = process.env.HOME || process.env.USERPROFILE || '';
    if (home) {
        tryPaths.push(path.join(home, '.local', 'bin', binName));
    }

    for (const p of tryPaths) {
        if (fs.existsSync(p)) {
            resolvedSdkServer = p;
            return p;
        }
    }

    /* 4. Fall back to PATH lookup */
    try {
        const which = cp.execSync(`which ${binName} 2>/dev/null`, {
            encoding: 'utf-8',
        }).trim();
        if (which) {
            resolvedSdkServer = which;
            return which;
        }
    } catch { /* not on PATH */ }

    resolvedSdkServer = null;
    return null;
}

function startLspClient(context: vscode.ExtensionContext,
                        outputChannel: vscode.OutputChannel): void {
    const enabled = vscode.workspace.getConfiguration('nova')
        .get<boolean>('lsp.enabled', true);
    if (!enabled) {
        outputChannel.appendLine('Nova LSP: disabled by configuration');
        return;
    }

    const serverPath = resolveSdkServer(context);
    if (!serverPath) {
        outputChannel.appendLine(
            'Nova LSP: nova-sdk-server not found — diagnostics unavailable. ' +
            'Build nova-sdk and ensure nova-sdk-server is on PATH or set ' +
            'nova.sdkServerPath in settings.'
        );
        return;
    }

    outputChannel.appendLine(`Nova LSP: using ${serverPath} --lsp`);

    const logArg = vscode.workspace.getConfiguration('nova')
        .get<string>('lsp.trace', 'off') !== 'off'
        ? ['--log', path.join(context.logUri.fsPath, 'nova-lsp.log')]
        : [];

    /* Build workspace arg if available */
    const wsFolder = vscode.workspace.workspaceFolders?.[0]?.uri.fsPath;
    const wsArg = wsFolder ? ['--workspace', wsFolder] : [];

    const serverOptions: ServerOptions = {
        run: {
            command: serverPath,
            args: ['--lsp', '--stdio', ...wsArg, ...logArg],
            transport: TransportKind.stdio,
        },
        debug: {
            command: serverPath,
            args: ['--lsp', '--stdio', ...wsArg, '--log',
                   path.join(context.logUri.fsPath, 'nova-lsp.log')],
            transport: TransportKind.stdio,
        },
    };

    const clientOptions: LanguageClientOptions = {
        documentSelector: [
            { scheme: 'file', language: 'nova' },
            { scheme: 'file', language: 'nova-header' },
            { scheme: 'file', language: 'nini' },
            { scheme: 'file', language: 'nini-taskfile' },
        ],
        synchronize: {
            fileEvents: vscode.workspace.createFileSystemWatcher(
                '**/*.{n,m,no,nini,ni}'
            ),
        },
        outputChannel,
        traceOutputChannel: outputChannel,
    };

    lspClient = new LanguageClient(
        'nova-sdk-server',
        'Nova Language Server',
        serverOptions,
        clientOptions
    );

    lspClient.start().then(() => {
        outputChannel.appendLine('Nova LSP: started');
    }, (err: Error) => {
        outputChannel.appendLine(`Nova LSP: failed to start — ${err.message}`);
        lspClient = undefined;
    });

    context.subscriptions.push({
        dispose: () => {
            if (lspClient) {
                lspClient.stop();
                lspClient = undefined;
            }
        },
    });
}

/* ================================================================
 * MCP Server Configuration
 * ================================================================ */

/**
 * @brief Register the Nova MCP server with VS Code's Copilot MCP
 *        configuration if the binary is available.
 *
 * This writes to .vscode/mcp.json in the workspace root so that
 * GitHub Copilot and other MCP clients can discover the server.
 */
function configureMcpServer(context: vscode.ExtensionContext,
                            outputChannel: vscode.OutputChannel): void {
    const enabled = vscode.workspace.getConfiguration('nova')
        .get<boolean>('mcp.enabled', true);
    if (!enabled) {
        outputChannel.appendLine('Nova MCP: disabled by configuration');
        return;
    }

    const serverPath = resolveSdkServer(context);
    if (!serverPath) {
        outputChannel.appendLine(
            'Nova MCP: nova-sdk-server not found — AI tools unavailable. ' +
            'Build nova-sdk and ensure nova-sdk-server is on PATH or set ' +
            'nova.sdkServerPath in settings.'
        );
        return;
    }

    const sandbox = vscode.workspace.getConfiguration('nova')
        .get<boolean>('mcp.sandbox', true);

    /* Generate MCP server config for .vscode/mcp.json */
    const workspaceFolder = vscode.workspace.workspaceFolders?.[0];
    if (!workspaceFolder) {
        outputChannel.appendLine('Nova MCP: no workspace folder — skipping');
        return;
    }

    const args = ['--mcp', '--stdio',
                  '--workspace', workspaceFolder.uri.fsPath];
    if (!sandbox) { args.push('--no-sandbox'); }

    const mcpDir = path.join(workspaceFolder.uri.fsPath, '.vscode');
    const mcpFile = path.join(mcpDir, 'mcp.json');

    /* Read existing mcp.json if present */
    let mcpConfig: Record<string, unknown> = {};
    if (fs.existsSync(mcpFile)) {
        try {
            const raw = fs.readFileSync(mcpFile, 'utf-8');
            mcpConfig = JSON.parse(raw);
        } catch { /* start fresh */ }
    }

    /* Add/update nova-sdk-server MCP entry */
    const servers = (mcpConfig['servers'] as Record<string, unknown>) ?? {};

    /* Remove legacy nova-mcp entry if present */
    delete servers['nova-mcp'];

    servers['nova-sdk-server'] = {
        command: serverPath,
        args,
        type: 'stdio',
    };
    mcpConfig['servers'] = servers;

    /* Write back */
    if (!fs.existsSync(mcpDir)) {
        fs.mkdirSync(mcpDir, { recursive: true });
    }
    fs.writeFileSync(mcpFile, JSON.stringify(mcpConfig, null, 4) + '\n',
                     'utf-8');

    outputChannel.appendLine(
        `Nova MCP: configured in ${mcpFile} (sandbox=${sandbox ? 'ON' : 'OFF'}, binary=${serverPath})`
    );
}

class NovaTerminalProfileProvider implements vscode.TerminalProfileProvider {
    provideTerminalProfile(): vscode.ProviderResult<vscode.TerminalProfile> {
        const nova = getNovaPath();
        return new vscode.TerminalProfile({
            name: 'Nova',
            shellPath: nova,
            shellArgs: [],
            iconPath: new vscode.ThemeIcon('terminal'),
        });
    }
}

/* ================================================================
 * Nova Sidebar Dashboard
 * ================================================================ */

class NovaDashboardViewProvider implements vscode.WebviewViewProvider {
    public static readonly viewType = 'nova.dashboardView';
    private _context: vscode.ExtensionContext;

    constructor(context: vscode.ExtensionContext) {
        this._context = context;
    }

    resolveWebviewView(
        webviewView: vscode.WebviewView,
    ): void {
        webviewView.webview.options = { enableScripts: true };

        const nova = getNovaPath();
        const lspEnabled = vscode.workspace.getConfiguration('nova').get<boolean>('lsp.enabled', true);
        const mcpEnabled = vscode.workspace.getConfiguration('nova').get<boolean>('mcp.enabled', true);

        webviewView.webview.html = `<!DOCTYPE html>
<html>
<head>
<style>
body {
    font-family: var(--vscode-font-family);
    font-size: var(--vscode-font-size);
    color: var(--vscode-foreground);
    background: var(--vscode-sideBar-background);
    padding: 12px;
    margin: 0;
}
h2 {
    font-size: 13px;
    font-weight: 600;
    text-transform: uppercase;
    letter-spacing: 0.5px;
    margin: 0 0 12px 0;
    color: var(--vscode-sideBarTitle-foreground);
}
.btn {
    display: flex;
    align-items: center;
    gap: 8px;
    width: 100%;
    padding: 6px 10px;
    margin: 4px 0;
    border: 1px solid var(--vscode-button-border, transparent);
    background: var(--vscode-button-secondaryBackground);
    color: var(--vscode-button-secondaryForeground);
    font-size: 12px;
    cursor: pointer;
    border-radius: 3px;
    text-align: left;
    box-sizing: border-box;
}
.btn:hover {
    background: var(--vscode-button-secondaryHoverBackground);
}
.btn-primary {
    background: var(--vscode-button-background);
    color: var(--vscode-button-foreground);
}
.btn-primary:hover {
    background: var(--vscode-button-hoverBackground);
}
.status {
    font-size: 11px;
    padding: 4px 0;
    color: var(--vscode-descriptionForeground);
}
.status-dot {
    display: inline-block;
    width: 8px;
    height: 8px;
    border-radius: 50%;
    margin-right: 6px;
}
.dot-on { background: #00FF41; }
.dot-off { background: #FF0055; }
hr {
    border: none;
    border-top: 1px solid var(--vscode-sideBar-border, var(--vscode-panel-border));
    margin: 12px 0;
}
</style>
</head>
<body>
<h2>Nova SDK</h2>

<button class="btn btn-primary" onclick="cmd('nova.runFile')">&#9654; Run File</button>
<button class="btn" onclick="cmd('nova.compileFile')">&#9881; Compile</button>
<button class="btn" onclick="cmd('nova.runTask')">&#9776; Run Task</button>
<button class="btn" onclick="cmd('nova.openTerminal')">&#62;&gt;_ Terminal</button>
<button class="btn" onclick="cmd('nova.disassemble')">&#128270; Disassemble</button>
<button class="btn" onclick="cmd('nova.showAST')">&#127795; Show AST</button>

<hr/>

<h2>Services</h2>
<div class="status"><span class="status-dot ${lspEnabled ? 'dot-on' : 'dot-off'}"></span>LSP Server</div>
<div class="status"><span class="status-dot ${mcpEnabled ? 'dot-on' : 'dot-off'}"></span>MCP Server</div>

<hr/>

<button class="btn" onclick="cmd('nova.openSettings')">&#9881; Settings</button>
<button class="btn" onclick="cmd('nova.explainError')">&#10067; Explain Error</button>
<button class="btn" onclick="cmd('nova.showVersion')">&#8505; Version</button>

<script>
const vscode = acquireVsCodeApi();
function cmd(id) { vscode.postMessage({ command: 'execute', id: id }); }
</script>
</body>
</html>`;

        webviewView.webview.onDidReceiveMessage((msg) => {
            if (msg.command === 'execute' && msg.id) {
                vscode.commands.executeCommand(msg.id);
            }
        });
    }
}

/* ================================================================
 * Extension Lifecycle
 * ================================================================ */

export function activate(context: vscode.ExtensionContext): void {
    /* Output channel */
    const outputChannel = vscode.window.createOutputChannel('Nova SDK');
    context.subscriptions.push(outputChannel);

    /* Startup diagnostics */
    const cfg = vscode.workspace.getConfiguration('nova');
    const isRemote = vscode.env.remoteName !== undefined;
    outputChannel.appendLine(`Nova SDK v0.2.0 activating (remote=${isRemote ? vscode.env.remoteName : 'no'})`);
    outputChannel.appendLine(`  extensionPath: ${context.extensionPath}`);
    outputChannel.appendLine(`  nova.mcp.enabled: ${cfg.get('mcp.enabled')}`);
    outputChannel.appendLine(`  nova.lsp.enabled: ${cfg.get('lsp.enabled', true)}`);
    outputChannel.appendLine(`  nova.sdkServerPath: ${cfg.get('sdkServerPath', '(not set)')}`);
    outputChannel.appendLine(`  nova.executablePath: ${cfg.get('executablePath', 'nova')}`);

    /* Warn if *.n files are not mapped to Nova language */
    const fileAssoc = vscode.workspace.getConfiguration('files')
        .get<Record<string, string>>('associations', {});
    if (fileAssoc['*.n'] && fileAssoc['*.n'] !== 'nova') {
        const msg = `Warning: *.n files are mapped to "${fileAssoc['*.n']}" instead of "nova". ` +
            'Nova language features will not work for .n files. ' +
            'Update files.associations in your workspace settings.';
        outputChannel.appendLine(`  ⚠ ${msg}`);
        vscode.window.showWarningMessage(msg, 'Fix Now').then(choice => {
            if (choice === 'Fix Now') {
                vscode.workspace.getConfiguration('files').update(
                    'associations',
                    { ...fileAssoc, '*.n': 'nova' },
                    vscode.ConfigurationTarget.Workspace
                );
            }
        });
    }

    /* Status bar */
    createStatusBar(context);

    /* Commands */
    registerCommands(context);

    /* Task provider */
    const autoDetect = vscode.workspace.getConfiguration('nova')
        .get<boolean>('taskfile.autoDetect', true);
    if (autoDetect) {
        const taskProvider = new NovaTaskProvider();
        context.subscriptions.push(
            vscode.tasks.registerTaskProvider(NovaTaskProvider.type, taskProvider)
        );
    }

    /* Terminal profile */
    context.subscriptions.push(
        vscode.window.registerTerminalProfileProvider(
            'nova.terminal',
            new NovaTerminalProfileProvider()
        )
    );

    /* Nova sidebar dashboard */
    context.subscriptions.push(
        vscode.window.registerWebviewViewProvider(
            'nova.dashboardView',
            new NovaDashboardViewProvider(context)
        )
    );

    /* Nova toolbox tree view */
    const toolboxProvider = new NovaToolboxProvider(context);
    const toolboxView = vscode.window.createTreeView('nova.toolboxView', {
        treeDataProvider: toolboxProvider,
        showCollapseAll: true,
    });
    context.subscriptions.push(toolboxView);

    /* Toolbox commands */
    context.subscriptions.push(
        vscode.commands.registerCommand('nova.refreshToolbox', () => {
            toolboxProvider.refresh();
        })
    );

    context.subscriptions.push(
        vscode.commands.registerCommand('nova.runNamedTask', async (taskName: string) => {
            const nova = getNovaPath();
            const terminal = getNovaToolTerminal();
            terminal.sendText(`${nova} task ${taskName}`);
        })
    );

    context.subscriptions.push(
        vscode.commands.registerCommand('nova.runBuiltinTool', async (toolName: string) => {
            const nova = getNovaPath();
            const editor = vscode.window.activeTextEditor;
            const activeFile = editor?.document.fileName;

            const cmdParts: string[] = [nova, toolName];

            /* Tools that require a file argument */
            if (['cat', 'head', 'tail', 'wc'].includes(toolName)) {
                let target = activeFile;
                if (!target) {
                    const uris = await vscode.window.showOpenDialog({
                        canSelectMany: false,
                        title: `Select file for nova ${toolName}`,
                    });
                    if (!uris || uris.length === 0) { return; }
                    target = uris[0].fsPath;
                }
                cmdParts.push(sq(target));
            } else if (toolName === 'grep') {
                const sel = editor?.document.getText(editor.selection);
                const pattern = await vscode.window.showInputBox({
                    prompt: 'Search pattern',
                    value: sel && sel.length > 0 && sel.length < 200 ? sel : '',
                    placeHolder: 'TODO',
                });
                if (!pattern) { return; }
                cmdParts.push(activeFile ? sq(activeFile) : '.');
                cmdParts.push(`-m=${sq(pattern)}`, '-r');
            } else if (toolName === 'find') {
                const pattern = await vscode.window.showInputBox({
                    prompt: 'File glob pattern',
                    placeHolder: '*.c',
                });
                if (!pattern) { return; }
                cmdParts.push('.', `-m=${sq(pattern)}`);
            }
            /* ls, tree, pwd — no args needed */

            const terminal = getNovaToolTerminal();
            terminal.sendText(cmdParts.join(' '));
        })
    );

    context.subscriptions.push(
        vscode.commands.registerCommand('nova.runUserScript', async (arg: unknown) => {
            /* arg may be a string (from commandArgs) or a ToolboxItem (from inline context menu) */
            let scriptPath: string;
            if (typeof arg === 'string') {
                scriptPath = arg;
            } else if (arg && typeof arg === 'object' && 'data' in arg) {
                scriptPath = (arg as { data: { scriptPath?: string } }).data.scriptPath || '';
            } else {
                vscode.window.showWarningMessage('No script path provided.');
                return;
            }
            if (!scriptPath) {
                vscode.window.showWarningMessage('Script path is empty.');
                return;
            }

            const nova = getNovaPath();
            const terminal = getNovaToolTerminal();
            terminal.sendText(`${nova} ${sq(scriptPath)}`);
        })
    );

    context.subscriptions.push(
        vscode.commands.registerCommand('nova.cdToFolder', async (resource?: vscode.Uri) => {
            let targetDir: string | undefined;

            if (resource) {
                /* Called from explorer context menu — resource is the URI */
                const fsPath = resource.fsPath;
                try {
                    const stat = fs.statSync(fsPath);
                    targetDir = stat.isDirectory() ? fsPath : path.dirname(fsPath);
                } catch {
                    targetDir = path.dirname(fsPath);
                }
            } else {
                /* Called from command palette — pick a folder */
                const uris = await vscode.window.showOpenDialog({
                    canSelectFiles: false,
                    canSelectFolders: true,
                    canSelectMany: false,
                    title: 'Select directory to cd into',
                });
                if (!uris || uris.length === 0) { return; }
                targetDir = uris[0].fsPath;
            }

            if (!targetDir) { return; }

            const terminal = getNovaToolTerminal();
            terminal.sendText(`cd ${sq(targetDir)}`);
        })
    );

    context.subscriptions.push(
        vscode.commands.registerCommand('nova.addUserScript', async () => {
            const wsRoot = vscode.workspace.workspaceFolders?.[0]?.uri.fsPath;
            if (!wsRoot) {
                vscode.window.showWarningMessage('No workspace folder open.');
                return;
            }

            const choice = await vscode.window.showQuickPick(
                ['Create new script', 'Link existing file'],
                { placeHolder: 'Add a script to the toolbox' },
            );

            if (choice === 'Create new script') {
                const name = await vscode.window.showInputBox({
                    prompt: 'Script name (without .n extension)',
                    placeHolder: 'my_tool',
                    validateInput: (v) => /^[a-zA-Z_]\w*$/.test(v) ? null : 'Invalid name',
                });
                if (!name) { return; }

                const toolsDir = path.join(wsRoot, '.nova', 'tools');
                if (!fs.existsSync(toolsDir)) {
                    fs.mkdirSync(toolsDir, { recursive: true });
                }

                const filePath = path.join(toolsDir, `${name}.n`);
                if (fs.existsSync(filePath)) {
                    vscode.window.showWarningMessage(`Script ${name}.n already exists.`);
                    return;
                }

                fs.writeFileSync(filePath,
                    `-- ${name}.n — Nova toolbox script\n` +
                    `-- Created: ${new Date().toISOString().split('T')[0]}\n\n` +
                    `echo("Hello from ${name}!")\n`,
                    'utf8',
                );

                const doc = await vscode.workspace.openTextDocument(filePath);
                await vscode.window.showTextDocument(doc);
                toolboxProvider.refresh();

            } else if (choice === 'Link existing file') {
                const files = await vscode.window.showOpenDialog({
                    canSelectFiles: true,
                    canSelectMany: false,
                    filters: { 'Nova Scripts': ['n'] },
                    title: 'Select a Nova script',
                });
                if (!files || !files[0]) { return; }

                const toolsDir = path.join(wsRoot, '.nova', 'tools');
                if (!fs.existsSync(toolsDir)) {
                    fs.mkdirSync(toolsDir, { recursive: true });
                }

                const srcPath = files[0].fsPath;
                const destPath = path.join(toolsDir, path.basename(srcPath));
                fs.copyFileSync(srcPath, destPath);
                toolboxProvider.refresh();
                vscode.window.showInformationMessage(`Added ${path.basename(srcPath)} to toolbox.`);
            }
        })
    );

    /* LSP client */
    startLspClient(context, outputChannel);

    /* MCP server configuration */
    configureMcpServer(context, outputChannel);

    /* Watch for taskfile changes to refresh tasks */
    const taskfileWatcher = vscode.workspace.createFileSystemWatcher(
        '**/taskfile.nini'
    );
    taskfileWatcher.onDidChange(() => {
        vscode.commands.executeCommand('workbench.action.tasks.refreshTasks');
    });
    taskfileWatcher.onDidCreate(() => {
        vscode.commands.executeCommand('workbench.action.tasks.refreshTasks');
    });
    taskfileWatcher.onDidDelete(() => {
        vscode.commands.executeCommand('workbench.action.tasks.refreshTasks');
    });
    context.subscriptions.push(taskfileWatcher);

    /* Watch for config changes to restart LSP */
    context.subscriptions.push(
        vscode.workspace.onDidChangeConfiguration((e) => {
            if (e.affectsConfiguration('nova.lsp')) {
                outputChannel.appendLine('Nova LSP: config changed, restarting...');
                if (lspClient) {
                    lspClient.stop().then(() => {
                        lspClient = undefined;
                        startLspClient(context, outputChannel);
                    });
                } else {
                    startLspClient(context, outputChannel);
                }
            }
            if (e.affectsConfiguration('nova.mcp')) {
                configureMcpServer(context, outputChannel);
            }
        })
    );

    /* Log activation */
    outputChannel.appendLine('Nova SDK activated.');
    outputChannel.appendLine(`  Executable: ${getNovaPath()}`);
    outputChannel.appendLine(`  Taskfile: ${getTaskfilePath()}`);
}

export async function deactivate(): Promise<void> {
    if (lspClient) {
        await lspClient.stop();
        lspClient = undefined;
    }
}
