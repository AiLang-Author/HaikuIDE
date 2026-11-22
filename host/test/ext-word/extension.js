/* Thirty-line completion extension for the phase 4 gate.
 *
 * Copyright (c) 2026 Sean Collins, 2 Paws Machine and Engineering. MIT.
 */
const vscode = require("vscode");

function activate(context) {
    const provider = {
        provideCompletionItems() {
            const item = new vscode.CompletionItem("haikuide_ext");
            item.insertText = "haikuide_ext";
            item.detail = "extension";
            return [item];
        }
    };
    context.subscriptions.push(
        vscode.languages.registerCompletionItemProvider(
            [{ language: "cpp" }, { language: "plaintext" }],
            provider,
            "."
        )
    );
}

function deactivate() {}

module.exports = { activate, deactivate };
