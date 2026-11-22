/* Built-in stdio language client. No Haiku headers.
 * The reader thread owns the pipes. Callers only enqueue frames.
 *
 * Copyright (c) 2026 Sean Collins, 2 Paws Machine and Engineering. MIT.
 */
#ifndef HAIKUIDE_CLIENT_HPP
#define HAIKUIDE_CLIENT_HPP

#include "protocol.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace haikuide {

struct Diagnostic {
    std::string path;
    int line = 0;
    int character = 0;
    int end_line = 0;
    int end_character = 0;
    int severity = 1;
    std::string message;
};

struct CompletionItem {
    std::string label;
    std::string insert;
    bool has_range = false;
    int start_line = 0;
    int start_character = 0;
    int end_line = 0;
    int end_character = 0;
};

struct Location {
    std::string path;
    int line = 0;
    int character = 0;
};

class LspClient {
public:
    class Handler {
    public:
        virtual ~Handler() = default;
        virtual void OnDiagnostics(std::string path, std::vector<Diagnostic> diags) = 0;
        virtual void OnCompletion(std::vector<CompletionItem> items) = 0;
        virtual void OnHover(std::string text) = 0;
        virtual void OnDefinition(std::vector<Location> locations) = 0;
        virtual void OnNote(std::string text) = 0;
        virtual void OnServerExit(std::string message) = 0;
    };

    LspClient();
    ~LspClient();

    LspClient(const LspClient&) = delete;
    LspClient& operator=(const LspClient&) = delete;

    bool Start(const std::string& command, const std::vector<std::string>& args,
               const std::string& root_path, Handler* handler, std::string* err);
    void Stop();
    bool running() const;

    void DidOpen(const std::string& path, const std::string& language, int64_t version,
                 const std::string& text);
    void DidChange(const std::string& path, int64_t version, Pos start, Pos end,
                   const std::string& text, const std::string& full);
    void DidChangeFull(const std::string& path, int64_t version, const std::string& text);
    void DidSave(const std::string& path, const std::string& text);
    void DidClose(const std::string& path);
    void Completion(const std::string& path, Pos pos, int trigger_kind, char trigger);
    void Hover(const std::string& path, Pos pos);
    void Definition(const std::string& path, Pos pos);

private:
    struct Process;

    void Enqueue(const std::string& json);
    void Loop();
    void Fail(const std::string& message);

    Process* proc_ = nullptr;
};

}  // namespace haikuide

#endif
