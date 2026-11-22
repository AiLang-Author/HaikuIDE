/* HaikuIDE text view.
 * One viewport-sized BView inside a BScrollView. Draw paints the clip only.
 *
 * Copyright (c) 2026 Sean Collins, 2 Paws Machine and Engineering. MIT.
 */
#ifndef HAIKUIDE_EDITOR_VIEW_HPP
#define HAIKUIDE_EDITOR_VIEW_HPP

#ifndef __HAIKU__
#error Build ui/ on Haiku: g++ -std=c++17 -O2 -Ieditor -o HaikuIDE ui/app.cpp ui/editor_view.cpp editor/document.cpp -ltracker -lbe
#endif

#include "document.hpp"

#include <View.h>

#include <string>
#include <vector>

namespace haikuide {

/* Menu and key commands. Haiku matches menu shortcuts against the
 * control-mapped byte, so Ctrl-S arrives as 0x13 and the view handles
 * raw_char itself. The menu item still shows the shortcut. */
enum EditorCommand : uint32 {
    kMsgNew = 0x48000001,
    kMsgOpen = 0x48000002,
    kMsgSave = 0x48000003,
    kMsgSaveAs = 0x48000004,
    kMsgClose = 0x48000005,
    kMsgQuit = 0x48000006,
    kMsgUndo = 0x48000007,
    kMsgRedo = 0x48000008,
    kMsgCut = 0x48000009,
    kMsgCopy = 0x4800000a,
    kMsgPaste = 0x4800000b,
    kMsgSelectAll = 0x4800000c,
    kMsgLineNumbers = 0x4800000d,
    kMsgSelectTab = 0x4800000e,
    kMsgDefinition = 0x4800000f
};

struct LineDiag {
    int line = 0;
    int character = 0;
    int end_line = 0;
    int end_character = 0;
    int severity = 1;
    std::string message;
    size_t start_byte = 0;
    size_t end_byte = 0;
};

struct CaretMark {
    size_t caret = 0;
    size_t anchor = 0;
};

struct EditorTab {
    Document doc;
    std::string path;
    std::string saved;
    std::string flat;
    std::string name;
    size_t caret = 0;
    size_t anchor = 0;
    float preferred_x = 0;
    float scroll_x = 0;
    float scroll_y = 0;
    float content_w = 0;
    bool dirty = false;
    std::vector<CaretMark> undo_marks;
    std::vector<CaretMark> redo_marks;
    std::vector<LineDiag> diags;
};

class EditorHost {
public:
    virtual ~EditorHost() = default;
    virtual void OnEditorChanged() = 0;
    virtual void OnViewportResized() = 0;
    virtual void Command(uint32 what) = 0;
    virtual void OnEdit(size_t /*pos*/, size_t /*removed*/, const std::string& /*inserted*/) {}
    virtual void OnFullSync() {}
    virtual void OnComplete(int /*kind*/, char /*trigger*/) {}
    virtual void OnGoToDefinition() {}
    virtual void OnHoverAt(size_t /*byte*/) {}
    virtual bool OnCompletionKey(int32 /*raw*/) { return false; }
};

class GutterView;

class EditorView : public BView {
public:
    explicit EditorView(BRect frame);

    void SetHost(EditorHost* host);
    void SetGutter(GutterView* gutter);
    void SetTab(EditorTab* tab);
    EditorTab* Tab() const { return tab_; }

    float LineHeight() const { return line_h_; }
    float Ascent() const { return ascent_; }
    float CellWidth() const { return cell_; }
    float ContentHeight() const;
    float ContentWidth() const;
    float GutterWidth() const;
    bool LineNumbers() const { return line_numbers_; }
    void SetLineNumbers(bool on);

    rgb_color TextColor() const { return fg_; }
    rgb_color GutterColor() const { return gutter_bg_; }
    rgb_color NumberColor() const { return num_fg_; }

    void Undo();
    void Redo();
    void Cut();
    void Copy();
    void Paste();
    void SelectAll();
    void SelectLine(size_t line, bool extend);
    void InsertText(std::string_view text);
    void ApplyEdit(size_t pos, size_t len, const std::string& text);
    void PlaceCaret(size_t byte);
    BPoint CaretScreenPoint() const;

    void Draw(BRect update) override;
    void KeyDown(const char* bytes, int32 numBytes) override;
    void MouseDown(BPoint where) override;
    void MouseUp(BPoint where) override;
    void MouseMoved(BPoint where, uint32 code, const BMessage* drag) override;
    void FrameResized(float newWidth, float newHeight) override;
    void ScrollTo(BPoint where) override;
    void AttachedToWindow() override;

private:
    void ApplyFont();
    void LoadColors();
    void SyncGutter();
    void RecomputeWidth();
    void FinishRefresh(bool remember_x);
    bool ReplaceRange(size_t pos, size_t len, std::string_view text);
    void RevealCaret();
    void MoveCaret(size_t next, bool shift, bool remember_x);
    void MoveVertical(int delta, bool shift);
    void DeleteWord(bool forward);
    size_t OffsetAt(BPoint where) const;
    size_t OffsetOnLine(size_t line, float x) const;
    size_t LineOf(size_t byte) const;
    float CaretX() const;
    float WidthOf(size_t begin, size_t end) const;
    void DrawLineText(size_t begin, size_t end, BRect update, float baseline);
    void ClampCaret();

    EditorHost* host_ = nullptr;
    GutterView* gutter_ = nullptr;
    EditorTab* tab_ = nullptr;
    float line_h_ = 16.f;
    float ascent_ = 12.f;
    float cell_ = 8.f;
    rgb_color bg_{255, 255, 255, 255};
    rgb_color fg_{0, 0, 0, 255};
    rgb_color sel_{173, 214, 255, 255};
    rgb_color gutter_bg_{232, 232, 232, 255};
    rgb_color num_fg_{80, 80, 80, 255};
    bool line_numbers_ = true;
    bool dragging_ = false;
    bool scroll_lock_ = false;
    bool font_ready_ = false;
};

class GutterView : public BView {
public:
    explicit GutterView(BRect frame);
    void SetEditor(EditorView* editor);

    void Draw(BRect update) override;
    void MouseDown(BPoint where) override;
    void AttachedToWindow() override;

private:
    EditorView* editor_ = nullptr;
};

}  // namespace haikuide

#endif
