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
import * as vscode from 'vscode';
export declare class ThemeCustomizerPanel {
    static readonly viewType = "novaThemeCustomizer";
    private static _instance;
    private readonly _panel;
    private _disposables;
    static show(extensionUri: vscode.Uri): void;
    private constructor();
    private _getCurrentOverrides;
    private _getCustomProfiles;
    private _getAllProfiles;
    private _getCategoryColors;
    private _applyTokenColors;
    private _clearOverrides;
    private _handleMessage;
    private _getHtml;
}
//# sourceMappingURL=themeCustomizer.d.ts.map