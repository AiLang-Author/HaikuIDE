/* ExtensionContext and the JSON memento under the storage root.
 *
 * Copyright (c) 2026 Sean Collins, 2 Paws Machine and Engineering. MIT.
 */

import * as fs from "fs";
import * as path from "path";

import { bridge } from "./bridge";
import { EventEmitter } from "./events";
import { Uri } from "./types";

class Memento {
    private data: Record<string, unknown> = {};
    constructor(private readonly file: string) {
        try {
            this.data = JSON.parse(fs.readFileSync(file, "utf8"));
        } catch {
            this.data = {};
        }
    }
    keys(): readonly string[] {
        return Object.keys(this.data);
    }
    get<T>(key: string, def?: T): T | undefined {
        if (Object.prototype.hasOwnProperty.call(this.data, key)) {
            return this.data[key] as T;
        }
        return def;
    }
    update(key: string, value: unknown): Promise<void> {
        if (value === undefined) {
            delete this.data[key];
        } else {
            this.data[key] = value;
        }
        fs.mkdirSync(path.dirname(this.file), { recursive: true });
        fs.writeFileSync(this.file, JSON.stringify(this.data));
        return Promise.resolve();
    }
}

export function makeContext(id: string, extensionPath: string, packageJSON: unknown): any {
    const root = bridge.storageRoot || path.join(process.env.HOME || "/tmp", "config/settings/HaikuIDE/globalStorage");
    const storage = path.join(root, id);
    fs.mkdirSync(storage, { recursive: true });
    const extensionUri = Uri.file(extensionPath);
    return {
        subscriptions: [] as Array<{ dispose(): void }>,
        extensionPath,
        extensionUri,
        globalState: new Memento(path.join(storage, "state.json")),
        workspaceState: new Memento(path.join(storage, "workspace.json")),
        storageUri: Uri.file(storage),
        storagePath: storage,
        logUri: Uri.file(storage),
        extensionMode: 1,
        extension: {
            id,
            extensionUri,
            extensionPath,
            isActive: true,
            packageJSON,
            exports: undefined
        },
        asAbsolutePath(relative: string) {
            return path.resolve(extensionPath, relative);
        },
        secrets: {
            get() { return Promise.resolve(undefined); },
            store() { return Promise.resolve(); },
            delete() { return Promise.resolve(); },
            onDidChange: new EventEmitter<unknown>().event
        }
    };
}
