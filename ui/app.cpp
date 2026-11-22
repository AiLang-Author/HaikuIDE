/* HaikuIDE window.
 * BApplication, menu bar, tab strip, open / save. The text view scrolls
 * inside a BScrollView. libbe only.
 *
 * Copyright (c) 2026 Sean Collins, 2 Paws Machine and Engineering. MIT.
 */
#include "editor_view.hpp"
#include "host_link.hpp"
#include "lsp_bridge.hpp"
#include "protocol.hpp"

#include <Alert.h>
#include <Application.h>
#include <Directory.h>
#include <Entry.h>
#include <File.h>
#include <FilePanel.h>
#include <Menu.h>
#include <MenuBar.h>
#include <MenuItem.h>
#include <Path.h>
#include <Region.h>
#include <Screen.h>
#include <ScrollView.h>
#include <Window.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

namespace haikuide {
namespace {

const char* kSignature = "application/x-vnd.2paws-HaikuIDE";

enum Pending { kPendingNone, kPendingSaveAs, kPendingClose, kPendingQuit };

// Posted from Show(). The window thread is not running yet inside the
// constructor, and Show() releases that lock before it returns.
const uint32 kMsgReady = 0x48000020;

void Info(const char* text) {
    BAlert alert("HaikuIDE", text, "OK", nullptr, nullptr, B_WIDTH_AS_USUAL, B_STOP_ALERT);
    alert.Go();
}

int ConfirmSave(const std::string& name) {
    const std::string text = "Save changes to " + name + "?";
    BAlert alert("HaikuIDE", text.c_str(), "Cancel", "Discard", "Save", B_WIDTH_AS_USUAL,
                 B_WARNING_ALERT);
    alert.SetShortcut(0, B_ESCAPE);
    return alert.Go();
}

std::string Leaf(const std::string& path) {
    const size_t slash = path.find_last_of('/');
    if (slash == std::string::npos || slash + 1 >= path.size()) {
        return path;
    }
    return path.substr(slash + 1);
}

bool ReadFile(const char* path, std::string* out, std::string* err) {
    BFile file(path, B_READ_ONLY);
    if (file.InitCheck() != B_OK) {
        *err = std::string("Could not open ") + path;
        return false;
    }
    off_t size = 0;
    if (file.GetSize(&size) != B_OK || size < 0) {
        *err = std::string("Could not read ") + path;
        return false;
    }
    if (size > 32 * 1024 * 1024) {
        *err = "File is larger than 32 MB.";
        return false;
    }
    out->assign(static_cast<size_t>(size), '\0');
    size_t got = 0;
    while (got < out->size()) {
        const ssize_t n = file.Read(&(*out)[got], out->size() - got);
        if (n < 0) {
            *err = std::string("Could not read ") + path;
            return false;
        }
        if (n == 0) {
            break;
        }
        got += static_cast<size_t>(n);
    }
    out->resize(got);
    return true;
}

bool WriteFile(const char* path, const EditorTab& tab, std::string* err) {
    std::string converted;
    const std::string* src = &tab.flat;
    if (tab.doc.newline_style() == NewlineStyle::Crlf) {
        converted.reserve(tab.flat.size() + 16);
        for (char c : tab.flat) {
            if (c == '\n') {
                converted += "\r\n";
            } else {
                converted.push_back(c);
            }
        }
        src = &converted;
    } else if (tab.doc.newline_style() == NewlineStyle::Cr) {
        converted = tab.flat;
        for (char& c : converted) {
            if (c == '\n') {
                c = '\r';
            }
        }
        src = &converted;
    }
    BFile file(path, B_WRITE_ONLY | B_CREATE_FILE | B_ERASE_FILE);
    if (file.InitCheck() != B_OK) {
        *err = std::string("Could not write ") + path;
        return false;
    }
    size_t put = 0;
    while (put < src->size()) {
        const ssize_t n = file.Write(src->data() + put, src->size() - put);
        if (n < 0) {
            *err = std::string("Could not write ") + path;
            return false;
        }
        if (n == 0) {
            break;
        }
        put += static_cast<size_t>(n);
    }
    if (put != src->size() || file.Sync() != B_OK) {
        *err = std::string("Could not write ") + path;
        return false;
    }
    return true;
}

BRect DefaultFrame() {
    BScreen screen;
    BRect desk = screen.Frame();
    if (desk.Width() < 200.f || desk.Height() < 200.f) {
        desk.Set(0, 0, 1024, 768);
    }
    float w = std::min(960.f, desk.Width() - 48.f);
    float h = std::min(700.f, desk.Height() - 48.f);
    if (w < 480.f) {
        w = desk.Width();
    }
    if (h < 320.f) {
        h = desk.Height();
    }
    const float x = desk.left + (desk.Width() - w) / 2.f;
    const float y = desk.top + (desk.Height() - h) / 2.f;
    return BRect(x, y, x + w, y + h);
}

}  // namespace

class TabStrip : public BView {
public:
    TabStrip()
        : BView(BRect(0, 0, 100, 28), "tabs", B_FOLLOW_NONE, B_WILL_DRAW | B_FULL_UPDATE_ON_RESIZE) {}

