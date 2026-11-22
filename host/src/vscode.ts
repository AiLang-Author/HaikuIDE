/* Public vscode module. Extensions require("vscode") and get this.
 *
 * Copyright (c) 2026 Sean Collins, 2 Paws Machine and Engineering. MIT.
 */

import * as fs from "fs";
import * as path from "path";

import { bridge } from "./bridge";
import * as docs from "./documents";
import { CancellationTokenSource, DisposableImpl, EventEmitter, neverToken } from "./events";
import * as services from "./services";
import { TextDocument } from "./documents";
import * as types from "./types";
import { Diagnostic, DiagnosticSeverity, Uri, WorkspaceEdit } from "./types";

const seen = new Set<string>();

function soft(name: string): any {
    if (!seen.has(name)) {
        seen.add(name);
        const line = "unimplemented " + name;
        bridge.log(line);
        bridge.notify("host/unimplemented", { method: name, extensionId: bridge.extensionId, count: 1 });
    }
    const fn = function unimplemented(): any {
        return soft(name + "()");
    };
    return new Proxy(fn, {
        get(_target, prop) {
            if (prop === "then") {
                return undefined;
            }
            if (prop === "dispose") {
                return () => {};
            }
            if (typeof prop === "symbol") {
                return undefined;
            }
            return soft(name + "." + String(prop));
        },
        apply() {
            return soft(name + "()");
        },
        construct() {
            return soft("new " + name);
        }
    });
}

function namespace(name: string, impl: object): any {
    return new Proxy(impl, {
        get(target, prop) {
            if (prop in target || typeof prop === "symbol") {
                return (target as any)[prop];
            }
            return soft(name + "." + String(prop));
        }
    });
}

function byteOffset(text: string, utf16: number): number {
    return Buffer.byteLength(text.slice(0, utf16), "utf8");
}

class DiagnosticCollection {
    private readonly buckets = new Map<string, Diagnostic[]>();
    constructor(readonly name: string) {}

    set(uri: Uri, diagnostics?: readonly Diagnostic[]): void {
        const key = uri.toString();
        if (!diagnostics) {
            this.buckets.delete(key);
        } else {
            this.buckets.set(key, diagnostics.slice());
        }
        const wire = (diagnostics || []).map((diag) => ({
            range: {
                start: { line: diag.range.start.line, character: diag.range.start.character },
                end: { line: diag.range.end.line, character: diag.range.end.character }
            },
            severity: (diag.severity === undefined ? DiagnosticSeverity.Error : diag.severity) + 1,
            message: diag.message,
            source: diag.source || ""
        }));
        bridge.notify("diagnostics/set", {
            uri: key,
            path: uri.fsPath,
            diagnostics: wire
        });
    }

    delete(uri: Uri): void {
        this.set(uri, undefined);
    }

    clear(): void {
        for (const key of Array.from(this.buckets.keys())) {
            const uri = Uri.parse(key);
            this.set(uri, undefined);
        }
    }

    get(uri: Uri): readonly Diagnostic[] {
        return this.buckets.get(uri.toString()) || [];
    }

    has(uri: Uri): boolean {
        return this.buckets.has(uri.toString());
    }

    forEach(callback: (uri: Uri, diagnostics: readonly Diagnostic[], collection: DiagnosticCollection) => void): void {
        for (const [key, diagnostics] of this.buckets) {
            callback(Uri.parse(key), diagnostics, this);
        }
    }

    dispose(): void {
        this.clear();
    }
}

const diagnosticCollections: DiagnosticCollection[] = [];

function scoreOne(selector: any, document: TextDocument): number {
    if (selector === undefined || selector === null) {
        return 0;
    }
    if (typeof selector === "string") {
        if (selector === "*") {
            return 5;
        }
        return selector === document.languageId ? 10 : 0;
    }
    if (selector.notebookType || selector.notebook) {
        return 0;
    }
    let score = 0;
    let saw = false;
    if (selector.language) {
        saw = true;
        if (selector.language === document.languageId) {
            score = 10;
        } else if (selector.language === "*") {
            score = 5;
        } else {
            return 0;
        }
    }
    if (selector.scheme) {
        saw = true;
        if (selector.scheme === document.uri.scheme) {
            score = Math.max(score, 10);
        } else if (selector.scheme === "*") {
            score = Math.max(score, 5);
        } else {
            return 0;
        }
    }
    if (selector.pattern) {
        saw = true;
        const pattern = typeof selector.pattern === "string"
            ? selector.pattern
            : selector.pattern.pattern || "";
        if (!globMatch(pattern, document.uri.fsPath)) {
            return 0;
        }
        score = Math.max(score, 10);
    }
    return saw ? score : 0;
}

