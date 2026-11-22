/* Provider registry. The shell asks; registered extensions answer.
 *
 * Copyright (c) 2026 Sean Collins, 2 Paws Machine and Engineering. MIT.
 */

import { bridge } from "./bridge";
import { CancellationTokenSource, DisposableImpl } from "./events";
import { find, TextDocument } from "./documents";
import { CompletionItem, CompletionList, CompletionTriggerKind, Hover, Location, MarkdownString, Position, Range, SnippetString } from "./types";

export interface ProviderRegistration {
    selector: unknown;
    provider: any;
    triggers: string[];
}

export const completionProviders: ProviderRegistration[] = [];
export const hoverProviders: ProviderRegistration[] = [];
export const definitionProviders: ProviderRegistration[] = [];
export const formattingProviders: ProviderRegistration[] = [];
export const codeActionProviders: ProviderRegistration[] = [];
export const renameProviders: ProviderRegistration[] = [];

const commands = new Map<string, { callback: (...args: unknown[]) => unknown; thisArg: unknown }>();

export let matchDocument: (selector: unknown, document: TextDocument) => number = () => 0;

export function setMatcher(fn: (selector: unknown, document: TextDocument) => number): void {
    matchDocument = fn;
}

function push(list: ProviderRegistration[], selector: unknown, provider: any, triggers: string[]): DisposableImpl {
    const entry: ProviderRegistration = { selector, provider, triggers };
    list.push(entry);
    return new DisposableImpl(() => {
        const index = list.indexOf(entry);
        if (index >= 0) {
            list.splice(index, 1);
        }
    });
}

export function registerCompletion(selector: unknown, provider: any, triggers: string[]): DisposableImpl {
    return push(completionProviders, selector, provider, triggers);
}

export function registerHover(selector: unknown, provider: any): DisposableImpl {
    return push(hoverProviders, selector, provider, []);
}

export function registerDefinition(selector: unknown, provider: any): DisposableImpl {
    return push(definitionProviders, selector, provider, []);
}

export function registerFormatting(selector: unknown, provider: any): DisposableImpl {
    return push(formattingProviders, selector, provider, []);
}

export function registerCodeAction(selector: unknown, provider: any): DisposableImpl {
    return push(codeActionProviders, selector, provider, []);
}

export function registerRename(selector: unknown, provider: any): DisposableImpl {
    return push(renameProviders, selector, provider, []);
}

export function registerCommand(command: string, callback: (...args: unknown[]) => unknown, thisArg?: unknown): DisposableImpl {
    commands.set(command, { callback, thisArg });
    bridge.notify("command/register", { command });
    return new DisposableImpl(() => {
        commands.delete(command);
        bridge.notify("command/unregister", { command });
    });
}

export function executeCommand(command: string, args: unknown[]): Promise<unknown> {
    const found = commands.get(command);
    if (!found) {
        return Promise.resolve(undefined);
    }
    try {
        return Promise.resolve(found.callback.apply(found.thisArg, args));
    } catch (err) {
        return Promise.reject(err);
    }
}

function labelOf(item: any): string {
    if (!item) {
        return "";
    }
    if (typeof item.label === "string") {
        return item.label;
    }
    if (item.label && typeof item.label.label === "string") {
        return item.label.label;
    }
    return "";
}

function insertOf(item: any): string {
    const insert = item.insertText;
    if (typeof insert === "string") {
        return insert;
    }
    if (insert instanceof SnippetString || (insert && typeof insert.value === "string")) {
        return insert.value;
    }
    if (item.textEdit && typeof item.textEdit.newText === "string") {
        return item.textEdit.newText;
    }
    return labelOf(item);
}

function flatten(result: any): any[] {
    if (!result) {
        return [];
    }
    if (result instanceof CompletionList || Array.isArray(result.items)) {
        return result.items;
    }
    if (Array.isArray(result)) {
        return result;
    }
    if (result instanceof CompletionItem || result.label !== undefined) {
        return [result];
    }
    return [];
}

function wants(entry: ProviderRegistration, triggerKind: number, triggerCharacter: string | undefined): boolean {
    if (triggerKind !== 2) {
        return true;
    }
    if (!triggerCharacter) {
        return true;
    }
    return entry.triggers.indexOf(triggerCharacter) >= 0;
}