    void SetItems(const std::vector<std::string>& labels, int32 active) {
        labels_ = labels;
        active_ = active;
        Invalidate();
    }

    void AttachedToWindow() override {
        BView::AttachedToWindow();
        SetFont(be_plain_font);
        SetViewColor(ui_color(B_PANEL_BACKGROUND_COLOR));
    }

    void Draw(BRect update) override {
        const rgb_color panel = ui_color(B_PANEL_BACKGROUND_COLOR);
        SetHighColor(panel);
        FillRect(update);
        rects_.clear();
        float x = 4.f;
        const float top = 3.f;
        const float bottom = Bounds().bottom - 1.f;
        for (size_t i = 0; i < labels_.size(); ++i) {
            float w = StringWidth(labels_[i].c_str()) + 22.f;
            if (w < 72.f) {
                w = 72.f;
            }
            if (w > 180.f) {
                w = 180.f;
            }
            const BRect rect(x, top, x + w, bottom);
            rects_.push_back(rect);
            const bool on = static_cast<int32>(i) == active_;
            SetHighColor(on ? ui_color(B_DOCUMENT_BACKGROUND_COLOR) : panel);
            FillRect(rect);
            SetHighColor(ui_color(B_PANEL_TEXT_COLOR));
            BRegion clip;
            clip.Set(rect.InsetByCopy(6.f, 1.f));
            ConstrainClippingRegion(&clip);
            DrawString(labels_[i].c_str(), BPoint(x + 8.f, bottom - 6.f));
            ConstrainClippingRegion(nullptr);
            x += w + 4.f;
        }
        SetHighColor(tint_color(panel, B_DARKEN_1_TINT));
        StrokeLine(BPoint(update.left, Bounds().bottom), BPoint(update.right, Bounds().bottom));
    }

    void MouseDown(BPoint where) override {
        if (rects_.empty()) {
            LayoutRects();
        }
        for (size_t i = 0; i < rects_.size(); ++i) {
            if (rects_[i].Contains(where)) {
                BMessage message(kMsgSelectTab);
                message.AddInt32("index", static_cast<int32>(i));
                if (Window() != nullptr) {
                    Window()->PostMessage(&message);
                }
                return;
            }
        }
    }

private:
    void LayoutRects() {
        rects_.clear();
        float x = 4.f;
        for (const std::string& label : labels_) {
            float w = StringWidth(label.c_str()) + 22.f;
            if (w < 72.f) {
                w = 72.f;
            }
            if (w > 180.f) {
                w = 180.f;
            }
            rects_.push_back(BRect(x, 3.f, x + w, Bounds().bottom - 1.f));
            x += w + 4.f;
        }
    }

    std::vector<std::string> labels_;
    std::vector<BRect> rects_;
    int32 active_ = -1;
};

class MainWindow : public BWindow, public EditorHost, public LspSurface {
public:
    explicit MainWindow(std::vector<std::string> paths)
        : BWindow(DefaultFrame(), "HaikuIDE", B_TITLED_WINDOW, B_WILL_ACCEPT_FIRST_CLICK),
          initial_(std::move(paths)) {
        SetSizeLimits(420, 10000, 280, 10000);
        BuildMenus();
        tabs_view_ = new TabStrip();
        gutter_ = new GutterView(BRect(0, 0, 40, 200));
        editor_ = new EditorView(BRect(0, 0, 640, 480));
        editor_->SetHost(this);
        editor_->SetGutter(gutter_);
        gutter_->SetEditor(editor_);
        scroll_ = new BScrollView("editor_scroll", editor_, B_FOLLOW_NONE, 0, true, true,
                                  B_NO_BORDER);
        AddChild(menu_);
        AddChild(tabs_view_);
        AddChild(gutter_);
        AddChild(scroll_);
        SetKeyMenuBar(menu_);
        BMessenger target(this);
        open_panel_ = new BFilePanel(B_OPEN_PANEL, &target, nullptr, B_FILE_NODE, false);
        save_panel_ = new BFilePanel(B_SAVE_PANEL, &target, nullptr, B_FILE_NODE, false);
        lsp_ = new LspBridge(this, this);
        host_ = new HostLink(this, lsp_, this);
        AddHandler(host_);
    }

