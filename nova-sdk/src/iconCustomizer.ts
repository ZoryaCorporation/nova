/**
 * @file    iconCustomizer.ts
 * @brief   Custom file icon manager — add icons for any file extension
 * @author  Anthony Taliento (Zorya Corporation)
 *
 * Provides a webview UI for:
 *  - Viewing all current file-extension → icon mappings
 *  - Adding new mappings (pick an SVG, assign to an extension)
 *  - Removing custom mappings
 *  - Importing SVG icons into the extension's icons/ directory
 *  - Live-updates the nova-icon-theme.json
 */

import * as vscode from 'vscode';
import * as path from 'path';
import * as fs from 'fs';

/* ------------------------------------------------------------------ */
/*  Icon theme JSON shape (subset we care about)                       */
/* ------------------------------------------------------------------ */

interface IconDef { iconPath: string }
interface IconTheme {
    hidesExplorerArrows?: boolean;
    iconDefinitions: Record<string, IconDef>;
    file?: string;
    folder?: string;
    folderExpanded?: string;
    fileExtensions?: Record<string, string>;
    fileNames?: Record<string, string>;
    languageIds?: Record<string, string>;
    light?: {
        file?: string;
        folder?: string;
        folderExpanded?: string;
        fileExtensions?: Record<string, string>;
        fileNames?: Record<string, string>;
        languageIds?: Record<string, string>;
    };
}

/* ------------------------------------------------------------------ */
/*  Webview Panel                                                      */
/* ------------------------------------------------------------------ */

export class IconCustomizerPanel {
    public static readonly viewType = 'novaIconCustomizer';
    private static _instance: IconCustomizerPanel | undefined;

    private readonly _panel: vscode.WebviewPanel;
    private readonly _extensionUri: vscode.Uri;
    private _disposables: vscode.Disposable[] = [];

    public static show(extensionUri: vscode.Uri): void {
        if (IconCustomizerPanel._instance) {
            IconCustomizerPanel._instance._panel.reveal();
            return;
        }
        const panel = vscode.window.createWebviewPanel(
            IconCustomizerPanel.viewType,
            'Custom File Icons',
            vscode.ViewColumn.One,
            { enableScripts: true, retainContextWhenHidden: true },
        );
        IconCustomizerPanel._instance = new IconCustomizerPanel(panel, extensionUri);
    }

    private constructor(panel: vscode.WebviewPanel, extensionUri: vscode.Uri) {
        this._panel = panel;
        this._extensionUri = extensionUri;
        this._panel.webview.html = this._getHtml();

        this._panel.webview.onDidReceiveMessage(
            (msg) => this._handleMessage(msg),
            undefined,
            this._disposables,
        );

        this._panel.onDidDispose(() => {
            IconCustomizerPanel._instance = undefined;
            this._disposables.forEach((d) => d.dispose());
        });
    }

    /* ---- Paths ---- */
    private get _iconsDir(): string {
        return path.join(this._extensionUri.fsPath, 'icons');
    }

    private get _themeFile(): string {
        return path.join(this._iconsDir, 'nova-icon-theme.json');
    }

    /* ---- Read / write icon theme ---- */
    private _readTheme(): IconTheme {
        const raw = fs.readFileSync(this._themeFile, 'utf8');
        return JSON.parse(raw) as IconTheme;
    }

    private _writeTheme(theme: IconTheme): void {
        fs.writeFileSync(this._themeFile, JSON.stringify(theme, null, 4) + '\n', 'utf8');
    }

    /* ---- Get current custom mappings ---- */
    private _getCustomMappings(): Array<{ ext: string; iconId: string; iconPath: string }> {
        const theme = this._readTheme();
        const exts = theme.fileExtensions || {};
        const defs = theme.iconDefinitions || {};

        return Object.entries(exts)
            .filter(([, iconId]) => iconId.startsWith('custom-'))
            .map(([ext, iconId]) => ({
                ext,
                iconId,
                iconPath: defs[iconId]?.iconPath || '',
            }));
    }

    /* ---- Get built-in mappings ---- */
    private _getBuiltinMappings(): Array<{ ext: string; iconId: string }> {
        const theme = this._readTheme();
        const exts = theme.fileExtensions || {};

        return Object.entries(exts)
            .filter(([, iconId]) => !iconId.startsWith('custom-'))
            .map(([ext, iconId]) => ({ ext, iconId }));
    }

