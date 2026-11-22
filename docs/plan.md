# Native Haiku IDE with a VS Code extension host

Status: approved. D1 is locked (libbe, native Haiku window, no Qt). Phase 1 document tests pass (`make test`, 10 MB line index 26 ms on Linux). Phase 2 gate passed on the KVM guest: 5,000-line file, type, undo, save, Page Down. Dirty flag cleared on save. Screenshots: `docs/phase2-top.png`, `docs/phase2-scrolled.png`. The plan's TCG clause was not re-run; this boot is KVM. Phase 3 gate passed on the same KVM guest with clangd 23.1.4: diagnostic, go to definition, completion, and server exit. Screenshots: `docs/phase3-diagnostics.png`, `docs/phase3-definition.png`, `docs/phase3-completion.png`, `docs/phase3-server-exit.png`. Phase 4 gate passed on that guest: the unpacked `haikuide.word` extension and clangd both completed on `p.` in one session. Screenshot: `docs/phase4-completion.png`. The method list is `docs/vscode-api.md`.

Working title: HaikuIDE. The name is disposable.

This is a new C++ application. It does not go in `Ailang-Self-Hosting-`, `HalCode-Haiku`, or the Linux bridge. AILANG shows up only after the editor and the extension host are real. HalCode's Haiku window blits a desk framebuffer and is the wrong shell for a text editor. The AILANG display-system notes (`TextBuffer`, BSP, framebuffer) are a different project.

## 1. Goal

A native Haiku program, linked with libbe, that is a code editor, and that loads unpacked VS Code extensions by running them on Haiku's Node.js.

The first extensions that must work are the ones that carry a language: a `package.json` with languages, grammars, snippets, and commands, plus a JavaScript entry that speaks Language Server Protocol through `vscode-languageclient`. clangd, rust-analyzer, and pyright are the proof, each behind whatever extension (or built-in server entry) launches it.

Electron stays off the machine. Chromium stays off the machine. WebKit is allowed later, and only as a panel for extensions that open a webview.

## 2. What already exists on Haiku

Checked against HaikuDepot, October 2026, x86_64:

| Piece | Package | Use |
|---|---|---|
| Extension runtime | `nodejs24` 24.21.0-1 | Run extension JavaScript. Same major line as VS Code 1.147's extension host (Node 24.22). |
| Older runtimes | `nodejs22` 22.23.3-1, `nodejs20` | Fallback only. |
| Small JS engines | QuickJS, QuickJS-NG, SpiderMonkey, JavaScriptCore inside WebKit | Cannot host an extension. Extensions call `require("fs")`, `require("vscode")`, `child_process`. |
| JVM | `openjdk25` | Unrelated. Extensions are not Java. |
| UI | libbe, app_server | The IDE window. |
| HTML later | Haiku WebKit (WebPositive / haikuwebkit, now on Codeberg) | Webview panels only. |

`pkgman install nodejs24` is the runtime. People on Haiku already npm-install large Node programs on `nodejs20` (March 2026). The 2019 Node 12 port is not the port we are targeting.

## 3. Decisions

These are the choices the rest of the plan stands on. Push back on any of them before implementation starts.

**D1. Locked at review: native Haiku window, libbe only. No Qt.**

The shell is a `BApplication` with `BWindow` and `BView`, linked with `-lbe`. Qt, Qt Creator's editor stack, and any other toolkit are out. Sources live in `/home/bob/HaikuIDE`. The editor buffer is plain C++ with no Haiku headers, so it tests on Linux. The window is `#ifdef __HAIKU__` and builds on the guest with `g++ -lbe`, the same way `halcode_shell_haiku.cxx` builds. Install prefix is `$HOME/config/non-packaged`. Haiku's rootfs rejects regular files under `/usr`.

**D2. We implement the public `vscode` module. We do not run Code-OSS's extension host.**

An extension does `require("vscode")` and exports `activate(context)`. That module is the contract (`vscode.d.ts`, semver). Code-OSS's real extension host (`src/vs/workbench/api/node/extensionHostProcess.ts` plus `extHost.protocol.ts`, about 3,600 lines of internal RPC) talks to the workbench over a private protocol that moves every month. Implementing that private protocol means tracking Microsoft's main-thread customers forever, which is the trap Onivim 2 fell into.

