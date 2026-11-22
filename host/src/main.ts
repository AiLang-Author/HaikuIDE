/* Extension supervisor. Content-Length JSON-RPC on stdin/stdout.
 * Logs go to stderr. stdout is frames only.
 *
 * Copyright (c) 2026 Sean Collins, 2 Paws Machine and Engineering. MIT.
 */

import * as fs from "fs";
import * as path from "path";

const Module = require("module");

import { bridge } from "./bridge";
import * as docs from "./documents";
import { makeContext } from "./context";
import { executeCommand, requestCompletion, requestDefinition, requestHover } from "./services";

const VSCODE_VERSION = "1.85.0";

interface ExtensionRecord {
    id: string;
    dir: string;
    main: string;
    version: string;
    engines: string;
    activationEvents: string[];
    packageJSON: unknown;
    activated: boolean;
    failed: string;
}

const extensions: ExtensionRecord[] = [];
const failedNotes: Array<{ id: string; message: string }> = [];

let stdoutBroken = false;

function writeFrame(message: unknown): void {
    if (stdoutBroken) {
        return;
    }
    const body = Buffer.from(JSON.stringify(message), "utf8");
    const header = Buffer.from("Content-Length: " + body.length + "\r\n\r\n", "ascii");
    process.stdout.write(header);
    process.stdout.write(body);
}

function notify(method: string, params: unknown): void {
    writeFrame({ jsonrpc: "2.0", method, params });
}

function reply(id: unknown, result: unknown): void {
    writeFrame({ jsonrpc: "2.0", id, result });
}

function replyError(id: unknown, message: string): void {
    writeFrame({ jsonrpc: "2.0", id, error: { code: -32603, message } });
}

bridge.notify = notify;
bridge.log = (line: string) => {
    process.stderr.write(line + "\n");
};
bridge.storageRoot = process.env.HAIKUIDE_STORAGE
    || path.join(process.env.HOME || "/tmp", "config/settings/HaikuIDE/globalStorage");

const vscodeFile = path.join(__dirname, "vscode.js");
const resolveFilename = (Module as any)._resolveFilename as (...args: unknown[]) => string;
(Module as any)._resolveFilename = function (request: string, ...rest: unknown[]) {
    if (request === "vscode") {
        return vscodeFile;
    }
    return resolveFilename.call(this, request, ...rest);
};

function loadRecord(dir: string): void {
    const pkgPath = path.join(dir, "package.json");
    let pkg: any;
    try {
        pkg = JSON.parse(fs.readFileSync(pkgPath, "utf8"));
    } catch (err) {
        bridge.log("extension package.json unreadable " + dir + " " + String(err));
        return;
    }
    if (!pkg.main) {
        bridge.log("skip " + dir + " (no main; browser entry is not this host)");
        return;
    }
    const id = pkg.publisher ? pkg.publisher + "." + pkg.name : String(pkg.name || path.basename(dir));
    extensions.push({
        id,
        dir,
        main: path.resolve(dir, pkg.main),
        version: String(pkg.version || "0.0.0"),
        engines: pkg.engines && pkg.engines.vscode ? String(pkg.engines.vscode) : "",
        activationEvents: Array.isArray(pkg.activationEvents) ? pkg.activationEvents.map(String) : [],
        packageJSON: pkg,
        activated: false,
        failed: ""
    });
}

function discover(): void {
    const argv = process.argv.slice(2);
    for (let i = 0; i < argv.length; i++) {
        if (argv[i] === "--extension" && argv[i + 1]) {
            loadRecord(path.resolve(argv[++i]));
        }
    }
    const root = process.env.HAIKUIDE_EXTENSIONS
        || path.join(process.env.HOME || "", "config/settings/HaikuIDE/extensions");
    if (!root || !fs.existsSync(root)) {
        return;
    }
    let names: string[] = [];
    try {
        names = fs.readdirSync(root);
    } catch (err) {
        bridge.log("extensions dir " + root + " " + String(err));
        return;
    }
    for (const name of names) {
        const dir = path.join(root, name);
        if (fs.existsSync(path.join(dir, "package.json"))) {
            loadRecord(dir);
        }
    }
}

function matches(ext: ExtensionRecord, kind: string, language?: string, command?: string): boolean {
    for (const event of ext.activationEvents) {
        if (event === "*" && (kind === "startup" || kind === "language" || kind === "command")) {
            return true;
        }
        if (event === "onStartupFinished" && kind === "startup") {
            return true;
        }
        if (kind === "startup" && event.startsWith("workspaceContains:") && workspaceContains(event.slice("workspaceContains:".length))) {
            return true;
        }
        if (kind === "language" && event === "onLanguage:" + language) {
            return true;
        }
        if (kind === "command" && event === "onCommand:" + command) {
            return true;
        }
    }
    return false;
}