    /* ---- Add a custom icon mapping ---- */
    private async _addMapping(ext: string, svgPath: string): Promise<boolean> {
        const theme = this._readTheme();
        const sanitizedExt = ext.replace(/^\./, '').toLowerCase().replace(/[^a-z0-9_-]/g, '');
        if (!sanitizedExt) { return false; }

        /* Copy SVG to icons directory */
        const iconFileName = `custom-${sanitizedExt}.svg`;
        const destPath = path.join(this._iconsDir, iconFileName);

        /* Validate SVG before copying */
        const svgContent = fs.readFileSync(svgPath, 'utf8');
        if (!svgContent.includes('<svg')) {
            vscode.window.showErrorMessage('Selected file does not appear to be a valid SVG.');
            return false;
        }

        fs.copyFileSync(svgPath, destPath);

        /* Update theme */
        const iconId = `custom-${sanitizedExt}`;
        theme.iconDefinitions[iconId] = { iconPath: `./${iconFileName}` };
        if (!theme.fileExtensions) { theme.fileExtensions = {}; }
        theme.fileExtensions[sanitizedExt] = iconId;

        this._writeTheme(theme);
        return true;
    }

    /* ---- Remove a custom mapping ---- */
    private _removeMapping(ext: string): void {
        const theme = this._readTheme();
        const sanitizedExt = ext.replace(/^\./, '').toLowerCase();
        const iconId = `custom-${sanitizedExt}`;

        /* Remove from theme */
        delete theme.iconDefinitions[iconId];
        if (theme.fileExtensions) { delete theme.fileExtensions[sanitizedExt]; }
        if (theme.light?.fileExtensions) { delete theme.light.fileExtensions[sanitizedExt]; }

        this._writeTheme(theme);

        /* Remove SVG file */
        const svgPath = path.join(this._iconsDir, `custom-${sanitizedExt}.svg`);
        if (fs.existsSync(svgPath)) {
            fs.unlinkSync(svgPath);
        }
    }

    /* ---- Message handler ---- */
    private async _handleMessage(msg: { type: string; ext?: string }): Promise<void> {
        switch (msg.type) {
            case 'getState':
                this._panel.webview.postMessage({
                    type: 'state',
                    custom: this._getCustomMappings(),
                    builtin: this._getBuiltinMappings(),
                });
                break;

            case 'addMapping': {
                const ext = await vscode.window.showInputBox({
                    prompt: 'File extension (without dot)',
                    placeHolder: 'e.g. rs, go, zig, vue',
                    validateInput: (v) => {
                        const clean = v.replace(/^\./, '').trim();
                        if (!clean) { return 'Extension cannot be empty'; }
                        if (!/^[a-zA-Z0-9_-]+$/.test(clean)) { return 'Invalid characters'; }
                        return null;
                    },
                });
                if (!ext) { break; }

                const svgFiles = await vscode.window.showOpenDialog({
                    canSelectFiles: true,
                    canSelectFolders: false,
                    canSelectMany: false,
                    filters: { 'SVG Icons': ['svg'] },
                    title: `Select SVG icon for .${ext} files`,
                });
                if (!svgFiles || !svgFiles[0]) { break; }

                const ok = await this._addMapping(ext, svgFiles[0].fsPath);
                if (ok) {
                    vscode.window.showInformationMessage(`Added icon for .${ext} files. Reload window to see changes.`);
                }
                /* refresh */
                this._panel.webview.postMessage({
                    type: 'state',
                    custom: this._getCustomMappings(),
                    builtin: this._getBuiltinMappings(),
                });
                break;
            }

            case 'removeMapping': {
                if (!msg.ext) { break; }
                this._removeMapping(msg.ext);
                vscode.window.showInformationMessage(`Removed icon for .${msg.ext} files.`);
                this._panel.webview.postMessage({
                    type: 'state',
                    custom: this._getCustomMappings(),
                    builtin: this._getBuiltinMappings(),
                });
                break;
            }
        }
    }

