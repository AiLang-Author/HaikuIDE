/* Language client wiring for the libbe window.
 *
 * Copyright (c) 2026 Sean Collins, 2 Paws Machine and Engineering. MIT.
 */
#ifndef HAIKUIDE_LSP_BRIDGE_HPP
#define HAIKUIDE_LSP_BRIDGE_HPP

#ifndef __HAIKU__
#error The language-client window builds on Haiku.
#endif

#include "client.hpp"
#include "editor_view.hpp"

#include <Handler.h>

#include <cstdint>
#include <string>
#include <vector>

class BMessageRunner;
class BStringView;
class BView;
class BWindow;

namespace haikuide {

class LspSurface {
public:
    virtual ~LspSurface() = default;
    virtual EditorTab* ActiveTab() = 0;
    virtual EditorTab* FindTab(const std::string& path) = 0;
    virtual EditorView* Editor() = 0;
    virtual void GoTo(const std::string& path, int line, int character) = 0;
    virtual void ApplyEdit(size_t pos, size_t len, const std::string& text) = 0;
    virtual void RefreshEditor() = 0;
};

class LspBridge : public BHandler, private LspClient::Handler {
public:
    LspBridge(BWindow* window, LspSurface* surface);
    void Shutdown();

    float ChromeHeight() const;
    void Layout(float width, float height);

    void Open(EditorTab* tab);
    void Close(const std::string& path);
    void Save(EditorTab* tab);
    void ShowTab(EditorTab* tab);
    void RememberEdit(EditorTab* tab, size_t pos, size_t removed, const std::string& inserted);
    void FlushEdit(EditorTab* tab);
    void FullSync(EditorTab* tab);
    void Complete(EditorTab* tab, int kind, char trigger);
    void SetHostAttached(bool attached);
    uint64_t CompleteEpoch() const;
    void OfferHost(uint64_t epoch, std::vector<CompletionItem> items);
    void HostDown(const std::string& message);
    void HostDiagnostics(const std::string& path, std::vector<Diagnostic> diags);
    void SetStatusText(const std::string& text);
    void Define(EditorTab* tab);
    void ArmHover(size_t byte);
    bool Key(int32 raw);
    void Accept();

    void MessageReceived(BMessage* message) override;

private:
    struct ServerSpec {
        std::string language;
        std::string command;
        std::vector<std::string> args;
        std::vector<std::string> extensions;
    };

    void OnDiagnostics(std::string path, std::vector<Diagnostic> diags) override;
    void OnCompletion(std::vector<CompletionItem> items) override;
    void OnHover(std::string text) override;
    void OnDefinition(std::vector<Location> locations) override;
    void OnNote(std::string text) override;
    void OnServerExit(std::string message) override;

    void LoadConfig();
    const ServerSpec* Match(const std::string& path) const;
    bool Ensure(const ServerSpec& spec, const std::string& path);
    void SetStatus(const std::string& text);
    void Post(BMessage* message);
    void ApplyDiagnostics(const std::string& path, const std::vector<Diagnostic>& diags);
    void HidePopups();
    void BeginComplete();
    void ShowMerged();

    BWindow* window_ = nullptr;
    LspSurface* surface_ = nullptr;
    LspClient client_;
    BView* problems_ = nullptr;
    BStringView* status_ = nullptr;
    BView* completion_ = nullptr;
    BView* hover_ = nullptr;
    BMessageRunner* hover_runner_ = nullptr;
    std::vector<ServerSpec> servers_;
    std::vector<std::string> opened_;
    std::string root_;
    std::string ready_name_;
    bool edit_pending_ = false;
    bool accepting_ = false;
    bool dead_ = false;
    Pos edit_start_;
    Pos edit_end_;
    std::string edit_text_;
    int32 hover_gen_ = 0;
    size_t hover_byte_ = 0;
    uint64_t complete_epoch_ = 0;
    bool clang_ready_ = false;
    bool host_ready_ = false;
    bool host_attached_ = false;
    bool host_failed_ = false;
    std::vector<CompletionItem> clang_items_;
    std::vector<CompletionItem> host_items_;
};

}  // namespace haikuide

#endif