function globMatch(pattern: string, fsPath: string): boolean {
    if (!pattern || pattern === "*" || pattern === "**" || pattern === "**/*") {
        return true;
    }
    const body = pattern.split("/").map((segment) => {
        if (segment === "**") {
            return ".*";
        }
        return segment.replace(/[.+^${}()|[\]\\]/g, "\\$&").replace(/\*/g, "[^/]*").replace(/\?/g, ".");
    }).join("/");
    return new RegExp("^" + body + "$").test(fsPath);
}

function match(selector: unknown, document: TextDocument): number {
    if (Array.isArray(selector)) {
        let best = 0;
        for (const item of selector) {
            best = Math.max(best, scoreOne(item, document));
        }
        return best;
    }
    return scoreOne(selector, document);
}

services.setMatcher(match);

class WorkspaceConfiguration {
    constructor(private readonly section: string | undefined, private readonly values: Map<string, unknown>) {}
    get<T>(key: string, def?: T): T | undefined {
        const full = this.section ? this.section + "." + key : key;
        if (this.values.has(full)) {
            return this.values.get(full) as T;
        }
        return def;
    }
    has(key: string): boolean {
        const full = this.section ? this.section + "." + key : key;
        return this.values.has(full);
    }
    update(): Promise<void> {
        return Promise.resolve();
    }
    inspect(): undefined {
        return undefined;
    }
}

const configurationValues = new Map<string, unknown>();

function fileTypeOf(stats: fs.Stats): number {
    if (stats.isSymbolicLink()) {
        return 64;
    }
    if (stats.isDirectory()) {
        return 2;
    }
    if (stats.isFile()) {
        return 1;
    }
    return 0;
}

const commands = {
    registerCommand(command: string, callback: (...args: unknown[]) => unknown, thisArg?: unknown) {
        return services.registerCommand(command, callback, thisArg);
    },
    registerTextEditorCommand(command: string, callback: (...args: unknown[]) => unknown, thisArg?: unknown) {
        return services.registerCommand(command, callback, thisArg);
    },
    executeCommand(command: string, ...args: unknown[]) {
        return services.executeCommand(command, args);
    },
    getCommands() {
        return Promise.resolve([]);
    }
};

const languages = {
    match,
    registerCompletionItemProvider(selector: unknown, provider: any, ...triggerCharacters: string[]) {
        return services.registerCompletion(selector, provider, triggerCharacters);
    },
    registerHoverProvider(selector: unknown, provider: any) {
        return services.registerHover(selector, provider);
    },
    registerDefinitionProvider(selector: unknown, provider: any) {
        return services.registerDefinition(selector, provider);
    },
    registerDocumentFormattingEditProvider(selector: unknown, provider: any) {
        return services.registerFormatting(selector, provider);
    },
    registerCodeActionsProvider(selector: unknown, provider: any) {
        return services.registerCodeAction(selector, provider);
    },
    registerRenameProvider(selector: unknown, provider: any) {
        return services.registerRename(selector, provider);
    },
    createDiagnosticCollection(name?: string) {
        const collection = new DiagnosticCollection(name || "");
        diagnosticCollections.push(collection);
        return collection;
    },
    getDiagnostics(uri?: Uri) {
        if (!uri) {
            const all: Array<[Uri, readonly Diagnostic[]]> = [];
            for (const collection of diagnosticCollections) {
                collection.forEach((itemUri, diagnostics) => {
                    all.push([itemUri, diagnostics]);
                });
            }
            return all;
        }
        const merged: Diagnostic[] = [];
        for (const collection of diagnosticCollections) {
            merged.push(...collection.get(uri));
        }
        return merged;
    },
    setLanguageConfiguration() {
        return new DisposableImpl(() => {});
    },
    onDidChangeDiagnostics: new EventEmitter<unknown>().event
};

