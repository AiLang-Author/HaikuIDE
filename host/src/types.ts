/* vscode value types the language client constructs.
 *
 * Copyright (c) 2026 Sean Collins, 2 Paws Machine and Engineering. MIT.
 */

export class Position {
    line: number;
    character: number;

    constructor(line: number, character: number) {
        this.line = line;
        this.character = character;
    }

    isEqual(other: Position): boolean {
        return this.line === other.line && this.character === other.character;
    }

    isBefore(other: Position): boolean {
        return this.line < other.line || (this.line === other.line && this.character < other.character);
    }

    isAfter(other: Position): boolean {
        return other.isBefore(this);
    }

    translate(lineDelta = 0, characterDelta = 0): Position {
        return new Position(this.line + lineDelta, this.character + characterDelta);
    }

    with(change: {line?: number; character?: number}): Position {
        return new Position(
            change.line === undefined ? this.line : change.line,
            change.character === undefined ? this.character : change.character
        );
    }

    compareTo(other: Position): number {
        if (this.isBefore(other)) {
            return -1;
        }
        if (this.isAfter(other)) {
            return 1;
        }
        return 0;
    }
}

export class Range {
    start: Position;
    end: Position;

    constructor(startLine: number | Position, startChar: number | Position, endLine?: number, endChar?: number) {
        if (typeof startLine === "number") {
            this.start = new Position(startLine, startChar as number);
            this.end = new Position(endLine as number, endChar as number);
        } else {
            this.start = startLine;
            this.end = startChar as Position;
        }
    }

    get isEmpty(): boolean {
        return this.start.isEqual(this.end);
    }

    get isSingleLine(): boolean {
        return this.start.line === this.end.line;
    }

    contains(positionOrRange: Position | Range): boolean {
        if (positionOrRange instanceof Range) {
            return this.contains(positionOrRange.start) && this.contains(positionOrRange.end);
        }
        return !positionOrRange.isBefore(this.start) && !this.end.isBefore(positionOrRange);
    }
}

export class Selection extends Range {
    anchor: Position;
    active: Position;

    constructor(anchorLine: number | Position, anchorChar: number | Position, activeLine?: number, activeChar?: number) {
        if (typeof anchorLine === "number") {
            const anchor = new Position(anchorLine, anchorChar as number);
            const active = new Position(activeLine as number, activeChar as number);
            super(anchor, active);
            this.anchor = anchor;
            this.active = active;
        } else {
            super(anchorLine, anchorChar as Position);
            this.anchor = anchorLine;
            this.active = anchorChar as Position;
        }
    }

    get isReversed(): boolean {
        return this.active.isBefore(this.anchor);
    }
}

export class Uri {
    scheme: string;
    authority: string;
    path: string;
    query: string;
    fragment: string;

    private constructor(scheme: string, authority: string, path: string, query: string, fragment: string) {
        this.scheme = scheme;
        this.authority = authority;
        this.path = path;
        this.query = query;
        this.fragment = fragment;
    }