    ~MainWindow() override {
        dying_ = true;
        if (editor_ != nullptr) {
            editor_->SetHost(nullptr);
        }
        if (host_ != nullptr) {
            host_->Shutdown();
            RemoveHandler(host_);
            delete host_;
            host_ = nullptr;
        }
        if (lsp_ != nullptr) {
            lsp_->Shutdown();
            RemoveHandler(lsp_);
            delete lsp_;
            lsp_ = nullptr;
        }
        delete open_panel_;
        delete save_panel_;
        open_panel_ = nullptr;
        save_panel_ = nullptr;
    }

    void Show() override {
        BWindow::Show();
        // Show() runs the looper and drops the constructor lock. View queries
        // (Frame, Bounds, ResizeTo) require the window lock, so the first
        // layout and file open run here, on the window thread.
        PostMessage(kMsgReady);
    }

    void FinishShow() {
        LayoutChildren();
        if (host_ != nullptr && lsp_ != nullptr) {
            const char* env = getenv("HAIKUIDE_HOST");
            const std::string script = env != nullptr && env[0] != '\0'
                                           ? env
                                           : "/boot/home/HaikuIDE/host/dist/main.js";
            std::string err;
            if (!host_->Start(script, &err)) {
                lsp_->SetStatusText(err);
            }
        }
        if (initial_.empty()) {
            NewUntitled();
        } else {
            for (const std::string& path : initial_) {
                OpenPath(path);
            }
            if (tabs_.empty()) {
                NewUntitled();
            }
        }
        initial_.clear();
        if (editor_ != nullptr) {
            editor_->MakeFocus(true);
        }
    }

    void FrameResized(float newWidth, float newHeight) override {
        BWindow::FrameResized(newWidth, newHeight);
        LayoutChildren();
    }

    void WindowActivated(bool active) override {
        BWindow::WindowActivated(active);
        if (active && editor_ != nullptr) {
            editor_->MakeFocus(true);
            editor_->Invalidate();
        }
    }

    void MenusBeginning() override {
        EditorTab* tab = Active();
        const bool have = tab != nullptr;
        if (undo_item_ != nullptr) {
            undo_item_->SetEnabled(have && tab->doc.can_undo());
        }
        if (redo_item_ != nullptr) {
            redo_item_->SetEnabled(have && tab->doc.can_redo());
        }
        const bool selection = have && tab->caret != tab->anchor;
        if (cut_item_ != nullptr) {
            cut_item_->SetEnabled(selection);
        }
        if (copy_item_ != nullptr) {
            copy_item_->SetEnabled(selection);
        }
    }

    bool QuitRequested() override {
        if (!PrepareQuit()) {
            return false;
        }
        be_app->PostMessage(B_QUIT_REQUESTED);
        return true;
    }

    void MessageReceived(BMessage* message) override {
        if (dying_ || message == nullptr) {
            return;
        }
        switch (message->what) {
        case B_REFS_RECEIVED: {
            entry_ref ref;
            for (int32 i = 0; message->FindRef("refs", i, &ref) == B_OK; ++i) {
                BPath path(&ref);
                if (path.InitCheck() == B_OK && path.Path() != nullptr) {
                    OpenPath(path.Path());
                }
            }
            break;
        }
        case B_SAVE_REQUESTED:
            SaveRequested(message);
            break;
        case B_CANCEL:
            pending_ = kPendingNone;
            break;
        case kMsgReady:
            FinishShow();
            break;
        case kMsgSelectTab: {
            int32 index = 0;
            if (message->FindInt32("index", &index) == B_OK) {
                SelectTab(index);
            }
            break;
        }
        case kMsgNew:
        case kMsgOpen:
        case kMsgSave:
        case kMsgSaveAs:
        case kMsgClose:
        case kMsgQuit:
        case kMsgUndo:
        case kMsgRedo:
        case kMsgCut:
        case kMsgCopy:
        case kMsgPaste:
        case kMsgSelectAll:
        case kMsgLineNumbers:
        case kMsgDefinition:
            Command(message->what);
            break;
        default:
            BWindow::MessageReceived(message);
            break;
        }
    }