function workspaceContains(pattern: string): boolean {
    const root = docs.rootPath();
    if (!root || !fs.existsSync(root)) {
        return false;
    }
    const suffix = pattern.replace(/^\*\*\//, "").replace(/^\*/, "");
    const queue = [root];
    let seen = 0;
    while (queue.length > 0 && seen < 2000) {
        const dir = queue.pop() as string;
        let names: string[] = [];
        try {
            names = fs.readdirSync(dir);
        } catch {
            continue;
        }
        for (const name of names) {
            seen++;
            const full = path.join(dir, name);
            let stat: fs.Stats;
            try {
                stat = fs.statSync(full);
            } catch {
                continue;
            }
            if (stat.isDirectory()) {
                queue.push(full);
                continue;
            }
            if (suffix && (full.endsWith(suffix) || name === suffix)) {
                return true;
            }
        }
    }
    return false;
}

async function activate(ext: ExtensionRecord): Promise<void> {
    if (ext.activated) {
        return;
    }
    ext.activated = true;
    bridge.extensionId = ext.id;
    try {
        const loaded = require(ext.main);
        const context = makeContext(ext.id, ext.dir, ext.packageJSON);
        if (loaded && typeof loaded.activate === "function") {
            const result = loaded.activate(context);
            if (result && typeof result.then === "function") {
                await result;
            }
        }
        notify("host/activated", { id: ext.id });
    } catch (err: any) {
        const message = err && err.stack ? String(err.stack) : String(err);
        ext.failed = message;
        failedNotes.push({ id: ext.id, message });
        bridge.log("extension failed " + ext.id + " " + message);
        notify("host/extensionFailed", { id: ext.id, message });
    }
}

async function activateKind(kind: string, language?: string, command?: string): Promise<string[]> {
    const ids: string[] = [];
    for (const ext of extensions) {
        if (ext.activated || !matches(ext, kind, language, command)) {
            continue;
        }
        await activate(ext);
        ids.push(ext.id);
    }
    return ids;
}

async function route(method: string, params: any, id: unknown): Promise<unknown> {
    if (method === "workspace/open") {
        docs.setWorkspace(params.rootUri, params.rootPath);
        const activated = await activateKind("startup");
        return { ok: true, activated, failed: failedNotes.slice() };
    }
    if (method === "workspace/close") {
        return { ok: true };
    }
    if (method === "doc/didOpen") {
        docs.open(String(params.uri || ""), String(params.languageId || "plaintext"), Number(params.version || 1), String(params.text || ""));
        const activated = await activateKind("language", String(params.languageId || ""));
        await activateKind("startup");
        return { ok: true, activated, failed: failedNotes.slice() };
    }
    if (method === "doc/didChange") {
        docs.change(String(params.uri || ""), Number(params.version || 1), String(params.text || ""));
        return { ok: true };
    }
    if (method === "doc/didSave") {
        docs.save(String(params.uri || ""));
        return { ok: true };
    }
    if (method === "doc/didClose") {
        docs.close(String(params.uri || ""));
        return { ok: true };
    }
    if (method === "command/execute") {
        await activateKind("command", undefined, String(params.command || ""));
        const result = await executeCommand(String(params.command || ""), Array.isArray(params.args) ? params.args : []);
        return { result };
    }
    if (method === "completion/request") {
        const items = await requestCompletion(params);
        notify("completion/result", { id, items });
        return { items };
    }
    if (method === "hover/request") {
        const text = await requestHover(params);
        notify("hover/result", { id, text });
        return { text };
    }
    if (method === "definition/request") {
        const locations = await requestDefinition(params);
        notify("definition/result", { id, locations });
        return { locations };
    }
    const message = "unknown method " + method;
    bridge.log(message);
    notify("host/unimplemented", { method, extensionId: bridge.extensionId, count: 1 });
    if (id !== undefined) {
        replyError(id, message);
        return undefined;
    }
    return undefined;
}

let chain: Promise<void> = Promise.resolve();

function dispatch(body: string): void {
    chain = chain.then(async () => {
        let message: any;
        try {
            message = JSON.parse(body);
        } catch (err) {
            bridge.log("bad json " + String(err));
            return;
        }
        const method = String(message.method || "");
        const id = message.id;
        try {
            const result = await route(method, message.params || {}, id);
            if (id !== undefined && result !== undefined) {
                reply(id, result);
            }
        } catch (err: any) {
            const text = err && err.stack ? String(err.stack) : String(err);
            bridge.log("handler " + method + " " + text);
            if (id !== undefined) {
                replyError(id, text);
            }
        }
    });
}

let buffer = Buffer.alloc(0);

function takeFrames(): void {
    while (true) {
        const headerEnd = buffer.indexOf("\r\n\r\n");
        if (headerEnd < 0) {
            return;
        }
        const header = buffer.slice(0, headerEnd).toString("ascii");
        const match = /content-length:\s*(\d+)/i.exec(header);
        if (!match) {
            bridge.log("frame missing Content-Length");
            buffer = buffer.slice(headerEnd + 4);
            continue;
        }
        const length = Number(match[1]);
        const start = headerEnd + 4;
        if (buffer.length < start + length) {
            return;
        }
        const body = buffer.slice(start, start + length).toString("utf8");
        buffer = buffer.slice(start + length);
        dispatch(body);
    }
}

process.stdin.on("data", (chunk: Buffer) => {
    buffer = Buffer.concat([buffer, chunk]);
    takeFrames();
});

process.stdin.on("end", () => {
    process.exit(0);
});

process.stdout.on("error", (err: NodeJS.ErrnoException) => {
    if (err.code === "EPIPE") {
        stdoutBroken = true;
        process.exit(0);
    }
    bridge.log("stdout " + String(err));
});

process.on("uncaughtException", (err: Error) => {
    bridge.log("uncaught " + (err && err.stack ? err.stack : String(err)));
});

process.on("unhandledRejection", (err: unknown) => {
    const error = err as Error;
    bridge.log("rejection " + (error && error.stack ? error.stack : String(err)));
});

discover();
notify("host/hello", {
    vscodeVersion: VSCODE_VERSION,
    extensions: extensions.map((ext) => ({
        id: ext.id,
        version: ext.version,
        engines: ext.engines
    }))
});
