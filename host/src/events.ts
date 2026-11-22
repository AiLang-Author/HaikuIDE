/* Disposable and events the vscode module hands to extensions.
 *
 * Copyright (c) 2026 Sean Collins, 2 Paws Machine and Engineering. MIT.
 */

export interface Disposable {
    dispose(): void;
}

type Listener<T> = { fn: (value: T) => void; thisArg: unknown };

export class EventEmitter<T> {
    private listeners: Array<Listener<T>> = [];

    readonly event = (listener: (value: T) => void, thisArg?: unknown, disposables?: Disposable[]): Disposable => {
        const entry: Listener<T> = { fn: listener, thisArg };
        this.listeners.push(entry);
        const disposable = new DisposableImpl(() => {
            const index = this.listeners.indexOf(entry);
            if (index >= 0) {
                this.listeners.splice(index, 1);
            }
        });
        if (disposables) {
            disposables.push(disposable);
        }
        return disposable;
    };

    fire(value: T): void {
        for (const entry of this.listeners.slice()) {
            entry.fn.call(entry.thisArg, value);
        }
    }

    dispose(): void {
        this.listeners = [];
    }
}

export class DisposableImpl {
    private disposed = false;
    constructor(private readonly callOnDispose: () => void) {}

    dispose(): void {
        if (this.disposed) {
            return;
        }
        this.disposed = true;
        this.callOnDispose();
    }

    static from(...items: Array<{ dispose(): void } | undefined>): DisposableImpl {
        return new DisposableImpl(() => {
            for (const item of items) {
                if (item) {
                    item.dispose();
                }
            }
        });
    }
}

export class CancellationTokenSource {
    private cancelled = false;
    private readonly emitter = new EventEmitter<void>();
    readonly token: {
        isCancellationRequested: boolean;
        onCancellationRequested: EventEmitter<void>["event"];
    };

    constructor() {
        const self = this;
        this.token = {
            get isCancellationRequested() {
                return self.cancelled;
            },
            onCancellationRequested: this.emitter.event
        };
    }

    cancel(): void {
        if (this.cancelled) {
            return;
        }
        this.cancelled = true;
        this.emitter.fire();
    }

    dispose(): void {
        this.emitter.dispose();
    }
}

export function neverToken(): { isCancellationRequested: boolean; onCancellationRequested: EventEmitter<void>["event"] } {
    const emitter = new EventEmitter<void>();
    return {
        isCancellationRequested: false,
        onCancellationRequested: emitter.event
    };
}