coc.nvim showed the other road: a Node process, a `vscode` shim, and a growing subset of the public API. We do that, and we read Code-OSS `extHost*.ts` as documentation of semantics. We copy a type or a function only with the MIT header still on it. The host we ship is our code.

Unimplemented methods log once and return an empty value or a rejected promise that names the method. They must not tear down the host.

**D3. A built-in LSP client lands before the extension host.**

The editor has to be useful while the shim is small. A C++ Language Server Protocol client speaks stdio to clangd (or a tiny test server) with no extension loaded. When the host arrives, extension-owned language clients paint through the same completion, diagnostic, and navigation UI. One UI, two sources.

**D4. Extensions install as directories, then as VSIX files.**

A VSIX is a zip of the same layout as `~/.vscode/extensions/<publisher.extension-version>/`. v1 has no Gallery client and no account. The user points at a folder or a `.vsix`. Gallery download is a later, small, feature once the host can run what it unpacks.

**D5. Node owns extension processes. The window owns the screen and the documents.**

`ide` is the BApplication. It spawns `node host/main.js` with a pipe. The host `require`s extensions and spawns language servers. The C++ process is the parent of Node and is not the parent of those servers. Closing the window kills the host, and the host kills its children.

**D6. AILANG is phase 11.**

No `ailang_lsp.ailang` work, no `--haiku` compiler work, and no HalCode tool is on the path to the first extension loading.

**D7. API pin.**

The shim reports `vscode.version` as the VS Code API version we actually implement, starting at `1.85.0` and moving only when a method we added exists in that release. Extensions whose `engines.vscode` is newer still load. A startup log lists every method they called that we do not implement.

## 4. Architecture

```
HaikuIDE (C++, libbe)                         node host (nodejs24)
  BWindow, menus, tabs, palette                 loads package.json
  editor view, visible lines only               require("vscode") shim
  document model + undo                         activate(context)
  diagnostics, completions, hover               spawns language servers
  command registry                              grammars, snippets, themes
        ^                                                |
        +-------- JSON-RPC, Content-Length -------------+
                         our protocol, frozen per host version
```

Three teams, on purpose:

1. The BApplication thread. Input, paint, menus. It never waits on Node.
2. A reader thread (or a `BMessage` port fed by one) that parses host messages and posts them onto the window.
3. The Node team. Extensions, file IO that extensions ask for, language-server children.

Documents live in the C++ process. The host receives `doc/didOpen`, `doc/didChange`, `doc/didSave`, `doc/didClose` with a version number. The host asks for text when a provider needs it. Extension-driven edits come back as `doc/applyEdit` and run through the same undo stack as keystrokes.

### Our protocol

JSON-RPC 2.0 with `Content-Length` headers, the same framing as LSP, on the host's stdin/stdout. A second pipe is reserved for host logs so trace lines never corrupt the frame.

Shell to host:

- `workspace/open`, `workspace/close`
- `doc/didOpen`, `doc/didChange`, `doc/didSave`, `doc/didClose`
- `doc/getText` response
- `command/execute`
- `completion/request`, `hover/request`, `definition/request`
- `ui/quickPickResult`, `ui/inputResult`, `ui/messageResult`

Host to shell:

- `command/register`, `command/unregister`
- `menu/register`
- `status/set`
- `diagnostics/set`
- `completion/result`, `hover/result`, `definition/result`
- `doc/applyEdit`
- `ui/message`, `ui/quickPick`, `ui/input`, `ui/output`
- `output/append`
- `tree/setChildren` (phase 7)
- `webview/create`, `webview/setHtml`, `webview/postMessage` (phase 10)
- `host/unimplemented` (method name, extension id, count)

The schema lives in `protocol/schema.json` and is the only shared contract. The C++ side and the TypeScript side both reject unknown methods with a logged error, and both stay up.

### Editor model

A piece table (or a rope; piece table preferred because edits are splices of original and added buffers). On top of it, a line index: start offset of each line, rebuilt for the dirty span. Positions are UTF-16 for the wire, because LSP and the VS Code API both use UTF-16 offsets, and UTF-8 in memory. v1 files are UTF-8, LF. A CRLF file is normalized on open and the save dialog can preserve the original ending if we recorded it.

The view asks the line index for the range that intersects the clip and draws those lines with `BFont::StringWidth` and `DrawString`. No full-document layout. `BTextView` lays out the whole text and is the wrong widget; Pe and StyledEdit can keep it.