    /* ---- HTML ---- */
    private _getHtml(): string {
        return `<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<meta http-equiv="Content-Security-Policy"
      content="default-src 'none'; style-src 'unsafe-inline'; script-src 'unsafe-inline';">
<title>Custom File Icons</title>
<style>
:root {
    --bg: #0a0f0a;
    --surface: #0d140d;
    --border: #1a3a1a;
    --text: #b8d4b8;
    --text-dim: #5a7a5a;
    --green: #4CAF50;
    --green-dark: #2E7D32;
    --green-light: #81C784;
    --accent: #69F0AE;
    --radius: 8px;
}
* { box-sizing: border-box; margin: 0; padding: 0; }
body {
    font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif;
    background: var(--bg); color: var(--text);
    padding: 24px; line-height: 1.5;
}
h1 { color: var(--green); font-size: 22px; margin-bottom: 4px; }
h2 { color: var(--green-light); font-size: 16px; margin: 20px 0 10px; }
.subtitle { color: var(--text-dim); font-size: 13px; margin-bottom: 20px; }

.icon-grid {
    display: grid; grid-template-columns: repeat(auto-fill, minmax(200px, 1fr));
    gap: 8px; margin-bottom: 16px;
}
.icon-card {
    display: flex; align-items: center; gap: 10px;
    background: var(--surface); border: 1px solid var(--border); border-radius: var(--radius);
    padding: 10px 14px;
}
.icon-card .ext {
    font-family: 'Consolas', monospace; font-size: 14px; font-weight: 600;
    color: var(--accent); min-width: 50px;
}
.icon-card .icon-id { font-size: 11px; color: var(--text-dim); flex: 1; }
.icon-card .remove-btn {
    background: none; border: 1px solid #a04040; color: #ef9a9a;
    border-radius: 4px; padding: 2px 8px; cursor: pointer; font-size: 11px;
}
.icon-card .remove-btn:hover { background: #3a1515; border-color: #cf6679; }
.icon-card.builtin { opacity: 0.7; }
.icon-card.builtin .ext { color: var(--green); }

.btn {
    padding: 8px 18px; border-radius: var(--radius); border: 1px solid var(--border);
    background: var(--surface); color: var(--text); cursor: pointer;
    font-size: 13px; transition: all 0.2s;
}
.btn:hover { border-color: var(--green); color: var(--green-light); }
.btn-primary { background: var(--green-dark); border-color: var(--green-dark); color: #e0f0e0; }
.btn-primary:hover { background: var(--green); }

.info-text { font-size: 12px; color: var(--text-dim); margin: 12px 0; }
.empty { color: var(--text-dim); font-style: italic; padding: 20px; text-align: center; }
</style>
</head>
<body>
<h1>Custom File Icons</h1>
<p class="subtitle">Add custom SVG icons for any file extension. Icons are stored in the extension and persist across sessions.</p>

<button class="btn btn-primary" onclick="addMapping()">+ Add Icon Mapping</button>

<h2>Custom Icons</h2>
<div class="icon-grid" id="custom-grid">
    <div class="empty">No custom icons yet. Click "Add Icon Mapping" to get started.</div>
</div>

<h2>Built-in Nova Icons</h2>
<div class="icon-grid" id="builtin-grid"></div>

<p class="info-text">
    Built-in icons cannot be removed. Custom icons are saved to the extension's icons directory
    and the icon theme is updated automatically. You may need to reload the VS Code window
    for icon changes to take effect.
</p>

<script>
const vscode = acquireVsCodeApi();

function addMapping() { vscode.postMessage({ type: 'addMapping' }); }
function removeMapping(ext) { vscode.postMessage({ type: 'removeMapping', ext: ext }); }

function esc(s) { const d = document.createElement('div'); d.textContent = s; return d.innerHTML; }

function render(state) {
    /* Custom */
    const cg = document.getElementById('custom-grid');
    if (state.custom.length === 0) {
        cg.innerHTML = '<div class="empty">No custom icons yet. Click "Add Icon Mapping" to get started.</div>';
    } else {
        cg.innerHTML = state.custom.map(m =>
            '<div class="icon-card">' +
                '<span class="ext">.' + esc(m.ext) + '</span>' +
                '<span class="icon-id">' + esc(m.iconId) + '</span>' +
                '<button class="remove-btn" onclick="removeMapping(\'' + esc(m.ext) + '\')">Remove</button>' +
            '</div>'
        ).join('');
    }

    /* Built-in */
    const bg = document.getElementById('builtin-grid');
    bg.innerHTML = state.builtin.map(m =>
        '<div class="icon-card builtin">' +
            '<span class="ext">.' + esc(m.ext) + '</span>' +
            '<span class="icon-id">' + esc(m.iconId) + '</span>' +
        '</div>'
    ).join('');
}

window.addEventListener('message', e => {
    if (e.data.type === 'state') render(e.data);
});

vscode.postMessage({ type: 'getState' });
</script>
</body>
</html>`;
    }
}