const workspace = {
    get textDocuments() {
        return docs.all();
    },
    get notebookDocuments() {
        return [];
    },
    get workspaceFolders() {
        return docs.workspaceFolders();
    },
    get name() {
        const folders = docs.workspaceFolders();
        return folders && folders.length > 0 ? folders[0].name : undefined;
    },
    get rootPath() {
        return docs.rootPath();
    },
    onDidOpenTextDocument: docs.onDidOpenTextDocument.event,
    onDidChangeTextDocument: docs.onDidChangeTextDocument.event,
    onDidSaveTextDocument: docs.onDidSaveTextDocument.event,
    onDidCloseTextDocument: docs.onDidCloseTextDocument.event,
    onWillSaveTextDocument: docs.onWillSaveTextDocument.event,
    onDidChangeConfiguration: docs.onDidChangeConfiguration.event,
    onDidChangeWorkspaceFolders: docs.onDidChangeWorkspaceFolders.event,
    onDidCreateFiles: docs.onDidCreateFiles.event,
    onDidRenameFiles: docs.onDidRenameFiles.event,
    onDidDeleteFiles: docs.onDidDeleteFiles.event,
    onWillCreateFiles: docs.onWillCreateFiles.event,
    onWillRenameFiles: docs.onWillRenameFiles.event,
    onWillDeleteFiles: docs.onWillDeleteFiles.event,
    onDidOpenNotebookDocument: docs.onDidOpenNotebookDocument.event,
    onDidChangeNotebookDocument: docs.onDidChangeNotebookDocument.event,
    onDidSaveNotebookDocument: docs.onDidSaveNotebookDocument.event,
    onDidCloseNotebookDocument: docs.onDidCloseNotebookDocument.event,
    openTextDocument(arg?: string | Uri | { content?: string; language?: string }) {
        if (arg && typeof arg === "object" && "content" in arg && !(arg instanceof Uri)) {
            const text = arg.content || "";
            const language = arg.language || "plaintext";
            return Promise.resolve(docs.open("untitled:buffer", language, 1, text));
        }
        const uri = typeof arg === "string" ? docs.asUri(arg) : arg instanceof Uri ? arg : undefined;
        if (!uri) {
            return Promise.reject(new Error("openTextDocument needs a uri"));
        }
        const existing = docs.find(uri.toString());
        if (existing) {
            return Promise.resolve(existing);
        }
        const text = fs.readFileSync(uri.fsPath, "utf8");
        return Promise.resolve(docs.open(uri.toString(), "plaintext", 1, text));
    },
    getConfiguration(section?: string) {
        return new WorkspaceConfiguration(section, configurationValues);
    },
    getWorkspaceFolder(uri: Uri) {
        const folders = docs.workspaceFolders();
        if (!folders) {
            return undefined;
        }
        for (const folder of folders) {
            if (uri.fsPath === folder.uri.fsPath || uri.fsPath.startsWith(folder.uri.fsPath + "/")) {
                return folder;
            }
        }
        return undefined;
    },
    asRelativePath(uriOrString: Uri | string) {
        const value = typeof uriOrString === "string" ? uriOrString : uriOrString.fsPath;
        const root = docs.rootPath();
        if (root && value.startsWith(root + "/")) {
            return value.slice(root.length + 1);
        }
        return value;
    },
    applyEdit(edit: WorkspaceEdit) {
        if (!edit || typeof edit.entries !== "function") {
            return Promise.resolve(false);
        }
        for (const [uri, edits] of edit.entries()) {
            const doc = docs.find(uri.toString());
            const text = doc ? doc.getText() : "";
            for (const change of edits) {
                const start = doc ? doc.offsetAt(change.range.start) : 0;
                const end = doc ? doc.offsetAt(change.range.end) : 0;
                bridge.notify("doc/applyEdit", {
                    path: uri.fsPath,
                    pos: byteOffset(text, start),
                    len: byteOffset(text, end) - byteOffset(text, start),
                    text: change.newText
                });
            }
        }
        return Promise.resolve(true);
    },
    createFileSystemWatcher() {
        const create = new EventEmitter<Uri>();
        const change = new EventEmitter<Uri>();
        const deleted = new EventEmitter<Uri>();
        return {
            ignoreCreateEvents: false,
            ignoreChangeEvents: false,
            ignoreDeleteEvents: false,
            onDidCreate: create.event,
            onDidChange: change.event,
            onDidDelete: deleted.event,
            dispose() {
                create.dispose();
                change.dispose();
                deleted.dispose();
            }
        };
    },
    fs: {
        readFile(uri: Uri) {
            return Promise.resolve(fs.readFileSync(uri.fsPath));
        },
        writeFile(uri: Uri, content: Uint8Array) {
            fs.mkdirSync(path.dirname(uri.fsPath), { recursive: true });
            fs.writeFileSync(uri.fsPath, content);
            return Promise.resolve();
        },
        stat(uri: Uri) {
            const stats = fs.statSync(uri.fsPath);
            return Promise.resolve({
                type: fileTypeOf(stats),
                ctime: stats.ctimeMs,
                mtime: stats.mtimeMs,
                size: stats.size
            });
        },
        readDirectory(uri: Uri) {
            const names = fs.readdirSync(uri.fsPath);
            return Promise.resolve(names.map((name) => {
                const stats = fs.statSync(path.join(uri.fsPath, name));
                return [name, fileTypeOf(stats)];
            }));
        },
        createDirectory(uri: Uri) {
            fs.mkdirSync(uri.fsPath, { recursive: true });
            return Promise.resolve();
        },
        delete(uri: Uri) {
            fs.rmSync(uri.fsPath, { recursive: true, force: true });
            return Promise.resolve();
        }
    }
};