Paint is dirty rectangles through app_server. The HalCode desk's full-frame blit is not the model. The QEMU guest is often on TCG, so a 1280x800 RGBA upload every keystroke will feel bad even when the buffer is fast.

### Extension host

`host/` is a Node program, TypeScript compiled to plain CommonJS so Haiku's Node can run it with no extra loader.

- `ExtensionLoader` reads `package.json`, checks `main`, `browser` (skipped; that entry is for the web host), `activationEvents`, and `contributes`.
- `Activation` runs `activate` on `onLanguage`, `onCommand`, `workspaceContains`, and `*` . `onStartupFinished` runs after the shell sends `workspace/open`.
- `vscode` is a module we put on `Module._nodeModulePaths` / a custom `require` hook so the extension's `require("vscode")` hits us and never hits npm.
- `ExtensionContext` has `subscriptions`, `extensionPath`, `extensionUri`, `globalState`, `workspaceState`, `storageUri`. State is a JSON file under `$HOME/config/settings/HaikuIDE/globalStorage/<id>/`.
- Language servers stay inside the extension. We implement the `vscode.languages` and `vscode.workspace` surface that `vscode-languageclient` calls. We do not reimplement the LSP state machine in the host.

`contributes` that are data, and that we honor before any JS runs:

- `languages` (id, extensions, aliases, filenames, configuration)
- `grammars` (TextMate, phase 5)
- `snippets`
- `commands`, `menus`, `keybindings`
- `configuration` (JSON schema merged into settings)
- `themes` (token colors first; workbench chrome colors only for the keys we have)
- `debuggers` (phase 8, stored until DAP exists)

### Native addons

If an extension's entry, or a dependency it loads, is a `.node` file, the host catches the load error, marks that extension failed, and reports the path in the Extensions panel. Linux and macOS prebuilds will not `dlopen` on Haiku. The extension's JavaScript neighbors still load. We do not ship a fake `node-pty`; the terminal, when it exists, is a pty the C++ side opens.

### File watching

Phase 0 measures `fs.watch` on `nodejs24`. The 2019 port had no filesystem events. If `fs.watch` is still a no-op, the shell watches with Haiku's node monitor and forwards changes to the host as `workspace/didChangeFiles`. Extensions then see `vscode.workspace.createFileSystemWatcher` backed by the shell, which is the reliable path anyway.

## 5. Repository layout

```
HaikuIDE/
  editor/        piece table, line index, undo, UTF-16 map. No libbe.
  editor/test/   C++ tests, run on Linux
  ui/            BApplication, editor view, tabs, palette, problems list
  lsp/           built-in stdio LSP client
  protocol/      schema.json, C++ and TS codecs
  host/          TypeScript extension supervisor and the vscode shim
  host/test/     fake shell on a pipe, run with node on Linux
  grammars/      Oniguruma wrapper, phase 5
  docs/          this plan, plus a short compatibility list
  Makefile       Linux: editor tests + host. Haiku: ui + lsp + link -lbe
```

Settings and extensions, on the guest:

```
/boot/home/config/settings/HaikuIDE/settings.json
/boot/home/config/settings/HaikuIDE/keybindings.json
/boot/home/config/settings/HaikuIDE/extensions/<id>/
/boot/home/config/non-packaged/apps/HaikuIDE
```

## 6. Phases

Each phase has one gate. The next phase starts when the gate is measured, on the guest for anything with a window, on Linux for the buffer and the host. No phase is "done" because it compiles.

### Phase 0. Node and pipe spike

On the Haiku guest, no IDE code:

- `pkgman install nodejs24` and record `process.version`, `process.arch`, `process.platform`.
- Spawn a child with pipes. Parent writes a `Content-Length` JSON message, child replies.
- `fs.watch` a file, touch it, record whether a callback runs.
- `require` a one-file extension that exports `activate`.
- `require` a tiny native addon and record the error.

Gate: a one-page note in `docs/node-spike.md` with those four results. `fs.watch` either works or it is recorded as broken, and D-filewatch (shell-side watch) becomes mandatory.

### Phase 1. Document model

Linux-testable. Piece table, undo/redo groups, line index, UTF-8 storage, UTF-16 position conversion, a version integer that increments on every committed edit.