export async function requestCompletion(params: any): Promise<Array<{ label: string; insertText: string; detail?: string }>> {
    const doc = find(params.uri);
    if (!doc) {
        return [];
    }
    const position = new Position(params.line || 0, params.character || 0);
    const lspKind = params.triggerKind || 1;
    const triggerCharacter = params.triggerCharacter ? String(params.triggerCharacter) : undefined;
    const context = {
        triggerKind: lspKind === 2 ? CompletionTriggerKind.TriggerCharacter
            : lspKind === 3 ? CompletionTriggerKind.TriggerForIncompleteCompletions
            : CompletionTriggerKind.Invoke,
        triggerCharacter: lspKind === 2 ? triggerCharacter : undefined
    };
    const token = new CancellationTokenSource();
    const items: Array<{ label: string; insertText: string; detail?: string }> = [];
    try {
        for (const entry of completionProviders.slice()) {
            if (matchDocument(entry.selector, doc) <= 0) {
                continue;
            }
            if (!wants(entry, lspKind, triggerCharacter)) {
                continue;
            }
            const provider = entry.provider;
            if (!provider || typeof provider.provideCompletionItems !== "function") {
                continue;
            }
            const result = await provider.provideCompletionItems(doc, position, token.token, context);
            for (const item of flatten(result)) {
                const label = labelOf(item);
                if (!label) {
                    continue;
                }
                const wire: { label: string; insertText: string; detail?: string } = {
                    label,
                    insertText: insertOf(item)
                };
                if (typeof item.detail === "string") {
                    wire.detail = item.detail;
                }
                items.push(wire);
            }
        }
    } finally {
        token.dispose();
    }
    return items;
}

function textOf(contents: any): string {
    if (!contents) {
        return "";
    }
    if (typeof contents === "string") {
        return contents;
    }
    if (contents instanceof MarkdownString || typeof contents.value === "string") {
        return contents.value;
    }
    if (Array.isArray(contents)) {
        return contents.map(textOf).filter((part) => part.length > 0).join("\n");
    }
    if (contents instanceof Hover || contents.contents !== undefined) {
        return textOf(contents.contents);
    }
    return "";
}

export async function requestHover(params: any): Promise<string> {
    const doc = find(params.uri);
    if (!doc) {
        return "";
    }
    const position = new Position(params.line || 0, params.character || 0);
    const token = new CancellationTokenSource();
    try {
        for (const entry of hoverProviders.slice()) {
            if (matchDocument(entry.selector, doc) <= 0) {
                continue;
            }
            if (typeof entry.provider.provideHover !== "function") {
                continue;
            }
            const result = await entry.provider.provideHover(doc, position, token.token);
            const text = textOf(result);
            if (text) {
                return text;
            }
        }
    } finally {
        token.dispose();
    }
    return "";
}

export async function requestDefinition(params: any): Promise<Array<{ uri: string; path: string; line: number; character: number }>> {
    const doc = find(params.uri);
    if (!doc) {
        return [];
    }
    const position = new Position(params.line || 0, params.character || 0);
    const token = new CancellationTokenSource();
    const locations: Array<{ uri: string; path: string; line: number; character: number }> = [];
    try {
        for (const entry of definitionProviders.slice()) {
            if (matchDocument(entry.selector, doc) <= 0) {
                continue;
            }
            if (typeof entry.provider.provideDefinition !== "function") {
                continue;
            }
            const result = await entry.provider.provideDefinition(doc, position, token.token);
            const list = Array.isArray(result) ? result : result ? [result] : [];
            for (const loc of list) {
                if (!loc) {
                    continue;
                }
                const target = loc instanceof Location || loc.range ? loc : undefined;
                if (!target || !target.range) {
                    continue;
                }
                const uri = target.uri && typeof target.uri.toString === "function" ? target.uri.toString() : String(target.uri || "");
                const path = target.uri && target.uri.fsPath ? target.uri.fsPath : uri;
                locations.push({
                    uri,
                    path,
                    line: target.range.start.line,
                    character: target.range.start.character
                });
            }
        }
    } finally {
        token.dispose();
    }
    return locations;
}

export function unusedRange(): Range {
    return new Range(0, 0, 0, 0);
}