    void OnEditorChanged() override {
        if (dying_) {
            return;
        }
        UpdateTitle();
        if (tabs_view_ != nullptr) {
            tabs_view_->SetItems(Labels(), active_);
        }
        if (lsp_ != nullptr) {
            lsp_->ShowTab(Active());
            lsp_->FlushEdit(Active());
        }
        if (host_ != nullptr) {
            host_->FlushEdit(Active());
        }
        if (in_layout_ || editor_ == nullptr) {
            return;
        }
        const float want = editor_->GutterWidth();
        if (std::fabs(want - laid_gutter_w_) > 0.5f) {
            LayoutChildren();
        } else {
            UpdateScrollRange();
        }
    }

    void OnViewportResized() override {
        if (dying_ || in_layout_) {
            return;
        }
        UpdateScrollRange();
    }

    void OnEdit(size_t pos, size_t removed, const std::string& inserted) override {
        if (dying_ || lsp_ == nullptr) {
            return;
        }
        lsp_->RememberEdit(Active(), pos, removed, inserted);
        if (host_ != nullptr) {
            host_->NoteEdit();
        }
    }

    void OnFullSync() override {
        if (dying_ || lsp_ == nullptr) {
            return;
        }
        lsp_->FullSync(Active());
        if (host_ != nullptr) {
            host_->FullSync(Active());
        }
    }

    void OnComplete(int kind, char trigger) override {
        if (dying_ || lsp_ == nullptr) {
            return;
        }
        lsp_->Complete(Active(), kind, trigger);
        if (host_ != nullptr) {
            host_->Complete(Active(), lsp_->CompleteEpoch(), kind, trigger);
        }
    }

    void OnGoToDefinition() override {
        if (dying_ || lsp_ == nullptr) {
            return;
        }
        lsp_->Define(Active());
    }

    void OnHoverAt(size_t byte) override {
        if (dying_ || lsp_ == nullptr) {
            return;
        }
        lsp_->ArmHover(byte);
    }

    bool OnCompletionKey(int32 raw) override {
        if (dying_ || lsp_ == nullptr) {
            return false;
        }
        return lsp_->Key(raw);
    }

    EditorTab* ActiveTab() override { return Active(); }

    EditorTab* FindTab(const std::string& path) override {
        for (const auto& tab : tabs_) {
            if (tab->path == path) {
                return tab.get();
            }
        }
        return nullptr;
    }

    EditorView* Editor() override { return editor_; }

    void GoTo(const std::string& path, int line, int character) override {
        if (path.empty()) {
            return;
        }
        EditorTab* tab = FindTab(path);
        if (tab == nullptr) {
            OpenPath(path);
            tab = FindTab(path);
        } else {
            for (int32 i = 0; i < static_cast<int32>(tabs_.size()); ++i) {
                if (tabs_[static_cast<size_t>(i)].get() == tab) {
                    if (i != active_) {
                        SelectTab(i);
                    }
                    break;
                }
            }
        }
        if (tab == nullptr || editor_ == nullptr) {
            return;
        }
        editor_->PlaceCaret(PosToByte(tab->flat, line, character));
    }

    void ApplyEdit(size_t pos, size_t len, const std::string& text) override {
        if (editor_ != nullptr) {
            editor_->ApplyEdit(pos, len, text);
        }
    }

    void RefreshEditor() override {
        if (editor_ != nullptr) {
            editor_->Invalidate();
        }
        if (gutter_ != nullptr) {
            gutter_->Invalidate();
        }
    }