Gate: tests cover insert, delete, undo across a piece boundary, a 10 MB file's line index built in under 50 ms on the host, and UTF-16 offsets for a string that contains a character outside the BMP. No Haiku link.

### Phase 2. Window

`BApplication`, one `BWindow`, menu bar (File, Edit, View), tabs, open, save, save as. Custom `BView` inside a `BScrollView`. Monospace from a packaged font (Noto Sans Mono or DejaVu Sans Mono). Caret, shift-selection, page up/down, Ctrl-Z / Ctrl-Shift-Z, Ctrl-S. Word wrap off. Line numbers on.

Gate: on the guest, open a 5,000-line file, type, undo, save, and scroll. Scrolling and typing stay responsive under TCG. A screenshot of the window with the file open. Dirty flag clears on save. The buffer tests from phase 1 still pass.

Measured 2026-11-21 on the KVM guest (`-enable-kvm`, not TCG). Font Noto Sans Mono Regular, 8 px cell, 19 px line. Opened `/boot/home/haikuide-5000.txt` (5,000 lines). Typed `x` (title gained `*`), Ctrl-Z cleared it, typed `x` again, Ctrl-S cleared the star and the file starts with `x`. Six Page Down paints lines 160–193. Screenshots `docs/phase2-top.png` and `docs/phase2-scrolled.png`. Linux `make test` still passes (26 ms).

### Phase 3. Built-in LSP

C++ client. Config key `haikuide.languageServers` is a list of `{ language, command, args }`. Frame stdio. Send `initialize`, `initialized`, `textDocument/didOpen`, `didChange` (incremental, falling back to full), `didSave`, `didClose`. Draw diagnostics in a gutter and a problems list. Completion popup on a trigger character and on Ctrl-Space. Hover as a panel. Go to definition jumps in the same window or opens the file.

Gate: one real server. Prefer `clangd` if `pkgman` has it; otherwise a 50-line Python server committed under `lsp/testserver` that completes one word and publishes one diagnostic. Completion, diagnostic, and definition each demonstrated once on the guest. The server dying mid-session leaves the editor up and shows a status error.

Measured 2026-11-21 on the KVM guest. `clangd` 23.1.4 comes from `llvm23_clang` (there is no separate clangd package). Settings key `haikuide.languageServers` was written on first launch. Opened `lsp/testdata/sample.cpp`. The problems list showed three errors, including `Use of undeclared identifier 'missing'`, with a gutter mark and a squiggle. Status read `3 errors`. F12 on `Point` in `int use(Point p)` moved the caret to `struct Point` and the status read `Definition line 3`. Ctrl-Space after the dot on `p.` offered `x` and `y`. SIGTERM to that clangd team left the window up; the status read `clangd exited (1)`. Screenshots: `docs/phase3-diagnostics.png`, `docs/phase3-definition.png`, `docs/phase3-completion.png`, `docs/phase3-server-exit.png`. Linux `make test` still passes. Hover is drawn, and was not part of this pass. Phase 0 is still unmeasured.

### Phase 4. Extension supervisor, language slice

The Node host and the `vscode` methods a language client needs:

- types: `Uri`, `Position`, `Range`, `Selection`, `Diagnostic`, `CompletionItem`, `MarkdownString`, `EventEmitter`, `Disposable`, `CancellationTokenSource`
- `commands.registerCommand`, `commands.executeCommand`
- `workspace.textDocuments`, `onDidOpenTextDocument`, `onDidChangeTextDocument`, `onDidSaveTextDocument`, `onDidCloseTextDocument`, `openTextDocument`, `getConfiguration`, `workspaceFolders`, `fs` (readFile, writeFile, stat)
- `languages.registerCompletionItemProvider`, `registerHoverProvider`, `registerDefinitionProvider`, `registerDocumentFormattingEditProvider`, `registerCodeActionsProvider`, `registerRenameProvider`, `createDiagnosticCollection`, `setLanguageConfiguration`, `getDiagnostics`
- `window.activeTextEditor`, `showInformationMessage`, `showWarningMessage`, `showErrorMessage`, `createOutputChannel`
- `ExtensionContext` as in section 4
- `env.appName`, `env.language`, `env.clipboard` can wait

The shell's phase 3 UI becomes the renderer for both the built-in client and these providers. Built-in servers stay for languages with no extension.

Gate, in order:

1. On Linux, a fake shell plus a 30-line extension that registers a completion item. The test sends `completion/request` and gets the item back.
2. On Linux, `vscode-languageclient` against the phase 3 Python server, driven by the fake shell.
3. On the guest, the same extension loaded by the real window. One completion from the extension, one completion from the built-in client, in the same session.

Measured 2026-11-21 on Linux and on the KVM guest. `make test` runs both Linux gates. The 30-line extension `haikuide.word` answers `completion/request` with `haikuide_ext`. `vscode-languageclient` 9.0.1, alone against `lsp/testserver/server.py`, answers with `haikuide_word`, and a document containing `bad` publishes `test diagnostic`. The shim reports `vscode.version` `1.85.0`, which satisfies that client's `^1.82.0`.

On the guest, `nodejs24` is v24.21.0. The extension directory `/boot/home/config/settings/HaikuIDE/extensions/haikuide.word/` was loaded by the scanner, with no `--extension` flag. Opened `lsp/testdata/sample.cpp`. Status was `3 errors` before the key sequence, so clangd was still the diagnostic source. Ctrl-Home, ten Down, End, Ctrl-Space landed on the `p.` line. The popup showed `x` (selected), `y`, and `haikuide_ext`. Status read ` x,  y, haikuide_ext`: the clangd labels include a leading space, so the join has a leading space and a double space after the first comma. The problems list still showed the three clangd errors, including `Use of undeclared identifier 'missing'`. Screenshot `docs/phase4-completion.png`. The window is the parent of `node host/dist/main.js`. clangd stays a child of the window. Node dying mid-session is implemented and was not part of this pass. The shim can answer hover and definition, and the window does not ask it for those yet. Phase 0 is still unmeasured.

### Phase 5. Grammars, snippets, themes

Parse `contributes.grammars`, `contributes.snippets`, `contributes.themes` without activating JavaScript. Embed Oniguruma (HaikuPorts) and run TextMate rules for the visible lines only, on a background pass that posts token spans to the view. Snippets expand on tab from a registered prefix. Theme token colors (`keyword`, `string`, `comment`, and the standard TextMate scopes) recolor those spans.

Tree-sitter is a later swap for languages that have a grammar we care to write. v1 highlighting is TextMate because that is the file extensions already ship.

Gate: install one real grammar-only extension from a local VSIX (a small one, for example a VS Code language grammar package unpacked by hand). Open a file of that type. Keywords and strings are colored. A snippet from that extension expands. No JS from that extension is required for this gate.

### Phase 6. Commands, palette, keys, status bar

Command palette over the registry (built-in commands plus `command/register`). `contributes.keybindings` and `contributes.menus` map onto Haiku menus and a key table. `window.createStatusBarItem`. `window.showQuickPick` and `showInputBox` as real modal UI, round-tripped through the protocol.

Gate: an extension command is visible in the palette, has a keybinding from its `package.json`, runs, and appends to an output channel. `showQuickPick` returns the chosen row to the extension.

### Phase 7. Trees and the outline

`window.createTreeView` / `registerTreeDataProvider`: the host asks for children when the shell expands a row. The outline view is fed by `textDocument/documentSymbol` from whichever client (built-in or extension) owns the language. Problems, output, and outline are `BTabView` panels under the editor.

Gate: one extension tree with two levels expands and refreshes when the extension calls `treeDataProvider.onDidChangeTreeData`. The outline for the phase 3 server shows at least one symbol.

### Phase 8. Debug

A C++ DAP client, same framing as LSP, launched from `contributes.debuggers` (type, program, args, configuration attributes we pass through). Breakpoints, continue, next, stack, and variables. The first adapter is whatever HaikuPorts has that speaks DAP (`lldb-dap` if present, otherwise a stub adapter in-tree that implements `initialize`, `launch`, `threads`, `stackTrace`, `scopes`, `variables`, `disconnect`).

Gate: a breakpoint in a C file, or in the stub's fake program, stops and shows one stack frame and one variable.

### Phase 9. Terminal

The C++ side opens a pty, runs `/bin/sh` or the user's shell, and draws a scrollback `BView`. `window.createTerminal` and `Terminal.sendText` map onto it. `node-pty` is not a dependency.

Gate: a shell prompt in a panel, and an extension that calls `createTerminal` and `sendText` runs `echo haikuide-ok` with that string visible.

### Phase 10. Webviews

