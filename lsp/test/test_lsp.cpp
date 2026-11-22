/* Linux tests for the JSON codec, framing, positions, and the stdio client.
 *
 * Copyright (c) 2026 Sean Collins, 2 Paws Machine and Engineering. MIT.
 */
#include "client.hpp"
#include "json.hpp"
#include "protocol.hpp"
#include "rpc.hpp"

#include <chrono>
#include <iostream>
#include <string>
#include <thread>

namespace {

int g_failed = 0;

void Expect(bool ok, const char* what) {
    if (ok) {
        std::cout << "ok  " << what << "\n";
        return;
    }
    std::cout << "FAIL  " << what << "\n";
    ++g_failed;
}

class Waiter : public haikuide::LspClient::Handler {
public:
    void OnDiagnostics(std::string, std::vector<haikuide::Diagnostic> diags) override {
        diagnostic = !diags.empty() ? diags[0].message : std::string();
        got_diag = true;
    }
    void OnCompletion(std::vector<haikuide::CompletionItem> items) override {
        completion = !items.empty() ? items[0].label : std::string();
        got_completion = true;
    }
    void OnHover(std::string text) override {
        hover = std::move(text);
        got_hover = true;
    }
    void OnDefinition(std::vector<haikuide::Location> locations) override {
        if (!locations.empty()) {
            def_line = locations[0].line;
        }
        got_def = true;
    }
    void OnNote(std::string text) override { note = std::move(text); }
    void OnServerExit(std::string message) override {
        exit_message = std::move(message);
        exited = true;
    }

    bool Wait(bool* flag) {
        for (int i = 0; i < 60; ++i) {
            if (*flag) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        return false;
    }

    std::string diagnostic;
    std::string completion;
    std::string hover;
    std::string note;
    std::string exit_message;
    int def_line = -1;
    bool got_diag = false;
    bool got_completion = false;
    bool got_hover = false;
    bool got_def = false;
    bool exited = false;
};

void TestJson() {
    haikuide::Value value;
    std::string err;
    const char* raw = "{\"a\":1,\"b\":\"x\\ny\",\"c\":[true,null]}";
    Expect(haikuide::Parse(raw, &value, &err), "parse object");
    const haikuide::Value* a = value.find("a");
    Expect(a != nullptr && a->is_int && a->i == 1, "integer field");
    const haikuide::Value* b = value.find("b");
    Expect(b != nullptr && b->s == "x\ny", "escaped string");
    const std::string again = haikuide::Stringify(value);
    haikuide::Value round;
    Expect(haikuide::Parse(again, &round, &err), "round trip");
    Expect(round.find("c") != nullptr && round.find("c")->a.size() == 2, "array kept");
}

void TestFrame() {
    const std::string frame = haikuide::EncodeFrame("{\"m\":1}");
    std::string buf = frame.substr(0, 8);
    std::string body;
    std::string err;
    Expect(!haikuide::TakeFrame(&buf, &body, &err) && err.empty(), "short frame waits");
    buf += frame.substr(8);
    Expect(haikuide::TakeFrame(&buf, &body, &err), "frame completes");
    Expect(body == "{\"m\":1}" && buf.empty(), "frame body");
}

void TestPositions() {
    const std::string text = std::string("a") + "\xF0\x9F\x98\x80" + "b";
    const haikuide::Pos at_b = haikuide::ByteToPos(text, 5);
    Expect(at_b.line == 0 && at_b.character == 3, "emoji is two utf-16 units");
    Expect(haikuide::PosToByte(text, 0, 3) == 5, "utf-16 3 is the b");
    Expect(haikuide::PosToByte(text, 0, 2) == 1, "trailing surrogate snaps to emoji");
    Expect(haikuide::PathToUri("/boot/home/a b.cpp") == "file:///boot/home/a%20b.cpp", "uri space");
    Expect(haikuide::UriToPath("file:///boot/home/a%20b.cpp") == "/boot/home/a b.cpp", "path space");
}

void TestServer() {
    Waiter waiter;
    haikuide::LspClient client;
    std::string err;
    Expect(client.Start("python3", {"lsp/testserver/server.py"}, "/tmp", &waiter, &err),
           "start test server");
    if (!err.empty()) {
        std::cout << "    " << err << "\n";
    }
    client.DidOpen("/tmp/sample.cpp", "cpp", 1, "bad idea\n");
    Expect(waiter.Wait(&waiter.got_diag), "diagnostic arrived");
    Expect(waiter.diagnostic == "test diagnostic", "diagnostic text");
    client.Completion("/tmp/sample.cpp", haikuide::Pos{0, 1}, 1, 0);
    Expect(waiter.Wait(&waiter.got_completion), "completion arrived");
    Expect(waiter.completion == "haikuide_word", "completion label");
    client.Hover("/tmp/sample.cpp", haikuide::Pos{0, 1});
    Expect(waiter.Wait(&waiter.got_hover), "hover arrived");
    Expect(waiter.hover == "hover text", "hover text");
    client.Definition("/tmp/sample.cpp", haikuide::Pos{0, 1});
    Expect(waiter.Wait(&waiter.got_def), "definition arrived");
    Expect(waiter.def_line == 2, "definition line");
    client.Stop();
    Expect(!client.running(), "stop leaves the client idle");
}

void TestServerExit() {
    Waiter waiter;
    haikuide::LspClient client;
    std::string err;
    Expect(client.Start("python3", {"lsp/testserver/server.py"}, "/tmp", &waiter, &err),
           "start exit server");
    client.DidOpen("/tmp/die.cpp", "cpp", 1, "PLEASE_EXIT\n");
    Expect(waiter.Wait(&waiter.exited), "server exit is reported");
    Expect(waiter.exit_message.find("exited") != std::string::npos, "exit message");
    Expect(true, "client still answers after the server dies");
    client.Stop();
}

}  // namespace

int main() {
    TestJson();
    TestFrame();
    TestPositions();
    TestServer();
    TestServerExit();
    if (g_failed != 0) {
        std::cout << g_failed << " failed\n";
        return 1;
    }
    std::cout << "all passed\n";
    return 0;
}
