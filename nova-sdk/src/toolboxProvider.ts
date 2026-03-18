/**
 * @file    toolboxProvider.ts
 * @brief   Nova Toolbox — File Explorer tree view
 * @author  Anthony Taliento (Zorya Corporation)
 *
 * Provides a persistent tree view in the Nova sidebar:
 *   - Quick Actions (run, compile, disassemble, AST)
 *   - Tasks (discovered from taskfile.nini)
 *   - Built-in Tools (cat, grep, find, wc, etc.)
 *   - User Scripts (.nova/tools/*.n)
 *   - Settings shortcuts (theme customizer, icons, settings)
 */

import * as vscode from 'vscode';
import * as path from 'path';
import * as fs from 'fs';

/* ================================================================
 *  Types
 * ================================================================ */

type ItemKind = 'group' | 'action' | 'task' | 'tool' | 'script' | 'setting';

interface ToolboxItemData {
    label: string;
    kind: ItemKind;
    icon?: string;
    command?: string;
    commandArgs?: unknown[];
    tooltip?: string;
    children?: ToolboxItemData[];
    /** For scripts: absolute path to the .n file */
    scriptPath?: string;
}

/* ================================================================
 *  Tree Item
 * ================================================================ */

export class ToolboxItem extends vscode.TreeItem {
    public readonly data: ToolboxItemData;

    constructor(data: ToolboxItemData) {
        const collapsible = data.children && data.children.length > 0
            ? vscode.TreeItemCollapsibleState.Collapsed
            : vscode.TreeItemCollapsibleState.None;

        super(data.label, collapsible);
        this.data = data;
        this.tooltip = data.tooltip || data.label;
        this.contextValue = data.kind;

        /* Icon */
        if (data.icon) {
            this.iconPath = new vscode.ThemeIcon(data.icon);
        }

        /* Command (leaf nodes only) */
        if (!data.children || data.children.length === 0) {
            if (data.command) {
                this.command = {
                    title: data.label,
                    command: data.command,
                    arguments: data.commandArgs,
                };
            }
        }

        /* Description for scripts — show relative path */
        if (data.kind === 'script' && data.scriptPath) {
            const wsRoot = vscode.workspace.workspaceFolders?.[0]?.uri.fsPath;
            if (wsRoot) {
                this.description = path.relative(wsRoot, data.scriptPath);
            }
        }
    }
}

/* ================================================================
 *  Tree Data Provider
 * ================================================================ */

export class NovaToolboxProvider implements vscode.TreeDataProvider<ToolboxItem> {
    private _onDidChangeTreeData = new vscode.EventEmitter<ToolboxItem | undefined | void>();
    readonly onDidChangeTreeData = this._onDidChangeTreeData.event;

    private _context: vscode.ExtensionContext;
    private _watcher: vscode.FileSystemWatcher | undefined;

    constructor(context: vscode.ExtensionContext) {
        this._context = context;
        this._setupWatchers();
    }

    refresh(): void {
        this._onDidChangeTreeData.fire();
    }

    getTreeItem(element: ToolboxItem): vscode.TreeItem {
        return element;
    }

    getChildren(element?: ToolboxItem): ToolboxItem[] {
        if (!element) {
            return this._getRootItems();
        }
        return (element.data.children || []).map((c) => new ToolboxItem(c));
    }

    dispose(): void {
        this._watcher?.dispose();
        this._onDidChangeTreeData.dispose();
    }

    /* ---- File watchers for auto-refresh ---- */
    private _setupWatchers(): void {
        /* Watch for taskfile changes */
        const pattern = new vscode.RelativePattern(
            vscode.workspace.workspaceFolders?.[0] || '',
            '{taskfile.nini,Taskfile.nini,.nova/tools/**/*.n,.nova/scripts/**/*.n,tools/**/*.n}',
        );
        this._watcher = vscode.workspace.createFileSystemWatcher(pattern);
        this._watcher.onDidChange(() => this.refresh());
        this._watcher.onDidCreate(() => this.refresh());
        this._watcher.onDidDelete(() => this.refresh());
    }

    /* ---- Root groups ---- */
    private _getRootItems(): ToolboxItem[] {
        const items: ToolboxItem[] = [];

        items.push(new ToolboxItem(this._buildQuickActions()));
        items.push(new ToolboxItem(this._buildTasks()));
        items.push(new ToolboxItem(this._buildBuiltinTools()));
        items.push(new ToolboxItem(this._buildUserScripts()));
        items.push(new ToolboxItem(this._buildSettings()));

        return items;
    }

    /* ---- Quick Actions ---- */
    private _buildQuickActions(): ToolboxItemData {
        return {
            label: 'Quick Actions',
            kind: 'group',
            icon: 'zap',
            children: [
                {
                    label: 'Run Current File',
                    kind: 'action',
                    icon: 'play',
                    command: 'nova.runFile',
                    tooltip: 'Execute the active Nova script',
                },
                {
                    label: 'Compile Current File',
                    kind: 'action',
                    icon: 'gear',
                    command: 'nova.compileFile',
                    tooltip: 'Compile without executing',
                },
                {
                    label: 'Disassemble',
                    kind: 'action',
                    icon: 'file-binary',
                    command: 'nova.disassemble',
                    tooltip: 'Show bytecode disassembly',
                },
                {
                    label: 'Show AST',
                    kind: 'action',
                    icon: 'type-hierarchy',
                    command: 'nova.showAST',
                    tooltip: 'Display abstract syntax tree',
                },
                {
                    label: 'Open Terminal',
                    kind: 'action',
                    icon: 'terminal',
                    command: 'nova.openTerminal',
                    tooltip: 'Open a Nova terminal',
                },
            ],
        };
    }