function showMessage(level: string, message: string): Promise<undefined> {
    bridge.notify("ui/message", { level, message: String(message) });
    return Promise.resolve(undefined);
}

const windowApi = {
    get activeTextEditor() {
        return docs.activeEditor();
    },
    get visibleTextEditors() {
        const editor = docs.activeEditor();
        return editor ? [editor] : [];
    },
    onDidChangeActiveTextEditor: docs.onDidChangeActiveTextEditor.event,
    onDidChangeVisibleTextEditors: new EventEmitter<unknown>().event,
    tabGroups: { all: [] as unknown[], onDidChangeTabs: undefined as undefined },
    showInformationMessage(message: string) {
        return showMessage("info", message);
    },
    showWarningMessage(message: string) {
        return showMessage("warning", message);
    },
    showErrorMessage(message: string) {
        return showMessage("error", message);
    },
    createOutputChannel(name: string) {
        return {
            name,
            append(text: string) {
                bridge.notify("output/append", { channel: name, text: String(text) });
            },
            appendLine(text: string) {
                bridge.notify("output/append", { channel: name, text: String(text) + "\n" });
            },
            replace() {},
            clear() {},
            show() {},
            hide() {},
            dispose() {}
        };
    },
    withProgress(_options: unknown, task: (progress: { report: () => void }, token: unknown) => unknown) {
        return Promise.resolve(task({ report() {} }, neverToken()));
    },
    showTextDocument() {
        return Promise.resolve(docs.activeEditor());
    },
    createStatusBarItem() {
        return {
            text: "",
            tooltip: "",
            show() {},
            hide() {},
            dispose() {}
        };
    },
    setStatusBarMessage(text: string) {
        bridge.notify("status/set", { text: String(text) });
        return new DisposableImpl(() => {});
    }
};

const env = {
    appName: "HaikuIDE",
    appHost: "desktop",
    language: "en",
    uriScheme: "file",
    remoteName: undefined as undefined,
    sessionId: "haikuide",
    machineId: "haikuide",
    uiKind: 1,
    clipboard: {
        readText() {
            return Promise.resolve("");
        },
        writeText() {
            return Promise.resolve();
        }
    },
    openExternal() {
        return Promise.resolve(false);
    },
    asExternalUri(uri: Uri) {
        return Promise.resolve(uri);
    }
};

const api: any = {
    version: "1.85.0",
    commands: namespace("commands", commands),
    languages: namespace("languages", languages),
    workspace: namespace("workspace", workspace),
    window: namespace("window", windowApi),
    env: namespace("env", env),
    EventEmitter,
    Disposable: DisposableImpl,
    CancellationTokenSource,
    CancellationError: types.CancellationError
};

for (const name of Object.keys(types)) {
    if (api[name] === undefined) {
        api[name] = (types as any)[name];
    }
}

api.FileType = { Unknown: 0, File: 1, Directory: 2, SymbolicLink: 64 };
api.ProgressLocation = { SourceControl: 1, Window: 10, Notification: 15 };
api.UIKind = { Desktop: 1, Web: 2 };
api.ExtensionMode = { Production: 1, Development: 2, Test: 3 };
api.ViewColumn = { Active: -1, Beside: -2, One: 1, Two: 2, Three: 3 };
api.TextEditorRevealType = { Default: 0, InCenter: 1, InCenterIfOutsideViewport: 2, AtTop: 3 };

const proxied = new Proxy(api, {
    get(target, prop) {
        if (prop in target || typeof prop === "symbol") {
            return target[prop];
        }
        return soft("vscode." + String(prop));
    }
});

export = proxied;