    void Command(uint32 what) override {
        if (dying_) {
            return;
        }
        switch (what) {
        case kMsgNew:
            NewUntitled();
            break;
        case kMsgOpen:
            if (open_panel_ != nullptr) {
                open_panel_->Show();
            }
            break;
        case kMsgSave:
            SaveActive();
            break;
        case kMsgSaveAs:
            PromptSave(kPendingSaveAs);
            break;
        case kMsgClose:
            if (active_ >= 0) {
                CloseIndex(active_, true);
            }
            break;
        case kMsgQuit:
            PostMessage(B_QUIT_REQUESTED);
            break;
        case kMsgUndo:
            if (editor_ != nullptr) {
                editor_->Undo();
            }
            break;
        case kMsgRedo:
            if (editor_ != nullptr) {
                editor_->Redo();
            }
            break;
        case kMsgCut:
            if (editor_ != nullptr) {
                editor_->Cut();
            }
            break;
        case kMsgCopy:
            if (editor_ != nullptr) {
                editor_->Copy();
            }
            break;
        case kMsgPaste:
            if (editor_ != nullptr) {
                editor_->Paste();
            }
            break;
        case kMsgSelectAll:
            if (editor_ != nullptr) {
                editor_->SelectAll();
            }
            break;
        case kMsgLineNumbers:
            if (editor_ != nullptr) {
                editor_->SetLineNumbers(!editor_->LineNumbers());
                if (line_item_ != nullptr) {
                    line_item_->SetMarked(editor_->LineNumbers());
                }
            }
            break;
        case kMsgDefinition:
            if (lsp_ != nullptr) {
                lsp_->Define(Active());
            }
            break;
        default:
            break;
        }
    }

private:
    void BuildMenus() {
        menu_ = new BMenuBar(BRect(0, 0, 100, 20), "menu");
        BMenu* file = new BMenu("File");
        file->AddItem(new BMenuItem("New", new BMessage(kMsgNew), 'N', B_CONTROL_KEY));
        file->AddItem(new BMenuItem("Open" B_UTF8_ELLIPSIS, new BMessage(kMsgOpen), 'O',
                                    B_CONTROL_KEY));
        file->AddItem(new BMenuItem("Save", new BMessage(kMsgSave), 'S', B_CONTROL_KEY));
        file->AddItem(new BMenuItem("Save As" B_UTF8_ELLIPSIS, new BMessage(kMsgSaveAs), 'S',
                                    B_CONTROL_KEY | B_SHIFT_KEY));
        file->AddSeparatorItem();
        file->AddItem(new BMenuItem("Close", new BMessage(kMsgClose), 'W', B_CONTROL_KEY));
        file->AddSeparatorItem();
        file->AddItem(new BMenuItem("Quit", new BMessage(kMsgQuit), 'Q', B_CONTROL_KEY));
        menu_->AddItem(file);

        BMenu* edit = new BMenu("Edit");
        undo_item_ = new BMenuItem("Undo", new BMessage(kMsgUndo), 'Z', B_CONTROL_KEY);
        redo_item_ = new BMenuItem("Redo", new BMessage(kMsgRedo), 'Z', B_CONTROL_KEY | B_SHIFT_KEY);
        cut_item_ = new BMenuItem("Cut", new BMessage(kMsgCut), 'X', B_CONTROL_KEY);
        copy_item_ = new BMenuItem("Copy", new BMessage(kMsgCopy), 'C', B_CONTROL_KEY);
        edit->AddItem(undo_item_);
        edit->AddItem(redo_item_);
        edit->AddSeparatorItem();
        edit->AddItem(cut_item_);
        edit->AddItem(copy_item_);
        edit->AddItem(new BMenuItem("Paste", new BMessage(kMsgPaste), 'V', B_CONTROL_KEY));
        edit->AddSeparatorItem();
        edit->AddItem(new BMenuItem("Select All", new BMessage(kMsgSelectAll), 'A', B_CONTROL_KEY));
        menu_->AddItem(edit);

        BMenu* view = new BMenu("View");
        line_item_ = new BMenuItem("Line Numbers", new BMessage(kMsgLineNumbers), 'L', B_CONTROL_KEY);
        line_item_->SetMarked(true);
        view->AddItem(line_item_);
        menu_->AddItem(view);

        BMenu* go = new BMenu("Go");
        go->AddItem(new BMenuItem("Go to Definition", new BMessage(kMsgDefinition)));
        menu_->AddItem(go);
    }

    EditorTab* Active() const {
        if (active_ < 0 || active_ >= static_cast<int32>(tabs_.size())) {
            return nullptr;
        }
        return tabs_[static_cast<size_t>(active_)].get();
    }

    std::vector<std::string> Labels() const {
        std::vector<std::string> labels;
        labels.reserve(tabs_.size());
        for (const auto& tab : tabs_) {
            std::string label = tab->name;
            if (tab->dirty) {
                label += " *";
            }
            labels.push_back(std::move(label));
        }
        return labels;
    }

    void UpdateTitle() {
        std::string title = "HaikuIDE";
        if (EditorTab* tab = Active()) {
            title += " - ";
            title += tab->name;
            if (tab->dirty) {
                title += " *";
            }
        }
        SetTitle(title.c_str());
    }

    void SelectTab(int32 index) {
        if (index < 0 || index >= static_cast<int32>(tabs_.size()) || editor_ == nullptr) {
            return;
        }
        active_ = index;
        editor_->SetTab(tabs_[static_cast<size_t>(index)].get());
        editor_->MakeFocus(true);
    }

    void NewUntitled() {
        auto tab = std::make_unique<EditorTab>();
        tab->doc = Document::empty();
        ++untitled_n_;
        if (untitled_n_ == 1) {
            tab->name = "Untitled";
        } else {
            tab->name = "Untitled " + std::to_string(untitled_n_);
        }
        tabs_.push_back(std::move(tab));
        SelectTab(static_cast<int32>(tabs_.size()) - 1);
    }