    /* ---- Tasks (from taskfile.nini) ---- */
    private _buildTasks(): ToolboxItemData {
        const tasks = this._discoverTasks();
        const children: ToolboxItemData[] = tasks.map((t) => ({
            label: t,
            kind: 'task' as ItemKind,
            icon: 'tasklist',
            command: 'nova.runNamedTask',
            commandArgs: [t],
            tooltip: `Run task: ${t}`,
        }));

        if (children.length === 0) {
            children.push({
                label: 'No taskfile found',
                kind: 'task',
                icon: 'info',
                tooltip: 'Create a taskfile.nini to define tasks',
            });
        }

        return {
            label: 'Tasks',
            kind: 'group',
            icon: 'checklist',
            children,
        };
    }

    /* ---- Built-in Tools ---- */
    private _buildBuiltinTools(): ToolboxItemData {
        const tools = [
            { name: 'cat',  desc: 'Print file contents',        icon: 'file' },
            { name: 'ls',   desc: 'List directory',              icon: 'folder-opened' },
            { name: 'tree', desc: 'Directory tree',              icon: 'list-tree' },
            { name: 'find', desc: 'Find files by pattern',       icon: 'search' },
            { name: 'grep', desc: 'Search text in files',        icon: 'search-fuzzy' },
            { name: 'head', desc: 'First N lines of a file',     icon: 'arrow-up' },
            { name: 'tail', desc: 'Last N lines of a file',      icon: 'arrow-down' },
            { name: 'wc',   desc: 'Line/word/char counts',       icon: 'symbol-number' },
            { name: 'pwd',  desc: 'Print working directory',     icon: 'home' },
        ];

        return {
            label: 'Built-in Tools',
            kind: 'group',
            icon: 'tools',
            children: tools.map((t) => ({
                label: t.name,
                kind: 'tool' as ItemKind,
                icon: t.icon,
                command: 'nova.runBuiltinTool',
                commandArgs: [t.name],
                tooltip: t.desc,
            })),
        };
    }

    /* ---- User Scripts ---- */
    private _buildUserScripts(): ToolboxItemData {
        const scripts = this._discoverUserScripts();
        const children: ToolboxItemData[] = scripts.map((s) => ({
            label: path.basename(s, '.n'),
            kind: 'script' as ItemKind,
            icon: 'file-code',
            command: 'nova.runUserScript',
            commandArgs: [s],
            tooltip: s,
            scriptPath: s,
        }));

        /* Always add the "Add Script" action at the end */
        children.push({
            label: 'Add Script...',
            kind: 'action',
            icon: 'add',
            command: 'nova.addUserScript',
            tooltip: 'Create or link a Nova script to the toolbox',
        });

        return {
            label: 'User Scripts',
            kind: 'group',
            icon: 'library',
            children,
        };
    }

    /* ---- Settings ---- */
    private _buildSettings(): ToolboxItemData {
        return {
            label: 'Customize',
            kind: 'group',
            icon: 'paintcan',
            children: [
                {
                    label: 'Theme & Syntax Colors',
                    kind: 'setting',
                    icon: 'symbol-color',
                    command: 'nova.customizeTheme',
                },
                {
                    label: 'Custom File Icons',
                    kind: 'setting',
                    icon: 'file-media',
                    command: 'nova.customizeIcons',
                },
                {
                    label: 'Extension Settings',
                    kind: 'setting',
                    icon: 'settings-gear',
                    command: 'nova.openSettings',
                },
            ],
        };
    }

    /* ================================================================
     *  Discovery
     * ================================================================ */

    /** Parse taskfile.nini for [task:NAME] sections */
    private _discoverTasks(): string[] {
        const wsRoot = vscode.workspace.workspaceFolders?.[0]?.uri.fsPath;
        if (!wsRoot) { return []; }

        const cfg = vscode.workspace.getConfiguration('nova');
        const taskfileName = cfg.get<string>('taskfile.path', 'taskfile.nini');
        const taskfilePath = path.join(wsRoot, taskfileName);

        if (!fs.existsSync(taskfilePath)) { return []; }

        const content = fs.readFileSync(taskfilePath, 'utf8');
        const tasks: string[] = [];
        const re = /^\[task:(\w+)\]/gm;
        let match;
        while ((match = re.exec(content)) !== null) {
            tasks.push(match[1]);
        }
        return tasks;
    }

    /** Scan .nova/tools/, .nova/scripts/, and tools/ for .n files */
    private _discoverUserScripts(): string[] {
        const wsRoot = vscode.workspace.workspaceFolders?.[0]?.uri.fsPath;
        if (!wsRoot) { return []; }

        const scripts: string[] = [];
        const dirs = [
            path.join(wsRoot, '.nova', 'tools'),
            path.join(wsRoot, '.nova', 'scripts'),
            path.join(wsRoot, 'tools'),
        ];

        for (const dir of dirs) {
            if (!fs.existsSync(dir)) { continue; }
            try {
                const entries = fs.readdirSync(dir, { withFileTypes: true });
                for (const ent of entries) {
                    if (ent.isFile() && ent.name.endsWith('.n')) {
                        scripts.push(path.join(dir, ent.name));
                    }
                }
            } catch {
                /* permission issues — skip silently */
            }
        }

        return scripts;
    }
}