Embed Haiku WebKit in a `BView` for one panel class. `window.createWebviewPanel`, `webview.html`, `postMessage` both ways, `asWebviewUri` restricted to the extension directory. No Node inside the webview. JavaScriptCore in WebKit and V8 in the host stay separate heaps.

Gate: a 20-line extension sets `webview.html` to a button, the click `postMessage`s, and the extension writes the payload to the output channel.

### Phase 11. AILANG

Only after phase 4's guest gate. Either a built-in server entry pointing at a future stdio language server, or an extension that uses the same `vscode.languages` surface. The current `ailang_lsp.ailang` is a one-shot JSON dump (`./ailang_lsp.x <file>`), so it has to grow `initialize` / `textDocument/*` before either path works. That work is its own plan.

## 7. Compatibility tiers

We publish `docs/vscode-api.md` and update it at the end of every phase. An extension is "supported" only when every method it called in a run is checked off.

| Tier | When | What loads |
|---|---|---|
| 0 | phase 3 | Built-in language servers. No marketplace code. |
| 1 | phase 4 | Extensions that are a language client plus commands and messages. |
| 2 | phase 5–6 | Grammar, snippet, theme, keybinding, and palette extensions. Many language packs are done here even when their JS never runs. |
| 3 | phase 7–9 | Tree views, outline, DAP, integrated terminal. |
| 4 | phase 10 | One webview panel. |
| never in this plan | | Notebooks, custom editors, the test explorer, authentication providers, timeline, comments, chat, remote-SSH, the Gallery, extensions that ship only a `.node` binary. |

`engines.vscode` is recorded, not enforced, apart from the log line in D7.

## 8. Build and where it runs

- Linux, every change: `editor/test` and `host/test`. These do not need the guest.
- Guest, at each UI gate: rsync the tree to `/boot/home/HaikuIDE`, `g++` on Haiku, run the app, screenshot. SSH is `user@127.0.0.1` port 2222, key `/home/bob/.ssh/id_ed25519`, `KexAlgorithms=curve25519-sha256`. The guest is the QEMU disk in `/home/bob/Haiku-Linux-bridge`. One QEMU only.
- The IDE is a native Haiku ELF. It is never started under `sys_compat_run`. Tests do not kill bridged processes. HalCode is not launched as part of this work.
- Node on the guest is `nodejs24` from HaikuDepot. The host's `package.json` engines field is `>=22`, and the gate runs are on 24.

## 9. Risks

| Risk | What we will do |
|---|---|
| `fs.watch` is still dead on Haiku Node | Phase 0 measures it. The shell's node monitor is the backstop. |
| `vscode-languageclient` calls a method we skipped | The unimplemented log names it. We add the method or we document the extension as unsupported. We do not vendor a fork of the language client. |
| A popular extension is half native code | It fails that extension only. The host keeps running. |
| Code-OSS public API drift | We pin the reported version and grow `docs/vscode-api.md` by method, not by "we track upstream." |
| TCG paint is slow | Visible lines only, dirty rects, no per-key full framebuffer. |
| WebKit embedding blows the schedule | Phase 10 is after a usable editor. Language extensions do not need it. |
| Piece-table bugs | Phase 1 tests are the contract. UI work does not start on a buffer that fails them. |
| Guest time lost to KDL from unrelated bridge work | This plan does not load `sys_compat` and does not start HalCode. |

## 10. What phase 2 plus phase 4 feels like

After phase 2 you have a native editor that opens, edits, and saves. After phase 3 a language server paints diagnostics without any extension. After phase 4 a real VS Code language extension, unpacked into the extensions directory, completes through Node into that window. That is the first honest demo of "native IDE, VS Code plugins, no Electron." Phases 5 through 10 widen the set of extensions. They are not required to prove the architecture.

## 11. Review notes

The choice most worth disagreeing with is D2. The alternative is to build Code-OSS's `extensionHostProcess.js`, run it on `nodejs24`, and implement the private `MainThread*` RPC in C++ or in a second Node process. That loads more extensions on day one and binds us to `extHost.protocol.ts`. D2 loads fewer extensions at first and keeps a protocol we own.

D1 is locked. The window is a native Haiku `BApplication` / `BWindow` / `BView` on libbe. Qt is out.

The third is scope of v1. This plan's first demo is phase 4, not phase 10. Webviews, DAP, and the terminal are sequenced behind a working language extension.