    void OpenPath(const std::string& requested) {
        BEntry entry(requested.c_str(), true);
        if (entry.InitCheck() != B_OK || !entry.Exists()) {
            Info((std::string("Cannot open ") + requested).c_str());
            return;
        }
        if (entry.IsDirectory()) {
            Info("That path is a directory.");
            return;
        }
        BPath full;
        if (entry.GetPath(&full) != B_OK || full.Path() == nullptr) {
            Info("Cannot resolve that path.");
            return;
        }
        std::string data;
        std::string err;
        if (!ReadFile(full.Path(), &data, &err)) {
            Info(err.c_str());
            return;
        }
        auto tab = std::make_unique<EditorTab>();
        tab->doc = Document::from_text(data);
        tab->flat = tab->doc.text();
        tab->saved = tab->flat;
        tab->path = full.Path();
        tab->name = Leaf(tab->path);
        tabs_.push_back(std::move(tab));
        SelectTab(static_cast<int32>(tabs_.size()) - 1);
        if (lsp_ != nullptr) {
            lsp_->Open(Active());
        }
        if (host_ != nullptr) {
            host_->Open(Active());
        }
    }

    bool WriteTab(int32 index) {
        if (index < 0 || index >= static_cast<int32>(tabs_.size())) {
            return false;
        }
        EditorTab* tab = tabs_[static_cast<size_t>(index)].get();
        if (tab->path.empty()) {
            return false;
        }
        std::string err;
        if (!WriteFile(tab->path.c_str(), *tab, &err)) {
            Info(err.c_str());
            return false;
        }
        tab->saved = tab->flat;
        tab->dirty = false;
        OnEditorChanged();
        if (lsp_ != nullptr) {
            lsp_->Save(tab);
        }
        if (host_ != nullptr) {
            host_->Save(tab);
        }
        return true;
    }

    void SaveActive() {
        EditorTab* tab = Active();
        if (tab == nullptr) {
            return;
        }
        if (tab->path.empty()) {
            PromptSave(kPendingSaveAs);
            return;
        }
        WriteTab(active_);
    }

    void PromptSave(Pending why) {
        if (active_ < 0 || save_panel_ == nullptr) {
            return;
        }
        pending_ = why;
        pending_index_ = active_;
        save_panel_->SetSaveText(tabs_[static_cast<size_t>(active_)]->name.c_str());
        save_panel_->Show();
    }

    void SaveRequested(BMessage* message) {
        entry_ref dir;
        const char* name = nullptr;
        if (message->FindRef("directory", &dir) != B_OK ||
            message->FindString("name", &name) != B_OK || name == nullptr) {
            pending_ = kPendingNone;
            return;
        }
        BDirectory directory(&dir);
        BPath path(&directory, name);
        if (path.InitCheck() != B_OK && path.SetTo(&dir) == B_OK) {
            path.Append(name);
        }
        if (path.InitCheck() != B_OK || path.Path() == nullptr) {
            pending_ = kPendingNone;
            Info("Cannot use that location.");
            return;
        }
        const int32 index = pending_index_ >= 0 ? pending_index_ : active_;
        const Pending why = pending_;
        pending_ = kPendingNone;
        if (index < 0 || index >= static_cast<int32>(tabs_.size())) {
            return;
        }
        EditorTab* tab = tabs_[static_cast<size_t>(index)].get();
        tab->path = path.Path();
        tab->name = Leaf(tab->path);
        if (!WriteTab(index)) {
            return;
        }
        if (why == kPendingClose) {
            RemoveIndex(index);
        } else if (why == kPendingQuit) {
            Quit();
        }
    }

    void CloseIndex(int32 index, bool ask) {
        if (index < 0 || index >= static_cast<int32>(tabs_.size())) {
            return;
        }
        if (ask && tabs_[static_cast<size_t>(index)]->dirty) {
            SelectTab(index);
            const int choice = ConfirmSave(tabs_[static_cast<size_t>(index)]->name);
            if (choice == 0) {
                return;
            }
            if (choice == 2) {
                if (tabs_[static_cast<size_t>(index)]->path.empty()) {
                    PromptSave(kPendingClose);
                    return;
                }
                if (!WriteTab(index)) {
                    return;
                }
            }
        }
        RemoveIndex(index);
    }

