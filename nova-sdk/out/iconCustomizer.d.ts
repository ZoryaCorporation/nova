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
export declare class IconCustomizerPanel {
    static readonly viewType = "novaIconCustomizer";
    private static _instance;
    private readonly _panel;
    private readonly _extensionUri;
    private _disposables;
    static show(extensionUri: vscode.Uri): void;
    private constructor();
    private get _iconsDir();
    private get _themeFile();
    private _readTheme;
    private _writeTheme;
    private _getCustomMappings;
    private _getBuiltinMappings;
    private _addMapping;
    private _removeMapping;
    private _handleMessage;
    private _getHtml;
}
//# sourceMappingURL=iconCustomizer.d.ts.map