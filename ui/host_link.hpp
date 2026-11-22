/* Node extension host. The window thread only enqueues frames.
 *
 * Copyright (c) 2026 Sean Collins, 2 Paws Machine and Engineering. MIT.
 */
#ifndef HAIKUIDE_HOST_LINK_HPP
#define HAIKUIDE_HOST_LINK_HPP

#ifndef __HAIKU__
#error The extension host link builds on Haiku.
#endif

#include "editor_view.hpp"
#include "json.hpp"
#include "lsp_bridge.hpp"

#include <Handler.h>

#include <cstdint>
#include <string>
#include <vector>

namespace haikuide {

class HostLink : public BHandler {
public:
    HostLink(BWindow* window, LspBridge* lsp, LspSurface* surface);
    void Shutdown();

    bool Start(const std::string& script, std::string* err);
    bool running() const;

    void Open(EditorTab* tab);
    void NoteEdit();
    void FlushEdit(EditorTab* tab);
    void FullSync(EditorTab* tab);
    void Save(EditorTab* tab);
    void Close(const std::string& path);
    void Complete(EditorTab* tab, uint64_t epoch, int kind, char trigger);

    void MessageReceived(BMessage* message) override;

private:
    struct Process;

    void Enqueue(const std::string& json);
    void Request(const std::string& method, Value params, uint64_t epoch);
    void Loop();
    void Post(BMessage* message);

    BWindow* window_ = nullptr;
    LspBridge* lsp_ = nullptr;
    LspSurface* surface_ = nullptr;
    Process* proc_ = nullptr;
    bool dead_ = false;
    bool edit_pending_ = false;
    std::vector<std::string> opened_;
};

}  // namespace haikuide

#endif
