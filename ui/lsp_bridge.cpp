/* Copyright (c) 2026 Sean Collins, 2 Paws Machine and Engineering. MIT. */
#include "lsp_bridge.hpp"

#include "json.hpp"
#include "protocol.hpp"

#include <Message.h>
#include <MessageRunner.h>
#include <Messenger.h>
#include <StringView.h>
#include <Window.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sys/stat.h>

namespace haikuide {
namespace {

const uint32 kLspDiagnostics = 0x48000101;
const uint32 kLspCompletion = 0x48000102;
const uint32 kLspHover = 0x48000103;
const uint32 kLspDefinition = 0x48000104;
const uint32 kLspNote = 0x48000105;
const uint32 kLspExit = 0x48000106;
const uint32 kLspHoverFire = 0x48000107;
const uint32 kLspProblemsClick = 0x48000108;
const uint32 kLspCompletionAccept = 0x48000109;

const float kProblemsH = 96.f;
const float kStatusH = 22.f;
const float kRowH = 18.f;

class ProblemsView : public BView {
public:
    explicit ProblemsView(BRect frame) : BView(frame, "problems", B_FOLLOW_NONE, B_WILL_DRAW) {
        SetViewColor(ui_color(B_PANEL_BACKGROUND_COLOR));
    }

    void SetRows(std::string path, std::vector<LineDiag> rows) {
        path_ = std::move(path);
        rows_ = std::move(rows);
        Invalidate();
    }

    const LineDiag* RowAt(int index) const {
        if (index < 0 || index >= static_cast<int>(rows_.size())) {
            return nullptr;
        }
        return &rows_[static_cast<size_t>(index)];
    }

    const std::string& Path() const { return path_; }

    void SetBridge(BHandler* handler) { bridge_ = handler; }

    void Draw(BRect update) override {
        SetHighColor(ui_color(B_PANEL_BACKGROUND_COLOR));
        FillRect(update);
        SetHighColor(ui_color(B_PANEL_TEXT_COLOR));
        StrokeLine(BPoint(0, 0), BPoint(Bounds().Width(), 0));
        SetFont(be_plain_font);
        if (rows_.empty()) {
            DrawString("No problems", BPoint(8.f, 18.f));
            return;
        }
        const int first = std::max(0, static_cast<int>(update.top / kRowH));
        int last = static_cast<int>(update.bottom / kRowH) + 1;
        if (last > static_cast<int>(rows_.size())) {
            last = static_cast<int>(rows_.size());
        }
        for (int i = first; i < last; ++i) {
            const LineDiag& row = rows_[static_cast<size_t>(i)];
            const char* kind = "info";
            rgb_color color{40, 90, 160, 255};
            if (row.severity == 1) {
                kind = "error";
                color = rgb_color{170, 40, 40, 255};
            } else if (row.severity == 2) {
                kind = "warning";
                color = rgb_color{150, 110, 20, 255};
            }
            char buf[96];
            std::snprintf(buf, sizeof(buf), "%s  %d:%d  ", kind, row.line + 1, row.character + 1);
            const float y = static_cast<float>(i) * kRowH + 14.f;
            SetHighColor(color);
            DrawString(buf, BPoint(8.f, y));
            SetHighColor(ui_color(B_PANEL_TEXT_COLOR));
            DrawString(row.message.c_str(), BPoint(8.f + StringWidth(buf), y));
        }
    }

    void MouseDown(BPoint where) override {
        if (rows_.empty() || bridge_ == nullptr) {
            return;
        }
        const int index = static_cast<int>(where.y / kRowH);
        if (index < 0 || index >= static_cast<int>(rows_.size())) {
            return;
        }
        BMessage message(kLspProblemsClick);
        message.AddInt32("index", index);
        BMessenger(bridge_).SendMessage(&message);
    }

private:
    std::string path_;
    std::vector<LineDiag> rows_;
    BHandler* bridge_ = nullptr;
};

class CompletionView : public BView {
public:
    explicit CompletionView(BRect frame)
        : BView(frame, "completion", B_FOLLOW_NONE, B_WILL_DRAW) {
        SetViewColor(255, 255, 255);
    }