    static parse(value: string): Uri {
        const match = /^([a-zA-Z][a-zA-Z0-9+.-]*):\/\/([^/?#]*)([^?#]*)(?:\?([^#]*))?(?:#(.*))?$/.exec(value);
        if (!match) {
            return new Uri("file", "", value, "", "");
        }
        let authority = match[2] || "";
        let path = decodeURIComponent(match[3] || "");
        if (authority === "localhost") {
            authority = "";
        }
        if (match[1] === "file" && path[0] !== "/") {
            path = "/" + path;
        }
        return new Uri(match[1], authority, path, match[4] || "", match[5] || "");
    }

    static file(fsPath: string): Uri {
        let path = fsPath;
        if (path[0] !== "/") {
            path = "/" + path;
        }
        return new Uri("file", "", path, "", "");
    }

    static joinPath(base: Uri, ...parts: string[]): Uri {
        const chunks = base.path.split("/").filter((part) => part.length > 0);
        for (const part of parts) {
            for (const piece of part.split("/")) {
                if (piece === "" || piece === ".") {
                    continue;
                }
                if (piece === "..") {
                    chunks.pop();
                } else {
                    chunks.push(piece);
                }
            }
        }
        return new Uri(base.scheme, base.authority, "/" + chunks.join("/"), "", "");
    }

    static from(components: {scheme: string; authority?: string; path?: string; query?: string; fragment?: string}): Uri {
        return new Uri(components.scheme, components.authority || "", components.path || "", components.query || "", components.fragment || "");
    }

    get fsPath(): string {
        return this.path;
    }

    toString(): string {
        const encoded = this.path.split("/").map((part) => encodeURIComponent(part)).join("/");
        const auth = this.authority ? this.authority : "";
        return this.scheme + "://" + auth + encoded;
    }

    toJSON(): string {
        return this.toString();
    }

    with(change: {scheme?: string; authority?: string; path?: string; query?: string; fragment?: string}): Uri {
        return new Uri(
            change.scheme === undefined ? this.scheme : change.scheme,
            change.authority === undefined ? this.authority : change.authority,
            change.path === undefined ? this.path : change.path,
            change.query === undefined ? this.query : change.query,
            change.fragment === undefined ? this.fragment : change.fragment
        );
    }
}

export function offsetAt(text: string, position: Position): number {
    let line = 0;
    let i = 0;
    while (i < text.length && line < position.line) {
        if (text[i] === "\n") {
            line += 1;
        }
        i += 1;
    }
    let need = Math.max(0, position.character);
    while (i < text.length && text[i] !== "\n" && need > 0) {
        const cp = text.codePointAt(i) || 0;
        const units = cp > 0xffff ? 2 : 1;
        if (units > need) {
            return i;
        }
        need -= units;
        i += units;
    }
    return i;
}

export function positionAt(text: string, offset: number): Position {
    if (offset < 0) {
        offset = 0;
    }
    if (offset > text.length) {
        offset = text.length;
    }
    let line = 0;
    let lineStart = 0;
    for (let i = 0; i < offset; i += 1) {
        if (text[i] === "\n") {
            line += 1;
            lineStart = i + 1;
        }
    }
    let character = 0;
    for (let i = lineStart; i < offset; ) {
        const cp = text.codePointAt(i) || 0;
        const units = cp > 0xffff ? 2 : 1;
        if (i + units > offset) {
            break;
        }
        character += units;
        i += units;
    }
    return new Position(line, character);
}

export class MarkdownString {
    value: string;
    isTrusted = false;
    supportThemeIcons = false;
    supportHtml = false;

    constructor(value?: string) {
        this.value = value || "";
    }

    appendText(value: string): MarkdownString {
        this.value += value.replace(/[\\`*_{}[\]()#+\-.!]/g, "\\$&");
        return this;
    }

    appendMarkdown(value: string): MarkdownString {
        this.value += value;
        return this;
    }

    appendCodeblock(value: string, language?: string): MarkdownString {
        this.value += "\n```" + (language || "") + "\n" + value + "\n```\n";
        return this;
    }
}

export class SnippetString {
    value: string;
    constructor(value?: string) {
        this.value = value || "";
    }
    appendText(text: string): SnippetString {
        this.value += text;
        return this;
    }
    appendTabstop(number = 0): SnippetString {
        this.value += "$" + number;
        return this;
    }
    appendPlaceholder(value: string, number = 0): SnippetString {
        this.value += "${" + number + ":" + value + "}";
        return this;
    }
}

export class TextEdit {
    range: Range;
    newText: string;
    constructor(range: Range, newText: string) {
        this.range = range;
        this.newText = newText;
    }
    static replace(range: Range, newText: string): TextEdit {
        return new TextEdit(range, newText);
    }
    static insert(position: Position, newText: string): TextEdit {
        return new TextEdit(new Range(position, position), newText);
    }
    static delete(range: Range): TextEdit {
        return new TextEdit(range, "");
    }
}

export class Location {
    uri: Uri;
    range: Range;
    constructor(uri: Uri, rangeOrPosition: Range | Position) {
        this.uri = uri;
        this.range = rangeOrPosition instanceof Range ? rangeOrPosition : new Range(rangeOrPosition, rangeOrPosition);
    }
}

export class DiagnosticRelatedInformation {
    location: Location;
    message: string;
    constructor(location: Location, message: string) {
        this.location = location;
        this.message = message;
    }
}

export enum DiagnosticSeverity {
    Error = 0,
    Warning = 1,
    Information = 2,
    Hint = 3
}

export class Diagnostic {
    range: Range;
    message: string;
    severity: DiagnosticSeverity;
    source?: string;
    code?: string | number | {value: string | number; target: Uri};
    relatedInformation?: DiagnosticRelatedInformation[];
    tags?: number[];
    constructor(range: Range, message: string, severity: DiagnosticSeverity = DiagnosticSeverity.Error) {
        this.range = range;
        this.message = message;
        this.severity = severity;
    }
}

export enum CompletionItemKind {
    Text = 0,
    Method = 1,
    Function = 2,
    Constructor = 3,
    Field = 4,
    Variable = 5,
    Class = 6,
    Interface = 7,
    Module = 8,
    Property = 9,
    Unit = 10,
    Value = 11,
    Enum = 12,
    Keyword = 13,
    Snippet = 14,
    Color = 15,
    File = 16,
    Reference = 17,
    Folder = 18,
    EnumMember = 19,
    Constant = 20,
    Struct = 21,
    Event = 22,
    Operator = 23,
    TypeParameter = 24
}

export enum CompletionItemTag {
    Deprecated = 1
}

export enum CompletionTriggerKind {
    Invoke = 0,
    TriggerCharacter = 1,
    TriggerForIncompleteCompletions = 2
}

export class CompletionItem {
    label: string;
    kind?: number;
    detail?: string;
    documentation?: string | MarkdownString;
    sortText?: string;
    filterText?: string;
    insertText?: string | SnippetString;
    range?: Range;
    commitCharacters?: string[];
    command?: {title: string; command: string; arguments?: unknown[]};
    preselect?: boolean;
    additionalTextEdits?: TextEdit[];
    constructor(label: string | {label: string; detail?: string; description?: string}, kind?: number) {
        if (typeof label === "string") {
            this.label = label;
        } else {
            this.label = label.label;
            if (label.detail || label.description) {
                this.detail = label.detail || label.description;
            }
        }
        this.kind = kind;
    }
}

export class CompletionList {
    items: CompletionItem[];
    isIncomplete: boolean;
    constructor(items?: CompletionItem[], isIncomplete = false) {
        this.items = items || [];
        this.isIncomplete = isIncomplete;
    }
}

export class Hover {
    contents: string | MarkdownString | Array<string | MarkdownString>;
    range?: Range;
    constructor(contents: Hover["contents"], range?: Range) {
        this.contents = contents;
        this.range = range;
    }
}

export class WorkspaceEdit {
    private map = new Map<string, TextEdit[]>();

    set(uri: Uri, edits: TextEdit[]): void {
        this.map.set(uri.toString(), edits);
    }

    get(uri: Uri): TextEdit[] {
        return this.map.get(uri.toString()) || [];
    }

    entries(): Array<[Uri, TextEdit[]]> {
        const out: Array<[Uri, TextEdit[]]> = [];
        for (const [key, edits] of this.map) {
            out.push([Uri.parse(key), edits]);
        }
        return out;
    }

    has(uri: Uri): boolean {
        return this.map.has(uri.toString());
    }

    insert(uri: Uri, position: Position, newText: string): void {
        this.push(uri, TextEdit.insert(position, newText));
    }

    replace(uri: Uri, range: Range, newText: string): void {
        this.push(uri, TextEdit.replace(range, newText));
    }

    delete(uri: Uri, range: Range): void {
        this.push(uri, TextEdit.delete(range));
    }

    private push(uri: Uri, edit: TextEdit): void {
        const key = uri.toString();
        const list = this.map.get(key) || [];
        list.push(edit);
        this.map.set(key, list);
    }
}

export class CodeAction {
    title: string;
    kind?: CodeActionKind;
    edit?: WorkspaceEdit;
    command?: {title: string; command: string};
    diagnostics?: Diagnostic[];
    isPreferred?: boolean;
    constructor(title: string, kind?: CodeActionKind) {
        this.title = title;
        this.kind = kind;
    }
}

export class CodeActionKind {
    static Empty = new CodeActionKind("");
    static QuickFix = new CodeActionKind("quickfix");
    static Refactor = new CodeActionKind("refactor");
    static Source = new CodeActionKind("source");
    readonly value: string;
    constructor(value: string) {
        this.value = value;
    }
    append(parts: string): CodeActionKind {
        return new CodeActionKind(this.value ? this.value + "." + parts : parts);
    }
    contains(other: CodeActionKind): boolean {
        return this.value === other.value || other.value.startsWith(this.value + ".");
    }
}

export class SymbolInformation {
    name: string;
    kind: number;
    containerName: string;
    location: Location;
    constructor(name: string, kind: number, containerName: string, location: Location) {
        this.name = name;
        this.kind = kind;
        this.containerName = containerName;
        this.location = location;
    }
}

export class DocumentSymbol {
    name: string;
    detail: string;
    kind: number;
    range: Range;
    selectionRange: Range;
    children: DocumentSymbol[] = [];
    constructor(name: string, detail: string, kind: number, range: Range, selectionRange: Range) {
        this.name = name;
        this.detail = detail;
        this.kind = kind;
        this.range = range;
        this.selectionRange = selectionRange;
    }
}

export class CodeLens {
    range: Range;
    command?: {title: string; command: string};
    constructor(range: Range, command?: {title: string; command: string}) {
        this.range = range;
        this.command = command;
    }
}

export class DocumentLink {
    range: Range;
    target?: Uri;
    constructor(range: Range, target?: Uri) {
        this.range = range;
        this.target = target;
    }
}

export class InlayHint {
    position: Position;
    label: string | {value: string}[];
    kind?: number;
    constructor(position: Position, label: string | {value: string}[], kind?: number) {
        this.position = position;
        this.label = label;
        this.kind = kind;
    }
}

export class InlayHintLabelPart {
    value: string;
    constructor(value: string) {
        this.value = value;
    }
}

export class CallHierarchyItem {
    kind: number;
    name: string;
    detail: string;
    uri: Uri;
    range: Range;
    selectionRange: Range;
    constructor(kind: number, name: string, detail: string, uri: Uri, range: Range, selectionRange: Range) {
        this.kind = kind;
        this.name = name;
        this.detail = detail;
        this.uri = uri;
        this.range = range;
        this.selectionRange = selectionRange;
    }
}

export class TypeHierarchyItem extends CallHierarchyItem {}

export class CallHierarchyIncomingCall {
    from: CallHierarchyItem;
    fromRanges: Range[];
    constructor(from: CallHierarchyItem, fromRanges: Range[]) {
        this.from = from;
        this.fromRanges = fromRanges;
    }
}

export class CallHierarchyOutgoingCall {
    to: CallHierarchyItem;
    fromRanges: Range[];
    constructor(to: CallHierarchyItem, fromRanges: Range[]) {
        this.to = to;
        this.fromRanges = fromRanges;
    }
}

export class SignatureHelp {
    signatures: SignatureInformation[] = [];
    activeSignature = 0;
    activeParameter = 0;
}

export class SignatureInformation {
    label: string;
    documentation?: string | MarkdownString;
    parameters: ParameterInformation[] = [];
    constructor(label: string, documentation?: string | MarkdownString) {
        this.label = label;
        this.documentation = documentation;
    }
}

export class ParameterInformation {
    label: string | [number, number];
    documentation?: string | MarkdownString;
    constructor(label: string | [number, number], documentation?: string | MarkdownString) {
        this.label = label;
        this.documentation = documentation;
    }
}

export class DocumentHighlight {
    range: Range;
    kind?: number;
    constructor(range: Range, kind?: number) {
        this.range = range;
        this.kind = kind;
    }
}

export class Color {
    red: number;
    green: number;
    blue: number;
    alpha: number;
    constructor(red: number, green: number, blue: number, alpha: number) {
        this.red = red;
        this.green = green;
        this.blue = blue;
        this.alpha = alpha;
    }
}

export class ColorInformation {
    range: Range;
    color: Color;
    constructor(range: Range, color: Color) {
        this.range = range;
        this.color = color;
    }
}

export class ColorPresentation {
    label: string;
    constructor(label: string) {
        this.label = label;
    }
}

export class FoldingRange {
    start: number;
    end: number;
    kind?: number;
    constructor(start: number, end: number, kind?: number) {
        this.start = start;
        this.end = end;
        this.kind = kind;
    }
}

export class SelectionRange {
    range: Range;
    parent?: SelectionRange;
    constructor(range: Range, parent?: SelectionRange) {
        this.range = range;
        this.parent = parent;
    }
}

export class InlineValueText {
    range: Range;
    text: string;
    constructor(range: Range, text: string) {
        this.range = range;
        this.text = text;
    }
}

export class InlineValueVariableLookup {
    range: Range;
    variableName: string;
    caseSensitiveLookup: boolean;
    constructor(range: Range, variableName: string, caseSensitiveLookup: boolean) {
        this.range = range;
        this.variableName = variableName;
        this.caseSensitiveLookup = caseSensitiveLookup;
    }
}

export class InlineValueEvaluatableExpression {
    range: Range;
    expression?: string;
    constructor(range: Range, expression?: string) {
        this.range = range;
        this.expression = expression;
    }
}

export class SemanticTokens {
    data: Uint32Array;
    resultId?: string;
    constructor(data: Uint32Array, resultId?: string) {
        this.data = data;
        this.resultId = resultId;
    }
}

export class SemanticTokensEdit {
    start: number;
    deleteCount: number;
    data?: Uint32Array;
    constructor(start: number, deleteCount: number, data?: Uint32Array) {
        this.start = start;
        this.deleteCount = deleteCount;
        this.data = data;
    }
}

export class SemanticTokensEdits {
    edits: SemanticTokensEdit[];
    resultId?: string;
    constructor(edits: SemanticTokensEdit[], resultId?: string) {
        this.edits = edits;
        this.resultId = resultId;
    }
}

export class LinkedEditingRanges {
    ranges: Range[];
    wordPattern?: RegExp;
    constructor(ranges: Range[], wordPattern?: RegExp) {
        this.ranges = ranges;
        this.wordPattern = wordPattern;
    }
}

export class RelativePattern {
    base: string | Uri;
    pattern: string;
    constructor(base: string | Uri | {uri: Uri}, pattern: string) {
        if (typeof base === "string" || base instanceof Uri) {
            this.base = base;
        } else {
            this.base = base.uri;
        }
        this.pattern = pattern;
    }
}

export class InlineCompletionItem {
    insertText: string | SnippetString;
    range?: Range;
    command?: {title: string; command: string};
    constructor(insertText: string | SnippetString, range?: Range, command?: {title: string; command: string}) {
        this.insertText = insertText;
        this.range = range;
        this.command = command;
    }
}

export class InlineCompletionList {
    items: InlineCompletionItem[];
    constructor(items: InlineCompletionItem[]) {
        this.items = items;
    }
}

export enum TextDocumentSaveReason {
    Manual = 1,
    AfterDelay = 2,
    FocusOut = 3
}

export enum EndOfLine {
    LF = 1,
    CRLF = 2
}

export class CancellationError extends Error {
    constructor(message?: string) {
        super(message || "Canceled");
        this.name = "Canceled";
    }
}

export class ThemeIcon {
    id: string;
    color?: string;
    static File = new ThemeIcon("file");
    static Folder = new ThemeIcon("folder");
    constructor(id: string, color?: string) {
        this.id = id;
        this.color = color;
    }
}
