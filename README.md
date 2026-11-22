# HaikuIDE

Native Haiku code editor. The window is a `BApplication` on libbe. Extensions will run on Haiku's Node.js. Qt is not part of this program.

The document model in `editor/` has no Haiku headers. On Linux:

```
make test
```

On the Haiku guest, `make app` installs the libbe window at `$HOME/config/non-packaged/apps/HaikuIDE`. The approved plan, phase screenshots, and the vscode method list are in `docs/`.