    void SetItems(std::vector<CompletionItem> items) {
        items_ = std::move(items);
        if (items_.size() > 40) {
            items_.resize(40);
        }
        selected_ = 0;
        Invalidate();
    }

    float PreferredHeight() const {
        const int n = std::max(1, std::min(8, static_cast<int>(items_.size())));
        return static_cast<float>(n) * kRowH + 4.f;
    }

    void MoveSelection(int delta) {
        if (items_.empty()) {
            return;
        }
        int next = selected_ + delta;
        if (next < 0) {
            next = 0;
        }
        if (next >= static_cast<int>(items_.size())) {
            next = static_cast<int>(items_.size()) - 1;
        }
        selected_ = next;
        Invalidate();
    }

    const CompletionItem* Selected() const {
        if (selected_ < 0 || selected_ >= static_cast<int>(items_.size())) {
            return nullptr;
        }
        return &items_[static_cast<size_t>(selected_)];
    }

    int IndexAt(float y) const {
        const int row = static_cast<int>(y / kRowH);
        const int shown = std::min(8, static_cast<int>(items_.size()));
        if (row < 0 || row >= shown) {
            return -1;
        }
        const int index = std::max(0, selected_ - 7) + row;
        if (index < 0 || index >= static_cast<int>(items_.size())) {
            return -1;
        }
        return index;
    }

    void SetSelected(int index) { selected_ = index; }

    void SetBridge(BHandler* handler) { bridge_ = handler; }

    void MouseDown(BPoint where) override {
        const int index = IndexAt(where.y);
        if (index < 0 || bridge_ == nullptr) {
            return;
        }
        selected_ = index;
        Invalidate();
        BMessage message(kLspCompletionAccept);
        BMessenger(bridge_).SendMessage(&message);
    }

    void Draw(BRect update) override {
        SetHighColor(255, 255, 255);
        FillRect(update);
        SetHighColor(180, 180, 180);
        StrokeRect(Bounds());
        SetFont(be_plain_font);
        const int n = std::min(8, static_cast<int>(items_.size()));
        const int start = std::max(0, selected_ - 7);
        for (int i = 0; i < n && start + i < static_cast<int>(items_.size()); ++i) {
            const int index = start + i;
            const float y = static_cast<float>(i) * kRowH;
            if (index == selected_) {
                SetHighColor(173, 214, 255);
                FillRect(BRect(1.f, y + 1.f, Bounds().Width() - 1.f, y + kRowH));
            }
            SetHighColor(20, 20, 20);
            DrawString(items_[static_cast<size_t>(index)].label.c_str(), BPoint(6.f, y + 13.f));
        }
    }

private:
    std::vector<CompletionItem> items_;
    int selected_ = 0;
    BHandler* bridge_ = nullptr;
};

class HoverView : public BView {
public:
    explicit HoverView(BRect frame) : BView(frame, "hover", B_FOLLOW_NONE, B_WILL_DRAW) {
        SetViewColor(255, 255, 240);
    }

    void SetText(std::string text) {
        lines_.clear();
        std::string current;
        for (char c : text) {
            if (c == '\n' || current.size() > 80) {
                if (!current.empty()) {
                    lines_.push_back(current);
                }
                current.clear();
                if (c != '\n') {
                    current.push_back(c);
                }
            } else if (c != '\r') {
                current.push_back(c);
            }
            if (lines_.size() == 12) {
                break;
            }
        }
        if (!current.empty() && lines_.size() < 12) {
            lines_.push_back(current);
        }
        Invalidate();
    }

    float PreferredHeight() const {
        const int n = std::max(1, static_cast<int>(lines_.size()));
        return static_cast<float>(n) * kRowH + 8.f;
    }