    void RemoveIndex(int32 index) {
        if (index < 0 || index >= static_cast<int32>(tabs_.size())) {
            return;
        }
        if (lsp_ != nullptr) {
            lsp_->Close(tabs_[static_cast<size_t>(index)]->path);
        }
        if (host_ != nullptr) {
            host_->Close(tabs_[static_cast<size_t>(index)]->path);
        }
        if (editor_ != nullptr && editor_->Tab() == tabs_[static_cast<size_t>(index)].get()) {
            editor_->SetTab(nullptr);
        }
        tabs_.erase(tabs_.begin() + static_cast<std::ptrdiff_t>(index));
        if (tabs_.empty()) {
            active_ = -1;
            NewUntitled();
            return;
        }
        if (active_ > index) {
            --active_;
        }
        if (active_ < 0 || active_ >= static_cast<int32>(tabs_.size())) {
            active_ = std::min(index, static_cast<int32>(tabs_.size()) - 1);
        }
        if (editor_ != nullptr) {
            editor_->SetTab(tabs_[static_cast<size_t>(active_)].get());
        }
    }

    bool PrepareQuit() {
        if (pending_ != kPendingNone) {
            return false;
        }
        for (int32 i = 0; i < static_cast<int32>(tabs_.size()); ++i) {
            if (!tabs_[static_cast<size_t>(i)]->dirty) {
                continue;
            }
            SelectTab(i);
            const int choice = ConfirmSave(tabs_[static_cast<size_t>(i)]->name);
            if (choice == 0) {
                return false;
            }
            if (choice == 1) {
                tabs_[static_cast<size_t>(i)]->dirty = false;
                continue;
            }
            if (tabs_[static_cast<size_t>(i)]->path.empty()) {
                PromptSave(kPendingQuit);
                return false;
            }
            if (!WriteTab(i)) {
                return false;
            }
        }
        return true;
    }

    void FitEditorInScroll() {
        BScrollBar* vertical = scroll_->ScrollBar(B_VERTICAL);
        BScrollBar* horizontal = scroll_->ScrollBar(B_HORIZONTAL);
        float bar_w = B_V_SCROLL_BAR_WIDTH;
        float bar_h = B_H_SCROLL_BAR_HEIGHT;
        if (vertical != nullptr) {
            float w = 0.f;
            float h = 0.f;
            vertical->GetPreferredSize(&w, &h);
            if (w > 1.f) {
                bar_w = w;
            }
        }
        if (horizontal != nullptr) {
            float w = 0.f;
            float h = 0.f;
            horizontal->GetPreferredSize(&w, &h);
            if (h > 1.f) {
                bar_h = h;
            }
        }
        BRect inner = scroll_->Bounds();
        if (vertical != nullptr) {
            inner.right -= bar_w + 1.f;
        }
        if (horizontal != nullptr) {
            inner.bottom -= bar_h + 1.f;
        }
        if (inner.right < inner.left) {
            inner.right = inner.left;
        }
        if (inner.bottom < inner.top) {
            inner.bottom = inner.top;
        }
        editor_->MoveTo(inner.LeftTop());
        editor_->ResizeTo(inner.Width(), inner.Height());
        if (horizontal != nullptr) {
            BRect rect = inner;
            rect.top = rect.bottom + 1.f;
            rect.bottom = rect.top + bar_h;
            if (vertical != nullptr) {
                rect.right += 1.f;
            }
            horizontal->MoveTo(rect.LeftTop());
            horizontal->ResizeTo(rect.Width(), rect.Height());
        }
        if (vertical != nullptr) {
            BRect rect = inner;
            rect.left = rect.right + 1.f;
            rect.right = rect.left + bar_w;
            if (horizontal != nullptr) {
                rect.bottom += 1.f;
            }
            vertical->MoveTo(rect.LeftTop());
            vertical->ResizeTo(rect.Width(), rect.Height());
        }
    }

