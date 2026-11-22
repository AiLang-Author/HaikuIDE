/* Shared hooks the vscode module calls. main.ts fills these in before
 * any extension is loaded.
 *
 * Copyright (c) 2026 Sean Collins, 2 Paws Machine and Engineering. MIT.
 */

export interface Notify {
    (method: string, params: unknown): void;
}

export interface Bridge {
    notify: Notify;
    log: (line: string) => void;
    storageRoot: string;
    extensionId: string;
}

export const bridge: Bridge = {
    notify: () => {},
    log: (line) => {
        process.stderr.write(line + "\n");
    },
    storageRoot: "",
    extensionId: ""
};