    void Draw(BRect update) override {
        SetHighColor(255, 255, 240);
        FillRect(update);
        SetHighColor(160, 160, 120);
        StrokeRect(Bounds());
        SetHighColor(30, 30, 30);
        SetFont(be_plain_font);
        for (size_t i = 0; i < lines_.size(); ++i) {
            DrawString(lines_[i].c_str(), BPoint(6.f, 16.f + static_cast<float>(i) * kRowH));
        }
    }

private:
    std::vector<std::string> lines_;
};

std::string SettingsPath() {
    const char* home = std::getenv("HOME");
    if (home == nullptr || home[0] == '\0') {
        home = "/boot/home";
    }
    return std::string(home) + "/config/settings/HaikuIDE/settings.json";
}

}  // namespace

LspBridge::LspBridge(BWindow* window, LspSurface* surface)
    : BHandler("lsp"), window_(window), surface_(surface) {
    problems_ = new ProblemsView(BRect(0, 0, 100, kProblemsH));
    status_ = new BStringView(BRect(0, 0, 100, kStatusH), "status", "Ready");
    status_->SetViewColor(ui_color(B_PANEL_BACKGROUND_COLOR));
    status_->SetHighColor(ui_color(B_PANEL_TEXT_COLOR));
    completion_ = new CompletionView(BRect(0, 0, 280, 40));
    hover_ = new HoverView(BRect(0, 0, 360, 40));
    window_->AddChild(problems_);
    window_->AddChild(status_);
    window_->AddChild(completion_);
    window_->AddChild(hover_);
    window_->AddHandler(this);
    static_cast<ProblemsView*>(problems_)->SetBridge(this);
    static_cast<CompletionView*>(completion_)->SetBridge(this);
    completion_->Hide();
    hover_->Hide();
    LoadConfig();
}

void LspBridge::Shutdown() {
    dead_ = true;
    delete hover_runner_;
    hover_runner_ = nullptr;
    client_.Stop();
}

float LspBridge::ChromeHeight() const { return kProblemsH + kStatusH; }

void LspBridge::Layout(float width, float height) {
    status_->MoveTo(0.f, std::max(0.f, height - kStatusH));
    status_->ResizeTo(std::max(0.f, width), kStatusH);
    problems_->MoveTo(0.f, std::max(0.f, height - kStatusH - kProblemsH));
    problems_->ResizeTo(std::max(0.f, width), kProblemsH);
}

void LspBridge::SetStatus(const std::string& text) {
    if (status_ != nullptr) {
        status_->SetText(text.c_str());
    }
}

void LspBridge::Post(BMessage* message) {
    if (dead_) {
        delete message;
        return;
    }
    BMessenger(this).SendMessage(message);
    delete message;
}

void LspBridge::HidePopups() {
    if (completion_ != nullptr && !completion_->IsHidden()) {
        completion_->Hide();
    }
    if (hover_ != nullptr && !hover_->IsHidden()) {
        hover_->Hide();
    }
}

void LspBridge::LoadConfig() {
    servers_.clear();
    ServerSpec fallback;
    fallback.language = "cpp";
    fallback.command = "clangd";
    fallback.args = {"--background-index=0", "--header-insertion=never"};
    fallback.extensions = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx"};
    const std::string path = SettingsPath();
    std::ifstream in(path);
    bool used_file = false;
    if (in) {
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        Value root;
        std::string err;
        if (Parse(text, &root, &err)) {
            if (const Value* list = root.find("haikuide.languageServers")) {
                if (list->kind == Value::Kind::Array) {
                    for (const Value& item : list->a) {
                        ServerSpec spec = fallback;
                        spec.args.clear();
                        spec.extensions.clear();
                        if (const Value* language = item.find("language")) {
                            if (language->kind == Value::Kind::String) {
                                spec.language = language->s;
                            }
                        }
                        if (const Value* command = item.find("command")) {
                            if (command->kind == Value::Kind::String) {
                                spec.command = command->s;
                            }
                        }
                        if (const Value* args = item.find("args")) {
                            if (args->kind == Value::Kind::Array) {
                                for (const Value& arg : args->a) {
                                    if (arg.kind == Value::Kind::String) {
                                        spec.args.push_back(arg.s);
                                    }
                                }
                            }
                        }
                        if (const Value* exts = item.find("extensions")) {
                            if (exts->kind == Value::Kind::Array) {
                                for (const Value& ext : exts->a) {
                                    if (ext.kind == Value::Kind::String) {
                                        spec.extensions.push_back(ext.s);
                                    }
                                }
                            }
                        }
                        if (spec.extensions.empty()) {
                            spec.extensions = fallback.extensions;
                        }
                        if (!spec.command.empty()) {
                            servers_.push_back(std::move(spec));
                        }
                    }
                    used_file = true;
                }
            }
        }
    }
    if (servers_.empty()) {
        servers_.push_back(fallback);
    }
    if (!used_file) {
        const std::string dir = path.substr(0, path.find_last_of('/'));
        mkdir(dir.c_str(), 0755);
        Value root = JsonObject();
        Value list = JsonArray();
        Value item = JsonObject();
        Put(item, "language", JsonString(fallback.language));
        Put(item, "command", JsonString(fallback.command));
        Value args = JsonArray();
        for (const std::string& arg : fallback.args) {
            Push(args, JsonString(arg));
        }
        Put(item, "args", std::move(args));
        Value exts = JsonArray();
        for (const std::string& ext : fallback.extensions) {
            Push(exts, JsonString(ext));
        }
        Put(item, "extensions", std::move(exts));
        Push(list, std::move(item));
        Put(root, "haikuide.languageServers", std::move(list));
        std::ofstream out(path);
        if (out) {
            out << Stringify(root) << "\n";
        }
    }
}

const LspBridge::ServerSpec* LspBridge::Match(const std::string& path) const {
    const std::string ext = ExtensionOf(path);
    for (const ServerSpec& spec : servers_) {
        for (const std::string& one : spec.extensions) {
            if (one == ext) {
                return &spec;
            }
        }
    }
    return nullptr;
}

bool LspBridge::Ensure(const ServerSpec& spec, const std::string& path) {
    if (client_.running()) {
        return true;
    }
    root_ = path;
    const size_t slash = root_.find_last_of('/');
    if (slash != std::string::npos && slash > 0) {
        root_.resize(slash);
    } else {
        root_ = "/boot/home";
    }
    std::string err;
    opened_.clear();
    if (!client_.Start(spec.command, spec.args, root_, this, &err)) {
        SetStatus(err);
        return false;
    }
    ready_name_ = spec.command;
    SetStatus(spec.command + " starting");
    return true;
}

void LspBridge::Open(EditorTab* tab) {
    if (dead_ || tab == nullptr || tab->path.empty()) {
        return;
    }
    const ServerSpec* spec = Match(tab->path);
    if (spec == nullptr) {
        SetStatus("plain text");
        return;
    }
    if (!Ensure(*spec, tab->path)) {
        return;
    }
    if (std::find(opened_.begin(), opened_.end(), tab->path) != opened_.end()) {
        return;
    }
    opened_.push_back(tab->path);
    client_.DidOpen(tab->path, spec->language, static_cast<int64_t>(tab->doc.version()), tab->flat);
}

void LspBridge::Close(const std::string& path) {
    if (path.empty()) {
        return;
    }
    auto found = std::find(opened_.begin(), opened_.end(), path);
    if (found == opened_.end()) {
        return;
    }
    opened_.erase(found);
    client_.DidClose(path);
}

void LspBridge::Save(EditorTab* tab) {
    Open(tab);
    if (tab == nullptr || tab->path.empty()) {
        return;
    }
    if (std::find(opened_.begin(), opened_.end(), tab->path) == opened_.end()) {
        return;
    }
    client_.DidSave(tab->path, tab->flat);
}

void LspBridge::ShowTab(EditorTab* tab) {
    auto* problems = static_cast<ProblemsView*>(problems_);
    if (tab == nullptr) {
        problems->SetRows(std::string(), {});
        return;
    }
    problems->SetRows(tab->path, tab->diags);
}

void LspBridge::RememberEdit(EditorTab* tab, size_t pos, size_t removed, const std::string& inserted) {
    if (!accepting_) {
        HidePopups();
    }
    if (tab == nullptr || tab->path.empty() || Match(tab->path) == nullptr) {
        edit_pending_ = false;
        return;
    }
    edit_start_ = ByteToPos(tab->flat, pos);
    edit_end_ = ByteToPos(tab->flat, pos + removed);
    edit_text_ = inserted;
    edit_pending_ = true;
}

void LspBridge::FlushEdit(EditorTab* tab) {
    if (!edit_pending_ || tab == nullptr) {
        return;
    }
    edit_pending_ = false;
    const ServerSpec* spec = Match(tab->path);
    if (spec == nullptr || !Ensure(*spec, tab->path)) {
        return;
    }
    if (std::find(opened_.begin(), opened_.end(), tab->path) == opened_.end()) {
        opened_.push_back(tab->path);
        client_.DidOpen(tab->path, spec->language, static_cast<int64_t>(tab->doc.version()), tab->flat);
        return;
    }
    client_.DidChange(tab->path, static_cast<int64_t>(tab->doc.version()), edit_start_, edit_end_,
                      edit_text_, tab->flat);
}

void LspBridge::FullSync(EditorTab* tab) {
    edit_pending_ = false;
    if (tab == nullptr || tab->path.empty()) {
        return;
    }
    const ServerSpec* spec = Match(tab->path);
    if (spec == nullptr || !Ensure(*spec, tab->path)) {
        return;
    }
    if (std::find(opened_.begin(), opened_.end(), tab->path) == opened_.end()) {
        opened_.push_back(tab->path);
        client_.DidOpen(tab->path, spec->language, static_cast<int64_t>(tab->doc.version()), tab->flat);
        return;
    }
    client_.DidChangeFull(tab->path, static_cast<int64_t>(tab->doc.version()), tab->flat);
}

void LspBridge::SetHostAttached(bool attached) {
    host_attached_ = attached;
    if (!attached) {
        host_ready_ = true;
    }
}

void LspBridge::SetStatusText(const std::string& text) { SetStatus(text); }

uint64_t LspBridge::CompleteEpoch() const { return complete_epoch_; }

void LspBridge::BeginComplete() {
    ++complete_epoch_;
    clang_items_.clear();
    host_items_.clear();
    clang_ready_ = false;
    host_ready_ = !host_attached_;
}

void LspBridge::OfferHost(uint64_t epoch, std::vector<CompletionItem> items) {
    if (epoch != complete_epoch_) {
        return;
    }
    host_items_ = std::move(items);
    host_ready_ = true;
    ShowMerged();
}

void LspBridge::HostDown(const std::string& message) {
    host_attached_ = false;
    host_failed_ = true;
    host_ready_ = true;
    SetStatus(message);
    ShowMerged();
}

void LspBridge::HostDiagnostics(const std::string& path, std::vector<Diagnostic> diags) {
    ApplyDiagnostics(path, diags);
}

void LspBridge::ShowMerged() {
    if (!clang_ready_ && !host_ready_) {
        return;
    }
    std::vector<CompletionItem> merged = clang_items_;
    merged.insert(merged.end(), host_items_.begin(), host_items_.end());
    auto* completion = static_cast<CompletionView*>(completion_);
    if (completion == nullptr) {
        return;
    }
    if (merged.empty()) {
        if (clang_ready_ && host_ready_) {
            completion->Hide();
            if (!host_failed_) {
                SetStatus("no completions");
            }
        }
        return;
    }
    if (surface_ == nullptr || surface_->Editor() == nullptr || window_ == nullptr) {
        return;
    }
    completion->SetItems(std::move(merged));
    BPoint where = window_->ConvertFromScreen(surface_->Editor()->CaretScreenPoint());
    const float height = completion->PreferredHeight();
    const float width = 280.f;
    if (where.x + width > window_->Bounds().Width()) {
        where.x = std::max(0.f, window_->Bounds().Width() - width);
    }
    if (where.y + height > window_->Bounds().Height() - ChromeHeight()) {
        where.y = std::max(0.f, where.y - height - 20.f);
    }
    completion->MoveTo(where);
    completion->ResizeTo(width, height);
    if (completion->IsHidden()) {
        completion->Show();
    }
    completion->Invalidate();
    if (clang_ready_ && host_ready_ && !host_failed_) {
        std::string text;
        auto append = [&text](const std::vector<CompletionItem>& items) {
            for (const CompletionItem& item : items) {
                if (item.label.empty()) {
                    continue;
                }
                if (!text.empty()) {
                    text += ", ";
                }
                text += item.label;
                if (text.size() > 120) {
                    return;
                }
            }
        };
        append(clang_items_);
        append(host_items_);
        if (!text.empty()) {
            SetStatus(text);
        }
    }
}

void LspBridge::Complete(EditorTab* tab, int kind, char trigger) {
    BeginComplete();
    if (tab == nullptr || tab->path.empty() || !client_.running()) {
        Open(tab);
    }
    if (tab == nullptr || tab->path.empty() || !client_.running()) {
        clang_ready_ = true;
        ShowMerged();
        return;
    }
    const Pos pos = ByteToPos(tab->flat, tab->caret);
    client_.Completion(tab->path, pos, kind, trigger);
}

void LspBridge::Define(EditorTab* tab) {
    HidePopups();
    if (tab == nullptr || tab->path.empty()) {
        return;
    }
    Open(tab);
    if (!client_.running()) {
        return;
    }
    const Pos pos = ByteToPos(tab->flat, tab->caret);
    client_.Definition(tab->path, pos);
}

void LspBridge::ArmHover(size_t byte) {
    if (dead_ || surface_ == nullptr || surface_->ActiveTab() == nullptr) {
        return;
    }
    if (byte == hover_byte_ && hover_runner_ != nullptr) {
        return;
    }
    hover_byte_ = byte;
    ++hover_gen_;
    delete hover_runner_;
    BMessage fire(kLspHoverFire);
    fire.AddInt32("gen", hover_gen_);
    hover_runner_ = new BMessageRunner(BMessenger(this), &fire, 400000, 1);
}

bool LspBridge::Key(int32 raw) {
    auto* completion = static_cast<CompletionView*>(completion_);
    if (completion->IsHidden()) {
        if (hover_ != nullptr && !hover_->IsHidden()) {
            hover_->Hide();
        }
        return false;
    }
    if (raw == B_UP_ARROW) {
        completion->MoveSelection(-1);
        return true;
    }
    if (raw == B_DOWN_ARROW) {
        completion->MoveSelection(1);
        return true;
    }
    if (raw == B_RETURN || raw == B_TAB) {
        Accept();
        return true;
    }
    if (raw == B_ESCAPE) {
        completion->Hide();
        return true;
    }
    return false;
}

void LspBridge::Accept() {
    auto* completion = static_cast<CompletionView*>(completion_);
    const CompletionItem* item = completion->Selected();
    EditorTab* tab = surface_ != nullptr ? surface_->ActiveTab() : nullptr;
    if (item == nullptr || tab == nullptr || surface_ == nullptr) {
        completion->Hide();
        return;
    }
    const std::string text = item->insert;
    const bool has_range = item->has_range;
    const int sl = item->start_line;
    const int sc = item->start_character;
    const int el = item->end_line;
    const int ec = item->end_character;
    completion->Hide();
    accepting_ = true;
    if (has_range) {
        const size_t a = PosToByte(tab->flat, sl, sc);
        const size_t b = PosToByte(tab->flat, el, ec);
        surface_->ApplyEdit(a, b >= a ? b - a : 0, text);
    } else {
        size_t b = tab->caret;
        size_t a = b;
        while (a > 0) {
            const unsigned char c = static_cast<unsigned char>(tab->flat[a - 1]);
            if (std::isalnum(c) == 0 && c != '_') {
                break;
            }
            --a;
        }
        surface_->ApplyEdit(a, b - a, text);
    }
    accepting_ = false;
}

void LspBridge::OnDiagnostics(std::string path, std::vector<Diagnostic> diags) {
    BMessage* message = new BMessage(kLspDiagnostics);
    message->AddString("path", path.c_str());
    for (const Diagnostic& diag : diags) {
        message->AddInt32("line", diag.line);
        message->AddInt32("character", diag.character);
        message->AddInt32("end_line", diag.end_line);
        message->AddInt32("end_character", diag.end_character);
        message->AddInt32("severity", diag.severity);
        message->AddString("message", diag.message.c_str());
    }
    Post(message);
}

void LspBridge::OnCompletion(std::vector<CompletionItem> items) {
    BMessage* message = new BMessage(kLspCompletion);
    for (const CompletionItem& item : items) {
        message->AddString("label", item.label.c_str());
        message->AddString("insert", item.insert.c_str());
        message->AddInt32("has_range", item.has_range ? 1 : 0);
        message->AddInt32("start_line", item.start_line);
        message->AddInt32("start_character", item.start_character);
        message->AddInt32("end_line", item.end_line);
        message->AddInt32("end_character", item.end_character);
    }
    Post(message);
}

void LspBridge::OnHover(std::string text) {
    BMessage* message = new BMessage(kLspHover);
    message->AddString("text", text.c_str());
    Post(message);
}

void LspBridge::OnDefinition(std::vector<Location> locations) {
    BMessage* message = new BMessage(kLspDefinition);
    if (!locations.empty()) {
        message->AddString("path", locations[0].path.c_str());
        message->AddInt32("line", locations[0].line);
        message->AddInt32("character", locations[0].character);
    }
    Post(message);
}

void LspBridge::OnNote(std::string text) {
    BMessage* message = new BMessage(kLspNote);
    message->AddString("text", text.c_str());
    Post(message);
}

void LspBridge::OnServerExit(std::string message) {
    BMessage* note = new BMessage(kLspExit);
    note->AddString("text", message.c_str());
    Post(note);
}

void LspBridge::ApplyDiagnostics(const std::string& path, const std::vector<Diagnostic>& diags) {
    EditorTab* tab = surface_ != nullptr ? surface_->FindTab(path) : nullptr;
    if (tab == nullptr) {
        return;
    }
    tab->diags.clear();
    for (const Diagnostic& diag : diags) {
        LineDiag row;
        row.line = diag.line;
        row.character = diag.character;
        row.end_line = diag.end_line;
        row.end_character = diag.end_character;
        row.severity = diag.severity;
        row.message = diag.message;
        row.start_byte = PosToByte(tab->flat, diag.line, diag.character);
        row.end_byte = PosToByte(tab->flat, diag.end_line, diag.end_character);
        tab->diags.push_back(std::move(row));
    }
    if (surface_->ActiveTab() == tab) {
        ShowTab(tab);
        int errors = 0;
        for (const LineDiag& row : tab->diags) {
            if (row.severity == 1) {
                ++errors;
            }
        }
        if (tab->diags.empty()) {
            SetStatus(ready_name_.empty() ? "no problems" : ready_name_ + "  no problems");
        } else if (errors == static_cast<int>(tab->diags.size())) {
            SetStatus(std::to_string(errors) + (errors == 1 ? " error" : " errors"));
        } else {
            SetStatus(std::to_string(tab->diags.size()) + " problems");
        }
    }
    surface_->RefreshEditor();
}

void LspBridge::MessageReceived(BMessage* message) {
    if (dead_) {
        return;
    }
    switch (message->what) {
    case kLspDiagnostics: {
        const char* path = nullptr;
        if (message->FindString("path", &path) != B_OK || path == nullptr) {
            break;
        }
        std::vector<Diagnostic> diags;
        int32 line = 0;
        for (int32 i = 0; message->FindInt32("line", i, &line) == B_OK; ++i) {
            Diagnostic diag;
            diag.path = path;
            diag.line = line;
            int32 value = 0;
            if (message->FindInt32("character", i, &value) == B_OK) {
                diag.character = value;
            }
            if (message->FindInt32("end_line", i, &value) == B_OK) {
                diag.end_line = value;
            }
            if (message->FindInt32("end_character", i, &value) == B_OK) {
                diag.end_character = value;
            }
            if (message->FindInt32("severity", i, &value) == B_OK) {
                diag.severity = value;
            }
            const char* text = nullptr;
            if (message->FindString("message", i, &text) == B_OK && text != nullptr) {
                diag.message = text;
            }
            diags.push_back(std::move(diag));
        }
        ApplyDiagnostics(path, diags);
        break;
    }
    case kLspCompletion: {
        std::vector<CompletionItem> items;
        const char* label = nullptr;
        for (int32 i = 0; message->FindString("label", i, &label) == B_OK; ++i) {
            CompletionItem item;
            item.label = label != nullptr ? label : "";
            const char* insert = nullptr;
            if (message->FindString("insert", i, &insert) == B_OK && insert != nullptr) {
                item.insert = insert;
            }
            int32 value = 0;
            if (message->FindInt32("has_range", i, &value) == B_OK) {
                item.has_range = value != 0;
            }
            if (message->FindInt32("start_line", i, &value) == B_OK) {
                item.start_line = value;
            }
            if (message->FindInt32("start_character", i, &value) == B_OK) {
                item.start_character = value;
            }
            if (message->FindInt32("end_line", i, &value) == B_OK) {
                item.end_line = value;
            }
            if (message->FindInt32("end_character", i, &value) == B_OK) {
                item.end_character = value;
            }
            if (!item.label.empty()) {
                items.push_back(std::move(item));
            }
        }
        clang_items_ = std::move(items);
        clang_ready_ = true;
        ShowMerged();
        break;
    }
    case kLspHover: {
        const char* text = nullptr;
        if (message->FindString("text", &text) != B_OK || text == nullptr || text[0] == '\0' ||
            surface_ == nullptr || surface_->Editor() == nullptr) {
            hover_->Hide();
            break;
        }
        auto* hover = static_cast<HoverView*>(hover_);
        hover->SetText(text);
        BPoint where = window_->ConvertFromScreen(surface_->Editor()->CaretScreenPoint());
        where.y -= hover->PreferredHeight() + 4.f;
        if (where.y < 0.f) {
            where.y = 0.f;
        }
        hover->MoveTo(where);
        hover->ResizeTo(420.f, hover->PreferredHeight());
        if (hover->IsHidden()) {
            hover->Show();
        }
        hover->Invalidate();
        break;
    }
    case kLspDefinition: {
        const char* path = nullptr;
        int32 line = 0;
        int32 character = 0;
        if (message->FindString("path", &path) != B_OK || path == nullptr ||
            message->FindInt32("line", &line) != B_OK) {
            SetStatus("No definition");
            break;
        }
        message->FindInt32("character", &character);
        SetStatus(std::string("Definition line ") + std::to_string(line + 1));
        if (surface_ != nullptr) {
            surface_->GoTo(path, line, character);
        }
        break;
    }
    case kLspNote: {
        const char* text = nullptr;
        if (message->FindString("text", &text) == B_OK && text != nullptr) {
            ready_name_ = text;
            SetStatus(ready_name_);
        }
        break;
    }
    case kLspExit: {
        const char* text = nullptr;
        opened_.clear();
        HidePopups();
        if (message->FindString("text", &text) == B_OK && text != nullptr) {
            SetStatus(text);
        } else {
            SetStatus("language server stopped");
        }
        break;
    }
    case kLspHoverFire: {
        int32 gen = 0;
        if (message->FindInt32("gen", &gen) != B_OK || gen != hover_gen_) {
            break;
        }
        EditorTab* tab = surface_ != nullptr ? surface_->ActiveTab() : nullptr;
        if (tab == nullptr || tab->path.empty()) {
            break;
        }
        Open(tab);
        if (!client_.running()) {
            break;
        }
        client_.Hover(tab->path, ByteToPos(tab->flat, hover_byte_));
        break;
    }
    case kLspProblemsClick: {
        int32 index = -1;
        auto* problems = static_cast<ProblemsView*>(problems_);
        if (message->FindInt32("index", &index) == B_OK) {
            const LineDiag* row = problems->RowAt(index);
            if (row != nullptr && surface_ != nullptr) {
                surface_->GoTo(problems->Path(), row->line, row->character);
            }
        }
        break;
    }
    case kLspCompletionAccept:
        Accept();
        break;
    default:
        BHandler::MessageReceived(message);
        break;
    }
}

}  // namespace haikuide
