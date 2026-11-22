/* Document cache. The shell owns the bytes. Providers read this copy.
 *
 * Copyright (c) 2026 Sean Collins, 2 Paws Machine and Engineering. MIT.
 */

import { EventEmitter } from "./events";
import { EndOfLine, Position, Range, Selection, TextDocumentSaveReason, Uri, offsetAt, positionAt } from "./types";

export interface TextLine {
    lineNumber: number;
    text: string;
    range: Range;
    rangeIncludingLineBreak: Range;
    firstNonWhitespaceCharacterIndex: number;
    isEmptyOrWhitespace: boolean;
}

export class TextDocument {
    readonly uri: Uri;
    languageId: string;
    version: number;
    private text: string;
    isDirty = false;
    isClosed = false;
    eol = EndOfLine.LF;

    constructor(uri: Uri, languageId: string, version: number, text: string) {
        this.uri = uri;
        this.languageId = languageId;
        this.version = version;
        this.text = text;
    }

    get fileName(): string {
        return this.uri.fsPath;
    }

    get lineCount(): number {
        let count = 1;
        for (let i = 0; i < this.text.length; i++) {
            if (this.text.charCodeAt(i) === 10) {
                count++;
            }
        }
        return count;
    }

    getText(range?: Range): string {
        if (!range) {
            return this.text;
        }
        const start = offsetAt(this.text, range.start);
        const end = offsetAt(this.text, range.end);
        return this.text.slice(start, end);
    }

    offsetAt(position: Position): number {
        return offsetAt(this.text, position);
    }

    positionAt(offset: number): Position {
        return positionAt(this.text, offset);
    }

    lineAt(lineOrPosition: number | Position): TextLine {
        const line = typeof lineOrPosition === "number" ? lineOrPosition : lineOrPosition.line;
        const parts = this.text.split("\n");
        const text = parts[line] !== undefined ? parts[line] : "";
        const start = new Position(line, 0);
        const end = new Position(line, text.length);
        const next = line + 1 < parts.length ? new Position(line + 1, 0) : end;
        const trimmed = text.replace(/^[ \t]+/, "");
        return {
            lineNumber: line,
            text,
            range: new Range(start, end),
            rangeIncludingLineBreak: new Range(start, next),
            firstNonWhitespaceCharacterIndex: text.length - trimmed.length,
            isEmptyOrWhitespace: trimmed.length === 0
        };
    }

    save(): Promise<boolean> {
        return Promise.resolve(true);
    }

    setText(text: string, version: number): string {
        const previous = this.text;
        this.text = text;
        this.version = version;
        this.isDirty = true;
        return previous;
    }
}

export interface ChangeEvent {
    document: TextDocument;
    contentChanges: Array<{ range: Range; rangeOffset: number; rangeLength: number; text: string }>;
    reason?: number;
}

export interface SaveEvent {
    document: TextDocument;
    reason: number;
    waitUntil(thenable: Promise<unknown>): void;
}

export interface WorkspaceFolder {
    uri: Uri;
    name: string;
    index: number;
}

const byUri = new Map<string, TextDocument>();
const byPath = new Map<string, TextDocument>();
let active: TextDocument | undefined;
let folders: WorkspaceFolder[] | undefined;

export const onDidOpenTextDocument = new EventEmitter<TextDocument>();
export const onDidChangeTextDocument = new EventEmitter<ChangeEvent>();
export const onDidSaveTextDocument = new EventEmitter<TextDocument>();
export const onDidCloseTextDocument = new EventEmitter<TextDocument>();
export const onWillSaveTextDocument = new EventEmitter<SaveEvent>();
export const onDidChangeConfiguration = new EventEmitter<{ affectsConfiguration(section: string): boolean }>();
export const onDidChangeWorkspaceFolders = new EventEmitter<{ added: WorkspaceFolder[]; removed: WorkspaceFolder[] }>();
export const onDidChangeActiveTextEditor = new EventEmitter<unknown>();

export const onDidCreateFiles = new EventEmitter<unknown>();
export const onDidRenameFiles = new EventEmitter<unknown>();
export const onDidDeleteFiles = new EventEmitter<unknown>();
export const onWillCreateFiles = new EventEmitter<unknown>();
export const onWillRenameFiles = new EventEmitter<unknown>();
export const onWillDeleteFiles = new EventEmitter<unknown>();
export const onDidOpenNotebookDocument = new EventEmitter<unknown>();
export const onDidChangeNotebookDocument = new EventEmitter<unknown>();
export const onDidSaveNotebookDocument = new EventEmitter<unknown>();
export const onDidCloseNotebookDocument = new EventEmitter<unknown>();

