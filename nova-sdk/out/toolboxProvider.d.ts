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
export declare class ToolboxItem extends vscode.TreeItem {
    readonly data: ToolboxItemData;
    constructor(data: ToolboxItemData);
}
export declare class NovaToolboxProvider implements vscode.TreeDataProvider<ToolboxItem> {
    private _onDidChangeTreeData;
    readonly onDidChangeTreeData: vscode.Event<void | ToolboxItem | undefined>;
    private _context;
    private _watcher;
    constructor(context: vscode.ExtensionContext);
    refresh(): void;
    getTreeItem(element: ToolboxItem): vscode.TreeItem;
    getChildren(element?: ToolboxItem): ToolboxItem[];
    dispose(): void;
    private _setupWatchers;
    private _getRootItems;
    private _buildQuickActions;
    private _buildTasks;
    private _buildBuiltinTools;
    private _buildUserScripts;
    private _buildSettings;
    /** Parse taskfile.nini for [task:NAME] sections */
    private _discoverTasks;
    /** Scan .nova/tools/, .nova/scripts/, and tools/ for .n files */
    private _discoverUserScripts;
}
export {};
//# sourceMappingURL=toolboxProvider.d.ts.map