    void LayoutChildren() {
        if (in_layout_ || menu_ == nullptr || tabs_view_ == nullptr || gutter_ == nullptr ||
            scroll_ == nullptr || editor_ == nullptr) {
            return;
        }
        in_layout_ = true;
        const float width = Bounds().Width();
        const float height = Bounds().Height();
        const float menu_h = menu_->Frame().Height() + 1.f;
        const float tab_h = 28.f;
        menu_->MoveTo(0, 0);
        menu_->ResizeTo(width, menu_->Frame().Height());
        tabs_view_->MoveTo(0, menu_h);
        tabs_view_->ResizeTo(width, tab_h);
        const float top = menu_h + tab_h;
        const float chrome = lsp_ != nullptr ? lsp_->ChromeHeight() : 0.f;
        const float gw = editor_->GutterWidth();
        laid_gutter_w_ = gw;
        scroll_->MoveTo(gw, top);
        scroll_->ResizeTo(std::max(0.f, width - gw), std::max(0.f, height - top - chrome));
        // BScrollView sizes itself around the target at construction and does
        // not resize a B_FOLLOW_NONE target later. Fit the editor and the bars
        // to the inner frame (B_NO_BORDER insets the bars by one extra pixel).
        FitEditorInScroll();
        BPoint origin = editor_->Frame().LeftTop();
        if (editor_->Parent() != nullptr) {
            origin = editor_->Parent()->ConvertToParent(origin);
        }
        const float editor_h = std::max(0.f, editor_->Frame().Height());
        if (gw < 1.f) {
            if (!gutter_->IsHidden()) {
                gutter_->Hide();
            }
        } else {
            if (gutter_->IsHidden()) {
                gutter_->Show();
            }
            gutter_->MoveTo(0, origin.y);
            gutter_->ResizeTo(gw, editor_h);
        }
        if (lsp_ != nullptr) {
            lsp_->Layout(width, height);
        }
        in_layout_ = false;
        UpdateScrollRange();
    }

    void UpdateScrollRange() {
        if (editor_ == nullptr || scroll_ == nullptr) {
            return;
        }
        BScrollBar* vertical = scroll_->ScrollBar(B_VERTICAL);
        BScrollBar* horizontal = scroll_->ScrollBar(B_HORIZONTAL);
        if (vertical == nullptr || horizontal == nullptr) {
            return;
        }
        float vis_h = editor_->Bounds().Height();
        float vis_w = editor_->Bounds().Width();
        if (vis_h < 1.f) {
            vis_h = 1.f;
        }
        if (vis_w < 1.f) {
            vis_w = 1.f;
        }
        const float doc_h = std::max(editor_->ContentHeight(), vis_h);
        const float doc_w = std::max(editor_->ContentWidth(), vis_w);
        const float vmax = std::max(0.f, doc_h - vis_h);
        const float hmax = std::max(0.f, doc_w - vis_w);
        const float line = std::max(1.f, editor_->LineHeight());
        const float cell = std::max(1.f, editor_->CellWidth());
        vertical->SetRange(0.f, vmax);
        vertical->SetProportion(std::min(1.f, std::max(0.02f, vis_h / doc_h)));
        vertical->SetSteps(line, std::max(line, vis_h - line));
        horizontal->SetRange(0.f, hmax);
        horizontal->SetProportion(std::min(1.f, std::max(0.02f, vis_w / doc_w)));
        horizontal->SetSteps(cell, std::max(cell * 4.f, vis_w / 2.f));
        float y = editor_->Bounds().top;
        float x = editor_->Bounds().left;
        if (y > vmax) {
            y = vmax;
        }
        if (x > hmax) {
            x = hmax;
        }
        if (y < 0.f) {
            y = 0.f;
        }
        if (x < 0.f) {
            x = 0.f;
        }
        vertical->SetValue(y);
        horizontal->SetValue(x);
    }

    std::vector<std::string> initial_;
    std::vector<std::unique_ptr<EditorTab>> tabs_;
    int32 active_ = -1;
    int untitled_n_ = 0;
    BMenuBar* menu_ = nullptr;
    BMenuItem* undo_item_ = nullptr;
    BMenuItem* redo_item_ = nullptr;
    BMenuItem* cut_item_ = nullptr;
    BMenuItem* copy_item_ = nullptr;
    BMenuItem* line_item_ = nullptr;
    TabStrip* tabs_view_ = nullptr;
    GutterView* gutter_ = nullptr;
    EditorView* editor_ = nullptr;
    BScrollView* scroll_ = nullptr;
    BFilePanel* open_panel_ = nullptr;
    BFilePanel* save_panel_ = nullptr;
    Pending pending_ = kPendingNone;
    int32 pending_index_ = -1;
    float laid_gutter_w_ = -1.f;
    bool in_layout_ = false;
    bool dying_ = false;
    LspBridge* lsp_ = nullptr;
    HostLink* host_ = nullptr;
};

class App : public BApplication {
public:
    App(int argc, char** argv) : BApplication(kSignature) {
        for (int i = 1; i < argc; ++i) {
            if (argv[i] != nullptr && argv[i][0] != '\0' && argv[i][0] != '-') {
                paths_.emplace_back(argv[i]);
            }
        }
    }

    void ReadyToRun() override { window_ = new MainWindow(paths_); window_->Show(); }

private:
    std::vector<std::string> paths_;
    MainWindow* window_ = nullptr;
};

}  // namespace haikuide

int main(int argc, char** argv) {
    haikuide::App app(argc, argv);
    app.Run();
    return 0;
}
