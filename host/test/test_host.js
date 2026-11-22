/* Fake shell for the phase 4 host gates.
 *
 * Copyright (c) 2026 Sean Collins, 2 Paws Machine and Engineering. MIT.
 */
const { spawn } = require("child_process");
const fs = require("fs");
const os = require("os");
const path = require("path");

const root = path.resolve(__dirname, "../..");
const hostMain = path.join(__dirname, "../dist/main.js");

function frame(obj) {
    const body = Buffer.from(JSON.stringify(obj), "utf8");
    return Buffer.concat([
        Buffer.from("Content-Length: " + body.length + "\r\n\r\n", "ascii"),
        body
    ]);
}

function start(extensionDir, extraEnv) {
    const storage = fs.mkdtempSync(path.join(os.tmpdir(), "haikuide-host-"));
    const extRoot = fs.mkdtempSync(path.join(os.tmpdir(), "haikuide-ext-"));
    const stderr = [];
    const child = spawn(process.execPath, [hostMain, "--extension", extensionDir], {
        cwd: root,
        env: Object.assign({}, process.env, {
            HAIKUIDE_STORAGE: storage,
            HAIKUIDE_EXTENSIONS: extRoot
        }, extraEnv || {}),
        stdio: ["pipe", "pipe", "pipe"]
    });
    const notes = [];
    const waiters = new Map();
    let buffer = Buffer.alloc(0);
    let nextId = 1;

    function take() {
        while (true) {
            const headerEnd = buffer.indexOf("\r\n\r\n");
            if (headerEnd < 0) {
                return;
            }
            const header = buffer.slice(0, headerEnd).toString("ascii");
            const match = /content-length:\s*(\d+)/i.exec(header);
            if (!match) {
                buffer = buffer.slice(headerEnd + 4);
                continue;
            }
            const length = Number(match[1]);
            const startAt = headerEnd + 4;
            if (buffer.length < startAt + length) {
                return;
            }
            const body = buffer.slice(startAt, startAt + length).toString("utf8");
            buffer = buffer.slice(startAt + length);
            let message;
            try {
                message = JSON.parse(body);
            } catch (err) {
                stderr.push("bad json " + err);
                continue;
            }
            if (message.method) {
                notes.push(message);
            }
            if (message.id !== undefined && waiters.has(message.id)) {
                const waiter = waiters.get(message.id);
                waiters.delete(message.id);
                clearTimeout(waiter.timer);
                if (message.error) {
                    waiter.reject(new Error(message.error.message || JSON.stringify(message.error)));
                } else {
                    waiter.resolve(message.result);
                }
            }
        }
    }

    child.stdout.on("data", (chunk) => {
        buffer = Buffer.concat([buffer, chunk]);
        take();
    });
    child.stderr.on("data", (chunk) => {
        stderr.push(chunk.toString("utf8"));
    });

    function rpc(method, params, timeoutMs) {
        const id = nextId++;
        return new Promise((resolve, reject) => {
            const timer = setTimeout(() => {
                waiters.delete(id);
                reject(new Error("timeout " + method + "\n" + stderr.join("")));
            }, timeoutMs);
            waiters.set(id, { resolve, reject, timer });
            child.stdin.write(frame({ jsonrpc: "2.0", id, method, params }));
        });
    }

    function stop() {
        child.kill("SIGTERM");
    }

    return { child, notes, rpc, stderr, stop };
}

function fail(name, err, session) {
    process.stderr.write(name + " FAILED\n" + String(err && err.stack || err) + "\n");
    if (session) {
        process.stderr.write("--- host stderr ---\n" + session.stderr.join("") + "\n");
        session.stop();
    }
    process.exit(1);
}

async function gateWord() {
    const session = start(path.join(__dirname, "ext-word"));
    try {
        await session.rpc("workspace/open", { rootUri: "file:///tmp", rootPath: "/tmp" }, 5000);
        const opened = await session.rpc("doc/didOpen", {
            uri: "file:///tmp/sample.cpp",
            languageId: "cpp",
            version: 1,
            text: "int x;\n"
        }, 5000);
        if (opened.failed && opened.failed.length) {
            throw new Error("extension failed " + JSON.stringify(opened.failed));
        }
        const result = await session.rpc("completion/request", {
            uri: "file:///tmp/sample.cpp",
            line: 0,
            character: 5,
            triggerKind: 1
        }, 5000);
        const labels = (result.items || []).map((item) => item.label);
        if (labels.indexOf("haikuide_ext") < 0) {
            throw new Error("missing haikuide_ext in " + JSON.stringify(labels));
        }
        session.stop();
        process.stdout.write("gate word: haikuide_ext\n");
    } catch (err) {
        fail("gate word", err, session);
    }
}

async function gateLanguageClient() {
    const server = path.join(root, "lsp/testserver/server.py");
    const session = start(path.join(__dirname, "ext-lsp"), { HAIKUIDE_TEST_SERVER: server });
    try {
        await session.rpc("workspace/open", { rootUri: "file:///tmp", rootPath: "/tmp" }, 15000);
        const opened = await session.rpc("doc/didOpen", {
            uri: "file:///tmp/note.txt",
            languageId: "plaintext",
            version: 1,
            text: "bad start\n"
        }, 15000);
        if (opened.failed && opened.failed.length) {
            throw new Error("extension failed " + JSON.stringify(opened.failed));
        }
        const result = await session.rpc("completion/request", {
            uri: "file:///tmp/note.txt",
            line: 0,
            character: 3,
            triggerKind: 1
        }, 15000);
        const labels = (result.items || []).map((item) => item.label);
        if (labels.indexOf("haikuide_word") < 0) {
            throw new Error("missing haikuide_word in " + JSON.stringify(labels));
        }
        const deadline = Date.now() + 5000;
        let diagnostic = null;
        while (Date.now() < deadline) {
            diagnostic = session.notes.find((note) => {
                return note.method === "diagnostics/set"
                    && JSON.stringify(note.params).indexOf("test diagnostic") >= 0;
            });
            if (diagnostic) {
                break;
            }
            await new Promise((resolve) => setTimeout(resolve, 50));
        }
        if (!diagnostic) {
            throw new Error("missing test diagnostic in " + JSON.stringify(session.notes.map((note) => note.method)));
        }
        session.stop();
        process.stdout.write("gate languageclient: haikuide_word, test diagnostic\n");
    } catch (err) {
        fail("gate languageclient", err, session);
    }
}

async function main() {
    await gateWord();
    await gateLanguageClient();
}

main().catch((err) => fail("host", err, null));