export function asUri(value: string): Uri {
    if (/^[a-zA-Z][a-zA-Z0-9+.-]*:/.test(value)) {
        return Uri.parse(value);
    }
    return Uri.file(value);
}

function remember(doc: TextDocument): void {
    byUri.set(doc.uri.toString(), doc);
    byPath.set(doc.uri.fsPath, doc);
}

export function find(uriOrPath: string | undefined): TextDocument | undefined {
    if (!uriOrPath) {
        return active;
    }
    const direct = byUri.get(uriOrPath) || byPath.get(uriOrPath);
    if (direct) {
        return direct;
    }
    const uri = asUri(uriOrPath);
    return byUri.get(uri.toString()) || byPath.get(uri.fsPath);
}

export function all(): TextDocument[] {
    return Array.from(byUri.values());
}

export function activeDocument(): TextDocument | undefined {
    return active;
}

export function workspaceFolders(): WorkspaceFolder[] | undefined {
    return folders;
}

export function rootPath(): string | undefined {
    if (!folders || folders.length === 0) {
        return undefined;
    }
    return folders[0].uri.fsPath;
}

export function setWorkspace(rootUri: string | undefined, root: string | undefined): void {
    const path = root || (rootUri ? asUri(rootUri).fsPath : "");
    if (!path) {
        folders = undefined;
        return;
    }
    const uri = rootUri ? asUri(rootUri) : Uri.file(path);
    const name = uri.fsPath.split("/").filter((part) => part.length > 0).pop() || uri.fsPath;
    folders = [{ uri, name, index: 0 }];
}

export function open(uriValue: string, languageId: string, version: number, text: string): TextDocument {
    const uri = asUri(uriValue);
    let doc = byUri.get(uri.toString());
    if (!doc) {
        doc = new TextDocument(uri, languageId, version, text);
        remember(doc);
        active = doc;
        onDidOpenTextDocument.fire(doc);
        onDidChangeActiveTextEditor.fire(editorFor(doc));
        return doc;
    }
    doc.languageId = languageId;
    doc.isClosed = false;
    const previous = doc.setText(text, version);
    active = doc;
    if (previous !== text) {
        fireChange(doc, previous, text);
    }
    onDidOpenTextDocument.fire(doc);
    return doc;
}

export function change(uriValue: string, version: number, text: string): TextDocument | undefined {
    const doc = find(uriValue);
    if (!doc) {
        return undefined;
    }
    const previous = doc.setText(text, version);
    fireChange(doc, previous, text);
    return doc;
}

function fireChange(doc: TextDocument, previous: string, text: string): void {
    const end = positionAt(previous, previous.length);
    onDidChangeTextDocument.fire({
        document: doc,
        contentChanges: [{
            range: new Range(new Position(0, 0), end),
            rangeOffset: 0,
            rangeLength: previous.length,
            text
        }]
    });
}

export function save(uriValue: string): TextDocument | undefined {
    const doc = find(uriValue);
    if (!doc) {
        return undefined;
    }
    const waits: Array<Promise<unknown>> = [];
    onWillSaveTextDocument.fire({
        document: doc,
        reason: TextDocumentSaveReason.Manual,
        waitUntil(thenable: Promise<unknown>) {
            waits.push(thenable);
        }
    });
    doc.isDirty = false;
    onDidSaveTextDocument.fire(doc);
    return doc;
}

export function close(uriValue: string): void {
    const doc = find(uriValue);
    if (!doc) {
        return;
    }
    doc.isClosed = true;
    byUri.delete(doc.uri.toString());
    byPath.delete(doc.uri.fsPath);
    onDidCloseTextDocument.fire(doc);
    if (active === doc) {
        active = byUri.values().next().value;
        onDidChangeActiveTextEditor.fire(active ? editorFor(active) : undefined);
    }
}

export function editorFor(doc: TextDocument | undefined): unknown {
    if (!doc) {
        return undefined;
    }
    const selection = new Selection(0, 0, 0, 0);
    return {
        document: doc,
        selection,
        selections: [selection],
        visibleRanges: [new Range(0, 0, Math.max(0, doc.lineCount - 1), 0)],
        options: { tabSize: 4, insertSpaces: true },
        viewColumn: 1,
        edit: () => Promise.resolve(true),
        revealRange: () => {},
        show: () => {},
        hide: () => {}
    };
}

export function activeEditor(): unknown {
    return editorFor(active);
}
