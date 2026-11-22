#!/usr/bin/env python3
"""Tiny LSP server for the built-in client test.
One completion, one diagnostic, one definition. No third-party imports.
"""
import json
import sys


def read_message():
    headers = {}
    while True:
        line = sys.stdin.buffer.readline()
        if line == b"":
            return None
        if line in (b"\r\n", b"\n"):
            break
        text = line.decode("utf-8", "replace")
        name, value = text.split(":", 1)
        headers[name.strip().lower()] = value.strip()
    length = int(headers.get("content-length", "0"))
    body = sys.stdin.buffer.read(length)
    if len(body) != length:
        return None
    return json.loads(body.decode("utf-8"))


def send(obj):
    data = json.dumps(obj).encode("utf-8")
    sys.stdout.buffer.write(("Content-Length: %d\r\n\r\n" % len(data)).encode("ascii"))
    sys.stdout.buffer.write(data)
    sys.stdout.buffer.flush()


def publish(uri, text):
    diags = []
    if "bad" in text or "PLEASE_EXIT" in text:
        diags.append(
            {
                "range": {
                    "start": {"line": 0, "character": 0},
                    "end": {"line": 0, "character": 3},
                },
                "severity": 1,
                "message": "test diagnostic",
            }
        )
    send(
        {
            "jsonrpc": "2.0",
            "method": "textDocument/publishDiagnostics",
            "params": {"uri": uri, "diagnostics": diags},
        }
    )


def main():
    while True:
        msg = read_message()
        if msg is None:
            return 0
        method = msg.get("method")
        msg_id = msg.get("id")
        if method == "initialize":
            send(
                {
                    "jsonrpc": "2.0",
                    "id": msg_id,
                    "result": {
                        "capabilities": {
                            "textDocumentSync": 2,
                            "completionProvider": {"triggerCharacters": [".", ">", ":"]},
                            "hoverProvider": True,
                            "definitionProvider": True,
                        }
                    },
                }
            )
        elif method == "textDocument/didOpen":
            doc = msg["params"]["textDocument"]
            publish(doc["uri"], doc["text"])
            if "PLEASE_EXIT" in doc["text"]:
                return 3
        elif method == "textDocument/completion":
            send(
                {
                    "jsonrpc": "2.0",
                    "id": msg_id,
                    "result": {
                        "items": [
                            {"label": "haikuide_word", "insertText": "haikuide_word"}
                        ]
                    },
                }
            )
        elif method == "textDocument/hover":
            send(
                {
                    "jsonrpc": "2.0",
                    "id": msg_id,
                    "result": {"contents": {"kind": "plaintext", "value": "hover text"}},
                }
            )
        elif method == "textDocument/definition":
            uri = msg["params"]["textDocument"]["uri"]
            send(
                {
                    "jsonrpc": "2.0",
                    "id": msg_id,
                    "result": {
                        "uri": uri,
                        "range": {
                            "start": {"line": 2, "character": 0},
                            "end": {"line": 2, "character": 4},
                        },
                    },
                }
            )
        elif method == "shutdown":
            send({"jsonrpc": "2.0", "id": msg_id, "result": None})
        elif method == "exit":
            return 0


if __name__ == "__main__":
    sys.exit(main())
