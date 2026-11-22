/* vscode-languageclient against the phase 3 Python server.
 *
 * Copyright (c) 2026 Sean Collins, 2 Paws Machine and Engineering. MIT.
 */
const vscode = require("vscode");
const { LanguageClient, TransportKind } = require("vscode-languageclient/node");

async function activate(context) {
    const server = process.env.HAIKUIDE_TEST_SERVER;
    const client = new LanguageClient(
        "haikuide-test",
        "HaikuIDE Test",
        {
            command: "python3",
            args: [server],
            transport: TransportKind.stdio
        },
        {
            documentSelector: [{ language: "plaintext" }]
        }
    );
    context.subscriptions.push({ dispose: () => client.stop() });
    await client.start();
}

function deactivate() {}

module.exports = { activate, deactivate };
