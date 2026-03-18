"use strict";
/**
 * @file    themeCustomizer.ts
 * @brief   Theme Customizer webview — universal syntax color editor & profiles
 * @author  Anthony Taliento (Zorya Corporation)
 *
 * Provides:
 *  - Color pickers for all major token categories (works on ANY language)
 *  - Pre-built syntax profiles (Phosphor, Cyberpunk, Monokai, Dracula, etc.)
 *  - Live preview — changes write to editor.tokenColorCustomizations
 *  - UI theme color overrides via workbench.colorCustomizations
 *  - Does NOT modify theme files — uses VS Code's override system
 */
var __createBinding = (this && this.__createBinding) || (Object.create ? (function(o, m, k, k2) {
    if (k2 === undefined) k2 = k;
    var desc = Object.getOwnPropertyDescriptor(m, k);
    if (!desc || ("get" in desc ? !m.__esModule : desc.writable || desc.configurable)) {
      desc = { enumerable: true, get: function() { return m[k]; } };
    }
    Object.defineProperty(o, k2, desc);
}) : (function(o, m, k, k2) {
    if (k2 === undefined) k2 = k;
    o[k2] = m[k];
}));
var __setModuleDefault = (this && this.__setModuleDefault) || (Object.create ? (function(o, v) {
    Object.defineProperty(o, "default", { enumerable: true, value: v });
}) : function(o, v) {
    o["default"] = v;
});
var __importStar = (this && this.__importStar) || (function () {
    var ownKeys = function(o) {
        ownKeys = Object.getOwnPropertyNames || function (o) {
            var ar = [];
            for (var k in o) if (Object.prototype.hasOwnProperty.call(o, k)) ar[ar.length] = k;
            return ar;
        };
        return ownKeys(o);
    };
    return function (mod) {
        if (mod && mod.__esModule) return mod;
        var result = {};
        if (mod != null) for (var k = ownKeys(mod), i = 0; i < k.length; i++) if (k[i] !== "default") __createBinding(result, mod, k[i]);
        __setModuleDefault(result, mod);
        return result;
    };
})();
Object.defineProperty(exports, "__esModule", { value: true });
exports.ThemeCustomizerPanel = void 0;
const vscode = __importStar(require("vscode"));
const TOKEN_CATEGORIES = [
    { label: 'Comments', description: 'Code comments', scopes: ['comment', 'punctuation.definition.comment'] },
    { label: 'Strings', description: 'String literals', scopes: ['string', 'string.quoted'] },
    { label: 'Numbers', description: 'Numeric literals', scopes: ['constant.numeric'] },
    { label: 'Constants', description: 'Language constants (true, false, nil)', scopes: ['constant.language'] },
    { label: 'Keywords', description: 'Control flow & declarations', scopes: ['keyword.control', 'keyword.declaration', 'keyword'] },
    { label: 'Operators', description: 'Arithmetic, logical, assignment', scopes: ['keyword.operator'] },
    { label: 'Functions', description: 'Function definitions', scopes: ['entity.name.function'] },
    { label: 'Function Calls', description: 'Function invocations', scopes: ['entity.name.function.call', 'meta.function-call'] },
    { label: 'Methods', description: 'Method calls', scopes: ['entity.name.function.method', 'entity.name.function.method-call'] },
    { label: 'Variables', description: 'Variable names', scopes: ['variable.other', 'variable'] },
    { label: 'Parameters', description: 'Function parameters', scopes: ['variable.parameter'] },
    { label: 'Types', description: 'Type annotations & class names', scopes: ['entity.name.type', 'support.type', 'storage.type'] },
    { label: 'Built-ins', description: 'Built-in / support functions', scopes: ['support.function', 'support.function.builtin'] },
    { label: 'Punctuation', description: 'Brackets, commas, semicolons', scopes: ['punctuation'] },
    { label: 'Preprocessor', description: 'Preprocessor / directives', scopes: ['keyword.control.directive', 'meta.preprocessor'] },
    { label: 'Escape Chars', description: 'Escape sequences in strings', scopes: ['constant.character.escape'] },
    { label: 'Tags (HTML)', description: 'HTML/XML tag names', scopes: ['entity.name.tag'] },
    { label: 'Attributes', description: 'HTML/XML attributes', scopes: ['entity.other.attribute-name'] },
    { label: 'CSS Properties', description: 'CSS property names', scopes: ['support.type.property-name.css'] },
    { label: 'Regex', description: 'Regular expressions', scopes: ['string.regexp'] },
];
const SYNTAX_PROFILES = [
    {
        id: 'phosphor',
        name: 'Nova Phosphor',
        description: 'Green monochrome — the default Nova look',
        colors: [
            '#3a5a3a', '#81C784', '#C8E6C9', '#66BB6A', '#4CAF50',
            '#A5D6A7', '#E8F5E9', '#C8E6C9', '#A5D6A7', '#b8d4b8',
            '#90c090', '#66BB6A', '#69F0AE', '#6a9a6a', '#388E3C',
            '#A5D6A7', '#4CAF50', '#81C784', '#A5D6A7', '#81C784',
        ],
        fontStyles: [
            'italic', undefined, undefined, 'italic', 'bold',
            undefined, undefined, undefined, undefined, undefined,
            'italic', undefined, 'bold', undefined, undefined,
            undefined, undefined, undefined, undefined, undefined,
        ],
    },
    {
        id: 'cyberpunk',
        name: 'Cyberpunk Neon',
        description: 'Vivid neon rainbow on true black',
        colors: [
            '#5c6370', '#98c379', '#d19a66', '#56b6c2', '#c678dd',
            '#ff79c6', '#61afef', '#e5c07b', '#56b6c2', '#abb2bf',
            '#e06c75', '#61afef', '#e5c07b', '#636d83', '#c678dd',
            '#98c379', '#e06c75', '#d19a66', '#56b6c2', '#98c379',
        ],
        fontStyles: [
            'italic', undefined, undefined, undefined, 'bold',
            undefined, 'bold', undefined, 'italic', undefined,
            'italic', undefined, undefined, undefined, 'bold',
            undefined, undefined, undefined, undefined, undefined,
        ],
    },
    {
        id: 'monokai',
        name: 'Monokai',
        description: 'Classic warm syntax colors',
        colors: [
            '#75715e', '#e6db74', '#ae81ff', '#ae81ff', '#f92672',
            '#f92672', '#a6e22e', '#66d9ef', '#a6e22e', '#f8f8f2',
            '#fd971f', '#66d9ef', '#66d9ef', '#75715e', '#f92672',
            '#ae81ff', '#f92672', '#a6e22e', '#66d9ef', '#e6db74',
        ],
        fontStyles: [
            'italic', undefined, undefined, undefined, undefined,
            undefined, undefined, 'italic', undefined, undefined,
            'italic', 'italic', undefined, undefined, undefined,
            undefined, undefined, undefined, undefined, undefined,
        ],
    },
    {
        id: 'dracula',
        name: 'Dracula',
        description: 'Purple-pink-cyan palette',
        colors: [
            '#6272a4', '#f1fa8c', '#bd93f9', '#bd93f9', '#ff79c6',
            '#ff79c6', '#50fa7b', '#50fa7b', '#50fa7b', '#f8f8f2',
            '#ffb86c', '#8be9fd', '#8be9fd', '#6272a4', '#ff79c6',
            '#f1fa8c', '#ff79c6', '#50fa7b', '#8be9fd', '#f1fa8c',
        ],
        fontStyles: [
            'italic', undefined, undefined, 'italic', undefined,
            undefined, undefined, undefined, 'italic', undefined,
            'italic', 'italic', undefined, undefined, undefined,
            undefined, undefined, undefined, undefined, undefined,
        ],
    },
    {
        id: 'nord',
        name: 'Nord',
        description: 'Arctic blue palette — cool and calm',
        colors: [
            '#616e88', '#a3be8c', '#b48ead', '#81a1c1', '#81a1c1',
            '#81a1c1', '#88c0d0', '#88c0d0', '#88c0d0', '#d8dee9',
            '#d8dee9', '#8fbcbb', '#88c0d0', '#4c566a', '#5e81ac',
            '#ebcb8b', '#81a1c1', '#8fbcbb', '#88c0d0', '#ebcb8b',
        ],
    },
    {
        id: 'solarized',
        name: 'Solarized Dark',
        description: 'Ethan Schoonover\'s warm-cool scheme',
        colors: [
            '#586e75', '#2aa198', '#d33682', '#cb4b16', '#859900',
            '#657b83', '#268bd2', '#268bd2', '#2aa198', '#839496',
            '#b58900', '#b58900', '#268bd2', '#586e75', '#cb4b16',
            '#dc322f', '#268bd2', '#93a1a1', '#2aa198', '#2aa198',
        ],
    },
    {
        id: 'gruvbox',
        name: 'Gruvbox Dark',
        description: 'Retro brown-orange palette',
        colors: [
            '#928374', '#b8bb26', '#d3869b', '#d3869b', '#fb4934',
            '#fe8019', '#fabd2f', '#fabd2f', '#8ec07c', '#ebdbb2',
            '#83a598', '#83a598', '#fabd2f', '#928374', '#fb4934',
            '#b8bb26', '#fb4934', '#8ec07c', '#fabd2f', '#b8bb26',
        ],
    },
    {
        id: 'tokyonight',
        name: 'Tokyo Night',
        description: 'Blue-purple city lights',
        colors: [
            '#565f89', '#9ece6a', '#ff9e64', '#ff9e64', '#bb9af7',
            '#89ddff', '#7aa2f7', '#7aa2f7', '#7aa2f7', '#c0caf5',
            '#e0af68', '#2ac3de', '#7aa2f7', '#3b4261', '#bb9af7',
            '#9ece6a', '#f7768e', '#73daca', '#2ac3de', '#9ece6a',
        ],
    },
    {
        id: 'catppuccin',
        name: 'Catppuccin Mocha',
        description: 'Warm pastel comfort',
        colors: [
            '#6c7086', '#a6e3a1', '#fab387', '#fab387', '#cba6f7',
            '#89dceb', '#89b4fa', '#89b4fa', '#89b4fa', '#cdd6f4',
            '#f38ba8', '#f9e2af', '#89b4fa', '#585b70', '#cba6f7',
            '#a6e3a1', '#f38ba8', '#a6e3a1', '#89dceb', '#a6e3a1',
        ],
    },
    {
        id: 'onedark',
        name: 'One Dark Pro',
        description: 'Atom-inspired muted pastels',
        colors: [
            '#5c6370', '#98c379', '#d19a66', '#d19a66', '#c678dd',
            '#56b6c2', '#61afef', '#61afef', '#61afef', '#abb2bf',
            '#e06c75', '#e5c07b', '#61afef', '#5c6370', '#c678dd',
            '#98c379', '#e06c75', '#98c379', '#56b6c2', '#56b6c2',
        ],
        fontStyles: [
            'italic', undefined, undefined, undefined, undefined,
            undefined, undefined, undefined, undefined, undefined,
            'italic', undefined, undefined, undefined, undefined,
            undefined, undefined, undefined, undefined, undefined,
        ],
    },
];
/* ------------------------------------------------------------------ */
/*  Webview Panel                                                      */
/* ------------------------------------------------------------------ */
class ThemeCustomizerPanel {
    static viewType = 'novaThemeCustomizer';
    static _instance;
    _panel;
    _disposables = [];
    static show(extensionUri) {
        if (ThemeCustomizerPanel._instance) {
            ThemeCustomizerPanel._instance._panel.reveal();
            return;
        }
        const panel = vscode.window.createWebviewPanel(ThemeCustomizerPanel.viewType, 'Theme Customizer', vscode.ViewColumn.One, { enableScripts: true, retainContextWhenHidden: true });
        ThemeCustomizerPanel._instance = new ThemeCustomizerPanel(panel);
    }
    constructor(panel) {
        this._panel = panel;
        this._panel.webview.html = this._getHtml();
        this._panel.webview.onDidReceiveMessage((msg) => this._handleMessage(msg), undefined, this._disposables);
        this._panel.onDidDispose(() => {
            ThemeCustomizerPanel._instance = undefined;
            this._disposables.forEach((d) => d.dispose());
        });
    }
    /* ---- Read current token overrides from VS Code settings ---- */
    _getCurrentOverrides() {
        const cfg = vscode.workspace.getConfiguration('editor');
        const custom = cfg.get('tokenColorCustomizations', {});
        const rules = custom
            .textMateRules || [];
        const map = {};
        for (const rule of rules) {
            const scopes = Array.isArray(rule.scope) ? rule.scope : [rule.scope];
            for (const s of scopes) {
                map[s] = rule.settings;
            }
        }
        return map;
    }
    /* ---- Load saved custom profiles from settings ---- */
    _getCustomProfiles() {
        const cfg = vscode.workspace.getConfiguration('nova');
        const saved = cfg.get('customSyntaxProfiles', []);
        return saved.map(p => ({
            id: p.id,
            name: p.name,
            description: p.description,
            colors: p.colors,
            fontStyles: p.fontStyles,
        }));
    }
    /* ---- All profiles (built-in + custom) ---- */
    _getAllProfiles() {
        return [...SYNTAX_PROFILES, ...this._getCustomProfiles()];
    }
    /* ---- Build per-category color from current overrides ---- */
    _getCategoryColors() {
        const overrides = this._getCurrentOverrides();
        return TOKEN_CATEGORIES.map((cat) => {
            for (const scope of cat.scopes) {
                if (overrides[scope]) {
                    return {
                        foreground: overrides[scope].foreground || '',
                        fontStyle: overrides[scope].fontStyle || '',
                    };
                }
            }
            return { foreground: '', fontStyle: '' };
        });
    }
    /* ---- Apply a profile or individual change ---- */
    async _applyTokenColors(colors) {
        const cfg = vscode.workspace.getConfiguration('editor');
        const current = cfg.get('tokenColorCustomizations', {});
        const rules = [];
        for (const entry of colors) {
            const settings = {};
            if (entry.foreground) {
                settings.foreground = entry.foreground;
            }
            if (entry.fontStyle) {
                settings.fontStyle = entry.fontStyle;
            }
            if (Object.keys(settings).length > 0) {
                rules.push({ scope: entry.scopes, settings });
            }
        }
        const updated = { ...current, textMateRules: rules };
        await cfg.update('tokenColorCustomizations', updated, vscode.ConfigurationTarget.Global);
    }
    async _clearOverrides() {
        const cfg = vscode.workspace.getConfiguration('editor');
        await cfg.update('tokenColorCustomizations', undefined, vscode.ConfigurationTarget.Global);
    }
    /* ---- Messages from webview ---- */
    async _handleMessage(msg) {
        switch (msg.type) {
            case 'getState': {
                const colors = this._getCategoryColors();
                const allProfiles = this._getAllProfiles();
                const customIds = new Set(this._getCustomProfiles().map(p => p.id));
                this._panel.webview.postMessage({
                    type: 'state',
                    categories: TOKEN_CATEGORIES.map((c, i) => ({
                        label: c.label,
                        description: c.description,
                        foreground: colors[i].foreground,
                        fontStyle: colors[i].fontStyle,
                    })),
                    profiles: allProfiles.map((p) => ({
                        id: p.id,
                        name: p.name,
                        description: p.description,
                        colors: p.colors,
                        isCustom: customIds.has(p.id),
                    })),
                });
                break;
            }
            case 'applyProfile': {
                const allProfiles = this._getAllProfiles();
                const profile = allProfiles.find((p) => p.id === msg.profileId);
                if (!profile) {
                    break;
                }
                const entries = TOKEN_CATEGORIES.map((cat, i) => ({
                    scopes: cat.scopes,
                    foreground: profile.colors[i] || '',
                    fontStyle: profile.fontStyles?.[i],
                }));
                await this._applyTokenColors(entries);
                /* refresh UI */
                const colors = this._getCategoryColors();
                this._panel.webview.postMessage({
                    type: 'colorsUpdated',
                    categories: colors,
                });
                vscode.window.showInformationMessage(`Applied "${profile.name}" syntax profile`);
                break;
            }
            case 'saveProfile': {
                const name = msg.profileName?.trim();
                if (!name) {
                    break;
                }
                const currentColors = this._getCategoryColors();
                const id = 'custom-' + name.toLowerCase().replace(/[^a-z0-9]+/g, '-').replace(/^-|-$/g, '');
                const newProfile = {
                    id,
                    name,
                    description: 'Custom profile',
                    colors: currentColors.map(c => c.foreground || '#808080'),
                    fontStyles: currentColors.map(c => c.fontStyle || undefined),
                };
                const cfg = vscode.workspace.getConfiguration('nova');
                const existing = cfg.get('customSyntaxProfiles', []);
                const idx = existing.findIndex(p => p.id === id);
                if (idx >= 0) {
                    existing[idx] = newProfile;
                }
                else {
                    existing.push(newProfile);
                }
                await cfg.update('customSyntaxProfiles', existing, vscode.ConfigurationTarget.Global);
                /* Refresh state */
                this._panel.webview.postMessage({ type: 'getState' });
                await this._handleMessage({ type: 'getState' });
                vscode.window.showInformationMessage(`Saved syntax profile "${name}"`);
                break;
            }
            case 'deleteProfile': {
                if (!msg.profileId) {
                    break;
                }
                const cfg = vscode.workspace.getConfiguration('nova');
                const profiles = cfg.get('customSyntaxProfiles', []);
                const filtered = profiles.filter(p => p.id !== msg.profileId);
                await cfg.update('customSyntaxProfiles', filtered, vscode.ConfigurationTarget.Global);
                await this._handleMessage({ type: 'getState' });
                vscode.window.showInformationMessage('Deleted custom profile');
                break;
            }
            case 'updateColor': {
                if (msg.index === undefined) {
                    break;
                }
                const cat = TOKEN_CATEGORIES[msg.index];
                if (!cat) {
                    break;
                }
                /* Re-read all current overrides and update just this one */
                const currentColors = this._getCategoryColors();
                const entries = TOKEN_CATEGORIES.map((c, i) => ({
                    scopes: c.scopes,
                    foreground: i === msg.index ? (msg.foreground || '') : currentColors[i].foreground,
                    fontStyle: i === msg.index ? (msg.fontStyle || '') : currentColors[i].fontStyle,
                }));
                await this._applyTokenColors(entries);
                break;
            }
            case 'clearAll':
                await this._clearOverrides();
                this._panel.webview.postMessage({
                    type: 'colorsUpdated',
                    categories: TOKEN_CATEGORIES.map(() => ({ foreground: '', fontStyle: '' })),
                });
                vscode.window.showInformationMessage('Cleared all token color overrides');
                break;
        }
    }
    /* ---- HTML ---- */
    _getHtml() {
        const profilesJson = JSON.stringify(SYNTAX_PROFILES.map((p) => ({ id: p.id, name: p.name, description: p.description, colors: p.colors }))).replace(/</g, '\\u003c');
        const categoriesJson = JSON.stringify(TOKEN_CATEGORIES.map((c) => ({ label: c.label, description: c.description }))).replace(/</g, '\\u003c');
        return `<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<meta http-equiv="Content-Security-Policy"
      content="default-src 'none'; style-src 'unsafe-inline'; script-src 'unsafe-inline';">
<title>Theme Customizer</title>
<style>
:root {
    --bg: #0c0c0c;
    --surface: #141414;
    --surface2: #1a1a1a;
    --border: #252525;
    --text: #a8a8a8;
    --text-dim: #606060;
    --green: #4A8C5C;
    --green-dark: #2D5E3A;
    --green-light: #6DB880;
    --accent: #69F0AE;
    --warn: #ff9800;
    --radius: 8px;
}
* { box-sizing: border-box; margin: 0; padding: 0; }
body {
    font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif;
    background: var(--bg); color: var(--text);
    padding: 24px; line-height: 1.5;
}
h1 { color: var(--green); font-size: 22px; margin-bottom: 4px; }
.subtitle { color: var(--text-dim); font-size: 13px; margin-bottom: 20px; }

/* ---- Tabs ---- */
.tabs { display: flex; gap: 0; border-bottom: 1px solid var(--border); margin-bottom: 20px; }
.tab {
    padding: 8px 18px; cursor: pointer; color: var(--text-dim);
    border-bottom: 2px solid transparent; font-size: 13px; font-weight: 500;
    transition: all 0.2s;
}
.tab:hover { color: var(--text); }
.tab.active { color: var(--green); border-bottom-color: var(--green); }

/* ---- Sections ---- */
.section { display: none; }
.section.active { display: block; }

/* ---- Profile grid ---- */
.profile-grid { display: grid; grid-template-columns: repeat(auto-fill, minmax(260px, 1fr)); gap: 12px; margin-bottom: 20px; }
.profile-card {
    background: var(--surface); border: 1px solid var(--border); border-radius: var(--radius);
    padding: 14px; cursor: pointer; transition: all 0.2s;
}
.profile-card:hover { border-color: var(--green); transform: translateY(-1px); }
.profile-card.active { border-color: var(--accent); box-shadow: 0 0 8px rgba(105,240,174,0.15); }
.profile-delete {
    cursor: pointer; font-size: 18px; color: var(--text-dim);
    width: 24px; height: 24px; display: flex; align-items: center;
    justify-content: center; border-radius: 4px;
}
.profile-delete:hover { color: #ef9a9a; background: #3a1515; }
.profile-name { font-weight: 600; font-size: 14px; color: var(--green-light); margin-bottom: 2px; }
.profile-desc { font-size: 12px; color: var(--text-dim); margin-bottom: 8px; }
.profile-swatches { display: flex; gap: 3px; flex-wrap: wrap; }
.swatch {
    width: 16px; height: 16px; border-radius: 3px; border: 1px solid rgba(255,255,255,0.1);
}

/* ---- Color editor ---- */
.color-grid { display: grid; grid-template-columns: 1fr 1fr; gap: 10px; }
@media (max-width: 600px) { .color-grid { grid-template-columns: 1fr; } }
.color-row {
    display: flex; align-items: center; gap: 10px;
    background: var(--surface); border: 1px solid var(--border); border-radius: var(--radius);
    padding: 10px 14px;
}
.color-row:hover { border-color: var(--green-dark); }
.color-label { flex: 1; min-width: 0; }
.color-label-name { font-size: 13px; font-weight: 500; }
.color-label-desc { font-size: 11px; color: var(--text-dim); }
.color-picker-wrap { position: relative; width: 34px; height: 34px; }
.color-picker-wrap input[type="color"] {
    position: absolute; top: 0; left: 0; width: 100%; height: 100%;
    border: 2px solid var(--border); border-radius: 6px; cursor: pointer;
    background: none; padding: 0;
}
.color-picker-wrap input[type="color"]::-webkit-color-swatch-wrapper { padding: 0; }
.color-picker-wrap input[type="color"]::-webkit-color-swatch { border: none; border-radius: 4px; }
.color-hex {
    width: 72px; background: var(--bg); border: 1px solid var(--border);
    color: var(--text); border-radius: 4px; padding: 4px 6px; font-size: 12px;
    font-family: 'Consolas', monospace; text-align: center;
}
.color-hex:focus { border-color: var(--green); outline: none; }
select.font-style-sel {
    background: var(--bg); border: 1px solid var(--border); color: var(--text);
    border-radius: 4px; padding: 4px; font-size: 11px; cursor: pointer;
}

.preview-box {
    background: #1e1e1e; border: 1px solid var(--border); border-radius: var(--radius);
    padding: 16px 20px; font-family: 'Consolas', 'Courier New', monospace;
    font-size: 13px; line-height: 1.6; margin-bottom: 20px; overflow-x: auto;
}
.preview-box .ln { color: #5a5a5a; user-select: none; margin-right: 16px; }

/* ---- Buttons ---- */
.btn-row { display: flex; gap: 10px; margin: 16px 0; flex-wrap: wrap; }
.btn {
    padding: 8px 18px; border-radius: var(--radius); border: 1px solid var(--border);
    background: var(--surface); color: var(--text); cursor: pointer;
    font-size: 13px; transition: all 0.2s;
}
.btn:hover { border-color: var(--green); color: var(--green-light); }
.btn-primary { background: var(--green-dark); border-color: var(--green-dark); color: #e0f0e0; }
.btn-primary:hover { background: var(--green); }
.btn-danger { border-color: #a04040; color: #ef9a9a; }
.btn-danger:hover { background: #3a1515; border-color: #cf6679; }

.info-text { font-size: 12px; color: var(--text-dim); margin: 12px 0; }
</style>
</head>
<body>
<h1>Theme Customizer</h1>
<p class="subtitle">Customize syntax colors for any language. Changes apply globally via VS Code settings.</p>

<div class="tabs">
    <div class="tab active" data-tab="profiles">Profiles</div>
    <div class="tab" data-tab="editor">Color Editor</div>
    <div class="tab" data-tab="preview">Live Preview</div>
</div>

<!-- ============ PROFILES ============ -->
<div class="section active" id="section-profiles">
    <p class="info-text">Click a profile to instantly apply its syntax colors. Works with any base theme.</p>
    <div class="profile-grid" id="profile-grid"></div>
    <div class="btn-row">
        <button class="btn btn-primary" onclick="saveProfile()">Save Current as Profile</button>
        <button class="btn btn-danger" onclick="clearAll()">Reset to Theme Defaults</button>
    </div>
</div>

<!-- ============ COLOR EDITOR ============ -->
<div class="section" id="section-editor">
    <p class="info-text">Fine-tune individual token colors. Leave a field empty to use the active theme's default.</p>
    <div class="color-grid" id="color-grid"></div>
    <div class="btn-row">
        <button class="btn btn-danger" onclick="clearAll()">Reset All</button>
    </div>
</div>

<!-- ============ LIVE PREVIEW ============ -->
<div class="section" id="section-preview">
    <p class="info-text">Preview how your current colors look on sample code.</p>
    <div class="preview-box" id="preview-box"></div>
    <p class="info-text" style="margin-top: 8px;">
        Colors are applied globally to <code>editor.tokenColorCustomizations</code> in your VS Code settings.
        They override (but don't replace) your active color theme.
    </p>
</div>

<script>
const vscode = acquireVsCodeApi();
const profiles = ${profilesJson};
const categoryDefs = ${categoriesJson};
let categories = [];

/* ---- Tab switching ---- */
document.querySelectorAll('.tab').forEach(tab => {
    tab.addEventListener('click', () => {
        document.querySelectorAll('.tab').forEach(t => t.classList.remove('active'));
        document.querySelectorAll('.section').forEach(s => s.classList.remove('active'));
        tab.classList.add('active');
        const s = document.getElementById('section-' + tab.dataset.tab);
        if (s) s.classList.add('active');
        if (tab.dataset.tab === 'preview') updatePreview();
    });
});

/* ---- Build profile cards ---- */
function buildProfiles() {
    const grid = document.getElementById('profile-grid');
    grid.innerHTML = '';
    profiles.forEach(p => {
        const card = document.createElement('div');
        card.className = 'profile-card';
        card.dataset.id = p.id;
        const badge = p.isCustom ? ' <span style="font-size:10px;color:#5a9a5a;border:1px solid #1b4d2e;border-radius:4px;padding:1px 5px;margin-left:6px;">custom</span>' : '';
        const deleteBtn = p.isCustom ? '<span class="profile-delete" data-id="' + esc(p.id) + '" title="Delete profile">&times;</span>' : '';
        card.innerHTML =
            '<div style="display:flex;justify-content:space-between;align-items:center">' +
            '<div class="profile-name">' + esc(p.name) + badge + '</div>' +
            deleteBtn +
            '</div>' +
            '<div class="profile-desc">' + esc(p.description) + '</div>' +
            '<div class="profile-swatches">' +
            p.colors.slice(0, 12).map(c => '<div class="swatch" style="background:' + esc(c) + '"></div>').join('') +
            '</div>';
        card.addEventListener('click', (e) => {
            if (e.target.classList && e.target.classList.contains('profile-delete')) return;
            vscode.postMessage({ type: 'applyProfile', profileId: p.id });
        });
        grid.appendChild(card);
    });
    /* delete buttons */
    grid.querySelectorAll('.profile-delete').forEach(btn => {
        btn.addEventListener('click', (e) => {
            e.stopPropagation();
            vscode.postMessage({ type: 'deleteProfile', profileId: btn.dataset.id });
        });
    });
}

/* ---- Build color editor ---- */
function buildColorEditor() {
    const grid = document.getElementById('color-grid');
    grid.innerHTML = '';
    categoryDefs.forEach((cat, i) => {
        const fg = (categories[i] && categories[i].foreground) || '#808080';
        const fs = (categories[i] && categories[i].fontStyle) || '';
        const row = document.createElement('div');
        row.className = 'color-row';
        row.innerHTML =
            '<div class="color-label">' +
                '<div class="color-label-name">' + esc(cat.label) + '</div>' +
                '<div class="color-label-desc">' + esc(cat.description) + '</div>' +
            '</div>' +
            '<div class="color-picker-wrap">' +
                '<input type="color" value="' + (fg || '#808080') + '" data-idx="' + i + '">' +
            '</div>' +
            '<input type="text" class="color-hex" value="' + esc(fg) + '" data-idx="' + i + '" placeholder="#hex">' +
            '<select class="font-style-sel" data-idx="' + i + '">' +
                '<option value=""' + (fs === '' ? ' selected' : '') + '>Normal</option>' +
                '<option value="bold"' + (fs === 'bold' ? ' selected' : '') + '>Bold</option>' +
                '<option value="italic"' + (fs === 'italic' ? ' selected' : '') + '>Italic</option>' +
                '<option value="bold italic"' + (fs === 'bold italic' ? ' selected' : '') + '>Bold Italic</option>' +
                '<option value="underline"' + (fs === 'underline' ? ' selected' : '') + '>Underline</option>' +
            '</select>';
        grid.appendChild(row);
    });

    /* event: color picker */
    grid.querySelectorAll('input[type="color"]').forEach(el => {
        el.addEventListener('input', e => {
            const idx = parseInt(e.target.dataset.idx);
            const hex = e.target.value;
            const hexInput = grid.querySelector('input.color-hex[data-idx="' + idx + '"]');
            if (hexInput) hexInput.value = hex;
            sendColorUpdate(idx);
        });
    });

    /* event: hex text */
    let hexTimer = null;
    grid.querySelectorAll('input.color-hex').forEach(el => {
        el.addEventListener('input', e => {
            const idx = parseInt(e.target.dataset.idx);
            const val = e.target.value;
            if (/^#[0-9a-fA-F]{6}$/.test(val)) {
                const picker = grid.querySelector('input[type="color"][data-idx="' + idx + '"]');
                if (picker) picker.value = val;
            }
            clearTimeout(hexTimer);
            hexTimer = setTimeout(() => sendColorUpdate(idx), 400);
        });
    });

    /* event: font style */
    grid.querySelectorAll('select.font-style-sel').forEach(el => {
        el.addEventListener('change', e => {
            const idx = parseInt(e.target.dataset.idx);
            sendColorUpdate(idx);
        });
    });
}

function sendColorUpdate(idx) {
    const grid = document.getElementById('color-grid');
    const hexInput = grid.querySelector('input.color-hex[data-idx="' + idx + '"]');
    const styleSel = grid.querySelector('select.font-style-sel[data-idx="' + idx + '"]');
    vscode.postMessage({
        type: 'updateColor',
        index: idx,
        foreground: hexInput ? hexInput.value : '',
        fontStyle: styleSel ? styleSel.value : '',
    });
}

/* ---- Preview ---- */
function updatePreview() {
    const box = document.getElementById('preview-box');
    const c = (idx) => (categories[idx] && categories[idx].foreground) || '#808080';
    const s = (idx) => {
        const fs = categories[idx] && categories[idx].fontStyle;
        let style = 'color:' + c(idx);
        if (fs && fs.includes('italic')) style += ';font-style:italic';
        if (fs && fs.includes('bold')) style += ';font-weight:bold';
        if (fs && fs.includes('underline')) style += ';text-decoration:underline';
        return style;
    };

    box.innerHTML =
        '<div><span class="ln">1</span><span style="' + s(0) + '">-- A sample function</span></div>' +
        '<div><span class="ln">2</span><span style="' + s(4) + '">local</span> <span style="' + s(5) + '">function</span> <span style="' + s(6) + '">greet</span><span style="' + s(13) + '">(</span><span style="' + s(10) + '">name</span><span style="' + s(13) + '">)</span></div>' +
        '<div><span class="ln">3</span>    <span style="' + s(12) + '">echo</span><span style="' + s(13) + '">(</span><span style="' + s(1) + '">"Hello, "</span> <span style="' + s(5) + '">..</span> <span style="' + s(9) + '">name</span><span style="' + s(13) + '">)</span></div>' +
        '<div><span class="ln">4</span>    <span style="' + s(4) + '">return</span> <span style="' + s(2) + '">42</span></div>' +
        '<div><span class="ln">5</span><span style="' + s(4) + '">end</span></div>' +
        '<div><span class="ln">6</span></div>' +
        '<div><span class="ln">7</span><span style="' + s(4) + '">local</span> <span style="' + s(9) + '">result</span> <span style="' + s(5) + '">=</span> <span style="' + s(7) + '">greet</span><span style="' + s(13) + '">(</span><span style="' + s(1) + '">"World"</span><span style="' + s(13) + '">)</span></div>' +
        '<div><span class="ln">8</span><span style="' + s(4) + '">if</span> <span style="' + s(9) + '">result</span> <span style="' + s(5) + '">==</span> <span style="' + s(3) + '">nil</span> <span style="' + s(4) + '">then</span></div>' +
        '<div><span class="ln">9</span>    <span style="' + s(12) + '">echo</span><span style="' + s(13) + '">(</span><span style="' + s(1) + '">"nothing"</span><span style="' + s(13) + '">)</span></div>' +
        '<div><span class="ln">10</span><span style="' + s(4) + '">end</span></div>';
}

function clearAll() { vscode.postMessage({ type: 'clearAll' }); }

function saveProfile() {
    const name = prompt('Enter a name for this syntax profile:');
    if (name && name.trim()) {
        vscode.postMessage({ type: 'saveProfile', profileName: name.trim() });
    }
}

function esc(s) { const d = document.createElement('div'); d.textContent = s; return d.innerHTML; }

/* ---- Message handling ---- */
window.addEventListener('message', e => {
    const msg = e.data;
    if (msg.type === 'state') {
        categories = msg.categories;
        if (msg.profiles) profiles.length = 0, msg.profiles.forEach(p => profiles.push(p));
        buildProfiles();
        buildColorEditor();
        updatePreview();
    } else if (msg.type === 'colorsUpdated') {
        categories = msg.categories;
        buildColorEditor();
        updatePreview();
    }
});

/* Initial load */
vscode.postMessage({ type: 'getState' });
</script>
</body>
</html>`;
    }
}
exports.ThemeCustomizerPanel = ThemeCustomizerPanel;
//# sourceMappingURL=themeCustomizer.js.map