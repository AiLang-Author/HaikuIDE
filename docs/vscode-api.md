# vscode API implemented by the HaikuIDE host

The shim reports `vscode.version` as `1.85.0`. `engines.vscode` is recorded and not enforced. An extension is supported only when every method it called in a run is listed here as implemented. Anything else is a proxy: the host logs the name once, notifies `host/unimplemented`, and returns a callable stand-in that does not throw and is not a thenable. That miss does not exit the host.

`require("vscode")` resolves to `host/dist/vscode.js`. The public object is the module (`export =`), not a `.default` property.

This list is what phase 4 measured. The window paints extension completions and extension diagnostics. Hover and definition providers run inside the host and are not requested by the window yet. `ui/message`, `output/append`, and `status/set` leave the host and the window ignores them.

## Types

Constructable, and copied onto the module: `Uri`, `Position`, `Range`, `Selection`, `Diagnostic`, `DiagnosticRelatedInformation`, `MarkdownString`, `SnippetString`, `TextEdit`, `Location`, `CompletionItem`, `CompletionList`, `Hover`, `WorkspaceEdit`, `CodeAction`, `CodeActionKind`, `SymbolInformation`, `DocumentSymbol`, `CodeLens`, `DocumentLink`, `InlayHint`, `InlayHintLabelPart`, `CallHierarchyItem`, `TypeHierarchyItem`, `CallHierarchyIncomingCall`, `CallHierarchyOutgoingCall`, `SignatureHelp`, `SignatureInformation`, `ParameterInformation`, `DocumentHighlight`, `Color`, `ColorInformation`, `ColorPresentation`, `FoldingRange`, `SelectionRange`, `InlineValueText`, `InlineValueVariableLookup`, `InlineValueEvaluatableExpression`, `SemanticTokens`, `SemanticTokensEdit`, `SemanticTokensEdits`, `LinkedEditingRanges`, `RelativePattern`, `InlineCompletionItem`, `InlineCompletionList`, `CancellationError`, `ThemeIcon`.

`CompletionItem` accepts a string or `{ label, detail, description }`. `insertText` may be a string, a `SnippetString` (the host reads `.value` and cuts at `$` before the window inserts), or `textEdit.newText`.

Enums on the module: `DiagnosticSeverity`, `CompletionItemKind`, `CompletionItemTag`, `CompletionTriggerKind`, `EndOfLine`, `TextDocumentSaveReason`, `FileType`, `ProgressLocation`, `UIKind`, `ExtensionMode`, `ViewColumn`, `TextEditorRevealType`.

`EventEmitter` takes `(listener, thisArgs, disposables)`. `Disposable.from` disposes each item. `CancellationTokenSource.token` has `isCancellationRequested` and `onCancellationRequested`.

`Position`, `Range`, and `TextDocument` offsets are UTF-16. A trailing newline counts as its own line, matching the C++ document.

## commands

Implemented: `registerCommand`, `registerTextEditorCommand`, `executeCommand`. `getCommands` resolves to an empty list. Registration notifies `command/register`.

## workspace

Implemented: `textDocuments`, `workspaceFolders`, `name`, `rootPath`, `onDidOpenTextDocument`, `onDidChangeTextDocument`, `onDidSaveTextDocument`, `onDidCloseTextDocument`, `onWillSaveTextDocument`, `openTextDocument`, `getConfiguration`, `getWorkspaceFolder`, `asRelativePath`, `applyEdit`, `fs.readFile`, `fs.writeFile`, `fs.stat`, `fs.readDirectory`, `fs.createDirectory`, `fs.delete`.

`getConfiguration(section).get(key, default)` returns the default. The configuration object is a class instance, so `Object.keys` on it is empty. `applyEdit` notifies `doc/applyEdit` with a UTF-8 byte position and length. The window applies that through the same edit path as a completion.

`notebookDocuments` is `[]`. File and notebook events exist and do not fire. `createFileSystemWatcher` returns a watcher whose events do not fire. `fs.watch` is not used.

## languages

Implemented: `match`, `registerCompletionItemProvider`, `registerHoverProvider`, `registerDefinitionProvider`, `registerDocumentFormattingEditProvider`, `registerCodeActionsProvider`, `registerRenameProvider`, `createDiagnosticCollection`, `getDiagnostics`, `setLanguageConfiguration`, `onDidChangeDiagnostics`.

`match` scores a string language at 10, `*` at 5, and an array as the max of its entries. A `{ language, scheme, pattern }` selector scores only when every field that is present matches. A notebook selector scores 0.

`DiagnosticCollection.set` notifies `diagnostics/set`. Wire severity is the vscode severity plus one, so vscode Error (0) is the gutter's error (1). The window draws those marks through the same problems list as clangd.

The shell sends LSP trigger kinds (1 invoked, 2 character). The host converts them to vscode kinds (0, 1) before calling providers. An invoke runs every matching provider. A trigger character runs only providers that registered that character.

Formatting, code actions, and rename can be registered. The window does not call them.

## window

Implemented and read by the host: `activeTextEditor`, `visibleTextEditors`, `onDidChangeActiveTextEditor`, `showTextDocument`. `tabGroups` is `{ all: [], onDidChangeTabs: undefined }` on purpose, so a `!== undefined` check does not treat it as a live tab API.

`showInformationMessage`, `showWarningMessage`, and `showErrorMessage` notify `ui/message` and resolve `undefined`. `createOutputChannel` notifies `output/append` from `append` and `appendLine`. `setStatusBarMessage` notifies `status/set`. The window does not paint those three. `createStatusBarItem` returns an object whose `show` and `hide` do nothing. `withProgress` runs the task and ignores the progress UI.

The editor object handed to extensions has `document`, a zero selection, `visibleRanges`, and `options`. `edit`, `revealRange`, `show`, and `hide` do not move the caret.

## env and ExtensionContext

`env.appName` is `HaikuIDE`. `env.language` is `en`. `env.uiKind` is desktop. `env.clipboard` reads an empty string and writes nothing. `env.openExternal` resolves `false`. `env.asExternalUri` returns the same uri.

`ExtensionContext` has `subscriptions`, `extensionPath`, `extensionUri`, `extensionMode` (Production), `globalState`, `workspaceState`, `storageUri`, `storagePath`, `asAbsolutePath`, and a `secrets` stub. Mementos are JSON files under `$HAIKUIDE_STORAGE/<id>/`, or `/boot/home/config/settings/HaikuIDE/globalStorage/<id>/` when that variable is unset.

## What the gates called

The word extension called `languages.registerCompletionItemProvider` and `CompletionItem`. The language-client gate also constructed `CodeLens`, `DocumentLink`, `CodeAction`, `Diagnostic`, `CallHierarchyItem`, `TypeHierarchyItem`, `InlayHint`, `SymbolInformation`, and `CancellationError`, and it used `workspace.getConfiguration`, `workspace.textDocuments`, `languages.createDiagnosticCollection`, `window.createOutputChannel`, and `env.appName`. Those calls completed without hitting the unimplemented proxy.
